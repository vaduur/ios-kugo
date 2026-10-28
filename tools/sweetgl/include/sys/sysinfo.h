/*
 * sweetgl: Linux sysinfo() for Mesa on iOS. freedreno_screen.c (Mesa 25.3) only reads totalram (the
 * screen's RAM size); iOS has hw.memsize.
 */
#ifndef SWEETGL_SYS_SYSINFO_H
#define SWEETGL_SYS_SYSINFO_H

#include <stdint.h>
#include <string.h>
#include <sys/sysctl.h>

struct sysinfo {
	unsigned long totalram;
	unsigned int mem_unit;
};

static inline int
sysinfo(struct sysinfo *si)
{
	uint64_t mem = 0;
	size_t len = sizeof(mem);
	memset(si, 0, sizeof(*si));
	if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0) {
		return -1;
	}
	si->totalram = (unsigned long)mem;
	si->mem_unit = 1;
	return 0;
}

#endif
