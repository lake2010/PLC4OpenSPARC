/*
 * pcx_cpx.c
 *
 *  Created on: Nov 17, 2013
 *      Author: ntarano
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "types.h"
#include "config.h"
#include "addr_map.h"
#include "reverse_dir.h"
#include "uart.h"
#include "iob.h"
//#include "fpu.h"
#include "pcx_cpx.h"

#define STORE_SIZE_1 0
#define STORE_SIZE_2 1
#define STORE_SIZE_4 2
#define STORE_SIZE_8 3
#define STORE_SIZE_16 4

#define T1_ICACHE_LINE_ALIGN_MASK (~(0x1FULL))
#define T1_L2_CACHE_LINE_ALIGN_MASK (~(0x3FULL))

#define MB_ADDR_ICACHE_LINE_ALIGN_MASK (~(0x1FU))
#define MB_ADDR_QWORD_ALIGN_MASK (~(0xFU))
#define MB_ADDR_L2_CACHE_LINE_ALIGN_MASK (~(0x3FU))

#define T1_L2_CACHE_LINE_OFFSET 0x3F

/*
 * OpenSPARC T1 DRAM physical address starts from 0 and this fact is used to
 * optimize the conversion of T1 DRAM address to Microblaze DRAM address.
 * But in REGRESSION_MODE, T1 DRAM physical address range is not contiguous
 * and can be anywhere in the 39-bit cacheable physical address space.
 */

#ifdef REGRESSION_MODE

#define T1_DRAM_ADDR_2_MB_ADDR(pcx_pkt, t1_addr, mb_addr_ptr) (void) translate_addr(pcx_pkt, t1_addr, mb_addr_ptr)
#define IS_T1_DRAM_ADDR(t1_addr) 0

#else

#define T1_DRAM_ADDR_2_MB_ADDR(pcx_pkt, t1_addr, mb_addr_ptr) *(mb_addr_ptr) = ((t1_addr) + MB_T1_DRAM_START)
#define IS_T1_DRAM_ADDR(t1_addr) ((t1_addr) < T1_DRAM_SIZE)

#endif /* ifndef REGRESSION_MODE */

#define VECINTR_VNET 29

struct pcx_pkt cpu_cas1_packet[T1_NUM_OF_CPUS];
int get_local = 1;
#ifdef T1_FPGA_DUAL_CORE
int get_remote = 1;
#else
int get_remote = 0;
#endif

struct max_mem_config mem_config;

void load_mem_config(struct max_mem_config config){
	memcpy(&mem_config, &config, sizeof(struct max_mem_config));
}

maddr_t t1_addr_to_max(taddr_t t1_addr){
	return (maddr_t)(mem_config.t1_dram_start + t1_addr);
}

int is_t1_dram_addr(taddr_t t1_addr){
	return (t1_addr < mem_config.t1_dram_size);
}

taddr_t max_addr_to_t1(maddr_t max_addr){
	return (taddr_t)(max_addr - mem_config.t1_dram_start);
}

void add_cpx_ctl(struct cpx_pkt* cpx_pkt, struct max_cpx_pkt* max_cpx_pkt){
	max_cpx_pkt->ctrl = 0x1800000000 | cpx_pkt->ctrl;
	max_cpx_pkt->data3 = 0x1000000000 | cpx_pkt->data3;
	max_cpx_pkt->data2 = 0x1000000000 | cpx_pkt->data2;
	max_cpx_pkt->data1 = 0x1000000000 | cpx_pkt->data1;
	max_cpx_pkt->data0 = 0x1000000000 | cpx_pkt->data0;
	max_cpx_pkt->waste2 = 0x0000000000;
	max_cpx_pkt->waste1 = 0x0000000000;
	max_cpx_pkt->waste0 = 0x0000000000;
}

void cpx_pkt_init(struct cpx_pkt *cpx_pkt) {
	cpx_pkt->ctrl = 0x0;
	cpx_pkt->data3 = 0x0;
	cpx_pkt->data2 = 0x0;
	cpx_pkt->data1 = 0x0;
	cpx_pkt->data0 = 0x0;

	CPX_PKT_SET_VALID(cpx_pkt, 1);
}

void print_cpx_pkt(struct cpx_pkt *pkt) {
	printf("\n");
	printf("INFO: cpx_pkt: ctrl 0x%08x \n", pkt->ctrl);
	printf("INFO: cpx_pkt: data3 0x%08x \n", pkt->data3);
	printf("INFO: cpx_pkt: data2 0x%08x \n", pkt->data2);
	printf("INFO: cpx_pkt: data1 0x%08x \n", pkt->data1);
	printf("INFO: cpx_pkt: data0 0x%08x \n", pkt->data0);
	printf("\n");
}

void print_max_cpx_pkt(struct max_cpx_pkt *pkt) {
	printf("\n");
	printf("INFO: cpx_pkt: ctrl 0x%08x \n", (uint32_t)(pkt->ctrl));
	printf("INFO: cpx_pkt: data3 0x%08x \n", (uint32_t)(pkt->data3));
	printf("INFO: cpx_pkt: data2 0x%08x \n", (uint32_t)(pkt->data2));
	printf("INFO: cpx_pkt: data1 0x%08x \n", (uint32_t)(pkt->data1));
	printf("INFO: cpx_pkt: data0 0x%08x \n", (uint32_t)(pkt->data0));
	printf("\n");
}

void print_pcx_pkt(struct pcx_pkt *pcx_pkt) {
	printf("\n");
	printf("INFO: pcx_pkt: addr_hi_ctrl 0x%08x \n", pcx_pkt->addr_hi_ctrl);
	printf("INFO: pcx_pkt: addr_lo 0x%08x \n", pcx_pkt->addr_lo);
	printf("INFO: pcx_pkt: data1 0x%08x \n", pcx_pkt->data1);
	printf("INFO: pcx_pkt: data0 0x%08x \n", pcx_pkt->data0);
	printf("\n");
}

static int invalidate_core_dcache(struct pcx_pkt *pcx_pkt, int core_id,
		int target_core_id, taddr_opt_t t1_addr) {

	int way, pabit54;
	struct cpx_pkt cpx_pkt_buf;
	struct cpx_pkt *cpx_pkt = &cpx_pkt_buf;

	way = invalidate_dcache(target_core_id, t1_addr);
	if (way == -1) {
		return way;
	}

	CPX_PKT_CTRL_EVICT_INV(cpx_pkt, 0);
	cpx_pkt->data3 = 0;
	cpx_pkt->data2 = 0;
	cpx_pkt->data1 = 0;
	cpx_pkt->data0 = 0;

	pabit54 = (t1_addr & 0x30) >> 4;

	switch (pabit54) {
	case 0:
		cpx_pkt->data0 |= (way << 2);
		cpx_pkt->data0 |= 0x1;
		cpx_pkt->data0 <<= (target_core_id * 4);
		break;
	case 1:
		cpx_pkt->data1 = (way << 1);
		cpx_pkt->data1 |= 0x1;
		cpx_pkt->data1 <<= (target_core_id * 3);
		break;
	case 2:
		/* assuming core_id < 2. this code needs to be modified if core_id >= 2 */
		cpx_pkt->data1 = (way << 2);
		cpx_pkt->data1 |= 0x1;
		cpx_pkt->data1 <<= 24;
		cpx_pkt->data1 <<= (target_core_id * 4);
		break;
	case 3:
		/* assuming core_id < 3. this code needs to be modified if core_id >= 3 */
		cpx_pkt->data2 = (way << 1);
		cpx_pkt->data2 |= 0x1;
		cpx_pkt->data2 <<= 24;
		cpx_pkt->data2 <<= (target_core_id * 3);
		break;
	}

	CPX_PKT_REFLECT_THREAD_ID(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_BIS(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_5_4(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_11_6(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_CORE_ID(cpx_pkt, pcx_pkt);

	//send_cpx_pkt(target_core_id, cpx_pkt);

	return way;
}

static int invalidate_core_icache(struct pcx_pkt *pcx_pkt, int core_id,
		int target_core_id, taddr_opt_t t1_addr) {
	int way, pabit5;
	struct cpx_pkt cpx_pkt_buf;
	struct cpx_pkt *cpx_pkt = &cpx_pkt_buf;

	way = invalidate_icache(target_core_id, t1_addr);
	if (way == -1) {
		return way;
	}

	CPX_PKT_CTRL_EVICT_INV(cpx_pkt, 0);
	cpx_pkt->data3 = 0;
	cpx_pkt->data2 = 0;
	cpx_pkt->data1 = 0;
	cpx_pkt->data0 = 0;

	pabit5 = (t1_addr & 0x20) >> 5;

	switch (pabit5) {
	case 0:
		cpx_pkt->data0 |= (way << 2);
		cpx_pkt->data0 |= 0x2;
		cpx_pkt->data0 <<= (target_core_id * 4);
		break;
	case 1:
		cpx_pkt->data1 |= (way << 2);
		cpx_pkt->data1 |= 0x2;
		cpx_pkt->data1 <<= 24;
		cpx_pkt->data1 <<= (target_core_id * 4);
		break;
	}

	CPX_PKT_REFLECT_THREAD_ID(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_BIS(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_5_4(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_11_6(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_CORE_ID(cpx_pkt, pcx_pkt);

	//send_cpx_pkt(target_core_id, cpx_pkt);

	return way;
}

#ifdef T1_FPGA_DUAL_CORE

static void invalidate_other_dcache(struct pcx_pkt *pcx_pkt, taddr_opt_t t1_addr)
{
	int target_core_id;

	int core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	for (target_core_id=0; target_core_id<T1_NUM_OF_CORES; target_core_id++) {
		if (target_core_id != core_id) {
			invalidate_core_dcache(pcx_pkt, core_id, target_core_id, t1_addr);
		}
	}

	return;
}

static void invalidate_other_icache(struct pcx_pkt *pcx_pkt, taddr_opt_t t1_addr)
{
	int target_core_id;

	int core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	for (target_core_id=0; target_core_id<T1_NUM_OF_CORES; target_core_id++) {
		if (target_core_id != core_id) {
			invalidate_core_icache(pcx_pkt, core_id, target_core_id, t1_addr);
		}
	}

	return;
}

static void invalidate_other_cache(struct pcx_pkt *pcx_pkt, taddr_opt_t t1_addr)
{
	int target_core_id;

	int core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	for (target_core_id=0; target_core_id<T1_NUM_OF_CORES; target_core_id++) {
		if (target_core_id != core_id) {
			if (invalidate_core_dcache(pcx_pkt, core_id, target_core_id, t1_addr) == -1) {
				invalidate_core_icache(pcx_pkt, core_id, target_core_id, t1_addr);
			}
		}
	}

	return;
}

#else /* ifdef T1_FPGA_DUAL_CORE */

#define invalidate_other_cache(pcx_pkt, t1_addr)
#define invalidate_other_icache(pcx_pkt, t1_addr)
#define invalidate_other_dcache(pcx_pkt, t1_addr)

#endif /* ifdef T1_FPGA_DUAL_CORE */

void return_load_req(struct pcx_pkt *pcx_pkt, struct cpx_pkt *cpx_pkt,
		taddr_opt_t t1_addr, maddr_t mb_addr, uint32_t preinit_ctrl_flag) {
	int way;
	maddr_t mb_addr_qw_align;
	int core_id;

	core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	if (mb_addr != MB_INVALID_ADDR) {

		mb_addr_qw_align = mb_addr & MB_ADDR_QWORD_ALIGN_MASK;

		cpx_pkt->data3 = *(uint32_t *) (mb_addr_qw_align + 0x0);
		cpx_pkt->data2 = *(uint32_t *) (mb_addr_qw_align + 0x4);
		cpx_pkt->data1 = *(uint32_t *) (mb_addr_qw_align + 0x8);
		cpx_pkt->data0 = *(uint32_t *) (mb_addr_qw_align + 0xC);
	}

	CPX_PKT_CTRL_LOAD(cpx_pkt, preinit_ctrl_flag);
	CPX_PKT_REFLECT_NC_THREAD_ID(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_PREFETCH(cpx_pkt, pcx_pkt);

	if (PCX_PKT_IS_CACHEABLE(pcx_pkt)) {

		invalidate_other_icache(pcx_pkt, t1_addr);

		way = invalidate_icache(core_id, t1_addr);
		if (way != -1) {
			CPX_PKT_SET_WV(cpx_pkt, 1);
			CPX_PKT_SET_WAY(cpx_pkt, way);
		}
	}

	//send_cpx_pkt(core_id, cpx_pkt);

	return;
}

static void process_load_invalidate(struct pcx_pkt *pcx_pkt,
		struct cpx_pkt *cpx_pkt) {
	taddr_opt_t t1_addr;
	int core_id;

	core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);
	t1_addr = PCX_PKT_GET_T1_ADDR_OPT(pcx_pkt);

	// Invalidate all ways for the given index
	dcache_invalidate_all_ways(core_id, t1_addr);

	// Send store ack packet back to core to invalidate cachelines in core
	cpx_pkt->data0 = 0;
	cpx_pkt->data1 = 0;
	cpx_pkt->data2 = 0;
	cpx_pkt->data3 = 0;

	CPX_PKT_CTRL_STORE_ACK(cpx_pkt, 0);
	CPX_PKT_REFLECT_THREAD_ID(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_5_4(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_11_6(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_CORE_ID(cpx_pkt, pcx_pkt);
	CPX_PKT_SET_STORE_ACK_INVALIDATE_ALL(cpx_pkt, 0x1);

	//send_cpx_pkt(core_id, cpx_pkt);

	return;
}

static void process_load_fast(struct pcx_pkt *pcx_pkt, struct cpx_pkt *cpx_pkt) {
	uint_t l1_way;
	int signed_way;
	int core_id;

	taddr_opt_t t1_addr;
	maddr_t mb_addr, mb_addr_qw_align;

	core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	t1_addr = PCX_PKT_GET_T1_ADDR_OPT(pcx_pkt);
	mb_addr = t1_addr_to_max(t1_addr);

	l1_way = PCX_PKT_GET_L1_WAY(pcx_pkt);
	add_dcache_line(core_id, t1_addr, l1_way);

	CPX_PKT_CTRL_LOAD(cpx_pkt, 0);

	mb_addr_qw_align = mb_addr & MB_ADDR_QWORD_ALIGN_MASK;

	cpx_pkt->data3 = *(uint32_t *) (mb_addr_qw_align + 0x0);
	cpx_pkt->data2 = *(uint32_t *) (mb_addr_qw_align + 0x4);
	cpx_pkt->data1 = *(uint32_t *) (mb_addr_qw_align + 0x8);
	cpx_pkt->data0 = *(uint32_t *) (mb_addr_qw_align + 0xC);

	CPX_PKT_REFLECT_THREAD_ID(cpx_pkt, pcx_pkt);

	invalidate_other_icache(pcx_pkt, t1_addr);

	signed_way = invalidate_icache(core_id, t1_addr);
	if (signed_way != -1) {
		CPX_PKT_SET_WV(cpx_pkt, 1);
		CPX_PKT_SET_WAY(cpx_pkt, signed_way);
	}

	//send_cpx_pkt(core_id, cpx_pkt);

	return;
}

static void process_load(struct pcx_pkt *pcx_pkt, struct cpx_pkt *cpx_pkt) {
	uint_t l1_way;
	taddr_t t1_addr;
	maddr_t mb_addr;
	int core_id;

	enum addr_type addr_type = MB_MEM_ADDR;

	core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	t1_addr = PCX_PKT_GET_T1_ADDR(pcx_pkt);
	if (is_t1_dram_addr(t1_addr)) {
		mb_addr = t1_addr_to_max(t1_addr);
	} else {
		addr_type = translate_addr(pcx_pkt, t1_addr, &mb_addr);
	}

	switch (addr_type) {

	case MB_MEM_ADDR:
		if (PCX_PKT_IS_INVALIDATE(pcx_pkt)) {
			process_load_invalidate(pcx_pkt, cpx_pkt);
			return;
		}

		if (PCX_PKT_IS_CACHEABLE(pcx_pkt)) {
			l1_way = PCX_PKT_GET_L1_WAY(pcx_pkt);
			add_dcache_line(core_id, t1_addr, l1_way);
		}

		return_load_req(pcx_pkt, cpx_pkt, t1_addr, mb_addr, 0);
		break;

	case UART_ADDR:
		process_uart_load(pcx_pkt, cpx_pkt, t1_addr); // TODO
		break;

	case ETH_ADDR:
		//mbfw_snet_load(pcx_pkt, cpx_pkt, t1_addr);	//	TODO
		break;

	case IOB_ADDR:
		process_iob_load(pcx_pkt, cpx_pkt, t1_addr); // TODO
		break;

	default:
		printf("ERROR: process_load(): Internal error. unknown "
			"addr_type %d \r\n", addr_type);
		print_pcx_pkt(pcx_pkt);
	}

	return;
}

static void process_store_fast(struct pcx_pkt *pcx_pkt, struct cpx_pkt *cpx_pkt) {
	taddr_opt_t t1_addr;
	maddr_t mb_addr;

	int way;
	int pcx_size;
	int pabit5, pabit54;
	int core_id;

	core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	t1_addr = PCX_PKT_GET_T1_ADDR_OPT(pcx_pkt);
	mb_addr = t1_addr_to_max(t1_addr);

	pcx_size = PCX_PKT_GET_SIZE(pcx_pkt);

	switch (pcx_size) {
	case STORE_SIZE_1:
		*(uint8_t *) mb_addr = pcx_pkt->data0 & 0xFF;
		break;
	case STORE_SIZE_2:
		*(uint16_t *) mb_addr = pcx_pkt->data0 & 0xFFFF;
		break;
	case STORE_SIZE_4:
		*(uint32_t *) mb_addr = pcx_pkt->data1;
		break;
	case STORE_SIZE_8:
		*(uint32_t *) mb_addr = pcx_pkt->data1;
		*(uint32_t *) (mb_addr + 4) = pcx_pkt->data0;
		break;
	}

#ifdef REGRESSION_MODE
	uint32_t *addr_ptr;

	if (t1_addr == THREAD_EXIT_STATUS_ADDR_0 ||
			t1_addr == THREAD_EXIT_STATUS_ADDR_1) {
		int cpu_id = PCX_PKT_GET_CPU_ID(pcx_pkt);

		if (pcx_pkt->data1) {
			return_store_ack(pcx_pkt, cpx_pkt, t1_addr, 0, INVALIDATE_ICACHE);

			printf("ERROR: thread %d reached bad trap. \r\n", cpu_id);
			addr_ptr = (uint32_t *) EXITCODE_ADDR;
			*addr_ptr = EXITCODE_BADTRAP;
			exit(1);
		} else {
			printf("INFO: Thread %d reached good trap.\r\n", cpu_id);
		}

		started_cpus &= ~(1U << cpu_id);
		if (started_cpus == 0) {
			return_store_ack(pcx_pkt, cpx_pkt, t1_addr, 0, INVALIDATE_ICACHE);

			printf("INFO: All threads reached good trap. \r\n");
			addr_ptr = (uint32_t *) EXITCODE_ADDR;
			*addr_ptr = EXITCODE_GOODTRAP;
			exit(0);
		}
	}
#endif/* ifdef REGRESSION_MODE */

	invalidate_other_cache(pcx_pkt, t1_addr);

	cpx_pkt->data0 = 0;
	cpx_pkt->data1 = 0;
	cpx_pkt->data2 = 0;
	cpx_pkt->data3 = 0;
	CPX_PKT_CTRL_STORE_ACK(cpx_pkt, 0);

	way = search_dcache(core_id, t1_addr);
	if (way != -1) {
		pabit54 = (t1_addr & 0x30) >> 4;

		switch (pabit54) {
		case 0:
			cpx_pkt->data0 |= (way << 2);
			cpx_pkt->data0 |= 0x1;
			cpx_pkt->data0 <<= (core_id * 4);
			break;
		case 1:
			cpx_pkt->data1 = (way << 1);
			cpx_pkt->data1 |= 0x1;
			cpx_pkt->data1 <<= (core_id * 3);
			break;
		case 2:
			/* assuming core_id < 2. this code needs to be modified if core_id >= 2 */
			cpx_pkt->data1 = (way << 2);
			cpx_pkt->data1 |= 0x1;
			cpx_pkt->data1 <<= 24;
			cpx_pkt->data1 <<= (core_id * 4);
			break;
		case 3:
			/* assuming core_id < 3. this code needs to be modified if core_id >= 3 */
			cpx_pkt->data2 = (way << 1);
			cpx_pkt->data2 |= 0x1;
			cpx_pkt->data2 <<= 24;
			cpx_pkt->data2 <<= (core_id * 3);
			break;
		}
	} else {
		way = invalidate_icache(core_id, t1_addr);
		if (way != -1) {
			pabit5 = (t1_addr & 0x20) >> 5;
			switch (pabit5) {
			case 0:
				cpx_pkt->data0 |= (way << 2);
				cpx_pkt->data0 |= 0x2;
				cpx_pkt->data0 <<= (core_id * 4);
				break;
			case 1:
				cpx_pkt->data1 |= (way << 2);
				cpx_pkt->data1 |= 0x2;
				cpx_pkt->data1 <<= 24;
				cpx_pkt->data1 <<= (core_id * 4);
				break;
			}
		}
	}

	CPX_PKT_REFLECT_THREAD_ID(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_5_4(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_11_6(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_CORE_ID(cpx_pkt, pcx_pkt);

	//send_cpx_pkt(core_id, cpx_pkt);

	return;
}

void return_store_ack(struct pcx_pkt *pcx_pkt, struct cpx_pkt *cpx_pkt,
		taddr_opt_t t1_addr, uint_t preinit_ctrl_flag, int inv_flag) {
	int way;

	int pabit5, pabit54;
	int core_id;

	core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	CPX_PKT_CTRL_STORE_ACK(cpx_pkt, preinit_ctrl_flag);
	cpx_pkt->data3 = 0;
	cpx_pkt->data2 = 0;
	cpx_pkt->data1 = 0;
	cpx_pkt->data0 = 0;

	if (inv_flag != INVALIDATE_NONE) {

		invalidate_other_cache(pcx_pkt, t1_addr);

		if (inv_flag & INVALIDATE_DCACHE) {
			way = invalidate_dcache(core_id, t1_addr);
		} else {
			way = search_dcache(core_id, t1_addr);
		}
		if (way != -1) {
			pabit54 = (t1_addr & 0x30) >> 4;

			switch (pabit54) {
			case 0:
				cpx_pkt->data0 |= (way << 2);
				cpx_pkt->data0 |= 0x1;
				cpx_pkt->data0 <<= (core_id * 4);
				break;
			case 1:
				cpx_pkt->data1 = (way << 1);
				cpx_pkt->data1 |= 0x1;
				cpx_pkt->data1 <<= (core_id * 3);
				break;
			case 2:
				/* assuming core_id < 2. this code needs to be modified if core_id >= 2 */
				cpx_pkt->data1 = (way << 2);
				cpx_pkt->data1 |= 0x1;
				cpx_pkt->data1 <<= 24;
				cpx_pkt->data1 <<= (core_id * 4);
				break;
			case 3:
				/* assuming core_id < 3. this code needs to be modified if core_id >= 3 */
				cpx_pkt->data2 = (way << 1);
				cpx_pkt->data2 |= 0x1;
				cpx_pkt->data2 <<= 24;
				cpx_pkt->data2 <<= (core_id * 3);
				break;
			}
		} else {
			way = invalidate_icache(core_id, t1_addr);
			if (way != -1) {
				pabit5 = (t1_addr & 0x20) >> 5;

				switch (pabit5) {
				case 0:
					cpx_pkt->data0 |= (way << 2);
					cpx_pkt->data0 |= 0x2;
					cpx_pkt->data0 <<= (core_id * 4);
					break;
				case 1:
					cpx_pkt->data1 |= (way << 2);
					cpx_pkt->data1 |= 0x2;
					cpx_pkt->data1 <<= 24;
					cpx_pkt->data1 <<= (core_id * 4);
					break;
				}
			}
		}
	}

	CPX_PKT_REFLECT_THREAD_ID(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_BIS(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_5_4(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_11_6(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_CORE_ID(cpx_pkt, pcx_pkt);

	//send_cpx_pkt(core_id, cpx_pkt);

	return;
}

static int is_l2_miss(struct pcx_pkt *pcx_pkt, taddr_opt_t t1_addr) {
	/*
	 * Since L2 cache is inclusive of L1, a memory access misses L2 if
	 * it is not present in L1.
	 */

	int i;
	int core_id;

	t1_addr = t1_addr & T1_L2_CACHE_LINE_ALIGN_MASK;

	for (i = 0; i < (T1_L2_CACHE_LINE_SIZE / T1_DCACHE_LINE_SIZE); i++) {
		for (core_id = 0; core_id < T1_NUM_OF_CORES; core_id++) {
			if (search_dcache(core_id, t1_addr) != -1) {
				return 0;
			}
		}
		t1_addr += T1_DCACHE_LINE_SIZE;
	}

	for (i = 0; i < (T1_L2_CACHE_LINE_SIZE / T1_ICACHE_LINE_SIZE); i++) {
		for (core_id = 0; core_id < T1_NUM_OF_CORES; core_id++) {
			if (search_icache(core_id, t1_addr) != -1) {
				return 0;
			}
		}
		t1_addr += T1_ICACHE_LINE_SIZE;
	}

	return 1;
}

static void process_bis_bst(struct pcx_pkt *pcx_pkt, struct cpx_pkt *cpx_pkt) {
	taddr_opt_t t1_addr;
	maddr_t mb_addr;

	int i;
	uint32_t *tmp_ptr;

	int n_words_in_l2_cache_line = T1_L2_CACHE_LINE_SIZE / sizeof(uint32_t);

	t1_addr = PCX_PKT_GET_T1_ADDR_OPT(pcx_pkt);
	mb_addr = t1_addr_to_max(t1_addr);

	if (PCX_PKT_GET_BST(pcx_pkt) == 0) {

		/* block store init */

		if ((t1_addr & T1_L2_CACHE_LINE_OFFSET) == 0) {
			/* access to the first word of L2 cache line */
			if (is_l2_miss(pcx_pkt, t1_addr)) {
				tmp_ptr = (uint32_t *) mb_addr;
				for (i = 0; i < n_words_in_l2_cache_line; i++) {
					*tmp_ptr++ = 0;
				}
			}
		}
	}

	*(uint32_t *) mb_addr = pcx_pkt->data1;
	*(uint32_t *) (mb_addr + 4) = pcx_pkt->data0;

	return_store_ack(pcx_pkt, cpx_pkt, t1_addr, 0, INVALIDATE_DCACHE
			| INVALIDATE_ICACHE);

	return;
}

static void process_store(struct pcx_pkt *pcx_pkt, struct cpx_pkt *cpx_pkt) {
	taddr_t t1_addr;
	maddr_t mb_addr;
	int pcx_size;

	enum addr_type addr_type = MB_MEM_ADDR;

	t1_addr = PCX_PKT_GET_T1_ADDR(pcx_pkt);
	if (is_t1_dram_addr(t1_addr)) {
		mb_addr = t1_addr_to_max(t1_addr);
	} else {
		addr_type = translate_addr(pcx_pkt, t1_addr, &mb_addr);
	}

	switch (addr_type) {

	case MB_MEM_ADDR:
		pcx_size = PCX_PKT_GET_SIZE(pcx_pkt);

		switch (pcx_size) {
		case STORE_SIZE_1:
			*(uint8_t *) mb_addr = pcx_pkt->data0 & 0xFF;
			break;
		case STORE_SIZE_2:
			*(uint16_t *) mb_addr = pcx_pkt->data0 & 0xFFFF;
			break;
		case STORE_SIZE_4:
			*(uint32_t *) mb_addr = pcx_pkt->data1;
			break;
		case STORE_SIZE_8:
			*(uint32_t *) mb_addr = pcx_pkt->data1;
			*(uint32_t *) (mb_addr + 4) = pcx_pkt->data0;
			break;
		}

		return_store_ack(pcx_pkt, cpx_pkt, t1_addr, 0, INVALIDATE_ICACHE);
		break;

	case UART_ADDR:
		process_uart_store(pcx_pkt, cpx_pkt, t1_addr); // TODO
		break;

	case ETH_ADDR:
		// mbfw_snet_store(pcx_pkt, cpx_pkt, t1_addr);	// TODO
		break;

	case IOB_ADDR:
		process_iob_store(pcx_pkt, cpx_pkt, t1_addr); // TODO
		break;

	default:
		printf("ERROR: process_store(): Internal error. unknown "
			"addr_type %d \r\n", addr_type);
		print_pcx_pkt(pcx_pkt);
		break;
	}

	return;
}

static void process_ifill_invalidate(struct pcx_pkt *pcx_pkt,
		struct cpx_pkt *cpx_pkt) {
	taddr_opt_t t1_addr;
	int core_id;

	core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);
	t1_addr = PCX_PKT_GET_T1_ADDR_OPT(pcx_pkt);

	// Invalidate all ways for the given index
	icache_invalidate_all_ways(core_id, t1_addr);

	// Send store ack packet back to core to invalidate cachelines in core
	cpx_pkt->data0 = 0;
	cpx_pkt->data1 = 0;
	cpx_pkt->data2 = 0;
	cpx_pkt->data3 = 0;

	CPX_PKT_CTRL_STORE_ACK(cpx_pkt, 0);
	CPX_PKT_REFLECT_THREAD_ID(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_5_4(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_ADDR_11_6(cpx_pkt, pcx_pkt);
	CPX_PKT_REFLECT_CORE_ID(cpx_pkt, pcx_pkt);
	CPX_PKT_SET_STORE_ACK_INVALIDATE_ALL(cpx_pkt, 0x2);

	//send_cpx_pkt(core_id, cpx_pkt);

	return;
}

static void return_16bytes_ifill_resp(struct pcx_pkt *pcx_pkt,
		struct cpx_pkt *cpx_pkt, maddr_t mb_addr) {

	maddr_t mb_addr_qw_align = mb_addr & 0xFFFFFFFFFFFFFFF0;// & MB_ADDR_QWORD_ALIGN_MASK;
	int core_id;

	core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	CPX_PKT_CTRL_IFILL_4B(cpx_pkt);
	CPX_PKT_REFLECT_NC_THREAD_ID(cpx_pkt, pcx_pkt);

	cpx_pkt->data3 = *(uint32_t *) (mb_addr_qw_align + 0x0);
	cpx_pkt->data2 = *(uint32_t *) (mb_addr_qw_align + 0x4);
	cpx_pkt->data1 = *(uint32_t *) (mb_addr_qw_align + 0x8);
	cpx_pkt->data0 = *(uint32_t *) (mb_addr_qw_align + 0xC);
	//send_cpx_pkt(core_id, cpx_pkt);

	return;
}

static int process_ifill(struct pcx_pkt *pcx_pkt, struct cpx_pkt *cpx_pkt) {
	taddr_t t1_addr;
	maddr_t mb_addr;
	int core_id;

	maddr_t mb_addr_ic_align;
	taddr_opt_t t1_addr_ic_align;

	uint_t l1_way;
	int signed_way;

	int num_pkts;

	core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	t1_addr = PCX_PKT_GET_T1_ADDR(pcx_pkt);
	//printf("Core #%d requests from 0x%x\n", core_id, t1_addr);
	if (is_t1_dram_addr(t1_addr)){//IS_T1_DRAM_ADDR(t1_addr)) {
		//printf("Is T1 address\n");
		mb_addr = t1_addr_to_max(t1_addr);
		//T1_DRAM_ADDR_2_MB_ADDR(pcx_pkt, t1_addr, &mb_addr);
	} else {
		//printf("Not T1 address!\n");
		translate_addr(pcx_pkt, t1_addr, &mb_addr);
	}

	if (PCX_PKT_IS_INVALIDATE(pcx_pkt)) {
		printf(" invalidate\n");
		process_ifill_invalidate(pcx_pkt, cpx_pkt);
		num_pkts = 1; //!
		return 1;
	} else {
		if (PCX_PKT_IS_IO_SPACE(pcx_pkt)) {

			/* send 16 bytes only */
			return_16bytes_ifill_resp(pcx_pkt, cpx_pkt, mb_addr);
			num_pkts = 1; //!
		} else {
			printf(" 2 packets\n");
			/* send 32 bytes */
			mb_addr_ic_align = mb_addr & MB_ADDR_ICACHE_LINE_ALIGN_MASK;
			t1_addr_ic_align = ((taddr_opt_t) t1_addr)
					& T1_ICACHE_LINE_ALIGN_MASK;

			if (PCX_PKT_IS_CACHEABLE(pcx_pkt)) {
				l1_way = PCX_PKT_GET_L1_WAY(pcx_pkt);
				add_icache_line(core_id, t1_addr_ic_align, l1_way);
			}

			// Prepare first packet
			CPX_PKT_CTRL_IFILL_0(cpx_pkt);
			CPX_PKT_REFLECT_NC_THREAD_ID(cpx_pkt, pcx_pkt);

			cpx_pkt->data3 = *(uint32_t *) (mb_addr_ic_align + 0x0);
			cpx_pkt->data2 = *(uint32_t *) (mb_addr_ic_align + 0x4);
			cpx_pkt->data1 = *(uint32_t *) (mb_addr_ic_align + 0x8);
			cpx_pkt->data0 = *(uint32_t *) (mb_addr_ic_align + 0xC);

			invalidate_other_dcache(pcx_pkt, t1_addr_ic_align);

			signed_way = invalidate_dcache(core_id, t1_addr_ic_align);
			if (signed_way != -1) {
				CPX_PKT_SET_WV(cpx_pkt, 1);
				CPX_PKT_SET_WAY(cpx_pkt, signed_way);
			}

			//send_cpx_pkt(core_id, cpx_pkt);

			// Prepare second packet
			cpx_pkt++; // advance to second packet

			CPX_PKT_CTRL_IFILL_1(cpx_pkt);
			CPX_PKT_REFLECT_NC_THREAD_ID(cpx_pkt, pcx_pkt);

			cpx_pkt->data3 = *(uint32_t *) (mb_addr_ic_align + 0x10);
			cpx_pkt->data2 = *(uint32_t *) (mb_addr_ic_align + 0x14);
			cpx_pkt->data1 = *(uint32_t *) (mb_addr_ic_align + 0x18);
			cpx_pkt->data0 = *(uint32_t *) (mb_addr_ic_align + 0x1C);

			invalidate_other_dcache(pcx_pkt, t1_addr_ic_align + T1_DCACHE_LINE_SIZE);

			signed_way = invalidate_dcache(core_id, t1_addr_ic_align
					+ T1_DCACHE_LINE_SIZE);
			if (signed_way != -1) {
				CPX_PKT_SET_WV(cpx_pkt, 1);
				CPX_PKT_SET_WAY(cpx_pkt, signed_way);
			}

			//send_cpx_pkt(core_id, cpx_pkt);
			num_pkts = 2;
		}
	}

	return num_pkts;
}

static void process_ifill_fast(struct pcx_pkt *pcx_pkt, struct cpx_pkt *cpx_pkt) {
	taddr_opt_t t1_addr;
	maddr_t mb_addr;
	int core_id;

	uint_t l1_way;
	int signed_way;

	core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	t1_addr = PCX_PKT_GET_T1_ADDR_OPT(pcx_pkt);
	mb_addr = t1_addr_to_max(t1_addr);
	//T1_DRAM_ADDR_2_MB_ADDR(pcx_pkt, t1_addr, &mb_addr);

	l1_way = PCX_PKT_GET_L1_WAY(pcx_pkt);

	add_icache_line(core_id, t1_addr, l1_way);

	// Prepare first packet
	CPX_PKT_CTRL_IFILL_0(cpx_pkt);
	CPX_PKT_REFLECT_THREAD_ID(cpx_pkt, pcx_pkt);

	cpx_pkt->data3 = *(uint32_t *) (mb_addr + 0x0);
	cpx_pkt->data2 = *(uint32_t *) (mb_addr + 0x4);
	cpx_pkt->data1 = *(uint32_t *) (mb_addr + 0x8);
	cpx_pkt->data0 = *(uint32_t *) (mb_addr + 0xC);

	invalidate_other_dcache(pcx_pkt, t1_addr);

	signed_way = invalidate_dcache(core_id, t1_addr);
	if (signed_way != -1) {
		CPX_PKT_SET_WV(cpx_pkt, 1);
		CPX_PKT_SET_WAY(cpx_pkt, signed_way);
	}

	//send_cpx_pkt(core_id, cpx_pkt);

	// Advance to next packet
	cpx_pkt++; // TODO: CHECK IF THIS IS RIGHT!!!

	// Prepare second packet
	CPX_PKT_CTRL_IFILL_1(cpx_pkt);
	CPX_PKT_REFLECT_THREAD_ID(cpx_pkt, pcx_pkt);

	cpx_pkt->data3 = *(uint32_t *) (mb_addr + 0x10);
	cpx_pkt->data2 = *(uint32_t *) (mb_addr + 0x14);
	cpx_pkt->data1 = *(uint32_t *) (mb_addr + 0x18);
	cpx_pkt->data0 = *(uint32_t *) (mb_addr + 0x1C);

	invalidate_other_dcache(pcx_pkt, t1_addr + T1_DCACHE_LINE_SIZE);

	signed_way = invalidate_dcache(core_id, t1_addr + T1_DCACHE_LINE_SIZE);
	if (signed_way != -1) {
		CPX_PKT_SET_WV(cpx_pkt, 1);
		CPX_PKT_SET_WAY(cpx_pkt, signed_way);
	}

	//send_cpx_pkt(core_id, cpx_pkt);

	return;
}

static void process_swap_ldstub(struct pcx_pkt *pcx_pkt,
		struct cpx_pkt *cpx_pkt) {
	taddr_opt_t t1_addr;
	maddr_t mb_addr;

	t1_addr = PCX_PKT_GET_T1_ADDR_OPT(pcx_pkt);
	mb_addr = t1_addr_to_max(t1_addr);

	return_load_req(pcx_pkt, cpx_pkt, t1_addr, mb_addr, CPX_PKT_CTRL_ATOMIC);

	if (PCX_PKT_GET_SIZE(pcx_pkt) == 0) {
		*(uint8_t *) mb_addr = pcx_pkt->data1; /* LDSTUB */
	} else {
		*(uint32_t *) mb_addr = pcx_pkt->data1; /* SWAP */
	}

	// Advance ptr
	cpx_pkt++;

	return_store_ack(pcx_pkt, cpx_pkt, t1_addr, CPX_PKT_CTRL_ATOMIC,
			INVALIDATE_DCACHE | INVALIDATE_ICACHE);

	return;
}

static void process_cas(struct pcx_pkt *load_pcx_pkt, struct pcx_pkt *pcx_pkt,
		struct cpx_pkt *cpx_pkt) {
	taddr_opt_t t1_addr;
	maddr_t mb_addr;
	int pcx_size;

	uint64_t casx_cmp_value;
	uint64_t casx_mem_value;

	t1_addr = PCX_PKT_GET_T1_ADDR_OPT(pcx_pkt);
	mb_addr = t1_addr_to_max(t1_addr);
	pcx_size = PCX_PKT_GET_SIZE(pcx_pkt);

	return_load_req(pcx_pkt, cpx_pkt, t1_addr, mb_addr, CPX_PKT_CTRL_ATOMIC);

	// Advance pointer
	cpx_pkt++;

	switch (pcx_size) {

	case 0x2: /* CASA instruction 32-bit */
		if (*(uint32_t *) mb_addr == load_pcx_pkt->data1) {
			if ((mb_addr & 0x7) == 0) {
				*(uint32_t *) mb_addr = pcx_pkt->data1;
			} else {
				*(uint32_t *) mb_addr = pcx_pkt->data0;
			}
		}
		break;

	case 0x3: /* CASXA */
		casx_cmp_value = ((uint64_t) (load_pcx_pkt->data1) << 32)
				| load_pcx_pkt->data0;
		casx_mem_value = *(uint64_t *) mb_addr;

		if (casx_cmp_value == casx_mem_value) {
			uint64_t casx_write_value = ((uint64_t) (pcx_pkt->data1) << 32)
					| pcx_pkt->data0;
			*(uint64_t *) mb_addr = casx_write_value;
		}
		break;

	default:
		printf("ERROR: process_cas(): unknown pcx_size %d \r\n", pcx_size);
		print_pcx_pkt(pcx_pkt);
		return;
	}

	return_store_ack(load_pcx_pkt, cpx_pkt, t1_addr, CPX_PKT_CTRL_ATOMIC,
			INVALIDATE_DCACHE | INVALIDATE_ICACHE);

	return;
}

static void process_int_flush(struct pcx_pkt *pcx_pkt, struct cpx_pkt *cpx_pkt) {
	int target_core_id = PCX_PKT_GET_CORE_ID(pcx_pkt);

	cpx_pkt_init(cpx_pkt);

	CPX_PKT_SET_RTNTYP(cpx_pkt, CPX_PKT_RTNTYP_INT_FLUSH);
	CPX_PKT_REFLECT_NC_THREAD_ID(cpx_pkt, pcx_pkt);
	CPX_PKT_INT_FLUSH_REFLECT_BITS_17_0(cpx_pkt, pcx_pkt);
	if (PCX_PKT_GET_NC(pcx_pkt)) {
		CPX_PKT_REFLECT_ADDR_5_4(cpx_pkt, pcx_pkt); /* flush packet */
		CPX_PKT_REFLECT_ADDR_11_6(cpx_pkt, pcx_pkt);
		CPX_PKT_REFLECT_CORE_ID(cpx_pkt, pcx_pkt);
	} else {
		target_core_id = pcx_pkt->data0 >> INT_FLUSH_CPU_ID_SHIFT; /* interrupt packet */
		target_core_id = target_core_id & CPU_ID_MASK;
		target_core_id = target_core_id >> CPU_ID_CORE_ID_SHIFT;
	}

	cpx_pkt->data0 = pcx_pkt->data0 & 0x3FFFF;
	cpx_pkt->data2 = cpx_pkt->data0;

	//send_cpx_pkt(target_core_id, cpx_pkt);

	return;
}

// Returns the number of packets to send
int process(struct pcx_pkt *pcx_pkt, struct cpx_pkt *cpx_pkt) {
	if (!pcx_pkt || !PCX_PKT_IS_VALID(pcx_pkt)) {
		return -1;
	}
	uint_t rqtyp = PCX_PKT_GET_RQTYP(pcx_pkt);
	//printf("T%d\n", rqtyp);
	// Process common cases (faster processing)
	if (IS_COMMON_CASE_PCX_PKT(pcx_pkt)) {
		printf("is Common\n");
		return 1;
		if (rqtyp == PCX_REQ_STORE) {
			process_store_fast(pcx_pkt, cpx_pkt);
			return 1;
		} else if (rqtyp == PCX_REQ_LOAD) {
			process_load_fast(pcx_pkt, cpx_pkt);
			return 1;
		} else if (rqtyp == PCX_REQ_IFILL) {
			process_ifill_fast(pcx_pkt, cpx_pkt);
			return 2;
		}
	}
	//printf("is uncommon\n");
	// Process non-common cases (normal processing)
	int num_pkts = 1;
	int cpu_id = -1;

	switch (rqtyp) {

	case PCX_REQ_LOAD:
		printf("LOAD\n");
		process_load(pcx_pkt, cpx_pkt); // DONE
		break;

	case PCX_REQ_STORE:
		printf("STORE\n");
		if (PCX_PKT_IS_BIS_BST(pcx_pkt)) {
			process_bis_bst(pcx_pkt, cpx_pkt); // DONE
		} else {
			process_store(pcx_pkt, cpx_pkt); // DONE
		}
		break;

	case PCX_REQ_IFILL:
		//printf("IFILL");
		num_pkts = process_ifill(pcx_pkt, cpx_pkt); // DONE
		break;

	case PCX_REQ_SWAP_LDSTUB:
		printf("SWAP\n");
		process_swap_ldstub(pcx_pkt, cpx_pkt); // DONE
		num_pkts = 2;
		break;

	case PCX_REQ_CAS_LOAD: // DONE
		printf("CAS_LOAD\n");
		cpu_id = PCX_PKT_GET_CPU_ID(pcx_pkt);
		if (PCX_PKT_GET_CORE_ID(pcx_pkt) == T1_MASTER_CORE_ID) {
			cpu_cas1_packet[cpu_id] = *pcx_pkt;
			get_remote = 0;
		} else {
			cpu_cas1_packet[cpu_id] = *pcx_pkt;
			get_local = 0;
		}
		return -1;

	case PCX_REQ_CAS_STORE: // DONE
		printf("CAS_STORE\n");
		cpu_id = PCX_PKT_GET_CPU_ID(pcx_pkt);
		process_cas(&cpu_cas1_packet[cpu_id], pcx_pkt, cpx_pkt);
		get_local = 1;
#ifdef T1_FPGA_DUAL_CORE
		get_remote = 1;
#else
		get_remote = 0;
#endif
		num_pkts = 2;
		break;

	case PCX_REQ_INT_FLUSH:
		printf("IFLUSH\n");
		process_int_flush(pcx_pkt, cpx_pkt); // DONE
		break;

	/*case PCX_REQ_FP_1:
		process_fp_1(pcx_pkt, cpx_pkt); // DONE
		break;

	case PCX_REQ_FP_2:
		process_fp_2(pcx_pkt, cpx_pkt); // DONE
		break;*/

	default:
		printf("ERROR: t1_system_emulation_loop(): pcx rqtyp "
			"0x%x not yet implemented \r\n", rqtyp);
		print_pcx_pkt(pcx_pkt);
		return -1;
	}

	return num_pkts;
}
