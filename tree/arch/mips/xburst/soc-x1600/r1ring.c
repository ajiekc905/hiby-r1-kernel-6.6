// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 ajiekc905
/*
 * Boot log ring for the HiBy R1 dev loop, in place of the UART early_printk.
 * prom_putchar() stores every early console byte in a 1 MiB window above the
 * RAM both kernels use (4.4 boots with mem=63M, this kernel's DT memory is
 * 63 MiB). Writes go through KSEG1, so they reach DRAM at once and survive a
 * watchdog reset; 4.4 reads them back through /dev/mem (devloop/r1ring.c).
 * With keep_bootcon the early console stays registered for the whole boot.
 * Same layout as the letux tree's earlycon-r1ring.c.
 * r1_kexec_cache_sync() runs first in prom_init: 4.4's relocate_kernel jumps
 * without writing back the D-cache, and instruction fetches fill from L2, so
 * build #13-style images executed stale lines (uasm field overflow at cache
 * init, layout and timing dependent).
 * uart_base only satisfies pm.c, whose sleep code prints to a UART: do not
 * suspend until that debug output is removed (pm.h PRINT_DEBUG).
 */
#include <linux/init.h>
#include <linux/types.h>
#include <asm/addrspace.h>
#include <asm/barrier.h>
#include <asm/cacheops.h>
#include <asm/hazards.h>
#include <asm/r4kcache.h>
#include <asm/setup.h>

#define R1RING_MAGIC	0x474c3152	/* "R1LG" */
#define R1RING_SIZE	0x100000
#define R1RING_HDR	64

struct r1ring {
	u32 magic;
	u32 seq;
	u32 head;
	u32 cap;
	u32 pad[12];
	u8 data[];
};

volatile u32 *uart_base;

static struct r1ring *ring;

static struct r1ring *r1ring_open(void)
{
	struct r1ring *r = (struct r1ring *)CKSEG1ADDR(CONFIG_R1_LOG_RING_PHYS);
	u32 seq = r->magic == R1RING_MAGIC ? r->seq + 1 : 1;

	r->magic = 0;
	r->head = 0;
	r->cap = R1RING_SIZE - R1RING_HDR;
	r->seq = seq;
	r->magic = R1RING_MAGIC;
	return r;
}

void prom_putchar(char c)
{
	struct r1ring *r = ring;

	if (c == '\r')
		return;
	if (!r)
		ring = r = r1ring_open();
	r->data[r->head % r->cap] = c;
	r->head++;
}

void __init r1_kexec_cache_sync(void)
{
	unsigned long a;

	for (a = CKSEG0; a < CKSEG0 + 16384; a += 32)
		cache_op(Index_Writeback_Inv_D, a);
	for (a = CKSEG0; a < CKSEG0 + 131072; a += 32)
		cache_op(Index_Writeback_Inv_SD, a);
	for (a = CKSEG0; a < CKSEG0 + 16384; a += 32)
		cache_op(Index_Invalidate_I, a);
	__sync();
	instruction_hazard();
}
