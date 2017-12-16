/*
 * QEMU PIIX4 PCI Bridge Emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2016 Hervé Poussineau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/isa/isa.h"
#include "hw/char/isa.h"
#include "hw/sysbus.h"
#include "hw/audio/pcspk.h"
#include "hw/timer/i8254.h"
#include "hw/timer/mc146818rtc.h"
#include "qapi/error.h"

PCIDevice *piix4_dev;

typedef struct PIIX4State {
    PCIDevice dev;
    qemu_irq cpu_intr;
    qemu_irq *isa;

    FDCtrlISABus floppy;
    ISASerialState serial[2];
    ISAParallelState parallel;
    RTCState rtc;

    /* Reset Control Register */
    MemoryRegion rcr_mem;
    uint8_t rcr;
} PIIX4State;

#define TYPE_PIIX4_PCI_DEVICE "piix4-isa"
#define PIIX4_PCI_DEVICE(obj) \
    OBJECT_CHECK(PIIX4State, (obj), TYPE_PIIX4_PCI_DEVICE)

static void piix4_reset(void *opaque)
{
    PIIX4State *d = opaque;
    uint8_t *pci_conf = d->dev.config;

    pci_conf[0x04] = 0x07; // master, memory and I/O
    pci_conf[0x05] = 0x00;
    pci_conf[0x06] = 0x00;
    pci_conf[0x07] = 0x02; // PCI_status_devsel_medium
    pci_conf[0x4c] = 0x4d;
    pci_conf[0x4e] = 0x03;
    pci_conf[0x4f] = 0x00;
    pci_conf[0x60] = 0x0a; // PCI A -> IRQ 10
    pci_conf[0x61] = 0x0a; // PCI B -> IRQ 10
    pci_conf[0x62] = 0x0b; // PCI C -> IRQ 11
    pci_conf[0x63] = 0x0b; // PCI D -> IRQ 11
    pci_conf[0x69] = 0x02;
    pci_conf[0x70] = 0x80;
    pci_conf[0x76] = 0x0c;
    pci_conf[0x77] = 0x0c;
    pci_conf[0x78] = 0x02;
    pci_conf[0x79] = 0x00;
    pci_conf[0x80] = 0x00;
    pci_conf[0x82] = 0x00;
    pci_conf[0xa0] = 0x08;
    pci_conf[0xa2] = 0x00;
    pci_conf[0xa3] = 0x00;
    pci_conf[0xa4] = 0x00;
    pci_conf[0xa5] = 0x00;
    pci_conf[0xa6] = 0x00;
    pci_conf[0xa7] = 0x00;
    pci_conf[0xa8] = 0x0f;
    pci_conf[0xaa] = 0x00;
    pci_conf[0xab] = 0x00;
    pci_conf[0xac] = 0x00;
    pci_conf[0xae] = 0x00;
}

static const VMStateDescription vmstate_piix4 = {
    .name = "PIIX4",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, PIIX4State),
        VMSTATE_END_OF_LIST()
    }
};

static void piix4_request_i8259_irq(void *opaque, int irq, int level)
{
    PIIX4State *s = opaque;
    qemu_set_irq(s->cpu_intr, level);
}

static void piix4_set_i8259_irq(void *opaque, int irq, int level)
{
    PIIX4State *s = opaque;
    qemu_set_irq(s->isa[irq], level);
}

static void piix4_rcr_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned int len)
{
    PIIX4State *s = opaque;

    if (val & 4) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }
    s->rcr = val & 2; /* keep System Reset type only */
}

static uint64_t piix4_rcr_read(void *opaque, hwaddr addr, unsigned int len)
{
    PIIX4State *s = opaque;
    return s->rcr;
}

static const MemoryRegionOps piix4_rcr_ops = {
    .read = piix4_rcr_read,
    .write = piix4_rcr_write,
    .endianness = DEVICE_LITTLE_ENDIAN
};

static void piix4_realize(PCIDevice *pci, Error **errp)
{
    DeviceState *dev = DEVICE(pci);
    PIIX4State *s = DO_UPCAST(PIIX4State, dev, pci);
    ISABus *isa_bus;
    ISADevice *pit;
    qemu_irq *i8259_out_irq;
    int i;
    Error *err = NULL;

    isa_bus = isa_bus_new(dev, pci_address_space(pci),
                          pci_address_space_io(pci), errp);
    if (!isa_bus) {
        return;
    }

    qdev_init_gpio_in_named(dev, piix4_set_i8259_irq, "isa", ISA_NUM_IRQS);
    qdev_init_gpio_out_named(dev, &s->cpu_intr, "intr", 1);

    memory_region_init_io(&s->rcr_mem, OBJECT(dev), &piix4_rcr_ops, s,
                          "reset-control", 1);
    memory_region_add_subregion_overlap(pci_address_space_io(pci), 0xcf9,
                                        &s->rcr_mem, 1);

    /* initialize i8259 pic */
    i8259_out_irq = qemu_allocate_irqs(piix4_request_i8259_irq, s, 1);
    s->isa = i8259_init(isa_bus, *i8259_out_irq);

    /* initialize ISA irqs */
    isa_bus_irqs(isa_bus, s->isa);

    /* initialize pit */
    pit = pit_init(isa_bus, 0x40, 0, NULL);

    /* speaker */
    pcspk_init(isa_bus, pit);

    /* DMA */
    DMA_init(isa_bus, 0);

    /* Super I/O */
    isa_create_simple(isa_bus, "i8042");

    /* floppy */
    qdev_set_parent_bus(DEVICE(&s->floppy), BUS(isa_bus));
    object_property_set_bool(OBJECT(&s->floppy), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    /* serial ports */
    for (i = 0; i < 2; i++) {
        qdev_set_parent_bus(DEVICE(&s->serial[i]), BUS(isa_bus));
        if (!qemu_chr_fe_backend_connected(&s->serial[i].state.chr)) {
            char prop[] = "serial?";
            char label[] = "piix4.serial?";
            prop[6] = i + '0';
            label[12] = i + '0';
            qdev_prop_set_chr(dev, prop, qemu_chr_new(label, "null"));
        }
        object_property_set_bool(OBJECT(&s->serial[i]), true, "realized", &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
    }

    /* parallel port */
    qdev_set_parent_bus(DEVICE(&s->parallel), BUS(isa_bus));
    if (!qemu_chr_fe_backend_connected(&s->parallel.state.chr)) {
        qdev_prop_set_chr(dev, "parallel",
                          qemu_chr_new("pii4x.parallel", "null"));
    }
    object_property_set_bool(OBJECT(&s->parallel), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    /* timer */
    qdev_set_parent_bus(DEVICE(&s->rtc), BUS(isa_bus));
    object_property_set_bool(OBJECT(&s->rtc), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    isa_init_irq(ISA_DEVICE(&s->rtc), &s->rtc.irq, RTC_ISA_IRQ);

    piix4_dev = pci;
    qemu_register_reset(piix4_reset, s);
}

static void piix4_init(Object *obj)
{
    PIIX4State *s = PIIX4_PCI_DEVICE(obj);
    int i;

    object_initialize(&s->floppy, sizeof(s->floppy), TYPE_ISA_FDC);
    for (i = 0; i < 2; i++) {
        object_initialize(&s->serial[i], sizeof(s->serial[i]), TYPE_ISA_SERIAL);
    }
    object_initialize(&s->parallel, sizeof(s->parallel), TYPE_ISA_PARALLEL);
    object_initialize(&s->rtc, sizeof(s->rtc), TYPE_MC146818_RTC);

    object_property_add_alias(obj, "floppy", OBJECT(&s->floppy), "driveA",
                              &error_abort);
    object_property_add_alias(obj, "serial0", OBJECT(&s->serial[0]), "chardev",
                              &error_abort);
    object_property_add_alias(obj, "serial1", OBJECT(&s->serial[1]), "chardev",
                              &error_abort);
    object_property_add_alias(obj, "parallel", OBJECT(&s->parallel), "chardev",
                              &error_abort);
}

static void piix4_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = piix4_realize;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_82371AB_0;
    k->class_id = PCI_CLASS_BRIDGE_ISA;
    dc->desc = "ISA bridge";
    dc->vmsd = &vmstate_piix4;
}

static const TypeInfo piix4_info = {
    .name          = TYPE_PIIX4_PCI_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PIIX4State),
    .instance_init = piix4_init,
    .class_init    = piix4_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void piix4_register_types(void)
{
    type_register_static(&piix4_info);
}

type_init(piix4_register_types)
