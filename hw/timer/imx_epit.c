/*
 * IMX EPIT Timer
 *
 * Copyright (c) 2008 OK Labs
 * Copyright (c) 2011 NICTA Pty Ltd
 * Originally written by Hans Jiang
 * Updated by Peter Chubb
 * Updated by Jean-Christophe Dubois <jcd@tribudubois.net>
 *
 * This code is licensed under GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "hw/timer/imx_epit.h"
#include "migration/vmstate.h"
#include "hw/irq.h"
#include "hw/misc/imx_ccm.h"
#include "qemu/module.h"
#include "qemu/log.h"

#ifndef DEBUG_IMX_EPIT
#define DEBUG_IMX_EPIT 0
#endif

#define DPRINTF(fmt, args...) \
    do { \
        if (DEBUG_IMX_EPIT) { \
            fprintf(stderr, "[%s]%s: " fmt , TYPE_IMX_EPIT, \
                                             __func__, ##args); \
        } \
    } while (0)

static const char *imx_epit_reg_name(uint32_t reg)
{
    switch (reg) {
    case 0:
        return "CR";
    case 1:
        return "SR";
    case 2:
        return "LR";
    case 3:
        return "CMP";
    case 4:
        return "CNT";
    default:
        return "[?]";
    }
}

/*
 * Exact clock frequencies vary from board to board.
 * These are typical.
 */
static const IMXClk imx_epit_clocks[] =  {
    CLK_NONE,      /* 00 disabled */
    CLK_IPG,       /* 01 ipg_clk, ~532MHz */
    CLK_IPG_HIGH,  /* 10 ipg_clk_highfreq */
    CLK_32k,       /* 11 ipg_clk_32k -- ~32kHz */
};

/*
 * Update interrupt status
 */
static void imx_epit_update_int(IMXEPITState *s)
{
    if (s->sr && (s->cr & CR_OCIEN) && (s->cr & CR_EN)) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

/*
 * Must be called from within a ptimer_transaction_begin/commit block
 * for both s->timer_cmp and s->timer_reload.
 */
static uint32_t imx_epit_set_freq(IMXEPITState *s)
{
    uint32_t clksrc;
    uint32_t prescaler;
    uint32_t freq;

    clksrc = extract32(s->cr, CR_CLKSRC_SHIFT, 2);
    prescaler = 1 + extract32(s->cr, CR_PRESCALE_SHIFT, 12);

    freq = imx_ccm_get_clock_frequency(s->ccm,
                                imx_epit_clocks[clksrc]) / prescaler;

    DPRINTF("Setting ptimer frequency to %u\n", freq);

    if (freq) {
        ptimer_set_freq(s->timer_reload, freq);
        ptimer_set_freq(s->timer_cmp, freq);
    }
    return freq;
}

static void imx_epit_reset(DeviceState *dev)
{
    IMXEPITState *s = IMX_EPIT(dev);

    /*
     * Soft reset doesn't touch some bits; hard reset clears them
     */
    s->cr &= (CR_EN|CR_ENMOD|CR_STOPEN|CR_DOZEN|CR_WAITEN|CR_DBGEN);
    s->sr = 0;
    s->lr = EPIT_TIMER_MAX;
    s->cmp = 0;
    ptimer_transaction_begin(s->timer_cmp);
    ptimer_transaction_begin(s->timer_reload);
    /* stop both timers */
    ptimer_stop(s->timer_cmp);
    ptimer_stop(s->timer_reload);
    /* compute new frequency */
    uint32_t freq = imx_epit_set_freq(s);
    /* init both timers to EPIT_TIMER_MAX */
    ptimer_set_limit(s->timer_cmp, EPIT_TIMER_MAX, 1);
    ptimer_set_limit(s->timer_reload, EPIT_TIMER_MAX, 1);
    if (freq && (s->cr & CR_EN)) {
        /* if the timer is still enabled, restart it */
        ptimer_run(s->timer_reload, 0);
    }
    ptimer_transaction_commit(s->timer_cmp);
    ptimer_transaction_commit(s->timer_reload);
}

static uint64_t imx_epit_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXEPITState *s = IMX_EPIT(opaque);
    uint32_t reg_value = 0;

    switch (offset >> 2) {
    case 0: /* Control Register */
        reg_value = s->cr;
        break;

    case 1: /* Status Register */
        reg_value = s->sr;
        break;

    case 2: /* LR - ticks*/
        reg_value = s->lr;
        break;

    case 3: /* CMP */
        reg_value = s->cmp;
        break;

    case 4: /* CNT */
        reg_value = ptimer_get_count(s->timer_reload);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: Bad register at offset 0x%"
                      HWADDR_PRIx "\n", TYPE_IMX_EPIT, __func__, offset);
        break;
    }

    DPRINTF("(%s) = 0x%08x\n", imx_epit_reg_name(offset >> 2), reg_value);

    return reg_value;
}

/* Must be called from ptimer_transaction_begin/commit block for s->timer_cmp */
static void imx_epit_reload_compare_timer(IMXEPITState *s)
{
    if ((s->cr & (CR_EN | CR_OCIEN)) == (CR_EN | CR_OCIEN))  {
        /* if the compare feature is on and timers are running */
        uint32_t tmp = ptimer_get_count(s->timer_reload);
        uint64_t next;
        if (tmp > s->cmp) {
            /* It'll fire in this round of the timer */
            next = tmp - s->cmp;
        } else { /* catch it next time around */
            next = tmp - s->cmp + ((s->cr & CR_RLD) ? EPIT_TIMER_MAX : s->lr);
        }
        ptimer_set_count(s->timer_cmp, next);
    }
}

static void imx_epit_write_cr(IMXEPITState *s, uint32_t value)
{
    uint32_t freq = 0;
    uint32_t oldcr = s->cr;
    s->cr = value & 0x03ffffff;
    if (s->cr & CR_SWR) {
        /* handle the reset */
        imx_epit_reset(DEVICE(s));
        /*
         * TODO: could we 'break' here? following operations appear
         * to duplicate the work imx_epit_reset() already did.
         */
    }

    ptimer_transaction_begin(s->timer_cmp);
    ptimer_transaction_begin(s->timer_reload);

    if (!(s->cr & CR_SWR)) {
        freq = imx_epit_set_freq(s);
    }

    if (freq && (s->cr & CR_EN) && !(oldcr & CR_EN)) {
        if (s->cr & CR_ENMOD) {
            if (s->cr & CR_RLD) {
                ptimer_set_limit(s->timer_reload, s->lr, 1);
                ptimer_set_limit(s->timer_cmp, s->lr, 1);
            } else {
                ptimer_set_limit(s->timer_reload, EPIT_TIMER_MAX, 1);
                ptimer_set_limit(s->timer_cmp, EPIT_TIMER_MAX, 1);
            }
        }

        imx_epit_reload_compare_timer(s);
        ptimer_run(s->timer_reload, 0);
        if (s->cr & CR_OCIEN) {
            ptimer_run(s->timer_cmp, 0);
        } else {
            ptimer_stop(s->timer_cmp);
        }
    } else if (!(s->cr & CR_EN)) {
        /* stop both timers */
        ptimer_stop(s->timer_reload);
        ptimer_stop(s->timer_cmp);
    } else if (s->cr & CR_OCIEN) {
        if (!(oldcr & CR_OCIEN)) {
            imx_epit_reload_compare_timer(s);
            ptimer_run(s->timer_cmp, 0);
        }
    } else {
        ptimer_stop(s->timer_cmp);
    }

    ptimer_transaction_commit(s->timer_cmp);
    ptimer_transaction_commit(s->timer_reload);
}

static void imx_epit_write_sr(IMXEPITState *s, uint32_t value)
{
    /* writing 1 to OCIF clears the OCIF bit */
    if (value & 0x01) {
        s->sr = 0;
        imx_epit_update_int(s);
    }
}

static void imx_epit_write_lr(IMXEPITState *s, uint32_t value)
{
    s->lr = value;

    ptimer_transaction_begin(s->timer_cmp);
    ptimer_transaction_begin(s->timer_reload);
    if (s->cr & CR_RLD) {
        /* Also set the limit if the LRD bit is set */
        /* If IOVW bit is set then set the timer value */
        ptimer_set_limit(s->timer_reload, s->lr, s->cr & CR_IOVW);
        ptimer_set_limit(s->timer_cmp, s->lr, 0);
    } else if (s->cr & CR_IOVW) {
        /* If IOVW bit is set then set the timer value */
        ptimer_set_count(s->timer_reload, s->lr);
    }
    /*
     * Commit the change to s->timer_reload, so it can propagate. Otherwise
     * the timer interrupt may not fire properly. The commit must happen
     * before calling imx_epit_reload_compare_timer(), which reads
     * s->timer_reload internally again.
     */
    ptimer_transaction_commit(s->timer_reload);
    imx_epit_reload_compare_timer(s);
    ptimer_transaction_commit(s->timer_cmp);
}

static void imx_epit_write_cmp(IMXEPITState *s, uint32_t value)
{
    s->cmp = value;

    ptimer_transaction_begin(s->timer_cmp);
    imx_epit_reload_compare_timer(s);
    ptimer_transaction_commit(s->timer_cmp);
}

static void imx_epit_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    IMXEPITState *s = IMX_EPIT(opaque);

    DPRINTF("(%s, value = 0x%08x)\n", imx_epit_reg_name(offset >> 2),
            (uint32_t)value);

    switch (offset >> 2) {
    case 0: /* CR */
        imx_epit_write_cr(s, (uint32_t)value);
        break;

    case 1: /* SR - ACK*/
        imx_epit_write_sr(s, (uint32_t)value);
        break;

    case 2: /* LR - set ticks */
        imx_epit_write_lr(s, (uint32_t)value);
        break;

    case 3: /* CMP */
        imx_epit_write_cmp(s, (uint32_t)value);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: Bad register at offset 0x%"
                      HWADDR_PRIx "\n", TYPE_IMX_EPIT, __func__, offset);

        break;
    }
}
static void imx_epit_cmp(void *opaque)
{
    IMXEPITState *s = IMX_EPIT(opaque);

    DPRINTF("sr was %d\n", s->sr);

    s->sr = 1;
    imx_epit_update_int(s);
}

static void imx_epit_reload(void *opaque)
{
    /* No action required on rollover of timer_reload */
}

static const MemoryRegionOps imx_epit_ops = {
    .read = imx_epit_read,
    .write = imx_epit_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_imx_timer_epit = {
    .name = TYPE_IMX_EPIT,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(cr, IMXEPITState),
        VMSTATE_UINT32(sr, IMXEPITState),
        VMSTATE_UINT32(lr, IMXEPITState),
        VMSTATE_UINT32(cmp, IMXEPITState),
        VMSTATE_PTIMER(timer_reload, IMXEPITState),
        VMSTATE_PTIMER(timer_cmp, IMXEPITState),
        VMSTATE_END_OF_LIST()
    }
};

static void imx_epit_realize(DeviceState *dev, Error **errp)
{
    IMXEPITState *s = IMX_EPIT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    DPRINTF("\n");

    sysbus_init_irq(sbd, &s->irq);
    memory_region_init_io(&s->iomem, OBJECT(s), &imx_epit_ops, s, TYPE_IMX_EPIT,
                          0x00001000);
    sysbus_init_mmio(sbd, &s->iomem);

    /*
     * The reload timer keeps running when the peripheral is enabled. It is a
     * kind of wall clock that does not generate any interrupts. The callback
     * needs to be provided, but it does nothing as the ptimer already supports
     * all necessary reloading functionality.
     */
    s->timer_reload = ptimer_init(imx_epit_reload, s, PTIMER_POLICY_LEGACY);

    /*
     * The compare timer is running only when the peripheral configuration is
     * in a state that will generate compare interrupts.
     */
    s->timer_cmp = ptimer_init(imx_epit_cmp, s, PTIMER_POLICY_LEGACY);
}

static void imx_epit_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc  = DEVICE_CLASS(klass);

    dc->realize = imx_epit_realize;
    dc->reset = imx_epit_reset;
    dc->vmsd = &vmstate_imx_timer_epit;
    dc->desc = "i.MX periodic timer";
}

static const TypeInfo imx_epit_info = {
    .name = TYPE_IMX_EPIT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXEPITState),
    .class_init = imx_epit_class_init,
};

static void imx_epit_register_types(void)
{
    type_register_static(&imx_epit_info);
}

type_init(imx_epit_register_types)
