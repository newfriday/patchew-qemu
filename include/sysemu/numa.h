#ifndef SYSEMU_NUMA_H
#define SYSEMU_NUMA_H

#include "qemu/bitmap.h"
#include "sysemu/sysemu.h"
#include "sysemu/hostmem.h"
#include "hw/boards.h"

extern bool have_numa_distance;

struct NodeInfo {
    uint64_t node_mem;
    struct HostMemoryBackend *node_memdev;
    bool present;
    uint8_t distance[MAX_NODES];
};

extern NodeInfo numa_info[MAX_NODES];

struct NumaNodeMem {
    uint64_t node_mem;
    uint64_t node_plugged_mem;
};

struct NumaState {
    /* Number of NUMA nodes */
    int num_nodes;

};
typedef struct NumaState NumaState;

void parse_numa_opts(MachineState *ms);
void numa_complete_configuration(MachineState *ms);
void query_numa_node_mem(NumaNodeMem node_mem[], MachineState *ms);
extern QemuOptsList qemu_numa_opts;
void numa_legacy_auto_assign_ram(MachineClass *mc, NodeInfo *nodes,
                                 int nb_nodes, ram_addr_t size);
void numa_default_auto_assign_ram(MachineClass *mc, NodeInfo *nodes,
                                  int nb_nodes, ram_addr_t size);
void numa_cpu_pre_plug(const CPUArchId *slot, DeviceState *dev, Error **errp);
#endif
