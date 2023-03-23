/*
 * tpm_tis_i2c.c - QEMU's TPM TIS I2C Device
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * Implementation of the TIS interface according to specs found at
 * http://www.trustedcomputinggroup.org. This implementation currently
 * supports version 1.3, 21 March 2013
 * In the developers menu choose the PC Client section then find the TIS
 * specification.
 *
 * TPM TIS for TPM 2 implementation following TCG PC Client Platform
 * TPM Profile (PTP) Specification, Familiy 2.0, Revision 00.43
 *
 * TPM I2C implementation follows TCG TPM I2c Interface specification,
 * Family 2.0, Level 00, Revision 1.00
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/qdev-properties.h"
#include "hw/acpi/tpm.h"
#include "migration/vmstate.h"
#include "tpm_prop.h"
#include "tpm_tis.h"
#include "qom/object.h"
#include "block/aio.h"
#include "qemu/main-loop.h"

/* TPM TIS I2C registers */
#define TPM_TIS_I2C_REG_LOC_SEL          0x00
#define TPM_TIS_I2C_REG_ACCESS           0x04
#define TPM_TIS_I2C_REG_INT_ENABLE       0x08
#define TPM_TIS_I2C_REG_INT_CAPABILITY   0x14
#define TPM_TIS_I2C_REG_STS              0x18
#define TPM_TIS_I2C_REG_DATA_FIFO        0x24
#define TPM_TIS_I2C_REG_INTF_CAPABILITY  0x30
#define TPM_TIS_I2C_REG_I2C_DEV_ADDRESS  0x38
#define TPM_TIS_I2C_REG_DATA_CSUM_ENABLE 0x40
#define TPM_TIS_I2C_REG_DATA_CSUM_GET    0x44
#define TPM_TIS_I2C_REG_DID_VID          0x48
#define TPM_TIS_I2C_REG_RID              0x4c
#define TPM_TIS_I2C_REG_UNKNOWN          0xff

/* I2C specific interface capabilities */
#define TPM_I2C_CAP_INTERFACE_TYPE     (0x2 << 0)       /* FIFO interface */
#define TPM_I2C_CAP_INTERFACE_VER      (0x0 << 4)       /* TCG I2C intf 1.0 */
#define TPM_I2C_CAP_TPM2_FAMILY        (0x1 << 7)       /* TPM 2.0 family. */
#define TPM_I2C_CAP_DEV_ADDR_CHANGE    (0x0 << 27)      /* No dev addr chng */
#define TPM_I2C_CAP_BURST_COUNT_STATIC (0x1 << 29)      /* Burst count static */
#define TPM_I2C_CAP_LOCALITY_CAP       (0x1 << 25)      /* 0-5 locality */
#define TPM_I2C_CAP_BUS_SPEED          (3   << 21)      /* std and fast mode */

/* TPM_STS mask for read bits 31:26 must be zero */
#define TPM_I2C_STS_READ_MASK          0x03ffffff

/* Operations */
#define OP_SEND   1
#define OP_RECV   2

typedef struct TPMStateI2C {
    /*< private >*/
    I2CSlave parent_obj;

    int      offset;     /* offset in to data[] */
    int      size;       /* Size of the current reg data */
    uint8_t  operation;  /* OP_SEND & OP_RECV */
    uint8_t  data[4096]; /* Data */

    bool     csum_enable;
    uint32_t tis_intf_cap;  /* save TIS interface Capabilities */

    /*< public >*/
    TPMState state; /* not a QOM object */

} TPMStateI2C;

DECLARE_INSTANCE_CHECKER(TPMStateI2C, TPM_TIS_I2C,
                         TYPE_TPM_TIS_I2C)

/* Register map */
typedef struct reg_map {
    uint16_t  i2c_reg;    /* I2C register */
    uint16_t  tis_reg;    /* TIS register */
    uint32_t  data_size;  /* data size expected */
} i2c_reg_map;

/*
 * The register values in the common code is different than the latest
 * register numbers as per the spec hence add the conversion map
 */
static const i2c_reg_map tpm_tis_reg_map[] = {
    { TPM_TIS_I2C_REG_LOC_SEL,          TPM_TIS_REG_ACCESS,               1, },
    { TPM_TIS_I2C_REG_ACCESS,           TPM_TIS_REG_ACCESS,               1, },
    { TPM_TIS_I2C_REG_INT_ENABLE,       TPM_TIS_REG_INT_ENABLE,           4, },
    { TPM_TIS_I2C_REG_INT_CAPABILITY,   TPM_TIS_REG_INT_VECTOR,           4, },
    { TPM_TIS_I2C_REG_STS,              TPM_TIS_REG_STS,                  4, },
    { TPM_TIS_I2C_REG_DATA_FIFO,        TPM_TIS_REG_DATA_FIFO,            0, },
    { TPM_TIS_I2C_REG_INTF_CAPABILITY,  TPM_TIS_REG_INTF_CAPABILITY,      4, },
    { TPM_TIS_I2C_REG_I2C_DEV_ADDRESS,  TPM_TIS_I2C_REG_I2C_DEV_ADDRESS,  2, },
    { TPM_TIS_I2C_REG_DATA_CSUM_ENABLE, TPM_TIS_I2C_REG_DATA_CSUM_ENABLE, 1, },
    { TPM_TIS_I2C_REG_DATA_CSUM_GET,    TPM_TIS_I2C_REG_DATA_CSUM_GET,    2, },
    { TPM_TIS_I2C_REG_DID_VID,          TPM_TIS_REG_DID_VID,              4, },
    { TPM_TIS_I2C_REG_RID,              TPM_TIS_REG_RID,                  1, },
};

/* Generate interface capability based on what is returned by TIS and what is
 * expected by I2C. Save the capability in the data array overwriting the TIS
 * capability.
 */
static uint32_t tpm_i2c_interface_capability(TPMStateI2C *i2cst, uint32_t tis_cap)
{
    uint32_t i2c_cap = 0;

    i2cst->tis_intf_cap = tis_cap;

    /* Now generate i2c capability */
    i2c_cap = (TPM_I2C_CAP_INTERFACE_TYPE |
               TPM_I2C_CAP_INTERFACE_VER  |
               TPM_I2C_CAP_TPM2_FAMILY    |
               TPM_I2C_CAP_LOCALITY_CAP   |
               TPM_I2C_CAP_BUS_SPEED      |
               TPM_I2C_CAP_DEV_ADDR_CHANGE);

    /* Now check the TIS and set some capabilities */

    /* Static burst count set */
    if (i2cst->tis_intf_cap & 0x100) {
        i2c_cap |= TPM_I2C_CAP_BURST_COUNT_STATIC;
    }

    return i2c_cap;
}

static inline uint16_t tpm_tis_i2c_to_tis_reg(uint64_t i2c_reg, int *size)
{
    uint16_t tis_reg = i2c_reg;
    const i2c_reg_map *reg_map;
    int i;

    for (i = 0; i < ARRAY_SIZE(tpm_tis_reg_map); i++) {
        reg_map = &tpm_tis_reg_map[i];
        if (reg_map->i2c_reg == (i2c_reg & 0xff)) {
            tis_reg = reg_map->tis_reg;
            *size = reg_map->data_size;
            break;
        }
    }

    return tis_reg;
}

/* Initialize the cached data */
static inline void tpm_tis_i2c_init_cache(TPMStateI2C *i2cst)
{
    /* Clear operation and offset */
    i2cst->operation = 0;
    i2cst->offset = 0;
    i2cst->size = 0;

    return;
}

/* Send data to TPM */
static inline void tpm_tis_i2c_tpm_send(TPMStateI2C *i2cst)
{
    uint16_t tis_reg;
    uint32_t data;
    int      i;

    if ((i2cst->operation == OP_SEND) && (i2cst->offset > 1)) {

        /*
         * Checksum is not handled by common code hence we will consume the
         * register here.
         */
        if (i2cst->data[0] == TPM_TIS_I2C_REG_DATA_CSUM_ENABLE) {
            i2cst->csum_enable = true;
        } else if (i2cst->data[0] != TPM_TIS_I2C_REG_DATA_FIFO) {
            tis_reg = tpm_tis_i2c_to_tis_reg(i2cst->data[0], &i2cst->size);

            /* Index 0 is always a register */
            for (i = 1; i < i2cst->offset; i++) {
                data = i2cst->data[i];
                tpm_tis_write_data(&i2cst->state, tis_reg, data, 1);
            }
        }

        tpm_tis_i2c_init_cache(i2cst);
    }

    return;
}

/* Callback from TPM to indicate that response is copied */
static void tpm_tis_i2c_request_completed(TPMIf *ti, int ret)
{
    TPMStateI2C *i2cst = TPM_TIS_I2C(ti);
    TPMState *s = &i2cst->state;

    /* Inform the common code. */
    tpm_tis_request_completed(s, ret);
}

static enum TPMVersion tpm_tis_i2c_get_tpm_version(TPMIf *ti)
{
    TPMStateI2C *i2cst = TPM_TIS_I2C(ti);
    TPMState *s = &i2cst->state;

    return tpm_tis_get_tpm_version(s);
}

static int tpm_tis_i2c_event(I2CSlave *i2c, enum i2c_event event)
{
    TPMStateI2C *i2cst = TPM_TIS_I2C(i2c);
    int ret = 0;

    switch (event) {
    case I2C_START_RECV:
        break;
    case I2C_START_SEND:
        tpm_tis_i2c_init_cache(i2cst);
        break;
    case I2C_FINISH:
        if (i2cst->operation == OP_SEND) {
            tpm_tis_i2c_tpm_send(i2cst);
        } else {
            tpm_tis_i2c_init_cache(i2cst);
        }
        break;
    default:
        break;
    }

    return ret;
}

/*
 * If data is for FIFO then it is received from tpm_tis_common buffer
 * otherwise it will be handled using single call to common code and
 * cached in the local buffer.
 */
static uint8_t tpm_tis_i2c_recv(I2CSlave *i2c)
{
    int ret = 0;
    int i, j;
    uint32_t addr;
    uint32_t data_read;
    uint16_t i2c_reg;
    TPMStateI2C *i2cst = TPM_TIS_I2C(i2c);
    TPMState *s = &i2cst->state;

    if (i2cst->operation == OP_RECV) {

        /* Do not cache FIFO data. */
        if (i2cst->data[0] == TPM_TIS_I2C_REG_DATA_FIFO) {
            addr = tpm_tis_i2c_to_tis_reg(TPM_TIS_I2C_REG_DATA_FIFO,
                                          &i2cst->size);
            data_read = tpm_tis_read_data(s, addr, 1);
            ret = (data_read & 0xff);
        } else if (i2cst->offset < 4096) {
            ret = i2cst->data[i2cst->offset++];
        }

    } else if ((i2cst->operation == OP_SEND) && (i2cst->offset < 2)) {
        /* First receive call after send */

        i2c_reg = i2cst->data[0];

        i2cst->operation = OP_RECV;
        i2cst->offset = 1; /* keep the register value intact for debug */

        addr = tpm_tis_i2c_to_tis_reg(i2c_reg, &i2cst->size);

        /* FIFO data is directly read from TPM TIS */
        if (i2c_reg == TPM_TIS_I2C_REG_DATA_FIFO) {
            data_read = tpm_tis_read_data(s, addr, 1);
            ret = (data_read & 0xff);
        } else {
            /*
             * Save the data in the data field. Save it in the little
             * endian format.
             */
            for (i = 1; i <= i2cst->size;) {
                /*
                 * Checksum registers are not supported by common code hence
                 * call a common code to get the checksum.
                 */
                if (i2c_reg == TPM_TIS_I2C_REG_DATA_CSUM_GET) {
                    data_read = tpm_tis_get_checksum(s);
                } else {
                    data_read = tpm_tis_read_data(s, addr, 4);

                    if (i2c_reg == TPM_TIS_I2C_REG_INTF_CAPABILITY) {
                        /* Prepare the capabilities as per I2C interface */
                        data_read = tpm_i2c_interface_capability(i2cst,
                                                                 data_read);
                    } else if (i2c_reg == TPM_TIS_I2C_REG_STS) {
                        /*
                         * As per specs, STS bit 31:26 are reserved and must
                         * be set to 0
                         */
                        data_read &= TPM_I2C_STS_READ_MASK;
                    }
                }
                for (j = 0; j < 4; j++) {
                    i2cst->data[i++] = (data_read & 0xff);
                    data_read >>= 8;
                }
            }

            /* Return first byte with this call */
            ret = i2cst->data[i2cst->offset++];
        }
    } else {
        i2cst->operation = OP_RECV;
    }

    return ret;
}

/*
 * Send function only remembers data in the buffer and then calls
 * TPM TIS common code during FINISH event.
 */
static int tpm_tis_i2c_send(I2CSlave *i2c, uint8_t data)
{
    TPMStateI2C *i2cst = TPM_TIS_I2C(i2c);
    uint16_t tis_reg;

    /* Reject non-supported registers. */
    if (i2cst->offset == 0) {
        /* We do not support device address change */
        if (data == TPM_TIS_I2C_REG_I2C_DEV_ADDRESS) {
            return 1;
        }
    }

    if (i2cst->offset < 4096) {
        i2cst->operation = OP_SEND;

        /* Remember data locally for non-FIFO registers */
        if ((i2cst->offset == 0) ||
            (i2cst->data[0] != TPM_TIS_I2C_REG_DATA_FIFO)) {
            i2cst->data[i2cst->offset++] = data;
        } else {
            tis_reg = tpm_tis_i2c_to_tis_reg(i2cst->data[0], &i2cst->size);
            tpm_tis_write_data(&i2cst->state, tis_reg, data, 1);
        }

        return 0;

    } else {
        /* Return non-zero to indicate NAK */
        return 1;
    }
}

static Property tpm_tis_i2c_properties[] = {
    DEFINE_PROP_UINT32("irq", TPMStateI2C, state.irq_num, TPM_TIS_IRQ),
    DEFINE_PROP_TPMBE("tpmdev", TPMStateI2C, state.be_driver),
    DEFINE_PROP_END_OF_LIST(),
};

static void tpm_tis_i2c_realizefn(DeviceState *dev, Error **errp)
{
    TPMStateI2C *i2cst = TPM_TIS_I2C(dev);
    TPMState *s = &i2cst->state;

    if (!tpm_find()) {
        error_setg(errp, "at most one TPM device is permitted");
        return;
    }

    /*
     * Get the backend pointer. It is not initialized propery during
     * device_class_set_props
     */
    s->be_driver = qemu_find_tpm_be("tpm0");

    if (!s->be_driver) {
        error_setg(errp, "'tpmdev' property is required");
        return;
    }
    if (s->irq_num > 15) {
        error_setg(errp, "IRQ %d is outside valid range of 0 to 15",
                   s->irq_num);
        return;
    }
}

static void tpm_tis_i2c_reset(DeviceState *dev)
{
    TPMStateI2C *i2cst = TPM_TIS_I2C(dev);
    TPMState *s = &i2cst->state;

    tpm_tis_i2c_init_cache(i2cst);

    i2cst->csum_enable = false;

    return tpm_tis_reset(s);
}

static void tpm_tis_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    TPMIfClass *tc = TPM_IF_CLASS(klass);

    dc->realize = tpm_tis_i2c_realizefn;
    dc->reset = tpm_tis_i2c_reset;
    device_class_set_props(dc, tpm_tis_i2c_properties);

    k->event = tpm_tis_i2c_event;
    k->recv = tpm_tis_i2c_recv;
    k->send = tpm_tis_i2c_send;

    tc->model = TPM_MODEL_TPM_TIS;
    tc->request_completed = tpm_tis_i2c_request_completed;
    tc->get_version = tpm_tis_i2c_get_tpm_version;
}

static const TypeInfo tpm_tis_i2c_info = {
    .name          = TYPE_TPM_TIS_I2C,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TPMStateI2C),
    .class_init    = tpm_tis_i2c_class_init,
        .interfaces = (InterfaceInfo[]) {
        { TYPE_TPM_IF },
        { }
    }
};

static void tpm_tis_i2c_register_types(void)
{
    type_register_static(&tpm_tis_i2c_info);
}

type_init(tpm_tis_i2c_register_types)
