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
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/libata.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include <asm/fsl_lbc.h>

#define DRV_NAME	"pata_rbppc_upm"
#define DRV_VERSION	"0.1.0"

/*
 * Constants related to M[ABC]MR UPM operation.
 *
 * These might belong in fsl_upm.h.
 */
#define MxMR_OP			0x30000000	/* Operation mask */
#define MxMR_RLF_SHIFT		14		/* Read loop field */
#define MxMR_WLF_SHIFT		10		/* Write loop field */

/*
 * UPM programming constants.
 *
 * Some of these also probably belong in fsl_upm.h (aliased to more suitable
 * names, like those found in the MPC83xx documentation).
 */
#define INST_N_BASE		0x00f00000 /* G0L, LGPL0 negated, first half
					      phase */
#define INST_N_CS		0xf0000000 /* Chip-select timing (LCSn) mask */
#define INST_N_CS_H1		0xc0000000 /* CST1/2, first half phase */
#define INST_N_CS_H2		0x30000000 /* CST3/4, second half phase */
#define INST_N_WE		0x0f000000 /* Byte-select timing (LBSn) mask */
#define INST_N_WE_H1		0x0c000000 /* BST1/2, first half phase */
#define INST_N_WE_H2		0x03000000 /* BST3/4, second half phase */
#define INST_N_OE		0x00030000 /* G2 (LGPL2) mask */
#define INST_N_OE_H1		0x00020000 /* G2T1, first half phase */
#define INST_N_OE_H2		0x00010000 /* G2T3, second half phase */
#define INST_WAEN		0x00001000 /* Enable LUPWAIT */
#define INST_REDO_2		0x00000100 /* REDO 2x */
#define INST_REDO_3		0x00000200 /* REDO 3x */
#define INST_REDO_4		0x00000300 /* REDO 4x */
#define INST_LOOP		0x00000080 /* First time LOOP is set starts,
					      next ends */
#define INST_NA			0x00000008 /* Next burst address */
#define INST_UTA		0x00000004 /* Transfer acknowledge */
#define INST_LAST		0x00000001 /* End of pattern */

#define INST_READ_BASE		(INST_N_BASE | INST_N_WE)
#define INST_WRITE_BASE		(INST_N_BASE | INST_N_OE)
#define INST_EMPTY		(INST_N_BASE | INST_N_CS | INST_N_OE | INST_N_WE | INST_LAST)

#define X_INST_TABLE_END	0
#define X_INST_ANOTHER_TIMING	1

#define OA_CPUIN_MIN		(1 << 0)
#define OA_CPUOUT_MAX		(1 << 1)
#define OD_CPUOUT_MIN		(1 << 2)
#define OA_CPUOUT_DELTA		(OD_CPUOUT_MIN | OA_CPUOUT_MAX)
#define OA_EXTDEL_MAX		(1 << 3)
#define OD_EXTDEL_MIN		(1 << 4)
#define OA_EXTDEL_DELTA		(OD_EXTDEL_MIN | OA_EXTDEL_MAX)
#define O_MIN_CYCLE_TIME	(1 << 5)
#define O_MINUS_PREV		(1 << 6)
#define O_HALF_CYCLE		(1 << 7)

#define REDOS(mult)		(INST_REDO_2 * ((mult) - 1))
#define REDO_MAX_MULT		4

#define LOOPS			4

/*
 * This is extremely convoluted code that does some sort of alignment with what
 * appears to be arbitrary memory offsets. It used to be part of rb_iomap.c, but
 * it was only used for ATA operations so it's been migrated here instead (where
 * it might actually make some small amount of sense).
 */
#define REG_OFFSET(base, reg) ((base) + (((reg) << 16) | ((((reg) ^ 8) & 8) << 17)))

/*
 * Since multiple ATA hosts use the same UPM, we need to make sure we only
 * program a UPM to operate in a higher PIO mode when all hosts registered on
 * that UPM are ready to use that mode. Otherwise, we have to pick the lowest
 * mode that all of them support and generate timings from there.
 */
struct pata_rbppc_upm_pio_status {
	int configured_mode;
	int actual_mode;
	struct pata_rbppc_upm_prv *prv;

	struct pata_rbppc_upm_pio_status *next;
};

static DEFINE_MUTEX(pio_status_mutex);

static struct pata_rbppc_upm_pio_status *pio_statuses = NULL;

/*
 * These represent custom additional board-specific timings specified in the
 * device tree.
 */
struct pata_rbppc_upm_localbus_timing {
	u32 cpuin_min;
	u32 cpuout_min;
	u32 cpuout_max;
	u32 extdel_min;
	u32 extdel_max;
};

struct pata_rbppc_upm_prv {
	struct fsl_upm upm;
	u32 timing;
	struct pata_rbppc_upm_localbus_timing localbus_timings;
	int irq;
	struct pata_rbppc_upm_pio_status pio_status;

	struct fsl_lbc_ctrl *ctrl;
	struct ata_host *host;

	struct device *dev;
};

#define UPM_P_RSS	0x00 /* Read single-beat */
#define UPM_P_RBS	0x08 /* Read burst */
#define UPM_P_WSS 	0x18 /* Write single-beat */
#define UPM_P_WBS	0x20 /* Write burst */
#define UPM_P_RTS	0x30 /* Refresh timer */
#define UPM_P_EXS	0x3c /* Exception condition */
#define UPM_P_SIZE	0x40 /* UPM program RAM is 64 32-bit words */

struct pata_rbppc_upm_program {
	u32 program[UPM_P_SIZE];
	void __iomem *io_addr;
};

struct pata_rbppc_upm_cfg {
	unsigned value;
	unsigned timings[7]; /* PIO modes 0 - 6, in nanoseconds */
	unsigned clk_minus;
	unsigned group_size;
	unsigned options;
};

static const struct pata_rbppc_upm_cfg pata_rbppc_upm_read_table[] = {
	{ INST_READ_BASE | INST_N_OE,
	  /* t1 - ADDR setup time */
		{  70,  50,  30,  30,  25,  15,  10 }, 0, 0, (OA_CPUOUT_DELTA |
							      OA_EXTDEL_MAX) },
	{ INST_READ_BASE | INST_N_OE_H1,
		{   0,   0,   0,   0,   0,   0,   0 }, 0, 0, O_HALF_CYCLE },
	{ INST_READ_BASE,
	  /* t2 - OE0 time */
		{ 290, 290, 290,  80,  70,  65,  55 }, 0, 2, (OA_CPUOUT_MAX |
							      OA_CPUIN_MIN) },
	{ INST_READ_BASE | INST_WAEN,
		{   1,   1,   1,   1,   1,   0,   0 }, 0, 0, 0 },
	{ INST_READ_BASE | INST_UTA,
		{   1,   1,   1,   1,   1,   1,   1 }, 0, 0, 0 },
	{ INST_READ_BASE | INST_N_OE,
	  /* t9 - ADDR hold time */
		{  20,  15,  10,  10,  10,  10,  10 }, 0, 0, (OA_CPUOUT_DELTA |
							      OD_EXTDEL_MIN) },
	{ INST_READ_BASE | INST_N_OE | INST_N_CS_H2,
		{   0,   0,   0,   0,   0,   0,   0 }, 0, 0, O_HALF_CYCLE },
	{ INST_READ_BASE | INST_N_OE | INST_N_CS,
	  /* t6Z -IORD data tristate */
		{  30,  30,  30,  30,  30,  20,  20 }, 1, 1, O_MINUS_PREV },
	{ X_INST_ANOTHER_TIMING,
	  /* t2i -IORD recovery time */
		{   0,   0,   0,  70,  25,  25,  20 }, 2, 0, 0 },
	{ X_INST_ANOTHER_TIMING,
	  /* CS 0 -> 1 MAX */
		{   0,   0,   0,   0,   0,   0,   0 }, 1, 0, (OA_CPUOUT_DELTA |
							      OA_EXTDEL_MAX) },
	{ INST_READ_BASE | INST_N_OE | INST_N_CS | INST_LAST,
		{   1,   1,   1,   1,   1,   1,   1 }, 0, 0, 0 },
	{ X_INST_TABLE_END,
	  /* min total cycle time - includes turnaround and ALE cycle */
		{ 600, 383, 240, 180, 120, 100,  80 }, 2, 0, O_MIN_CYCLE_TIME },
};

static const struct pata_rbppc_upm_cfg pata_rbppc_upm_write_table[] = {
	{ INST_WRITE_BASE | INST_N_WE,
	  /* t1 - ADDR setup time */
		{  70,  50,  30,  30,  25,  15,  10 }, 0, 0, (OA_CPUOUT_DELTA |
							      OA_EXTDEL_MAX) },
	{ INST_WRITE_BASE | INST_N_WE_H1,
		{   0,   0,   0,   0,   0,   0,   0 }, 0, 0, O_HALF_CYCLE },
	{ INST_WRITE_BASE,
	  /* t2 - WE0 time */
		{ 290, 290, 290,  80,  70,  65,  55 }, 0, 1, OA_CPUOUT_DELTA },
	{ INST_WRITE_BASE | INST_WAEN,
		{   1,   1,   1,   1,   1,   0,   0 }, 0, 0, 0 },
	{ INST_WRITE_BASE | INST_N_WE,
	  /* t9 - ADDR hold time */
		{  20,  15,  10,  10,  10,  10,  10 }, 0, 0, (OA_CPUOUT_DELTA |
							      OD_EXTDEL_MIN) },
	{ INST_WRITE_BASE | INST_N_WE | INST_N_CS_H2,
		{   0,   0,   0,   0,   0,   0,   0 }, 0, 0, O_HALF_CYCLE },
	{ INST_WRITE_BASE | INST_N_WE | INST_N_CS,
	  /* t4 - DATA hold time */
		{  30,  20,  15,  10,  10,  10,  10 }, 0, 1, O_MINUS_PREV },
	{ X_INST_ANOTHER_TIMING,
	  /* t2i -IOWR recovery time */
		{   0,   0,   0,  70,  25,  25,  20 }, 1, 0, 0 },
	{ X_INST_ANOTHER_TIMING,
	  /* CS 0 -> 1 MAX */
		{   0,   0,   0,   0,   0,   0,   0 }, 0, 0, (OA_CPUOUT_DELTA |
							      OA_EXTDEL_MAX) },
	{ INST_WRITE_BASE | INST_N_WE | INST_N_CS | INST_UTA | INST_LAST,
		{   1,   1,   1,   1,   1,   1,   1 }, 0, 0, 0 },
	/* min total cycle time - includes ALE cycle */
	{ X_INST_TABLE_END,
		{ 600, 383, 240, 180, 120, 100,  80 }, 1, 0, O_MIN_CYCLE_TIME },
};

struct __upm_timing {
	int clk, ps;
	const struct pata_rbppc_upm_cfg *cfg;
};

static int __ps_to_clk(int ps, u32 bus_timing) {
	int ps_over;
	if (ps <= 0)
		return 0;

	/* Round down if we're less than 2% over clk border, but no more than
	 * 1/4 clk cycle. */
	ps_over = ps * 2 / 100;
	if (4 * ps_over > bus_timing)
		ps_over = bus_timing / 4;

	return (ps + bus_timing - 1 - ps_over) / bus_timing;
}

static void __upm_table_populate_times(struct __upm_timing *timings, int mode,
				       u32 bus_timing,
				       struct pata_rbppc_upm_localbus_timing *localbus_timings)
{
	int i = 0, group_i = 0;
	struct __upm_timing *last = NULL, *group = NULL;

	do {
		const struct pata_rbppc_upm_cfg *cfg = timings[i].cfg;

		int ps = cfg->timings[mode] * 1000
			- cfg->clk_minus * bus_timing;

		if (cfg->options & OA_CPUIN_MIN)
			ps += localbus_timings->cpuin_min;
		if (cfg->options & OD_CPUOUT_MIN)
			ps -= localbus_timings->cpuout_min;
		if (cfg->options & OA_CPUOUT_MAX)
			ps += localbus_timings->cpuout_max;
		if (cfg->options & OD_EXTDEL_MIN)
			ps -= localbus_timings->extdel_min;
		if (cfg->options & OA_EXTDEL_MAX)
			ps += localbus_timings->extdel_max;

		if (last && cfg->value == X_INST_ANOTHER_TIMING) {
			if (last->ps < ps)
				last->ps = ps;

			timings[i].ps = 0;
		} else {
			if (cfg->group_size) {
				group = &timings[i];
				group_i = cfg->group_size;
			} else if (group && group_i > 0) {
				int clk = __ps_to_clk(ps, bus_timing);
				group->ps -= clk * bus_timing;
				group_i--;
			}

			if (cfg->options & O_MINUS_PREV) {
				int clk = __ps_to_clk(last->ps, bus_timing);
				ps -= clk * bus_timing;
			}

			timings[i].ps = ps;
			last = &timings[i];
		}
	} while (timings[i++].cfg->value != X_INST_TABLE_END);
}

static inline int __free_half(struct __upm_timing *timing, u32 bus_timing)
{
	return timing->clk < 2
		? 0
		: (timing->clk * bus_timing - timing->ps) * 2 >= bus_timing;
}

static void __upm_table_populate_clks(struct __upm_timing *timings,
				      u32 bus_timing)
{
	int i;
	int clk_total = 0;

	/*
	 * Convert picoseconds determined from table/local bus timings to actual
	 * clock cycles.
	 */
	for (i = 0; timings[i].cfg->value != X_INST_TABLE_END; i++) {
		timings[i].clk = __ps_to_clk(timings[i].ps, bus_timing);
		clk_total += timings[i].clk;
	}

	/*
	 * Check whether we have free half cycles surrounding an operation.
	 *
	 * We need at least three operations in the table for this to make
	 * sense.
	 */
	if (i >= 2) {
		int j;

		for (j = 1; timings[j + 1].cfg->value != X_INST_TABLE_END;
		     j++) {
			if (timings[j].cfg->options & O_HALF_CYCLE &&
			    __free_half(&timings[j - 1], bus_timing) &&
			    __free_half(&timings[j + 1], bus_timing)) {
				timings[j].clk++;
				timings[j - 1].clk--;
				timings[j + 1].clk--;
			}
		}
	}

	/*
	 * Finally see if we need to adjust any timings to meet the minimum
	 * requirements for standards.
	 */
	if (timings[i].cfg->options & O_MIN_CYCLE_TIME) {
		int j = 0;

		timings[i].clk = __ps_to_clk(timings[i].ps, bus_timing);

		while (clk_total < timings[i].clk) {
			if (timings[j].cfg->value == X_INST_TABLE_END)
				j = 0;

			if (timings[j].clk > 0) {
				timings[j].clk++;
				clk_total++;
			}

			j++;
		}
	}
}

static void __upm_table_populate_value(u32 value, int *clk, u32 **program)
{
	if (*clk == 0)
		/* Nothing to do. */;
	else if (*clk >= LOOPS * 2) {
		int times, times_r1, times_r2;

		times = *clk / LOOPS;
		if (times > REDO_MAX_MULT * 2)
			times = REDO_MAX_MULT * 2;

		times_r1 = times / 2;
		times_r2 = times - times_r1;

		value |= INST_LOOP;
		**program = value | REDOS(times_r1);
		(*program)++;
		**program = value | REDOS(times_r2);
		(*program)++;

		*clk -= times * LOOPS;
	} else {
		int clk_for_value = *clk < REDO_MAX_MULT ? *clk : REDO_MAX_MULT;

		value |= REDOS(clk_for_value);
		*clk -= clk_for_value;

		**program = value;
		(*program)++;
	}
}

static void __upm_table_populate_values(struct __upm_timing *timings,
					struct pata_rbppc_upm_program *program,
					int offset)
{
	int i;

	u32 *wr = &program->program[offset];
	for (i = 0; timings[i].cfg->value != X_INST_TABLE_END; i++) {
		int clk = timings[i].clk;
		while (clk > 0)
			__upm_table_populate_value(timings[i].cfg->value, &clk,
						   &wr);
	}
}

static int __upm_table_to_program(struct pata_rbppc_upm_prv *prv,
				  struct __upm_timing *timings, int mode,
				  struct pata_rbppc_upm_program *program,
				  int offset)
{
	__upm_table_populate_times(timings, mode, prv->timing,
				   &prv->localbus_timings);
	__upm_table_populate_clks(timings, prv->timing);
	__upm_table_populate_values(timings, program, offset);

	return 0;
}

static int pata_rbppc_upm_get_program(struct pata_rbppc_upm_prv *prv, int mode,
				      struct pata_rbppc_upm_program *program)
{
	int i;
	int retval;
	struct __upm_timing read_timings[ARRAY_SIZE(pata_rbppc_upm_read_table)];
	struct __upm_timing write_timings[ARRAY_SIZE(pata_rbppc_upm_write_table)];

	/*
	 * Initialize program to empty values.
	 */
	for (i = 0; i < UPM_P_SIZE; i++) {
		program->program[i] = INST_EMPTY;
	}

	/*
	 * Initialize the timing data and map it to our table.
	 */
#define INITIALIZE_TIMINGS(timings, table)				\
	do {								\
		int i = 0;						\
		do {							\
			(timings)[i].clk = 0;				\
			(timings)[i].ps = 0;				\
			(timings)[i].cfg = &(table)[i];			\
		} while ((table)[i++].value != X_INST_TABLE_END);	\
	} while(0)
	INITIALIZE_TIMINGS(read_timings, pata_rbppc_upm_read_table);
	INITIALIZE_TIMINGS(write_timings, pata_rbppc_upm_write_table);

	/*
	 * Build read/write programs from our table structures.
	 */
	retval = __upm_table_to_program(prv, read_timings, mode, program,
				        UPM_P_RSS);
	if (retval) {
		dev_err(prv->dev, "Could not generate read program for PIO "
			"mode %u\n", mode);
		return retval;
	}

	retval = __upm_table_to_program(prv, write_timings, mode, program,
					UPM_P_WSS);
	if (retval) {
		dev_err(prv->dev, "Could not generate write program for PIO "
			"mode %u\n", mode);
	}

	return 0;
}

static void pata_rbppc_upm_program(struct pata_rbppc_upm_prv *prv,
				   struct pata_rbppc_upm_program *program)
{
	u32 i;

	clrsetbits_be32(prv->upm.mxmr, MxMR_MAD, MxMR_OP_WA);
	in_be32(prv->upm.mxmr);

	for (i = 0; i < UPM_P_SIZE; i++) {
		out_be32(&prv->ctrl->regs->mdr, program->program[i]);
		in_be32(&prv->ctrl->regs->mdr);

		out_8(program->io_addr, 0x0);

		while ((in_be32(prv->upm.mxmr) ^ (i + 1)) & MxMR_MAD)
			cpu_relax();
	}

	clrsetbits_be32(prv->upm.mxmr, MxMR_MAD | MxMR_OP,
			MxMR_OP_NO | (LOOPS << MxMR_RLF_SHIFT) |
			(LOOPS << MxMR_WLF_SHIFT));
	in_be32(prv->upm.mxmr);
}

static int pata_rbppc_upm_program_for_piomode(struct pata_rbppc_upm_prv *prv,
					      int mode)
{
	struct pata_rbppc_upm_program program;
	int retval;

	retval = pata_rbppc_upm_get_program(prv, mode, &program);
	if (retval)
		return retval;

	program.io_addr = prv->host->ports[0]->ioaddr.cmd_addr;
	pata_rbppc_upm_program(prv, &program);

	return 0;
}

static void pata_rbppc_upm_set_piomode(struct ata_port *ap, struct ata_device *adev)
{
	struct pata_rbppc_upm_prv *prv = ap->host->private_data;
	struct pata_rbppc_upm_pio_status *pio_status;
	int requested_mode = adev->pio_mode - XFER_PIO_0;
	int actual_mode = requested_mode;
	int retval;

	if (requested_mode < 0 || requested_mode > 6) {
		dev_err(prv->dev, "Illegal PIO mode %u\n", requested_mode);
		return;
	}

	prv->pio_status.configured_mode = requested_mode;

	mutex_lock(&pio_status_mutex);

	/*
	 * Find other hosts that are on the same UPM as this one, and make sure
	 * they're all configured for the PIO mode we want.
	 */
	for (pio_status = pio_statuses; pio_status != NULL;
	     pio_status = pio_status->next) {
		if (pio_status->prv == prv)
			continue;
		else if (pio_status->prv->upm.mxmr == prv->upm.mxmr &&
			 pio_status->configured_mode < actual_mode)
			actual_mode = pio_status->configured_mode;
	}

	if (actual_mode < 0) {
		dev_info(prv->dev, "Waiting until another device comes up to "
			 "program UPM for new PIO mode\n");
		goto out;
	} else if (actual_mode < requested_mode) {
		dev_info(prv->dev, "Requested PIO mode %u, but UPM can only be "
			 "configured at PIO mode %u\n", requested_mode,
			 actual_mode);
	}

	retval = pata_rbppc_upm_program_for_piomode(prv, actual_mode);
	if (retval) {
		dev_err(prv->dev, "Could not update PIO mode: %d\n", retval);
		goto out;
	}

	/*
	 * Now update everything on the UPM to have the new actual mode.
	 */
	for (pio_status = pio_statuses; pio_status != NULL;
	     pio_status = pio_status->next) {
		if (pio_status->prv->upm.mxmr == prv->upm.mxmr) {
			pio_status->actual_mode = actual_mode;
			dev_info(pio_status->prv->dev,
				 "PIO mode changed to %u\n", actual_mode);
		}
	}

out:
	mutex_unlock(&pio_status_mutex);
}

static u8 pata_rbppc_upm_check_status(struct ata_port *ap) {
	u8 val = ioread8(ap->ioaddr.status_addr);
	if (val == 0xF9)
		val = 0x7F;
	return val;
}

static u8 pata_rbppc_upm_check_altstatus(struct ata_port *ap) {
	u8 val = ioread8(ap->ioaddr.altstatus_addr);
	if (val == 0xF9)
		val = 0x7F;
	return val;
}

static irqreturn_t pata_rbppc_upm_interrupt(int irq, void *dev_instance)
{
	irqreturn_t retval = ata_sff_interrupt(irq, dev_instance);
	if (retval == IRQ_RETVAL(0)) {
		struct ata_host *host = dev_instance;
		struct ata_port *ap = host->ports[0];

		/* Clear interrupt. */
		ap->ops->sff_check_status(ap);

		ata_port_printk(ap, KERN_WARNING, "IRQ %d not handled\n", irq);
	}

	return retval;
}

static struct scsi_host_template pata_rbppc_upm_sht = {
	ATA_BASE_SHT(DRV_NAME),
	.dma_boundary		= ATA_DMA_BOUNDARY,
};

static struct ata_port_operations pata_rbppc_upm_port_ops = {
	.inherits		= &ata_sff_port_ops,

	.set_piomode		= pata_rbppc_upm_set_piomode,

	.sff_check_status	= pata_rbppc_upm_check_status,
	.sff_check_altstatus	= pata_rbppc_upm_check_altstatus,
};

static int pata_rbppc_upm_probe_timings(struct pata_rbppc_upm_prv *prv)
{
	struct device *dev = prv->dev;
	struct device_node *dn_soc;
	const u32 *prop;
	int prop_size;
	u32 bus_frequency, lcrr_clkdiv;
	int retval = 0;

	dn_soc = of_find_node_by_type(NULL, "soc");
	if (!dn_soc) {
		dev_err(dev, "Could not find SoC node\n");
		return -EINVAL;
	}

	prop = of_get_property(dn_soc, "bus-frequency", NULL);
	if (!prop || !*prop) {
		dev_err(dev, "Could not determine bus frequency\n");
		retval = -EINVAL;
		goto out;
	}

	bus_frequency = *prop;

	/*
	 * The actual speed is determined by the ratio between the bus frequency
	 * and the CLKDIV register.
	 */
	lcrr_clkdiv = (in_be32(&prv->ctrl->regs->lcrr) & LCRR_CLKDIV)
		>> LCRR_CLKDIV_SHIFT;
	bus_frequency /= lcrr_clkdiv;

	/* (picoseconds / kHz) */
	prv->timing = 1000000000 / (bus_frequency / 1000);

	/*
	 * Additional timings are set up in the device node itself, also in
	 * picoseconds.
	 */
	prop = of_get_property(dev->of_node, "rb,pata-upm-localbus-timings",
			       &prop_size);
	if (prop && prop_size == 5 * sizeof(u32)) {
		prv->localbus_timings.cpuin_min = prop[0];
		prv->localbus_timings.cpuout_min = prop[1];
		prv->localbus_timings.cpuout_max = prop[2];
		prv->localbus_timings.extdel_min = prop[3];
		prv->localbus_timings.extdel_max = prop[4];
	}

out:
	of_node_put(dn_soc);
	return retval;
}

static int pata_rbppc_upm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pata_rbppc_upm_prv *prv;
	struct ata_host *host;
	struct ata_port *ap;
	struct ata_ioports *aio;
	struct device_node *dn = dev->of_node;
	struct resource res;
	void __iomem *io_addr;
	int retval;

	printk(KERN_INFO "MikroTik RouterBOARD UPM PATA driver for "
	       "MPC83xx/MPC85xx-based platforms, version " DRV_VERSION "\n");

	if (!fsl_lbc_ctrl_dev || !fsl_lbc_ctrl_dev->regs)
		return -ENODEV;

	prv = devm_kzalloc(dev, sizeof(*prv), GFP_KERNEL);
	if (!prv) {
		dev_err(dev, "Can't allocate memory!\n");
		return -ENOMEM;
	}

	prv->ctrl = fsl_lbc_ctrl_dev;
	prv->dev = dev;

	retval = pata_rbppc_upm_probe_timings(prv);
	if (retval) {
		dev_err(dev, "Could not initialize timing data from SoC\n");
		return retval;
	}

	retval = of_address_to_resource(dn, 0, &res);
	if (retval) {
		dev_err(dev, "No reg property found\n");
		return retval;
	}

	retval = fsl_upm_find(res.start, &prv->upm);
	if (retval) {
		dev_err(dev, "Could not find UPM\n");
		return retval;
	}

	if (!devm_request_mem_region(dev, res.start, res.end - res.start + 1,
				     DRV_NAME)) {
		dev_err(dev, "Could not request region\n");
		return -EBUSY;
	}

	io_addr = devm_ioremap(dev, res.start, res.end - res.start + 1);
	if (!io_addr) {
		dev_err(dev, "Could not map IO region\n");
		return -ENOMEM;
	}

	host = ata_host_alloc(dev, 1);
	if (!host) {
		dev_err(dev, "Can't allocate memory!\n");
		return -ENOMEM;
	}

	host->private_data = prv;

	ap = host->ports[0];
	ap->ops = &pata_rbppc_upm_port_ops;
	ap->pio_mask = ATA_PIO6;
	ap->udma_mask = 0;
	ap->mwdma_mask = 0;

	/*
	 * This is sort of halfheartedly based on the extremely strange logic in
	 * rb_iomap.c. I think setting these to the values they eventually get
	 * mapped to (look at localbus_regoff() if you're curious) should
	 * eliminate the need for RouterBOARD-specific iomapping.
	 */
	aio = &ap->ioaddr;
	aio->cmd_addr = REG_OFFSET(io_addr, 0);
	aio->data_addr = REG_OFFSET(io_addr, ATA_REG_DATA);
	aio->error_addr = REG_OFFSET(io_addr, ATA_REG_ERR);
	aio->feature_addr = REG_OFFSET(io_addr, ATA_REG_FEATURE);
	aio->nsect_addr = REG_OFFSET(io_addr, ATA_REG_NSECT);
	aio->lbal_addr = REG_OFFSET(io_addr, ATA_REG_LBAL);
	aio->lbam_addr = REG_OFFSET(io_addr, ATA_REG_LBAM);
	aio->lbah_addr = REG_OFFSET(io_addr, ATA_REG_LBAH);
	aio->device_addr = REG_OFFSET(io_addr, ATA_REG_DEVICE);
	aio->status_addr = REG_OFFSET(io_addr, ATA_REG_STATUS);
	aio->command_addr = REG_OFFSET(io_addr, ATA_REG_CMD);
	aio->ctl_addr = REG_OFFSET(io_addr, 14);
	aio->altstatus_addr = aio->ctl_addr;

	prv->irq = irq_of_parse_and_map(dev->of_node, 0);
	if (prv->irq == NO_IRQ) {
		dev_err(dev, "Could not acquire IRQ\n");
		return -EINVAL;
	}

	retval = ata_host_activate(host, prv->irq, pata_rbppc_upm_interrupt,
				   IRQF_TRIGGER_LOW, &pata_rbppc_upm_sht);
	if (retval) {
		irq_dispose_mapping(prv->irq);
		dev_err(dev, "Could not activate ATA host\n");
		return retval;
	}

	prv->host = host;

	/*
	 * Set up the PIO mode tracking mechanism.
	 */
	prv->pio_status.configured_mode = -1;
	prv->pio_status.actual_mode = -1;
	prv->pio_status.prv = prv;

	mutex_lock(&pio_status_mutex);

	prv->pio_status.next = pio_statuses;
	pio_statuses = &prv->pio_status;

	mutex_unlock(&pio_status_mutex);

	return 0;
}

static int pata_rbppc_upm_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ata_host *host = dev_get_drvdata(dev);
	struct pata_rbppc_upm_prv *prv = host->private_data;
	struct pata_rbppc_upm_pio_status *pio_status;

	/*
	 * Remove PIO mode tracking.
	 */
	mutex_lock(&pio_status_mutex);

	if (!pio_statuses->next)
		pio_statuses = NULL;
	else {
		for (pio_status = pio_statuses; pio_status != NULL;
		     pio_status = pio_status->next) {
			if (pio_status->next && pio_status->next->prv == prv) {
				pio_status->next = pio_status->next->next;
				break;
			}
		}
	}

	mutex_unlock(&pio_status_mutex);

	/*
	 * And clean up all the things we allocated. ALL THE THINGS.
	 */
	ata_host_detach(host);
	irq_dispose_mapping(prv->irq);

	return 0;
}

static struct of_device_id pata_rbppc_upm_ids[] = {
	{ .compatible = "rb,pata-upm", },
	{ },
};

static struct platform_driver pata_rbppc_upm_driver = {
	.probe = pata_rbppc_upm_probe,
	.remove = pata_rbppc_upm_remove,
	.driver	= {
		.name = "rbppc-upm",
		.owner = THIS_MODULE,
		.of_match_table = pata_rbppc_upm_ids,
	},
};

static int __init pata_rbppc_upm_init(void)
{
	return platform_driver_register(&pata_rbppc_upm_driver);
}

static void __exit pata_rbppc_upm_exit(void)
{
	platform_driver_unregister(&pata_rbppc_upm_driver);
}

MODULE_AUTHOR("Mikrotikls SIA");
MODULE_AUTHOR("Noah Fontes");
MODULE_DESCRIPTION("MikroTik RouterBOARD UPM PATA driver for MPC83xx/MPC85xx-based platforms");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

module_init(pata_rbppc_upm_init);
module_exit(pata_rbppc_upm_exit);
