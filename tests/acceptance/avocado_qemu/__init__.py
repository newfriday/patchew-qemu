# Test class and utilities for functional tests
#
# Copyright (c) 2018 Red Hat, Inc.
#
# Author:
#  Cleber Rosa <crosa@redhat.com>
#
# This work is licensed under the terms of the GNU GPL, version 2 or
# later.  See the COPYING file in the top-level directory.

import os
import sys

import avocado

SRC_ROOT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
SRC_ROOT_DIR = os.path.abspath(os.path.dirname(SRC_ROOT_DIR))
sys.path.append(os.path.join(SRC_ROOT_DIR, 'scripts'))

from qemu import QEMUMachine


#: Mapping of host arch names to target arch names.  It's expected that the
#: arch identification on the host, using os.uname()[4], would return the
#: key (LHS).  The QEMU target name, and consequently the target binary, would
#: be based on the name on the value (RHS).
HOST_TARGET_ARCH = {
    'armeb': 'arm',
    'aarch64_be': 'aarch64',
    'microblazeel': 'microblaze',
    'mipsel': 'mips',
    'mipsn32el' : 'mips64',
    'mips64el': 'mips64',
    'or1k': 'openrisc',
    'ppc64le': 'ppc64',
    'ppc64abi32': 'ppc64',
    'riscv64': 'riscv',
    'sh4eb': 'sh4',
    'sparc32plus': 'sparc64',
    'xtensaeb': 'xtensa'
    }


def is_readable_executable_file(path):
    return os.path.isfile(path) and os.access(path, os.R_OK | os.X_OK)


def pick_default_qemu_bin():
    """
    Picks the path of a QEMU binary, starting either in the current working
    directory or in the source tree root directory.
    """
    arch = os.uname()[4]
    arch = HOST_TARGET_ARCH.get(arch, arch)
    qemu_bin_relative_path = os.path.join("%s-softmmu" % arch,
                                          "qemu-system-%s" % arch)
    if is_readable_executable_file(qemu_bin_relative_path):
        return qemu_bin_relative_path

    qemu_bin_from_src_dir_path = os.path.join(SRC_ROOT_DIR,
                                              qemu_bin_relative_path)
    if is_readable_executable_file(qemu_bin_from_src_dir_path):
        return qemu_bin_from_src_dir_path


class Test(avocado.Test):
    def setUp(self):
        self.vm = None
        self.qemu_bin = self.params.get('qemu_bin',
                                        default=pick_default_qemu_bin())
        if self.qemu_bin is None:
            self.cancel("No QEMU binary defined or found in the source tree")
        self.vm = QEMUMachine(self.qemu_bin)

    def tearDown(self):
        if self.vm is not None:
            self.vm.shutdown()
