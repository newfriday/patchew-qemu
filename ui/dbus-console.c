/*
 * QEMU DBus display console
 *
 * Copyright (c) 2021 Marc-André Lureau <marcandre.lureau@redhat.com>
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
#include "ui/input.h"
#include "ui/kbd-state.h"
#include "trace.h"

#include <gio/gunixfdlist.h>

#include "dbus.h"

struct _DBusDisplayConsole {
    GDBusObjectSkeleton parent_instance;

    DBusDisplay *display;
    QemuConsole *con;
    DBusDisplayListener *listener;
    QemuDBusDisplay1Console *iface;

    QemuDBusDisplay1Keyboard *iface_kbd;
    QKbdState *kbd;

    QemuDBusDisplay1Mouse *iface_mouse;
    gboolean last_set;
    guint last_x;
    guint last_y;
    Notifier mouse_mode_notifier;
};

G_DEFINE_TYPE(DBusDisplayConsole,
              dbus_display_console,
              G_TYPE_DBUS_OBJECT_SKELETON)

void
dbus_display_console_set_size(DBusDisplayConsole *ddc,
                              uint32_t width, uint32_t height)
{
    g_object_set(ddc->iface,
                 "width", width,
                 "height", height,
                 NULL);
}

static void
dbus_display_console_init(DBusDisplayConsole *object)
{
}

static void
dbus_display_console_dispose(GObject *object)
{
    DBusDisplayConsole *ddc = DBUS_DISPLAY_CONSOLE(object);

    g_clear_object(&ddc->iface_kbd);
    g_clear_object(&ddc->iface);
    dbus_display_listener_unref_all_connections(ddc->listener);
    g_clear_object(&ddc->listener);
    g_clear_pointer(&ddc->kbd, qkbd_state_free);

    G_OBJECT_CLASS(dbus_display_console_parent_class)->dispose(object);
}

static void
dbus_display_console_class_init(DBusDisplayConsoleClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->dispose = dbus_display_console_dispose;
}

static void
listener_vanished_cb(DBusDisplayListenerConnection *ddlc)
{
    DBusDisplayConsole *ddc = dbus_display_listener_connection_get_console(ddlc);
    const char *name = dbus_display_listener_connection_get_bus_name(ddlc);

    trace_dbus_listener_vanished(name);

    g_object_unref(ddlc);
    qkbd_state_lift_all_keys(ddc->kbd);
}

static gboolean
dbus_console_set_ui_info(DBusDisplayConsole *ddc,
                         GDBusMethodInvocation *invocation,
                         guint16 arg_width_mm,
                         guint16 arg_height_mm,
                         gint arg_xoff,
                         gint arg_yoff,
                         guint arg_width,
                         guint arg_height)
{
    QemuUIInfo info = {
        .width_mm = arg_width_mm,
        .height_mm = arg_height_mm,
        .xoff = arg_xoff,
        .yoff = arg_yoff,
        .width = arg_width,
        .height = arg_height,
    };

    if (!dpy_ui_info_supported(ddc->con)) {
        g_dbus_method_invocation_return_error(invocation,
                                              DBUS_DISPLAY_ERROR,
                                              DBUS_DISPLAY_ERROR_UNSUPPORTED,
                                              "SetUIInfo is not supported");
        return DBUS_METHOD_INVOCATION_HANDLED;
    }

    dpy_set_ui_info(ddc->con, &info, false);
    qemu_dbus_display1_console_complete_set_uiinfo(ddc->iface, invocation);
    return DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
dbus_console_register_listener(DBusDisplayConsole *ddc,
                               GDBusMethodInvocation *invocation,
                               GUnixFDList *fd_list,
                               GVariant *arg_listener)
{
    const char *sender = g_dbus_method_invocation_get_sender(invocation);
    GDBusConnection *conn;
    g_autoptr(GError) err = NULL;
    g_autoptr(GSocket) socket = NULL;
    g_autoptr(GSocketConnection) socket_conn = NULL;
    g_autofree char *guid = g_dbus_generate_guid();
    DBusDisplayListenerConnection *listener_conn;
    int fd;

    if (sender && dbus_display_listener_has_connection(ddc->listener, sender)) {
        g_dbus_method_invocation_return_error(
            invocation,
            DBUS_DISPLAY_ERROR,
            DBUS_DISPLAY_ERROR_INVALID,
            "`%s` is already registered!",
            sender);
        return DBUS_METHOD_INVOCATION_HANDLED;
    }

    fd = g_unix_fd_list_get(fd_list, g_variant_get_handle(arg_listener), &err);
    if (err) {
        g_dbus_method_invocation_return_error(
            invocation,
            DBUS_DISPLAY_ERROR,
            DBUS_DISPLAY_ERROR_FAILED,
            "Couldn't get peer fd: %s", err->message);
        return DBUS_METHOD_INVOCATION_HANDLED;
    }

    socket = g_socket_new_from_fd(fd, &err);
    if (err) {
        g_dbus_method_invocation_return_error(
            invocation,
            DBUS_DISPLAY_ERROR,
            DBUS_DISPLAY_ERROR_FAILED,
            "Couldn't make a socket: %s", err->message);
        close(fd);
        return DBUS_METHOD_INVOCATION_HANDLED;
    }
    socket_conn = g_socket_connection_factory_create_connection(socket);

    qemu_dbus_display1_console_complete_register_listener(
        ddc->iface, invocation, NULL);

    conn = g_dbus_connection_new_sync(
        G_IO_STREAM(socket_conn),
        guid,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER,
        NULL, NULL, &err);
    if (err) {
        error_report("Failed to setup peer connection: %s", err->message);
        return DBUS_METHOD_INVOCATION_HANDLED;
    }

    listener_conn = dbus_display_listener_add_connection(ddc->listener,
                                                         sender, conn);
    if (!listener_conn) {
        return DBUS_METHOD_INVOCATION_HANDLED;
    }

    g_object_connect(conn,
                     "swapped-signal::closed", listener_vanished_cb,
                     listener_conn,
                     NULL);

    trace_dbus_registered_listener(sender);
    return DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
dbus_kbd_press(DBusDisplayConsole *ddc,
               GDBusMethodInvocation *invocation,
               guint arg_keycode)
{
    QKeyCode qcode = qemu_input_key_number_to_qcode(arg_keycode);

    trace_dbus_kbd_press(arg_keycode);

    qkbd_state_key_event(ddc->kbd, qcode, true);

    qemu_dbus_display1_keyboard_complete_press(ddc->iface_kbd, invocation);

    return DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
dbus_kbd_release(DBusDisplayConsole *ddc,
                 GDBusMethodInvocation *invocation,
                 guint arg_keycode)
{
    QKeyCode qcode = qemu_input_key_number_to_qcode(arg_keycode);

    trace_dbus_kbd_release(arg_keycode);

    qkbd_state_key_event(ddc->kbd, qcode, false);

    qemu_dbus_display1_keyboard_complete_release(ddc->iface_kbd, invocation);

    return DBUS_METHOD_INVOCATION_HANDLED;
}

static void
dbus_kbd_qemu_leds_updated(void *data, int ledstate)
{
    DBusDisplayConsole *ddc = DBUS_DISPLAY_CONSOLE(data);

    qemu_dbus_display1_keyboard_set_modifiers(ddc->iface_kbd, ledstate);
}

static gboolean
dbus_mouse_rel_motion(DBusDisplayConsole *ddc,
                      GDBusMethodInvocation *invocation,
                      int dx, int dy)
{
    trace_dbus_mouse_rel_motion(dx, dy);

    if (qemu_input_is_absolute()) {
        g_dbus_method_invocation_return_error(
            invocation, DBUS_DISPLAY_ERROR,
            DBUS_DISPLAY_ERROR_INVALID,
            "Mouse is not relative");
        return DBUS_METHOD_INVOCATION_HANDLED;
    }

    qemu_input_queue_rel(ddc->con, INPUT_AXIS_X, dx);
    qemu_input_queue_rel(ddc->con, INPUT_AXIS_Y, dy);
    qemu_input_event_sync();

    qemu_dbus_display1_mouse_complete_rel_motion(ddc->iface_mouse,
                                                    invocation);

    return DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
dbus_mouse_set_pos(DBusDisplayConsole *ddc,
                   GDBusMethodInvocation *invocation,
                   guint x, guint y)
{
    int width, height;

    trace_dbus_mouse_set_pos(x, y);

    if (!qemu_input_is_absolute()) {
        g_dbus_method_invocation_return_error(
            invocation, DBUS_DISPLAY_ERROR,
            DBUS_DISPLAY_ERROR_INVALID,
            "Mouse is not absolute");
        return DBUS_METHOD_INVOCATION_HANDLED;
    }

    width = qemu_console_get_width(ddc->con, 0);
    height = qemu_console_get_height(ddc->con, 0);
    if (x >= width || y >= height) {
        g_dbus_method_invocation_return_error(
            invocation, DBUS_DISPLAY_ERROR,
            DBUS_DISPLAY_ERROR_INVALID,
            "Invalid mouse position");
        return DBUS_METHOD_INVOCATION_HANDLED;
    }
    qemu_input_queue_abs(ddc->con, INPUT_AXIS_X, x, 0, width);
    qemu_input_queue_abs(ddc->con, INPUT_AXIS_Y, y, 0, height);
    qemu_input_event_sync();

    qemu_dbus_display1_mouse_complete_set_abs_position(ddc->iface_mouse,
                                                          invocation);

    return DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
dbus_mouse_press(DBusDisplayConsole *ddc,
                 GDBusMethodInvocation *invocation,
                 guint button)
{
    trace_dbus_mouse_press(button);

    qemu_input_queue_btn(ddc->con, button, true);
    qemu_input_event_sync();

    qemu_dbus_display1_mouse_complete_press(ddc->iface_mouse, invocation);

    return DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
dbus_mouse_release(DBusDisplayConsole *ddc,
                   GDBusMethodInvocation *invocation,
                   guint button)
{
    trace_dbus_mouse_release(button);

    qemu_input_queue_btn(ddc->con, button, false);
    qemu_input_event_sync();

    qemu_dbus_display1_mouse_complete_release(ddc->iface_mouse, invocation);

    return DBUS_METHOD_INVOCATION_HANDLED;
}

static void
dbus_mouse_mode_change(Notifier *notify, void *data)
{
    DBusDisplayConsole *ddc =
        container_of(notify, DBusDisplayConsole, mouse_mode_notifier);

    g_object_set(ddc->iface_mouse,
                 "is-absolute", qemu_input_is_absolute(),
                 NULL);
}

int dbus_display_console_get_index(DBusDisplayConsole *ddc)
{
    return qemu_console_get_index(ddc->con);
}

DBusDisplayConsole *
dbus_display_console_new(DBusDisplay *display, QemuConsole *con)
{
    g_autofree char *path = NULL;
    g_autofree char *label = NULL;
    char device_addr[256] = "";
    DBusDisplayConsole *ddc;
    int idx;

    assert(display);
    assert(con);

    label = qemu_console_get_label(con);
    idx = qemu_console_get_index(con);
    path = g_strdup_printf(DBUS_DISPLAY1_ROOT "/Console_%d", idx);
    ddc = g_object_new(DBUS_DISPLAY_TYPE_CONSOLE,
                        "g-object-path", path,
                        NULL);
    ddc->display = display;
    ddc->con = con;
    /* handle errors, and skip non graphics? */
    qemu_console_fill_device_address(
        con, device_addr, sizeof(device_addr), NULL);

    ddc->iface = qemu_dbus_display1_console_skeleton_new();
    g_object_set(ddc->iface,
        "label", label,
        "type", qemu_console_is_graphic(con) ? "Graphic" : "Text",
        "head", qemu_console_get_head(con),
        "width", qemu_console_get_width(con, 0),
        "height", qemu_console_get_height(con, 0),
        "device-address", device_addr,
        NULL);
    g_object_connect(ddc->iface,
        "swapped-signal::handle-register-listener",
        dbus_console_register_listener, ddc,
        "swapped-signal::handle-set-uiinfo",
        dbus_console_set_ui_info, ddc,
        NULL);
    g_dbus_object_skeleton_add_interface(G_DBUS_OBJECT_SKELETON(ddc),
        G_DBUS_INTERFACE_SKELETON(ddc->iface));

    ddc->kbd = qkbd_state_init(con);
    ddc->iface_kbd = qemu_dbus_display1_keyboard_skeleton_new();
    qemu_add_led_event_handler(dbus_kbd_qemu_leds_updated, ddc);
    g_object_connect(ddc->iface_kbd,
        "swapped-signal::handle-press", dbus_kbd_press, ddc,
        "swapped-signal::handle-release", dbus_kbd_release, ddc,
        NULL);
    g_dbus_object_skeleton_add_interface(G_DBUS_OBJECT_SKELETON(ddc),
        G_DBUS_INTERFACE_SKELETON(ddc->iface_kbd));

    ddc->iface_mouse = qemu_dbus_display1_mouse_skeleton_new();
    g_object_connect(ddc->iface_mouse,
        "swapped-signal::handle-set-abs-position", dbus_mouse_set_pos, ddc,
        "swapped-signal::handle-rel-motion", dbus_mouse_rel_motion, ddc,
        "swapped-signal::handle-press", dbus_mouse_press, ddc,
        "swapped-signal::handle-release", dbus_mouse_release, ddc,
        NULL);
    g_dbus_object_skeleton_add_interface(G_DBUS_OBJECT_SKELETON(ddc),
        G_DBUS_INTERFACE_SKELETON(ddc->iface_mouse));

    ddc->listener = dbus_display_listener_new(ddc);
    ddc->mouse_mode_notifier.notify = dbus_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&ddc->mouse_mode_notifier);

    return ddc;
}
