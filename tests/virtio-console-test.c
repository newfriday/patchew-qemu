/*
 * QTest testcase for VirtIO Console
 *
 * Copyright (c) 2014 SUSE LINUX Products GmbH
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Tests only initialization so far. TODO: Replace with functional tests */
static void console_nop(void)
{
    qtest_start("-device virtio-serial,id=vser0 "
                "-device virtconsole,bus=vser0.0");
    qtest_end();
}

static void serialport_nop(void)
{
    qtest_start("-device virtio-serial,id=vser0 "
                "-device virtserialport,bus=vser0.0");
    qtest_end();
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/virtio/console/nop", console_nop);
    qtest_add_func("/virtio/serialport/nop", serialport_nop);

    return g_test_run();
}
