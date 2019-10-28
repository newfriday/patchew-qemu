# Functional test that boots a PReP/40p machine and checks its serial console.
#
# Copyright (c) Philippe Mathieu-Daudé <f4bug@amsat.org>
#
# This work is licensed under the terms of the GNU GPL, version 2 or
# later. See the COPYING file in the top-level directory.

import os
import logging

from avocado import skipIf
from avocado import skipUnless
from avocado_qemu import MachineTest
from avocado_qemu import exec_command_and_wait_for_pattern
from avocado_qemu import wait_for_console_pattern


class IbmPrep40pMachine(MachineTest):

    timeout = 60

    # 12H0455 PPS Firmware Licensed Materials
    # Property of IBM (C) Copyright IBM Corp. 1994.
    # All rights reserved.
    # U.S. Government Users Restricted Rights - Use, duplication or disclosure
    # restricted by GSA ADP Schedule Contract with IBM Corp.
    @skipIf(os.getenv('CONTINUOUS_INTEGRATION'), 'Running on Travis-CI')
    @skipUnless(os.getenv('AVOCADO_ALLOW_UNTRUSTED_CODE'), 'untrusted code')
    def test_factory_firmware_and_netbsd(self):
        """
        :avocado: tags=arch:ppc
        :avocado: tags=machine:40p
        :avocado: tags=slowness:high
        """
        bios_url = ('ftp://ftp.boulder.ibm.com/rs6000/firmware/'
                    '7020-40p/P12H0456.IMG')
        bios_hash = '1775face4e6dc27f3a6ed955ef6eb331bf817f03'
        bios_path = self.fetch_asset(bios_url, asset_hash=bios_hash)
        drive_url = ('https://ftp.netbsd.org/pub/NetBSD/NetBSD-archive/'
                     'NetBSD-4.0/prep/installation/floppy/generic_com0.fs')
        drive_hash = 'dbcfc09912e71bd5f0d82c7c1ee43082fb596ceb'
        drive_path = self.fetch_asset(drive_url, asset_hash=drive_hash)

        self.vm.set_machine('40p')
        self.vm.set_console()
        self.vm.add_args('-bios', bios_path,
                         '-fda', drive_path)
        self.vm.launch()
        os_banner = 'NetBSD 4.0 (GENERIC) #0: Sun Dec 16 00:49:40 PST 2007'
        wait_for_console_pattern(self, os_banner)
        wait_for_console_pattern(self, 'Model: IBM PPS Model 6015')

    def test_openfirmware(self):
        """
        :avocado: tags=arch:ppc
        :avocado: tags=machine:40p
        """
        bios_url = ('https://github.com/artyom-tarasenko/openfirmware/'
                    'releases/download/40p-20190413/q40pofw-serial.rom')
        bios_hash = '880c80172ea5b2247c0ac2a8bf36bbe385192c72'
        bios_path = self.fetch_asset(bios_url, asset_hash=bios_hash)

        self.vm.set_machine('40p')
        self.vm.set_console()
        self.vm.add_args('-bios', bios_path)

        self.vm.launch()
        wait_for_console_pattern(self, 'QEMU PReP/40p')
        fw_banner = 'Open Firmware, built  April 13, 2019 09:29:23'
        wait_for_console_pattern(self, fw_banner)
        prompt_msg = 'Type any key to interrupt automatic startup'
        wait_for_console_pattern(self, prompt_msg)

    def test_openbios_192m(self):
        """
        :avocado: tags=arch:ppc
        :avocado: tags=machine:40p
        """
        self.vm.set_machine('40p')
        self.vm.set_console()
        self.vm.add_args('-m', '192') # test fw_cfg

        self.vm.launch()
        wait_for_console_pattern(self, '>> OpenBIOS')
        wait_for_console_pattern(self, '>> Memory: 192M')
        wait_for_console_pattern(self, '>> CPU type PowerPC,604')

    def test_openbios_and_netbsd(self):
        """
        :avocado: tags=arch:ppc
        :avocado: tags=machine:40p
        """
        drive_url = ('https://ftp.netbsd.org/pub/NetBSD/iso/7.1.2/'
                     'NetBSD-7.1.2-prep.iso')
        drive_hash = 'ac6fa2707d888b36d6fa64de6e7fe48e'
        drive_path = self.fetch_asset(drive_url, asset_hash=drive_hash,
                                      algorithm='md5')
        self.vm.set_machine('40p')
        self.vm.set_console()
        self.vm.add_args('-cdrom', drive_path,
                         '-boot', 'd')

        self.vm.launch()
        wait_for_console_pattern(self, 'NetBSD/prep BOOT, Revision 1.9')

    @skipUnless(os.getenv('AVOCADO_ALLOW_UNTRUSTED_CODE'), 'untrusted code')
    def test_sandalfoot_busybox(self):
        """
        :avocado: tags=arch:ppc
        :avocado: tags=machine:40p
        """
        drive_url = ('http://www.juneau-lug.org/zImage.initrd.sandalfoot')
        drive_hash = 'dacacfc4085ea51d34d99ef70e972b849e2c6949'
        drive_path = self.fetch_asset(drive_url, asset_hash=drive_hash)

        self.vm.set_machine('40p')
        self.vm.set_console()
        self.vm.add_args('-cdrom', drive_path,
                         '-boot', 'd')

        self.vm.launch()
        wait_for_console_pattern(self, 'Now booting the kernel')

        msg = 'Please press Enter to activate this console.'
        wait_for_console_pattern(self, msg)

        version = 'BusyBox v0.60.0 (2001.08.19-09:26+0000) Built-in shell (ash)'
        exec_command_and_wait_for_pattern(self, '', version)

        uname = 'Linux ppc 2.4.18 #5 Wed May 21 23:50:43 AKDT 2003 ppc unknown'
        exec_command_and_wait_for_pattern(self, 'uname -a', uname)

        cpu = 'PReP IBM 6015/7020 (Sandalfoot/Sandalbow)'
        exec_command_and_wait_for_pattern(self, 'cat /proc/cpuinfo', cpu)
