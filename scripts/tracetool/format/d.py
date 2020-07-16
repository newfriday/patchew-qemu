# -*- coding: utf-8 -*-

"""
trace/generated-tracers.dtrace (DTrace only).
"""

__author__     = "Lluís Vilanova <vilanova@ac.upc.edu>"
__copyright__  = "Copyright 2012-2014, Lluís Vilanova <vilanova@ac.upc.edu>"
__license__    = "GPL version 2 or (at your option) any later version"

__maintainer__ = "Stefan Hajnoczi"
__email__      = "stefanha@redhat.com"


from tracetool import out
from sys import platform


# Reserved keywords from
# https://wikis.oracle.com/display/DTrace/Types,+Operators+and+Expressions
RESERVED_WORDS = (
    'auto', 'goto', 'sizeof', 'break', 'if', 'static', 'case', 'import',
    'string', 'char', 'inline', 'stringof', 'const', 'int', 'struct',
    'continue', 'long', 'switch', 'counter', 'offsetof', 'this',
    'default', 'probe', 'translator', 'do', 'provider', 'typedef',
    'double', 'register', 'union', 'else', 'restrict', 'unsigned',
    'enum', 'return', 'void', 'extern', 'self', 'volatile', 'float',
    'short', 'while', 'for', 'signed', 'xlate',
)


def generate(events, backend, group):
    events = [e for e in events
              if "disable" not in e.properties]

    # SystemTap's dtrace(1) warns about empty "provider qemu {}" but is happy
    # with an empty file.  Avoid the warning.
    # But dtrace on macOS can't deal with empty files.
    if not events and platform != "darwin":
        return

    out('/* This file is autogenerated by tracetool, do not edit. */'
        '',
        'provider qemu {')

    for e in events:
        args = []
        for type_, name in e.args:
            if platform == "darwin":
                if type_ == 'bool':
                    type_ = '_Bool'
                if type_ == 'bool *':
                    type_ = '_Bool *'
            if name in RESERVED_WORDS:
                name += '_'
            args.append(type_ + ' ' + name)

        # Define prototype for probe arguments
        out('',
            'probe %(name)s(%(args)s);',
            name=e.name,
            args=','.join(args))

    out('',
        '};')
