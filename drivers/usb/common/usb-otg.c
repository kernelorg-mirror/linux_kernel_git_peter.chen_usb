/**
 * drivers/usb/common/usb-otg.c - USB OTG core
 *
 * Copyright (C) 2016 Texas Instruments Incorporated - http://www.ti.com
 * Author: Roger Quadros <rogerq@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/usb/of.h>
#include <linux/usb/otg.h>
#include <linux/usb/gadget.h>
#include <linux/workqueue.h>

#include "usb-otg.h"

struct otg_gcd {
	struct usb_gadget *gadget;
	struct otg_gadget_ops *ops;
};

/* OTG device list */
LIST_HEAD(otg_list);
static DEFINE_MUTEX(otg_list_mutex);

/* Hosts and Gadgets waiting for OTG controller */
struct otg_wait_data {
	struct device *dev;		/* OTG controller device */

	struct otg_hcd primary_hcd;
	struct otg_hcd shared_hcd;
	struct otg_gcd gcd;
	struct list_head list;
};

LIST_HEAD(wait_list);
static DEFINE_MUTEX(wait_list_mutex);

static int usb_otg_hcd_is_primary_hcd(struct usb_hcd *hcd)
{
	if (!hcd->primary_hcd)
		return 1;
	return hcd == hcd->primary_hcd;
}

/**
 * Check if the OTG device is in our wait list and return
 * otg_wait_data, else NULL.
 *
 * wait_list_mutex must be held.
 */
static struct otg_wait_data *usb_otg_get_wait(struct device *otg_dev)
{
	struct otg_wait_data *wait;

	if (!otg_dev)
		return NULL;

	/* is there an entry for this otg_dev ?*/
	list_for_each_entry(wait, &wait_list, list) {
		if (wait->dev == otg_dev)
			return wait;
	}

	return NULL;
}

/**
 * Add the hcd to our wait list
 */
static int usb_otg_hcd_wait_add(struct device *otg_dev, struct usb_hcd *hcd,
				unsigned int irqnum, unsigned long irqflags,
				struct otg_hcd_ops *ops)
{
	struct otg_wait_data *wait;
	int ret = -EINVAL;

	mutex_lock(&wait_list_mutex);

	wait = usb_otg_get_wait(otg_dev);
	if (!wait) {
		/* Not yet in wait list? allocate and add */
		wait = kzalloc(sizeof(*wait), GFP_KERNEL);
		if (!wait) {
			ret = -ENOMEM;
			goto fail;
		}

		wait->dev = otg_dev;
		list_add_tail(&wait->list, &wait_list);
	}

	if (usb_otg_hcd_is_primary_hcd(hcd)) {
		if (wait->primary_hcd.hcd)	/* already assigned? */
			goto fail;

		wait->primary_hcd.hcd = hcd;
		wait->primary_hcd.irqnum = irqnum;
		wait->primary_hcd.irqflags = irqflags;
		wait->primary_hcd.ops = ops;
		wait->primary_hcd.otg_dev = otg_dev;
	} else {
		if (wait->shared_hcd.hcd)	/* already assigned? */
			goto fail;

		wait->shared_hcd.hcd = hcd;
		wait->shared_hcd.irqnum = irqnum;
		wait->shared_hcd.irqflags = irqflags;
		wait->shared_hcd.ops = ops;
		wait->shared_hcd.otg_dev = otg_dev;
	}

	mutex_unlock(&wait_list_mutex);
	return 0;

fail:
	mutex_unlock(&wait_list_mutex);
	return ret;
}

/**
 * Check and free wait list entry if empty
 *
 * wait_list_mutex must be held
 */
static void usb_otg_check_free_wait(struct otg_wait_data *wait)
{
	if (wait->primary_hcd.hcd || wait->shared_hcd.hcd || wait->gcd.gadget)
		return;

	list_del(&wait->list);
	kfree(wait);
}

/**
 * Remove the hcd from our wait list
 */
static int usb_otg_hcd_wait_remove(struct usb_hcd *hcd)
{
	struct otg_wait_data *wait;

	mutex_lock(&wait_list_mutex);

	/* is there an entry for this hcd ?*/
	list_for_each_entry(wait, &wait_list, list) {
		if (wait->primary_hcd.hcd == hcd) {
			wait->primary_hcd.hcd = 0;
			goto found;
		} else if (wait->shared_hcd.hcd == hcd) {
			wait->shared_hcd.hcd = 0;
			goto found;
		}
	}

	mutex_unlock(&wait_list_mutex);
	return -EINVAL;

found:
	usb_otg_check_free_wait(wait);
	mutex_unlock(&wait_list_mutex);

	return 0;
}

/**
 * Add the gadget to our wait list
 */
static int usb_otg_gadget_wait_add(struct device *otg_dev,
				   struct usb_gadget *gadget,
				   struct otg_gadget_ops *ops)
{
	struct otg_wait_data *wait;
	int ret = -EINVAL;

	mutex_lock(&wait_list_mutex);

	wait = usb_otg_get_wait(otg_dev);
	if (!wait) {
		/* Not yet in wait list? allocate and add */
		wait = kzalloc(sizeof(*wait), GFP_KERNEL);
		if (!wait) {
			ret = -ENOMEM;
			goto fail;
		}

		wait->dev = otg_dev;
		list_add_tail(&wait->list, &wait_list);
	}

	if (wait->gcd.gadget) /* already assigned? */
		goto fail;

	wait->gcd.gadget = gadget;
	wait->gcd.ops = ops;
	mutex_unlock(&wait_list_mutex);

	return 0;

fail:
	mutex_unlock(&wait_list_mutex);
	return ret;
}

/**
 * Remove the gadget from our wait list
 */
static int usb_otg_gadget_wait_remove(struct usb_gadget *gadget)
{
	struct otg_wait_data *wait;

	mutex_lock(&wait_list_mutex);

	/* is there an entry for this gadget ?*/
	list_for_each_entry(wait, &wait_list, list) {
		if (wait->gcd.gadget == gadget) {
			wait->gcd.gadget = 0;
			goto found;
		}
	}

	mutex_unlock(&wait_list_mutex);

	return -EINVAL;

found:
	usb_otg_check_free_wait(wait);
	mutex_unlock(&wait_list_mutex);

	return 0;
}

/**
 * Register pending host/gadget and remove entry from wait list
 */
static void usb_otg_flush_wait(struct device *otg_dev)
{
	struct otg_wait_data *wait;
	struct otg_hcd *whcd;
	struct otg_gcd *wgcd;

	mutex_lock(&wait_list_mutex);

	wait = usb_otg_get_wait(otg_dev);
	if (!wait)
		goto done;

	dev_dbg(otg_dev, "otg: registering pending host/gadget\n");
	wgcd = &wait->gcd;
	if (wgcd->gadget)
		usb_otg_register_gadget(wgcd->gadget, wgcd->ops);

	whcd = &wait->primary_hcd;
	if (whcd->hcd)
		usb_otg_register_hcd(whcd->hcd, whcd->irqnum, whcd->irqflags,
				     whcd->ops);

	whcd = &wait->shared_hcd;
	if (whcd->hcd)
		usb_otg_register_hcd(whcd->hcd, whcd->irqnum, whcd->irqflags,
				     whcd->ops);

	list_del(&wait->list);
	kfree(wait);

done:
	mutex_unlock(&wait_list_mutex);
}

/**
 * Check if the OTG device is in our OTG list and return
 * usb_otg data, else NULL.
 *
 * otg_list_mutex must be held.
 */
static struct usb_otg *usb_otg_get_data(struct device *otg_dev)
{
	struct usb_otg *otg;

	if (!otg_dev)
		return NULL;

	list_for_each_entry(otg, &otg_list, list) {
		if (otg->dev == otg_dev)
			return otg;
	}

	return NULL;
}

/**
 * usb_otg_start_host - start/stop the host controller
 * @otg:	usb_otg instance
 * @on:		true to start, false to stop
 *
 * Start/stop the USB host controller. This function is meant
 * for use by the OTG controller driver.
 */
int usb_otg_start_host(struct usb_otg *otg, int on)
{
	struct otg_hcd_ops *hcd_ops = otg->hcd_ops;

	dev_dbg(otg->dev, "otg: %s %d\n", __func__, on);
	if (!otg->host) {
		WARN_ONCE(1, "otg: fsm running without host\n");
		return 0;
	}

	if (on) {
		if (otg->flags & OTG_FLAG_HOST_RUNNING)
			return 0;

		otg->flags |= OTG_FLAG_HOST_RUNNING;

		/* start host */
		hcd_ops->add(otg->primary_hcd.hcd, otg->primary_hcd.irqnum,
			     otg->primary_hcd.irqflags);
		if (otg->shared_hcd.hcd) {
			hcd_ops->add(otg->shared_hcd.hcd,
				     otg->shared_hcd.irqnum,
				     otg->shared_hcd.irqflags);
		}
	} else {
		if (!(otg->flags & OTG_FLAG_HOST_RUNNING))
			return 0;

		otg->flags &= ~OTG_FLAG_HOST_RUNNING;

		/* stop host */
		if (otg->shared_hcd.hcd)
			hcd_ops->remove(otg->shared_hcd.hcd);

		hcd_ops->remove(otg->primary_hcd.hcd);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_start_host);

/**
 * usb_otg_start_gadget - start/stop the gadget controller
 * @otg:	usb_otg instance
 * @on:		true to start, false to stop
 *
 * Start/stop the USB gadget controller. This function is meant
 * for use by the OTG controller driver.
 */
int usb_otg_start_gadget(struct usb_otg *otg, int on)
{
	struct usb_gadget *gadget = otg->gadget;

	dev_dbg(otg->dev, "otg: %s %d\n", __func__, on);
	if (!gadget) {
		WARN_ONCE(1, "otg: fsm running without gadget\n");
		return 0;
	}

	if (on) {
		if (otg->flags & OTG_FLAG_GADGET_RUNNING)
			return 0;

		otg->flags |= OTG_FLAG_GADGET_RUNNING;
		otg->gadget_ops->start(otg->gadget);
	} else {
		if (!(otg->flags & OTG_FLAG_GADGET_RUNNING))
			return 0;

		otg->flags &= ~OTG_FLAG_GADGET_RUNNING;
		otg->gadget_ops->stop(otg->gadget);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_start_gadget);

/**
 * Change USB protocol when there is a protocol change.
 * fsm->lock must be held.
 */
static int drd_set_protocol(struct otg_fsm *fsm, int protocol)
{
	struct usb_otg *otg = container_of(fsm, struct usb_otg, fsm);
	int ret = 0;

	if (fsm->protocol != protocol) {
		dev_dbg(otg->dev, "otg: changing role fsm->protocol= %d; new protocol= %d\n",
			fsm->protocol, protocol);
		/* stop old protocol */
		if (fsm->protocol == PROTO_HOST)
			ret = otg_start_host(otg, 0);
		else if (fsm->protocol == PROTO_GADGET)
			ret = otg_start_gadget(otg, 0);
		if (ret)
			return ret;

		/* start new protocol */
		if (protocol == PROTO_HOST)
			ret = otg_start_host(otg, 1);
		else if (protocol == PROTO_GADGET)
			ret = otg_start_gadget(otg, 1);
		if (ret)
			return ret;

		fsm->protocol = protocol;
		return 0;
	}

	return 0;
}

/**
 * Called when entering a DRD state.
 * fsm->lock must be held.
 */
static void drd_set_state(struct otg_fsm *fsm, enum usb_otg_state new_state)
{
	struct usb_otg *otg = container_of(fsm, struct usb_otg, fsm);

	if (otg->state == new_state)
		return;

	fsm->state_changed = 1;
	dev_dbg(otg->dev, "otg: set state: %s\n",
		usb_otg_state_string(new_state));
	switch (new_state) {
	case OTG_STATE_B_IDLE:
		drd_set_protocol(fsm, PROTO_UNDEF);
		otg_drv_vbus(otg, 0);
		break;
	case OTG_STATE_B_PERIPHERAL:
		drd_set_protocol(fsm, PROTO_GADGET);
		otg_drv_vbus(otg, 0);
		break;
	case OTG_STATE_A_HOST:
		drd_set_protocol(fsm, PROTO_HOST);
		otg_drv_vbus(otg, 1);
		break;
	default:
		dev_warn(otg->dev, "%s: otg: invalid state: %s\n",
			 __func__, usb_otg_state_string(new_state));
		break;
	}

	otg->state = new_state;
}

/**
 * DRD state change judgement
 *
 * For DRD we're only interested in some of the OTG states
 * i.e. OTG_STATE_B_IDLE: both peripheral and host are stopped
 *	OTG_STATE_B_PERIPHERAL: peripheral active
 *	OTG_STATE_A_HOST: host active
 * we're only interested in the following inputs
 *	fsm->id, fsm->b_sess_vld
 */
int drd_statemachine(struct usb_otg *otg)
{
	struct otg_fsm *fsm = &otg->fsm;
	enum usb_otg_state state;
	int ret;

	mutex_lock(&fsm->lock);

	fsm->state_changed = 0;
	state = otg->state;

	switch (state) {
	case OTG_STATE_UNDEFINED:
		if (!fsm->id)
			drd_set_state(fsm, OTG_STATE_A_HOST);
		else if (fsm->id && fsm->b_sess_vld)
			drd_set_state(fsm, OTG_STATE_B_PERIPHERAL);
		else
			drd_set_state(fsm, OTG_STATE_B_IDLE);
		break;
	case OTG_STATE_B_IDLE:
		if (!fsm->id)
			drd_set_state(fsm, OTG_STATE_A_HOST);
		else if (fsm->b_sess_vld)
			drd_set_state(fsm, OTG_STATE_B_PERIPHERAL);
		break;
	case OTG_STATE_B_PERIPHERAL:
		if (!fsm->id)
			drd_set_state(fsm, OTG_STATE_A_HOST);
		else if (!fsm->b_sess_vld)
			drd_set_state(fsm, OTG_STATE_B_IDLE);
		break;
	case OTG_STATE_A_HOST:
		if (fsm->id && fsm->b_sess_vld)
			drd_set_state(fsm, OTG_STATE_B_PERIPHERAL);
		else if (fsm->id && !fsm->b_sess_vld)
			drd_set_state(fsm, OTG_STATE_B_IDLE);
		break;

	default:
		dev_err(otg->dev, "%s: otg: invalid usb-drd state: %s\n",
			__func__, usb_otg_state_string(state));
		break;
	}

	ret = fsm->state_changed;
	mutex_unlock(&fsm->lock);
	dev_dbg(otg->dev, "otg: quit statemachine, changed %d\n",
		fsm->state_changed);

	return ret;
}
EXPORT_SYMBOL_GPL(drd_statemachine);

/**
 * Dual-role device (DRD) work function
 */
static void usb_drd_work(struct work_struct *work)
{
	struct usb_otg *otg = container_of(work, struct usb_otg, work);

	pm_runtime_get_sync(otg->dev);
	drd_statemachine(otg);
	pm_runtime_put_sync(otg->dev);
}

/**
 * usb_otg_register() - Register the OTG/dual-role device to OTG core
 * @dev: OTG/dual-role controller device.
 * @config: OTG configuration.
 *
 * Registers the OTG/dual-role controller device with the USB OTG core.
 *
 * Return: struct usb_otg * if success, ERR_PTR() if error.
 */
struct usb_otg *usb_otg_register(struct device *dev,
				 struct usb_otg_config *config)
{
	struct usb_otg *otg;
	struct otg_wait_data *wait;
	int ret = 0;

	if (!dev || !config || !config->fsm_ops)
		return ERR_PTR(-EINVAL);

	/* already in list? */
	mutex_lock(&otg_list_mutex);
	if (usb_otg_get_data(dev)) {
		dev_err(dev, "otg: %s: device already in otg list\n",
			__func__);
		ret = -EINVAL;
		goto unlock;
	}

	/* allocate and add to list */
	otg = kzalloc(sizeof(*otg), GFP_KERNEL);
	if (!otg) {
		ret = -ENOMEM;
		goto unlock;
	}

	otg->dev = dev;
	otg->caps = config->otg_caps;

	if ((otg->caps->hnp_support || otg->caps->srp_support ||
	     otg->caps->adp_support) && !config->otg_work) {
		dev_err(dev,
			"otg: otg_work must be provided for OTG support\n");
		ret = -EINVAL;
		goto err_wq;
	}

	if (config->otg_work)	/* custom otg_work ? */
		INIT_WORK(&otg->work, config->otg_work);
	else
		INIT_WORK(&otg->work, usb_drd_work);

	if (of_find_property(dev->of_node, "hcd-needs-companion", NULL) ||
	    config->hcd_needs_companion)	/* needs companion ? */
		otg->flags |= OTG_FLAG_HCD_NEEDS_COMPANION;

	otg->wq = create_singlethread_workqueue("usb_otg");
	if (!otg->wq) {
		dev_err(dev, "otg: %s: can't create workqueue\n",
			__func__);
		ret = -ENOMEM;
		goto err_wq;
	}

	/* set otg ops */
	otg->fsm.ops = config->fsm_ops;

	mutex_init(&otg->fsm.lock);

	list_add_tail(&otg->list, &otg_list);
	mutex_unlock(&otg_list_mutex);

	/* were we in wait list? */
	mutex_lock(&wait_list_mutex);
	wait = usb_otg_get_wait(dev);
	mutex_unlock(&wait_list_mutex);
	if (wait) {
		/* register pending host/gadget and flush from list */
		usb_otg_flush_wait(dev);
	}

	return otg;

err_wq:
	kfree(otg);
unlock:
	mutex_unlock(&otg_list_mutex);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(usb_otg_register);

/**
 * usb_otg_unregister() - Unregister the OTG/dual-role device from USB OTG core
 * @dev: OTG controller device.
 *
 * Unregisters the OTG/dual-role controller device from USB OTG core.
 * Prevents unregistering till both the associated Host and Gadget controllers
 * have unregistered from the OTG core.
 *
 * Return: 0 on success, error value otherwise.
 */
int usb_otg_unregister(struct device *dev)
{
	struct usb_otg *otg;

	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(dev);
	if (!otg) {
		dev_err(dev, "otg: %s: device not in otg list\n",
			__func__);
		mutex_unlock(&otg_list_mutex);
		return -EINVAL;
	}

	/* prevent unregister till both host & gadget have unregistered */
	if (otg->host || otg->gadget) {
		dev_err(dev, "otg: %s: host/gadget still registered\n",
			__func__);
		return -EBUSY;
	}

	/* OTG FSM is halted when host/gadget unregistered */
	destroy_workqueue(otg->wq);

	/* remove from otg list */
	list_del(&otg->list);
	kfree(otg);
	mutex_unlock(&otg_list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_unregister);

/**
 * start/kick the OTG FSM if we can
 * fsm->lock must be held
 */
static void usb_otg_start_fsm(struct usb_otg *otg)
{
	struct otg_fsm *fsm = &otg->fsm;

	if (fsm->running)
		goto kick_fsm;

	if (!otg->host) {
		dev_info(otg->dev, "otg: can't start till host registers\n");
		return;
	}

	if (!otg->gadget) {
		dev_info(otg->dev, "otg: can't start till gadget registers\n");
		return;
	}

	fsm->running = true;
kick_fsm:
	queue_work(otg->wq, &otg->work);
}

/**
 * stop the OTG FSM. Stops Host & Gadget controllers as well.
 * fsm->lock must be held
 */
static void usb_otg_stop_fsm(struct usb_otg *otg)
{
	struct otg_fsm *fsm = &otg->fsm;

	if (!fsm->running)
		return;

	/* no more new events queued */
	fsm->running = false;

	flush_workqueue(otg->wq);
	otg->state = OTG_STATE_UNDEFINED;

	/* stop host/gadget immediately */
	if (fsm->protocol == PROTO_HOST)
		otg_start_host(otg, 0);
	else if (fsm->protocol == PROTO_GADGET)
		otg_start_gadget(otg, 0);
	fsm->protocol = PROTO_UNDEF;
}

/**
 * usb_otg_sync_inputs - Sync OTG inputs with the OTG state machine
 * @fsm:	OTG FSM instance
 *
 * Used by the OTG driver to update the inputs to the OTG
 * state machine.
 *
 * Can be called in IRQ context.
 */
void usb_otg_sync_inputs(struct usb_otg *otg)
{
	/* Don't kick FSM till it has started */
	if (!otg->fsm.running)
		return;

	/* Kick FSM */
	queue_work(otg->wq, &otg->work);
}
EXPORT_SYMBOL_GPL(usb_otg_sync_inputs);

/**
 * usb_otg_kick_fsm - Kick the OTG state machine
 * @otg_dev:	OTG controller device
 *
 * Used by USB host/device stack to sync OTG related
 * events to the OTG state machine.
 * e.g. change in host_bus->b_hnp_enable, gadget->b_hnp_enable
 *
 * Returns: 0 on success, error value otherwise.
 */
int usb_otg_kick_fsm(struct device *otg_dev)
{
	struct usb_otg *otg;

	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(otg_dev);
	mutex_unlock(&otg_list_mutex);
	if (!otg) {
		dev_dbg(otg_dev, "otg: %s: invalid otg device\n",
			__func__);
		return -ENODEV;
	}

	usb_otg_sync_inputs(otg);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_kick_fsm);

/**
 * usb_otg_register_hcd - Register the host controller to OTG core
 * @hcd:	host controller device
 * @irqnum:	interrupt number
 * @irqflags:	interrupt flags
 * @ops:	HCD ops to interface with the HCD
 *
 * This is used by the USB Host stack to register the host controller
 * to the OTG core. Host controller must not be started by the
 * caller as it is left upto the OTG state machine to do so.
 * hcd->otg_dev must contain the related otg controller device.
 *
 * Returns: 0 on success, error value otherwise.
 */
int usb_otg_register_hcd(struct usb_hcd *hcd, unsigned int irqnum,
			 unsigned long irqflags, struct otg_hcd_ops *ops)
{
	struct usb_otg *otg;
	struct device *hcd_dev = hcd->self.controller;
	struct device *otg_dev = hcd->otg_dev;

	if (!otg_dev)
		return -EINVAL;

	/* we're otg but otg controller might not yet be registered */
	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(otg_dev);
	mutex_unlock(&otg_list_mutex);
	if (!otg) {
		dev_dbg(hcd_dev,
			"otg: controller not yet registered. waiting..\n");
		/*
		 * otg controller might register later. Put the hcd in
		 * wait list and call us back when ready
		 */
		if (usb_otg_hcd_wait_add(otg_dev, hcd, irqnum, irqflags, ops)) {
			dev_err(hcd_dev, "otg: failed to add hcd to wait list\n");
			return -EINVAL;
		}

		return 0;
	}

	/* HCD will be started by OTG fsm when needed */
	mutex_lock(&otg->fsm.lock);
	if (otg->primary_hcd.hcd) {
		/* probably a shared HCD or a companion OHCI HCD ? */
		if (!(otg->flags & OTG_FLAG_HCD_NEEDS_COMPANION) &&
		    usb_otg_hcd_is_primary_hcd(hcd)) {
			dev_err(otg_dev, "otg: primary host already registered\n");
			goto err;
		}

		if (otg->flags & OTG_FLAG_HCD_NEEDS_COMPANION ||
		    (hcd->shared_hcd == otg->primary_hcd.hcd)) {
			if (otg->shared_hcd.hcd) {
				dev_err(otg_dev,
					"otg: shared/companion host already registered\n");
				goto err;
			}

			otg->shared_hcd.hcd = hcd;
			otg->shared_hcd.irqnum = irqnum;
			otg->shared_hcd.irqflags = irqflags;
			otg->shared_hcd.ops = ops;
			dev_info(otg_dev,
				 "otg: shared/companion host %s registered\n",
				 dev_name(hcd->self.controller));
		} else {
			dev_err(otg_dev,
				"otg: invalid shared/companion host %s\n",
				dev_name(hcd->self.controller));
			goto err;
		}
	} else {
		if (!usb_otg_hcd_is_primary_hcd(hcd)) {
			dev_err(otg_dev, "otg: primary host must be registered first\n");
			goto err;
		}

		otg->primary_hcd.hcd = hcd;
		otg->primary_hcd.irqnum = irqnum;
		otg->primary_hcd.irqflags = irqflags;
		otg->primary_hcd.ops = ops;
		otg->hcd_ops = ops;
		dev_info(otg_dev, "otg: primary host %s registered\n",
			 dev_name(hcd->self.controller));
	}

	/*
	 * we're ready only if we have shared HCD
	 * or we don't need shared HCD.
	 */
	if (otg->shared_hcd.hcd ||
	    (!(otg->flags & OTG_FLAG_HCD_NEEDS_COMPANION) &&
	     !otg->primary_hcd.hcd->shared_hcd)) {
		otg->host = hcd_to_bus(hcd);
		/* FIXME: set bus->otg_port if this is true OTG port with HNP */

		/* start FSM */
		usb_otg_start_fsm(otg);
	} else {
		dev_dbg(otg_dev,
			"otg: can't start till shared/companion host registers\n");
	}

	mutex_unlock(&otg->fsm.lock);

	return 0;

err:
	mutex_unlock(&otg->fsm.lock);
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(usb_otg_register_hcd);

/**
 * usb_otg_unregister_hcd - Unregister the host controller from OTG core
 * @hcd:	host controller device
 *
 * This is used by the USB Host stack to unregister the host controller
 * from the OTG core. Ensures that host controller is not running
 * on successful return.
 *
 * Returns: 0 on success, error value otherwise.
 */
int usb_otg_unregister_hcd(struct usb_hcd *hcd)
{
	struct usb_otg *otg;
	struct device *hcd_dev = hcd_to_bus(hcd)->controller;
	struct device *otg_dev = hcd->otg_dev;

	if (!otg_dev)
		return -EINVAL;	/* we're definitely not OTG */

	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(otg_dev);
	mutex_unlock(&otg_list_mutex);
	if (!otg) {
		/* are we in wait list? */
		if (!usb_otg_hcd_wait_remove(hcd))
			return 0;

		dev_dbg(hcd_dev, "otg: host wasn't registered with otg\n");
		return -EINVAL;
	}

	mutex_lock(&otg->fsm.lock);
	if (hcd == otg->primary_hcd.hcd) {
		otg->primary_hcd.hcd = NULL;
		dev_info(otg_dev, "otg: primary host %s unregistered\n",
			 dev_name(hcd_dev));
	} else if (hcd == otg->shared_hcd.hcd) {
		otg->shared_hcd.hcd = NULL;
		dev_info(otg_dev,
			 "otg: shared/companion host %s unregistered\n",
			 dev_name(hcd_dev));
	} else {
		dev_err(otg_dev, "otg: host %s wasn't registered with otg\n",
			dev_name(hcd_dev));
		mutex_unlock(&otg->fsm.lock);
		return -EINVAL;
	}

	/* stop FSM & Host */
	usb_otg_stop_fsm(otg);
	otg->host = NULL;

	mutex_unlock(&otg->fsm.lock);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_unregister_hcd);

/**
 * usb_otg_register_gadget - Register the gadget controller to OTG core
 * @gadget:	gadget controller
 *
 * This is used by the USB gadget stack to register the gadget controller
 * to the OTG core. Gadget controller must not be started by the
 * caller as it is left upto the OTG state machine to do so.
 *
 * Gadget core must call this only when all resources required for
 * gadget controller to run are available.
 * i.e. gadget function driver is available.
 *
 * Returns: 0 on success, error value otherwise.
 */
int usb_otg_register_gadget(struct usb_gadget *gadget,
			    struct otg_gadget_ops *ops)
{
	struct usb_otg *otg;
	struct device *gadget_dev = &gadget->dev;
	struct device *otg_dev = gadget->otg_dev;

	if (!otg_dev)
		return -EINVAL;	/* we're definitely not OTG */

	/* we're otg but otg controller might not yet be registered */
	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(otg_dev);
	mutex_unlock(&otg_list_mutex);
	if (!otg) {
		dev_dbg(gadget_dev,
			"otg: controller not yet registered. waiting..\n");
		/*
		 * otg controller might register later. Put the gadget in
		 * wait list and call us back when ready
		 */
		if (usb_otg_gadget_wait_add(otg_dev, gadget, ops)) {
			dev_err(gadget_dev,
				"otg: failed to add to gadget to wait list\n");
			return -EINVAL;
		}

		return 0;
	}

	mutex_lock(&otg->fsm.lock);
	if (otg->gadget) {
		dev_err(otg_dev, "otg: gadget already registered with otg\n");
		mutex_unlock(&otg->fsm.lock);
		return -EINVAL;
	}

	otg->gadget = gadget;
	otg->gadget_ops = ops;
	dev_info(otg_dev, "otg: gadget %s registered\n",
		 dev_name(&gadget->dev));

	/* start FSM */
	usb_otg_start_fsm(otg);
	mutex_unlock(&otg->fsm.lock);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_register_gadget);

/**
 * usb_otg_unregister_gadget - Unregister the gadget controller from OTG core
 * @gadget:	gadget controller
 *
 * This is used by the USB gadget stack to unregister the gadget controller
 * from the OTG core. Ensures that gadget controller is halted
 * on successful return.
 *
 * Returns: 0 on success, error value otherwise.
 */
int usb_otg_unregister_gadget(struct usb_gadget *gadget)
{
	struct usb_otg *otg;
	struct device *gadget_dev = &gadget->dev;
	struct device *otg_dev = gadget->otg_dev;

	if (!otg_dev)
		return -EINVAL;

	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(otg_dev);
	mutex_unlock(&otg_list_mutex);
	if (!otg) {
		/* are we in wait list? */
		if (!usb_otg_gadget_wait_remove(gadget))
			return 0;

		dev_dbg(gadget_dev, "otg: gadget wasn't registered with otg\n");
		return -EINVAL;
	}

	mutex_lock(&otg->fsm.lock);
	if (otg->gadget != gadget) {
		dev_err(otg_dev, "otg: gadget %s wasn't registered with otg\n",
			dev_name(&gadget->dev));
		mutex_unlock(&otg->fsm.lock);
		return -EINVAL;
	}

	/* Stop FSM & gadget */
	usb_otg_stop_fsm(otg);
	otg->gadget = NULL;
	mutex_unlock(&otg->fsm.lock);

	dev_info(otg_dev, "otg: gadget %s unregistered\n",
		 dev_name(&gadget->dev));

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_unregister_gadget);
