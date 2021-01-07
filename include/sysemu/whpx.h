/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX) support
 *
 * Copyright Microsoft, Corp. 2017
 *
 * Authors:
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_WHPX_H
#define QEMU_WHPX_H

#ifdef CONFIG_WHPX

#include <WinHvPlatformDefs.h>

struct whpx_state {
    uint64_t mem_quota;
    WHV_PARTITION_HANDLE partition;
    bool kernel_irqchip_allowed;
    bool kernel_irqchip_required;
    bool apic_in_platform;
};

extern struct whpx_state whpx_global;
int whpx_enabled(void);

void whpx_apic_get(DeviceState *s);
#define whpx_apic_in_platform() (whpx_global.apic_in_platform)

#else /* CONFIG_WHPX */

#define whpx_enabled() (0)
#define whpx_apic_in_platform() (0)

#endif /* CONFIG_WHPX */

#endif /* QEMU_WHPX_H */
