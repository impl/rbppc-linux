/*
 * Copyright (C) 2008-2011 Noah Fontes <nfontes@invectorate.com>
 * Copyright (C) 2009 Michael Guntsche <mike@it-loops.com>
 * Copyright (C) Mikrotik 2007
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/init.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <asm/io.h>

#define DRV_NAME	"rbppc_nand"
#define DRV_VERSION	"0.1.0"

struct rbppc_nand_prv {
	struct mtd_info mtd;
	struct nand_chip chip;

	void *gpi;
	void *gpo;
	void *local_bus;

	struct device *dev;

	unsigned gpio_rdy;
	unsigned gpio_nce;
	unsigned gpio_cle;
	unsigned gpio_ale;
	unsigned gpio_ctrls;
};

/* We must use the OOB layout from yaffs 1 if we want this to be recognized
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

static int rbppc_nand_dev_ready(struct mtd_info *mtd) {
	struct nand_chip *chip = mtd->priv;
	struct rbppc_nand_prv *prv = chip->priv;

	return in_be32(prv->gpi) & prv->gpio_rdy;
}

static void rbppc_nand_cmd_ctrl(struct mtd_info *mtd, int cmd, unsigned int ctrl) {
	struct nand_chip *chip = mtd->priv;
	struct rbppc_nand_prv *prv = chip->priv;

	if (ctrl & NAND_CTRL_CHANGE) {
		unsigned val = in_be32(prv->gpo);
		if (!(val & prv->gpio_nce))
			/* make sure Local Bus has done NAND operations */
			readb(prv->local_bus);

		if (ctrl & NAND_CLE)
			val |= prv->gpio_cle;
		else
			val &= ~prv->gpio_cle;

		if (ctrl & NAND_ALE)
			val |= prv->gpio_ale;
		else
			val &= ~prv->gpio_ale;

		if (!(ctrl & NAND_NCE))
			val |= prv->gpio_nce;
		else
			val &= ~prv->gpio_nce;

		out_be32(prv->gpo, val);

		/* make sure GPIO output has changed */
		val ^= in_be32(prv->gpo);
		if (val & prv->gpio_ctrls) {
			dev_err(prv->dev, "NAND GPO change failed (0x%08x)\n", val);
		}
	}

	if (cmd != NAND_CMD_NONE) writeb(cmd, chip->IO_ADDR_W);
}

static void rbppc_nand_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	struct nand_chip *chip = mtd->priv;

	memcpy(buf, chip->IO_ADDR_R, len);
}

static int rbppc_nand_probe_gpio(struct rbppc_nand_prv *prv, unsigned phandle)
{
	struct device *dev = prv->dev;
	struct device_node *dn_gpio;
	struct resource res;
	int retval = 0;

	dn_gpio = of_find_node_by_phandle(phandle);
	if (!dn_gpio) {
		dev_err(dev, "No GPIO<%x> node found\n", phandle);
		return -EINVAL;
	}

	retval = of_address_to_resource(dn_gpio, 0, &res);
	if (retval) {
		dev_err(dev, "No reg property in GPIO found\n");
		goto out;
	}

	if (!devm_request_mem_region(dev, res.start, res.end - res.start + 1, DRV_NAME)) {
		dev_err(dev, "Could not reserve GPIO memory (0) for NAND\n");
		retval = -EBUSY;
		goto out;
	}

	prv->gpo = devm_ioremap_nocache(dev, res.start, res.end - res.start + 1);
	if (!prv->gpo) {
		dev_err(dev, "Could not map GPIO memory (0) for NAND\n");
		retval = -ENOMEM;
		goto out;
	}

	if (of_address_to_resource(dn_gpio, 1, &res) != 0)
		prv->gpi = prv->gpo;
	else {
		if (!devm_request_mem_region(dev, res.start, res.end - res.start + 1, DRV_NAME)) {
			dev_err(dev, "Could not reserve GPIO memory (1) for NAND\n");
			retval = -EBUSY;
			goto out;
		}

		prv->gpi = devm_ioremap_nocache(dev, res.start, res.end - res.start + 1);
		if (!prv->gpi) {
			dev_err(dev, "Could not map GPIO memory (1) for NAND\n");
			retval = -ENOMEM;
			goto out;
		}
	}

out:
	of_node_put(dn_gpio);
	return retval;
}

static int rbppc_nand_probe_nnand(struct rbppc_nand_prv *prv, unsigned phandle)
{
	struct device *dev = prv->dev;
	struct device_node *dn_nnand;
	struct resource res;
	int retval = 0;

	dn_nnand = of_find_node_by_phandle(phandle);
	if (!dn_nnand) {
		dev_err(dev, "No nNAND<%x> node found\n", phandle);
		return -EINVAL;
	}

	retval = of_address_to_resource(dn_nnand, 0, &res);
	if (retval) {
		dev_err(dev, "No reg property in nNAND found\n");
		goto out;
	}

	if (!devm_request_mem_region(dev, res.start, res.end - res.start + 1, DRV_NAME)) {
		dev_err(dev, "Could not reserve nNAND memory for NAND\n");
		retval = -EBUSY;
		goto out;
	}

	prv->local_bus = devm_ioremap_nocache(dev, res.start, res.end - res.start + 1);
	if (!prv->local_bus) {
		dev_err(dev, "Could not map nNAND memory for NAND\n");
		retval = -ENOMEM;
		goto out;
	}

out:
	of_node_put(dn_nnand);
	return retval;
}

#ifdef CONFIG_MTD_CMDLINE_PARTS
static const char *rbppc_nand_pprobes[] = {
	"cmdlinepart",
	NULL,
};
#endif

static __devinit int rbppc_nand_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rbppc_nand_prv *prv;
	struct mtd_info *mtd;
	struct nand_chip *chip;
	struct device_node *dn = dev->of_node;
	struct resource res;
	const unsigned *rdy, *nce, *cle, *ale;
	const unsigned *nnand_phandle;
	void *baddr;
	struct mtd_partition *partitions;
	int partition_count = 0;
	int retval;

	printk(KERN_INFO "MikroTik RouterBOARD 333/600 series NAND driver, version " DRV_VERSION "\n");

	prv = devm_kzalloc(dev, sizeof(*prv), GFP_KERNEL);
	if (!prv) {
		dev_err(dev, "Can't allocate memory!\n");
		return -ENOMEM;
	}

	prv->dev = dev;

	chip = &prv->chip;
	chip->priv = prv;

	mtd = &prv->mtd;
	mtd->name = "rbppc_nand";
	mtd->priv = chip;
	mtd->owner = THIS_MODULE;

	rdy = of_get_property(dn, "rdy", NULL);
	nce = of_get_property(dn, "nce", NULL);
	cle = of_get_property(dn, "cle", NULL);
	ale = of_get_property(dn, "ale", NULL);

	if (!rdy || !nce || !cle || !ale) {
		dev_err(dev, "GPIO properties are missing\n");
		return -EINVAL;
	}
	if (rdy[0] != nce[0] || rdy[0] != cle[0] || rdy[0] != ale[0]) {
		dev_err(dev, "NAND chip must be on the same GPIO\n");
		return -EINVAL;
	}

	retval = rbppc_nand_probe_gpio(prv, rdy[0]);
	if (retval)
		return retval;

	prv->gpio_rdy = 1 << (31 - rdy[1]);
	prv->gpio_nce = 1 << (31 - nce[1]);
	prv->gpio_cle = 1 << (31 - cle[1]);
	prv->gpio_ale = 1 << (31 - ale[1]);
	prv->gpio_ctrls = prv->gpio_nce | prv->gpio_cle | prv->gpio_ale;

	nnand_phandle = of_get_property(dn, "nnand", NULL);
	if (!nnand_phandle || !*nnand_phandle) {
		dev_err(dev, "Could not find reference to nNAND\n");
		return -EINVAL;
	}

	retval = rbppc_nand_probe_nnand(prv, *nnand_phandle);
	if (retval)
		return retval;

	retval = of_address_to_resource(dn, 0, &res);
	if (retval) {
		dev_err(dev, "No reg property found\n");
		return retval;
	}

	if (!devm_request_mem_region(dev, res.start, res.end - res.start + 1, DRV_NAME)) {
		dev_err(dev, "Could not reserve NAND memory\n");
		return -EBUSY;
	}

	baddr = devm_ioremap_nocache(dev, res.start, res.end - res.start + 1);
	if (!baddr) {
		dev_err(dev, "Could not map NAND memory\n");
		return -ENOMEM;
	}

	chip->dev_ready = rbppc_nand_dev_ready;
	chip->cmd_ctrl = rbppc_nand_cmd_ctrl;
	chip->read_buf = rbppc_nand_read_buf;
	chip->IO_ADDR_W = baddr;
	chip->IO_ADDR_R = baddr;
	chip->chip_delay = 25;
	chip->ecc.mode = NAND_ECC_SOFT;
	chip->ecc.layout = &rbppc_nand_oob_16;
	chip->options |= NAND_NO_AUTOINCR;

	retval = nand_scan(mtd, 1);
	if (retval) {
		dev_err(dev, "RouterBOARD NAND device not found\n");
		return retval;
	}

#ifdef CONFIG_MTD_CMDLINE_PARTS
	partition_count = parse_mtd_partitions(mtd, rbppc_nand_pprobes, &partitions, 0);
#endif
#ifdef CONFIG_MTD_OF_PARTS
	if (partition_count == 0)
		partition_count = of_mtd_parse_partitions(dev, dn, &partitions);
#endif
	if (partition_count < 0) {
		dev_err(dev, "Could not map partitions\n");
		return partition_count;
	} else if (partition_count == 0) {
		dev_err(dev, "Could not map any partitions on NAND device (try "
			"enabling CONFIG_MTD_CMDLINE_PARTS or "
			"CONFIG_MTD_OF_PARTS)\n");
		return -EINVAL;
	}

	retval = mtd_device_register(mtd, partitions, partition_count);
	if (retval) {
		dev_err(dev, "Could not register new MTD device\n");
		return retval;
	}

	dev_set_drvdata(dev, mtd);

	return 0;
}

static int __devexit rbppc_nand_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtd_info *mtd = dev_get_drvdata(dev);

	nand_release(mtd);

	return 0;
}

static struct of_device_id rbppc_nand_ids[] __devinitdata = {
	{ .compatible = "rb,nand", },
	{},
};

static struct platform_driver rbppc_nand_driver = {
	.probe	= rbppc_nand_probe,
	.remove = __devexit_p(rbppc_nand_remove),
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
MODULE_DESCRIPTION("MikroTik RouterBOARD 333/600 series NAND driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

module_init(rbppc_nand_init);
module_exit(rbppc_nand_exit);
