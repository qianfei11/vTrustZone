/*
 * ARM TrustZone Address Space Controller TZC-400 emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "exec/target_page.h"
#include "hw/irq.h"
#include "hw/misc/tzc400.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TZC400_REG_WINDOW_SIZE     0x10000
#define TZC400_DEFAULT_ADDR_WIDTH  40
#define TZC400_DEFAULT_FILTERS     1
#define TZC400_DEFAULT_REGIONS     9
#define TZC400_REGION_STRIDE       0x20
#define TZC400_REGION_BASE         0x100

#define TZC400_ACTION_ERR          0x1
#define TZC400_ACTION_INT          0x2

#define TZC400_SEC_READ            0x1
#define TZC400_SEC_WRITE           0x2

enum {
    TZC400_IOMMU_IDX_SECURE = 0,
    TZC400_IOMMU_IDX_NS_BASE,
    TZC400_IOMMU_NUM_INDEXES = TZC400_IOMMU_IDX_NS_BASE + 16,
};

REG32(BUILD_CONFIG, 0x000)
    FIELD(BUILD_CONFIG, NR, 0, 5)
    FIELD(BUILD_CONFIG, AW, 8, 6)
    FIELD(BUILD_CONFIG, NF, 24, 2)
REG32(ACTION, 0x004)
    FIELD(ACTION, RV, 0, 2)
REG32(GATE_KEEPER, 0x008)
    FIELD(GATE_KEEPER, OR, 0, 4)
    FIELD(GATE_KEEPER, OS, 16, 4)
REG32(SPECULATION_CTRL, 0x00c)
REG32(INT_STATUS, 0x010)
REG32(INT_CLEAR, 0x014)
REG32(FAIL_ADDRESS_LOW, 0x020)
REG32(FAIL_ADDRESS_HIGH, 0x024)
REG32(FAIL_CONTROL, 0x028)
REG32(FAIL_ID, 0x02c)
REG32(REGION_ATTRIBUTES, 0x110)
    FIELD(REGION_ATTRIBUTES, F_EN, 0, 4)
    FIELD(REGION_ATTRIBUTES, SEC, 30, 2)
REG32(PID4, 0xfd0)
REG32(PID5, 0xfd4)
REG32(PID6, 0xfd8)
REG32(PID7, 0xfdc)
REG32(PID0, 0xfe0)
REG32(PID1, 0xfe4)
REG32(PID2, 0xfe8)
REG32(PID3, 0xfec)
REG32(CID0, 0xff0)
REG32(CID1, 0xff4)
REG32(CID2, 0xff8)
REG32(CID3, 0xffc)

static const uint8_t tzc400_idregs[] = {
    0x00, 0x00, 0x00, 0x00,
    0x60, 0xb4, 0x2b, 0x00,
    0x0d, 0xf0, 0x05, 0xb1,
};

static uint32_t tzc400_filter_mask(TZC400State *s)
{
    return (1u << s->num_filters) - 1;
}

static uint64_t tzc400_addr_mask(TZC400State *s)
{
    if (s->addr_width >= 64) {
        return UINT64_MAX;
    }

    return MAKE_64BIT_MASK(0, s->addr_width);
}

static bool tzc400_attrs_are_secure(MemTxAttrs attrs)
{
    return attrs.unspecified || attrs.secure;
}

static void tzc400_update_irq(TZC400State *s, unsigned filter)
{
    bool level;

    level = (s->action & TZC400_ACTION_INT) && (s->int_status & (1u << filter));
    qemu_set_irq(s->irq[filter], level);
}

static void tzc400_update_irqs(TZC400State *s)
{
    unsigned i;

    for (i = 0; i < s->num_filters; i++) {
        tzc400_update_irq(s, i);
    }
}

static void tzc400_iommu_replay_all(TZC400State *s)
{
    IOMMUNotifier *n;

    IOMMU_NOTIFIER_FOREACH(n, &s->upstream) {
        memory_region_iommu_replay(&s->upstream, n);
    }
}

static bool tzc400_region_enabled(TZC400State *s, unsigned region)
{
    if (region == 0) {
        return true;
    }

    return FIELD_EX32(s->region[region].attr, REGION_ATTRIBUTES, F_EN) != 0;
}

static bool tzc400_region_matches(TZC400Region *region, hwaddr addr)
{
    return addr >= region->base && addr <= region->top;
}

static TZC400Region *tzc400_find_region(TZC400State *s, hwaddr addr)
{
    int i;

    for (i = s->num_regions - 1; i >= 0; i--) {
        TZC400Region *region = &s->region[i];

        if (!tzc400_region_enabled(s, i)) {
            continue;
        }

        if (tzc400_region_matches(region, addr)) {
            return region;
        }
    }

    return NULL;
}

static void tzc400_record_failure(TZC400State *s, hwaddr addr,
                                  MemTxAttrs attrs, bool write)
{
    uint32_t fail = 0;

    if (write) {
        fail |= BIT(24);
    }
    if (!tzc400_attrs_are_secure(attrs)) {
        fail |= BIT(21);
    }
    if (!attrs.user) {
        fail |= BIT(20);
    }

    s->fail_addr[0] = addr;
    s->fail_control[0] = fail;
    s->fail_id[0] = attrs.requester_id & 0xffff;
    s->int_status |= BIT(0);
    tzc400_update_irq(s, 0);
}

static bool tzc400_access_check(TZC400State *s, hwaddr addr,
                                MemTxAttrs attrs, bool write)
{
    TZC400Region *region;
    bool secure = tzc400_attrs_are_secure(attrs);
    bool allowed = false;

    if (!(FIELD_EX32(s->gate_keeper, GATE_KEEPER, OR) & BIT(0))) {
        trace_tzc400_access(addr, secure, attrs.requester_id, write, false);
        return false;
    }

    region = tzc400_find_region(s, addr);
    if (!region) {
        trace_tzc400_access(addr, secure, attrs.requester_id, write, false);
        return false;
    }

    if (secure) {
        uint32_t sec = FIELD_EX32(region->attr, REGION_ATTRIBUTES, SEC);

        allowed = write ? (sec & TZC400_SEC_WRITE) : (sec & TZC400_SEC_READ);
    } else {
        uint32_t perms = write ? (region->id_access >> 16) : region->id_access;

        allowed = perms & BIT(attrs.requester_id & 0xf);
    }

    trace_tzc400_access(addr, secure, attrs.requester_id, write, allowed);
    return allowed;
}

static bool tzc400_decode_region_reg(hwaddr offset, unsigned *region,
                                     hwaddr *region_offset)
{
    unsigned index;

    if (offset < TZC400_REGION_BASE) {
        return false;
    }

    index = (offset - TZC400_REGION_BASE) / TZC400_REGION_STRIDE;
    *region_offset = (offset - TZC400_REGION_BASE) % TZC400_REGION_STRIDE;
    *region = index;
    return true;
}

static MemTxResult tzc400_reg_read(void *opaque, hwaddr addr, uint64_t *data,
                                   unsigned size, MemTxAttrs attrs)
{
    TZC400State *s = TZC400(opaque);
    hwaddr offset = addr & ~0x3;
    uint64_t value = 0;
    unsigned region;
    hwaddr region_offset;

    if (size != 4) {
        *data = 0;
        return MEMTX_ERROR;
    }

    if (!tzc400_attrs_are_secure(attrs) && offset < A_PID4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "TZC400 register read: NS access to offset 0x%" HWADDR_PRIx "\n",
                      offset);
        trace_tzc400_reg_read(addr, 0, size);
        *data = 0;
        return MEMTX_OK;
    }

    if (tzc400_decode_region_reg(offset, &region, &region_offset) &&
        region < s->num_regions) {
        TZC400Region *r = &s->region[region];

        switch (region_offset) {
        case 0x0:
            value = extract64(r->base, 0, 32);
            break;
        case 0x4:
            value = r->base >> 32;
            break;
        case 0x8:
            value = extract64(r->top, 0, 32);
            break;
        case 0xc:
            value = r->top >> 32;
            break;
        case 0x10:
            value = r->attr;
            break;
        case 0x14:
            value = r->id_access;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "TZC400 register read: bad region offset 0x%" HWADDR_PRIx "\n",
                          offset);
            value = 0;
            break;
        }
        trace_tzc400_reg_read(addr, value, size);
        *data = value;
        return MEMTX_OK;
    }

    switch (offset) {
    case A_BUILD_CONFIG:
        value = FIELD_DP32(0, BUILD_CONFIG, NF, s->num_filters - 1);
        value = FIELD_DP32(value, BUILD_CONFIG, AW, s->addr_width - 1);
        value = FIELD_DP32(value, BUILD_CONFIG, NR, s->num_regions - 1);
        break;
    case A_ACTION:
        value = s->action;
        break;
    case A_GATE_KEEPER:
        value = s->gate_keeper;
        break;
    case A_SPECULATION_CTRL:
        value = s->speculation_ctrl;
        break;
    case A_INT_STATUS:
        value = s->int_status;
        break;
    case A_FAIL_ADDRESS_LOW:
        value = extract64(s->fail_addr[0], 0, 32);
        break;
    case A_FAIL_ADDRESS_HIGH:
        value = s->fail_addr[0] >> 32;
        break;
    case A_FAIL_CONTROL:
        value = s->fail_control[0];
        break;
    case A_FAIL_ID:
        value = s->fail_id[0];
        break;
    case A_PID4:
    case A_PID5:
    case A_PID6:
    case A_PID7:
    case A_PID0:
    case A_PID1:
    case A_PID2:
    case A_PID3:
    case A_CID0:
    case A_CID1:
    case A_CID2:
    case A_CID3:
        value = tzc400_idregs[(offset - A_PID4) / 4];
        break;
    case A_INT_CLEAR:
        value = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "TZC400 register read: bad offset 0x%" HWADDR_PRIx "\n",
                      offset);
        value = 0;
        break;
    }

    trace_tzc400_reg_read(addr, value, size);
    *data = value;
    return MEMTX_OK;
}

static MemTxResult tzc400_reg_write(void *opaque, hwaddr addr, uint64_t value,
                                    unsigned size, MemTxAttrs attrs)
{
    TZC400State *s = TZC400(opaque);
    hwaddr offset = addr & ~0x3;
    unsigned region;
    hwaddr region_offset;

    trace_tzc400_reg_write(addr, value, size);

    if (size != 4) {
        return MEMTX_ERROR;
    }

    if (!tzc400_attrs_are_secure(attrs) && offset < A_PID4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "TZC400 register write: NS access to offset 0x%" HWADDR_PRIx "\n",
                      offset);
        return MEMTX_OK;
    }

    if (tzc400_decode_region_reg(offset, &region, &region_offset) &&
        region < s->num_regions) {
        TZC400Region *r = &s->region[region];

        switch (region_offset) {
        case 0x0:
            r->base = deposit64(r->base, 0, 32, value) & tzc400_addr_mask(s);
            break;
        case 0x4:
            r->base = deposit64(r->base, 32, 32, value) & tzc400_addr_mask(s);
            break;
        case 0x8:
            r->top = deposit64(r->top, 0, 32, value) & tzc400_addr_mask(s);
            break;
        case 0xc:
            r->top = deposit64(r->top, 32, 32, value) & tzc400_addr_mask(s);
            break;
        case 0x10:
            r->attr = value;
            break;
        case 0x14:
            r->id_access = value;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "TZC400 register write: bad region offset 0x%" HWADDR_PRIx "\n",
                          offset);
            return MEMTX_OK;
        }

        tzc400_iommu_replay_all(s);
        return MEMTX_OK;
    }

    switch (offset) {
    case A_ACTION:
        s->action = value & R_ACTION_RV_MASK;
        tzc400_update_irqs(s);
        break;
    case A_GATE_KEEPER: {
        uint32_t open = value & tzc400_filter_mask(s);

        s->gate_keeper = open;
        s->gate_keeper = FIELD_DP32(s->gate_keeper, GATE_KEEPER, OS, open);
        tzc400_iommu_replay_all(s);
        break;
    }
    case A_SPECULATION_CTRL:
        s->speculation_ctrl = value;
        break;
    case A_INT_CLEAR:
        s->int_clear = value & tzc400_filter_mask(s);
        s->int_status &= ~s->int_clear;
        tzc400_update_irqs(s);
        break;
    case A_BUILD_CONFIG:
    case A_INT_STATUS:
    case A_FAIL_ADDRESS_LOW:
    case A_FAIL_ADDRESS_HIGH:
    case A_FAIL_CONTROL:
    case A_FAIL_ID:
    case A_PID4:
    case A_PID5:
    case A_PID6:
    case A_PID7:
    case A_PID0:
    case A_PID1:
    case A_PID2:
    case A_PID3:
    case A_CID0:
    case A_CID1:
    case A_CID2:
    case A_CID3:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "TZC400 register write: bad offset 0x%" HWADDR_PRIx "\n",
                      offset);
        break;
    }

    return MEMTX_OK;
}

static const MemoryRegionOps tzc400_reg_ops = {
    .read_with_attrs = tzc400_reg_read,
    .write_with_attrs = tzc400_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static MemTxResult tzc400_blocked_read(void *opaque, hwaddr addr, uint64_t *data,
                                       unsigned size, MemTxAttrs attrs)
{
    (void)opaque;
    (void)addr;
    (void)size;
    (void)attrs;
    *data = 0;
    return MEMTX_ERROR;
}

static MemTxResult tzc400_blocked_write(void *opaque, hwaddr addr, uint64_t value,
                                        unsigned size, MemTxAttrs attrs)
{
    (void)opaque;
    (void)addr;
    (void)value;
    (void)size;
    (void)attrs;
    return MEMTX_ERROR;
}

static const MemoryRegionOps tzc400_blocked_ops = {
    .read_with_attrs = tzc400_blocked_read,
    .write_with_attrs = tzc400_blocked_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static IOMMUTLBEntry tzc400_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                      IOMMUAccessFlags flags, int iommu_idx)
{
    TZC400State *s = TZC400(container_of(iommu, TZC400State, upstream));
    MemTxAttrs attrs = {};
    bool write = flags & IOMMU_WO;
    bool allowed;

    if (iommu_idx == TZC400_IOMMU_IDX_SECURE) {
        attrs.secure = true;
    } else {
        attrs.requester_id = iommu_idx - TZC400_IOMMU_IDX_NS_BASE;
    }

    allowed = tzc400_access_check(s, addr, attrs, write);
    if (!allowed) {
        tzc400_record_failure(s, addr, attrs, write);
    }

    return (IOMMUTLBEntry) {
        .target_as = allowed ? &s->downstream_as : &s->blocked_as,
        .iova = addr,
        .translated_addr = addr,
        .addr_mask = TARGET_PAGE_SIZE - 1,
        .perm = allowed ? IOMMU_RW : IOMMU_NONE,
    };
}

static int tzc400_attrs_to_index(IOMMUMemoryRegion *iommu, MemTxAttrs attrs)
{
    (void)iommu;

    if (attrs.unspecified || attrs.secure) {
        return TZC400_IOMMU_IDX_SECURE;
    }

    return TZC400_IOMMU_IDX_NS_BASE + (attrs.requester_id & 0xf);
}

static int tzc400_num_indexes(IOMMUMemoryRegion *iommu)
{
    (void)iommu;
    return TZC400_IOMMU_NUM_INDEXES;
}

static void tzc400_reset(DeviceState *dev)
{
    TZC400State *s = TZC400(dev);

    s->num_filters = TZC400_DEFAULT_FILTERS;
    s->num_regions = TZC400_DEFAULT_REGIONS;
    s->addr_width = TZC400_DEFAULT_ADDR_WIDTH;
    s->action = 0;
    s->gate_keeper = 0;
    s->speculation_ctrl = 0;
    s->int_status = 0;
    s->int_clear = 0;
    memset(s->fail_addr, 0, sizeof(s->fail_addr));
    memset(s->fail_control, 0, sizeof(s->fail_control));
    memset(s->fail_id, 0, sizeof(s->fail_id));
    memset(s->region, 0, sizeof(s->region));

    s->region[0].top = tzc400_addr_mask(s);
    s->region[0].attr = FIELD_DP32(0, REGION_ATTRIBUTES, SEC,
                                   TZC400_SEC_READ | TZC400_SEC_WRITE);
    s->region[0].id_access = UINT32_MAX;

    tzc400_update_irqs(s);
}

static void tzc400_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    TZC400State *s = TZC400(obj);

    qdev_init_gpio_out_named(dev, s->irq, "irq", TZC400_MAX_FILTERS);
}

static void tzc400_realize(DeviceState *dev, Error **errp)
{
    Object *obj = OBJECT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    TZC400State *s = TZC400(dev);
    uint64_t size;

    if (!s->downstream) {
        error_setg(errp, "TZC400 'downstream' link not set");
        return;
    }

    size = memory_region_size(s->downstream);

    memory_region_init_iommu(&s->upstream, sizeof(s->upstream),
                             TYPE_TZC400_IOMMU_MEMORY_REGION, obj,
                             "tzc400-upstream", size);

    memory_region_init_io(&s->regs, obj, &tzc400_reg_ops, s,
                          "tzc400-regs", TZC400_REG_WINDOW_SIZE);
    sysbus_init_mmio(sbd, &s->regs);
    sysbus_init_mmio(sbd, MEMORY_REGION(&s->upstream));

    memory_region_init_io(&s->blocked_io, obj, &tzc400_blocked_ops, s,
                          "tzc400-blocked-io", size);

    address_space_init(&s->downstream_as, s->downstream, "tzc400-downstream");
    address_space_init(&s->blocked_as, &s->blocked_io, "tzc400-blocked-io");
}

static const VMStateDescription vmstate_tzc400_region = {
    .name = "tzc400-region",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(base, TZC400Region),
        VMSTATE_UINT64(top, TZC400Region),
        VMSTATE_UINT32(attr, TZC400Region),
        VMSTATE_UINT32(id_access, TZC400Region),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_tzc400 = {
    .name = "tzc400",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(num_filters, TZC400State),
        VMSTATE_UINT8(num_regions, TZC400State),
        VMSTATE_UINT8(addr_width, TZC400State),
        VMSTATE_UINT32(action, TZC400State),
        VMSTATE_UINT32(gate_keeper, TZC400State),
        VMSTATE_UINT32(speculation_ctrl, TZC400State),
        VMSTATE_UINT32(int_status, TZC400State),
        VMSTATE_UINT32(int_clear, TZC400State),
        VMSTATE_UINT64_ARRAY(fail_addr, TZC400State, TZC400_MAX_FILTERS),
        VMSTATE_UINT32_ARRAY(fail_control, TZC400State, TZC400_MAX_FILTERS),
        VMSTATE_UINT32_ARRAY(fail_id, TZC400State, TZC400_MAX_FILTERS),
        VMSTATE_STRUCT_ARRAY(region, TZC400State, TZC400_MAX_REGIONS, 1,
                             vmstate_tzc400_region, TZC400Region),
        VMSTATE_END_OF_LIST()
    }
};

static const Property tzc400_properties[] = {
    DEFINE_PROP_LINK("downstream", TZC400State, downstream,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

MemoryRegion *tzc400_get_upstream(TZC400State *s)
{
    return MEMORY_REGION(&s->upstream);
}

static void tzc400_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tzc400_realize;
    dc->vmsd = &vmstate_tzc400;
    device_class_set_legacy_reset(dc, tzc400_reset);
    device_class_set_props(dc, tzc400_properties);
}

static const TypeInfo tzc400_info = {
    .name = TYPE_TZC400,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TZC400State),
    .instance_init = tzc400_init,
    .class_init = tzc400_class_init,
};

static void tzc400_iommu_memory_region_class_init(ObjectClass *klass,
                                                  void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = tzc400_translate;
    imrc->attrs_to_index = tzc400_attrs_to_index;
    imrc->num_indexes = tzc400_num_indexes;
}

static const TypeInfo tzc400_iommu_memory_region_info = {
    .name = TYPE_TZC400_IOMMU_MEMORY_REGION,
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .class_init = tzc400_iommu_memory_region_class_init,
};

static void tzc400_register_types(void)
{
    type_register_static(&tzc400_info);
    type_register_static(&tzc400_iommu_memory_region_info);
}

type_init(tzc400_register_types);
