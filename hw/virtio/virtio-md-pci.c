/*
 * Abstract virtio based memory device
 *
 * Copyright (C) 2023 Red Hat, Inc.
 *
 * Authors:
 *  David Hildenbrand <david@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio-md-pci.h"
#include "hw/mem/memory-device.h"
#include "qapi/error.h"

void virtio_md_pci_pre_plug(VirtIOMDPCI *vmd, MachineState *ms, Error **errp)
{
    DeviceState *dev = DEVICE(vmd);
    HotplugHandler *bus_handler = qdev_get_bus_hotplug_handler(dev);
    MemoryDeviceState *md = MEMORY_DEVICE(vmd);
    Error *local_err = NULL;

    if (!bus_handler && dev->hotplugged) {
        /*
         * Without a bus hotplug handler, we cannot control the plug/unplug
         * order. We should never reach this point when hotplugging on x86,
         * however, better add a safety net.
         */
        error_setg(errp, "hotplug of virtio based memory devices not supported"
                   " on this bus.");
        return;
    }
    /*
     * First, see if we can plug this memory device at all. If that
     * succeeds, branch of to the actual hotplug handler.
     */
    memory_device_pre_plug(md, ms, NULL, &local_err);
    if (!local_err && bus_handler) {
        hotplug_handler_pre_plug(bus_handler, dev, &local_err);
    }
    error_propagate(errp, local_err);
}

void virtio_md_pci_plug(VirtIOMDPCI *vmd, MachineState *ms, Error **errp)
{
    DeviceState *dev = DEVICE(vmd);
    HotplugHandler *bus_handler = qdev_get_bus_hotplug_handler(dev);
    MemoryDeviceState *md = MEMORY_DEVICE(vmd);
    Error *local_err = NULL;

    /*
     * Plug the memory device first and then branch off to the actual
     * hotplug handler. If that one fails, we can easily undo the memory
     * device bits.
     */
    memory_device_plug(md, ms);
    if (bus_handler) {
        hotplug_handler_plug(bus_handler, dev, &local_err);
        if (local_err) {
            memory_device_unplug(md, ms);
        }
    }
    error_propagate(errp, local_err);
}

void virtio_md_pci_unplug_request(VirtIOMDPCI *vmd, MachineState *ms,
                                  Error **errp)
{
    /* We don't support hot unplug of virtio based memory devices */
    error_setg(errp, "virtio based memory devices cannot be unplugged.");
}

void virtio_md_pci_unplug(VirtIOMDPCI *vmd, MachineState *ms, Error **errp)
{
    /* We don't support hot unplug of virtio based memory devices */
}

static const TypeInfo virtio_md_pci_info = {
    .name = TYPE_VIRTIO_MD_PCI,
    .parent = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtIOMDPCI),
    .class_size = sizeof(VirtIOMDPCIClass),
    .abstract = true,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_MEMORY_DEVICE },
        { }
    },
};

static void virtio_md_pci_register(void)
{
    type_register_static(&virtio_md_pci_info);
}
type_init(virtio_md_pci_register)
