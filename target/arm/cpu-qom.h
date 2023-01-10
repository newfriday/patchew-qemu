/*
 * QEMU ARM CPU
 *
 * Copyright (c) 2012 SUSE LINUX Products GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see
 * <http://www.gnu.org/licenses/gpl-2.0.html>
 */
#ifndef QEMU_ARM_CPU_QOM_H
#define QEMU_ARM_CPU_QOM_H

#include "hw/core/cpu.h"
#include "hw/arm/cpu.h"
#include "qom/object.h"

struct arm_boot_info;

typedef struct ARMCPUInfo {
    const char *name;
    void (*initfn)(Object *obj);
    void (*class_init)(ObjectClass *oc, void *data);
} ARMCPUInfo;

void arm_cpu_register(const ARMCPUInfo *info);
void aarch64_cpu_register(const ARMCPUInfo *info);

/**
 * ARMCPUClass:
 * @parent_realize: The parent class' realize handler.
 * @parent_phases: The parent class' reset phase handlers.
 *
 * An ARM CPU model.
 */
struct ARMCPUClass {
    /*< private >*/
    CPUClass parent_class;
    /*< public >*/

    const ARMCPUInfo *info;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

struct AArch64CPUClass {
    /*< private >*/
    ARMCPUClass parent_class;
    /*< public >*/
};

void register_cp_regs_for_features(ARMCPU *cpu);
void init_cpreg_list(ARMCPU *cpu);

/* Callback functions for the generic timer's timers. */
void arm_gt_ptimer_cb(void *opaque);
void arm_gt_vtimer_cb(void *opaque);
void arm_gt_htimer_cb(void *opaque);
void arm_gt_stimer_cb(void *opaque);
void arm_gt_hvtimer_cb(void *opaque);

#endif
