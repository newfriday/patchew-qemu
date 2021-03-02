/*
 * QEMU model of the Xilinx XRAM Controller.
 *
 * Copyright (c) 2021 Xilinx Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Written by Edgar E. Iglesias <edgar.iglesias@xilinx.com>
 */

#ifndef XLNX_VERSAL_XRAMC_H
#define XLNX_VERSAL_XRAMC_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/register.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/irq.h"

#define TYPE_XLNX_XRAM_CTRL "xlnx.versal-xramc"

#define XLNX_XRAM_CTRL(obj) \
     OBJECT_CHECK(XlnxXramCtrl, (obj), TYPE_XLNX_XRAM_CTRL)

REG32(XRAM_ERR_CTRL, 0x0)
    FIELD(XRAM_ERR_CTRL, UE_RES, 3, 1)
    FIELD(XRAM_ERR_CTRL, PWR_ERR_RES, 2, 1)
    FIELD(XRAM_ERR_CTRL, PZ_ERR_RES, 1, 1)
    FIELD(XRAM_ERR_CTRL, APB_ERR_RES, 0, 1)
REG32(XRAM_ISR, 0x4)
    FIELD(XRAM_ISR, INV_APB, 0, 1)
REG32(XRAM_IMR, 0x8)
    FIELD(XRAM_IMR, INV_APB, 0, 1)
REG32(XRAM_IEN, 0xc)
    FIELD(XRAM_IEN, INV_APB, 0, 1)
REG32(XRAM_IDS, 0x10)
    FIELD(XRAM_IDS, INV_APB, 0, 1)
REG32(XRAM_ECC_CNTL, 0x14)
    FIELD(XRAM_ECC_CNTL, FI_MODE, 2, 1)
    FIELD(XRAM_ECC_CNTL, DET_ONLY, 1, 1)
    FIELD(XRAM_ECC_CNTL, ECC_ON_OFF, 0, 1)
REG32(XRAM_CLR_EXE, 0x18)
    FIELD(XRAM_CLR_EXE, MON_7, 7, 1)
    FIELD(XRAM_CLR_EXE, MON_6, 6, 1)
    FIELD(XRAM_CLR_EXE, MON_5, 5, 1)
    FIELD(XRAM_CLR_EXE, MON_4, 4, 1)
    FIELD(XRAM_CLR_EXE, MON_3, 3, 1)
    FIELD(XRAM_CLR_EXE, MON_2, 2, 1)
    FIELD(XRAM_CLR_EXE, MON_1, 1, 1)
    FIELD(XRAM_CLR_EXE, MON_0, 0, 1)
REG32(XRAM_CE_FFA, 0x1c)
    FIELD(XRAM_CE_FFA, ADDR, 0, 20)
REG32(XRAM_CE_FFD0, 0x20)
REG32(XRAM_CE_FFD1, 0x24)
REG32(XRAM_CE_FFD2, 0x28)
REG32(XRAM_CE_FFD3, 0x2c)
REG32(XRAM_CE_FFE, 0x30)
    FIELD(XRAM_CE_FFE, SYNDROME, 0, 16)
REG32(XRAM_UE_FFA, 0x34)
    FIELD(XRAM_UE_FFA, ADDR, 0, 20)
REG32(XRAM_UE_FFD0, 0x38)
REG32(XRAM_UE_FFD1, 0x3c)
REG32(XRAM_UE_FFD2, 0x40)
REG32(XRAM_UE_FFD3, 0x44)
REG32(XRAM_UE_FFE, 0x48)
    FIELD(XRAM_UE_FFE, SYNDROME, 0, 16)
REG32(XRAM_FI_D0, 0x4c)
REG32(XRAM_FI_D1, 0x50)
REG32(XRAM_FI_D2, 0x54)
REG32(XRAM_FI_D3, 0x58)
REG32(XRAM_FI_SY, 0x5c)
    FIELD(XRAM_FI_SY, DATA, 0, 16)
REG32(XRAM_RMW_UE_FFA, 0x70)
    FIELD(XRAM_RMW_UE_FFA, ADDR, 0, 20)
REG32(XRAM_FI_CNTR, 0x74)
    FIELD(XRAM_FI_CNTR, COUNT, 0, 24)
REG32(XRAM_IMP, 0x80)
    FIELD(XRAM_IMP, SIZE, 0, 4)
REG32(XRAM_PRDY_DBG, 0x84)
    FIELD(XRAM_PRDY_DBG, ISLAND3, 12, 4)
    FIELD(XRAM_PRDY_DBG, ISLAND2, 8, 4)
    FIELD(XRAM_PRDY_DBG, ISLAND1, 4, 4)
    FIELD(XRAM_PRDY_DBG, ISLAND0, 0, 4)
REG32(XRAM_SAFETY_CHK, 0xff8)

#define XRAM_CTRL_R_MAX (R_XRAM_SAFETY_CHK + 1)

typedef struct XlnxXramCtrl {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion ram;
    qemu_irq irq;

    struct {
        uint64_t size;
        unsigned int encoded_size;
    } cfg;

    uint32_t regs[XRAM_CTRL_R_MAX];
    RegisterInfo regs_info[XRAM_CTRL_R_MAX];
} XlnxXramCtrl;
#endif
