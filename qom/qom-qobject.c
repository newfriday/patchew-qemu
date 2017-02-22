/*
 * QEMU Object Model - QObject wrappers
 *
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * Author: Paolo Bonzini <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qom/object.h"
#include "qom/qom-qobject.h"
#include "qapi/visitor.h"
#include "qapi/qobject-input-visitor.h"
#include "qapi/qobject-output-visitor.h"

void object_property_set_qobject(Object *obj, QObject *value,
                                 const char *name, Error **errp)
{
    Visitor *v;
    /* TODO: Should we reject, rather than ignore, excess input? */
    v = qobject_input_visitor_new(value, false);
    object_property_set(obj, v, name, errp);
    visit_free(v);
}

QObject *object_property_get_qobject(Object *obj, const char *name,
                                     Error **errp)
{
    QObject *ret = NULL;
    Error *local_err = NULL;
    Visitor *v;

    v = qobject_output_visitor_new(&ret);
    object_property_get(obj, v, name, &local_err);
    if (!local_err) {
        visit_complete(v, &ret);
    }
    error_propagate(errp, local_err);
    visit_free(v);
    return ret;
}

void object_property_set_ptr(Object *obj, void *ptr, const char *name,
                             void (*visit_type)(Visitor *, const char *,
                                                void **, Error **),
                             Error **errp)
{
    Error *local_err = NULL;
    QObject *ret = NULL;
    Visitor *v;
    v = qobject_output_visitor_new(&ret);
    visit_type(v, name, &ptr, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        visit_free(v);
        return;
    }
    visit_complete(v, &ret);
    visit_free(v);

    /* Do not use object_property_set_qobject until we switch it
     * to use qobject_input_visitor_new in strict mode.  See the
     * /qom/proplist/get-set-ptr/contravariant unit test.
     */
    v = qobject_input_visitor_new(ret, true);
    object_property_set(obj, v, name, errp);
    visit_free(v);
    qobject_decref(ret);
}

void *object_property_get_ptr(Object *obj, const char *name,
                              void (*visit_type)(Visitor *, const char *,
                                                 void **, Error **),
                              Error **errp)
{
    QObject *ret;
    Visitor *v;
    void *ptr = NULL;

    ret = object_property_get_qobject(obj, name, errp);
    if (!ret) {
        return NULL;
    }

    /* Do not enable strict mode to allow passing covariant
     * data types.
     */
    v = qobject_input_visitor_new(ret, false);
    visit_type(v, name, &ptr, errp);
    qobject_decref(ret);
    visit_free(v);
    return ptr;
}
