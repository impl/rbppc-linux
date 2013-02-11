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

#include <sysdev/fsl_soc.h>
#include <sysdev/fsl_pci.h>

#include "mpc83xx.h"

#define SICRL_GPIO1C_MASK	0x00800000
#define SICRL_GPIO1L_MASK 	0x00003000
#define SICRL_GPIO1L_GTM1_TOUT4	0x00001000

#define GP1DIR_OFFS		0xc00
#define GP1DIR_MASK(pin)	(1 << (31 - (pin)))
#define GP1DAT_OFFS		0xc08
#define GP1DAT_MASK(pin)	(1 << (31 - (pin)))

static void __init rb600_setup_arch(void) {
	void __iomem *cfg;
#ifdef CONFIG_PCI
	struct device_node *np;
#endif

	/*
	 * We do have to configure GTM1_TOUT4 to be used instead of GPIO1[11] if
	 * we want the speaker to work.
	 */
	cfg = ioremap(get_immrbase(), 0x1000);
	if (cfg) {
		clrsetbits_be32(cfg + MPC83XX_SICRL_OFFS, SICRL_GPIO1L_MASK,
				SICRL_GPIO1L_GTM1_TOUT4);
		iounmap(cfg);
	}

#ifdef CONFIG_PCI
	for_each_compatible_node(np, "pci", "fsl,mpc8349-pci")
		mpc83xx_add_bridge(np);
#endif
}

static void __init rb600_init_IRQ(void)
{
	struct device_node *np;

	np = of_find_node_by_type(NULL, "ipic");
	if (!np)
		return;

	ipic_init(np, 0);
	ipic_set_default_priority();

	of_node_put(np);
}

static int __init rb600_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

        return of_flat_dt_is_compatible(root, "RB600");
}

static void rb600_restart(char *cmd) {
	void __iomem *cfg;

	cfg = ioremap(get_immrbase(), 0x1000);
	if (cfg) {
		local_irq_disable();

		/*
		 * Make sure GPIO1[2] is active.
		 */
		clrbits32(cfg + MPC83XX_SICRL_OFFS, SICRL_GPIO1C_MASK);

		/*
		 * Grab GPIO1 (at 0xc00), put the third pin into output mode,
		 * and zero it out.
		 */
		clrsetbits_be32(cfg + GP1DIR_OFFS, GP1DIR_MASK(2), GP1DIR_MASK(2));
		clrbits32(cfg + GP1DAT_OFFS, GP1DAT_MASK(2));

		for (;;) ;
	}
	else
		mpc83xx_restart(cmd);
}

static struct of_device_id rb600_ids[] = {
	{ .compatible = "fsl,pq2pro-localbus", },
	{ .compatible = "simple-bus", },
	{ .compatible = "gianfar", },
	{ },
};

static int __init rb600_declare_of_platform_devices(void)
{
	return of_platform_bus_probe(NULL, rb600_ids, NULL);
}
machine_device_initcall(rb600, rb600_declare_of_platform_devices);

define_machine(rb600) {
	.name 				= "MikroTik RouterBOARD 600 series",
	.probe 				= rb600_probe,
	.setup_arch 			= rb600_setup_arch,
	.init_IRQ 			= rb600_init_IRQ,
	.get_irq 			= ipic_get_irq,
	.restart 			= rb600_restart,
	.time_init 			= mpc83xx_time_init,
	.calibrate_decr			= generic_calibrate_decr,
};
