/*
 * Adjunct Processor (AP) matrix device interfaces
 *
 * Copyright 2018 IBM Corp.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#ifndef HW_S390X_AP_DEVICE_H
#define HW_S390X_AP_DEVICE_H

#include "hw/qdev-core.h"
#include "qom/object.h"

#define AP_DEVICE_TYPE       "ap-device"

struct APDevice {
    DeviceState parent_obj;
};
typedef struct APDevice APDevice;

#define AP_DEVICE(obj) \
    OBJECT_CHECK(APDevice, (obj), AP_DEVICE_TYPE)

#endif /* HW_S390X_AP_DEVICE_H */
