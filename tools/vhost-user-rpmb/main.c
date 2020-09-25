/*
 * VIRTIO RPMB Emulation via vhost-user
 *
 * Copyright (c) 2020 Linaro Ltd
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "contrib/libvhost-user/libvhost-user-glib.h"
#include "contrib/libvhost-user/libvhost-user.h"

#ifndef container_of
#define container_of(ptr, type, member) ({                      \
        const typeof(((type *) 0)->member) *__mptr = (ptr);     \
        (type *) ((char *) __mptr - offsetof(type, member));})
#endif

static gchar *socket_path;
static gint socket_fd = -1;
static gboolean print_cap;

static GOptionEntry options[] =
{
    { "socket-path", 0, 0, G_OPTION_ARG_FILENAME, &socket_path, "Location of vhost-user Unix domain socket, incompatible with --fd", "PATH" },
    { "fd", 0, 0, G_OPTION_ARG_INT, &socket_fd, "Specify the file-descriptor of the backend, incompatible with --socket-path", "FD" },
    { "print-capabilities", 0, 0, G_OPTION_ARG_NONE, &print_cap, "Output to stdout the backend capabilities in JSON format and exit", NULL},
    { NULL }
};

enum {
    VHOST_USER_RPMB_MAX_QUEUES = 1,
};

/* These structures are defined in the specification */

struct virtio_rpmb_config {
    uint8_t capacity;
    uint8_t max_wr_cnt;
    uint8_t max_rd_cnt;
};

struct virtio_rpmb_frame {
    uint8_t stuff[196];
    uint8_t key_mac[32];
    uint8_t data[256];
    uint8_t nonce[16];
    /* remaining fields are big-endian */
    uint32_t write_counter;
    uint16_t address;
    uint16_t block_count;
    uint16_t result;
    uint16_t req_resp;
};

/*
 * Structure to track internal state of RPMB Device
 */

typedef struct VuRpmb {
    VugDev dev;
    struct virtio_rpmb_config virtio_config;
} VuRpmb;

struct virtio_rpmb_ctrl_command {
    VuVirtqElement elem;
    VuVirtq *vq;
    struct virtio_rpmb_frame frame;
    uint32_t error;
    bool finished;
};

static void vrpmb_panic(VuDev *dev, const char *msg)
{
    g_critical("%s\n", msg);
    exit(EXIT_FAILURE);
}

static uint64_t vrpmb_get_features(VuDev *dev)
{
    return 0;
}

static void vrpmb_set_features(VuDev *dev, uint64_t features)
{
    if (features) {
        g_autoptr(GString) s = g_string_new("Requested un-handled feature");
        g_string_append_printf(s, " 0x%" PRIx64 "", features);
        g_info("%s: %s", __func__, s->str);
    }
}

/*
 * The configuration of the device is static and set when we start the
 * daemon.
 */
static int
vrpmb_get_config(VuDev *dev, uint8_t *config, uint32_t len)
{
    VuRpmb *r = container_of(dev, VuRpmb, dev.parent);
    g_return_val_if_fail(len <= sizeof(struct virtio_rpmb_config), -1);
    memcpy(config, &r->virtio_config, len);
    return 0;
}

static int
vrpmb_set_config(VuDev *dev, const uint8_t *data,
                 uint32_t offset, uint32_t size,
                 uint32_t flags)
{
    /* ignore */
    return 0;
}

static void
vrpmb_handle_ctrl(VuDev *dev, int qidx)
{
    VuVirtq *vq = vu_get_queue(dev, qidx);
    struct virtio_rpmb_ctrl_command *cmd = NULL;

    for (;;) {
        cmd = vu_queue_pop(dev, vq, sizeof(struct virtio_rpmb_ctrl_command));
        if (!cmd) {
            break;
        }

        g_debug("un-handled cmd: %p", cmd);
    }
}

static void
vrpmb_queue_set_started(VuDev *dev, int qidx, bool started)
{
    VuVirtq *vq = vu_get_queue(dev, qidx);

    g_debug("queue started %d:%d\n", qidx, started);

    switch (qidx) {
    case 0:
        vu_set_queue_handler(dev, vq, started ? vrpmb_handle_ctrl : NULL);
        break;
    default:
        break;
    }
}

static int
vrpmb_process_msg(VuDev *dev, VhostUserMsg *msg, int *do_reply)
{
    switch (msg->request) {
    default:
        return 0;
    }

    return 0;
}

static const VuDevIface vuiface = {
    .set_features = vrpmb_set_features,
    .get_features = vrpmb_get_features,
    .queue_set_started = vrpmb_queue_set_started,
    .process_msg = vrpmb_process_msg,
    .get_config = vrpmb_get_config,
    .set_config = vrpmb_set_config,
};

static void vrpmb_destroy(VuRpmb *r)
{
    vug_deinit(&r->dev);
}

/* Print vhost-user.json backend program capabilities */
static void print_capabilities(void)
{
    printf("{\n");
    printf("  \"type\": \"block\"\n");
    printf("}\n");
}

int main (int argc, char *argv[])
{
    GError *error = NULL;
    GOptionContext *context;
    g_autoptr(GMainLoop) loop = NULL;
    g_autoptr(GSocket) socket = NULL;
    VuRpmb rpmb = {  };

    context = g_option_context_new ("vhost-user emulation of RPBM device");
    g_option_context_add_main_entries (context, options, "vhost-user-rpmb");
    if (!g_option_context_parse (context, &argc, &argv, &error))
    {
        g_print ("option parsing failed: %s\n", error->message);
        exit (1);
    }

    if (print_cap) {
        print_capabilities();
        exit(0);
    }

    if (!socket_path && socket_fd < 0) {
        g_printerr("Please specify either --fd or --socket-path\n");
        exit(EXIT_FAILURE);
    }

    /*
     * Now create a vhost-user socket that we will receive messages
     * on. Once we have our handler set up we can enter the glib main
     * loop.
     */
    if (socket_path) {
        g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(socket_path);
        g_autoptr(GSocket) bind_socket = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                                                      G_SOCKET_PROTOCOL_DEFAULT, &error);

        if (!g_socket_bind(bind_socket, addr, false, &error)) {
            g_printerr("Failed to bind to socket at %s (%s).\n",
                       socket_path, error->message);
            exit(EXIT_FAILURE);
        }
        if (!g_socket_listen(bind_socket, &error)) {
            g_printerr("Failed to listen on socket %s (%s).\n",
                       socket_path, error->message);
        }
        g_message("awaiting connection to %s", socket_path);
        socket = g_socket_accept(bind_socket, NULL, &error);
        if (!socket) {
            g_printerr("Failed to accept on socket %s (%s).\n",
                       socket_path, error->message);
        }
    } else {
        socket = g_socket_new_from_fd(socket_fd, &error);
        if (!socket) {
            g_printerr("Failed to connect to FD %d (%s).\n",
                       socket_fd, error->message);
            exit(EXIT_FAILURE);
        }
    }

    if (!vug_init(&rpmb.dev, VHOST_USER_RPMB_MAX_QUEUES, g_socket_get_fd(socket),
                  vrpmb_panic, &vuiface)) {
        g_printerr("Failed to initialize libvhost-user-glib.\n");
        exit(EXIT_FAILURE);
    }

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    vrpmb_destroy(&rpmb);
}
