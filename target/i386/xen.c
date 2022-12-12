/*
 * Xen HVM emulation support in KVM
 *
 * Copyright © 2019 Oracle and/or its affiliates. All rights reserved.
 * Copyright © 2022 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "kvm/kvm_i386.h"
#include "exec/address-spaces.h"
#include "xen.h"
#include "trace.h"
#include "hw/i386/kvm/xen_overlay.h"
#include "standard-headers/xen/version.h"
#include "standard-headers/xen/memory.h"
#include "standard-headers/xen/hvm/hvm_op.h"
#include "standard-headers/xen/vcpu.h"

static int kvm_gva_rw(CPUState *cs, uint64_t gva, void *_buf, size_t sz,
                      bool is_write)
{
    uint8_t *buf = (uint8_t *)_buf;
    size_t i = 0, len = 0;
    int ret;

    for (i = 0; i < sz; i+= len) {
        struct kvm_translation tr = {
            .linear_address = gva + i,
        };

        len = TARGET_PAGE_SIZE - (tr.linear_address & ~TARGET_PAGE_MASK);
        if (len > sz)
            len = sz;

        ret = kvm_vcpu_ioctl(cs, KVM_TRANSLATE, &tr);
        if (ret || !tr.valid || (is_write && !tr.writeable)) {
            return -EFAULT;
        }

        cpu_physical_memory_rw(tr.physical_address, buf + i, len, is_write);
    }

    return 0;
}

static inline int kvm_copy_from_gva(CPUState *cs, uint64_t gva, void *buf,
                                    size_t sz)
{
    return kvm_gva_rw(cs, gva, buf, sz, false);
}

static inline int kvm_copy_to_gva(CPUState *cs, uint64_t gva, void *buf,
                                  size_t sz)
{
    return kvm_gva_rw(cs, gva, buf, sz, false);
}

int kvm_xen_init(KVMState *s, uint32_t xen_version)
{
    const int required_caps = KVM_XEN_HVM_CONFIG_HYPERCALL_MSR |
        KVM_XEN_HVM_CONFIG_INTERCEPT_HCALL | KVM_XEN_HVM_CONFIG_SHARED_INFO;
    struct kvm_xen_hvm_config cfg = {
        .msr = XEN_HYPERCALL_MSR,
        .flags = KVM_XEN_HVM_CONFIG_INTERCEPT_HCALL,
    };
    int xen_caps, ret;

    xen_caps = kvm_check_extension(s, KVM_CAP_XEN_HVM);
    if (required_caps & ~xen_caps) {
        error_report("kvm: Xen HVM guest support not present or insufficient");
        return -ENOSYS;
    }

    if (xen_caps & KVM_XEN_HVM_CONFIG_EVTCHN_SEND) {
        struct kvm_xen_hvm_attr ha = {
            .type = KVM_XEN_ATTR_TYPE_XEN_VERSION,
            .u.xen_version = xen_version,
        };
        (void)kvm_vm_ioctl(s, KVM_XEN_HVM_SET_ATTR, &ha);

        cfg.flags |= KVM_XEN_HVM_CONFIG_EVTCHN_SEND;
    }

    ret = kvm_vm_ioctl(s, KVM_XEN_HVM_CONFIG, &cfg);
    if (ret < 0) {
        error_report("kvm: Failed to enable Xen HVM support: %s", strerror(-ret));
        return ret;
    }

    return 0;
}

static bool kvm_xen_hcall_xen_version(struct kvm_xen_exit *exit, X86CPU *cpu,
                                     int cmd, uint64_t arg)
{
    int err = 0;

    switch (cmd) {
    case XENVER_get_features: {
        struct xen_feature_info fi;

        err = kvm_copy_from_gva(CPU(cpu), arg, &fi, sizeof(fi));
        if (err) {
            break;
        }

        fi.submap = 0;
        if (fi.submap_idx == 0) {
            fi.submap |= 1 << XENFEAT_writable_page_tables |
                         1 << XENFEAT_writable_descriptor_tables |
                         1 << XENFEAT_auto_translated_physmap |
                         1 << XENFEAT_supervisor_mode_kernel;
        }

        err = kvm_copy_to_gva(CPU(cpu), arg, &fi, sizeof(fi));
        break;
    }

    default:
            return false;
    }

    exit->u.hcall.result = err;
    return true;
}

int kvm_xen_set_vcpu_attr(CPUState *cs, uint16_t type, uint64_t gpa)
{
    struct kvm_xen_vcpu_attr xhsi;

    xhsi.type = type;
    xhsi.u.gpa = gpa;

    trace_kvm_xen_set_vcpu_attr(cs->cpu_index, type, gpa);

    return kvm_vcpu_ioctl(cs, KVM_XEN_VCPU_SET_ATTR, &xhsi);
}

static void do_set_vcpu_info_default_gpa(CPUState *cs, run_on_cpu_data data)
{
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;

    env->xen_vcpu_info_default_gpa = data.host_ulong;

    /* Changing the default does nothing if a vcpu_info was explicitly set. */
    if (env->xen_vcpu_info_gpa == UINT64_MAX) {
            kvm_xen_set_vcpu_attr(cs, KVM_XEN_VCPU_ATTR_TYPE_VCPU_INFO,
                                  env->xen_vcpu_info_default_gpa);
    }
}

static void do_set_vcpu_info_gpa(CPUState *cs, run_on_cpu_data data)
{
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;

    env->xen_vcpu_info_gpa = data.host_ulong;

    kvm_xen_set_vcpu_attr(cs, KVM_XEN_VCPU_ATTR_TYPE_VCPU_INFO,
                          env->xen_vcpu_info_gpa);
}

static int xen_set_shared_info(CPUState *cs, uint64_t gfn)
{
    uint64_t gpa = gfn << TARGET_PAGE_BITS;
    int i, err;

    /* The xen_overlay device tells KVM about it too, since it had to
     * do that on migration load anyway (unless we're going to jump
     * through lots of hoops to maintain the fiction that this isn't
     * KVM-specific */
    err = xen_overlay_map_page(XENMAPSPACE_shared_info, 0, gpa);
    if (err)
            return err;

    trace_kvm_xen_set_shared_info(gfn);

    for (i = 0; i < XEN_LEGACY_MAX_VCPUS; i++) {
        CPUState *cpu = qemu_get_cpu(i);
        if (cpu) {
                async_run_on_cpu(cpu, do_set_vcpu_info_default_gpa, RUN_ON_CPU_HOST_ULONG(gpa));
        }
        gpa += sizeof(vcpu_info_t);
    }

    return err;
}

static bool kvm_xen_hcall_memory_op(struct kvm_xen_exit *exit,
                                   int cmd, uint64_t arg, X86CPU *cpu)
{
    CPUState *cs = CPU(cpu);
    int err = 0;

    switch (cmd) {
    case XENMEM_add_to_physmap: {
            struct xen_add_to_physmap xatp;

            err = kvm_copy_from_gva(cs, arg, &xatp, sizeof(xatp));
            if (err) {
                break;
            }

            switch (xatp.space) {
            case XENMAPSPACE_shared_info:
                break;
            default:
                err = -ENOSYS;
                break;
            }

            err = xen_set_shared_info(cs, xatp.gpfn);
            break;
         }

    default:
            return false;
    }

    exit->u.hcall.result = err;
    return true;
}

static bool kvm_xen_hcall_hvm_op(struct kvm_xen_exit *exit,
                                 int cmd, uint64_t arg)
{
    switch (cmd) {
    case HVMOP_pagetable_dying:
            exit->u.hcall.result = -ENOSYS;
            return true;

    default:
            return false;
    }
}

static int vcpuop_register_vcpu_info(CPUState *cs, CPUState *target,
                                     uint64_t arg)
{
    struct vcpu_register_vcpu_info rvi;
    uint64_t gpa;

    if (!target)
            return -ENOENT;

    if (kvm_copy_from_gva(cs, arg, &rvi, sizeof(rvi))) {
        return -EFAULT;
    }

    gpa = ((rvi.mfn << TARGET_PAGE_BITS) + rvi.offset);
    async_run_on_cpu(target, do_set_vcpu_info_gpa, RUN_ON_CPU_HOST_ULONG(gpa));
    return 0;
}

static bool kvm_xen_hcall_vcpu_op(struct kvm_xen_exit *exit, X86CPU *cpu,
                                  int cmd, int vcpu_id, uint64_t arg)
{
    CPUState *dest = qemu_get_cpu(vcpu_id);
    CPUState *cs = CPU(cpu);
    int err;

    switch (cmd) {
    case VCPUOP_register_vcpu_info:
            err = vcpuop_register_vcpu_info(cs, dest, arg);
            break;

    default:
            return false;
    }

    exit->u.hcall.result = err;
    return true;
}

static bool __kvm_xen_handle_exit(X86CPU *cpu, struct kvm_xen_exit *exit)
{
    uint16_t code = exit->u.hcall.input;

    if (exit->u.hcall.cpl > 0) {
        exit->u.hcall.result = -EPERM;
        return true;
    }

    switch (code) {
    case __HYPERVISOR_vcpu_op:
        return kvm_xen_hcall_vcpu_op(exit, cpu,
                                     exit->u.hcall.params[0],
                                     exit->u.hcall.params[1],
                                     exit->u.hcall.params[2]);
    case __HYPERVISOR_hvm_op:
        return kvm_xen_hcall_hvm_op(exit, exit->u.hcall.params[0],
                                    exit->u.hcall.params[1]);
    case __HYPERVISOR_memory_op:
        return kvm_xen_hcall_memory_op(exit, exit->u.hcall.params[0],
                                       exit->u.hcall.params[1], cpu);
    case __HYPERVISOR_xen_version:
        return kvm_xen_hcall_xen_version(exit, cpu, exit->u.hcall.params[0],
                                         exit->u.hcall.params[1]);
    default:
        return false;
    }
}

int kvm_xen_handle_exit(X86CPU *cpu, struct kvm_xen_exit *exit)
{
    if (exit->type != KVM_EXIT_XEN_HCALL)
        return -1;

    if (!__kvm_xen_handle_exit(cpu, exit)) {
        /* Some hypercalls will be deliberately "implemented" by returning
         * -ENOSYS. This case is for hypercalls which are unexpected. */
        exit->u.hcall.result = -ENOSYS;
        qemu_log_mask(LOG_GUEST_ERROR, "Unimplemented Xen hypercall %"
                      PRId64 " (0x%" PRIx64 " 0x%" PRIx64 " 0x%" PRIx64 ")\n",
                      (uint64_t)exit->u.hcall.input, (uint64_t)exit->u.hcall.params[0],
                      (uint64_t)exit->u.hcall.params[1], (uint64_t)exit->u.hcall.params[1]);
    }

    trace_kvm_xen_hypercall(CPU(cpu)->cpu_index, exit->u.hcall.cpl,
                            exit->u.hcall.input, exit->u.hcall.params[0],
                            exit->u.hcall.params[1], exit->u.hcall.params[2],
                            exit->u.hcall.result);
    return 0;
}
