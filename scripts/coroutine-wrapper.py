#!/usr/bin/env python3
"""Generate coroutine wrappers for block subsystem.

The program parses one or several concatenated c files from stdin,
searches for functions with 'generated_co_wrapper' specifier
and generates corresponding wrappers on stdout.

Usage: cat FILE.[ch]... | coroutine-wrapper.py > generated-file.c

Copyright (c) 2020 Virtuozzo International GmbH.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
"""

import sys
import re
import subprocess
from typing import Iterator


def prettify(code: str) -> str:
    """Prettify code using clang-format if available"""

    try:
        style = '{IndentWidth: 4, BraceWrapping: {AfterFunction: true}, ' \
            'BreakBeforeBraces: Custom, SortIncludes: false, ' \
            'MaxEmptyLinesToKeep: 2}'
        p = subprocess.run(['clang-format', f'-style={style}'], check=True,
                           encoding='utf-8', input=code, capture_output=True)
        return p.stdout
    except FileNotFoundError:
        return code


def gen_header():
    copyright = re.sub('^.*Copyright', 'Copyright', __doc__, flags=re.DOTALL)
    copyright = re.sub('^(?=.)', ' * ', copyright.strip(), flags=re.MULTILINE)
    copyright = re.sub('^$', ' *', copyright, flags=re.MULTILINE)
    return f"""\
/*
 * File is generated by scripts/coroutine-wrapper.py
 *
{copyright}
 */

#include "qemu/osdep.h"
#include "block/coroutines.h"
#include "block/block-gen.h"\
"""


class ParamDecl:
    param_re = re.compile(r'(?P<decl>'
                          r'(?P<type>.*[ *])'
                          r'(?P<name>[a-z][a-z0-9_]*)'
                          r')')

    def __init__(self, param_decl: str) -> None:
        m = self.param_re.match(param_decl.strip())
        if m is None:
            raise ValueError(f'Wrong parameter declaration: "{param_decl}"')
        self.decl = m.group('decl')
        self.type = m.group('type')
        self.name = m.group('name')


class FuncDecl:
    def __init__(self, return_type: str, name: str, args: str) -> None:
        self.return_type = return_type.strip()
        self.name = name.strip()
        self.args = [ParamDecl(arg.strip()) for arg in args.split(',')]

    def gen_list(self, format: str) -> str:
        return ', '.join(format.format_map(arg.__dict__) for arg in self.args)

    def gen_block(self, format: str) -> str:
        return '\n'.join(format.format_map(arg.__dict__) for arg in self.args)


# Match wrappers declared with a generated_co_wrapper mark
func_decl_re = re.compile(r'^int\s*generated_co_wrapper\s*'
                          r'(?P<wrapper_name>[a-z][a-z0-9_]*)'
                          r'\((?P<args>[^)]*)\);$', re.MULTILINE)


def func_decl_iter(text: str) -> Iterator:
    for m in func_decl_re.finditer(text):
        yield FuncDecl(return_type='int',
                       name=m.group('wrapper_name'),
                       args=m.group('args'))


def snake_to_camel(func_name: str) -> str:
    """
    Convert underscore names like 'some_function_name' to camel-case like
    'SomeFunctionName'
    """
    words = func_name.split('_')
    words = [w[0].upper() + w[1:] for w in words]
    return ''.join(words)


def gen_wrapper(func: FuncDecl) -> str:
    assert func.name.startswith('bdrv_')
    assert not func.name.startswith('bdrv_co_')
    assert func.return_type == 'int'
    assert func.args[0].type in ['BlockDriverState *', 'BdrvChild *']

    name = 'bdrv_co_' + func.name[5:]
    bs = 'bs' if func.args[0].type == 'BlockDriverState *' else 'child->bs'
    struct_name = snake_to_camel(name)

    return f"""\
/*
 * Wrappers for {name}
 */

typedef struct {struct_name} {{
    BdrvPollCo poll_state;
{ func.gen_block('    {decl};') }
}} {struct_name};

static void coroutine_fn {name}_entry(void *opaque)
{{
    {struct_name} *s = opaque;

    s->poll_state.ret = {name}({ func.gen_list('s->{name}') });
    s->poll_state.in_progress = false;

    bdrv_poll_co__on_exit();
}}

int {func.name}({ func.gen_list('{decl}') })
{{
    if (qemu_in_coroutine()) {{
        return {name}({ func.gen_list('{name}') });
    }} else {{
        {struct_name} s = {{
            .poll_state.bs = {bs},
            .poll_state.in_progress = true,

{ func.gen_block('            .{name} = {name},') }
        }};

        s.poll_state.co = qemu_coroutine_create({name}_entry, &s);

        return bdrv_poll_co(&s.poll_state);
    }}
}}"""


def gen_wrappers_file(input_code: str) -> str:
    res = gen_header()
    for func in func_decl_iter(input_code):
        res += '\n\n\n'
        res += gen_wrapper(func)

    return prettify(res)  # prettify to wrap long lines


if __name__ == '__main__':
    print(gen_wrappers_file(sys.stdin.read()))
