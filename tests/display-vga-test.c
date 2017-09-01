/*
 * QTest testcase for vga cards
 *
 * Copyright (c) 2014 Red Hat, Inc
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

static void pci_cirrus(void)
{
    qtest_quit(qtest_init("-vga none -device cirrus-vga"));
}

static void pci_stdvga(void)
{
    qtest_quit(qtest_init("-vga none -device VGA"));
}

static void pci_secondary(void)
{
    qtest_quit(qtest_init("-vga none -device secondary-vga"));
}

static void pci_multihead(void)
{
    qtest_quit(qtest_init("-vga none -device VGA -device secondary-vga"));
}

static void pci_virtio_gpu(void)
{
    qtest_quit(qtest_init("-vga none -device virtio-gpu-pci"));
}

#ifdef CONFIG_VIRTIO_VGA
static void pci_virtio_vga(void)
{
    qtest_quit(qtest_init("-vga none -device virtio-vga"));
}
#endif

int main(int argc, char **argv)
{
    const char *arch = qtest_get_arch();

    g_test_init(&argc, &argv, NULL);

    if (strcmp(arch, "alpha") == 0 || strcmp(arch, "i386") == 0 ||
        strcmp(arch, "mips") == 0 || strcmp(arch, "x86_64") == 0) {
        qtest_add_func("/display/pci/cirrus", pci_cirrus);
    }
    qtest_add_func("/display/pci/stdvga", pci_stdvga);
    qtest_add_func("/display/pci/secondary", pci_secondary);
    qtest_add_func("/display/pci/multihead", pci_multihead);
    qtest_add_func("/display/pci/virtio-gpu", pci_virtio_gpu);
#ifdef CONFIG_VIRTIO_VGA
    qtest_add_func("/display/pci/virtio-vga", pci_virtio_vga);
#endif
    return g_test_run();
}
