/*
 * Copyright (C) 2010 Alexandros C. Couloumbis <alex@ozo.com>
 * Copyright (C) 2008-2011 Noah Fontes <nfontes@invectorate.com>
 * Copyright (C) 2009 Michael Guntsche <mike@it-loops.com>
 * Copyright (C) Mikrotik 2007
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/root_dev.h>
#include <linux/initrd.h>
#include <linux/interrupt.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>

#include <asm/time.h>
#include <asm/ipic.h>
#include <asm/udbg.h>
#include <asm/pci-bridge.h>
#include <asm/io.h>
#include <asm/qe.h>
#include <asm/qe_ic.h>

#include <sysdev/fsl_soc.h>
#include <sysdev/fsl_pci.h>

#include "mpc83xx.h"

#define CPDIR1A_OFFS		0x1408
#define CPDIR1A_DIR4_MASK	0x00c00000
#define CPDIR1A_DIR4_OUT	0x00400000
#define CPDATA_OFFS		0x1404
#define CPDATA_D4_MASK		0x08000000

static void __init rb333_setup_arch(void)
{
#if defined (CONFIG_PCI) || defined (CONFIG_QUICC_ENGINE)
	struct device_node *np;
#endif

#ifdef CONFIG_PCI
	for_each_compatible_node(np, "pci", "fsl,mpc8349-pci")
		mpc83xx_add_bridge(np);
#endif

#ifdef CONFIG_QUICC_ENGINE
	qe_reset();

	if ((np = of_find_node_by_name(NULL, "par_io")) != NULL) {
		par_io_init(np);
		of_node_put(np);

		for (np = NULL; (np = of_find_node_by_name(np, "ucc")) != NULL;)
			par_io_of_config(np);
	}
#endif
}

static void __init rb333_init_IRQ(void)
{
	struct device_node *np;

	np = of_find_node_by_type(NULL, "ipic");
	if (!np)
		return;

	ipic_init(np, 0);
	ipic_set_default_priority();

	of_node_put(np);

#ifdef CONFIG_QUICC_ENGINE
	np = of_find_compatible_node(NULL, NULL, "fsl,qe-ic");
	if (!np)
		return;

	qe_ic_init(np, 0, qe_ic_cascade_low_ipic, qe_ic_cascade_high_ipic);
	of_node_put(np);
#endif
}

static int __init rb333_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	return of_flat_dt_is_compatible(root, "RB333");
}

static void rb333_restart(char *cmd)
{
	void __iomem *cfg;

	cfg = ioremap(get_immrbase(), 0x2000);
	if (cfg) {
		local_irq_disable();

		/*
		 * GPIO on QE port A (at 0x1400): Put the fifth pin into output
		 * mode and zero it out.
		 */
		clrsetbits_be32(cfg + CPDIR1A_OFFS, CPDIR1A_DIR4_MASK,
				CPDIR1A_DIR4_OUT);
		clrbits32(cfg + CPDATA_OFFS, CPDATA_D4_MASK);

		for (;;) ;
	}
	else
		mpc83xx_restart(cmd);
}

static struct of_device_id rb333_ids[] = {
	{ .compatible = "fsl,pq2pro-localbus", },
	{ .compatible = "simple-bus", },
	{ .compatible = "fsl,qe", },
	{ },
};

static int __init rb333_declare_of_platform_devices(void)
{
	return of_platform_bus_probe(NULL, rb333_ids, NULL);
}
machine_device_initcall(rb333, rb333_declare_of_platform_devices);

define_machine(rb333) {
	.name 				= "MikroTik RouterBOARD 333 series",
	.probe 				= rb333_probe,
	.setup_arch 			= rb333_setup_arch,
	.init_IRQ 			= rb333_init_IRQ,
	.get_irq 			= ipic_get_irq,
	.restart 			= rb333_restart,
	.time_init 			= mpc83xx_time_init,
	.calibrate_decr			= generic_calibrate_decr,
};
