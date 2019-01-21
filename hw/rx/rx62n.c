/*
 * RX62N device
 *
 * Copyright (c) 2019 Yoshinori Sato
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
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/rx/rx62n.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "sysemu/sysemu.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/address-spaces.h"

static const int ipr_table[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 15 */
    0x00, 0xff, 0xff, 0xff, 0xff, 0x01, 0xff, 0x02,
    0xff, 0xff, 0xff, 0x03, 0x04, 0x05, 0x06, 0x07, /* 31 */
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x14, 0x14, 0x14, /* 47 */
    0x15, 0x15, 0x15, 0x15, 0xff, 0xff, 0xff, 0xff,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x1d, 0x1e, 0x1f, /* 63 */
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, /* 79 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x3a, 0x3b, 0x3c, 0xff, 0xff, 0xff, /* 95 */
    0x40, 0xff, 0x44, 0x45, 0xff, 0xff, 0x48, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 111 */
    0xff, 0xff, 0x51, 0x51, 0x51, 0x51, 0x52, 0x52,
    0x52, 0x53, 0x53, 0x54, 0x54, 0x55, 0x55, 0x56, /* 127 */
    0x56, 0x57, 0x57, 0x57, 0x57, 0x58, 0x59, 0x59,
    0x59, 0x59, 0x5a, 0x5b, 0x5b, 0x5b, 0x5c, 0x5c, /* 143 */
    0x5c, 0x5c, 0x5d, 0x5d, 0x5d, 0x5e, 0x5e, 0x5f,
    0x5f, 0x60, 0x60, 0x61, 0x61, 0x62, 0x62, 0x62, /* 159 */
    0x62, 0x63, 0x64, 0x64, 0x64, 0x64, 0x65, 0x66,
    0x66, 0x66, 0x67, 0x67, 0x67, 0x67, 0x68, 0x68, /* 175 */
    0x68, 0x69, 0x69, 0x69, 0x6a, 0x6a, 0x6a, 0x6b,
    0x6b, 0x6b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 191 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x70, 0x71,
    0x72, 0x73, 0x74, 0x75, 0xff, 0xff, 0xff, 0xff, /* 207 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80, 0x80,
    0x80, 0x80, 0x81, 0x81, 0x81, 0x81, 0x82, 0x82, /* 223 */
    0x82, 0x82, 0x83, 0x83, 0x83, 0x83, 0xff, 0xff,
    0xff, 0xff, 0x85, 0x85, 0x85, 0x85, 0x86, 0x86, /* 239 */
    0x86, 0x86, 0xff, 0xff, 0xff, 0xff, 0x88, 0x89,
    0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, /* 255 */
};

static const uint32_t levelirq[] = {
     16,  21,  32,  44,  47,  48,  51,  64,  65,  66,
     67,  68,  69,  70,  71,  72,  73,  74,  75,  76,
     77,  78,  79,  90,  91, 170, 171, 172, 173, 214,
    217, 218, 221, 222, 225, 226, 229, 234, 237, 238,
    241, 246, 249, 250, 253,
};

static RXICUState *register_icu(RX62NState *s)
{
    SysBusDevice *icu;
    int i;

    icu = SYS_BUS_DEVICE(qdev_create(NULL, TYPE_RXICU));
    sysbus_mmio_map(icu, 0, 0x00087000);
    qdev_prop_set_string(DEVICE(icu), "icutype", "icua");
    qdev_prop_set_uint32(DEVICE(icu), "len-ipr-map", 256);
    for (i = 0; i < 256; i++) {
        char propname[32];
        snprintf(propname, sizeof(propname), "ipr-map[%d]", i);
        qdev_prop_set_uint32(DEVICE(icu), propname, ipr_table[i]);
    }
    qdev_prop_set_uint32(DEVICE(icu), "len-trigger-level", 256);
    for (i = 0; i < ARRAY_SIZE(levelirq); i++) {
        char propname[32];
        snprintf(propname, sizeof(propname), "trigger-level[%d]", i);
        qdev_prop_set_uint32(DEVICE(icu), propname, levelirq[i]);
    }
    for (i = 0; i < 256; i++) {
        s->irq[i] = qdev_get_gpio_in(DEVICE(icu), i);
    }

    qdev_init_nofail(DEVICE(icu));
    sysbus_connect_irq(SYS_BUS_DEVICE(icu), 0,
                       qdev_get_gpio_in(DEVICE(s->cpu), RX_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(icu), 1,
                       qdev_get_gpio_in(DEVICE(s->cpu), RX_CPU_FIR));
    sysbus_connect_irq(SYS_BUS_DEVICE(icu), 2, s->irq[SWI]);

    return RXICU(icu);
}

static RTMRState *register_tmr(RX62NState *s, int unit)
{
    SysBusDevice *tmr;
    int i, irqbase;

    tmr = SYS_BUS_DEVICE(qdev_create(NULL, TYPE_RENESAS_TMR));
    sysbus_mmio_map(tmr, 0, 0x00088200 + unit * 0x10);
    qdev_prop_set_uint64(DEVICE(tmr), "input-freq", 48000000);

    qdev_init_nofail(DEVICE(tmr));
    irqbase = 174 + 6 * unit;
    for (i = 0; i < 6; i++) {
        sysbus_connect_irq(tmr, i, s->irq[irqbase + i]);
    }

    return RTMR(tmr);
}

static RCMTState *register_cmt(RX62NState *s, int unit)
{
    SysBusDevice *cmt;
    int i, irqbase;

    cmt = SYS_BUS_DEVICE(qdev_create(NULL, TYPE_RENESAS_CMT));
    sysbus_mmio_map(cmt, 0, 0x00088000 + unit * 0x10);
    qdev_prop_set_uint64(DEVICE(cmt), "input-freq", 48000000);

    qdev_init_nofail(DEVICE(cmt));
    irqbase = 28 + 2 * unit;
    for (i = 0; i < 1; i++) {
        sysbus_connect_irq(cmt, i, s->irq[irqbase + i]);
    }

    return RCMT(cmt);
}

static RSCIState *register_sci(RX62NState *s, int unit)
{
    SysBusDevice *sci;
    int i, irqbase;

    sci = SYS_BUS_DEVICE(qdev_create(NULL, TYPE_RENESAS_SCI));
    qdev_prop_set_chr(DEVICE(sci), "chardev", serial_hd(unit));
    qdev_prop_set_uint64(DEVICE(sci), "input-freq", 48000000);
    qdev_init_nofail(DEVICE(sci));
    sysbus_mmio_map(sci, 0, 0x00088240 + unit * 0x08);
    irqbase = 214 + 4 * unit;
    for (i = 0; i < 4; i++) {
        sysbus_connect_irq(sci, i, s->irq[irqbase + i]);
    }

    object_property_set_bool(OBJECT(sci), true, "realized", NULL);
    return RSCI(sci);
}

static void rx62n_realize(DeviceState *dev, Error **errp)
{
    RX62NState *s = RX62N(dev);
    Error *err = NULL;

    memory_region_init_ram(&s->iram, NULL, "iram", 0x18000, NULL);
    memory_region_add_subregion(s->sysmem, 0x00000000, &s->iram);
    memory_region_init_rom(&s->d_flash, NULL, "dataflash", 0x8000, NULL);
    memory_region_add_subregion(s->sysmem, 0x00100000, &s->d_flash);
    memory_region_init_rom(&s->c_flash, NULL, "codeflash", 0x80000, NULL);
    memory_region_add_subregion(s->sysmem, 0xfff80000, &s->c_flash);

    s->cpu = RXCPU(object_new(TYPE_RXCPU));

    if (!s->kernel) {
        rom_add_file_fixed(bios_name, 0xfff80000, 0);
    }

    object_property_set_bool(OBJECT(s->cpu), true, "realized", &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }

    s->icu = register_icu(s);
    s->cpu->env.ack = qdev_get_gpio_in_named(DEVICE(s->icu), "ack", 0);
    s->tmr[0] = register_tmr(s, 0);
    s->tmr[1] = register_tmr(s, 1);
    s->cmt[0] = register_cmt(s, 0);
    s->cmt[1] = register_cmt(s, 1);
    s->sci[0] = register_sci(s, 0);
}

static void rx62n_init(Object *obj)
{
}

static Property rx62n_properties[] = {
    DEFINE_PROP_LINK("memory", RX62NState, sysmem, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_BOOL("load-kernel", RX62NState, kernel, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void rx62n_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rx62n_realize;
    dc->props = rx62n_properties;
}

static const TypeInfo rx62n_info = {
    .name = TYPE_RX62N,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX62NState),
    .instance_init = rx62n_init,
    .class_init = rx62n_class_init,
};

static void rx62n_register_types(void)
{
    type_register_static(&rx62n_info);
}

type_init(rx62n_register_types)
