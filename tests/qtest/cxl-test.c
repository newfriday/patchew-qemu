/*
 * QTest testcase for CXL
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define QEMU_PXB_CMD "-machine q35,cxl=on " \
                     "-device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52 "  \
                     "-M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=4G "

#define QEMU_2PXB_CMD "-machine q35,cxl=on "                            \
                      "-device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52 "  \
                      "-device pxb-cxl,id=cxl.1,bus=pcie.0,bus_nr=53 " \
                      "-M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.targets.1=cxl.1,cxl-fmw.0.size=4G "

#define QEMU_RP "-device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 "

/* Dual ports on first pxb */
#define QEMU_2RP "-device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 " \
                 "-device cxl-rp,id=rp1,bus=cxl.0,chassis=0,slot=1 "

/* Dual ports on each of the pxb instances */
#define QEMU_4RP "-device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 " \
                 "-device cxl-rp,id=rp1,bus=cxl.0,chassis=0,slot=1 " \
                 "-device cxl-rp,id=rp2,bus=cxl.1,chassis=0,slot=2 " \
                 "-device cxl-rp,id=rp3,bus=cxl.1,chassis=0,slot=3 "

#define QEMU_T3D "-object memory-backend-file,id=cxl-mem0,mem-path=%s,size=256M " \
                 "-object memory-backend-file,id=lsa0,mem-path=%s,size=256M "    \
                 "-device cxl-type3,bus=rp0,memdev=cxl-mem0,lsa=lsa0,id=cxl-pmem0 "

#define QEMU_2T3D "-object memory-backend-file,id=cxl-mem0,mem-path=%s,size=256M "    \
                  "-object memory-backend-file,id=lsa0,mem-path=%s,size=256M "    \
                  "-device cxl-type3,bus=rp0,memdev=cxl-mem0,lsa=lsa0,id=cxl-pmem0 " \
                  "-object memory-backend-file,id=cxl-mem1,mem-path=%s,size=256M "    \
                  "-object memory-backend-file,id=lsa1,mem-path=%s,size=256M "    \
                  "-device cxl-type3,bus=rp1,memdev=cxl-mem1,lsa=lsa1,id=cxl-pmem1 "

#define QEMU_4T3D "-object memory-backend-file,id=cxl-mem0,mem-path=%s,size=256M " \
                  "-object memory-backend-file,id=lsa0,mem-path=%s,size=256M "    \
                  "-device cxl-type3,bus=rp0,memdev=cxl-mem0,lsa=lsa0,id=cxl-pmem0 " \
                  "-object memory-backend-file,id=cxl-mem1,mem-path=%s,size=256M "    \
                  "-object memory-backend-file,id=lsa1,mem-path=%s,size=256M "    \
                  "-device cxl-type3,bus=rp1,memdev=cxl-mem1,lsa=lsa1,id=cxl-pmem1 " \
                  "-object memory-backend-file,id=cxl-mem2,mem-path=%s,size=256M "    \
                  "-object memory-backend-file,id=lsa2,mem-path=%s,size=256M "    \
                  "-device cxl-type3,bus=rp2,memdev=cxl-mem2,lsa=lsa2,id=cxl-pmem2 " \
                  "-object memory-backend-file,id=cxl-mem3,mem-path=%s,size=256M "    \
                  "-object memory-backend-file,id=lsa3,mem-path=%s,size=256M "    \
                  "-device cxl-type3,bus=rp3,memdev=cxl-mem3,lsa=lsa3,id=cxl-pmem3 "

static void cxl_basic_hb(void)
{
    qtest_start("-machine q35,cxl=on");
    qtest_end();
}

static void cxl_basic_pxb(void)
{
    qtest_start("-machine q35,cxl=on -device pxb-cxl,bus=pcie.0");
    qtest_end();
}

static void cxl_pxb_with_window(void)
{
    qtest_start(QEMU_PXB_CMD);
    qtest_end();
}

static void cxl_2pxb_with_window(void)
{
    qtest_start(QEMU_2PXB_CMD);
    qtest_end();
}

static void cxl_root_port(void)
{
    qtest_start(QEMU_PXB_CMD QEMU_RP);
    qtest_end();
}

static void cxl_2root_port(void)
{
    qtest_start(QEMU_PXB_CMD QEMU_2RP);
    qtest_end();
}

static void cxl_t3d(void)
{
    g_autoptr(GString) cmdline = g_string_new(NULL);
    char *template = g_strdup_printf("%s/cxl-test-XXXXXX", g_get_tmp_dir());
    const char *tmpfs;

    tmpfs = g_mkdtemp(template);

    g_string_printf(cmdline, QEMU_PXB_CMD QEMU_RP QEMU_T3D, tmpfs, tmpfs);
    g_free(template);

    qtest_start(cmdline->str);
    qtest_end();
}

static void cxl_1pxb_2rp_2t3d(void)
{
    g_autoptr(GString) cmdline = g_string_new(NULL);
    char *template = g_strdup_printf("%s/cxl-test-XXXXXX", g_get_tmp_dir());
    const char *tmpfs;

    tmpfs = g_mkdtemp(template);

    g_string_printf(cmdline, QEMU_PXB_CMD QEMU_2RP QEMU_2T3D,
                    tmpfs, tmpfs, tmpfs, tmpfs);
    g_free(template);

    qtest_start(cmdline->str);
    qtest_end();
}

static void cxl_2pxb_4rp_4t3d(void)
{
    g_autoptr(GString) cmdline = g_string_new(NULL);
    char *template = g_strdup_printf("%s/cxl-test-XXXXXX", g_get_tmp_dir());
    const char *tmpfs;

    tmpfs = g_mkdtemp(template);

    g_string_printf(cmdline, QEMU_2PXB_CMD QEMU_4RP QEMU_4T3D,
                    tmpfs, tmpfs, tmpfs, tmpfs, tmpfs, tmpfs,
                    tmpfs, tmpfs);
    g_free(template);

    qtest_start(cmdline->str);
    qtest_end();
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/pci/cxl/basic_hostbridge", cxl_basic_hb);
    qtest_add_func("/pci/cxl/basic_pxb", cxl_basic_pxb);
    qtest_add_func("/pci/cxl/pxb_with_window", cxl_pxb_with_window);
    qtest_add_func("/pci/cxl/pxb_x2_with_window", cxl_2pxb_with_window);
    qtest_add_func("/pci/cxl/rp", cxl_root_port);
    qtest_add_func("/pci/cxl/rp_x2", cxl_2root_port);
    qtest_add_func("/pci/cxl/type3_device", cxl_t3d);
    qtest_add_func("/pci/cxl/rp_x2_type3_x2", cxl_1pxb_2rp_2t3d);
    qtest_add_func("/pci/cxl/pxb_x2_root_port_x4_type3_x4", cxl_2pxb_4rp_4t3d);
    return g_test_run();
}
