/*
 * ARM TrustZone Address Space Controller TZC-400 emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_TZC400_H
#define HW_MISC_TZC400_H

#include "hw/sysbus.h"
#include "exec/hwaddr.h"
#include "exec/memory.h"

#define TYPE_TZC400 "tzc400"
OBJECT_DECLARE_SIMPLE_TYPE(TZC400State, TZC400)

#define TYPE_TZC400_IOMMU_MEMORY_REGION "tzc400-iommu-memory-region"

#define TZC400_MAX_FILTERS 4
#define TZC400_MAX_REGIONS 9

typedef struct TZC400Region {
    uint64_t base;
    uint64_t top;
    uint32_t attr;
    uint32_t id_access;
} TZC400Region;

typedef struct TZC400State {
    SysBusDevice parent_obj;

    MemoryRegion regs;
    IOMMUMemoryRegion upstream;
    MemoryRegion blocked_io;
    MemoryRegion *downstream;
    AddressSpace downstream_as;
    AddressSpace blocked_as;
    qemu_irq irq[TZC400_MAX_FILTERS];

    uint8_t num_filters;
    uint8_t num_regions;
    uint8_t addr_width;

    uint32_t action;
    uint32_t gate_keeper;
    uint32_t speculation_ctrl;
    uint32_t int_status;
    uint32_t int_clear;
    uint64_t fail_addr[TZC400_MAX_FILTERS];
    uint32_t fail_control[TZC400_MAX_FILTERS];
    uint32_t fail_id[TZC400_MAX_FILTERS];
    TZC400Region region[TZC400_MAX_REGIONS];
} TZC400State;

MemoryRegion *tzc400_get_upstream(TZC400State *s);

#endif
