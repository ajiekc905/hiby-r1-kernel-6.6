// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 ajiekc905
/*
 * r1ring.c — read the 6.x dev-loop boot log ring from 4.4 (docs/bringup-log.md).
 *
 * The 6.x kernel's early console writes every printk byte into 1 MiB at phys
 * 0x03f00000, above the 63 MiB both kernels use; it survives a watchdog reset.
 *
 *   r1ring dump    print the last boot's log (header to stderr)
 *   r1ring clear   drop the header so the next boot starts at seq 1
 *   r1ring fill    write a test pattern over the whole window
 *   r1ring check   verify that pattern (run after a watchdog reset)
 *   -a <phys>      other window address
 *
 * Build (static, any mipsel musl GCC):
 *   mipsel-linux-musl-gcc -static -O2 -Wall -Wextra -o r1ring r1ring.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAGIC 0x474c3152u
#define SIZE 0x100000u
#define HDR 64u
#define FILL_MAGIC 0x4c4c4946u

struct ring {
	volatile uint32_t magic;
	volatile uint32_t seq;
	volatile uint32_t head;
	volatile uint32_t cap;
	volatile uint32_t pad[12];
	volatile uint8_t data[];
};

static uint32_t pattern(uint32_t i)
{
	uint32_t x = i * 2654435761u;
	return x ^ (x >> 15) ^ 0xa5a5a5a5u;
}

static int dump(struct ring *r)
{
	uint32_t head = r->head, cap = r->cap, start, n, i;

	if (r->magic != MAGIC) {
		fprintf(stderr, "r1ring: no log (magic %08x)\n", r->magic);
		return 1;
	}
	if (cap == 0 || cap > SIZE - HDR) {
		fprintf(stderr, "r1ring: bad cap %u\n", cap);
		return 1;
	}
	n = head < cap ? head : cap;
	start = head < cap ? 0 : head % cap;
	fprintf(stderr, "r1ring: seq %u, %u bytes written, %u kept%s\n",
		r->seq, head, n, head > cap ? " (wrapped)" : "");
	for (i = 0; i < n; i++)
		putchar(r->data[(start + i) % cap]);
	return fflush(stdout) ? 1 : 0;
}

static int fill(struct ring *r)
{
	volatile uint32_t *w = (volatile uint32_t *)r;
	uint32_t i;

	for (i = 1; i < SIZE / 4; i++)
		w[i] = pattern(i);
	w[0] = FILL_MAGIC;
	printf("r1ring: filled %u words\n", SIZE / 4);
	return 0;
}

static int check(struct ring *r)
{
	volatile uint32_t *w = (volatile uint32_t *)r;
	uint32_t i, bad = 0, first = 0;

	if (w[0] != FILL_MAGIC) {
		printf("r1ring: no fill pattern (word 0 = %08x)\n", w[0]);
		return 1;
	}
	for (i = 1; i < SIZE / 4; i++)
		if (w[i] != pattern(i) && !bad++)
			first = i;
	if (bad)
		printf("r1ring: %u of %u words changed, first at +0x%x\n", bad, SIZE / 4 - 1, first * 4);
	else
		printf("r1ring: pattern intact (%u words)\n", SIZE / 4 - 1);
	return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
	unsigned long phys = 0x03f00000;
	const char *cmd;
	struct ring *r;
	int fd, c, rc;

	while ((c = getopt(argc, argv, "a:")) != -1) {
		if (c != 'a')
			goto usage;
		phys = strtoul(optarg, NULL, 0);
	}
	if (optind != argc - 1)
		goto usage;
	cmd = argv[optind];
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	r = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys);
	if (r == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	if (!strcmp(cmd, "dump"))
		rc = dump(r);
	else if (!strcmp(cmd, "clear"))
		rc = (r->magic = 0);
	else if (!strcmp(cmd, "fill"))
		rc = fill(r);
	else if (!strcmp(cmd, "check"))
		rc = check(r);
	else
		goto usage;
	munmap(r, SIZE);
	close(fd);
	return rc;
usage:
	fprintf(stderr, "usage: r1ring [-a phys] dump|clear|fill|check\n");
	return 2;
}
