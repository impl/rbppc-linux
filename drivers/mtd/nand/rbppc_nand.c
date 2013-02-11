/*
 * Copyright (C) 2008-2011 Noah Fontes <nfontes@invectorate.com>
 * Copyright (C) 2009 Michael Guntsche <mike@it-loops.com>
 * Copyright (C) Mikrotik 2007
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This is a strange driver indeed. Instead of using a rational layout for
 * handling NAND operations (like, say, the fsl_upm driver), this driver uses
 * two separate UPMs plus four pins on GPIO_1. One of the UPMs is responsible
 * for actual read/write operations; the other one seems to be for ensuring
 * commands are executed serially (i.e., a sync buffer). It's referred to as as
 * either "localbus" or "nnand" in MikroTik's own code -- neither name makes
 * much sense to me. The GPIO is used for R/B and CLE/ALE/nCE.
 */

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>

#define DRV_NAME "rbppc_nand"
#define DRV_VERSION "0.1.1"

struct rbppc_nand_prv {
	struct mtd_info mtd;
	struct nand_chip chip;

	int rnb_gpio;

	int nce_gpio;
	int cle_gpio;
	int ale_gpio;

	void __iomem *cmd_sync;

	struct device *dev;
};

/*
 * We must use the OOB layout from yaffs 1 if we want this to be recognized
 * properly. Borrowed from the OpenWRT patches for the RB532.
 *
 * See <https://dev.openwrt.org/browser/trunk/target/linux/rb532/
 * patches-2.6.28/025-rb532_nand_fixup.patch> for more details.
 */
static struct nand_ecclayout rbppc_nand_oob_16 = {
	.eccbytes = 6,
	.eccpos = { 8, 9, 10, 13, 14, 15 },
	.oobavail = 9,
	.oobfree = { { 0, 4 }, { 6, 2 }, { 11, 2 }, { 4, 1 } },
};

static inline void rbppc_nand_sync(struct rbppc_nand_prv *prv) {
	/*
	 * My understanding from reading the GPIO NAND driver (gpio.c) is that
	 * this enforces a MEMBAR that the CPU itself can't provide; in other
	 * words, it forces commands to be executed synchronously.
	 */
	readb(prv->cmd_sync);
}

static int rbppc_nand_dev_ready(struct mtd_info *mtd) {
	struct nand_chip *chip = mtd->priv;
	struct rbppc_nand_prv *prv = chip->priv;

	return gpio_get_value(prv->rnb_gpio);
}

static void rbppc_nand_cmd_ctrl(struct mtd_info *mtd, int cmd, unsigned int ctrl) {
	struct nand_chip *chip = mtd->priv;
	struct rbppc_nand_prv *prv = chip->priv;

	rbppc_nand_sync(prv);

	if (ctrl & NAND_CTRL_CHANGE) {
		gpio_set_value(prv->nce_gpio, !(ctrl & NAND_NCE));
		gpio_set_value(prv->cle_gpio, !!(ctrl & NAND_CLE));
		gpio_set_value(prv->ale_gpio, !!(ctrl & NAND_ALE));

		rbppc_nand_sync(prv);
	}
	if (cmd == NAND_CMD_NONE)
		return;

	writeb(cmd, chip->IO_ADDR_W);
	rbppc_nand_sync(prv);
}

static void rbppc_nand_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	struct nand_chip *chip = mtd->priv;

	readsb(chip->IO_ADDR_R, buf, len);
}

static void rbppc_nand_write_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{
	struct nand_chip *chip = mtd->priv;

	writesb(chip->IO_ADDR_W, buf, len);
}

static void rbppc_nand_free_gpio(struct rbppc_nand_prv *prv)
{
	if (gpio_is_valid(prv->rnb_gpio))
		gpio_free(prv->rnb_gpio);
	if (gpio_is_valid(prv->nce_gpio))
		gpio_free(prv->nce_gpio);
	if (gpio_is_valid(prv->cle_gpio))
		gpio_free(prv->cle_gpio);
	if (gpio_is_valid(prv->ale_gpio))
		gpio_free(prv->ale_gpio);
}

static int rbppc_nand_probe_gpio(struct rbppc_nand_prv *prv, int rnb_gpio, int nce_gpio, int cle_gpio, int ale_gpio)
{
	struct device *dev = prv->dev;
	int retval = 0;

	prv->rnb_gpio = -1;
	prv->nce_gpio = -1;
	prv->cle_gpio = -1;
	prv->ale_gpio = -1;

	retval = gpio_request(rnb_gpio, "RouterBOARD NAND R/B");
	if (retval) {
		dev_err(dev, "Couldn't request R/B GPIO\n");
		goto err;
	}
	gpio_direction_input(rnb_gpio);
	prv->rnb_gpio = rnb_gpio;

	retval = gpio_request(nce_gpio, "RouterBOARD NAND nCE");
	if (retval) {
		dev_err(dev, "Couldn't request nCE GPIO\n");
		goto err;
	}
	gpio_direction_output(nce_gpio, 1);
	prv->nce_gpio = nce_gpio;

	retval = gpio_request(cle_gpio, "RouterBOARD NAND CLE");
	if (retval) {
		dev_err(dev, "Couldn't request CLE GPIO\n");
		goto err;
	}
	gpio_direction_output(cle_gpio, 0);
	prv->cle_gpio = cle_gpio;

	retval = gpio_request(ale_gpio, "RouterBOARD NAND ALE");
	if (retval) {
		dev_err(dev, "Couldn't request ALE GPIO\n");
		goto err;
	}
	gpio_direction_output(ale_gpio, 0);
	prv->ale_gpio = ale_gpio;

	return 0;

err:
	rbppc_nand_free_gpio(prv);
	return retval;
}

static int rbppc_nand_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rbppc_nand_prv *prv;
	struct mtd_info *mtd;
	struct nand_chip *chip;
	struct device_node *dn = dev->of_node;
	struct device_node *dn_partitions;
	struct resource res;
	int rnb_gpio, nce_gpio, cle_gpio, ale_gpio;
	void __iomem *io_addr;
	void __iomem *sync_addr;
	struct mtd_part_parser_data pp_data;
	int retval;

	printk(KERN_INFO "MikroTik RouterBOARD NAND driver for "
		"MPC83xx/MPC85xx-based platforms, version " DRV_VERSION "\n");

	prv = devm_kzalloc(dev, sizeof(*prv), GFP_KERNEL);
	if (!prv) {
		dev_err(dev, "Can't allocate memory!\n");
		return -ENOMEM;
	}

	prv->dev = dev;

	chip = &prv->chip;
	chip->priv = prv;

	mtd = &prv->mtd;
	mtd->name = DRV_NAME;
	mtd->priv = chip;
	mtd->owner = THIS_MODULE;

	rnb_gpio = of_get_gpio(dn, 0);
	if (!gpio_is_valid(rnb_gpio)) {
		dev_err(dev, "No R/B GPIO (0) found\n");
		return rnb_gpio;
	}

	nce_gpio = of_get_gpio(dn, 1);
	if (!gpio_is_valid(nce_gpio)) {
		dev_err(dev, "No nCE GPIO (1) found\n");
		return nce_gpio;
	}

	cle_gpio = of_get_gpio(dn, 2);
	if (!gpio_is_valid(cle_gpio)) {
		dev_err(dev, "No CLE GPIO (2) found\n");
		return cle_gpio;
	}

	ale_gpio = of_get_gpio(dn, 3);
	if (!gpio_is_valid(ale_gpio)) {
		dev_err(dev, "No ALE GPIO (3) found\n");
		return ale_gpio;
	}

	retval = rbppc_nand_probe_gpio(prv, rnb_gpio, nce_gpio, cle_gpio, ale_gpio);
	if (retval)
		return retval;

	/*
	 * Allocate IO resource.
	 */
	retval = of_address_to_resource(dn, 0, &res);
	if (retval) {
		dev_err(dev, "No reg property found for IO (0)\n");
		goto err_after_probe_gpio;
	}

	if (!devm_request_mem_region(dev, res.start, res.end - res.start + 1, DRV_NAME)) {
		dev_err(dev, "Could not reserve NAND memory\n");
		retval = -EBUSY;
		goto err_after_probe_gpio;
	}

	io_addr = devm_ioremap_nocache(dev, res.start, res.end - res.start + 1);
	if (!io_addr) {
		dev_err(dev, "Could not map NAND memory\n");
		retval = -ENOMEM;
		goto err_after_probe_gpio;
	}

	/*
	 * Allocate sync resource.
	 */
	retval = of_address_to_resource(dn, 1, &res);
	if (retval) {
		dev_err(dev, "No reg property found for sync (1)\n");
		goto err_after_probe_gpio;
	}

	if (!devm_request_mem_region(dev, res.start, res.end - res.start + 1, DRV_NAME)) {
		dev_err(dev, "Could not reserve sync memory\n");
		retval = -EBUSY;
		goto err_after_probe_gpio;
	}

	sync_addr = devm_ioremap_nocache(dev, res.start, res.end - res.start + 1);
	if (!sync_addr) {
		dev_err(dev, "Could not map sync memory\n");
		retval = -ENOMEM;
		goto err_after_probe_gpio;
	}

	chip->dev_ready = rbppc_nand_dev_ready;
	chip->cmd_ctrl = rbppc_nand_cmd_ctrl;
	chip->read_buf = rbppc_nand_read_buf;
	chip->write_buf = rbppc_nand_write_buf;
	chip->IO_ADDR_W = io_addr;
	chip->IO_ADDR_R = io_addr;
	chip->chip_delay = 25;
	chip->ecc.mode = NAND_ECC_SOFT;
	chip->ecc.layout = &rbppc_nand_oob_16;

	prv->cmd_sync = sync_addr;

	retval = nand_scan(mtd, 1);
	if (retval) {
		dev_err(dev, "RouterBOARD NAND device not found\n");
		goto err_after_probe_gpio;
	}

	/*
	 * Parse partitions and register device.
	 */
	dn_partitions = of_get_next_child(dn, NULL);

	pp_data.of_node = dn_partitions;
	retval = mtd_device_parse_register(&prv->mtd, NULL, &pp_data, NULL, 0);
	of_node_put(dn_partitions);
	if (retval) {
		dev_err(dev, "Could not register new MTD device\n");
		goto err_after_probe_gpio;
	}

	dev_set_drvdata(dev, prv);

	return 0;

err_after_probe_gpio:
	rbppc_nand_free_gpio(prv);
	return retval;
}

static int rbppc_nand_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rbppc_nand_prv *prv = dev_get_drvdata(dev);

	nand_release(&prv->mtd);
	rbppc_nand_free_gpio(prv);

	dev_set_drvdata(dev, NULL);

	return 0;
}

static struct of_device_id rbppc_nand_ids[] = {
	{ .compatible = "rb,nand", },
	{},
};

static struct platform_driver rbppc_nand_driver = {
	.probe	= rbppc_nand_probe,
	.remove = rbppc_nand_remove,
	.driver	= {
		.name = "rbppc-nand",
		.owner = THIS_MODULE,
		.of_match_table = rbppc_nand_ids,
	},
};

static int __init rbppc_nand_init(void)
{
	return platform_driver_register(&rbppc_nand_driver);
}

static void __exit rbppc_nand_exit(void)
{
	platform_driver_unregister(&rbppc_nand_driver);
}

MODULE_AUTHOR("Mikrotikls SIA");
MODULE_AUTHOR("Noah Fontes");
MODULE_AUTHOR("Michael Guntsche");
MODULE_DESCRIPTION("MikroTik RouterBOARD NAND driver for MPC83xx/MPC85xx-based platforms");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

module_init(rbppc_nand_init);
module_exit(rbppc_nand_exit);
