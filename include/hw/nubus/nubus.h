/*
 * Copyright (c) 2013-2018 Laurent Vivier <laurent@vivier.eu>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef HW_NUBUS_NUBUS_H
#define HW_NUBUS_NUBUS_H

#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "qom/object.h"
#include "qemu/units.h"

#define NUBUS_SUPER_SLOT_SIZE 0x10000000U
#define NUBUS_SUPER_SLOT_NB   0x9

#define NUBUS_SLOT_SIZE       0x01000000
#define NUBUS_SLOT_NB         0xF

#define NUBUS_FIRST_SLOT      0x0
#define NUBUS_LAST_SLOT       0xF

#define TYPE_NUBUS_DEVICE "nubus-device"
OBJECT_DECLARE_SIMPLE_TYPE(NubusDevice, NUBUS_DEVICE)

#define TYPE_NUBUS_BUS "nubus-bus"
OBJECT_DECLARE_SIMPLE_TYPE(NubusBus, NUBUS_BUS)

#define TYPE_NUBUS_BRIDGE "nubus-bridge"

struct NubusBus {
    BusState qbus;

    MemoryRegion super_slot_io;
    MemoryRegion slot_io;

    uint32_t slot_available_mask;
};

#define NUBUS_DECL_ROM_MAX_SIZE    (128 * KiB)

struct NubusDevice {
    DeviceState qdev;

    int32_t slot;
    MemoryRegion super_slot_mem;
    MemoryRegion slot_mem;

    char *romfile;
    MemoryRegion decl_rom;
};

#endif
