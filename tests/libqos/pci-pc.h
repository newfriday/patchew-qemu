/*
 * libqos PCI bindings for PC
 *
 * Copyright IBM, Corp. 2012-2013
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef LIBQOS_PCI_PC_H
#define LIBQOS_PCI_PC_H

#include "libqos/pci.h"
#include "libqos/malloc.h"

/* qpci_pc_new():
 * @qts: The %QTestState for this PC machine
 * @alloc: A previously initialized @alloc providing memory for @qts
 *
 * This function creates a new QPCIBusPC object,
 * and properly initialize its fields.
 *
 * Returns a newly allocated QPCIBus.
 */
QPCIBus *qpci_new_pc(QTestState *qts, QGuestAllocator *alloc);

void     qpci_free_pc(QPCIBus *bus);

#endif
