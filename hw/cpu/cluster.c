/*
 * QEMU CPU cluster
 *
 * Copyright (c) 2018 GreenSocs SAS
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see
 * <http://www.gnu.org/licenses/gpl-2.0.html>
 */

#include "qemu/osdep.h"
#include "hw/cpu/cluster.h"
#include "hw/qdev-properties.h"
#include "hw/core/cpu.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/cutils.h"

static Property cpu_cluster_properties[] = {
    DEFINE_PROP_UINT32("cluster-id", CPUClusterState, cluster_id, 0),
    DEFINE_PROP_STRING("cpu-type", CPUClusterState, cpu_type),
    DEFINE_PROP_END_OF_LIST()
};

typedef struct CallbackData {
    CPUClusterState *cluster;
    int cpu_count;
} CallbackData;

static bool add_cpu_to_cluster(Object *obj, void *opaque, Error **errp)
{
    CallbackData *cbdata = opaque;
    CPUState *cpu;

    if (cbdata->cluster->cpu_type == NULL) {
        /* If no 'cpu-type' property set, enforce it with the first CPU added */
        assert(object_dynamic_cast(obj, TYPE_CPU) != NULL);
        cbdata->cluster->cpu_type = g_strdup(object_get_typename(obj));
    }

    cpu = (CPUState *)object_dynamic_cast(obj, cbdata->cluster->cpu_type);
    if (!cpu) {
        error_setg(errp, "cluster %s can only accept %s CPUs (got %s)",
                   object_get_canonical_path(OBJECT(cbdata->cluster)),
                   cbdata->cluster->cpu_type, object_get_typename(obj));
        return false;
    }

    cpu->cluster_index = cbdata->cluster->cluster_id;
    cbdata->cpu_count++;

    return true;
}

static void cpu_cluster_realize(DeviceState *dev, Error **errp)
{
    /* Iterate through all our CPU children and set their cluster_index */
    CPUClusterState *cluster = CPU_CLUSTER(dev);
    Object *cluster_obj = OBJECT(dev);
    CallbackData cbdata = {
        .cluster = cluster,
        .cpu_count = 0,
    };

    if (cluster->cluster_id >= MAX_CLUSTERS) {
        error_setg(errp, "cluster-id must be less than %d", MAX_CLUSTERS);
        return;
    }
    if (cluster->cpu_type) {
        if (object_class_is_abstract(object_class_by_name(cluster->cpu_type))) {
            error_setg(errp, "cpu-type must be a concrete class");
            return;
        }
    }

    if (!object_child_foreach(cluster_obj, add_cpu_to_cluster, &cbdata, errp)) {
        return;
    }

    /*
     * A cluster with no CPUs is a bug in the board/SoC code that created it;
     * if you hit this during development of new code, check that you have
     * created the CPUs and parented them into the cluster object before
     * realizing the cluster object.
     */
    assert(cbdata.cpu_count > 0);
}

static void cpu_cluster_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, cpu_cluster_properties);
    dc->realize = cpu_cluster_realize;

    /* This is not directly for users, CPU children must be attached by code */
    dc->user_creatable = false;
}

static const TypeInfo cpu_cluster_type_info = {
    .name = TYPE_CPU_CLUSTER,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(CPUClusterState),
    .class_init = cpu_cluster_class_init,
};

static void cpu_cluster_register_types(void)
{
    type_register_static(&cpu_cluster_type_info);
}

type_init(cpu_cluster_register_types)
