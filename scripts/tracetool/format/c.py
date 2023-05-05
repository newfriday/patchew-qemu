# -*- coding: utf-8 -*-

"""
trace/generated-tracers.c
"""

__author__     = "Lluís Vilanova <vilanova@ac.upc.edu>"
__copyright__  = "Copyright 2012-2014, Lluís Vilanova <vilanova@ac.upc.edu>"
__license__    = "GPL version 2 or (at your option) any later version"

__maintainer__ = "Stefan Hajnoczi"
__email__      = "stefanha@redhat.com"


from tracetool import out


def generate(events, backend, group):
    active_events = [e for e in events
                     if "disable" not in e.properties]

    header = "trace-" + group + ".h"

    out('/* This file is autogenerated by tracetool, do not edit. */',
        '',
        '#include "qemu/osdep.h"',
        '#include "qemu/module.h"',
        '#include "%s"' % header,
        '')

    for e in events:
        out('uint16_t %s;' % e.api(e.QEMU_DSTATE))

    for e in events:
        out('TraceEvent %(event)s = {',
            '    .id = 0,',
            '    .name = \"%(name)s\",',
            '    .sstate = %(sstate)s,',
            '    .dstate = &%(dstate)s ',
            '};',
            event = e.api(e.QEMU_EVENT),
            name = e.name,
            sstate = "TRACE_%s_ENABLED" % e.name.upper(),
            dstate = e.api(e.QEMU_DSTATE))

    out('TraceEvent *%(group)s_trace_events[] = {',
        group = group.lower())

    for e in events:
        out('    &%(event)s,', event = e.api(e.QEMU_EVENT))

    out('  NULL,',
        '};',
        '')

    out('static void trace_%(group)s_register_events(void)',
        '{',
        '    trace_event_register_group(%(group)s_trace_events);',
        '}',
        'trace_init(trace_%(group)s_register_events)',
        group = group.lower())

    backend.generate_begin(active_events, group)
    for event in active_events:
        backend.generate(event, group)
    backend.generate_end(active_events, group)
