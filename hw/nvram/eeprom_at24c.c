/*
 * *AT24C* series I2C EEPROM
 *
 * Copyright (c) 2015 Michael Davidsaver
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the LICENSE file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/i2c/i2c.h"
#include "sysemu/block-backend.h"
#include "qemu/error-report.h"

/* #define DEBUG_AT24C */

#ifdef DEBUG_AT24C
#define DPRINTK(FMT, ...) printf(TYPE_AT24C_EE " : " FMT, ## __VA_ARGS__)
#else
#define DPRINTK(FMT, ...) do {} while (0)
#endif

#define TYPE_AT24C_EE "at24c-eeprom"
#define AT24C_EE(obj) OBJECT_CHECK(EEPROMState, (obj), TYPE_AT24C_EE)

/* default is the size of a 24c01 EEPROM */
#define AT24C_ROMSIZE_DEFAULT 128

typedef struct EEPROMState {
    I2CSlave parent_obj;

    /* address counter */
    uint16_t cur;
    /* total size in bytes */
    uint32_t rsize;
    bool writable;
    /* cells changed since last START? */
    bool changed;
    /* during WRITE, # of address bytes transfered */
    uint8_t haveaddr;

    uint8_t *mem;

    BlockBackend *blk;
} EEPROMState;

static
int at24c_eeprom_event(I2CSlave *s, enum i2c_event event)
{
    EEPROMState *ee = container_of(s, EEPROMState, parent_obj);

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
    case I2C_FINISH:
        ee->haveaddr = 0;
        DPRINTK("clear\n");
        if (ee->blk && ee->changed) {
            int len = blk_pwrite(ee->blk, 0, ee->mem, ee->rsize, 0);
            if (len != ee->rsize) {
                error_report("failed to write backing file");
            }
            DPRINTK("Wrote to backing file\n");
        }
        ee->changed = false;
        break;
    case I2C_NACK:
        break;
    }
    return 0;
}

static
int at24c_eeprom_recv(I2CSlave *s)
{
    EEPROMState *ee = AT24C_EE(s);
    int ret;

    ret = ee->mem[ee->cur];

    ee->cur = (ee->cur + 1u) % ee->rsize;
    DPRINTK("Recv %02x %c\n", ret, ret);

    return ret;
}

static
int at24c_eeprom_send(I2CSlave *s, uint8_t data)
{
    EEPROMState *ee = AT24C_EE(s);

    if (ee->haveaddr < 2) {
        ee->cur <<= 8;
        ee->cur |= data;
        ee->haveaddr++;
        if (ee->haveaddr == 2) {
            ee->cur %= ee->rsize;
            DPRINTK("Set pointer %04x\n", ee->cur);
        }

    } else {
        if (ee->writable) {
            DPRINTK("Send %02x\n", data);
            ee->mem[ee->cur] = data;
            ee->changed = true;
        } else {
            DPRINTK("Send error %02x read-only\n", data);
        }
        ee->cur = (ee->cur + 1u) % ee->rsize;

    }

    return 0;
}

static
int at24c_eeprom_init(I2CSlave *i2c)
{
    EEPROMState *ee = AT24C_EE(i2c);

    if (!ee->rsize) {
        error_report("rom-size not allowed to be 0");
        exit(1);
    }

    ee->mem = g_malloc0(ee->rsize);

    if (ee->blk) {
        int64_t len = blk_getlength(ee->blk);

        if (len != ee->rsize) {
            error_report("Backing file size %lu != %u",
                    (unsigned long)len, (unsigned)ee->rsize);
            exit(1);
        }

        if (blk_set_perm(ee->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, &error_fatal) < 0)
        {
            error_report("Backing file incorrect permission");
            exit(1);
        }
    }
    return 0;
}

static
void at24c_eeprom_reset(DeviceState *state)
{
    EEPROMState *ee = AT24C_EE(state);

    ee->changed = false;
    ee->cur = 0;
    ee->haveaddr = 0;

    memset(ee->mem, 0, ee->rsize);

    if (ee->blk) {
        int len = blk_pread(ee->blk, 0, ee->mem, ee->rsize);

        if (len != ee->rsize) {
            error_report("Failed initial sync with backing file");
        }
        DPRINTK("Reset read backing file\n");
    }
}

static Property at24c_eeprom_props[] = {
    DEFINE_PROP_UINT32("rom-size", EEPROMState, rsize, AT24C_ROMSIZE_DEFAULT),
    DEFINE_PROP_BOOL("writable", EEPROMState, writable, true),
    DEFINE_PROP_DRIVE("drive", EEPROMState, blk),
    DEFINE_PROP_END_OF_LIST()
};

static
void at24c_eeprom_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->init = &at24c_eeprom_init;
    k->event = &at24c_eeprom_event;
    k->recv = &at24c_eeprom_recv;
    k->send = &at24c_eeprom_send;

    dc->props = at24c_eeprom_props;
    dc->reset = at24c_eeprom_reset;
}

static
const TypeInfo at24c_eeprom_type = {
    .name = TYPE_AT24C_EE,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(EEPROMState),
    .class_size = sizeof(I2CSlaveClass),
    .class_init = at24c_eeprom_class_init,
};

static void at24c_eeprom_register(void)
{
    type_register_static(&at24c_eeprom_type);
}

type_init(at24c_eeprom_register)
