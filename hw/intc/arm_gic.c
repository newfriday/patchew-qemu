/*
 * ARM Generic/Distributed Interrupt Controller
 *
 * Copyright (c) 2006-2007 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 */

/* This file contains implementation code for the RealView EB interrupt
 * controller, MPCore distributed interrupt controller and ARMv7-M
 * Nested Vectored Interrupt Controller.
 * It is compiled in two ways:
 *  (1) as a standalone file to produce a sysbus device which is a GIC
 *  that can be used on the realview board and as one of the builtin
 *  private peripherals for the ARM MP CPUs (11MPCore, A9, etc)
 *  (2) by being directly #included into armv7m_nvic.c to produce the
 *  armv7m_nvic device.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "gic_internal.h"
#include "qapi/error.h"
#include "qom/cpu.h"
#include "qemu/log.h"
#include "trace.h"
#include "sysemu/kvm.h"

/* #define DEBUG_GIC */

#ifdef DEBUG_GIC
#define DEBUG_GIC_GATE 1
#else
#define DEBUG_GIC_GATE 0
#endif

#define DPRINTF(fmt, ...) do {                                          \
        if (DEBUG_GIC_GATE) {                                           \
            fprintf(stderr, "%s: " fmt, __func__, ## __VA_ARGS__);      \
        }                                                               \
    } while (0)

static const uint8_t gic_id_11mpcore[] = {
    0x00, 0x00, 0x00, 0x00, 0x90, 0x13, 0x04, 0x00, 0x0d, 0xf0, 0x05, 0xb1
};

static const uint8_t gic_id_gicv1[] = {
    0x04, 0x00, 0x00, 0x00, 0x90, 0xb3, 0x1b, 0x00, 0x0d, 0xf0, 0x05, 0xb1
};

static const uint8_t gic_id_gicv2[] = {
    0x04, 0x00, 0x00, 0x00, 0x90, 0xb4, 0x2b, 0x00, 0x0d, 0xf0, 0x05, 0xb1
};

static inline int gic_get_current_cpu(GICState *s)
{
    if (s->num_cpu > 1) {
        return current_cpu->cpu_index;
    }
    return 0;
}

static inline int gic_get_current_vcpu(GICState *s)
{
    return gic_get_current_cpu(s) + GIC_NCPU;
}

/* Return true if this GIC config has interrupt groups, which is
 * true if we're a GICv2, or a GICv1 with the security extensions.
 */
static inline bool gic_has_groups(GICState *s)
{
    return s->revision == 2 || s->security_extn;
}

static inline void gic_get_best_irq(GICState *s, int cpu,
                                    int *best_irq, int *best_prio, int *group)
{
    int irq;
    int cm = 1 << cpu;

    *best_irq = 1023;
    *best_prio = 0x100;

    for (irq = 0; irq < s->num_irq; irq++) {
        if (GIC_DIST_TEST_ENABLED(irq, cm) && gic_test_pending(s, irq, cm) &&
            (!GIC_DIST_TEST_ACTIVE(irq, cm)) &&
            (irq < GIC_INTERNAL || GIC_DIST_TARGET(irq) & cm)) {
            if (GIC_DIST_GET_PRIORITY(irq, cpu) < *best_prio) {
                *best_prio = GIC_DIST_GET_PRIORITY(irq, cpu);
                *best_irq = irq;
            }
        }
    }

    if (*best_irq < 1023) {
        *group = GIC_DIST_TEST_GROUP(*best_irq, cm);
    }
}

static inline void gic_get_best_virq(GICState *s, int cpu,
                                     int *best_irq, int *best_prio, int *group)
{
    int lr = 0;
    uint64_t lrs_in_use;

    /* In-use LRs have their corresponding bit cleared. */
    lrs_in_use = ~(s->h_elrsr[cpu]);

#if GIC_NR_LR < 64
    /* Ignore unimplemented LRs */
    lrs_in_use &= (1 << GIC_NR_LR) - 1;
#endif

    *best_irq = 1023;
    *best_prio = 0x100;

    while (lrs_in_use) {
        if (lrs_in_use & 0x1) {
            uint32_t lr_entry = s->h_lr[lr][cpu];
            int state = GICH_LR_STATE(lr_entry);

            if (state == GICH_LR_STATE_PENDING) {
                int prio = GICH_LR_PRIORITY(lr_entry);

                if (prio < *best_prio) {
                    *best_prio = prio;
                    *best_irq = GICH_LR_VIRT_ID(lr_entry);
                    *group = GICH_LR_GROUP(lr_entry);
                }
            }
        }

        lrs_in_use >>= 1;
        lr++;
    }
}

/* Return true if IRQ distribution is enabled:
 *      - in the distributor, for the given group mask if virt if false,
 *      - in the given CPU virtual interface if virt is true. */
static inline bool gic_dist_enabled(GICState *s, int cpu, bool virt,
                                    int group_mask)
{
    return (virt && (s->h_hcr[cpu] & GICH_HCR_EN))
        || (!virt && (s->ctlr & group_mask));
}

/* TODO: Many places that call this routine could be optimized.  */
/* Update interrupt status after enabled or pending bits have been changed.  */
static inline void gic_update_internal(GICState *s, bool virt)
{
    int best_irq;
    int best_prio;
    int irq_level, fiq_level;
    int cpu, cpu_iface;
    int group = 0;
    qemu_irq *irq_lines = virt ? s->parent_virq : s->parent_irq;
    qemu_irq *fiq_lines = virt ? s->parent_vfiq : s->parent_fiq;

    for (cpu = 0; cpu < s->num_cpu; cpu++) {
        cpu_iface = virt ? (cpu + GIC_NCPU) : cpu;

        s->current_pending[cpu_iface] = 1023;
        if (!gic_dist_enabled(s, cpu, virt,
                              GICD_CTLR_EN_GRP0 | GICD_CTLR_EN_GRP1)
            || !(s->cpu_ctlr[cpu_iface] &
                 (GICC_CTLR_EN_GRP0 | GICC_CTLR_EN_GRP1))) {
            qemu_irq_lower(irq_lines[cpu]);
            qemu_irq_lower(fiq_lines[cpu]);
            continue;
        }

        if (virt) {
            gic_get_best_virq(s, cpu, &best_irq, &best_prio, &group);
        } else {
            gic_get_best_irq(s, cpu, &best_irq, &best_prio, &group);
        }

        if (best_irq != 1023) {
            trace_gic_update_bestirq(virt ? "vcpu" : "cpu", cpu,
                                     best_irq, best_prio,
                                     s->priority_mask[cpu_iface],
                                     s->running_priority[cpu_iface]);
        }

        irq_level = fiq_level = 0;

        if (best_prio < s->priority_mask[cpu_iface]) {
            s->current_pending[cpu_iface] = best_irq;
            if (best_prio < s->running_priority[cpu_iface]) {
                if (gic_dist_enabled(s, cpu, virt, 1 << group) &&
                    extract32(s->cpu_ctlr[cpu_iface], group, 1)) {
                    if (group == 0 &&
                        s->cpu_ctlr[cpu_iface] & GICC_CTLR_FIQ_EN) {
                        DPRINTF("Raised pending FIQ %d (cpu %d)\n",
                                best_irq, cpu_iface);
                        fiq_level = 1;
                        trace_gic_update_set_irq(cpu, virt ? "vfiq" : "fiq",
                                                 fiq_level);
                    } else {
                        DPRINTF("Raised pending IRQ %d (cpu %d)\n",
                                best_irq, cpu_iface);
                        irq_level = 1;
                        trace_gic_update_set_irq(cpu, virt ? "virq" : "irq",
                                                 irq_level);
                    }
                }
            }
        }

        qemu_set_irq(irq_lines[cpu], irq_level);
        qemu_set_irq(fiq_lines[cpu], fiq_level);
    }
}

static void gic_compute_misr(GICState *s, int cpu)
{
    int val;
    int vcpu = cpu + GIC_NCPU;

    /* EOI */
    val = s->h_eisr[cpu] != 0;
    s->h_misr[cpu] = val << 0;

    /* U: true if only 0 or 1 LR entry is valid */
    val = s->h_hcr[cpu] & GICH_HCR_UIE &&
        ((GIC_NR_LR - ctpop64(s->h_elrsr[cpu])) < 2);
    s->h_misr[cpu] |= val << 1;

    /* LRENP: EOICount is not 0 */
    val = s->h_hcr[cpu] & GICH_HCR_LRENPIE &&
        ((s->h_hcr[cpu] & GICH_HCR_EOICOUNT) != 0);
    s->h_misr[cpu] |= val << 2;

    /* NP: no pending interrupts. The number of LRs in pending state is cached
     * in s->pending_lrs[cpu]. */
    val = s->h_hcr[cpu] & GICH_HCR_NPIE &&
        (s->pending_lrs[cpu] == 0);
    s->h_misr[cpu] |= val << 3;

    /* VGrp0E: group0 virq signaling enabled */
    val = s->h_hcr[cpu] & GICH_HCR_VGRP0EIE &&
        (s->cpu_ctlr[vcpu] & GICC_CTLR_EN_GRP0);
    s->h_misr[cpu] |= val << 4;

    /* VGrp0D: group0 virq signaling disabled */
    val = s->h_hcr[cpu] & GICH_HCR_VGRP0DIE &&
        !(s->cpu_ctlr[vcpu] & GICC_CTLR_EN_GRP0);
    s->h_misr[cpu] |= val << 5;

    /* VGrp1E: group1 virq signaling enabled */
    val = s->h_hcr[cpu] & GICH_HCR_VGRP1EIE &&
        (s->cpu_ctlr[vcpu] & GICC_CTLR_EN_GRP1);
    s->h_misr[cpu] |= val << 6;

    /* VGrp1D: group1 virq signaling disabled */
    val = s->h_hcr[cpu] & GICH_HCR_VGRP1DIE &&
        !(s->cpu_ctlr[vcpu] & GICC_CTLR_EN_GRP1);
    s->h_misr[cpu] |= val << 7;
}

static void gic_update_maintenance(GICState *s)
{
    int cpu = 0;
    int maint_level;

    for (cpu = 0; cpu < s->num_cpu; cpu++) {
        gic_compute_misr(s, cpu);
        maint_level = (s->h_hcr[cpu] & GICH_HCR_EN) && s->h_misr[cpu];

        trace_gic_update_maintenance_irq(cpu, maint_level);
        qemu_set_irq(s->maintenance_irq[cpu], maint_level);
    }
}

static void gic_update(GICState *s)
{
    gic_update_internal(s, false);
}

static void gic_update_virt(GICState *s)
{
    gic_update_internal(s, true);
    gic_update_maintenance(s);
}

static void gic_set_irq_11mpcore(GICState *s, int irq, int level,
                                 int cm, int target)
{
    if (level) {
        GIC_DIST_SET_LEVEL(irq, cm);
        if (GIC_DIST_TEST_EDGE_TRIGGER(irq) || GIC_DIST_TEST_ENABLED(irq, cm)) {
            DPRINTF("Set %d pending mask %x\n", irq, target);
            GIC_DIST_SET_PENDING(irq, target);
        }
    } else {
        GIC_DIST_CLEAR_LEVEL(irq, cm);
    }
}

static void gic_set_irq_generic(GICState *s, int irq, int level,
                                int cm, int target)
{
    if (level) {
        GIC_DIST_SET_LEVEL(irq, cm);
        DPRINTF("Set %d pending mask %x\n", irq, target);
        if (GIC_DIST_TEST_EDGE_TRIGGER(irq)) {
            GIC_DIST_SET_PENDING(irq, target);
        }
    } else {
        GIC_DIST_CLEAR_LEVEL(irq, cm);
    }
}

/* Process a change in an external IRQ input.  */
static void gic_set_irq(void *opaque, int irq, int level)
{
    /* Meaning of the 'irq' parameter:
     *  [0..N-1] : external interrupts
     *  [N..N+31] : PPI (internal) interrupts for CPU 0
     *  [N+32..N+63] : PPI (internal interrupts for CPU 1
     *  ...
     */
    GICState *s = (GICState *)opaque;
    int cm, target;
    if (irq < (s->num_irq - GIC_INTERNAL)) {
        /* The first external input line is internal interrupt 32.  */
        cm = ALL_CPU_MASK;
        irq += GIC_INTERNAL;
        target = GIC_DIST_TARGET(irq);
    } else {
        int cpu;
        irq -= (s->num_irq - GIC_INTERNAL);
        cpu = irq / GIC_INTERNAL;
        irq %= GIC_INTERNAL;
        cm = 1 << cpu;
        target = cm;
    }

    assert(irq >= GIC_NR_SGIS);

    if (level == GIC_DIST_TEST_LEVEL(irq, cm)) {
        return;
    }

    if (s->revision == REV_11MPCORE) {
        gic_set_irq_11mpcore(s, irq, level, cm, target);
    } else {
        gic_set_irq_generic(s, irq, level, cm, target);
    }
    trace_gic_set_irq(irq, level, cm, target);

    gic_update(s);
}

static uint16_t gic_get_current_pending_irq(GICState *s, int cpu,
                                            MemTxAttrs attrs)
{
    uint16_t pending_irq = s->current_pending[cpu];

    if (pending_irq < GIC_MAXIRQ && gic_has_groups(s)) {
        int group = gic_test_group(s, pending_irq, cpu);

        /* On a GIC without the security extensions, reading this register
         * behaves in the same way as a secure access to a GIC with them.
         */
        bool secure = !s->security_extn || attrs.secure;

        if (group == 0 && !secure) {
            /* Group0 interrupts hidden from Non-secure access */
            return 1023;
        }
        if (group == 1 && secure && !(s->cpu_ctlr[cpu] & GICC_CTLR_ACK_CTL)) {
            /* Group1 interrupts only seen by Secure access if
             * AckCtl bit set.
             */
            return 1022;
        }
    }
    return pending_irq;
}

static int gic_get_group_priority(GICState *s, int cpu, int irq)
{
    /* Return the group priority of the specified interrupt
     * (which is the top bits of its priority, with the number
     * of bits masked determined by the applicable binary point register).
     */
    int bpr;
    uint32_t mask;

    if (gic_has_groups(s) &&
        !(s->cpu_ctlr[cpu] & GICC_CTLR_CBPR) &&
        gic_test_group(s, irq, cpu)) {
        bpr = s->abpr[cpu] - 1;
        assert(bpr >= 0);
    } else {
        bpr = s->bpr[cpu];
    }

    /* a BPR of 0 means the group priority bits are [7:1];
     * a BPR of 1 means they are [7:2], and so on down to
     * a BPR of 7 meaning no group priority bits at all.
     */
    mask = ~0U << ((bpr & 7) + 1);

    return gic_get_priority(s, irq, cpu) & mask;
}

static void gic_activate_irq(GICState *s, int cpu, int irq)
{
    /* Set the appropriate Active Priority Register bit for this IRQ,
     * and update the running priority.
     */
    int prio = gic_get_group_priority(s, cpu, irq);
    int min_bpr = gic_is_vcpu(cpu) ? GIC_VIRT_MIN_BPR : GIC_MIN_BPR;
    int preemption_level = prio >> (min_bpr + 1);
    int regno = preemption_level / 32;
    int bitno = preemption_level % 32;
    uint32_t *papr = NULL;

    if (gic_is_vcpu(cpu)) {
        assert(regno == 0);
        papr = &s->h_apr[gic_get_vcpu_real_id(cpu)];
    } else if (gic_has_groups(s) && gic_test_group(s, irq, cpu)) {
        papr = &s->nsapr[regno][cpu];
    } else {
        papr = &s->apr[regno][cpu];
    }

    *papr |= (1 << bitno);

    s->running_priority[cpu] = prio;
    gic_set_active(s, irq, cpu);
}

static int gic_get_prio_from_apr_bits(GICState *s, int cpu)
{
    /* Recalculate the current running priority for this CPU based
     * on the set bits in the Active Priority Registers.
     */
    int i;
    int min_bpr = gic_is_vcpu(cpu) ? GIC_VIRT_MIN_BPR : GIC_MIN_BPR;
    int nr_aprs = gic_is_vcpu(cpu) ? GIC_VIRT_NR_APRS : GIC_NR_APRS;

    for (i = 0; i < nr_aprs; i++) {
        uint32_t apr;

        if (gic_is_vcpu(cpu)) {
            apr = s->h_apr[gic_get_vcpu_real_id(cpu)];
        } else {
            apr = s->apr[i][cpu] | s->nsapr[i][cpu];
        }

        if (!apr) {
            continue;
        }
        return (i * 32 + ctz32(apr)) << (min_bpr + 1);
    }
    return 0x100;
}

static void gic_drop_prio(GICState *s, int cpu, int group)
{
    /* Drop the priority of the currently active interrupt in the
     * specified group.
     *
     * Note that we can guarantee (because of the requirement to nest
     * GICC_IAR reads [which activate an interrupt and raise priority]
     * with GICC_EOIR writes [which drop the priority for the interrupt])
     * that the interrupt we're being called for is the highest priority
     * active interrupt, meaning that it has the lowest set bit in the
     * APR registers.
     *
     * If the guest does not honour the ordering constraints then the
     * behaviour of the GIC is UNPREDICTABLE, which for us means that
     * the values of the APR registers might become incorrect and the
     * running priority will be wrong, so interrupts that should preempt
     * might not do so, and interrupts that should not preempt might do so.
     */
    int i;
    int nr_aprs = gic_is_vcpu(cpu) ? GIC_VIRT_NR_APRS : GIC_NR_APRS;

    for (i = 0; i < nr_aprs; i++) {
        uint32_t *papr = NULL;

        if (gic_is_vcpu(cpu)) {
            papr = &s->h_apr[gic_get_vcpu_real_id(cpu)];
        } else if (group) {
            papr = &s->nsapr[i][cpu];
        } else {
            papr = &s->apr[i][cpu];
        }

        if (!*papr) {
            continue;
        }
        /* Clear lowest set bit */
        *papr &= *papr - 1;
        break;
    }

    s->running_priority[cpu] = gic_get_prio_from_apr_bits(s, cpu);
}

static inline uint32_t gic_clear_pending_sgi(GICState *s, int irq, int cpu)
{
    int src;
    uint32_t ret;

    if (!gic_is_vcpu(cpu)) {
        /* Lookup the source CPU for the SGI and clear this in the
         * sgi_pending map.  Return the src and clear the overall pending
         * state on this CPU if the SGI is not pending from any CPUs.
         */
        assert(s->sgi_pending[irq][cpu] != 0);
        src = ctz32(s->sgi_pending[irq][cpu]);
        s->sgi_pending[irq][cpu] &= ~(1 << src);
        if (s->sgi_pending[irq][cpu] == 0) {
            gic_clear_pending(s, irq, cpu);
        }
        ret = irq | ((src & 0x7) << 10);
    } else {
        uint32_t *lr_entry = gic_get_lr_entry(s, irq, cpu);
        src = GICH_LR_CPUID(*lr_entry);

        gic_clear_pending(s, irq, cpu);
        ret = irq | (src << 10);
    }

    return ret;
}

uint32_t gic_acknowledge_irq(GICState *s, int cpu, MemTxAttrs attrs)
{
    int ret, irq;

    /* gic_get_current_pending_irq() will return 1022 or 1023 appropriately
     * for the case where this GIC supports grouping and the pending interrupt
     * is in the wrong group.
     */
    irq = gic_get_current_pending_irq(s, cpu, attrs);
    trace_gic_acknowledge_irq(gic_is_vcpu(cpu) ? "vcpu" : "cpu",
                              gic_get_vcpu_real_id(cpu), irq);

    if (irq >= GIC_MAXIRQ) {
        DPRINTF("ACK, no pending interrupt or it is hidden: %d\n", irq);
        return irq;
    }

    if (gic_get_priority(s, irq, cpu) >= s->running_priority[cpu]) {
        DPRINTF("ACK, pending interrupt (%d) has insufficient priority\n", irq);
        return 1023;
    }

    if (s->revision == REV_11MPCORE) {
        /* Clear pending flags for both level and edge triggered interrupts.
         * Level triggered IRQs will be reasserted once they become inactive.
         */
        gic_clear_pending(s, irq, cpu);
        ret = irq;
    } else {
        if (irq < GIC_NR_SGIS) {
            ret = gic_clear_pending_sgi(s, irq, cpu);
        } else {
            gic_clear_pending(s, irq, cpu);
            ret = irq;
        }
    }

    gic_activate_irq(s, cpu, irq);
    if (gic_is_vcpu(cpu)) {
        gic_update_virt(s);
    } else {
        gic_update(s);
    }
    DPRINTF("ACK %d\n", irq);
    return ret;
}

void gic_dist_set_priority(GICState *s, int cpu, int irq, uint8_t val,
                      MemTxAttrs attrs)
{
    if (s->security_extn && !attrs.secure) {
        if (!GIC_DIST_TEST_GROUP(irq, (1 << cpu))) {
            return; /* Ignore Non-secure access of Group0 IRQ */
        }
        val = 0x80 | (val >> 1); /* Non-secure view */
    }

    if (irq < GIC_INTERNAL) {
        s->priority1[irq][cpu] = val;
    } else {
        s->priority2[(irq) - GIC_INTERNAL] = val;
    }
}

static uint32_t gic_dist_get_priority(GICState *s, int cpu, int irq,
                                 MemTxAttrs attrs)
{
    uint32_t prio = GIC_DIST_GET_PRIORITY(irq, cpu);

    if (s->security_extn && !attrs.secure) {
        if (!GIC_DIST_TEST_GROUP(irq, (1 << cpu))) {
            return 0; /* Non-secure access cannot read priority of Group0 IRQ */
        }
        prio = (prio << 1) & 0xff; /* Non-secure view */
    }
    return prio;
}

static void gic_set_priority_mask(GICState *s, int cpu, uint8_t pmask,
                                  MemTxAttrs attrs)
{
    if (s->security_extn && !attrs.secure) {
        if (s->priority_mask[cpu] & 0x80) {
            /* Priority Mask in upper half */
            pmask = 0x80 | (pmask >> 1);
        } else {
            /* Non-secure write ignored if priority mask is in lower half */
            return;
        }
    }
    s->priority_mask[cpu] = pmask;
}

static uint32_t gic_get_priority_mask(GICState *s, int cpu, MemTxAttrs attrs)
{
    uint32_t pmask = s->priority_mask[cpu];

    if (s->security_extn && !attrs.secure) {
        if (pmask & 0x80) {
            /* Priority Mask in upper half, return Non-secure view */
            pmask = (pmask << 1) & 0xff;
        } else {
            /* Priority Mask in lower half, RAZ */
            pmask = 0;
        }
    }
    return pmask;
}

static uint32_t gic_get_cpu_control(GICState *s, int cpu, MemTxAttrs attrs)
{
    uint32_t ret = s->cpu_ctlr[cpu];

    if (s->security_extn && !attrs.secure) {
        /* Construct the NS banked view of GICC_CTLR from the correct
         * bits of the S banked view. We don't need to move the bypass
         * control bits because we don't implement that (IMPDEF) part
         * of the GIC architecture.
         */
        ret = (ret & (GICC_CTLR_EN_GRP1 | GICC_CTLR_EOIMODE_NS)) >> 1;
    }
    return ret;
}

static void gic_set_cpu_control(GICState *s, int cpu, uint32_t value,
                                MemTxAttrs attrs)
{
    uint32_t mask;

    if (s->security_extn && !attrs.secure) {
        /* The NS view can only write certain bits in the register;
         * the rest are unchanged
         */
        mask = GICC_CTLR_EN_GRP1;
        if (s->revision == 2) {
            mask |= GICC_CTLR_EOIMODE_NS;
        }
        s->cpu_ctlr[cpu] &= ~mask;
        s->cpu_ctlr[cpu] |= (value << 1) & mask;
    } else {
        if (s->revision == 2) {
            mask = s->security_extn ? GICC_CTLR_V2_S_MASK : GICC_CTLR_V2_MASK;
        } else {
            mask = s->security_extn ? GICC_CTLR_V1_S_MASK : GICC_CTLR_V1_MASK;
        }
        s->cpu_ctlr[cpu] = value & mask;
    }
    DPRINTF("CPU Interface %d: Group0 Interrupts %sabled, "
            "Group1 Interrupts %sabled\n", cpu,
            (s->cpu_ctlr[cpu] & GICC_CTLR_EN_GRP0) ? "En" : "Dis",
            (s->cpu_ctlr[cpu] & GICC_CTLR_EN_GRP1) ? "En" : "Dis");
}

static uint8_t gic_get_running_priority(GICState *s, int cpu, MemTxAttrs attrs)
{
    if ((s->revision != REV_11MPCORE) && (s->running_priority[cpu] > 0xff)) {
        /* Idle priority */
        return 0xff;
    }

    if (s->security_extn && !attrs.secure) {
        if (s->running_priority[cpu] & 0x80) {
            /* Running priority in upper half of range: return the Non-secure
             * view of the priority.
             */
            return s->running_priority[cpu] << 1;
        } else {
            /* Running priority in lower half of range: RAZ */
            return 0;
        }
    } else {
        return s->running_priority[cpu];
    }
}

/* Return true if we should split priority drop and interrupt deactivation,
 * ie whether the relevant EOIMode bit is set.
 */
static bool gic_eoi_split(GICState *s, int cpu, MemTxAttrs attrs)
{
    if (s->revision != 2) {
        /* Before GICv2 prio-drop and deactivate are not separable */
        return false;
    }
    if (s->security_extn && !attrs.secure) {
        return s->cpu_ctlr[cpu] & GICC_CTLR_EOIMODE_NS;
    }
    return s->cpu_ctlr[cpu] & GICC_CTLR_EOIMODE;
}

static void gic_deactivate_irq(GICState *s, int cpu, int irq, MemTxAttrs attrs)
{
    int group = gic_has_groups(s) && gic_test_group(s, irq, cpu);

    if (!gic_eoi_split(s, cpu, attrs)) {
        /* This is UNPREDICTABLE; we choose to ignore it */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gic_deactivate_irq: GICC_DIR write when EOIMode clear");
        return;
    }

    if (s->security_extn && !attrs.secure && !group) {
        DPRINTF("Non-secure DI for Group0 interrupt %d ignored\n", irq);
        return;
    }

    gic_clear_active(s, irq, cpu);
}

static void gic_complete_irq(GICState *s, int cpu, int irq, MemTxAttrs attrs)
{
    int cm = 1 << cpu;
    int group;

    DPRINTF("EOI %d\n", irq);
    if (gic_is_vcpu(cpu) && !gic_virq_is_valid(s, irq, cpu)) {
        /* This vIRQ does not have a valid LR entry. Increment EOICount and
         * ignore the write. */

        int rcpu = gic_get_vcpu_real_id(cpu);
        int eoicount = extract32(s->h_hcr[rcpu], 27, 5) + 1;

        /* This 5 bits counter wraps to 0 */
        eoicount &= 0x1f;

        s->h_hcr[rcpu] = deposit32(s->h_hcr[rcpu], 27, 5, eoicount);
        return;
    }

    if (irq >= s->num_irq) {
        /* This handles two cases:
         * 1. If software writes the ID of a spurious interrupt [ie 1023]
         * to the GICC_EOIR, the GIC ignores that write.
         * 2. If software writes the number of a non-existent interrupt
         * this must be a subcase of "value written does not match the last
         * valid interrupt value read from the Interrupt Acknowledge
         * register" and so this is UNPREDICTABLE. We choose to ignore it.
         */
        return;
    }
    if (s->running_priority[cpu] == 0x100) {
        return; /* No active IRQ.  */
    }

    if (s->revision == REV_11MPCORE) {
        /* Mark level triggered interrupts as pending if they are still
           raised.  */
        if (!GIC_DIST_TEST_EDGE_TRIGGER(irq) && GIC_DIST_TEST_ENABLED(irq, cm)
            && GIC_DIST_TEST_LEVEL(irq, cm)
            && (GIC_DIST_TARGET(irq) & cm) != 0) {
            DPRINTF("Set %d pending mask %x\n", irq, cm);
            GIC_DIST_SET_PENDING(irq, cm);
        }
    }

    group = gic_has_groups(s) && gic_test_group(s, irq, cpu);

    if (s->security_extn && !attrs.secure && !group) {
        DPRINTF("Non-secure EOI for Group0 interrupt %d ignored\n", irq);
        return;
    }

    /* Secure EOI with GICC_CTLR.AckCtl == 0 when the IRQ is a Group 1
     * interrupt is UNPREDICTABLE. We choose to handle it as if AckCtl == 1,
     * i.e. go ahead and complete the irq anyway.
     */

    gic_drop_prio(s, cpu, group);

    /* In GICv2 the guest can choose to split priority-drop and deactivate */
    if (!gic_eoi_split(s, cpu, attrs)) {
        gic_clear_active(s, irq, cpu);
    }

    if (gic_is_vcpu(cpu)) {
        gic_update_virt(s);
    } else {
        gic_update(s);
    }
}

static uint32_t gic_dist_readb(void *opaque, hwaddr offset, MemTxAttrs attrs)
{
    GICState *s = (GICState *)opaque;
    uint32_t res;
    int irq;
    int i;
    int cpu;
    int cm;
    int mask;

    cpu = gic_get_current_cpu(s);
    cm = 1 << cpu;
    if (offset < 0x100) {
        if (offset == 0) {      /* GICD_CTLR */
            if (s->security_extn && !attrs.secure) {
                /* The NS bank of this register is just an alias of the
                 * EnableGrp1 bit in the S bank version.
                 */
                return extract32(s->ctlr, 1, 1);
            } else {
                return s->ctlr;
            }
        }
        if (offset == 4)
            /* Interrupt Controller Type Register */
            return ((s->num_irq / 32) - 1)
                    | ((s->num_cpu - 1) << 5)
                    | (s->security_extn << 10);
        if (offset < 0x08)
            return 0;
        if (offset >= 0x80) {
            /* Interrupt Group Registers: these RAZ/WI if this is an NS
             * access to a GIC with the security extensions, or if the GIC
             * doesn't have groups at all.
             */
            res = 0;
            if (!(s->security_extn && !attrs.secure) && gic_has_groups(s)) {
                /* Every byte offset holds 8 group status bits */
                irq = (offset - 0x080) * 8 + GIC_BASE_IRQ;
                if (irq >= s->num_irq) {
                    goto bad_reg;
                }
                for (i = 0; i < 8; i++) {
                    if (GIC_DIST_TEST_GROUP(irq + i, cm)) {
                        res |= (1 << i);
                    }
                }
            }
            return res;
        }
        goto bad_reg;
    } else if (offset < 0x200) {
        /* Interrupt Set/Clear Enable.  */
        if (offset < 0x180)
            irq = (offset - 0x100) * 8;
        else
            irq = (offset - 0x180) * 8;
        irq += GIC_BASE_IRQ;
        if (irq >= s->num_irq)
            goto bad_reg;
        res = 0;
        for (i = 0; i < 8; i++) {
            if (s->security_extn && !attrs.secure &&
                !GIC_DIST_TEST_GROUP(irq + i, 1 << cpu)) {
                continue; /* Ignore Non-secure access of Group0 IRQ */
            }

            if (GIC_DIST_TEST_ENABLED(irq + i, cm)) {
                res |= (1 << i);
            }
        }
    } else if (offset < 0x300) {
        /* Interrupt Set/Clear Pending.  */
        if (offset < 0x280)
            irq = (offset - 0x200) * 8;
        else
            irq = (offset - 0x280) * 8;
        irq += GIC_BASE_IRQ;
        if (irq >= s->num_irq)
            goto bad_reg;
        res = 0;
        mask = (irq < GIC_INTERNAL) ?  cm : ALL_CPU_MASK;
        for (i = 0; i < 8; i++) {
            if (s->security_extn && !attrs.secure &&
                !GIC_DIST_TEST_GROUP(irq + i, 1 << cpu)) {
                continue; /* Ignore Non-secure access of Group0 IRQ */
            }

            if (gic_test_pending(s, irq + i, mask)) {
                res |= (1 << i);
            }
        }
    } else if (offset < 0x400) {
        /* Interrupt Active.  */
        irq = (offset - 0x300) * 8 + GIC_BASE_IRQ;
        if (irq >= s->num_irq)
            goto bad_reg;
        res = 0;
        mask = (irq < GIC_INTERNAL) ?  cm : ALL_CPU_MASK;
        for (i = 0; i < 8; i++) {
            if (s->security_extn && !attrs.secure &&
                !GIC_DIST_TEST_GROUP(irq + i, 1 << cpu)) {
                continue; /* Ignore Non-secure access of Group0 IRQ */
            }

            if (GIC_DIST_TEST_ACTIVE(irq + i, mask)) {
                res |= (1 << i);
            }
        }
    } else if (offset < 0x800) {
        /* Interrupt Priority.  */
        irq = (offset - 0x400) + GIC_BASE_IRQ;
        if (irq >= s->num_irq)
            goto bad_reg;
        res = gic_dist_get_priority(s, cpu, irq, attrs);
    } else if (offset < 0xc00) {
        /* Interrupt CPU Target.  */
        if (s->num_cpu == 1 && s->revision != REV_11MPCORE) {
            /* For uniprocessor GICs these RAZ/WI */
            res = 0;
        } else {
            irq = (offset - 0x800) + GIC_BASE_IRQ;
            if (irq >= s->num_irq) {
                goto bad_reg;
            }
            if (irq >= 29 && irq <= 31) {
                res = cm;
            } else {
                res = GIC_DIST_TARGET(irq);
            }
        }
    } else if (offset < 0xf00) {
        /* Interrupt Configuration.  */
        irq = (offset - 0xc00) * 4 + GIC_BASE_IRQ;
        if (irq >= s->num_irq)
            goto bad_reg;
        res = 0;
        for (i = 0; i < 4; i++) {
            if (s->security_extn && !attrs.secure &&
                !GIC_DIST_TEST_GROUP(irq + i, 1 << cpu)) {
                continue; /* Ignore Non-secure access of Group0 IRQ */
            }

            if (GIC_DIST_TEST_MODEL(irq + i)) {
                res |= (1 << (i * 2));
            }
            if (GIC_DIST_TEST_EDGE_TRIGGER(irq + i)) {
                res |= (2 << (i * 2));
            }
        }
    } else if (offset < 0xf10) {
        goto bad_reg;
    } else if (offset < 0xf30) {
        if (s->revision == REV_11MPCORE) {
            goto bad_reg;
        }

        if (offset < 0xf20) {
            /* GICD_CPENDSGIRn */
            irq = (offset - 0xf10);
        } else {
            irq = (offset - 0xf20);
            /* GICD_SPENDSGIRn */
        }

        if (s->security_extn && !attrs.secure &&
            !GIC_DIST_TEST_GROUP(irq, 1 << cpu)) {
            res = 0; /* Ignore Non-secure access of Group0 IRQ */
        } else {
            res = s->sgi_pending[irq][cpu];
        }
    } else if (offset < 0xfd0) {
        goto bad_reg;
    } else if (offset < 0x1000) {
        if (offset & 3) {
            res = 0;
        } else {
            switch (s->revision) {
            case REV_11MPCORE:
                res = gic_id_11mpcore[(offset - 0xfd0) >> 2];
                break;
            case 1:
                res = gic_id_gicv1[(offset - 0xfd0) >> 2];
                break;
            case 2:
                res = gic_id_gicv2[(offset - 0xfd0) >> 2];
                break;
            default:
                res = 0;
            }
        }
    } else {
        g_assert_not_reached();
    }
    return res;
bad_reg:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "gic_dist_readb: Bad offset %x\n", (int)offset);
    return 0;
}

static MemTxResult gic_dist_read(void *opaque, hwaddr offset, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    switch (size) {
    case 1:
        *data = gic_dist_readb(opaque, offset, attrs);
        break;
    case 2:
        *data = gic_dist_readb(opaque, offset, attrs);
        *data |= gic_dist_readb(opaque, offset + 1, attrs) << 8;
        break;
    case 4:
        *data = gic_dist_readb(opaque, offset, attrs);
        *data |= gic_dist_readb(opaque, offset + 1, attrs) << 8;
        *data |= gic_dist_readb(opaque, offset + 2, attrs) << 16;
        *data |= gic_dist_readb(opaque, offset + 3, attrs) << 24;
        break;
    default:
        return MEMTX_ERROR;
    }

    trace_gic_dist_read(offset, size, *data);
    return MEMTX_OK;
}

static void gic_dist_writeb(void *opaque, hwaddr offset,
                            uint32_t value, MemTxAttrs attrs)
{
    GICState *s = (GICState *)opaque;
    int irq;
    int i;
    int cpu;

    cpu = gic_get_current_cpu(s);
    if (offset < 0x100) {
        if (offset == 0) {
            if (s->security_extn && !attrs.secure) {
                /* NS version is just an alias of the S version's bit 1 */
                s->ctlr = deposit32(s->ctlr, 1, 1, value);
            } else if (gic_has_groups(s)) {
                s->ctlr = value & (GICD_CTLR_EN_GRP0 | GICD_CTLR_EN_GRP1);
            } else {
                s->ctlr = value & GICD_CTLR_EN_GRP0;
            }
            DPRINTF("Distributor: Group0 %sabled; Group 1 %sabled\n",
                    s->ctlr & GICD_CTLR_EN_GRP0 ? "En" : "Dis",
                    s->ctlr & GICD_CTLR_EN_GRP1 ? "En" : "Dis");
        } else if (offset < 4) {
            /* ignored.  */
        } else if (offset >= 0x80) {
            /* Interrupt Group Registers: RAZ/WI for NS access to secure
             * GIC, or for GICs without groups.
             */
            if (!(s->security_extn && !attrs.secure) && gic_has_groups(s)) {
                /* Every byte offset holds 8 group status bits */
                irq = (offset - 0x80) * 8 + GIC_BASE_IRQ;
                if (irq >= s->num_irq) {
                    goto bad_reg;
                }
                for (i = 0; i < 8; i++) {
                    /* Group bits are banked for private interrupts */
                    int cm = (irq < GIC_INTERNAL) ? (1 << cpu) : ALL_CPU_MASK;
                    if (value & (1 << i)) {
                        /* Group1 (Non-secure) */
                        GIC_DIST_SET_GROUP(irq + i, cm);
                    } else {
                        /* Group0 (Secure) */
                        GIC_DIST_CLEAR_GROUP(irq + i, cm);
                    }
                }
            }
        } else {
            goto bad_reg;
        }
    } else if (offset < 0x180) {
        /* Interrupt Set Enable.  */
        irq = (offset - 0x100) * 8 + GIC_BASE_IRQ;
        if (irq >= s->num_irq)
            goto bad_reg;
        if (irq < GIC_NR_SGIS) {
            value = 0xff;
        }

        for (i = 0; i < 8; i++) {
            if (value & (1 << i)) {
                int mask =
                    (irq < GIC_INTERNAL) ? (1 << cpu)
                                         : GIC_DIST_TARGET(irq + i);
                int cm = (irq < GIC_INTERNAL) ? (1 << cpu) : ALL_CPU_MASK;

                if (s->security_extn && !attrs.secure &&
                    !GIC_DIST_TEST_GROUP(irq + i, 1 << cpu)) {
                    continue; /* Ignore Non-secure access of Group0 IRQ */
                }

                if (!GIC_DIST_TEST_ENABLED(irq + i, cm)) {
                    DPRINTF("Enabled IRQ %d\n", irq + i);
                    trace_gic_enable_irq(irq + i);
                }
                GIC_DIST_SET_ENABLED(irq + i, cm);
                /* If a raised level triggered IRQ enabled then mark
                   is as pending.  */
                if (GIC_DIST_TEST_LEVEL(irq + i, mask)
                        && !GIC_DIST_TEST_EDGE_TRIGGER(irq + i)) {
                    DPRINTF("Set %d pending mask %x\n", irq + i, mask);
                    GIC_DIST_SET_PENDING(irq + i, mask);
                }
            }
        }
    } else if (offset < 0x200) {
        /* Interrupt Clear Enable.  */
        irq = (offset - 0x180) * 8 + GIC_BASE_IRQ;
        if (irq >= s->num_irq)
            goto bad_reg;
        if (irq < GIC_NR_SGIS) {
            value = 0;
        }

        for (i = 0; i < 8; i++) {
            if (value & (1 << i)) {
                int cm = (irq < GIC_INTERNAL) ? (1 << cpu) : ALL_CPU_MASK;

                if (s->security_extn && !attrs.secure &&
                    !GIC_DIST_TEST_GROUP(irq + i, 1 << cpu)) {
                    continue; /* Ignore Non-secure access of Group0 IRQ */
                }

                if (GIC_DIST_TEST_ENABLED(irq + i, cm)) {
                    DPRINTF("Disabled IRQ %d\n", irq + i);
                    trace_gic_disable_irq(irq + i);
                }
                GIC_DIST_CLEAR_ENABLED(irq + i, cm);
            }
        }
    } else if (offset < 0x280) {
        /* Interrupt Set Pending.  */
        irq = (offset - 0x200) * 8 + GIC_BASE_IRQ;
        if (irq >= s->num_irq)
            goto bad_reg;
        if (irq < GIC_NR_SGIS) {
            value = 0;
        }

        for (i = 0; i < 8; i++) {
            if (value & (1 << i)) {
                if (s->security_extn && !attrs.secure &&
                    !GIC_DIST_TEST_GROUP(irq + i, 1 << cpu)) {
                    continue; /* Ignore Non-secure access of Group0 IRQ */
                }

                GIC_DIST_SET_PENDING(irq + i, GIC_DIST_TARGET(irq + i));
            }
        }
    } else if (offset < 0x300) {
        /* Interrupt Clear Pending.  */
        irq = (offset - 0x280) * 8 + GIC_BASE_IRQ;
        if (irq >= s->num_irq)
            goto bad_reg;
        if (irq < GIC_NR_SGIS) {
            value = 0;
        }

        for (i = 0; i < 8; i++) {
            if (s->security_extn && !attrs.secure &&
                !GIC_DIST_TEST_GROUP(irq + i, 1 << cpu)) {
                continue; /* Ignore Non-secure access of Group0 IRQ */
            }

            /* ??? This currently clears the pending bit for all CPUs, even
               for per-CPU interrupts.  It's unclear whether this is the
               corect behavior.  */
            if (value & (1 << i)) {
                GIC_DIST_CLEAR_PENDING(irq + i, ALL_CPU_MASK);
            }
        }
    } else if (offset < 0x400) {
        /* Interrupt Active.  */
        goto bad_reg;
    } else if (offset < 0x800) {
        /* Interrupt Priority.  */
        irq = (offset - 0x400) + GIC_BASE_IRQ;
        if (irq >= s->num_irq)
            goto bad_reg;
        gic_dist_set_priority(s, cpu, irq, value, attrs);
    } else if (offset < 0xc00) {
        /* Interrupt CPU Target. RAZ/WI on uniprocessor GICs, with the
         * annoying exception of the 11MPCore's GIC.
         */
        if (s->num_cpu != 1 || s->revision == REV_11MPCORE) {
            irq = (offset - 0x800) + GIC_BASE_IRQ;
            if (irq >= s->num_irq) {
                goto bad_reg;
            }
            if (irq < 29) {
                value = 0;
            } else if (irq < GIC_INTERNAL) {
                value = ALL_CPU_MASK;
            }
            s->irq_target[irq] = value & ALL_CPU_MASK;
        }
    } else if (offset < 0xf00) {
        /* Interrupt Configuration.  */
        irq = (offset - 0xc00) * 4 + GIC_BASE_IRQ;
        if (irq >= s->num_irq)
            goto bad_reg;
        if (irq < GIC_NR_SGIS)
            value |= 0xaa;
        for (i = 0; i < 4; i++) {
            if (s->security_extn && !attrs.secure &&
                !GIC_DIST_TEST_GROUP(irq + i, 1 << cpu)) {
                continue; /* Ignore Non-secure access of Group0 IRQ */
            }

            if (s->revision == REV_11MPCORE) {
                if (value & (1 << (i * 2))) {
                    GIC_DIST_SET_MODEL(irq + i);
                } else {
                    GIC_DIST_CLEAR_MODEL(irq + i);
                }
            }
            if (value & (2 << (i * 2))) {
                GIC_DIST_SET_EDGE_TRIGGER(irq + i);
            } else {
                GIC_DIST_CLEAR_EDGE_TRIGGER(irq + i);
            }
        }
    } else if (offset < 0xf10) {
        /* 0xf00 is only handled for 32-bit writes.  */
        goto bad_reg;
    } else if (offset < 0xf20) {
        /* GICD_CPENDSGIRn */
        if (s->revision == REV_11MPCORE) {
            goto bad_reg;
        }
        irq = (offset - 0xf10);

        if (!s->security_extn || attrs.secure ||
            GIC_DIST_TEST_GROUP(irq, 1 << cpu)) {
            s->sgi_pending[irq][cpu] &= ~value;
            if (s->sgi_pending[irq][cpu] == 0) {
                GIC_DIST_CLEAR_PENDING(irq, 1 << cpu);
            }
        }
    } else if (offset < 0xf30) {
        /* GICD_SPENDSGIRn */
        if (s->revision == REV_11MPCORE) {
            goto bad_reg;
        }
        irq = (offset - 0xf20);

        if (!s->security_extn || attrs.secure ||
            GIC_DIST_TEST_GROUP(irq, 1 << cpu)) {
            GIC_DIST_SET_PENDING(irq, 1 << cpu);
            s->sgi_pending[irq][cpu] |= value;
        }
    } else {
        goto bad_reg;
    }
    gic_update(s);
    return;
bad_reg:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "gic_dist_writeb: Bad offset %x\n", (int)offset);
}

static void gic_dist_writew(void *opaque, hwaddr offset,
                            uint32_t value, MemTxAttrs attrs)
{
    gic_dist_writeb(opaque, offset, value & 0xff, attrs);
    gic_dist_writeb(opaque, offset + 1, value >> 8, attrs);
}

static void gic_dist_writel(void *opaque, hwaddr offset,
                            uint32_t value, MemTxAttrs attrs)
{
    GICState *s = (GICState *)opaque;
    if (offset == 0xf00) {
        int cpu;
        int irq;
        int mask;
        int target_cpu;

        cpu = gic_get_current_cpu(s);
        irq = value & 0x3ff;
        switch ((value >> 24) & 3) {
        case 0:
            mask = (value >> 16) & ALL_CPU_MASK;
            break;
        case 1:
            mask = ALL_CPU_MASK ^ (1 << cpu);
            break;
        case 2:
            mask = 1 << cpu;
            break;
        default:
            DPRINTF("Bad Soft Int target filter\n");
            mask = ALL_CPU_MASK;
            break;
        }
        GIC_DIST_SET_PENDING(irq, mask);
        target_cpu = ctz32(mask);
        while (target_cpu < GIC_NCPU) {
            s->sgi_pending[irq][target_cpu] |= (1 << cpu);
            mask &= ~(1 << target_cpu);
            target_cpu = ctz32(mask);
        }
        gic_update(s);
        return;
    }
    gic_dist_writew(opaque, offset, value & 0xffff, attrs);
    gic_dist_writew(opaque, offset + 2, value >> 16, attrs);
}

static MemTxResult gic_dist_write(void *opaque, hwaddr offset, uint64_t data,
                                  unsigned size, MemTxAttrs attrs)
{
    trace_gic_dist_write(offset, size, data);

    switch (size) {
    case 1:
        gic_dist_writeb(opaque, offset, data, attrs);
        return MEMTX_OK;
    case 2:
        gic_dist_writew(opaque, offset, data, attrs);
        return MEMTX_OK;
    case 4:
        gic_dist_writel(opaque, offset, data, attrs);
        return MEMTX_OK;
    default:
        return MEMTX_ERROR;
    }
}

static inline uint32_t gic_apr_ns_view(GICState *s, int cpu, int regno)
{
    /* Return the Nonsecure view of GICC_APR<regno>. This is the
     * second half of GICC_NSAPR.
     */
    switch (GIC_MIN_BPR) {
    case 0:
        if (regno < 2) {
            return s->nsapr[regno + 2][cpu];
        }
        break;
    case 1:
        if (regno == 0) {
            return s->nsapr[regno + 1][cpu];
        }
        break;
    case 2:
        if (regno == 0) {
            return extract32(s->nsapr[0][cpu], 16, 16);
        }
        break;
    case 3:
        if (regno == 0) {
            return extract32(s->nsapr[0][cpu], 8, 8);
        }
        break;
    default:
        g_assert_not_reached();
    }
    return 0;
}

static inline void gic_apr_write_ns_view(GICState *s, int cpu, int regno,
                                         uint32_t value)
{
    /* Write the Nonsecure view of GICC_APR<regno>. */
    switch (GIC_MIN_BPR) {
    case 0:
        if (regno < 2) {
            s->nsapr[regno + 2][cpu] = value;
        }
        break;
    case 1:
        if (regno == 0) {
            s->nsapr[regno + 1][cpu] = value;
        }
        break;
    case 2:
        if (regno == 0) {
            s->nsapr[0][cpu] = deposit32(s->nsapr[0][cpu], 16, 16, value);
        }
        break;
    case 3:
        if (regno == 0) {
            s->nsapr[0][cpu] = deposit32(s->nsapr[0][cpu], 8, 8, value);
        }
        break;
    default:
        g_assert_not_reached();
    }
}

static MemTxResult gic_cpu_read(GICState *s, int cpu, int offset,
                                uint64_t *data, MemTxAttrs attrs)
{
    switch (offset) {
    case 0x00: /* Control */
        *data = gic_get_cpu_control(s, cpu, attrs);
        break;
    case 0x04: /* Priority mask */
        *data = gic_get_priority_mask(s, cpu, attrs);
        break;
    case 0x08: /* Binary Point */
        if (s->security_extn && !attrs.secure) {
            if (s->cpu_ctlr[cpu] & GICC_CTLR_CBPR) {
                /* NS view of BPR when CBPR is 1 */
                *data = MIN(s->bpr[cpu] + 1, 7);
            } else {
                /* BPR is banked. Non-secure copy stored in ABPR. */
                *data = s->abpr[cpu];
            }
        } else {
            *data = s->bpr[cpu];
        }
        break;
    case 0x0c: /* Acknowledge */
        *data = gic_acknowledge_irq(s, cpu, attrs);
        break;
    case 0x14: /* Running Priority */
        *data = gic_get_running_priority(s, cpu, attrs);
        break;
    case 0x18: /* Highest Pending Interrupt */
        *data = gic_get_current_pending_irq(s, cpu, attrs);
        break;
    case 0x1c: /* Aliased Binary Point */
        /* GIC v2, no security: ABPR
         * GIC v1, no security: not implemented (RAZ/WI)
         * With security extensions, secure access: ABPR (alias of NS BPR)
         * With security extensions, nonsecure access: RAZ/WI
         */
        if (!gic_has_groups(s) || (s->security_extn && !attrs.secure)) {
            *data = 0;
        } else {
            *data = s->abpr[cpu];
        }
        break;
    case 0xd0: case 0xd4: case 0xd8: case 0xdc:
    {
        int regno = (offset - 0xd0) / 4;
        int nr_aprs = gic_is_vcpu(cpu) ? GIC_VIRT_NR_APRS : GIC_NR_APRS;

        if (regno >= nr_aprs || s->revision != 2) {
            *data = 0;
        } else if (s->security_extn && !attrs.secure) {
            /* NS view of GICC_APR<n> is the top half of GIC_NSAPR<n> */
            *data = gic_apr_ns_view(s, regno, cpu);
        } else if (gic_is_vcpu(cpu)) {
            *data = s->h_apr[gic_get_vcpu_real_id(cpu)];
        } else {
            *data = s->apr[regno][cpu];
        }
        break;
    }
    case 0xe0: case 0xe4: case 0xe8: case 0xec:
    {
        int regno = (offset - 0xe0) / 4;

        if (regno >= GIC_NR_APRS || s->revision != 2 || !gic_has_groups(s) ||
            (s->security_extn && !attrs.secure) || gic_is_vcpu(cpu)) {
            *data = 0;
        } else {
            *data = s->nsapr[regno][cpu];
        }
        break;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gic_cpu_read: Bad offset %x\n", (int)offset);
        *data = 0;
        break;
    }

    trace_gic_cpu_read(gic_is_vcpu(cpu) ? "vcpu" : "cpu",
                       gic_get_vcpu_real_id(cpu), offset, *data);
    return MEMTX_OK;
}

static MemTxResult gic_cpu_write(GICState *s, int cpu, int offset,
                                 uint32_t value, MemTxAttrs attrs)
{
    trace_gic_cpu_write(gic_is_vcpu(cpu) ? "vcpu" : "cpu",
                        gic_get_vcpu_real_id(cpu), offset, value);

    switch (offset) {
    case 0x00: /* Control */
        gic_set_cpu_control(s, cpu, value, attrs);
        break;
    case 0x04: /* Priority mask */
        gic_set_priority_mask(s, cpu, value, attrs);
        break;
    case 0x08: /* Binary Point */
        if (s->security_extn && !attrs.secure) {
            if (s->cpu_ctlr[cpu] & GICC_CTLR_CBPR) {
                /* WI when CBPR is 1 */
                return MEMTX_OK;
            } else {
                s->abpr[cpu] = MAX(value & 0x7, GIC_MIN_ABPR);
            }
        } else {
            int min_bpr = gic_is_vcpu(cpu) ? GIC_VIRT_MIN_BPR : GIC_MIN_BPR;
            s->bpr[cpu] = MAX(value & 0x7, min_bpr);
        }
        break;
    case 0x10: /* End Of Interrupt */
        gic_complete_irq(s, cpu, value & 0x3ff, attrs);
        return MEMTX_OK;
    case 0x1c: /* Aliased Binary Point */
        if (!gic_has_groups(s) || (s->security_extn && !attrs.secure)) {
            /* unimplemented, or NS access: RAZ/WI */
            return MEMTX_OK;
        } else {
            s->abpr[cpu] = MAX(value & 0x7, GIC_MIN_ABPR);
        }
        break;
    case 0xd0: case 0xd4: case 0xd8: case 0xdc:
    {
        int regno = (offset - 0xd0) / 4;
        int nr_aprs = gic_is_vcpu(cpu) ? GIC_VIRT_NR_APRS : GIC_NR_APRS;

        if (regno >= nr_aprs || s->revision != 2) {
            return MEMTX_OK;
        }
        if (s->security_extn && !attrs.secure) {
            /* NS view of GICC_APR<n> is the top half of GIC_NSAPR<n> */
            gic_apr_write_ns_view(s, regno, cpu, value);
        } else if (gic_is_vcpu(cpu)) {
            s->h_apr[gic_get_vcpu_real_id(cpu)] = value;
        } else {
            s->apr[regno][cpu] = value;
        }
        break;
    }
    case 0xe0: case 0xe4: case 0xe8: case 0xec:
    {
        int regno = (offset - 0xe0) / 4;

        if (regno >= GIC_NR_APRS || s->revision != 2) {
            return MEMTX_OK;
        }
        if (!gic_has_groups(s) || (s->security_extn && !attrs.secure)) {
            return MEMTX_OK;
        }
        if (gic_is_vcpu(cpu)) {
            return MEMTX_OK;
        }
        s->nsapr[regno][cpu] = value;
        break;
    }
    case 0x1000:
        /* GICC_DIR */
        gic_deactivate_irq(s, cpu, value & 0x3ff, attrs);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gic_cpu_write: Bad offset %x\n", (int)offset);
        return MEMTX_OK;
    }

    if (gic_is_vcpu(cpu)) {
        gic_update_virt(s);
    } else {
        gic_update(s);
    }

    return MEMTX_OK;
}

/* Wrappers to read/write the GIC CPU interface for the current CPU */
static MemTxResult gic_thiscpu_read(void *opaque, hwaddr addr, uint64_t *data,
                                    unsigned size, MemTxAttrs attrs)
{
    GICState *s = (GICState *)opaque;
    return gic_cpu_read(s, gic_get_current_cpu(s), addr, data, attrs);
}

static MemTxResult gic_thiscpu_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    GICState *s = (GICState *)opaque;
    return gic_cpu_write(s, gic_get_current_cpu(s), addr, value, attrs);
}

/* Wrappers to read/write the GIC CPU interface for a specific CPU.
 * These just decode the opaque pointer into GICState* + cpu id.
 */
static MemTxResult gic_do_cpu_read(void *opaque, hwaddr addr, uint64_t *data,
                                   unsigned size, MemTxAttrs attrs)
{
    GICState **backref = (GICState **)opaque;
    GICState *s = *backref;
    int id = (backref - s->backref);
    return gic_cpu_read(s, id, addr, data, attrs);
}

static MemTxResult gic_do_cpu_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    GICState **backref = (GICState **)opaque;
    GICState *s = *backref;
    int id = (backref - s->backref);
    return gic_cpu_write(s, id, addr, value, attrs);

}

static MemTxResult gic_thisvcpu_read(void *opaque, hwaddr addr, uint64_t *data,
                                    unsigned size, MemTxAttrs attrs)
{
    GICState *s = (GICState *)opaque;

    /* The exposed vCPU interface does not implement security extensions.
     * Pretend this access is secure to go through the "no security extension
     * or secure access" path in the CPU interface. */
    attrs.secure = 1;

    return gic_cpu_read(s, gic_get_current_cpu(s) + GIC_NCPU,
                        addr, data, attrs);
}

static MemTxResult gic_thisvcpu_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    GICState *s = (GICState *)opaque;

    attrs.secure = 1;

    return gic_cpu_write(s, gic_get_current_cpu(s) + GIC_NCPU,
                         addr, value, attrs);
}

static MemTxResult gic_do_vcpu_read(void *opaque, hwaddr addr, uint64_t *data,
                                    unsigned size, MemTxAttrs attrs)
{
    GICState **backref = (GICState **)opaque;
    GICState *s = *backref;
    int id = (backref - s->backref);

    attrs.secure = 1;

    return gic_cpu_read(s, id + GIC_NCPU, addr, data, attrs);
}

static MemTxResult gic_do_vcpu_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    GICState **backref = (GICState **)opaque;
    GICState *s = *backref;
    int id = (backref - s->backref);

    attrs.secure = 1;

    return gic_cpu_write(s, id + GIC_NCPU, addr, value, attrs);

}

static void gic_vmcr_write(GICState *s, uint32_t value)
{
    int vcpu = gic_get_current_vcpu(s);
    uint32_t ctlr;
    uint32_t abpr;
    uint32_t bpr;
    uint32_t prio_mask;

    /* Always pretend to do a secure access */
    MemTxAttrs attrs = { .secure = 1 };

    ctlr = extract32(value, 0, 5) & 0x0000021f;
    abpr = extract32(value, 18, 3);
    bpr = extract32(value, 21, 3);
    prio_mask = extract32(value, 27, 5) << 3;

    gic_set_cpu_control(s, vcpu, ctlr, attrs);
    s->abpr[vcpu] = MAX(abpr, GIC_VIRT_MIN_ABPR);
    s->bpr[vcpu] = MAX(bpr, GIC_VIRT_MIN_BPR);
    gic_set_priority_mask(s, vcpu, prio_mask, attrs);
}

static void gic_set_lr_entry(GICState *s, int cpu, int lr_num, uint32_t entry)
{
    assert(lr_num < GIC_NR_LR);

    uint32_t prev_entry = s->h_lr[lr_num][cpu];
    int irq;
    bool is_free;

    if (!gic_lr_entry_is_free(prev_entry)) {
        /* The entry was valid, flush it */
        irq = GICH_LR_VIRT_ID(prev_entry);
        gic_clear_virq_cache(s, irq, cpu);
    }

    s->h_lr[lr_num][cpu] = entry;
    trace_gic_lr_entry(cpu, lr_num, entry);
    is_free = gic_lr_update(s, lr_num, cpu);

    if (!is_free) {
        /* Update the vIRQ -> LR entry cache */
        irq = GICH_LR_VIRT_ID(entry);
        gic_set_virq_cache(s, irq, cpu, lr_num);
    }
}

static MemTxResult gic_hyp_read(void *opaque, hwaddr addr, uint64_t *data,
                                unsigned size, MemTxAttrs attrs)
{
    GICState *s = ARM_GIC(opaque);
    int cpu = gic_get_current_cpu(s);
    int vcpu = gic_get_current_vcpu(s);

    switch (addr) {
    case 0x0: /* Hypervisor Control */
        *data = s->h_hcr[cpu];
        break;

    case 0x4: /* VGIC Type */
        *data = ((6 - GIC_VIRT_MIN_BPR) << 29)
            | ((GIC_VIRT_MAX_GROUP_PRIO_BITS - 1) << 26)
            | (GIC_NR_LR - 1);
        break;

    case 0x8: /* Virtual Machine Control */
        *data = extract32(s->cpu_ctlr[vcpu], 0, 10);
        *data |= extract32(s->abpr[vcpu], 0, 3) << 18;
        *data |= extract32(s->bpr[vcpu], 0, 3) << 21;
        *data |= extract32(s->priority_mask[vcpu], 3, 5) << 27;
        break;

    case 0x10: /* Maintenance Interrupt Status */
        *data = s->h_misr[cpu];
        break;

    case 0x20: /* End of Interrupt Status 0 and 1 */
    case 0x24:
        *data = (uint32_t) extract64(s->h_eisr[cpu], (addr - 0x20) * 8, 32);
        break;

    case 0x30: /* Empty List Status 0 and 1 */
    case 0x34:
        *data = (uint32_t) extract64(s->h_elrsr[cpu], (addr - 0x30) * 8, 32);
        break;

    case 0xf0: /* Active Priorities */
        *data = s->h_apr[cpu];
        break;

    case 0x100 ... 0x1fc: /* List Registers */
    {
        int lr_num = (addr - 0x100) / 4;

        if (lr_num > GIC_NR_LR) {
            *data = 0;
        } else {
            *data = s->h_lr[lr_num][cpu];
        }
        break;
    }

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gic_hyp_read: Bad offset %" HWADDR_PRIx "\n", addr);
        return MEMTX_OK;
    }

    trace_gic_hyp_read(addr, *data);
    return MEMTX_OK;
}

static MemTxResult gic_hyp_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size, MemTxAttrs attrs)
{
    GICState *s = ARM_GIC(opaque);
    int cpu = gic_get_current_cpu(s);

    trace_gic_hyp_write(addr, value);

    switch (addr) {
    case 0x0: /* Hypervisor Control */
        s->h_hcr[cpu] = value & 0xf80000ff;
        break;

    case 0x8: /* Virtual Machine Control */
        gic_vmcr_write(s, value);
        break;

    case 0xf0: /* Active Priorities */
        s->h_apr[cpu] = value;
        break;

    case 0x100 ... 0x1fc: /* List Registers */
    {
        int lr_num = (addr - 0x100) / 4;

        if (lr_num > GIC_NR_LR) {
            return MEMTX_OK;
        }

        gic_set_lr_entry(s, cpu, lr_num, value & 0xff8fffff);
        break;
    }

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gic_hyp_write: Bad offset %" HWADDR_PRIx "\n", addr);
        return MEMTX_OK;
    }

    gic_update_virt(s);
    return MEMTX_OK;
}

static void arm_gic_post_load(GICState *s)
{
    if (s->virt_extn) {
        gic_recompute_virt_cache(s);
    }
}

static const MemoryRegionOps gic_ops[2] = {
    {
        .read_with_attrs = gic_dist_read,
        .write_with_attrs = gic_dist_write,
        .endianness = DEVICE_NATIVE_ENDIAN,
    },
    {
        .read_with_attrs = gic_thiscpu_read,
        .write_with_attrs = gic_thiscpu_write,
        .endianness = DEVICE_NATIVE_ENDIAN,
    }
};

static const MemoryRegionOps gic_cpu_ops = {
    .read_with_attrs = gic_do_cpu_read,
    .write_with_attrs = gic_do_cpu_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps gic_virt_ops[2] = {
    {
        .read_with_attrs = gic_hyp_read,
        .write_with_attrs = gic_hyp_write,
        .endianness = DEVICE_NATIVE_ENDIAN,
    },
    {
        .read_with_attrs = gic_thisvcpu_read,
        .write_with_attrs = gic_thisvcpu_write,
        .endianness = DEVICE_NATIVE_ENDIAN,
    }
};

static const MemoryRegionOps gic_vcpu_ops = {
    .read_with_attrs = gic_do_vcpu_read,
    .write_with_attrs = gic_do_vcpu_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void arm_gic_realize(DeviceState *dev, Error **errp)
{
    /* Device instance realize function for the GIC sysbus device */
    int i;
    GICState *s = ARM_GIC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    ARMGICClass *agc = ARM_GIC_GET_CLASS(s);
    Error *local_err = NULL;

    agc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (kvm_enabled() && !kvm_arm_supports_user_irq()) {
        error_setg(errp, "KVM with user space irqchip only works when the "
                         "host kernel supports KVM_CAP_ARM_USER_IRQ");
        return;
    }

    /* This creates distributor and main CPU interface (s->cpuiomem[0]) */
    gic_init_irqs_and_mmio(s, gic_set_irq, gic_ops, gic_virt_ops);

    /* Extra core-specific regions for the CPU interfaces. This is
     * necessary for "franken-GIC" implementations, for example on
     * Exynos 4.
     * NB that the memory region size of 0x100 applies for the 11MPCore
     * and also cores following the GIC v1 spec (ie A9).
     * GIC v2 defines a larger memory region (0x1000) so this will need
     * to be extended when we implement A15.
     */
    for (i = 0; i < s->num_cpu; i++) {
        s->backref[i] = s;
        memory_region_init_io(&s->cpuiomem[i + 1], OBJECT(s), &gic_cpu_ops,
                              &s->backref[i], "gic_cpu", 0x100);
        sysbus_init_mmio(sbd, &s->cpuiomem[i + 1]);
    }

    if (s->virt_extn) {
        for (i = 0; i < s->num_cpu; i++) {
            memory_region_init_io(&s->vcpuiomem[i + 1], OBJECT(s),
                                  &gic_vcpu_ops, &s->backref[i],
                                  "gic_vcpu", 0x2000);
            sysbus_init_mmio(sbd, &s->vcpuiomem[i + 1]);
        }
    }

}

static void arm_gic_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ARMGICCommonClass *agcc = ARM_GIC_COMMON_CLASS(klass);
    ARMGICClass *agc = ARM_GIC_CLASS(klass);

    agcc->post_load = arm_gic_post_load;
    device_class_set_parent_realize(dc, arm_gic_realize, &agc->parent_realize);
}

static const TypeInfo arm_gic_info = {
    .name = TYPE_ARM_GIC,
    .parent = TYPE_ARM_GIC_COMMON,
    .instance_size = sizeof(GICState),
    .class_init = arm_gic_class_init,
    .class_size = sizeof(ARMGICClass),
};

static void arm_gic_register_types(void)
{
    type_register_static(&arm_gic_info);
}

type_init(arm_gic_register_types)
