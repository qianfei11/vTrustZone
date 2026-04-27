# QEMU TZC-400 Core Isolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a configurable QEMU TZC-400 model for the `virt` TrustZone machine and use per-core requester IDs to demonstrate core-level isolation for OP-TEE/QEMU experiments.

**Architecture:** Model TZC-400 as a QEMU SysBus device with a secure-only register MMIO window and an IOMMU-like upstream MemoryRegion that protects the `virt` machine's non-secure DRAM (`VIRT_MEM`). Reuse QEMU's existing `MemTxAttrs.secure` field for TrustZone security state and `MemTxAttrs.requester_id` as the TZC-400 Non-Secure Access ID (NSAID). Assign each emulated CPU a static NSAID through a `virt` machine property, then expose the existing TF-A/OP-TEE TZC-400 driver APIs through a small QEMU platform SMC and a Linux test interface.

**Tech Stack:** QEMU 10.0.0 ARM `virt` machine, QEMU MemoryRegion/IOMMU APIs, QEMU TCG ARM softmmu, TF-A `drivers/arm/tzc/tzc400.c`, OP-TEE `core/drivers/tzc400.c`, Linux SMCCC helper APIs, OP-TEE build `build/qemu_v8.mk`.

---

## Paper-Derived Design

The plan is grounded in these local sources:

- `papers/sec17-hua.pdf` (`vTZ`): virtualize TrustZone by preserving secure boot, secure/normal CPU state separation, secure memory partitioning, secure peripheral partitioning, and secure interrupt dispatch. The implementation pattern is trap-and-emulate for virtual TZ controllers while the real secure monitor validates partition changes.
- `papers/kvTZ_TrustZone_Virtualization_for_Commodity_Arm-Based_Platforms.pdf`: expose virtual TrustZone to guests by multiplexing world state and giving each world distinct memory views. For QEMU, this motivates using the existing secure/non-secure address-space split and a virtual TrustZone memory controller instead of creating an unrelated protection path.
- `papers/ndss2019_01A-1_Brasser_paper.pdf` (`Sanctuary`): use TZASC/TZC plus bus-master identity filtering to isolate normal-world compartments, including compartments bound to specific cores.
- `papers/date22_paper_main.pdf` (`SafeTEE`): bind isolated normal-world partitions to dedicated cores and configure memory access before untrusted code runs.
- `papers/Securing Edge System Accelerators - A Design Evaluation Using ARM Fast Models.pdf`: use TZC-400 region registers plus NSAID permissions for core/device isolation. The key mechanism is region base/top/attributes plus `REGION_ID_ACCESS`, where each non-secure master has an NSAID and only matching NSAIDs can read/write a region.

The resulting implementation target is functional access-control isolation in QEMU TCG. It does not claim cache/timing side-channel modeling, and it does not target KVM/HVF because QEMU `virt,secure=on` already rejects those accelerators.

## Repository Integration Points

- QEMU `virt` has security extensions through separate normal and secure address spaces in `qemu/hw/arm/virt.c`.
- `qemu/include/exec/memattrs.h` already defines `MemTxAttrs.secure` and `MemTxAttrs.requester_id`.
- `qemu/hw/misc/tz-mpc.c` is the local pattern for a TrustZone memory protection device implemented as an IOMMU MemoryRegion.
- The `virt` low MMIO map has an unused hole after secure GPIO:
  - `VIRT_SECURE_GPIO`: `0x090b0000` size `0x1000`
  - `VIRT_MMIO`: `0x0a000000` size `0x200`
  - Use `VIRT_TZC400`: `0x090c0000` size `0x10000`
- TF-A already provides `tzc400_init()`, `tzc400_configure_region0()`, `tzc400_configure_region()`, `tzc400_set_action()`, and `tzc400_enable_filters()`.
- OP-TEE already has `CFG_TZC400` support in `optee_os/core/arch/arm/plat-vexpress/main.c`, but `PLATFORM_FLAVOR_qemu_armv8a` lacks `TZC400_BASE`.

## Current Repository Status

As of 2026-04-27, this branch implements the full planned functional path:

- QEMU provides the TZC-400 device model, register tests, `virt` machine wiring, secure FDT node, per-CPU `tzc-nsaid` property, and TCG propagation through `MemTxAttrs.requester_id`.
- TF-A provides QEMU platform TZC-400 initialization and the SiP SMC interface used to configure regions from the normal world.
- The build wrapper exposes `QEMU_TZC400=y`, passes TF-A/OP-TEE flags, enables the QEMU machine property, and rejects SBSA where the QEMU TZC base is not defined.
- Linux exposes `/dev/qemu_tzc400` for the test flow, and `optee_examples/qemu_tzc_core_isolation` drives a core-allowed/core-denied isolation check.
- The TZC-400 model includes an `addr-base` property so IOMMU offsets inside `VIRT_MEM` are checked and reported as guest physical addresses.
- TZC-400 policy writes invalidate cached IOMMU translations before replaying allowed mappings, so stale TCG TLB entries cannot bypass a newly restricted region.

## File Structure

### QEMU

- Create `qemu/include/hw/misc/tzc400.h`: QOM type, state struct, public accessor for the protected upstream MemoryRegion.
- Create `qemu/hw/misc/tzc400.c`: register model, region matching, access checks, failure status, IOMMU translation, QOM realization, and protected-window physical base handling.
- Modify `qemu/hw/misc/meson.build`: add `tzc400.c` under `CONFIG_TZC400`.
- Modify `qemu/hw/misc/Kconfig`: add `config TZC400`.
- Modify `qemu/hw/misc/trace-events`: add tracepoints for register writes, access checks, and denials.
- Modify `qemu/include/hw/arm/virt.h`: add `VIRT_TZC400` and machine state fields for TZC-400 enablement and CPU NSAIDs.
- Modify `qemu/hw/arm/virt.c`: add `tzc400` machine properties, instantiate the device, wire the protected DRAM MemoryRegion, add secure FDT node, and assign CPU NSAIDs.
- Modify `qemu/hw/arm/Kconfig`: select `TZC400` from `ARM_VIRT` when enabled by build config.
- Modify `qemu/target/arm/cpu.h`: add `uint16_t tzc_nsaid` to `ARMCPU`.
- Modify `qemu/target/arm/cpu.c`: add QOM property `tzc-nsaid`.
- Modify `qemu/target/arm/tcg/tlb_helper.c`: copy `cpu->tzc_nsaid` into `res.f.attrs.requester_id` before installing TCG TLB entries.
- Modify `qemu/target/arm/ptw.c`: make page-table-walk MMIO attributes carry the same requester ID for page tables placed behind an IOMMU path.
- Create `qemu/tests/qtest/tzc400-test.c`: device-register tests and region-permission tests using a test bus master.
- Modify `qemu/tests/qtest/meson.build`: add `tzc400-test` for aarch64 TCG builds.
- Modify `qemu/docs/system/arm/virt.rst`: document `tzc400=on` and `tzc400-cpu-nsaids=...`.

### Firmware and Guest

- Modify `trusted-firmware-a/plat/qemu/qemu/include/platform_def.h`: add `PLAT_QEMU_TZC400_BASE`, `PLAT_ARM_TZC_BASE`, `PLAT_ARM_TZC_FILTERS`, and `PLAT_ARM_TZC_NS_DEV_ACCESS`.
- Modify `trusted-firmware-a/plat/qemu/qemu/platform.mk`: compile `drivers/arm/tzc/tzc400.c`, `plat/arm/common/arm_tzc400.c`, and a QEMU TZC SMC service when `QEMU_TZC400=1`.
- Create `trusted-firmware-a/plat/qemu/qemu/qemu_tzc_svc.c`: platform SiP SMC handler that validates parameters and calls TF-A TZC-400 APIs.
- Modify `trusted-firmware-a/plat/qemu/common/qemu_bl31_setup.c`: initialize TZC-400 early when the build flag is enabled.
- Modify `optee_os/core/arch/arm/plat-vexpress/platform_config.h`: define `TZC400_BASE` for `PLATFORM_FLAVOR_qemu_armv8a`.
- Modify `build/qemu_v8.mk`: add `QEMU_TZC400 ?= n`; when enabled, pass QEMU machine properties and TF-A/OP-TEE build flags.
- Create `linux/drivers/misc/qemu_tzc400.c`: small test driver exposing `QEMU_TZC400_CONFIG_REGION` ioctl through `arm_smccc_smc()`.
- Modify `linux/drivers/misc/Kconfig` and `linux/drivers/misc/Makefile`: build the driver as `CONFIG_QEMU_TZC400_TEST`.
- Create `optee_examples/qemu_tzc_core_isolation/`: user-mode Linux test that allocates a page, asks EL3 to configure a TZC region for one NSAID, pins worker threads to cores, and records success/failure.

## Constants and Interfaces

Use these constants consistently:

```c
#define QEMU_TZC400_BASE       0x090c0000ULL
#define QEMU_TZC400_SIZE       0x00010000ULL
#define QEMU_TZC400_REGIONS    9
#define QEMU_TZC400_FILTERS    1
#define QEMU_TZC400_ADDR_BITS  40
#define QEMU_TZC400_MEM_BASE   0x40000000ULL
```

Use these machine options:

```text
-machine virt,secure=on,tzc400=on,tzc400-cpu-nsaids=0,,1,,2,,3
```

Use this TF-A SMC interface:

```c
#define QEMU_TZC400_SMC_CONFIG_REGION  0xc200ff00
#define QEMU_TZC400_SMC_REGION0        0xc200ff01
#define QEMU_TZC400_SMC_ENABLE         0xc200ff02

struct qemu_tzc400_region {
	uint32_t filters;
	uint32_t region;
	uint64_t base;
	uint64_t top;
	uint32_t sec_attr;
	uint32_t nsaid_permissions;
};
```

SMC register mapping:

```text
x0 = SMC function ID
x1 = physical address of `struct qemu_tzc400_region` for CONFIG_REGION
x2 = sizeof(struct qemu_tzc400_region) for CONFIG_REGION
x1 = sec_attr for REGION0
x2 = ns_device_access for REGION0
x1 = unused for ENABLE
x2 = unused for ENABLE
```

Return values:

```c
#define QEMU_TZC400_OK          0
#define QEMU_TZC400_E_DENIED   -1
#define QEMU_TZC400_E_RANGE    -2
#define QEMU_TZC400_E_ALIGN    -3
#define QEMU_TZC400_E_STATE    -4
```

## Task 1: Add QEMU TZC-400 Device Tests First

**Files:**
- Create: `qemu/tests/qtest/tzc400-test.c`
- Modify: `qemu/tests/qtest/meson.build:250-263`

- [ ] **Step 1: Write qtest scaffolding**

Create `qemu/tests/qtest/tzc400-test.c` with two test groups for the first RED state: register identity/reset state and TZC region-permission register programming. Do not add a requester-aware DMA master in this task; plain qtest memory helpers do not carry `MemTxAttrs.requester_id`, and requester-ID access-control coverage is added in Task 2 with the TZC-400 device model.

```c
/*
 * QEMU TZC-400 tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define TZC400_BASE                 0x090c0000ULL
#define TZC_BUILD_CONFIG            0x000
#define TZC_ACTION                  0x004
#define TZC_GATE_KEEPER             0x008
#define TZC_REGION_BASE_LOW(n)      (0x100 + ((n) * 0x20))
#define TZC_REGION_BASE_HIGH(n)     (0x104 + ((n) * 0x20))
#define TZC_REGION_TOP_LOW(n)       (0x108 + ((n) * 0x20))
#define TZC_REGION_TOP_HIGH(n)      (0x10c + ((n) * 0x20))
#define TZC_REGION_ATTRIBUTES(n)    (0x110 + ((n) * 0x20))
#define TZC_REGION_ID_ACCESS(n)     (0x114 + ((n) * 0x20))
#define TZC_CID0                    0xff0
#define TZC_CID1                    0xff4
#define TZC_CID2                    0xff8
#define TZC_CID3                    0xffc
#define QEMU_TZC400_REGIONS         9
#define QEMU_TZC400_FILTERS         1
#define QEMU_TZC400_ADDR_BITS       40

static uint32_t tzc_readl(uint64_t offset)
{
    return qtest_readl(global_qtest, TZC400_BASE + offset);
}

static void tzc_writel(uint64_t offset, uint32_t value)
{
    qtest_writel(global_qtest, TZC400_BASE + offset, value);
}

static void test_tzc400_ids(void)
{
    uint32_t cid = tzc_readl(TZC_CID0) |
                   (tzc_readl(TZC_CID1) << 8) |
                   (tzc_readl(TZC_CID2) << 16) |
                   (tzc_readl(TZC_CID3) << 24);

    g_assert_cmphex(cid, ==, 0xb105f00d);
    g_assert_cmphex(tzc_readl(TZC_BUILD_CONFIG), ==,
                    ((QEMU_TZC400_FILTERS - 1) << 24) |
                    ((QEMU_TZC400_ADDR_BITS - 1) << 8) |
                    (QEMU_TZC400_REGIONS - 1));
}

static void test_tzc400_region_programming(void)
{
    tzc_writel(TZC_ACTION, 1);
    tzc_writel(TZC_REGION_BASE_LOW(1), 0x40000000);
    tzc_writel(TZC_REGION_BASE_HIGH(1), 0);
    tzc_writel(TZC_REGION_TOP_LOW(1), 0x4000ffff);
    tzc_writel(TZC_REGION_TOP_HIGH(1), 0);
    tzc_writel(TZC_REGION_ATTRIBUTES(1), (3u << 30) | 1u);
    tzc_writel(TZC_REGION_ID_ACCESS(1), (1u << 1) | (1u << (16 + 1)));

    g_assert_cmphex(tzc_readl(TZC_REGION_ATTRIBUTES(1)), ==, (3u << 30) | 1u);
    g_assert_cmphex(tzc_readl(TZC_REGION_ID_ACCESS(1)), ==,
                    (1u << 1) | (1u << (16 + 1)));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_start("-machine virt,secure=on,tzc400=on -accel tcg "
                "-cpu max -smp 2 -m 512M");
    qtest_add_func("/tzc400/ids", test_tzc400_ids);
    qtest_add_func("/tzc400/region-programming", test_tzc400_region_programming);
    return g_test_run();
}
```

- [ ] **Step 2: Add qtest build entry**

Modify `qemu/tests/qtest/meson.build` so `qtests_aarch64` includes `tzc400-test` when TCG and `CONFIG_TZC400` are present:

```meson
qtests_aarch64 = \
  (cpu != 'arm' and unpack_edk2_blobs ? ['bios-tables-test'] : []) + \
  (config_all_accel.has_key('CONFIG_TCG') and config_all_devices.has_key('CONFIG_TPM_TIS_SYSBUS') ? \
    ['tpm-tis-device-test', 'tpm-tis-device-swtpm-test'] : []) + \
  (config_all_devices.has_key('CONFIG_XLNX_ZYNQMP_ARM') ? ['xlnx-can-test', 'fuzz-xlnx-dp-test'] : []) + \
  (config_all_devices.has_key('CONFIG_XLNX_VERSAL') ? ['xlnx-canfd-test', 'xlnx-versal-trng-test'] : []) + \
  (config_all_devices.has_key('CONFIG_RASPI') ? ['bcm2835-dma-test', 'bcm2835-i2c-test'] : []) + \
  (config_all_accel.has_key('CONFIG_TCG') and \
   config_all_devices.has_key('CONFIG_TPM_TIS_I2C') ? ['tpm-tis-i2c-test'] : []) + \
  (config_all_devices.has_key('CONFIG_ASPEED_SOC') ? qtests_aspeed64 : []) + \
  (config_all_accel.has_key('CONFIG_TCG') and \
   config_all_devices.has_key('CONFIG_TZC400') ? ['tzc400-test'] : []) + \
  ['arm-cpu-features',
   'numa-test',
   'boot-serial-test',
   'migration-test']
```

- [ ] **Step 3: Run tests and record the expected failure**

Run:

```bash
meson test -C qemu/build tzc400-test --suite qtest-aarch64 --print-errorlogs
```

Expected before implementation:

```text
ERROR: Test "tzc400-test" was not found
```

If `qemu/build` does not exist, run the QEMU configure command from Task 8 before rerunning this step.

## Task 2: Implement the QEMU TZC-400 Device

**Files:**
- Create: `qemu/include/hw/misc/tzc400.h`
- Create: `qemu/hw/misc/tzc400.c`
- Modify: `qemu/hw/misc/meson.build:117-119`
- Modify: `qemu/hw/misc/Kconfig:119-127`
- Modify: `qemu/hw/misc/trace-events`
- Extend: `qemu/tests/qtest/tzc400-test.c` with requester-ID access-control coverage once the device exposes a requester-aware test path.

- [ ] **Step 1: Define the public device interface**

Create `qemu/include/hw/misc/tzc400.h`:

```c
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

struct TZC400State {
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
};

MemoryRegion *tzc400_get_upstream(TZC400State *s);

#endif
```

- [ ] **Step 2: Implement register definitions and reset state**

Create `qemu/hw/misc/tzc400.c` with register offsets matching the TF-A and OP-TEE drivers:

```c
/*
 * ARM TrustZone Address Space Controller TZC-400 emulation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "hw/misc/tzc400.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TZC400_REG_SIZE             0x10000
#define TZC400_DEFAULT_ADDR_WIDTH   40
#define TZC400_DEFAULT_FILTERS      1
#define TZC400_DEFAULT_REGIONS      9

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
REG32(REGION_BASE_LOW, 0x100)
REG32(REGION_BASE_HIGH, 0x104)
REG32(REGION_TOP_LOW, 0x108)
REG32(REGION_TOP_HIGH, 0x10c)
REG32(REGION_ATTRIBUTES, 0x110)
    FIELD(REGION_ATTRIBUTES, F_EN, 0, 4)
    FIELD(REGION_ATTRIBUTES, SEC, 30, 2)
REG32(REGION_ID_ACCESS, 0x114)
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

#define TZC400_REGION_STRIDE        0x20
#define TZC400_REGION_BASE          0x100
#define TZC400_ACTION_ERR           0x1
#define TZC400_ACTION_INT           0x2
#define TZC400_SEC_RD               0x1
#define TZC400_SEC_WR               0x2

static const uint8_t tzc400_idregs[] = {
    0x00, 0x00, 0x00, 0x00,
    0x60, 0xb4, 0x2b, 0x00,
    0x0d, 0xf0, 0x05, 0xb1,
};
```

- [ ] **Step 3: Implement access decision helper**

Add the region selection and permission logic. Highest enabled matching region wins; region 0 is the default fallback.

```c
static bool tzc400_region_enabled(TZC400State *s, unsigned region)
{
    if (region == 0) {
        return true;
    }
    return FIELD_EX32(s->region[region].attr, REGION_ATTRIBUTES, F_EN) != 0;
}

static unsigned tzc400_lookup_region(TZC400State *s, hwaddr addr)
{
    unsigned selected = 0;

    for (unsigned i = 1; i < s->num_regions; i++) {
        TZC400Region *r = &s->region[i];

        if (tzc400_region_enabled(s, i) && addr >= r->base && addr <= r->top) {
            selected = i;
        }
    }

    return selected;
}

static bool tzc400_access_allowed(TZC400State *s, hwaddr addr,
                                  MemTxAttrs attrs, bool is_write)
{
    unsigned region = tzc400_lookup_region(s, addr);
    TZC400Region *r = &s->region[region];

    if (attrs.unspecified) {
        return true;
    }

    if (attrs.secure) {
        unsigned sec = FIELD_EX32(r->attr, REGION_ATTRIBUTES, SEC);
        return is_write ? (sec & TZC400_SEC_WR) : (sec & TZC400_SEC_RD);
    }

    uint32_t nsaid = attrs.requester_id & 0xf;
    uint32_t bit = 1u << nsaid;

    if (is_write) {
        return (r->id_access & (bit << 16)) != 0;
    }
    return (r->id_access & bit) != 0;
}
```

- [ ] **Step 4: Implement failure recording**

Record fail registers and return `MEMTX_ERROR` when `ACTION.RV` includes the error bit.

```c
static void tzc400_record_failure(TZC400State *s, hwaddr addr,
                                  MemTxAttrs attrs, bool is_write)
{
    uint32_t fail_control = 0;

    if (is_write) {
        fail_control |= 1u << 24;
    }
    if (!attrs.secure) {
        fail_control |= 1u << 21;
    }
    if (!attrs.user) {
        fail_control |= 1u << 20;
    }

    s->fail_addr[0] = addr;
    s->fail_control[0] = fail_control;
    s->fail_id[0] = attrs.requester_id & 0xffff;
    s->int_status |= 1u;
    qemu_set_irq(s->irq[0], (s->action & TZC400_ACTION_INT) != 0);
}
```

- [ ] **Step 5: Implement register read/write**

Implement `MemoryRegionOps` using 32-bit register semantics and reject non-secure configuration accesses before the ID registers:

```c
static MemTxResult tzc400_reg_read(void *opaque, hwaddr addr,
                                   uint64_t *data, unsigned size,
                                   MemTxAttrs attrs)
{
    TZC400State *s = TZC400(opaque);
    uint32_t off = addr & ~3u;
    uint32_t value = 0;

    if (size != 4) {
        return MEMTX_ERROR;
    }

    if (!attrs.secure && off < A_PID4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "TZC-400: non-secure read at offset 0x%x\n", off);
        *data = 0;
        return MEMTX_OK;
    }

    if (off >= TZC400_REGION_BASE &&
        off < TZC400_REGION_BASE + TZC400_REGION_STRIDE * s->num_regions) {
        unsigned region = (off - TZC400_REGION_BASE) / TZC400_REGION_STRIDE;
        unsigned roff = (off - TZC400_REGION_BASE) % TZC400_REGION_STRIDE;

        switch (roff) {
        case A_REGION_BASE_LOW - TZC400_REGION_BASE:
            value = extract64(s->region[region].base, 0, 32);
            break;
        case A_REGION_BASE_HIGH - TZC400_REGION_BASE:
            value = extract64(s->region[region].base, 32, 32);
            break;
        case A_REGION_TOP_LOW - TZC400_REGION_BASE:
            value = extract64(s->region[region].top, 0, 32);
            break;
        case A_REGION_TOP_HIGH - TZC400_REGION_BASE:
            value = extract64(s->region[region].top, 32, 32);
            break;
        case A_REGION_ATTRIBUTES - TZC400_REGION_BASE:
            value = s->region[region].attr;
            break;
        case A_REGION_ID_ACCESS - TZC400_REGION_BASE:
            value = s->region[region].id_access;
            break;
        default:
            value = 0;
            break;
        }
        *data = value;
        return MEMTX_OK;
    }

    switch (off) {
    case A_BUILD_CONFIG:
        value = ((s->num_filters - 1) << R_BUILD_CONFIG_NF_SHIFT) |
                ((s->addr_width - 1) << R_BUILD_CONFIG_AW_SHIFT) |
                (s->num_regions - 1);
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
        value = extract64(s->fail_addr[0], 32, 32);
        break;
    case A_FAIL_CONTROL:
        value = s->fail_control[0];
        break;
    case A_FAIL_ID:
        value = s->fail_id[0];
        break;
    case A_PID4 ... A_CID3:
        value = tzc400_idregs[(off - A_PID4) / 4];
        break;
    default:
        value = 0;
        break;
    }

    *data = value;
    return MEMTX_OK;
}
```

Add the write side with the same region offset decoding. The write side must:

- store `ACTION.RV`, `GATE_KEEPER.OR`, and `SPECULATION_CTRL`
- clear `INT_STATUS` bits when writing `INT_CLEAR`
- write region base/top/attributes/ID access registers
- ignore writes to PID/CID registers
- call `memory_region_iommu_replay_all(&s->upstream)` after region or gatekeeper writes

- [ ] **Step 6: Implement IOMMU translation**

Follow the `tz-mpc.c` pattern: translate allowed requests to the downstream address space and blocked requests to an empty blocked address space.

```c
enum {
    TZC400_IOMMU_IDX_S = 0,
    TZC400_IOMMU_IDX_NS_BASE = 1,
    TZC400_IOMMU_NUM_INDEXES = TZC400_IOMMU_IDX_NS_BASE + 16,
};

static int tzc400_attrs_to_index(IOMMUMemoryRegion *iommu, MemTxAttrs attrs)
{
    if (attrs.unspecified || attrs.secure) {
        return TZC400_IOMMU_IDX_S;
    }

    return TZC400_IOMMU_IDX_NS_BASE + (attrs.requester_id & 0xf);
}

static int tzc400_num_indexes(IOMMUMemoryRegion *iommu)
{
    return TZC400_IOMMU_NUM_INDEXES;
}

static IOMMUTLBEntry tzc400_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                      IOMMUAccessFlags flags, int iommu_idx)
{
    TZC400State *s = TZC400(container_of(iommu, TZC400State, upstream));
    MemTxAttrs attrs = {
        .secure = iommu_idx == TZC400_IOMMU_IDX_S,
        .requester_id = iommu_idx > TZC400_IOMMU_IDX_S ?
                        iommu_idx - TZC400_IOMMU_IDX_NS_BASE : 0,
    };
    bool is_write = flags & IOMMU_WO;
    bool allowed = tzc400_access_allowed(s, addr, attrs, is_write);

    if (!allowed) {
        tzc400_record_failure(s, addr, attrs, is_write);
    }

    return (IOMMUTLBEntry) {
        .target_as = allowed ? &s->downstream_as : &s->blocked_as,
        .iova = addr,
        .translated_addr = addr,
        .addr_mask = TARGET_PAGE_SIZE - 1,
        .perm = allowed ? IOMMU_RW : IOMMU_NONE,
    };
}
```

Register `translate`, `attrs_to_index`, and `num_indexes` in the `IOMMUMemoryRegionClass`, matching the `tz-mpc.c` pattern.

- [ ] **Step 7: Realize and reset device**

Initialize the register MMIO and upstream IOMMU MemoryRegion:

```c
static void tzc400_reset(DeviceState *dev)
{
    TZC400State *s = TZC400(dev);

    s->num_filters = TZC400_DEFAULT_FILTERS;
    s->num_regions = TZC400_DEFAULT_REGIONS;
    s->addr_width = TZC400_DEFAULT_ADDR_WIDTH;
    s->action = 0;
    s->gate_keeper = 1;
    s->speculation_ctrl = 0;
    s->int_status = 0;
    memset(s->fail_addr, 0, sizeof(s->fail_addr));
    memset(s->fail_control, 0, sizeof(s->fail_control));
    memset(s->fail_id, 0, sizeof(s->fail_id));
    memset(s->region, 0, sizeof(s->region));

    s->region[0].base = 0;
    s->region[0].top = UINT64_MAX >> (64 - s->addr_width);
    s->region[0].attr = 3u << R_REGION_ATTRIBUTES_SEC_SHIFT;
    s->region[0].id_access = 0xffffffffu;
}

static MemTxResult tzc400_blocked_read(void *opaque, hwaddr addr,
                                       uint64_t *data, unsigned size,
                                       MemTxAttrs attrs)
{
    *data = 0;
    return MEMTX_ERROR;
}

static MemTxResult tzc400_blocked_write(void *opaque, hwaddr addr,
                                        uint64_t value, unsigned size,
                                        MemTxAttrs attrs)
{
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

static void tzc400_realize(DeviceState *dev, Error **errp)
{
    TZC400State *s = TZC400(dev);

    if (!s->downstream) {
        error_setg(errp, "TZC-400 requires a downstream memory region");
        return;
    }

    memory_region_init_io(&s->blocked_io, OBJECT(dev), &tzc400_blocked_ops,
                          s, "tzc400-blocked-io",
                          memory_region_size(s->downstream));
    address_space_init(&s->downstream_as, s->downstream,
                       "tzc400-downstream");
    address_space_init(&s->blocked_as, &s->blocked_io,
                       "tzc400-blocked");
}

static void tzc400_init(Object *obj)
{
    TZC400State *s = TZC400(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->regs, obj, &tzc400_reg_ops,
                          s, "tzc400-regs", TZC400_REG_SIZE);
    sysbus_init_mmio(sbd, &s->regs);

    memory_region_init_iommu(&s->upstream, sizeof(s->upstream),
                             TYPE_TZC400_IOMMU_MEMORY_REGION,
                             obj, "tzc400-upstream",
                             UINT64_MAX);
}

static const Property tzc400_properties[] = {
    DEFINE_PROP_LINK("downstream", TZC400State, downstream,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void tzc400_iommu_class_init(ObjectClass *klass, void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = tzc400_translate;
    imrc->attrs_to_index = tzc400_attrs_to_index;
    imrc->num_indexes = tzc400_num_indexes;
}

static void tzc400_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tzc400_realize;
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

static const TypeInfo tzc400_iommu_info = {
    .name = TYPE_TZC400_IOMMU_MEMORY_REGION,
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .class_init = tzc400_iommu_class_init,
};

static void tzc400_register_types(void)
{
    type_register_static(&tzc400_info);
    type_register_static(&tzc400_iommu_info);
}

type_init(tzc400_register_types)
```

- [ ] **Step 8: Add build glue**

Modify `qemu/hw/misc/Kconfig`:

```kconfig
config TZC400
    bool
```

Modify `qemu/hw/misc/meson.build`:

```meson
system_ss.add(when: 'CONFIG_TZC400', if_true: files('tzc400.c'))
```

Modify `qemu/hw/misc/trace-events`:

```text
tzc400_reg_read(uint64_t addr, uint64_t value, unsigned size) "addr 0x%" PRIx64 " value 0x%" PRIx64 " size %u"
tzc400_reg_write(uint64_t addr, uint64_t value, unsigned size) "addr 0x%" PRIx64 " value 0x%" PRIx64 " size %u"
tzc400_access(uint64_t addr, bool secure, uint16_t requester, bool write, bool allowed) "addr 0x%" PRIx64 " secure %d requester %u write %d allowed %d"
```

- [ ] **Step 9: Run QEMU unit build**

Run:

```bash
ninja -C qemu/build qemu-system-aarch64
```

Expected:

```text
ninja: Entering directory `qemu/build'
```

and no compiler errors.

## Task 2A: Fix TZC-400 Physical Address Matching

**Files:**
- Modify: `qemu/include/hw/misc/tzc400.h`
- Modify: `qemu/hw/misc/tzc400.c`
- Later use from: `qemu/hw/arm/virt.c`

- [x] **Step 1: Add protected-window physical base to device state**

In `qemu/include/hw/misc/tzc400.h`, add this field near `downstream`:

```c
    hwaddr addr_base;
```

- [x] **Step 2: Add the QOM property**

In `qemu/hw/misc/tzc400.c`, extend `tzc400_properties[]`:

```c
    DEFINE_PROP_UINT64("addr-base", TZC400State, addr_base, 0),
```

- [x] **Step 3: Compare TZC regions against physical bus addresses**

Add a helper:

```c
static hwaddr tzc400_bus_addr(TZC400State *s, hwaddr addr)
{
    return s->addr_base + addr;
}
```

Then update `tzc400_translate()` so access checks and failure registers use the bus address:

```c
    hwaddr bus_addr = tzc400_bus_addr(s, addr);

    allowed = tzc400_access_check(s, bus_addr, attrs, write);
    if (!allowed) {
        tzc400_record_failure(s, bus_addr, attrs, write);
    }
```

Keep `.iova = addr` and `.translated_addr = addr`; only the policy lookup and fail reporting should use the guest physical address.

- [x] **Step 4: Include the base in migration state**

In `vmstate_tzc400.fields`, add:

```c
        VMSTATE_UINT64(addr_base, TZC400State),
```

- [x] **Step 5: Pass the base from `virt` when Task 4 wires the device**

When creating the TZC-400 device in `qemu/hw/arm/virt.c`, set:

```c
        object_property_set_uint(OBJECT(vms->tzc400_dev), "addr-base",
                                 vms->memmap[VIRT_MEM].base, &error_abort);
```

- [x] **Step 6: Rebuild the standalone device**

Run:

```bash
ninja -C qemu/build qemu-system-aarch64
```

Expected: build completes with no errors.

## Task 3: Add Per-Core NSAID Plumbing in ARM TCG

**Files:**
- Modify: `qemu/target/arm/cpu.h:913-920`
- Modify: `qemu/target/arm/cpu.c:2633-2643`
- Modify: `qemu/target/arm/tcg/tlb_helper.c:321-354`
- Modify: `qemu/target/arm/ptw.c:650-760`

- [ ] **Step 1: Add CPU field**

In `qemu/target/arm/cpu.h`, add the field next to memory access state:

```c
    /* Bus requester ID used by TrustZone address controllers. */
    uint16_t tzc_nsaid;
```

- [ ] **Step 2: Add CPU property**

In `qemu/target/arm/cpu.c`, add this entry to `arm_cpu_properties[]`:

```c
    DEFINE_PROP_UINT16("tzc-nsaid", ARMCPU, tzc_nsaid, 0),
```

- [ ] **Step 3: Stamp final TLB attributes**

In `arm_cpu_tlb_fill_align()` before `*out = res.f;`, set:

```c
        res.f.attrs.requester_id = cpu->tzc_nsaid;
        res.f.extra.arm.pte_attrs = res.cacheattrs.attrs;
        res.f.extra.arm.shareability = res.cacheattrs.shareability;
        *out = res.f;
```

- [ ] **Step 4: Stamp page-table-walk MMIO attributes**

In `qemu/target/arm/ptw.c`, add this helper near the `S1Translate` definition:

```c
static MemTxAttrs arm_ptw_attrs(CPUARMState *env, ARMSecuritySpace space)
{
    ARMCPU *cpu = env_archcpu(env);

    return (MemTxAttrs) {
        .space = space,
        .secure = arm_space_is_secure(space),
        .requester_id = cpu->tzc_nsaid,
    };
}
```

Replace local page-table-walk attribute literals in `arm_ldl_ptw()`, `arm_ldq_ptw()`, and `arm_casq_ptw()`:

```c
        MemTxAttrs attrs = arm_ptw_attrs(env, ptw->out_space);
```

- [ ] **Step 5: Run ARM TCG build**

Run:

```bash
ninja -C qemu/build qemu-system-aarch64
```

Expected: build completes with no errors.

## Task 4: Integrate TZC-400 into QEMU `virt`

**Files:**
- Modify: `qemu/include/hw/arm/virt.h:53-83`
- Modify: `qemu/include/hw/arm/virt.h:120-160`
- Modify: `qemu/hw/arm/virt.c:175-190`
- Modify: `qemu/hw/arm/virt.c:2106-2425`
- Modify: `qemu/hw/arm/virt.c:2466-2720`
- Modify: `qemu/hw/arm/virt.c:3203-3258`
- Modify: `qemu/hw/arm/Kconfig`

- [ ] **Step 1: Add memory-map slot**

In `qemu/include/hw/arm/virt.h`, add `VIRT_TZC400` after `VIRT_SECURE_GPIO`:

```c
    VIRT_SECURE_GPIO,
    VIRT_TZC400,
    VIRT_PCDIMM_ACPI,
```

In `qemu/hw/arm/virt.c`, add:

```c
    [VIRT_TZC400] =            { 0x090c0000, 0x00010000 },
```

- [ ] **Step 2: Add machine state**

In `VirtMachineState`, add:

```c
    bool tzc400;
    char *tzc400_cpu_nsaids;
    DeviceState *tzc400_dev;
    MemoryRegion *tzc400_ram;
```

- [ ] **Step 3: Add machine properties**

Add getter/setter functions:

```c
static bool virt_get_tzc400(Object *obj, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    return vms->tzc400;
}

static void virt_set_tzc400(Object *obj, bool value, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    vms->tzc400 = value;
}

static char *virt_get_tzc400_cpu_nsaids(Object *obj, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    return g_strdup(vms->tzc400_cpu_nsaids ? vms->tzc400_cpu_nsaids : "");
}

static void virt_set_tzc400_cpu_nsaids(Object *obj, const char *value,
                                       Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    g_free(vms->tzc400_cpu_nsaids);
    vms->tzc400_cpu_nsaids = g_strdup(value ? value : "");
}
```

Register them in `virt_machine_class_init()`:

```c
    object_class_property_add_bool(oc, "tzc400",
                                   virt_get_tzc400,
                                   virt_set_tzc400);
    object_class_property_set_description(oc, "tzc400",
                                          "Enable emulated TZC-400 for TrustZone DRAM partitioning");
    object_class_property_add_str(oc, "tzc400-cpu-nsaids",
                                  virt_get_tzc400_cpu_nsaids,
                                  virt_set_tzc400_cpu_nsaids);
    object_class_property_set_description(oc, "tzc400-cpu-nsaids",
                                          "Comma-separated non-secure access IDs assigned to CPUs");
```

Set the default string during machine instance initialization:

```c
    vms->tzc400_cpu_nsaids = g_strdup("");
```

- [ ] **Step 4: Parse CPU NSAIDs**

Add helper in `virt.c`:

```c
static uint16_t virt_tzc400_nsaid_for_cpu(VirtMachineState *vms,
                                          unsigned cpu_index,
                                          Error **errp)
{
    g_auto(GStrv) parts = NULL;
    unsigned count;

    if (!vms->tzc400_cpu_nsaids || !vms->tzc400_cpu_nsaids[0]) {
        return cpu_index & 0xf;
    }

    parts = g_strsplit(vms->tzc400_cpu_nsaids, ",", -1);
    count = g_strv_length(parts);
    if (cpu_index >= count) {
        error_setg(errp, "tzc400-cpu-nsaids has %u entries for CPU %u",
                   count, cpu_index);
        return 0;
    }

    unsigned long value = g_ascii_strtoull(parts[cpu_index], NULL, 0);
    if (value > 15) {
        error_setg(errp, "TZC-400 NSAID must be in range 0..15");
        return 0;
    }

    return value;
}
```

Before realizing each CPU, set:

```c
        if (vms->tzc400) {
            uint16_t nsaid = virt_tzc400_nsaid_for_cpu(vms, n, &error_fatal);

            object_property_set_uint(cpuobj, "tzc-nsaid", nsaid, &error_abort);
        }
```

- [ ] **Step 5: Instantiate the TZC around `VIRT_MEM`**

Replace the direct RAM mapping:

```c
    memory_region_add_subregion(sysmem, vms->memmap[VIRT_MEM].base,
                                machine->ram);
```

with:

```c
    if (vms->tzc400) {
        TZC400State *tzc;

        vms->tzc400_dev = qdev_new(TYPE_TZC400);
        vms->tzc400_ram = g_new0(MemoryRegion, 1);
        memory_region_init(vms->tzc400_ram, OBJECT(machine),
                           "virt.tzc400-dram", machine->ram_size);
        memory_region_add_subregion(vms->tzc400_ram, 0, machine->ram);
        object_property_set_link(OBJECT(vms->tzc400_dev), "downstream",
                                 OBJECT(vms->tzc400_ram), &error_abort);
        object_property_set_uint(OBJECT(vms->tzc400_dev), "addr-base",
                                 vms->memmap[VIRT_MEM].base, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(vms->tzc400_dev), &error_fatal);
        tzc = TZC400(vms->tzc400_dev);

        memory_region_add_subregion(sysmem, vms->memmap[VIRT_MEM].base,
                                    tzc400_get_upstream(tzc));
        sysbus_mmio_map(SYS_BUS_DEVICE(vms->tzc400_dev), 0,
                        vms->memmap[VIRT_TZC400].base);
    } else {
        memory_region_add_subregion(sysmem, vms->memmap[VIRT_MEM].base,
                                    machine->ram);
    }
```

Keep `VIRT_SECURE_MEM` unchanged; it remains the secure-only RAM view used by TF-A and OP-TEE.

- [ ] **Step 6: Add secure FDT node**

Add a node under the secure FDT view:

```c
static void create_tzc400_fdt(VirtMachineState *vms)
{
    MachineState *ms = MACHINE(vms);
    hwaddr base = vms->memmap[VIRT_TZC400].base;
    hwaddr size = vms->memmap[VIRT_TZC400].size;
    char *nodename = g_strdup_printf("/tzc@%" PRIx64, base);

    qemu_fdt_add_subnode(ms->fdt, nodename);
    qemu_fdt_setprop_string(ms->fdt, nodename, "compatible", "arm,tzc-400");
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "reg",
                                 2, base, 2, size);
    qemu_fdt_setprop_string(ms->fdt, nodename, "status", "disabled");
    qemu_fdt_setprop_string(ms->fdt, nodename, "secure-status", "okay");
    g_free(nodename);
}
```

Call it only when `vms->secure && vms->tzc400`.

- [ ] **Step 7: Enforce supported mode**

In `machvirt_init()`, after existing secure/KVM checks:

```c
    if (vms->tzc400 && !vms->secure) {
        error_report("mach-virt: tzc400 requires secure=on");
        exit(1);
    }
    if (vms->tzc400 && !tcg_enabled()) {
        error_report("mach-virt: tzc400 requires TCG");
        exit(1);
    }
```

- [ ] **Step 8: Build and smoke boot**

Run:

```bash
ninja -C qemu/build qemu-system-aarch64
qemu/build/qemu-system-aarch64 -machine virt,secure=on,tzc400=on,tzc400-cpu-nsaids=0,,1 -accel tcg -cpu max -smp 2 -m 512M -nographic -serial mon:stdio
```

Expected smoke output:

```text
QEMU monitor
```

Exit with `Ctrl-a x`.

## Task 5: Add TF-A TZC-400 Initialization and SMC API

**Files:**
- Modify: `trusted-firmware-a/plat/qemu/qemu/include/platform_def.h`
- Modify: `trusted-firmware-a/plat/qemu/qemu/platform.mk`
- Modify: `trusted-firmware-a/plat/qemu/common/qemu_bl31_setup.c`
- Create: `trusted-firmware-a/plat/qemu/qemu/qemu_tzc_svc.c`

- [ ] **Step 1: Add platform constants**

In `trusted-firmware-a/plat/qemu/qemu/include/platform_def.h`:

```c
#define PLAT_QEMU_TZC400_BASE          ULL(0x090c0000)
#define PLAT_ARM_TZC_BASE              PLAT_QEMU_TZC400_BASE
#define PLAT_ARM_TZC_FILTERS           TZC_400_REGION_ATTR_FILTER_BIT_ALL
#define PLAT_ARM_TZC_NS_DEV_ACCESS     0xffffffffU
```

Include the driver header near the other platform includes:

```c
#include <drivers/arm/tzc400.h>
```

- [ ] **Step 2: Add BL31 sources**

In `trusted-firmware-a/plat/qemu/qemu/platform.mk`:

```make
ifeq (${QEMU_TZC400},1)
BL31_SOURCES		+=	drivers/arm/tzc/tzc400.c		\
				plat/arm/common/arm_tzc400.c		\
				plat/qemu/qemu/qemu_tzc_svc.c
$(eval $(call add_define,QEMU_TZC400))
endif
```

- [ ] **Step 3: Initialize TZC in BL31**

In `trusted-firmware-a/plat/qemu/common/qemu_bl31_setup.c`, add:

```c
#if QEMU_TZC400
#include <drivers/arm/tzc400.h>
#endif
```

Call this before handing off to the non-secure world:

```c
#if QEMU_TZC400
static void qemu_tzc400_setup(void)
{
	tzc400_init(PLAT_QEMU_TZC400_BASE);
	tzc400_disable_filters();
	tzc400_configure_region0(TZC_REGION_S_RDWR, 0xffffffffU);
	tzc400_set_action(TZC_ACTION_ERR);
	tzc400_enable_filters();
}
#endif
```

Then in `bl31_platform_setup()`:

```c
#if QEMU_TZC400
	qemu_tzc400_setup();
#endif
```

- [ ] **Step 4: Add QEMU TZC SMC handler**

Create `trusted-firmware-a/plat/qemu/qemu/qemu_tzc_svc.c`:

```c
/*
 * QEMU TZC-400 SiP service.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <common/debug.h>
#include <common/runtime_svc.h>
#include <drivers/arm/tzc400.h>
#include <lib/smccc.h>
#include <smccc_helpers.h>

#define QEMU_TZC400_SMC_CONFIG_REGION  U(0xc200ff00)
#define QEMU_TZC400_SMC_REGION0        U(0xc200ff01)
#define QEMU_TZC400_SMC_ENABLE         U(0xc200ff02)

#define QEMU_TZC400_OK                 0
#define QEMU_TZC400_E_DENIED          -1
#define QEMU_TZC400_E_RANGE           -2
#define QEMU_TZC400_E_ALIGN           -3

struct qemu_tzc400_region {
	uint32_t filters;
	uint32_t region;
	uint64_t base;
	uint64_t top;
	uint32_t sec_attr;
	uint32_t nsaid_permissions;
};

static uintptr_t qemu_tzc_smc_handler(uint32_t smc_fid,
				      u_register_t x1,
				      u_register_t x2,
				      u_register_t x3,
				      u_register_t x4,
				      void *cookie,
				      void *handle,
				      u_register_t flags)
{
	if (!is_caller_non_secure(flags)) {
		SMC_RET1(handle, QEMU_TZC400_E_DENIED);
	}

	switch (smc_fid) {
	case QEMU_TZC400_SMC_REGION0:
		tzc400_disable_filters();
		tzc400_configure_region0((unsigned int)x1, (unsigned int)x2);
		tzc400_enable_filters();
		SMC_RET1(handle, QEMU_TZC400_OK);
	case QEMU_TZC400_SMC_CONFIG_REGION: {
		const struct qemu_tzc400_region *r;

		if (x2 != sizeof(struct qemu_tzc400_region)) {
			SMC_RET1(handle, QEMU_TZC400_E_RANGE);
		}

		r = (const struct qemu_tzc400_region *)(uintptr_t)x1;
		if (r->region == 0 || r->region >= 9) {
			SMC_RET1(handle, QEMU_TZC400_E_RANGE);
		}
		if ((r->base & 0xfff) != 0 ||
		    (r->top & 0xfff) != 0xfff ||
		    r->base > r->top) {
			SMC_RET1(handle, QEMU_TZC400_E_ALIGN);
		}
		tzc400_disable_filters();
		tzc400_configure_region(r->filters, r->region, r->base,
					r->top, r->sec_attr,
					r->nsaid_permissions);
		tzc400_enable_filters();
		SMC_RET1(handle, QEMU_TZC400_OK);
	}
	case QEMU_TZC400_SMC_ENABLE:
		tzc400_set_action(TZC_ACTION_ERR);
		tzc400_enable_filters();
		SMC_RET1(handle, QEMU_TZC400_OK);
	default:
		SMC_RET1(handle, SMC_UNK);
	}
}

static int qemu_tzc_smc_setup(void)
{
	return 0;
}

DECLARE_RT_SVC(qemu_tzc_svc,
	       OEN_SIP_START,
	       OEN_SIP_END,
	       SMC_TYPE_FAST,
	       qemu_tzc_smc_setup,
	       qemu_tzc_smc_handler);
```

- [ ] **Step 5: Build TF-A through OP-TEE build**

Run:

```bash
make -C build -f qemu_v8.mk QEMU_TZC400=y ARM_TF_CLEAN=y arm-tf
```

Expected:

```text
Building arm-tf
```

and no compile errors.

## Task 6: Enable OP-TEE TZC-400 Driver on QEMU ARMv8A

**Files:**
- Modify: `optee_os/core/arch/arm/plat-vexpress/platform_config.h:68-83`
- Modify: `build/qemu_v8.mk:498-516`

- [ ] **Step 1: Add QEMU ARMv8A base address**

In `optee_os/core/arch/arm/plat-vexpress/platform_config.h` under `PLATFORM_FLAVOR_qemu_armv8a`:

```c
#define TZC400_BASE		0x090c0000
```

- [ ] **Step 2: Gate OP-TEE flag from build**

In `build/qemu_v8.mk`:

```make
QEMU_TZC400 ?= n

ifeq ($(QEMU_TZC400),y)
OPTEE_OS_COMMON_FLAGS += CFG_TZC400=y
TF_A_FLAGS += QEMU_TZC400=1
endif
```

Change the existing machine line to:

```make
QEMU_BASE_ARGS += -machine virt,acpi=off,secure=on,mte=$(QEMU_MTE),gic-version=$(QEMU_GIC_VERSION),virtualization=$(QEMU_VIRT)$(QEMU_MACHINE_TZC400_ARGS)
```

- [ ] **Step 3: Build OP-TEE OS**

Run:

```bash
make -C build -f qemu_v8.mk QEMU_TZC400=y OPTEE_OS_CLEAN=y optee-os
```

Expected:

```text
CFG_TZC400=y
```

in the OP-TEE build command and no compile errors.

- [ ] **Step 4: Boot and confirm TZC driver probes**

Run:

```bash
make -C build -f qemu_v8.mk QEMU_TZC400=y run-only
```

Expected secure-world log contains:

```text
TZC
```

and does not contain:

```text
Wrong device ID
```

## Task 7: Add Linux Core-Isolation Test API

**Files:**
- Create: `linux/drivers/misc/qemu_tzc400.c`
- Modify: `linux/drivers/misc/Kconfig`
- Modify: `linux/drivers/misc/Makefile`
- Create: `optee_examples/qemu_tzc_core_isolation/Makefile`
- Create: `optee_examples/qemu_tzc_core_isolation/host/main.c`

- [ ] **Step 1: Add Linux misc driver**

Create `linux/drivers/misc/qemu_tzc400.c`:

```c
// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/arm-smccc.h>
#include <asm/io.h>

#define QEMU_TZC400_SMC_CONFIG_REGION  0xc200ff00
#define QEMU_TZC400_IOCTL_ALLOC        _IOR('z', 1, struct qemu_tzc400_alloc)
#define QEMU_TZC400_IOCTL_CONFIG       _IOW('z', 2, struct qemu_tzc400_region)
#define QEMU_TZC400_IOCTL_TOUCH        _IOWR('z', 3, struct qemu_tzc400_touch)

struct qemu_tzc400_region {
	__u32 filters;
	__u32 region;
	__u64 base;
	__u64 top;
	__u32 sec_attr;
	__u32 nsaid_permissions;
};

struct qemu_tzc400_alloc {
	__u64 phys;
	__u64 size;
};

struct qemu_tzc400_touch {
	__u32 write;
	__u32 value;
};

static void *test_page;
static phys_addr_t test_phys;

static long qemu_tzc400_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct qemu_tzc400_region r;
	struct qemu_tzc400_alloc alloc;
	struct qemu_tzc400_touch touch;
	struct arm_smccc_res res;

	if (cmd == QEMU_TZC400_IOCTL_ALLOC) {
		if (!test_page) {
			test_page = (void *)get_zeroed_page(GFP_KERNEL);
			if (!test_page)
				return -ENOMEM;
			test_phys = virt_to_phys(test_page);
		}
		alloc.phys = test_phys;
		alloc.size = PAGE_SIZE;
		return copy_to_user((void __user *)arg, &alloc, sizeof(alloc)) ?
		       -EFAULT : 0;
	}

	if (cmd == QEMU_TZC400_IOCTL_TOUCH) {
		if (!test_page)
			return -ENODEV;
		if (copy_from_user(&touch, (void __user *)arg, sizeof(touch)))
			return -EFAULT;
		if (touch.write)
			WRITE_ONCE(*(u32 *)test_page, touch.value);
		else
			touch.value = READ_ONCE(*(u32 *)test_page);
		return copy_to_user((void __user *)arg, &touch, sizeof(touch)) ?
		       -EFAULT : 0;
	}

	if (cmd == QEMU_TZC400_IOCTL_CONFIG) {
		if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
			return -EFAULT;

		arm_smccc_smc(QEMU_TZC400_SMC_CONFIG_REGION,
			      virt_to_phys(&r), sizeof(r), 0, 0, 0, 0, 0, &res);
		return res.a0 == 0 ? 0 : -EPERM;
	}

	return -ENOTTY;
}

static const struct file_operations qemu_tzc400_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = qemu_tzc400_ioctl,
	.compat_ioctl = qemu_tzc400_ioctl,
};

static struct miscdevice qemu_tzc400_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "qemu_tzc400",
	.fops = &qemu_tzc400_fops,
};

module_misc_device(qemu_tzc400_dev);
MODULE_LICENSE("GPL");
```

- [ ] **Step 2: Add Linux Kconfig/Makefile entries**

In `linux/drivers/misc/Kconfig`:

```kconfig
config QEMU_TZC400_TEST
	tristate "QEMU TZC-400 test interface"
	depends on ARM64
	help
	  Expose a test-only ioctl for configuring the QEMU TZC-400 model
	  through a platform SMC.
```

In `linux/drivers/misc/Makefile`:

```make
obj-$(CONFIG_QEMU_TZC400_TEST) += qemu_tzc400.o
```

- [ ] **Step 3: Add host test**

Create `optee_examples/qemu_tzc_core_isolation/host/main.c`:

```c
// SPDX-License-Identifier: BSD-2-Clause

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define QEMU_TZC400_IOCTL_ALLOC  _IOR('z', 1, struct qemu_tzc400_alloc)
#define QEMU_TZC400_IOCTL_CONFIG _IOW('z', 2, struct qemu_tzc400_region)
#define QEMU_TZC400_IOCTL_TOUCH  _IOWR('z', 3, struct qemu_tzc400_touch)
#define TZC_REGION_S_RDWR        (3u << 30)
#define TZC_REGION_ACCESS_RDWR(id) ((1u << (id)) | (1u << (16 + (id))))

struct qemu_tzc400_region {
	uint32_t filters;
	uint32_t region;
	uint64_t base;
	uint64_t top;
	uint32_t sec_attr;
	uint32_t nsaid_permissions;
};

struct qemu_tzc400_alloc {
	uint64_t phys;
	uint64_t size;
};

struct qemu_tzc400_touch {
	uint32_t write;
	uint32_t value;
};

static void pin_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0) {
		perror("sched_setaffinity");
		exit(1);
	}
}

int main(int argc, char **argv)
{
	int fd;
	struct qemu_tzc400_region region;
	struct qemu_tzc400_alloc alloc;
	struct qemu_tzc400_touch touch;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <allowed-cpu> <denied-cpu>\n", argv[0]);
		return 2;
	}

	fd = open("/dev/qemu_tzc400", O_RDWR);
	if (fd < 0) {
		perror("open /dev/qemu_tzc400");
		return 1;
	}

	if (ioctl(fd, QEMU_TZC400_IOCTL_ALLOC, &alloc) != 0) {
		perror("alloc");
		return 1;
	}

	region.filters = 1;
	region.region = 1;
	region.base = alloc.phys;
	region.top = alloc.phys + alloc.size - 1;
	region.sec_attr = TZC_REGION_S_RDWR;
	region.nsaid_permissions = TZC_REGION_ACCESS_RDWR(atoi(argv[1]));

	if (ioctl(fd, QEMU_TZC400_IOCTL_CONFIG, &region) != 0) {
		perror("ioctl");
		return 1;
	}

	pin_cpu(atoi(argv[1]));
	touch.write = 1;
	touch.value = 0x544a4334;
	if (ioctl(fd, QEMU_TZC400_IOCTL_TOUCH, &touch) != 0) {
		perror("allowed touch");
		return 1;
	}
	printf("allowed cpu write ok\n");

	pin_cpu(atoi(argv[2]));
	printf("denied cpu read begins\n");
	touch.write = 0;
	touch.value = 0;
	if (ioctl(fd, QEMU_TZC400_IOCTL_TOUCH, &touch) != 0) {
		perror("denied touch");
		return 1;
	}
	printf("value=%x\n", touch.value);
	return 0;
}
```

- [ ] **Step 4: Add example Makefile**

Create `optee_examples/qemu_tzc_core_isolation/Makefile`:

```make
CC ?= aarch64-linux-gnu-gcc
CFLAGS += -Wall -Wextra -O2

all: qemu_tzc_core_isolation

qemu_tzc_core_isolation: host/main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f qemu_tzc_core_isolation
```

- [ ] **Step 5: Build guest pieces**

Run:

```bash
make -C build -f qemu_v8.mk QEMU_TZC400=y linux
make -C optee_examples/qemu_tzc_core_isolation
```

Expected: Linux kernel and test binary build successfully.

## Task 8: Build, Boot, and Verify End-to-End Isolation

**Files:**
- Modify only files touched by Tasks 1-7 if verification exposes defects.

- [ ] **Step 1: Configure QEMU build if needed**

Run:

```bash
mkdir -p qemu/build
qemu/configure --target-list=aarch64-softmmu --enable-tcg --disable-werror --prefix=/tmp/qemu-tzc400
```

Expected:

```text
QEMU
```

in the configure summary.

- [ ] **Step 2: Run QEMU device tests**

Run:

```bash
meson test -C qemu/build tzc400-test --suite qtest-aarch64 --print-errorlogs
```

Expected:

```text
Ok:
```

with `/tzc400/ids` and `/tzc400/region-programming` passing.

- [ ] **Step 3: Build the OP-TEE QEMU stack**

Run:

```bash
make -C build -f qemu_v8.mk QEMU_TZC400=y QEMU_SMP=4 all
```

Expected:

```text
Building qemu
Building arm-tf
Building optee-os
Building linux
```

and the build exits with status 0.

- [ ] **Step 4: Boot with TZC enabled**

Run:

```bash
make -C build -f qemu_v8.mk QEMU_TZC400=y QEMU_SMP=4 run-only
```

Expected QEMU command contains:

```text
-machine virt,acpi=off,secure=on,tzc400=on
```

Expected secure-world log contains a TZC-400 probe and no wrong-ID panic.

- [ ] **Step 5: Verify allowed-core access**

Inside the guest, run:

```bash
modprobe qemu_tzc400
/usr/bin/qemu_tzc_core_isolation 1 0
```

Expected before denied access:

```text
allowed cpu write ok
denied cpu read begins
```

- [ ] **Step 6: Verify denied-core fault**

The denied CPU should hit a synchronous external abort because TZC action is `ERR`.

Expected Linux output:

```text
Synchronous External Abort
```

Expected QEMU trace when running with `-trace tzc400_access`:

```text
tzc400_access addr 0x........ secure 0 requester 0 write 0 allowed 0
```

- [ ] **Step 7: Verify right-core still works after fault reboot**

Reboot the guest and configure the same region for NSAID 0:

```bash
/usr/bin/qemu_tzc_core_isolation 0 1
```

Expected first write succeeds on CPU 0 and denied access faults on CPU 1.

## Task 9: Documentation and Risk Notes

**Files:**
- Modify: `qemu/docs/system/arm/virt.rst`
- Create: `docs/tzc400-core-isolation.md`

- [ ] **Step 1: Document QEMU options**

Add to `qemu/docs/system/arm/virt.rst`:

```rst
TrustZone address controller
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``virt`` machine can expose an emulated TZC-400 when TrustZone is enabled:

``-machine virt,secure=on,tzc400=on,tzc400-cpu-nsaids=0,,1,,2,,3``

The TZC-400 controls accesses to the main DRAM window at ``0x40000000``.
Secure accesses use the secure read/write bits in each TZC region. Non-secure
accesses use ``MemTxAttrs.requester_id`` as the TZC-400 NSAID; the
``tzc400-cpu-nsaids`` list assigns NSAIDs to vCPUs in CPU index order.
``tzc400`` requires TCG because QEMU does not expose TrustZone to KVM/HVF guests.
```

- [ ] **Step 2: Add project note**

Create `docs/tzc400-core-isolation.md`:

```markdown
# TZC-400 Core Isolation Notes

This repository models the paper mechanisms as follows:

- vTZ and kvTZ motivate virtual TrustZone state and virtual security controllers.
- Sanctuary and SafeTEE motivate binding isolated normal-world code to specific cores.
- The accelerator paper motivates using TZC-400 NSAIDs as the concrete bus-master identity.

In QEMU, each ARM CPU receives a static `tzc-nsaid`. TCG memory transactions carry
that ID in `MemTxAttrs.requester_id`. The TZC-400 model checks secure/non-secure
state and requester ID against its region registers before forwarding a memory
transaction to protected DRAM.

Limitations:

- This is a functional access-control model, not a cache or timing model.
- The initial machine integration protects `VIRT_MEM`; QEMU secure-only RAM remains
  protected by the existing secure address-space overlay.
- CPU NSAIDs are static per boot. Changing them at runtime requires flushing all
  affected CPU TLBs before untrusted code runs again.
```

- [ ] **Step 3: Run documentation checks**

Run:

```bash
rg -n "tzc400|TZC-400|tzc-nsaid" qemu/docs/system/arm/virt.rst docs/tzc400-core-isolation.md
```

Expected: all new option names and limitations are findable.

## Final Verification Matrix

Run these commands in order:

```bash
ninja -C qemu/build qemu-system-aarch64
meson test -C qemu/build tzc400-test --suite qtest-aarch64 --print-errorlogs
make -C build -f qemu_v8.mk QEMU_TZC400=y QEMU_SMP=4 all
make -C build -f qemu_v8.mk QEMU_TZC400=y QEMU_SMP=4 run-only
```

Inside the guest:

```bash
modprobe qemu_tzc400
/usr/bin/qemu_tzc_core_isolation 1 0
```

Completion criteria:

- QEMU boots with `virt,secure=on,tzc400=on`.
- OP-TEE probes the TZC-400 register block without a wrong-ID panic.
- TF-A accepts valid region SMC calls and rejects out-of-range region IDs.
- CPU with allowed NSAID reads and writes the protected test page.
- CPU with denied NSAID receives a synchronous external abort.
- QEMU failure registers report the denied requester ID.

## Implementation Risks

- QEMU qtest helpers do not carry arbitrary `MemTxAttrs`; access-control tests need a small test master or a direct unit-level hook.
- IOMMU MemoryRegion indexing must preserve both security state and NSAID. This plan uses 17 indexes: one secure index and sixteen non-secure NSAID indexes.
- TF-A SMC handlers in this tree expose `x1` through `x4`; region configuration therefore uses a physical descriptor pointer passed in `x1`.
- The Linux test should allocate the isolated page in kernel space or a reserved memory region so the physical address is reliable.
- Runtime NSAID changes require TLB flushes. This plan uses static boot-time CPU NSAIDs to keep the first implementation deterministic.
