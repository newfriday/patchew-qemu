/*
 * windbgstub-utils.h
 *
 * Copyright (c) 2010-2017 Institute for System Programming
 *                         of the Russian Academy of Sciences.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef WINDBGSTUB_UTILS_H
#define WINDBGSTUB_UTILS_H

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "exec/windbgstub.h"
#include "exec/windbgkd.h"

#ifndef TARGET_I386
#error Unsupported Architecture
#endif
#ifdef TARGET_X86_64 /* Unimplemented yet */
#error Unsupported Architecture
#endif

#if (WINDBG_DEBUG_ON)

# define WINDBG_DEBUG(...) do {    \
    printf("Debug: " __VA_ARGS__); \
    printf("\n");                  \
} while (false)

# define WINDBG_ERROR(...) do {    \
    printf("Error: " __VA_ARGS__); \
    printf("\n");                  \
} while (false)

#else

# define WINDBG_DEBUG(...)
# define WINDBG_ERROR(...) error_report(WINDBG ": " __VA_ARGS__)

#endif

#define FMT_ADDR "addr:0x" TARGET_FMT_lx
#define FMT_ERR  "Error:%d"

#define UINT8_P(ptr) ((uint8_t *) (ptr))
#define UINT32_P(ptr) ((uint32_t *) (ptr))
#define FIELD_P(type, field, ptr) ((typeof_field(type, field) *) (ptr))
#define PTR(var) UINT8_P(&var)

#define M64_SIZE sizeof(DBGKD_MANIPULATE_STATE64)

#define sizeof_field(type, field) sizeof(((type *) NULL)->field)

#define READ_VMEM(cpu, addr, type) ({                         \
    type _t;                                                  \
    cpu_memory_rw_debug(cpu, addr, PTR(_t), sizeof(type), 0); \
    _t;                                                       \
})

bool windbg_on_load(void);
void windbg_on_exit(void);

#endif
