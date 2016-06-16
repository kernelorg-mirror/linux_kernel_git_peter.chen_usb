/*
 * usb_generic.c	The generic power sequence driver for USB device
 *
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 * Author: Peter Chen <peter.chen@nxp.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/pwrseq.h>

struct usb_pwrseq_generic {
	struct pwrseq pwrseq;
	struct gpio_desc *gpiod_reset;
	struct gpio_desc *gpiod_enable;
	struct clk *clk;
	struct regulator *reg_power;
};

#define to_pwrseq_generic(p) container_of(p, struct usb_pwrseq_generic, pwrseq)

static void usb_pwrseq_power_off(struct pwrseq *_pwrseq)
{
	struct usb_pwrseq_generic *pwrseq = to_pwrseq_generic(_pwrseq);

	if (pwrseq->clk)
		clk_disable_unprepare(pwrseq->clk);

	if (pwrseq->gpiod_enable)
		gpiod_set_value(pwrseq->gpiod_enable, 0);

	if (pwrseq->reg_power)
		regulator_disable(pwrseq->reg_power);
}

static int usb_pwrseq_power_on(struct pwrseq *_pwrseq)
{
	struct usb_pwrseq_generic *pwrseq = to_pwrseq_generic(_pwrseq);
	struct gpio_desc *gpiod_reset = pwrseq->gpiod_reset;
	struct gpio_desc *gpiod_enable = pwrseq->gpiod_enable;
	struct device *dev = _pwrseq->dev;
	struct device_node *node = dev->of_node;
	u32 duration_us = 50, clk_rate = 0;
	int ret;

	if (pwrseq->clk) {
		ret = clk_prepare_enable(pwrseq->clk);
		if (ret) {
			dev_err(dev,
				"Can't enable external clock: %d\n",
				ret);
			return ret;
		}

		of_property_read_u32(node, "clock-frequency", &clk_rate);
		if (clk_rate) {
			ret = clk_set_rate(pwrseq->clk, clk_rate);
			if (ret) {
				dev_err(dev, "Error setting clock rate\n");
				goto disable_clk;
			}
		}
	}

	if (gpiod_reset) {
		of_property_read_u32(node, "reset-duration-us", &duration_us);
		gpiod_direction_output(gpiod_reset, 1);

		gpiod_set_value(gpiod_reset, 1);
		usleep_range(duration_us, duration_us + 10);
		gpiod_set_value(gpiod_reset, 0);
	}

	if (gpiod_enable) {
		gpiod_direction_output(gpiod_enable, 1);
		gpiod_set_value(gpiod_enable, 1);
	}

	if (pwrseq->reg_power) {
		ret = regulator_enable(pwrseq->reg_power);
		if (ret) {
			dev_err(dev, "Failed to power regulator, ret=%d\n",
					ret);
			goto disable_gpio;
		}
	}

	return 0;

disable_gpio:
	if (gpiod_enable)
		gpiod_set_value(gpiod_enable, 0);

disable_clk:
	if (pwrseq->clk)
		clk_disable_unprepare(pwrseq->clk);
	return ret;
}

static const struct pwrseq_ops usb_pwrseq_generic_ops = {
	.pre_power_on = usb_pwrseq_power_on,
	.power_off = usb_pwrseq_power_off,
};

static int usb_pwrseq_generic_probe(struct platform_device *pdev)
{
	struct usb_pwrseq_generic *pwrseq;
	struct device *dev = &pdev->dev;
	struct regulator *reg;
	int ret;

	pwrseq = devm_kzalloc(dev, sizeof(*pwrseq), GFP_KERNEL);
	if (!pwrseq)
		return -ENOMEM;

	pwrseq->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(pwrseq->clk)) {
		dev_dbg(dev, "Can't get clock: %ld\n",
			PTR_ERR(pwrseq->clk));
		pwrseq->clk = NULL;
	}

	pwrseq->gpiod_reset = devm_gpiod_get_optional(dev, "reset",
			GPIOD_ASIS);
	ret = PTR_ERR_OR_ZERO(pwrseq->gpiod_reset);
	if (ret) {
		dev_err(dev, "Failed to get reset gpio, err = %d\n", ret);
		return ret;
	}

	pwrseq->gpiod_enable = devm_gpiod_get_optional(dev, "enable",
			GPIOD_ASIS);
	ret = PTR_ERR_OR_ZERO(pwrseq->gpiod_enable);
	if (ret) {
		dev_err(dev, "Failed to get enable gpio, err = %d\n", ret);
		return ret;
	}

	reg = devm_regulator_get(dev, "power");
	if (PTR_ERR(reg) == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	else if (PTR_ERR(reg) == -ENODEV)
		/* no power regulator is needed */
		reg = NULL;
	else if (IS_ERR(reg))
		return PTR_ERR(reg);
	pwrseq->reg_power = reg;

	pwrseq->pwrseq.dev = dev;
	pwrseq->pwrseq.ops = &usb_pwrseq_generic_ops;
	pwrseq->pwrseq.owner = THIS_MODULE;
	platform_set_drvdata(pdev, pwrseq);

	return pwrseq_register(&pwrseq->pwrseq);
}

static int usb_pwrseq_generic_remove(struct platform_device *pdev)
{
	struct usb_pwrseq_generic *pwrseq = platform_get_drvdata(pdev);

	pwrseq_unregister(&pwrseq->pwrseq);

	return 0;
}

static struct platform_driver usb_pwrseq_generic_driver = {
	.probe = usb_pwrseq_generic_probe,
	.remove = usb_pwrseq_generic_remove,
	.driver = {
		.name = "usb_pwrseq_generic",
	},
};

module_platform_driver(usb_pwrseq_generic_driver);
MODULE_LICENSE("GPL v2");
