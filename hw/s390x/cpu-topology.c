/*
 * CPU Topology
 *
 * Copyright IBM Corp. 2022
 * Author(s): Pierre Morel <pmorel@linux.ibm.com>

 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/qdev-properties.h"
#include "hw/boards.h"
#include "qemu/typedefs.h"
#include "target/s390x/cpu.h"
#include "hw/s390x/s390-virtio-ccw.h"
#include "hw/s390x/cpu-topology.h"
#include "qapi/qapi-types-machine-target.h"
#include "qapi/qapi-types-machine.h"
#include "qapi/qapi-commands-machine-target.h"
#include "qapi/qapi-events-machine-target.h"
#include "qapi/qmp/qdict.h"
#include "monitor/hmp.h"
#include "monitor/monitor.h"

/*
 * s390_topology is used to keep the topology information.
 * .cores_per_socket: tracks information on the count of cores
 *                    per socket.
 * .smp: keeps track of the machine topology.
 * .list: queue the topology entries inside which
 *        we keep the information on the CPU topology.
 * .polarization: the current subsystem polarization
 *
 */
S390Topology s390_topology = {
    /* will be initialized after the cpu model is realized */
    .cores_per_socket = NULL,
    .smp = NULL,
    .polarization = S390_CPU_POLARIZATION_HORIZONTAL,
    .list = QTAILQ_HEAD_INITIALIZER(s390_topology.list),
};

/**
 * s390_socket_nb:
 * @cpu: s390x CPU
 *
 * Returns the socket number used inside the cores_per_socket array
 * for a cpu.
 */
int s390_socket_nb(S390CPU *cpu)
{
    return (cpu->env.drawer_id * s390_topology.smp->books + cpu->env.book_id) *
           s390_topology.smp->sockets + cpu->env.socket_id;
}

/**
 * s390_has_topology:
 *
 * Return value: if the topology is supported by the machine.
 */
bool s390_has_topology(void)
{
    return s390_has_feat(S390_FEAT_CONFIGURATION_TOPOLOGY);
}

/**
 * s390_topology_init:
 * @ms: the machine state where the machine topology is defined
 *
 * Keep track of the machine topology.
 *
 * Allocate an array to keep the count of cores per socket.
 * The index of the array starts at socket 0 from book 0 and
 * drawer 0 up to the maximum allowed by the machine topology.
 *
 * Insert a sentinel entry using unused level5 with its maximum value.
 * This entry will never be free.
 */
static void s390_topology_init(MachineState *ms)
{
    CpuTopology *smp = &ms->smp;
    S390TopologyEntry *entry;

    s390_topology.smp = smp;
    s390_topology.cores_per_socket = g_new0(uint8_t, smp->sockets *
                                            smp->books * smp->drawers);

    entry = g_malloc0(sizeof(S390TopologyEntry));
    entry->id.level5 = 0xff;
    QTAILQ_INSERT_HEAD(&s390_topology.list, entry, next);
}

/**
 * s390_topology_set_cpus_entitlement:
 * @polarization: polarization requested by the caller
 *
 * Set all CPU entitlement according to polarization and
 * dedication.
 * Default vertical entitlement is S390_CPU_ENTITLEMENT_MEDIUM as
 * it does not require host modification of the CPU provisioning
 * until the host decide to modify individual CPU provisioning
 * using QAPI interface.
 * However a dedicated vCPU will have a S390_CPU_ENTITLEMENT_HIGH
 * entitlement.
 */
static void s390_topology_set_cpus_entitlement(int polarization)
{
    CPUState *cs;

    CPU_FOREACH(cs) {
        if (polarization == S390_CPU_POLARIZATION_HORIZONTAL) {
            S390_CPU(cs)->env.entitlement = 0;
        } else if (S390_CPU(cs)->env.dedicated) {
            S390_CPU(cs)->env.entitlement = S390_CPU_ENTITLEMENT_HIGH;
        } else {
            S390_CPU(cs)->env.entitlement = S390_CPU_ENTITLEMENT_MEDIUM;
        }
    }
}

/*
 * s390_handle_ptf:
 *
 * @register 1: contains the function code
 *
 * Function codes 0 (horizontal) and 1 (vertical) define the CPU
 * polarization requested by the guest.
 *
 * Verify that the polarization really need to change and call
 * s390_topology_set_cpus_entitlement() specifying the requested polarization
 * to set for all CPUs.
 *
 * Function code 2 is handling topology changes and is interpreted
 * by the SIE.
 */
void s390_handle_ptf(S390CPU *cpu, uint8_t r1, uintptr_t ra)
{
    CPUS390XState *env = &cpu->env;
    uint64_t reg = env->regs[r1];
    int fc = reg & S390_TOPO_FC_MASK;

    if (!s390_has_feat(S390_FEAT_CONFIGURATION_TOPOLOGY)) {
        s390_program_interrupt(env, PGM_OPERATION, ra);
        return;
    }

    if (env->psw.mask & PSW_MASK_PSTATE) {
        s390_program_interrupt(env, PGM_PRIVILEGED, ra);
        return;
    }

    if (reg & ~S390_TOPO_FC_MASK) {
        s390_program_interrupt(env, PGM_SPECIFICATION, ra);
        return;
    }

    switch (fc) {
    case S390_CPU_POLARIZATION_VERTICAL:
    case S390_CPU_POLARIZATION_HORIZONTAL:
        if (s390_topology.polarization == fc) {
            env->regs[r1] |= S390_PTF_REASON_DONE;
            setcc(cpu, 2);
        } else {
            s390_topology.polarization = fc;
            s390_cpu_topology_set_changed(true);
            s390_topology_set_cpus_entitlement(fc);
            qapi_event_send_cpu_polarization_change(fc);
            setcc(cpu, 0);
        }
        break;
    default:
        /* Note that fc == 2 is interpreted by the SIE */
        s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }
}

/**
 * s390_topology_reset:
 *
 * Generic reset for CPU topology, calls s390_topology_reset()
 * to reset the kernel Modified Topology Change Record.
 */
void s390_topology_reset(void)
{
    s390_cpu_topology_set_changed(false);
    s390_topology.polarization = S390_CPU_POLARIZATION_HORIZONTAL;
    s390_topology_set_cpus_entitlement(S390_CPU_POLARIZATION_HORIZONTAL);
}

/**
 * s390_topology_cpu_default:
 * @cpu: pointer to a S390CPU
 * @errp: Error pointer
 *
 * Setup the default topology if no attributes are already set.
 * Passing a CPU with some, but not all, attributes set is considered
 * an error.
 *
 * The function calculates the (drawer_id, book_id, socket_id)
 * topology by filling the cores starting from the first socket
 * (0, 0, 0) up to the last (smp->drawers, smp->books, smp->sockets).
 *
 * CPU type, entitlement and dedication have defaults values set in the
 * s390x_cpu_properties, however entitlement is forced to 0 'none' when
 * the polarization is horizontale.
 */
static void s390_topology_cpu_default(S390CPU *cpu, Error **errp)
{
    CpuTopology *smp = s390_topology.smp;
    CPUS390XState *env = &cpu->env;

    /* All geometry topology attributes must be set or all unset */
    if ((env->socket_id < 0 || env->book_id < 0 || env->drawer_id < 0) &&
        (env->socket_id >= 0 || env->book_id >= 0 || env->drawer_id >= 0)) {
        error_setg(errp,
                   "Please define all or none of the topology geometry attributes");
        return;
    }

    /* Check if one of the geometry topology is unset */
    if (env->socket_id < 0) {
        /* Calculate default geometry topology attributes */
        env->socket_id = s390_std_socket(env->core_id, smp);
        env->book_id = s390_std_book(env->core_id, smp);
        env->drawer_id = s390_std_drawer(env->core_id, smp);
    }

    if (s390_topology.polarization == S390_CPU_POLARIZATION_HORIZONTAL) {
        env->entitlement = 0;
    }
}

/**
 * s390_topology_check:
 * @socket_id: socket to check
 * @book_id: book to check
 * @drawer_id: drawer to check
 * @entitlement: entitlement to check
 * @dedicated: dedication to check
 * @errp: Error pointer
 *
 * The function first setup default values and then checks if the topology
 * attributes fits inside the system topology.
 */
static void s390_topology_check(uint16_t socket_id, uint16_t book_id,
                                uint16_t drawer_id, uint16_t entitlement,
                                bool dedicated, Error **errp)
{
    CpuTopology *smp = s390_topology.smp;
    ERRP_GUARD();

    if (socket_id >= smp->sockets) {
        error_setg(errp, "Unavailable socket: %d", socket_id);
        return;
    }
    if (book_id >= smp->books) {
        error_setg(errp, "Unavailable book: %d", book_id);
        return;
    }
    if (drawer_id >= smp->drawers) {
        error_setg(errp, "Unavailable drawer: %d", drawer_id);
        return;
    }
    if (entitlement >= S390_CPU_ENTITLEMENT__MAX) {
        error_setg(errp, "Unknown entitlement: %d", entitlement);
        return;
    }
    if (dedicated && (entitlement == S390_CPU_ENTITLEMENT_LOW ||
                      entitlement == S390_CPU_ENTITLEMENT_MEDIUM)) {
        error_setg(errp, "A dedicated cpu implies high entitlement");
        return;
    }
}

/**
 * s390_topology_add_core_to_socket:
 * @cpu: the new S390CPU to insert in the topology structure
 * @drawer_id: new drawer_id
 * @book_id: new book_id
 * @socket_id: new socket_id
 * @creation: if is true the CPU is a new CPU and there is no old socket
 *            to handle.
 *            if is false, this is a moving the CPU and old socket count
 *            must be decremented.
 * @errp: the error pointer
 *
 */
static void s390_topology_add_core_to_socket(S390CPU *cpu, int drawer_id,
                                             int book_id, int socket_id,
                                             bool creation, Error **errp)
{
    int old_socket_entry = s390_socket_nb(cpu);
    int new_socket_entry;

    if (creation) {
        new_socket_entry = old_socket_entry;
    } else {
        new_socket_entry = (drawer_id * s390_topology.smp->books + book_id) *
                            s390_topology.smp->sockets + socket_id;
    }

    /* Check for space on new socket */
    if ((new_socket_entry != old_socket_entry) &&
        (s390_topology.cores_per_socket[new_socket_entry] >=
         s390_topology.smp->cores)) {
        error_setg(errp, "No more space on this socket");
        return;
    }

    /* Update the count of cores in sockets */
    s390_topology.cores_per_socket[new_socket_entry] += 1;
    if (!creation) {
        s390_topology.cores_per_socket[old_socket_entry] -= 1;
    }
}

/**
 * s390_topology_need_report
 * @cpu: Current cpu
 * @drawer_id: future drawer ID
 * @book_id: future book ID
 * @socket_id: future socket ID
 *
 * A modified topology change report is needed if the
 */
static int s390_topology_need_report(S390CPU *cpu, int drawer_id,
                                   int book_id, int socket_id,
                                   uint16_t entitlement, bool dedicated)
{
    return cpu->env.drawer_id != drawer_id ||
           cpu->env.book_id != book_id ||
           cpu->env.socket_id != socket_id ||
           cpu->env.entitlement != entitlement ||
           cpu->env.dedicated != dedicated;
}

/**
 * s390_update_cpu_props:
 * @ms: the machine state
 * @cpu: the CPU for which to update the properties from the environment.
 *
 */
static void s390_update_cpu_props(MachineState *ms, S390CPU *cpu)
{
    CpuInstanceProperties *props;

    props = &ms->possible_cpus->cpus[cpu->env.core_id].props;

    props->socket_id = cpu->env.socket_id;
    props->book_id = cpu->env.book_id;
    props->drawer_id = cpu->env.drawer_id;
}

/**
 * s390_topology_setup_cpu:
 * @ms: MachineState used to initialize the topology structure on
 *      first call.
 * @cpu: the new S390CPU to insert in the topology structure
 * @errp: the error pointer
 *
 * Called from CPU Hotplug to check and setup the CPU attributes
 * before to insert the CPU in the topology.
 * There is no use to update the MTCR explicitely here because it
 * will be updated by KVM on creation of the new vCPU.
 */
void s390_topology_setup_cpu(MachineState *ms, S390CPU *cpu, Error **errp)
{
    ERRP_GUARD();

    /*
     * We do not want to initialize the topology if the cpu model
     * does not support topology, consequently, we have to wait for
     * the first CPU to be realized, which realizes the CPU model
     * to initialize the topology structures.
     *
     * s390_topology_setup_cpu() is called from the cpu hotplug.
     */
    if (!s390_topology.cores_per_socket) {
        s390_topology_init(ms);
    }

    s390_topology_cpu_default(cpu, errp);
    if (*errp) {
        return;
    }

    s390_topology_check(cpu->env.socket_id, cpu->env.book_id,
                        cpu->env.drawer_id, cpu->env.entitlement,
                        cpu->env.dedicated, errp);
    if (*errp) {
        return;
    }

    /* Set the CPU inside the socket */
    s390_topology_add_core_to_socket(cpu, 0, 0, 0, true, errp);
    if (*errp) {
        return;
    }

    /* topology tree is reflected in props */
    s390_update_cpu_props(ms, cpu);
}

/*
 * qmp and hmp implementations
 */

#define TOPOLOGY_SET(n) do {                                \
                            if (has_ ## n) {                \
                                calc_ ## n = n;             \
                            } else {                        \
                                calc_ ## n = cpu->env.n;    \
                            }                               \
                        } while (0)

static void s390_change_topology(uint16_t core_id,
                                 bool has_socket_id, uint16_t socket_id,
                                 bool has_book_id, uint16_t book_id,
                                 bool has_drawer_id, uint16_t drawer_id,
                                 bool has_entitlement, uint16_t entitlement,
                                 bool has_dedicated, bool dedicated,
                                 Error **errp)
{
    MachineState *ms = current_machine;
    uint16_t calc_dedicated, calc_entitlement;
    uint16_t calc_socket_id, calc_book_id, calc_drawer_id;
    S390CPU *cpu;
    int report_needed;
    ERRP_GUARD();

    if (core_id >= ms->smp.max_cpus) {
        error_setg(errp, "Core-id %d out of range!", core_id);
        return;
    }

    cpu = (S390CPU *)ms->possible_cpus->cpus[core_id].cpu;
    if (!cpu) {
        error_setg(errp, "Core-id %d does not exist!", core_id);
        return;
    }

    /* Get unprovided attributes from cpu and verify the new topology */
    TOPOLOGY_SET(entitlement);
    TOPOLOGY_SET(dedicated);
    TOPOLOGY_SET(socket_id);
    TOPOLOGY_SET(book_id);
    TOPOLOGY_SET(drawer_id);

    s390_topology_check(calc_socket_id, calc_book_id, calc_drawer_id,
                        calc_entitlement, calc_dedicated, errp);
    if (*errp) {
        return;
    }

    /* Move the CPU into its new socket */
    s390_topology_add_core_to_socket(cpu, calc_drawer_id, calc_book_id,
                                     calc_socket_id, false, errp);
    if (*errp) {
        return;
    }

    /* Check if we need to report the modified topology */
    report_needed = s390_topology_need_report(cpu, calc_drawer_id, calc_book_id,
                                              calc_socket_id, calc_entitlement,
                                              calc_dedicated);

    /* All checks done, report new topology into the vCPU */
    cpu->env.drawer_id = calc_drawer_id;
    cpu->env.book_id = calc_book_id;
    cpu->env.socket_id = calc_socket_id;
    cpu->env.dedicated = calc_dedicated;
    cpu->env.entitlement = calc_entitlement;

    /* topology tree is reflected in props */
    s390_update_cpu_props(ms, cpu);

    /* Advertise the topology change */
    if (report_needed) {
        s390_cpu_topology_set_changed(true);
    }
}

void qmp_set_cpu_topology(uint16_t core,
                         bool has_socket, uint16_t socket,
                         bool has_book, uint16_t book,
                         bool has_drawer, uint16_t drawer,
                         const char *entitlement_str,
                         bool has_dedicated, bool dedicated,
                         Error **errp)
{
    bool has_entitlement = false;
    int entitlement;
    ERRP_GUARD();

    if (!s390_has_topology()) {
        error_setg(errp, "This machine doesn't support topology");
        return;
    }

    entitlement = qapi_enum_parse(&CpuS390Entitlement_lookup, entitlement_str,
                                  -1, errp);
    if (*errp) {
        return;
    }
    has_entitlement = entitlement >= 0;

    s390_change_topology(core, has_socket, socket, has_book, book,
                         has_drawer, drawer, has_entitlement, entitlement,
                         has_dedicated, dedicated, errp);
}

void hmp_set_cpu_topology(Monitor *mon, const QDict *qdict)
{
    const uint16_t core = qdict_get_int(qdict, "core-id");
    bool has_socket    = qdict_haskey(qdict, "socket-id");
    const uint16_t socket = qdict_get_try_int(qdict, "socket-id", 0);
    bool has_book    = qdict_haskey(qdict, "book-id");
    const uint16_t book = qdict_get_try_int(qdict, "book-id", 0);
    bool has_drawer    = qdict_haskey(qdict, "drawer-id");
    const uint16_t drawer = qdict_get_try_int(qdict, "drawer-id", 0);
    const char *entitlement = qdict_get_try_str(qdict, "entitlement");
    bool has_dedicated    = qdict_haskey(qdict, "dedicated");
    const bool dedicated = qdict_get_try_bool(qdict, "dedicated", false);
    Error *local_err = NULL;

    qmp_set_cpu_topology(core, has_socket, socket, has_book, book,
                           has_drawer, drawer, entitlement,
                           has_dedicated, dedicated, &local_err);
    hmp_handle_error(mon, local_err);
}
