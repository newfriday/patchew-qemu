/*
 * QEMU Crypto Device Implementation
 *
 * Copyright (c) 2016 HUAWEI TECHNOLOGIES CO., LTD.
 *
 * Authors:
 *    Gonglei <arei.gonglei@huawei.com>
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
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "sysemu/cryptodev.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-cryptodev.h"
#include "qapi/visitor.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "qom/object_interfaces.h"
#include "hw/virtio/virtio-crypto.h"


static QTAILQ_HEAD(, CryptoDevBackendClient) crypto_clients;

static int qmp_query_cryptodev_foreach(Object *obj, void *data)
{
    CryptoDevBackend *backend;
    CryptodevInfoList **infolist = data;
    uint32_t services;

    if (!object_dynamic_cast(obj, TYPE_CRYPTODEV_BACKEND)) {
        return 0;
    }

    CryptodevInfo *info = g_new0(CryptodevInfo, 1);
    info->id = g_strdup(object_get_canonical_path_component(obj));

    backend = CRYPTODEV_BACKEND(obj);
    if (backend->sym_stat) {
        info->has_sym_stat = true;
        info->sym_stat = g_memdup2(backend->sym_stat,
                                sizeof(QCryptodevBackendSymStat));
    }

    if (backend->asym_stat) {
        info->has_asym_stat = true;
        info->asym_stat = g_memdup2(backend->asym_stat,
                                sizeof(QCryptodevBackendAsymStat));
    }

    services = backend->conf.crypto_services;
    for (uint32_t i = 0; i < QCRYPTODEV_BACKEND_SERVICE__MAX; i++) {
        if (services & (1 << i)) {
            QAPI_LIST_PREPEND(info->service, i);
        }
    }

    for (uint32_t i = 0; i < backend->conf.peers.queues; i++) {
        CryptoDevBackendClient *cc = backend->conf.peers.ccs[i];
        CryptodevBackendClient *client = g_new0(CryptodevBackendClient, 1);

        client->queue = cc->queue_index;
        client->type = cc->type;
        if (cc->info_str) {
            client->has_info = true;
            client->info = strdup(cc->info_str);
        }
        QAPI_LIST_PREPEND(info->client, client);
    }

    QAPI_LIST_PREPEND(*infolist, info);

    return 0;
}

CryptodevInfoList *qmp_query_cryptodev(Error **errp)
{
    CryptodevInfoList *list = NULL;
    Object *objs = container_get(object_get_root(), "/objects");

    object_child_foreach(objs, qmp_query_cryptodev_foreach, &list);

    return list;
}

CryptoDevBackendClient *cryptodev_backend_new_client(void)
{
    CryptoDevBackendClient *cc;

    cc = g_new0(CryptoDevBackendClient, 1);
    QTAILQ_INSERT_TAIL(&crypto_clients, cc, next);

    return cc;
}

void cryptodev_backend_free_client(
                  CryptoDevBackendClient *cc)
{
    QTAILQ_REMOVE(&crypto_clients, cc, next);
    g_free(cc->info_str);
    g_free(cc);
}

void cryptodev_backend_cleanup(
             CryptoDevBackend *backend,
             Error **errp)
{
    CryptoDevBackendClass *bc =
                  CRYPTODEV_BACKEND_GET_CLASS(backend);

    if (bc->cleanup) {
        bc->cleanup(backend, errp);
    }

    g_free(backend->sym_stat);
    g_free(backend->asym_stat);
}

int cryptodev_backend_create_session(
           CryptoDevBackend *backend,
           CryptoDevBackendSessionInfo *sess_info,
           uint32_t queue_index,
           CryptoDevCompletionFunc cb,
           void *opaque)
{
    CryptoDevBackendClass *bc =
                      CRYPTODEV_BACKEND_GET_CLASS(backend);

    if (bc->create_session) {
        return bc->create_session(backend, sess_info, queue_index, cb, opaque);
    }
    return -VIRTIO_CRYPTO_NOTSUPP;
}

int cryptodev_backend_close_session(
           CryptoDevBackend *backend,
           uint64_t session_id,
           uint32_t queue_index,
           CryptoDevCompletionFunc cb,
           void *opaque)
{
    CryptoDevBackendClass *bc =
                      CRYPTODEV_BACKEND_GET_CLASS(backend);

    if (bc->close_session) {
        return bc->close_session(backend, session_id, queue_index, cb, opaque);
    }
    return -VIRTIO_CRYPTO_NOTSUPP;
}

static int cryptodev_backend_operation(
                 CryptoDevBackend *backend,
                 CryptoDevBackendOpInfo *op_info,
                 uint32_t queue_index,
                 CryptoDevCompletionFunc cb,
                 void *opaque)
{
    CryptoDevBackendClass *bc =
                      CRYPTODEV_BACKEND_GET_CLASS(backend);

    if (bc->do_op) {
        return bc->do_op(backend, op_info, queue_index, cb, opaque);
    }
    return -VIRTIO_CRYPTO_NOTSUPP;
}

static int cryptodev_backend_account(CryptoDevBackend *backend,
                 CryptoDevBackendOpInfo *op_info)
{
    enum QCryptodevBackendAlgType algtype = op_info->algtype;
    int len;

    if (algtype == QCRYPTODEV_BACKEND_ALG_ASYM) {
        CryptoDevBackendAsymOpInfo *asym_op_info = op_info->u.asym_op_info;
        len = asym_op_info->src_len;
        switch (op_info->op_code) {
        case VIRTIO_CRYPTO_AKCIPHER_ENCRYPT:
            QCryptodevAsymStatIncEncrypt(backend, len);
            break;
        case VIRTIO_CRYPTO_AKCIPHER_DECRYPT:
            QCryptodevAsymStatIncDecrypt(backend, len);
            break;
        case VIRTIO_CRYPTO_AKCIPHER_SIGN:
            QCryptodevAsymStatIncSign(backend, len);
            break;
        case VIRTIO_CRYPTO_AKCIPHER_VERIFY:
            QCryptodevAsymStatIncVerify(backend, len);
            break;
        default:
            return -VIRTIO_CRYPTO_NOTSUPP;
        }
    } else if (algtype == QCRYPTODEV_BACKEND_ALG_SYM) {
        CryptoDevBackendSymOpInfo *sym_op_info = op_info->u.sym_op_info;
        len = sym_op_info->src_len;
        switch (op_info->op_code) {
        case VIRTIO_CRYPTO_CIPHER_ENCRYPT:
            QCryptodevSymStatIncEncrypt(backend, len);
            break;
        case VIRTIO_CRYPTO_CIPHER_DECRYPT:
            QCryptodevSymStatIncDecrypt(backend, len);
            break;
        default:
            return -VIRTIO_CRYPTO_NOTSUPP;
        }
    } else {
        error_report("Unsupported cryptodev alg type: %" PRIu32 "", algtype);
        return -VIRTIO_CRYPTO_NOTSUPP;
    }

    return len;
}

int cryptodev_backend_crypto_operation(
                 CryptoDevBackend *backend,
                 void *opaque1,
                 uint32_t queue_index,
                 CryptoDevCompletionFunc cb, void *opaque2)
{
    VirtIOCryptoReq *req = opaque1;
    CryptoDevBackendOpInfo *op_info = &req->op_info;
    int ret;

    ret = cryptodev_backend_account(backend, op_info);
    if (ret < 0) {
        return ret;
    }
    return cryptodev_backend_operation(backend, op_info, queue_index,
                                       cb, opaque2);
}

static void
cryptodev_backend_get_queues(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    CryptoDevBackend *backend = CRYPTODEV_BACKEND(obj);
    uint32_t value = backend->conf.peers.queues;

    visit_type_uint32(v, name, &value, errp);
}

static void
cryptodev_backend_set_queues(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    CryptoDevBackend *backend = CRYPTODEV_BACKEND(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!value) {
        error_setg(errp, "Property '%s.%s' doesn't take value '%" PRIu32 "'",
                   object_get_typename(obj), name, value);
        return;
    }
    backend->conf.peers.queues = value;
}

static void
cryptodev_backend_complete(UserCreatable *uc, Error **errp)
{
    CryptoDevBackend *backend = CRYPTODEV_BACKEND(uc);
    CryptoDevBackendClass *bc = CRYPTODEV_BACKEND_GET_CLASS(uc);
    uint32_t services;

    if (bc->init) {
        bc->init(backend, errp);
    }

    services = backend->conf.crypto_services;
    if (services & (1 << QCRYPTODEV_BACKEND_SERVICE_CIPHER)) {
        backend->sym_stat = g_new0(QCryptodevBackendSymStat, 1);
    }

    if (services & (1 << QCRYPTODEV_BACKEND_SERVICE_AKCIPHER)) {
        backend->asym_stat = g_new0(QCryptodevBackendAsymStat, 1);
    }
}

void cryptodev_backend_set_used(CryptoDevBackend *backend, bool used)
{
    backend->is_used = used;
}

bool cryptodev_backend_is_used(CryptoDevBackend *backend)
{
    return backend->is_used;
}

void cryptodev_backend_set_ready(CryptoDevBackend *backend, bool ready)
{
    backend->ready = ready;
}

bool cryptodev_backend_is_ready(CryptoDevBackend *backend)
{
    return backend->ready;
}

static bool
cryptodev_backend_can_be_deleted(UserCreatable *uc)
{
    return !cryptodev_backend_is_used(CRYPTODEV_BACKEND(uc));
}

static void cryptodev_backend_instance_init(Object *obj)
{
    /* Initialize devices' queues property to 1 */
    object_property_set_int(obj, "queues", 1, NULL);
}

static void cryptodev_backend_finalize(Object *obj)
{
    CryptoDevBackend *backend = CRYPTODEV_BACKEND(obj);

    cryptodev_backend_cleanup(backend, NULL);
}

static void
cryptodev_backend_class_init(ObjectClass *oc, void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);

    ucc->complete = cryptodev_backend_complete;
    ucc->can_be_deleted = cryptodev_backend_can_be_deleted;

    QTAILQ_INIT(&crypto_clients);
    object_class_property_add(oc, "queues", "uint32",
                              cryptodev_backend_get_queues,
                              cryptodev_backend_set_queues,
                              NULL, NULL);
}

static const TypeInfo cryptodev_backend_info = {
    .name = TYPE_CRYPTODEV_BACKEND,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CryptoDevBackend),
    .instance_init = cryptodev_backend_instance_init,
    .instance_finalize = cryptodev_backend_finalize,
    .class_size = sizeof(CryptoDevBackendClass),
    .class_init = cryptodev_backend_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { }
    }
};

static void
cryptodev_backend_register_types(void)
{
    type_register_static(&cryptodev_backend_info);
}

type_init(cryptodev_backend_register_types);
