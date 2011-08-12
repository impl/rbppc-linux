/*
 * drivers/misc/ucc_tdm.h
 *
 * UCC Based Linux TDM Driver
 * This driver is designed to support UCC based TDM for PowerPC processors.
 * This driver can interface with SLIC device to run VOIP kind of
 * applications.
 *
 * Author: Ashish Kalra & Poonam Aggrwal
 *
 * Copyright (c) 2007 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef TDM_H
#define TDM_H

#define NUM_TS 8
#define ACTIVE_CH 8

/* SAMPLE_DEPTH is the sample depth is the number of frames before
 * an interrupt. Must be a multiple of 4
 */
#define SAMPLE_DEPTH 80

/* define the number of Rx interrupts to go by for initial stuttering */
#define STUTTER_INT_CNT 1

/* BMRx Field Descriptions to specify tstate and rstate in UCC parameter RAM*/
#define EN_BUS_SNOOPING 0x20
#define BE_BO 0x10

/* UPSMR Register for Transparent UCC controller Bit definitions*/
#define NBO 0x00000000 /* Normal Mode 1 bit of data per clock */

/* SI Mode register bit definitions */
#define NORMAL_OPERATION 0x0000
#define AUTO_ECHO 0x0400
#define INTERNAL_LB 0x0800
#define CONTROL_LB 0x0c00
#define SIMODE_CRT (0x8000 >> 9)
#define SIMODE_SL (0x8000 >> 10)
#define SIMODE_CE (0x8000 >> 11)
#define SIMODE_FE (0x8000 >> 12)
#define SIMODE_GM (0x8000 >> 13)
#define SIMODE_TFSD(val) (val)
#define SIMODE_RFSD(val) ((val) << 8)

#define SI_TDM_MODE_REGISTER_OFFSET 0

#define R_CM 0x02000000
#define T_CM 0x02000000

#define SET_RX_SI_RAM(n, val) \
 out_be16((u16 *)&qe_immr->sir.rx[(n)*2], (u16)(val))

#define SET_TX_SI_RAM(n, val) \
 out_be16((u16 *)&qe_immr->sir.tx[(n)*2], (u16)(val))

/* SI RAM entries */
#define SIR_LAST 0x0001
#define SIR_CNT(n) ((n) << 2)
#define SIR_BYTE 0x0002
#define SIR_BIT 0x0000
#define SIR_IDLE 0
#define SIR_UCC(uccx) (((uccx+9)) << 5)

/* BRGC Register Bit definitions */
#define BRGC_RESET (0x1<<17)
#define BRGC_EN (0x1<<16)
#define BRGC_EXTC_QE (0x00<<14)
#define BRGC_EXTC_CLK3 (0x01<<14)
#define BRGC_EXTC_CLK5 (0x01<<15)
#define BRGC_EXTC_CLK9 (0x01<<14)
#define BRGC_EXTC_CLK11 (0x01<<14)
#define BRGC_EXTC_CLK13 (0x01<<14)
#define BRGC_EXTC_CLK15 (0x01<<15)
#define BRGC_ATB (0x1<<13)
#define BRGC_DIV16 (0x1)

/* structure representing UCC transparent parameter RAM */
struct ucc_transparent_pram {
 __be16 riptr;
 __be16 tiptr;
 __be16 res0;
 __be16 mrblr;
 __be32 rstate;
 __be32 rbase;
 __be16 rbdstat;
 __be16 rbdlen;
 __be32 rdptr;
 __be32 tstate;
 __be32 tbase;
 __be16 tbdstat;
 __be16 tbdlen;
 __be32 tdptr;
 __be32 rbptr;
 __be32 tbptr;
 __be32 rcrc;
 __be32 res1;
 __be32 tcrc;
 __be32 res2;
 __be32 res3;
 __be32 c_mask;
 __be32 c_pres;
 __be16 disfc;
 __be16 crcec;
 __be32 res4[4];
 __be16 ts_tmp;
 __be16 tmp_mb;
};

#define UCC_TRANSPARENT_PRAM_SIZE 0x100

struct tdm_cfg {
 u8 com_pin; /* Common receive and transmit pins
 * 0 = separate pins
 * 1 = common pins
 */

 u8 fr_sync_level; /* SLx bit Frame Sync Polarity
 * 0 = L1R/TSYNC active logic "1"
 * 1 = L1R/TSYNC active logic "0"
 */

 u8 clk_edge; /* CEx bit Tx Rx Clock Edge
 * 0 = TX data on rising edge of clock
 * RX data on falling edge
 * 1 = TX data on falling edge of clock
 * RX data on rising edge
 */

 u8 fr_sync_edge; /* FEx bit Frame sync edge
 * Determine when the sync pulses are sampled
 * 0 = Falling edge
 * 1 = Rising edge
 */

 u8 rx_fr_sync_delay; /* TFSDx/RFSDx bits Frame Sync Delay
 * 00 = no bit delay
 * 01 = 1 bit delay
 * 10 = 2 bit delay
 * 11 = 3 bit delay
 */

 u8 tx_fr_sync_delay; /* TFSDx/RFSDx bits Frame Sync Delay
 * 00 = no bit delay
 * 01 = 1 bit delay
 * 10 = 2 bit delay
 * 11 = 3 bit delay
 */

 u8 active_num_ts; /* Number of active time slots in TDM
 * assume same active Rx/Tx time slots
 */
};

struct ucc_tdm_info {
 struct ucc_fast_info uf_info;
 u32 ucc_busy;
};

struct tdm_ctrl {
 u32 device_busy;
 struct device *device;
 struct ucc_fast_private *uf_private;
 struct ucc_tdm_info *ut_info;
 u32 tdm_port; /* port for this tdm:TDMA,TDMB,TDMC,TDMD */
 u32 si; /* serial interface: 0 or 1 */
 struct ucc_fast __iomem *uf_regs; /* UCC Fast registers */
 u16 rx_mask[8]; /* Active Receive channels LSB is ch0 */
 u16 tx_mask[8]; /* Active Transmit channels LSB is ch0 */
 /* Only channels less than the number of FRAME_SIZE are implemented */
 struct tdm_cfg cfg_ctrl; /* Signaling controls configuration */
 u8 *tdm_input_data; /* buffer used for Rx by the tdm */
 u8 *tdm_output_data; /* buffer used for Tx by the tdm */

 dma_addr_t dma_input_addr; /* dma mapped buffer for TDM Rx */
 dma_addr_t dma_output_addr; /* dma mapped buffer for TDM Tx */
 u16 physical_num_ts; /* physical number of timeslots in the tdm
 frame */
 u32 phase_rx; /* cycles through 0, 1, 2 */
 u32 phase_tx; /* cycles through 0, 1, 2 */
 /*
 * the following two variables are for dealing with "stutter" problem
 * "stutter" period is about 20 frames or so, varies depending active
 * channel num depending on the sample depth, the code should let a
 * few Rx interrupts go by
 */
 u32 tdm_icnt;
 u32 tdm_flag;
 struct ucc_transparent_pram __iomem *ucc_pram;
 struct qe_bd __iomem *tx_bd;
 struct qe_bd __iomem *rx_bd;
 u32 ucc_pram_offset;
 u32 tx_bd_offset;
 u32 rx_bd_offset;
 u32 rx_ucode_buf_offset;
 u32 tx_ucode_buf_offset;
 bool leg_slic;
 wait_queue_head_t wakeup_event;
};

struct tdm_client {
 u32 client_id;
 void (*tdm_read)(u32 client_id, short chn_id,
 short *pcm_buffer, short len);
 void (*tdm_write)(u32 client_id, short chn_id,
 short *pcm_buffer, short len);
 wait_queue_head_t *wakeup_event;
 };

#define MAX_PHASE 1
#define NR_BUFS 2
#define EFF_ACTIVE_CH ACTIVE_CH / 2

#endif
