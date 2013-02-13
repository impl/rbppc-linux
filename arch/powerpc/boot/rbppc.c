/*
 * The RouterBOARD platform -- for booting RouterBOARDs based on
 * MPC83xx/MPC85xx SoC CPUs.
 *
 * Copyright (C) 2011 Noah Fontes <nfontes@invectorate.com>
 * Copyright (C) 2010 Alexandros C. Couloumbis <alex@ozo.com>
 * Copyright (C) 2009 Michael Guntsche <mike@it-loops.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "ops.h"
#include "types.h"
#include "io.h"
#include "stdio.h"
#include <libfdt.h>

BSS_STACK(4096);

const void *firmware_dtb_start;
u64 memsize64;

struct rbppc_ethernet_map {
	char *firmware_dtb_path;
	char *alias;
};

static const struct rbppc_ethernet_map ethernet_maps[] = {
	/*
	 * RB333 (MPC832x/QE)
	 */
	{ .firmware_dtb_path = "/qe@e0100000/ucc@2200",
	  .alias = "ethernet0", },
	{ .firmware_dtb_path = "/qe@e0100000/ucc@3200",
	  .alias = "ethernet1", },
	{ .firmware_dtb_path = "/qe@e0100000/ucc@3000",
	  .alias = "ethernet2", },

	/*
	 * RB600 (MPC834x)
	 */
	{ .firmware_dtb_path = "/soc8343@e0000000/ethernet@24000",
	  .alias = "ethernet1", },
	{ .firmware_dtb_path = "/soc8343@e0000000/ethernet@25000",
	  .alias = "ethernet0", },

	{ },
};

static void rbppc_fixup_mac_addresses(void)
{
	const struct rbppc_ethernet_map *maps = ethernet_maps;
	struct rbppc_ethernet_map map;

	while((map = *maps++).firmware_dtb_path != NULL) {
		const u32 *prop;
		int node, size;

		node = fdt_path_offset(firmware_dtb_start,
				       map.firmware_dtb_path);
		if (node < 0)
			continue;

		prop = fdt_getprop(firmware_dtb_start, node, "mac-address",
				   &size);
		if (size != 6 * sizeof(u8))
			continue;

		dt_fixup_mac_address_by_alias(map.alias, (const u8 *)prop);
	}
}

static void rbppc_fixups(void)
{
	u32 timebase_frequency, clock_frequency, bus_frequency;
	void *dev;
	int node, size;

	/*
	 * Assign memory address.
	 */
	dt_fixup_memory(0, memsize64);

	/*
	 * Assign CPU clock frequency, time-base frequency, and bus frequency.
	 * The MPC834x documentation states that time-base frequency is equal
	 * to one-quarter bus frequency.
	 */
	node = fdt_node_offset_by_prop_value(firmware_dtb_start, -1,
					     "device_type", "cpu",
					     sizeof("cpu"));
	if (node < 0)
		fatal("Cannot find CPU node\n\r");

	clock_frequency = *(const u32 *)fdt_getprop(firmware_dtb_start, node,
						    "clock-frequency", &size);
	timebase_frequency = *(const u32 *)fdt_getprop(firmware_dtb_start, node,
						       "timebase-frequency",
						       &size);
	bus_frequency = timebase_frequency * 4;

	dt_fixup_cpu_clocks(clock_frequency, timebase_frequency, bus_frequency);

	/*
	 * Assign bus frequency to SoC node, serial devices, and GTMs.
	 *
	 * Borrowed from cuboot-83xx.c.
	 */
	dev = find_node_by_devtype(NULL, "soc");
	if (dev) {
		void *child;

		setprop_val(dev, "bus-frequency", bus_frequency);

		child = NULL;
		while ((child = find_node_by_devtype(child, "serial"))) {
			if (get_parent(child) != dev)
				continue;

			setprop_val(child, "clock-frequency", bus_frequency);
		}

		child = NULL;
		while ((child = find_node_by_compatible(child, "fsl,gtm"))) {
			if (get_parent(child) != dev)
				continue;

			setprop_val(child, "clock-frequency", bus_frequency);
		}
	}

	/*
	 * Fix up NIC MAC addresses. RB333 and RB600 vary here.
	 */
	rbppc_fixup_mac_addresses();

	/*
	 * Set up /chosen so it contains the boot parameters specified in the
	 * kernelparm segment of the image.
	 */
	dev = finddevice("/chosen");
	node = fdt_path_offset(firmware_dtb_start, "/chosen");
	if (dev && node >= 0) {
		const char *bootargs = fdt_getprop(firmware_dtb_start, node,
						   "bootargs", &size);
		if (size > 0)
			setprop_str(dev, "bootargs", bootargs);
	}
}

void platform_init(unsigned long r3, unsigned long r4, unsigned long r5,
		   unsigned long r6, unsigned long r7)
{
	const u32 *reg;
	int node, size;

	/*
	 * Make sure we're going to start with a device tree that's not insane.
	 */
	if (fdt_check_header(_dtb_start) != 0)
		fatal("Invalid device tree blob\n\r");

	firmware_dtb_start = (const void *)r3;

	/*
	 * Allocate memory based on the size that the bootloader device tree
	 * reports.
	 */
	node = fdt_node_offset_by_prop_value(firmware_dtb_start, -1,
					     "device_type", "memory",
					     sizeof("memory"));
	if (node < 0)
		fatal("Cannot find memory node\n\r");

	reg = fdt_getprop(firmware_dtb_start, node, "reg", &size);
	if (size != 2 * sizeof(u32))
		fatal("Cannot get memory range\n\r");

	memsize64 = reg[1];
	simple_alloc_init(_end, memsize64 - (unsigned long)_end, 32, 64);

	/*
	 * Use our device-tree for actual initialization, like in simpleboot.
	 */
	fdt_init(_dtb_start);

	/*
	 * And finish everything up; we'll fixup our blob with correct values
	 * for clocks and MAC address shortly.
	 */
	serial_console_init();
	platform_ops.fixups = rbppc_fixups;
}
