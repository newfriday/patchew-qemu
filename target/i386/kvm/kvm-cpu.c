/*
 * x86 KVM CPU type initialization
 *
 * Copyright 2020 SUSE LLC
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "host-cpu.h"
#include "kvm-cpu.h"
#include "qapi/error.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"

#include "kvm_i386.h"
#include "hw/core/accel-cpu.h"

static void kvm_cpu_realizefn(CPUState *cs, Error **errp)
{
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;

    /*
     * The realize order is important, since x86_cpu_realize() checks if
     * nothing else has been set by the user (or by accelerators) in
     * cpu->ucode_rev and cpu->phys_bits.
     *
     * realize order:
     * kvm_cpu -> host_cpu -> x86_cpu
     */
    if (cpu->max_features) {
        if (enable_cpu_pm && kvm_has_waitpkg()) {
            env->features[FEAT_7_0_ECX] |= CPUID_7_0_ECX_WAITPKG;
        }
        if (cpu->ucode_rev == 0) {
            cpu->ucode_rev =
                kvm_arch_get_supported_msr_feature(kvm_state,
                                                   MSR_IA32_UCODE_REV);
        }
    }
    host_cpu_realizefn(cs, errp);
}

/*
 * KVM-specific features that are automatically added/removed
 * from all CPU models when KVM is enabled.
 */
static PropValue kvm_default_props[] = {
    { "kvmclock", "on" },
    { "kvm-nopiodelay", "on" },
    { "kvm-asyncpf", "on" },
    { "kvm-steal-time", "on" },
    { "kvm-pv-eoi", "on" },
    { "kvmclock-stable-bit", "on" },
    { "x2apic", "on" },
    { "acpi", "off" },
    { "monitor", "off" },
    { "svm", "off" },
    { NULL, NULL },
};

void x86_cpu_change_kvm_default(const char *prop, const char *value)
{
    PropValue *pv;
    for (pv = kvm_default_props; pv->prop; pv++) {
        if (!strcmp(pv->prop, prop)) {
            pv->value = value;
            break;
        }
    }

    /*
     * It is valid to call this function only for properties that
     * are already present in the kvm_default_props table.
     */
    assert(pv->prop);
}

static bool lmce_supported(void)
{
    uint64_t mce_cap = 0;

    if (kvm_ioctl(kvm_state, KVM_X86_GET_MCE_CAP_SUPPORTED, &mce_cap) < 0) {
        return false;
    }
    return !!(mce_cap & MCG_LMCE_P);
}

static void kvm_cpu_max_instance_init(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;
    KVMState *s = kvm_state;

    host_cpu_max_instance_init(cpu);

    if (lmce_supported()) {
        object_property_set_bool(OBJECT(cpu), "lmce", true, &error_abort);
    }

    env->cpuid_min_level =
        kvm_arch_get_supported_cpuid(s, 0x0, 0, R_EAX);
    env->cpuid_min_xlevel =
        kvm_arch_get_supported_cpuid(s, 0x80000000, 0, R_EAX);
    env->cpuid_min_xlevel2 =
        kvm_arch_get_supported_cpuid(s, 0xC0000000, 0, R_EAX);
}

static void kvm_cpu_instance_init(CPUState *cs)
{
    X86CPU *cpu = X86_CPU(cs);

    host_cpu_instance_init(cpu);

    if (!kvm_irqchip_in_kernel()) {
        x86_cpu_change_kvm_default("x2apic", "off");
    }

    /* Special cases not set in the X86CPUDefinition structs: */

    x86_cpu_apply_props(cpu, kvm_default_props);

    if (cpu->max_features) {
        kvm_cpu_max_instance_init(cpu);
    }
}

static void kvm_cpu_accel_class_init(ObjectClass *oc, void *data)
{
    AccelCPUClass *acc = ACCEL_CPU_CLASS(oc);

    acc->cpu_realizefn = kvm_cpu_realizefn;
    acc->cpu_instance_init = kvm_cpu_instance_init;
}
static const TypeInfo kvm_cpu_accel_type_info = {
    .name = ACCEL_CPU_NAME("kvm"),

    .parent = TYPE_ACCEL_CPU,
    .class_init = kvm_cpu_accel_class_init,
    .abstract = true,
};
static void kvm_cpu_accel_register_types(void)
{
    type_register_static(&kvm_cpu_accel_type_info);
}
type_init(kvm_cpu_accel_register_types);
