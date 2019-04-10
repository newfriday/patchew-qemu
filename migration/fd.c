/*
 * QEMU live migration via generic fd
 *
 * Copyright Red Hat, Inc. 2009-2016
 *
 * Authors:
 *  Chris Lalancette <clalance@redhat.com>
 *  Daniel P. Berrange <berrange@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "channel.h"
#include "fd.h"
#include "migration.h"
#include "monitor/monitor.h"
#include "io/channel-util.h"
#include "trace.h"


void fd_start_outgoing_migration(MigrationState *s, const char *fdname, Error **errp)
{
    QIOChannel *ioc;
    int fd, dup_fd;

    fd = monitor_get_fd(cur_mon, fdname, errp);
    if (fd == -1) {
        return;
    }

    /* fd is previously created by qmp command 'getfd',
     * so client is responsible to close it. Dup it to save original value from
     * QIOChannel's destructor */
    dup_fd = qemu_dup(fd);
    if (dup_fd == -1) {
        error_setg(errp, "Cannot dup fd %s: %s (%d)", fdname, strerror(errno),
                   errno);
        return;
    }

    trace_migration_fd_outgoing(fd, dup_fd);
    ioc = qio_channel_new_fd(dup_fd, errp);
    if (!ioc) {
        close(dup_fd);
        return;
    }

    qio_channel_set_name(QIO_CHANNEL(ioc), "migration-fd-outgoing");
    migration_channel_connect(s, ioc, NULL, NULL);
    object_unref(OBJECT(ioc));
}

static gboolean fd_accept_incoming_migration(QIOChannel *ioc,
                                             GIOCondition condition,
                                             gpointer opaque)
{
    migration_channel_process_incoming(ioc);
    object_unref(OBJECT(ioc));
    return G_SOURCE_REMOVE;
}

void fd_start_incoming_migration(const char *infd, Error **errp)
{
    QIOChannel *ioc;
    int fd, dup_fd;

    fd = strtol(infd, NULL, 0);

    /* fd is previously created by qmp command 'add-fd' or something else,
     * so client is responsible to close it. Dup it to save original value from
     * QIOChannel's destructor */
    dup_fd = qemu_dup(fd);
    if (dup_fd == -1) {
        error_setg(errp, "Cannot dup fd %d: %s (%d)", fd, strerror(errno),
                   errno);
        return;
    }

    trace_migration_fd_incoming(fd, dup_fd);
    ioc = qio_channel_new_fd(dup_fd, errp);
    if (!ioc) {
        close(dup_fd);
        return;
    }

    qio_channel_set_name(QIO_CHANNEL(ioc), "migration-fd-incoming");
    qio_channel_add_watch_full(ioc, G_IO_IN,
                               fd_accept_incoming_migration,
                               NULL, NULL,
                               g_main_context_get_thread_default());
}
