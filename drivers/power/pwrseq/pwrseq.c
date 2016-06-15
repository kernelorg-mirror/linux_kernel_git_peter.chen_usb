/*
 *  Copyright (C) 2014 Linaro Ltd
 *
 * Author: Ulf Hansson <ulf.hansson@linaro.org>
 *
 * License terms: GNU General Public License (GPL) version 2
 *
 * Power sequence management helpers
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pwrseq.h>


static DEFINE_MUTEX(pwrseq_list_mutex);
static LIST_HEAD(pwrseq_list);

/**
 * pwrseq_alloc: allocate one power sequence instance for host
 *
 * @np: device node which power sequence is contained
 * @dev_name: if power sequence device has already created, it is NULL,
 * (eg: mmc); else it is the name for power sequence device (eg: usb).
 *
 * This function returns power sequence pointer for this node if succeed,
 * otherwise, returns an error pointer.
 */
struct pwrseq *pwrseq_alloc(struct device_node *np, const char *dev_name)
{
	struct pwrseq *p, *pwrseq = NULL;
	bool created;

	/* If there is no device is associated with this node, create it */
	if (!of_find_device_by_node(np)) {
		if (of_platform_device_create(np, dev_name, NULL))
			created = true;
		else
			return ERR_PTR(-ENODEV);
	}

	mutex_lock(&pwrseq_list_mutex);
	list_for_each_entry(p, &pwrseq_list, pwrseq_node) {
		if (p->dev->of_node == np) {
			if (!try_module_get(p->owner))
				dev_err(p->dev,
					"increasing module refcount failed\n");
			else
				pwrseq = p;
			break;
		}
	}

	mutex_unlock(&pwrseq_list_mutex);

	if (!pwrseq)
		return ERR_PTR(-EPROBE_DEFER);

	pwrseq->create_dev_from_alloc = created;
	dev_info(p->dev, "pwrseq is allocated\n");

	return pwrseq;
}
EXPORT_SYMBOL_GPL(pwrseq_alloc);

int pwrseq_pre_power_on(struct pwrseq *pwrseq)
{
	if (pwrseq && pwrseq->ops->pre_power_on)
		return pwrseq->ops->pre_power_on(pwrseq);
	else
		return 0;
}
EXPORT_SYMBOL_GPL(pwrseq_pre_power_on);

void pwrseq_post_power_on(struct pwrseq *pwrseq)
{
	if (pwrseq && pwrseq->ops->post_power_on)
		pwrseq->ops->post_power_on(pwrseq);
}
EXPORT_SYMBOL_GPL(pwrseq_post_power_on);

void pwrseq_power_off(struct pwrseq *pwrseq)
{
	if (pwrseq && pwrseq->ops->power_off)
		pwrseq->ops->power_off(pwrseq);
}
EXPORT_SYMBOL_GPL(pwrseq_power_off);

void pwrseq_free(struct pwrseq *pwrseq)
{
	if (pwrseq) {
		if (pwrseq->create_dev_from_alloc)
			device_unregister(pwrseq->dev);
		module_put(pwrseq->owner);
	}
}
EXPORT_SYMBOL_GPL(pwrseq_free);

int pwrseq_register(struct pwrseq *pwrseq)
{
	if (!pwrseq || !pwrseq->ops || !pwrseq->dev)
		return -EINVAL;

	mutex_lock(&pwrseq_list_mutex);
	list_add(&pwrseq->pwrseq_node, &pwrseq_list);
	mutex_unlock(&pwrseq_list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(pwrseq_register);

void pwrseq_unregister(struct pwrseq *pwrseq)
{
	if (pwrseq) {
		mutex_lock(&pwrseq_list_mutex);
		list_del(&pwrseq->pwrseq_node);
		mutex_unlock(&pwrseq_list_mutex);
	}
}
EXPORT_SYMBOL_GPL(pwrseq_unregister);
