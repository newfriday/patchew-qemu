/*
 * spice module support, also spice stubs.
 *
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "ui/qemu-spice-module.h"

int using_spice;

static void qemu_spice_init_stub(void)
{
}

static int qemu_spice_migrate_info_stub(const char *h, int p, int t,
                                        const char *s)
{
    return -1;
}

struct QemuSpiceOps qemu_spice = {
    .init         = qemu_spice_init_stub,
    .migrate_info = qemu_spice_migrate_info_stub,
};
