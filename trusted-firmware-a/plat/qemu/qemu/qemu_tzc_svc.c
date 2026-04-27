/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <stdbool.h>

#include <common/debug.h>
#include <common/runtime_svc.h>
#include <drivers/arm/tzc400.h>
#include <lib/smccc.h>
#include <lib/xlat_tables/xlat_tables_v2.h>
#include <platform_def.h>

#define QEMU_TZC400_SMC_CONFIG_REGION	U(0xc200ff00)
#define QEMU_TZC400_SMC_REGION0		U(0xc200ff01)
#define QEMU_TZC400_SMC_ENABLE		U(0xc200ff02)

#define QEMU_TZC400_OK			0
#define QEMU_TZC400_E_DENIED		-1
#define QEMU_TZC400_E_RANGE		-2
#define QEMU_TZC400_E_ALIGN		-3
#define QEMU_TZC400_E_STATE		-4

#define QEMU_TZC400_MAX_REGION		U(8)
#define QEMU_TZC400_FILTER_MASK		TZC_400_REGION_ATTR_FILTER_BIT(0)

struct qemu_tzc400_region {
	uint32_t filters;
	uint32_t region;
	uint64_t base;
	uint64_t top;
	uint32_t sec_attr;
	uint32_t nsaid_permissions;
};

static bool qemu_tzc400_in_ns_dram(uint64_t base, uint64_t size)
{
	uint64_t ns_top;
	uint64_t end;

	if (size == 0U) {
		return false;
	}

	ns_top = NS_DRAM0_BASE + NS_DRAM0_SIZE - 1U;
	end = base + size - 1U;

	return (end >= base) && (base >= NS_DRAM0_BASE) && (end <= ns_top);
}

static int qemu_tzc400_validate_region(const struct qemu_tzc400_region *req)
{
	if ((req->region == 0U) || (req->region > QEMU_TZC400_MAX_REGION) ||
	    (req->base > req->top)) {
		return QEMU_TZC400_E_RANGE;
	}

	if (((req->base & (PAGE_SIZE - 1U)) != 0U) ||
	    ((req->top & (PAGE_SIZE - 1U)) != (PAGE_SIZE - 1U))) {
		return QEMU_TZC400_E_ALIGN;
	}

	if (!qemu_tzc400_in_ns_dram(req->base, req->top - req->base + 1U)) {
		return QEMU_TZC400_E_RANGE;
	}

	if ((req->filters & ~QEMU_TZC400_FILTER_MASK) != 0U) {
		return QEMU_TZC400_E_RANGE;
	}

	if (req->sec_attr > TZC_REGION_S_RDWR) {
		return QEMU_TZC400_E_RANGE;
	}

	return QEMU_TZC400_OK;
}

static int qemu_tzc400_config_region(uint64_t req_pa, uint64_t req_size)
{
	struct qemu_tzc400_region req;
	uint64_t map_base;
	size_t map_size;
	int ret;

	if (req_size != sizeof(req)) {
		return QEMU_TZC400_E_RANGE;
	}

	if (!qemu_tzc400_in_ns_dram(req_pa, req_size)) {
		return QEMU_TZC400_E_RANGE;
	}

	map_base = req_pa & ~(uint64_t)(PAGE_SIZE - 1U);
	map_size = (size_t)(((req_pa + req_size + PAGE_SIZE - 1U) &
			     ~(uint64_t)(PAGE_SIZE - 1U)) - map_base);

	ret = mmap_add_dynamic_region(map_base, (uintptr_t)map_base, map_size,
				      MT_MEMORY | MT_RW | MT_NS);
	if (ret != 0) {
		return QEMU_TZC400_E_STATE;
	}

	req = *(const struct qemu_tzc400_region *)(uintptr_t)req_pa;

	ret = mmap_remove_dynamic_region((uintptr_t)map_base, map_size);
	if (ret != 0) {
		return QEMU_TZC400_E_STATE;
	}

	ret = qemu_tzc400_validate_region(&req);
	if (ret != QEMU_TZC400_OK) {
		return ret;
	}

	tzc400_disable_filters();
	tzc400_configure_region(req.filters, req.region, req.base, req.top,
				req.sec_attr, req.nsaid_permissions);
	tzc400_enable_filters();

	return QEMU_TZC400_OK;
}

static int qemu_tzc400_config_region0(uint32_t sec_attr,
				      uint32_t ns_device_access)
{
	if (sec_attr > TZC_REGION_S_RDWR) {
		return QEMU_TZC400_E_RANGE;
	}

	tzc400_disable_filters();
	tzc400_configure_region0(sec_attr, ns_device_access);
	tzc400_enable_filters();

	return QEMU_TZC400_OK;
}

static uintptr_t qemu_tzc_svc_handler(uint32_t smc_fid,
				      u_register_t x1,
				      u_register_t x2,
				      u_register_t x3,
				      u_register_t x4,
				      void *cookie,
				      void *handle,
				      u_register_t flags)
{
	int ret;

	(void)x3;
	(void)x4;
	(void)cookie;

	if (!is_caller_non_secure(flags)) {
		SMC_RET1(handle, QEMU_TZC400_E_DENIED);
	}

	switch (smc_fid) {
	case QEMU_TZC400_SMC_CONFIG_REGION:
		ret = qemu_tzc400_config_region(x1, x2);
		SMC_RET1(handle, ret);
	case QEMU_TZC400_SMC_REGION0:
		ret = qemu_tzc400_config_region0((uint32_t)x1, (uint32_t)x2);
		SMC_RET1(handle, ret);
	case QEMU_TZC400_SMC_ENABLE:
		tzc400_set_action(TZC_ACTION_ERR);
		tzc400_enable_filters();
		SMC_RET1(handle, QEMU_TZC400_OK);
	default:
		WARN("Unimplemented QEMU TZC-400 SiP call: 0x%x\n", smc_fid);
		SMC_RET1(handle, SMC_UNK);
	}
}

DECLARE_RT_SVC(
	qemu_tzc_svc,
	OEN_SIP_START,
	OEN_SIP_END,
	SMC_TYPE_FAST,
	NULL,
	qemu_tzc_svc_handler
);
