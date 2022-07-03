/*
 * ARM cpu parameters for qemu.
 *
 * Copyright (c) 2003 Fabrice Bellard
 * SPDX-License-Identifier: LGPL-2.0+
 */

#ifndef ARM_CPU_PARAM_H
#define ARM_CPU_PARAM_H

#ifdef TARGET_AARCH64
# define TARGET_LONG_BITS             64
# define TARGET_PHYS_ADDR_SPACE_BITS  52
# define TARGET_VIRT_ADDR_SPACE_BITS  52
#else
# define TARGET_LONG_BITS             32
# define TARGET_PHYS_ADDR_SPACE_BITS  40
# define TARGET_VIRT_ADDR_SPACE_BITS  32
#endif

#ifdef CONFIG_USER_ONLY
#define TARGET_PAGE_BITS 12
# ifdef TARGET_AARCH64
#  define TARGET_TAGGED_ADDRESSES
# endif
#else
/*
 * ARMv7 and later CPUs have 4K pages minimum, but ARMv5 and v6
 * have to support 1K tiny pages.
 */
# define TARGET_PAGE_BITS_VARY
# define TARGET_PAGE_BITS_MIN  10
/*
 * Extra information stored in softmmu page tables.
 */
# define TARGET_PAGE_ENTRY_EXTRA
struct PageEntryExtra {
    /* See PAGEENTRYEXTRA fields in cpu.h */
    uint64_t x;
};
#endif

#define NB_MMU_MODES 8

#endif
