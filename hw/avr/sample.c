/*
 * QEMU AVR CPU
 *
 * Copyright (c) 2016 Michael Rolnik
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
 * License along with this library; if not, see
 * <http://www.gnu.org/licenses/lgpl-2.1.html>
 */

/*
 *  NOTE:
 *      This is not a real AVR board, this is an example!
 *      The CPU is an approximation of an ATmega2560, but is missing various
 *      built-in peripherals.
 *
 *      This example board loads provided binary file into flash memory and
 *      executes it from 0x00000000 address in the code memory space.
 *
 *      Currently used for AVR CPU validation
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "cpu.h"
#include "hw/hw.h"
#include "sysemu/sysemu.h"
#include "sysemu/qtest.h"
#include "ui/console.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "qemu/error-report.h"
#include "exec/address-spaces.h"
#include "include/hw/sysbus.h"
#include "include/hw/char/avr_usart.h"
#include "include/hw/timer/avr_timer16.h"
#include "elf.h"

#define SIZE_FLASH 0x00040000
#define SIZE_SRAM 0x00002200
/*
 * Size of additional "external" memory, as if the AVR were configured to use
 * an external RAM chip.
 * Note that the configuration registers that normally enable this feature are
 * unimplemented.
 */
#define SIZE_EXMEM 0x00000000

/* Offsets of periphals in emulated memory space (i.e. not host addresses)  */
#define PRR0 0x64
#define PRR1 0x65
#define USART_BASE 0xc0
#define USART_PRR PRR0
#define USART_PRR_MASK 0b00000010
#define TIMER1_BASE 0x80
#define TIMER1_IMSK_BASE 0x6f
#define TIMER1_IFR_BASE 0x36
#define TIMER1_PRR PRR0
#define TIMER1_PRR_MASK 0b01000000

/* Interrupt numbers used by peripherals */
#define TIMER1_CAPT_IRQ 15
#define TIMER1_COMPA_IRQ 16
#define TIMER1_COMPB_IRQ 17
#define TIMER1_COMPC_IRQ 18
#define TIMER1_OVF_IRQ 19

typedef struct {
    MachineClass parent;
} SampleMachineClass;

typedef struct {
    MachineState parent;
    MemoryRegion *ram;
    MemoryRegion *flash;
    AVRUsartState *usart0;
    AVRTimer16State *timer1;
} SampleMachineState;

#define TYPE_SAMPLE_MACHINE MACHINE_TYPE_NAME("sample")

#define SAMPLE_MACHINE(obj) \
    OBJECT_CHECK(SampleMachineState, obj, TYPE_SAMPLE_MACHINE)
#define SAMPLE_MACHINE_GET_CLASS(obj) \
    OBJECT_GET_CLASS(SampleMachineClass, obj, TYPE_SAMPLE_MACHINE)
#define SAMPLE_MACHINE_CLASS(klass) \
    OBJECT_CLASS_CHECK(SampleMachineClass, klass, TYPE_SAMPLE_MACHINE)

static void sample_init(MachineState *machine)
{
    SampleMachineState *sms = SAMPLE_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    AVRCPU *cpu;
    const char *firmware = NULL;
    const char *filename;
    int bytes_loaded;
    SysBusDevice *busdev;
    DeviceState *cpudev;

    system_memory = get_system_memory();
    sms->ram = g_new(MemoryRegion, 1);
    sms->flash = g_new(MemoryRegion, 1);

    cpu = AVR_CPU(cpu_create(machine->cpu_type));

    memory_region_allocate_system_memory(
        sms->ram, NULL, "avr.ram", SIZE_SRAM + SIZE_EXMEM);
    memory_region_add_subregion(system_memory, OFFSET_DATA, sms->ram);

    memory_region_init_rom(sms->flash, NULL, "avr.flash", SIZE_FLASH,
            &error_fatal);
    memory_region_add_subregion(system_memory, OFFSET_CODE, sms->flash);

    /* USART 0 built-in peripheral */
    sms->usart0 = AVR_USART(object_new(TYPE_AVR_USART));
    busdev = SYS_BUS_DEVICE(sms->usart0);
    sysbus_mmio_map(busdev, 0, OFFSET_DATA + USART_BASE);
    /*
     * These IRQ numbers don't match the datasheet because we're counting from
     * zero and not including reset.
     */
    sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(DEVICE(cpu), 24));
    sysbus_connect_irq(busdev, 1, qdev_get_gpio_in(DEVICE(cpu), 25));
    sysbus_connect_irq(busdev, 2, qdev_get_gpio_in(DEVICE(cpu), 26));
    sms->usart0->prr_address = OFFSET_DATA + PRR0;
    sms->usart0->prr_mask = USART_PRR_MASK;
    qdev_prop_set_chr(DEVICE(sms->usart0), "chardev", serial_hd(0));
    object_property_set_bool(OBJECT(sms->usart0), true, "realized",
            &error_fatal);

    /* Timer 1 built-in periphal */
    sms->timer1 = AVR_TIMER16(object_new(TYPE_AVR_TIMER16));
    busdev = SYS_BUS_DEVICE(sms->timer1);
    sysbus_mmio_map(busdev, 0, OFFSET_DATA + TIMER1_BASE);
    sysbus_mmio_map(busdev, 1, OFFSET_DATA + TIMER1_IMSK_BASE);
    sysbus_mmio_map(busdev, 2, OFFSET_DATA + TIMER1_IFR_BASE);
    cpudev = DEVICE(cpu);
    sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(cpudev, TIMER1_CAPT_IRQ));
    sysbus_connect_irq(busdev, 1, qdev_get_gpio_in(cpudev, TIMER1_COMPA_IRQ));
    sysbus_connect_irq(busdev, 2, qdev_get_gpio_in(cpudev, TIMER1_COMPB_IRQ));
    sysbus_connect_irq(busdev, 3, qdev_get_gpio_in(cpudev, TIMER1_COMPC_IRQ));
    sysbus_connect_irq(busdev, 4, qdev_get_gpio_in(cpudev, TIMER1_OVF_IRQ));
    sms->timer1->prr_address = OFFSET_DATA + TIMER1_PRR;
    sms->timer1->prr_mask = TIMER1_PRR_MASK;
    object_property_set_bool(OBJECT(sms->timer1), true, "realized",
            &error_fatal);

    /* Load firmware (contents of flash) trying to auto-detect format */
    firmware = machine->firmware;
    if (firmware != NULL) {
        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, firmware);
        if (filename == NULL) {
            error_report("Unable to find %s", firmware);
            exit(1);
        }

        bytes_loaded = load_elf(
            filename, NULL, NULL, NULL, NULL, NULL, NULL, 0, EM_NONE, 0, 0);
        if (bytes_loaded < 0) {
            error_report(
                "Unable to load %s as ELF, trying again as raw binary",
                firmware);
            bytes_loaded = load_image_targphys(
                filename, OFFSET_CODE, SIZE_FLASH);
        }
        if (bytes_loaded < 0) {
            error_report(
                "Unable to load firmware image %s as ELF or raw binary",
                firmware);
            exit(1);
        }
    }
}

static void sample_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "AVR sample/example board (ATmega2560)";
    mc->init = sample_init;
    mc->default_cpus = 1;
    mc->min_cpus = mc->default_cpus;
    mc->max_cpus = mc->default_cpus;
    mc->default_cpu_type = AVR_CPU_TYPE_NAME("avr6"); /* ATmega2560. */
    mc->is_default = 1;
}

static const TypeInfo sample_info = {
    .name = TYPE_SAMPLE_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(SampleMachineState),
    .class_size = sizeof(SampleMachineClass),
    .class_init = sample_class_init,
};

static void sample_machine_init(void)
{
    type_register_static(&sample_info);
}

type_init(sample_machine_init);
