# TestEnv class to manage test environment variables.
#
# Copyright (c) 2020-2021 Virtuozzo International GmbH
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

import os
import sys
import tempfile
from pathlib import Path
import shutil
import collections
import subprocess
from contextlib import AbstractContextManager
from typing import Dict, Any, Optional


def get_default_machine(qemu_prog: str) -> str:
    outp = subprocess.run([qemu_prog, '-machine', 'help'], check=True,
                          text=True, stdout=subprocess.PIPE).stdout

    machines = outp.split('\n')
    default_machine = next(m for m in machines if m.endswith(' (default)'))
    default_machine = default_machine.split(' ', 1)[0]

    alias_suf = ' (alias of {})'.format(default_machine)
    alias = next((m for m in machines if m.endswith(alias_suf)), None)
    if alias is not None:
        default_machine = alias.split(' ', 1)[0]

    return default_machine


class TestEnv(AbstractContextManager['TestEnv']):
    """
    Manage system environment for running tests

    The following variables are supported/provided. They are represented by
    lower-cased TestEnv attributes.
    """

    # We store environment variables as instance attributes, and there are a
    # lot of them. Silence pylint:
    # pylint: disable=too-many-instance-attributes

    env_variables = ['PYTHONPATH', 'TEST_DIR', 'SOCK_DIR', 'SAMPLE_IMG_DIR',
                     'OUTPUT_DIR', 'PYTHON', 'QEMU_PROG', 'QEMU_IMG_PROG',
                     'QEMU_IO_PROG', 'QEMU_NBD_PROG', 'QSD_PROG',
                     'SOCKET_SCM_HELPER', 'QEMU_OPTIONS', 'QEMU_IMG_OPTIONS',
                     'QEMU_IO_OPTIONS', 'QEMU_NBD_OPTIONS', 'IMGOPTS',
                     'IMGFMT', 'IMGPROTO', 'AIOMODE', 'CACHEMODE',
                     'VALGRIND_QEMU', 'CACHEMODE_IS_DEFAULT', 'IMGFMT_GENERIC',
                     'IMGOPTSSYNTAX', 'IMGKEYSECRET', 'QEMU_DEFAULT_MACHINE']

    def get_env(self) -> Dict[str, str]:
        env = {}
        for v in self.env_variables:
            val = getattr(self, v.lower(), None)
            if val is not None:
                env[v] = val

        return env

    def init_directories(self):
        """Init directory variables:
             PYTHONPATH
             TEST_DIR
             SOCK_DIR
             SAMPLE_IMG_DIR
             OUTPUT_DIR
        """
        self.pythonpath = os.getenv('PYTHONPATH')
        if self.pythonpath:
            self.pythonpath = self.source_iotests + os.pathsep + \
                self.pythonpath
        else:
            self.pythonpath = self.source_iotests

        self.test_dir = os.getenv('TEST_DIR',
                                  os.path.join(os.getcwd(), 'scratch'))
        Path(self.test_dir).mkdir(parents=True, exist_ok=True)

        self.sock_dir = os.getenv('SOCK_DIR')
        self.tmp_sock_dir = False
        if self.sock_dir:
            Path(self.test_dir).mkdir(parents=True, exist_ok=True)
        else:
            self.sock_dir = tempfile.mkdtemp()
            self.tmp_sock_dir = True

        self.sample_img_dir = os.getenv('SAMPLE_IMG_DIR',
                                        os.path.join(self.source_iotests,
                                                     'sample_images'))

        self.output_dir = os.getcwd()  # OUTPUT_DIR

    def init_binaries(self):
        """Init binary path variables:
             PYTHON (for bash tests)
             QEMU_PROG, QEMU_IMG_PROG, QEMU_IO_PROG, QEMU_NBD_PROG, QSD_PROG
             SOCKET_SCM_HELPER
        """
        self.python = sys.executable

        def root(*names):
            return os.path.join(self.build_root, *names)

        arch = os.uname().machine
        if 'ppc64' in arch:
            arch = 'ppc64'

        self.qemu_prog = os.getenv('QEMU_PROG', root(f'qemu-system-{arch}'))
        self.qemu_img_prog = os.getenv('QEMU_IMG_PROG', root('qemu-img'))
        self.qemu_io_prog = os.getenv('QEMU_IO_PROG', root('qemu-io'))
        self.qemu_nbd_prog = os.getenv('QEMU_NBD_PROG', root('qemu-nbd'))
        self.qsd_prog = os.getenv('QSD_PROG', root('storage-daemon',
                                                   'qemu-storage-daemon'))

        for b in [self.qemu_img_prog, self.qemu_io_prog, self.qemu_nbd_prog,
                  self.qemu_prog, self.qsd_prog]:
            if not os.path.exists(b):
                sys.exit('Not such file: ' + b)
            if not os.access(b, os.X_OK):
                sys.exit('Not executable: ' + b)

        helper_path = os.path.join(self.build_iotests, 'socket_scm_helper')
        if os.access(helper_path, os.X_OK):
            self.socket_scm_helper = helper_path  # SOCKET_SCM_HELPER

    def __init__(self, imgfmt: str, imgproto: str, aiomode: str,
                 cachemode: Optional[str] = None,
                 imgopts: Optional[str] = None,
                 misalign: bool = False,
                 debug: bool = False) -> None:
        self.imgfmt = imgfmt
        self.imgproto = imgproto
        self.aiomode = aiomode
        self.imgopts = imgopts
        self.misalign = misalign
        self.debug = debug

        if cachemode is None:
            self.cachemode_is_default = 'true'
            self.cachemode = 'writeback'
        else:
            self.cachemode_is_default = 'false'
            self.cachemode = cachemode

        # Initialize generic paths: build_root, build_iotests, source_iotests,
        # which are needed to initialize some environment variables. They are
        # used by init_*() functions as well.

        if os.path.islink(sys.argv[0]):
            # called from the build tree
            self.source_iotests = os.path.dirname(os.readlink(sys.argv[0]))
            self.build_iotests = os.path.dirname(os.path.abspath(sys.argv[0]))
        else:
            # called from the source tree
            self.source_iotests = os.getcwd()
            self.build_iotests = self.source_iotests

        self.build_root = os.path.join(self.build_iotests, '..', '..')

        self.init_directories()
        self.init_binaries()

        # QEMU_OPTIONS
        self.qemu_options = '-nodefaults -display none -accel qtest'
        machine_map = (
            ('arm', 'virt'),
            ('aarch64', 'virt'),
            ('avr', 'mega2560'),
            ('rx', 'gdbsim-r5f562n8'),
            ('tricore', 'tricore_testboard')
        )
        for suffix, machine in machine_map:
            if self.qemu_prog.endswith(f'qemu-system-{suffix}'):
                self.qemu_options += f' -machine {machine}'

        # QEMU_DEFAULT_MACHINE
        self.qemu_default_machine = get_default_machine(self.qemu_prog)

        self.qemu_img_options = os.getenv('QEMU_IMG_OPTIONS')
        self.qemu_nbd_options = os.getenv('QEMU_NBD_OPTIONS')

        is_generic = self.imgfmt not in ['bochs', 'cloop', 'dmg']
        self.imgfmt_generic = 'true' if is_generic else 'false'

        self.qemu_io_options = f'--cache {self.cachemode} --aio {self.aiomode}'
        if self.misalign:
            self.qemu_io_options += ' --misalign'

        self.qemu_io_options_no_fmt = self.qemu_io_options

        if self.imgfmt == 'luks':
            self.imgoptssyntax = 'true'
            self.imgkeysecret = '123456'
            if not self.imgopts:
                self.imgopts = 'iter-time=10'
            elif 'iter-time=' not in self.imgopts:
                self.imgopts += ',iter-time=10'
        else:
            self.imgoptssyntax = 'false'
            self.qemu_io_options += ' -f ' + self.imgfmt

        if self.imgfmt == 'vmkd':
            if not self.imgopts:
                self.imgopts = 'zeroed_grain=on'
            elif 'zeroed_grain=' not in self.imgopts:
                self.imgopts += ',zeroed_grain=on'

    def close(self) -> None:
        if self.tmp_sock_dir:
            shutil.rmtree(self.sock_dir)

    def __enter__(self) -> 'TestEnv':
        return self

    def __exit__(self, exc_type: Any, exc_value: Any, traceback: Any) -> None:
        self.close()

    def print_env(self) -> None:
        template = """\
QEMU          -- "{QEMU_PROG}" {QEMU_OPTIONS}
QEMU_IMG      -- "{QEMU_IMG_PROG}" {QEMU_IMG_OPTIONS}
QEMU_IO       -- "{QEMU_IO_PROG}" {QEMU_IO_OPTIONS}
QEMU_NBD      -- "{QEMU_NBD_PROG}" {QEMU_NBD_OPTIONS}
IMGFMT        -- {IMGFMT}{imgopts}
IMGPROTO      -- {IMGPROTO}
PLATFORM      -- {platform}
TEST_DIR      -- {TEST_DIR}
SOCK_DIR      -- {SOCK_DIR}
SOCKET_SCM_HELPER -- {SOCKET_SCM_HELPER}"""

        args = collections.defaultdict(str, self.get_env())

        if 'IMGOPTS' in args:
            args['imgopts'] = f" ({args['IMGOPTS']})"

        u = os.uname()
        args['platform'] = f'{u.sysname}/{u.machine} {u.nodename} {u.release}'

        print(template.format_map(args))
