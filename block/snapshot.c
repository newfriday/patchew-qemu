/*
 * Block layer snapshot related functions
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
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
#include "block/snapshot.h"
#include "block/block_int.h"
#include "block/qdict.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qerror.h"
#include "qapi/qmp/qstring.h"
#include "qemu/option.h"
#include "sysemu/block-backend.h"

QemuOptsList internal_snapshot_opts = {
    .name = "snapshot",
    .head = QTAILQ_HEAD_INITIALIZER(internal_snapshot_opts.head),
    .desc = {
        {
            .name = SNAPSHOT_OPT_ID,
            .type = QEMU_OPT_STRING,
            .help = "snapshot id"
        },{
            .name = SNAPSHOT_OPT_NAME,
            .type = QEMU_OPT_STRING,
            .help = "snapshot name"
        },{
            /* end of list */
        }
    },
};

int bdrv_snapshot_find(BlockDriverState *bs, QEMUSnapshotInfo *sn_info,
                       const char *name)
{
    QEMUSnapshotInfo *sn_tab, *sn;
    int nb_sns, i, ret;

    ret = -ENOENT;
    nb_sns = bdrv_snapshot_list(bs, &sn_tab);
    if (nb_sns < 0) {
        return ret;
    }
    for (i = 0; i < nb_sns; i++) {
        sn = &sn_tab[i];
        if (!strcmp(sn->name, name)) {
            *sn_info = *sn;
            ret = 0;
            break;
        }
    }
    g_free(sn_tab);
    return ret;
}

/**
 * Look up an internal snapshot by @id and @name.
 * @bs: block device to search
 * @id: unique snapshot ID, or NULL
 * @name: snapshot name, or NULL
 * @sn_info: location to store information on the snapshot found
 * @errp: location to store error, will be set only for exception
 *
 * This function will traverse snapshot list in @bs to search the matching
 * one, @id and @name are the matching condition:
 * If both @id and @name are specified, find the first one with id @id and
 * name @name.
 * If only @id is specified, find the first one with id @id.
 * If only @name is specified, find the first one with name @name.
 * if none is specified, abort().
 *
 * Returns: true when a snapshot is found and @sn_info will be filled, false
 * when error or not found. If all operation succeed but no matching one is
 * found, @errp will NOT be set.
 */
bool bdrv_snapshot_find_by_id_and_name(BlockDriverState *bs,
                                       const char *id,
                                       const char *name,
                                       QEMUSnapshotInfo *sn_info,
                                       Error **errp)
{
    QEMUSnapshotInfo *sn_tab, *sn;
    int nb_sns, i;
    bool ret = false;

    assert(id || name);

    nb_sns = bdrv_snapshot_list(bs, &sn_tab);
    if (nb_sns < 0) {
        error_setg_errno(errp, -nb_sns, "Failed to get a snapshot list");
        return false;
    } else if (nb_sns == 0) {
        return false;
    }

    if (id && name) {
        for (i = 0; i < nb_sns; i++) {
            sn = &sn_tab[i];
            if (!strcmp(sn->id_str, id) && !strcmp(sn->name, name)) {
                *sn_info = *sn;
                ret = true;
                break;
            }
        }
    } else if (id) {
        for (i = 0; i < nb_sns; i++) {
            sn = &sn_tab[i];
            if (!strcmp(sn->id_str, id)) {
                *sn_info = *sn;
                ret = true;
                break;
            }
        }
    } else if (name) {
        for (i = 0; i < nb_sns; i++) {
            sn = &sn_tab[i];
            if (!strcmp(sn->name, name)) {
                *sn_info = *sn;
                ret = true;
                break;
            }
        }
    }

    g_free(sn_tab);
    return ret;
}

int bdrv_can_snapshot(BlockDriverState *bs)
{
    BlockDriver *drv = bs->drv;
    if (!drv || !bdrv_is_inserted(bs) || bdrv_is_read_only(bs)) {
        return 0;
    }

    if (!drv->bdrv_snapshot_create) {
        if (bs->file != NULL) {
            return bdrv_can_snapshot(bs->file->bs);
        }
        return 0;
    }

    return 1;
}

int bdrv_snapshot_create(BlockDriverState *bs,
                         QEMUSnapshotInfo *sn_info)
{
    BlockDriver *drv = bs->drv;
    if (!drv) {
        return -ENOMEDIUM;
    }
    if (drv->bdrv_snapshot_create) {
        return drv->bdrv_snapshot_create(bs, sn_info);
    }
    if (bs->file) {
        return bdrv_snapshot_create(bs->file->bs, sn_info);
    }
    return -ENOTSUP;
}

int bdrv_snapshot_goto(BlockDriverState *bs,
                       const char *snapshot_id,
                       Error **errp)
{
    BlockDriver *drv = bs->drv;
    int ret, open_ret;

    if (!drv) {
        error_setg(errp, "Block driver is closed");
        return -ENOMEDIUM;
    }

    if (!QLIST_EMPTY(&bs->dirty_bitmaps)) {
        error_setg(errp, "Device has active dirty bitmaps");
        return -EBUSY;
    }

    if (drv->bdrv_snapshot_goto) {
        ret = drv->bdrv_snapshot_goto(bs, snapshot_id);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "Failed to load snapshot");
        }
        return ret;
    }

    if (bs->file) {
        BlockDriverState *file;
        QDict *options = qdict_clone_shallow(bs->options);
        QDict *file_options;
        Error *local_err = NULL;

        file = bs->file->bs;
        /* Prevent it from getting deleted when detached from bs */
        bdrv_ref(file);

        qdict_extract_subqdict(options, &file_options, "file.");
        qobject_unref(file_options);
        qdict_put_str(options, "file", bdrv_get_node_name(file));

        if (drv->bdrv_close) {
            drv->bdrv_close(bs);
        }
        bdrv_unref_child(bs, bs->file);
        bs->file = NULL;

        ret = bdrv_snapshot_goto(file, snapshot_id, errp);
        open_ret = drv->bdrv_open(bs, options, bs->open_flags, &local_err);
        qobject_unref(options);
        if (open_ret < 0) {
            bdrv_unref(file);
            bs->drv = NULL;
            /* A bdrv_snapshot_goto() error takes precedence */
            error_propagate(errp, local_err);
            return ret < 0 ? ret : open_ret;
        }

        assert(bs->file->bs == file);
        bdrv_unref(file);
        return ret;
    }

    error_setg(errp, "Block driver does not support snapshots");
    return -ENOTSUP;
}

/**
 * Delete an internal snapshot by @snapshot_id and @name.
 * @bs: block device used in the operation
 * @snapshot_id: unique snapshot ID, or NULL
 * @name: snapshot name, or NULL
 * @errp: location to store error
 *
 * If both @snapshot_id and @name are specified, delete the first one with
 * id @snapshot_id and name @name.
 * If only @snapshot_id is specified, delete the first one with id
 * @snapshot_id.
 * If only @name is specified, delete the first one with name @name.
 * if none is specified, return -EINVAL.
 *
 * Returns: 0 on success, -errno on failure. If @bs is not inserted, return
 * -ENOMEDIUM. If @snapshot_id and @name are both NULL, return -EINVAL. If @bs
 * does not support internal snapshot deletion, return -ENOTSUP. If @bs does
 * not support parameter @snapshot_id or @name, or one of them is not correctly
 * specified, return -EINVAL. If @bs can't find one matching @id and @name,
 * return -ENOENT. If @errp != NULL, it will always be filled with error
 * message on failure.
 */
int bdrv_snapshot_delete(BlockDriverState *bs,
                         const char *snapshot_id,
                         const char *name,
                         Error **errp)
{
    BlockDriver *drv = bs->drv;
    int ret;

    if (!drv) {
        error_setg(errp, QERR_DEVICE_HAS_NO_MEDIUM, bdrv_get_device_name(bs));
        return -ENOMEDIUM;
    }
    if (!snapshot_id && !name) {
        error_setg(errp, "snapshot_id and name are both NULL");
        return -EINVAL;
    }

    /* drain all pending i/o before deleting snapshot */
    bdrv_drained_begin(bs);

    if (drv->bdrv_snapshot_delete) {
        ret = drv->bdrv_snapshot_delete(bs, snapshot_id, name, errp);
    } else if (bs->file) {
        ret = bdrv_snapshot_delete(bs->file->bs, snapshot_id, name, errp);
    } else {
        error_setg(errp, "Block format '%s' used by device '%s' "
                   "does not support internal snapshot deletion",
                   drv->format_name, bdrv_get_device_name(bs));
        ret = -ENOTSUP;
    }

    bdrv_drained_end(bs);
    return ret;
}

int bdrv_snapshot_list(BlockDriverState *bs,
                       QEMUSnapshotInfo **psn_info)
{
    BlockDriver *drv = bs->drv;
    if (!drv) {
        return -ENOMEDIUM;
    }
    if (drv->bdrv_snapshot_list) {
        return drv->bdrv_snapshot_list(bs, psn_info);
    }
    if (bs->file) {
        return bdrv_snapshot_list(bs->file->bs, psn_info);
    }
    return -ENOTSUP;
}

/**
 * Temporarily load an internal snapshot by @snapshot_id and @name.
 * @bs: block device used in the operation
 * @snapshot_id: unique snapshot ID, or NULL
 * @name: snapshot name, or NULL
 * @errp: location to store error
 *
 * If both @snapshot_id and @name are specified, load the first one with
 * id @snapshot_id and name @name.
 * If only @snapshot_id is specified, load the first one with id
 * @snapshot_id.
 * If only @name is specified, load the first one with name @name.
 * if none is specified, return -EINVAL.
 *
 * Returns: 0 on success, -errno on fail. If @bs is not inserted, return
 * -ENOMEDIUM. If @bs is not readonly, return -EINVAL. If @bs did not support
 * internal snapshot, return -ENOTSUP. If qemu can't find a matching @id and
 * @name, return -ENOENT. If @errp != NULL, it will always be filled on
 * failure.
 */
int bdrv_snapshot_load_tmp(BlockDriverState *bs,
                           const char *snapshot_id,
                           const char *name,
                           Error **errp)
{
    BlockDriver *drv = bs->drv;

    if (!drv) {
        error_setg(errp, QERR_DEVICE_HAS_NO_MEDIUM, bdrv_get_device_name(bs));
        return -ENOMEDIUM;
    }
    if (!snapshot_id && !name) {
        error_setg(errp, "snapshot_id and name are both NULL");
        return -EINVAL;
    }
    if (!bs->read_only) {
        error_setg(errp, "Device is not readonly");
        return -EINVAL;
    }
    if (drv->bdrv_snapshot_load_tmp) {
        return drv->bdrv_snapshot_load_tmp(bs, snapshot_id, name, errp);
    }
    error_setg(errp, "Block format '%s' used by device '%s' "
               "does not support temporarily loading internal snapshots",
               drv->format_name, bdrv_get_device_name(bs));
    return -ENOTSUP;
}

int bdrv_snapshot_load_tmp_by_id_or_name(BlockDriverState *bs,
                                         const char *id_or_name,
                                         Error **errp)
{
    int ret;
    Error *local_err = NULL;

    ret = bdrv_snapshot_load_tmp(bs, id_or_name, NULL, &local_err);
    if (ret == -ENOENT || ret == -EINVAL) {
        error_free(local_err);
        local_err = NULL;
        ret = bdrv_snapshot_load_tmp(bs, NULL, id_or_name, &local_err);
    }

    error_propagate(errp, local_err);

    return ret;
}

static bool bdrv_all_snapshots_includes_bs(BlockDriverState *bs,
                                           strList *devices)
{
    if (devices) {
        const char *node_name = bdrv_get_node_name(bs);
        while (devices) {
            if (g_str_equal(node_name, devices->value)) {
                return true;
            }
            devices = devices->next;
        }
        return false;
    } else {
        if (!bdrv_is_inserted(bs) || bdrv_is_read_only(bs)) {
            return false;
        }

        /* Include all nodes that are either in use by a BlockBackend, or that
         * aren't attached to any node, but owned by the monitor. */
        return bdrv_has_blk(bs) || QLIST_EMPTY(&bs->parents);
    }
}

/* Group operations. All block drivers are involved.
 * These functions will properly handle dataplane (take aio_context_acquire
 * when appropriate for appropriate block drivers) */

bool bdrv_all_can_snapshot(strList *devices, Error **errp)
{
    BlockDriverState *bs;
    BdrvNextIterator it;

    for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
        AioContext *ctx = bdrv_get_aio_context(bs);
        bool ok;

        aio_context_acquire(ctx);
        if (bdrv_all_snapshots_includes_bs(bs, devices)) {
            ok = bdrv_can_snapshot(bs);
        }
        aio_context_release(ctx);
        if (!ok) {
            error_setg(errp, "Device '%s' is writable but does not support "
                       "snapshots", bdrv_get_device_or_node_name(bs));
            bdrv_next_cleanup(&it);
            return false;
        }
    }

    return true;
}

int bdrv_all_delete_snapshot(const char *name, strList *devices, Error **errp)
{
    BlockDriverState *bs;
    BdrvNextIterator it;
    QEMUSnapshotInfo sn1, *snapshot = &sn1;

    for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
        AioContext *ctx = bdrv_get_aio_context(bs);
        int ret;

        aio_context_acquire(ctx);
        if (bdrv_all_snapshots_includes_bs(bs, devices) &&
            bdrv_snapshot_find(bs, snapshot, name) >= 0)
        {
            ret = bdrv_snapshot_delete(bs, snapshot->id_str,
                                       snapshot->name, errp);
        }
        aio_context_release(ctx);
        if (ret < 0) {
            error_prepend(errp, "Could not delete snapshot '%s' on '%s': ",
                          name, bdrv_get_device_or_node_name(bs));
            bdrv_next_cleanup(&it);
            return -1;
        }
    }

    return 0;
}


int bdrv_all_goto_snapshot(const char *name, strList *devices, Error **errp)
{
    BlockDriverState *bs;
    BdrvNextIterator it;

    for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
        AioContext *ctx = bdrv_get_aio_context(bs);
        int ret;

        aio_context_acquire(ctx);
        if (bdrv_all_snapshots_includes_bs(bs, devices)) {
            ret = bdrv_snapshot_goto(bs, name, errp);
        }
        aio_context_release(ctx);
        if (ret < 0) {
            error_prepend(errp, "Could not load snapshot '%s' on '%s': ",
                          name, bdrv_get_device_or_node_name(bs));
            bdrv_next_cleanup(&it);
            return -1;
        }
    }

    return 0;
}

int bdrv_all_find_snapshot(const char *name, strList *devices, Error **errp)
{
    QEMUSnapshotInfo sn;
    BlockDriverState *bs;
    BdrvNextIterator it;

    for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
        AioContext *ctx = bdrv_get_aio_context(bs);
        int ret;

        aio_context_acquire(ctx);
        if (bdrv_all_snapshots_includes_bs(bs, devices)) {
            ret = bdrv_snapshot_find(bs, &sn, name);
        }
        aio_context_release(ctx);
        if (ret < 0) {
            error_setg(errp, "Could not find snapshot '%s' on '%s'",
                       name, bdrv_get_device_or_node_name(bs));
            bdrv_next_cleanup(&it);
            return -1;
        }
    }

    return 0;
}

int bdrv_all_create_snapshot(QEMUSnapshotInfo *sn,
                             BlockDriverState *vm_state_bs,
                             uint64_t vm_state_size,
                             strList *devices,
                             Error **errp)
{
    BlockDriverState *bs;
    BdrvNextIterator it;

    for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
        AioContext *ctx = bdrv_get_aio_context(bs);
        int ret;

        aio_context_acquire(ctx);
        if (bs == vm_state_bs) {
            sn->vm_state_size = vm_state_size;
            ret = bdrv_snapshot_create(bs, sn);
        } else if (bdrv_all_snapshots_includes_bs(bs, devices)) {
            sn->vm_state_size = 0;
            ret = bdrv_snapshot_create(bs, sn);
        }
        aio_context_release(ctx);
        if (ret < 0) {
            error_setg(errp, "Could not create snapshot '%s' on '%s'",
                       sn->name, bdrv_get_device_or_node_name(bs));
            bdrv_next_cleanup(&it);
            return -1;
        }
    }

    return 0;
}

BlockDriverState *bdrv_all_find_vmstate_bs(strList *devices, Error **errp)
{
    BlockDriverState *bs;
    BdrvNextIterator it;

    for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
        AioContext *ctx = bdrv_get_aio_context(bs);
        bool found;

        aio_context_acquire(ctx);
        found = bdrv_all_snapshots_includes_bs(bs, devices) &&
            bdrv_can_snapshot(bs);
        aio_context_release(ctx);

        if (found) {
            bdrv_next_cleanup(&it);
            break;
        }
    }
    if (!bs) {
        error_setg(errp, "No block device supports snapshots");
    }
    return bs;
}
