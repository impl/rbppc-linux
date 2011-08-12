/*
 * The RouterBOARD platform -- for booting RB600(A) RouterBOARDs.
 *
 * Author: Michael Guntsche <mike@it-loops.com>
 *
 * Copyright (c) 2009 Michael Guntsche
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

BSS_STACK(4*1024);

u64 memsize64;
const void *fw_dtb;

static void rb600_fixups(void)
{
	const u32 *reg, *timebase, *clock;
	int node, size;
	void *chosen;
	const char* bootargs;

	dt_fixup_memory(0, memsize64);

	/* Set the MAC addresses. */
	node = fdt_path_offset(fw_dtb, "/soc8343@e0000000/ethernet@24000");
	reg = fdt_getprop(fw_dtb, node, "mac-address", &size);
	dt_fixup_mac_address_by_alias("ethernet1", (const u8 *)reg);

	node = fdt_path_offset(fw_dtb, "/soc8343@e0000000/ethernet@25000");
	reg = fdt_getprop(fw_dtb, node, "mac-address", &size);
	dt_fixup_mac_address_by_alias("ethernet0", (const u8 *)reg);

	/* Find the CPU timebase and clock frequencies. */
	node = fdt_node_offset_by_prop_value(fw_dtb, -1, "device_type", "cpu", sizeof("cpu"));
	timebase = fdt_getprop(fw_dtb, node, "timebase-frequency", &size);
	clock = fdt_getprop(fw_dtb, node, "clock-frequency", &size);
	dt_fixup_cpu_clocks(*clock, *timebase, 0);

}

void platform_init(unsigned long r3, unsigned long r4, unsigned long r5,
		   unsigned long r6, unsigned long r7)
{
	const u32 *reg;
	int node, size;

	fw_dtb = (const void *)r3;
	
	/* Find the memory range. */
	node = fdt_node_offset_by_prop_value(fw_dtb, -1, "device_type", "memory", sizeof("memory"));
	reg = fdt_getprop(fw_dtb, node, "reg", &size);
	memsize64 = reg[1];

	/* Now we have the memory size; initialize the heap. */
	simple_alloc_init(_end, memsize64 - (unsigned long)_end, 32, 64);

	/* Prepare the device tree and find the console. */
	fdt_init(_dtb_start);
	serial_console_init();

	/* Remaining fixups... */
	platform_ops.fixups = rb600_fixups;
}
