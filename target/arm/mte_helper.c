/*
 * ARM v8.5-MemTag Operations
 *
 * Copyright (c) 2019 Linaro, Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "internals.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "exec/helper-proto.h"


static int get_allocation_tag(CPUARMState *env, uint64_t ptr, uintptr_t ra)
{
    /* Tag storage not implemented.  */
    return -1;
}

static int allocation_tag_from_addr(uint64_t ptr)
{
    ptr += 1ULL << 55;  /* carry ptr[55] into ptr[59:56].  */
    return extract64(ptr, 56, 4);
}

/*
 * Perform a checked access for MTE.
 * On arrival, TBI is known to enabled, as is allocation_tag_access_enabled.
 */
static uint64_t do_mte_check(CPUARMState *env, uint64_t dirty_ptr,
                             uint64_t clean_ptr, uint32_t select,
                             uintptr_t ra)
{
    ARMMMUIdx stage1 = arm_stage1_mmu_idx(env);
    int ptr_tag, mem_tag;

    /*
     * If TCMA is enabled, then physical tag 0 is unchecked.
     * Note the rules in D6.8.1 are written with logical tags, where
     * the corresponding physical tag rule is simpler: equal to 0.
     * We will need the physical tag below anyway.
     */
    ptr_tag = allocation_tag_from_addr(dirty_ptr);
    if (ptr_tag == 0) {
        ARMVAParameters p = aa64_va_parameters(env, dirty_ptr, stage1, true);
        if (p.tcma) {
            return clean_ptr;
        }
    }

    /*
     * If an access is made to an address that does not provide tag
     * storage, the result is IMPLEMENTATION DEFINED.  We choose to
     * treat the access as unchecked.
     * This is similar to MemAttr != Tagged, which are also unchecked.
     */
    mem_tag = get_allocation_tag(env, clean_ptr, ra);
    if (mem_tag < 0) {
        return clean_ptr;
    }

    /* If the tags do not match, the tag check operation fails.  */
    if (unlikely(ptr_tag != mem_tag)) {
        int el, regime_el, tcf;
        uint64_t sctlr;

        el = arm_current_el(env);
        regime_el = (el ? el : 1);   /* TODO: ARMv8.1-VHE EL2&0 regime */
        sctlr = env->cp15.sctlr_el[regime_el];
        if (el == 0) {
            tcf = extract64(sctlr, 38, 2);
        } else {
            tcf = extract64(sctlr, 40, 2);
        }

        switch (tcf) {
        case 1:
            /*
             * Tag check fail causes a synchronous exception.
             *
             * In restore_state_to_opc, we set the exception syndrome
             * for the load or store operation.  Do that first so we
             * may overwrite that with the syndrome for the tag check.
             */
            cpu_restore_state(env_cpu(env), ra, true);
            env->exception.vaddress = dirty_ptr;
            raise_exception(env, EXCP_DATA_ABORT,
                            syn_data_abort_no_iss(el != 0, 0, 0, 0, 0, 0x11),
                            exception_target_el(env));
            /* noreturn; fall through to assert anyway */

        case 0:
            /*
             * Tag check fail does not affect the PE.
             * We eliminate this case by not setting MTE_ACTIVE
             * in tb_flags, so that we never make this runtime call.
             */
            g_assert_not_reached();

        case 2:
            /* Tag check fail causes asynchronous flag set.  */
            env->cp15.tfsr_el[regime_el] |= 1 << select;
            break;

        default:
            /* Case 3: Reserved. */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Tag check failure with SCTLR_EL%d.TCF "
                          "set to reserved value %d\n", regime_el, tcf);
            break;
        }
    }

    return clean_ptr;
}

/*
 * Perform check in translation regime w/single IA range.
 * It is known that TBI is enabled on entry.
 */
uint64_t HELPER(mte_check1)(CPUARMState *env, uint64_t dirty_ptr)
{
    uint64_t clean_ptr = extract64(dirty_ptr, 0, 56);
    return do_mte_check(env, dirty_ptr, clean_ptr, 0, GETPC());
}

/*
 * Perform check in translation regime w/two IA ranges.
 * It is known that TBI is enabled on entry.
 */
uint64_t HELPER(mte_check2)(CPUARMState *env, uint64_t dirty_ptr)
{
    uint32_t select = extract64(dirty_ptr, 55, 1);
    uint64_t clean_ptr = sextract64(dirty_ptr, 0, 56);
    return do_mte_check(env, dirty_ptr, clean_ptr, select, GETPC());
}

/*
 * Perform check in translation regime w/two IA ranges.
 * The TBI argument is the concatenation of TBI1:TBI0.
 */
uint64_t HELPER(mte_check3)(CPUARMState *env, uint64_t dirty_ptr, uint32_t tbi)
{
    uint32_t select = extract64(dirty_ptr, 55, 1);
    uint64_t clean_ptr = sextract64(dirty_ptr, 0, 56);

    if ((tbi >> select) & 1) {
        return do_mte_check(env, dirty_ptr, clean_ptr, select, GETPC());
    } else {
        /* TBI is disabled; the access is unchecked.  */
        return dirty_ptr;
    }
}
