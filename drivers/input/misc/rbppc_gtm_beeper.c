/*
 * Copyright (C) 2008-2011 Noah Fontes <nfontes@invectorate.com>
 * Copyright (C) Mikrotik 2007
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>

#include <asm/fsl_gtm.h>

#define DRV_NAME "rbppc_gtm_beeper"
#define DRV_VERSION "0.1.0"

struct rbppc_gtm_beeper_prv {
	int gpio, gpio_toggle;
	int irq;

	struct gtm_timer *timer;
	struct input_dev *input;

	struct device *dev;
};

static irqreturn_t rbppc_gtm_beeper_interrupt(int irq, void *data)
{
	struct rbppc_gtm_beeper_prv *prv = data;

	if (gpio_is_valid(prv->gpio)) {
		gpio_set_value(prv->gpio, prv->gpio_toggle);
		prv->gpio_toggle ^= 1;
	}

	if (prv->timer)
		gtm_ack_timer16(prv->timer, 0xFFFF);

	return IRQ_HANDLED;
}

static int rbppc_gtm_beeper_event(struct input_dev *input, unsigned int type,
				  unsigned int code, int value)
{
	struct rbppc_gtm_beeper_prv *prv = input_get_drvdata(input);

	if (type != EV_SND || value < 0)
		return -EINVAL;

	switch (code) {
	case SND_BELL:
		value = value ? 1000 : 0;
		break;
	case SND_TONE:
		break;
	default:
		return -EINVAL;
	}

	if (value == 0)
		gtm_stop_timer16(prv->timer);
	else
		/*
		 * "reload" is actually "free run", despite what the API
		 * documentation claims.
		 */
		gtm_set_timer16(prv->timer, value, true);

	return 0;
}

static int rbppc_gtm_beeper_probe_input(struct rbppc_gtm_beeper_prv *prv)
{
	int retval = 0;

	prv->input = input_allocate_device();
	if (!prv->input) {
		dev_err(prv->dev, "Can't allocate memory!\n");
		return -ENOMEM;
	}

	prv->input->name = "rbppc-gtm-beeper";
	prv->input->phys = "rbppc/input0";
	prv->input->id.bustype = BUS_HOST;
	prv->input->id.vendor = 0x001f;
	prv->input->id.product = 0x0001;
	prv->input->id.version = 0x0100;

	prv->input->evbit[0] = BIT_MASK(EV_SND);
	prv->input->sndbit[0] = BIT_MASK(SND_TONE) | BIT_MASK(SND_BELL);

	prv->input->event = rbppc_gtm_beeper_event;

	input_set_drvdata(prv->input, prv);

	retval = input_register_device(prv->input);
	if (retval) {
		dev_err(prv->dev, "Could not register input device\n");
		input_free_device(prv->input);
	}

	return retval;
}

static int rbppc_gtm_beeper_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *dn_timer, *dn = dev->of_node;
	struct gtm *gtm;
	struct rbppc_gtm_beeper_prv *prv;
	const __be32 *prop;
	int size, retval;

	printk(KERN_INFO "MikroTik RouterBOARD GTM speaker driver for "
	       "MPC83xx/MPC85xx-based platforms, version " DRV_VERSION "\n");

	prv = devm_kzalloc(dev, sizeof(*prv), GFP_KERNEL);
	if (!prv) {
		dev_err(dev, "Can't allocate memory!\n");
		return -ENOMEM;
	}

	prv->dev = dev;

	prop = of_get_property(dn, "timer", &size);
	if (size != 2 * sizeof(*prop)) {
		dev_err(dev, "Invalid timer property\n");
		return -EINVAL;
	}

	dn_timer = of_find_node_by_phandle(be32_to_cpu(prop[0]));
	if (!dn_timer) {
		dev_err(dev, "No GTM found\n");
		return -EINVAL;
	} else if (!dn_timer->data) {
		/*
		 * The FSL GTM initialization routines map the GTM to the ->data
		 * property of the OF node.
		 */
		dev_err(dev, "GTM node has not been initialized\n");
		of_node_put(dn_timer);
		return -EINVAL;
	}

	gtm = dn_timer->data;

	of_node_put(dn_timer);

	prv->timer = gtm_get_specific_timer16(gtm, be32_to_cpu(prop[1]));
	if (IS_ERR(prv->timer)) {
		dev_err(dev, "Could not request specific timer on GTM\n");
		return PTR_ERR(prv->timer);
	}

	/*
	 * On the RB333, we need to toggle some GPIO pins every time we get an
	 * interrupt.
	 */
	prv->gpio = -1;
	if (of_device_is_compatible(dn, "rb,rb333-gtm-beeper")) {
		int gpio;
		gpio = of_get_gpio(dn, 0);
		if (!gpio_is_valid(gpio)) {
			dev_err(dev, "No GPIO found\n");
			retval = gpio;
			goto err_after_get_timer;
		}

		retval = gpio_request(gpio, "RouterBOARD Speaker");
		if (retval) {
			dev_err(dev, "Couldn't request GPIO for speaker\n");
			goto err_after_get_timer;
		}
		gpio_direction_output(gpio, 0);
		prv->gpio = gpio;
		prv->gpio_toggle = 0;
	}

	retval = devm_request_irq(dev, prv->timer->irq,
				  rbppc_gtm_beeper_interrupt, 0, DRV_NAME, prv);
	if (retval) {
		dev_err(dev, "Could not request IRQ for speaker\n");
		goto err_after_request_gpio;
	}

	retval = rbppc_gtm_beeper_probe_input(prv);
	if (retval) {
		dev_err(dev, "Could not create input device for speaker\n");
		goto err_after_request_gpio;
	}

	dev_set_drvdata(dev, prv);

	return 0;

err_after_request_gpio:
	if (gpio_is_valid(prv->gpio))
		gpio_free(prv->gpio);
err_after_get_timer:
	gtm_put_timer16(prv->timer);
	return retval;
}

static int rbppc_gtm_beeper_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rbppc_gtm_beeper_prv *prv = dev_get_drvdata(dev);

	input_unregister_device(prv->input);
	gtm_put_timer16(prv->timer);

	if (gpio_is_valid(prv->gpio))
		gpio_free(prv->gpio);

	dev_set_drvdata(dev, NULL);

	return 0;
}

static struct of_device_id rbppc_gtm_beeper_ids[] = {
	{ .compatible = "rb,gtm-beeper", },
	{ },
};

static struct platform_driver rbppc_gtm_beeper_driver = {
	.probe = rbppc_gtm_beeper_probe,
	.remove = rbppc_gtm_beeper_remove,
	.driver = {
		.name = "rbppc-gtm-beeper",
		.owner = THIS_MODULE,
		.of_match_table = rbppc_gtm_beeper_ids,
	},
};

static int __init rbppc_gtm_beeper_init(void)
{
	return platform_driver_register(&rbppc_gtm_beeper_driver);
}

static void __exit rbppc_gtm_beeper_exit(void)
{
	platform_driver_unregister(&rbppc_gtm_beeper_driver);
}

MODULE_AUTHOR("Mikrotikls SIA");
MODULE_AUTHOR("Noah Fontes");
MODULE_DESCRIPTION("MikroTik RouterBOARD GTM speaker driver for MPC83xx/MPC85xx-based platforms");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

module_init(rbppc_gtm_beeper_init);
module_exit(rbppc_gtm_beeper_exit);
