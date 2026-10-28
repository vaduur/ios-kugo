/*
 * libdrm + msm DRM emulation for Mesa freedreno on iOS (no DRM here).
 *
 * freedreno's msm backend talks to the kernel only through the libdrm calls
 * declared in include/xf86drm.h and one mmap (freedreno_bo.c, hooked by
 * mesa.patch to sweet_drm_bo_map). This file answers them in user space on
 * top of the SweetGPU user client (xnu iokit/Kernel/arm/SweetGPU.cpp):
 *
 *   GEM_NEW / GEM_CLOSE      sel 10 / 11: wired buffer, kernel GPU VA,
 *                            write-combined mapping made by the kernel
 *   GEM_INFO GET_OFFSET      handle << 32; sweet_drm_bo_map() turns it back
 *                            into the kernel's mapping
 *   GEM_INFO GET_IOVA        the kernel GPU VA (softpin, UAPI 1.7)
 *   GEM_SUBMIT               sel 13: CMD_BUF cmds become IBs at bo VA +
 *                            offset; every listed bo remembers the fence
 *   WAIT_FENCE, CPU_PREP     the read-only fence page, then sel 14
 *   GET_PARAM                sel 15 (chip id, GMEM) and constants
 *
 * One device per process; the "fd" is /dev/null (Mesa dup()s and closes
 * it). sweet_drm_import_iosurface() makes a GEM handle for an IOSurface
 * (sel 12) for the EAGL layer.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <IOKit/IOKitLib.h>

#include "xf86drm.h"
#include "drm-uapi/msm_drm.h"
#include "sweet_drm.h"

struct sbo {
	uint64_t iova, map, size;
	uint32_t seq;           /* last submit listing it */
	int used;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static io_connect_t conn;
static int dev_fd = -1;
static struct sbo *bos;
static uint32_t nbos;
static uint32_t chip_id, gmem_size;
static uint64_t gmem_base, max_freq;
static uint32_t queues;

/* Lock held. The GPU comes up a while after boot: callers may retry. */
static void
connect_dev(void)
{
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"));
	if (svc == 0 || IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
		conn = 0;
		return;
	}
	IOObjectRelease(svc);
	uint64_t out[5];
	uint32_t n = 5;
	kern_return_t kr = IOConnectCallScalarMethod(conn, 15, NULL, 0, out, &n);
	if (kr != KERN_SUCCESS || n != 5) {
		IOServiceClose(conn);
		conn = 0;
		return;
	}
	chip_id = (uint32_t)out[0];
	gmem_size = (uint32_t)out[1];
	gmem_base = out[2];
	max_freq = out[3];
	dev_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
}

int
sweet_drm_open(void)
{
	pthread_mutex_lock(&lock);
	if (!conn) {
		connect_dev();
	}
	int fd = conn && dev_fd >= 0 ? dup(dev_fd) : -1;
	pthread_mutex_unlock(&lock);
	return fd;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	return name && strcmp(name, "msm") == 0 ? sweet_drm_open() : -1;
}

/* Lock held: the bo for a handle, or NULL. */
static struct sbo *
sbo(uint32_t handle)
{
	return handle >= 1 && handle <= nbos && bos[handle - 1].size ? &bos[handle - 1] : NULL;
}

/* Lock held. */
static int
sbo_add(uint32_t handle, uint64_t iova, uint64_t map, uint64_t size)
{
	if (handle == 0) {
		return -EINVAL;
	}
	if (handle > nbos) {
		uint32_t n = nbos ? nbos : 256;
		while (n < handle) {
			n *= 2;
		}
		struct sbo *b = realloc(bos, n * sizeof(*b));
		if (b == NULL) {
			return -ENOMEM;
		}
		memset(b + nbos, 0, (n - nbos) * sizeof(*b));
		bos = b;
		nbos = n;
	}
	bos[handle - 1] = (struct sbo){ .iova = iova, .map = map, .size = size };
	return 0;
}

static int
fence_passed(uint32_t seq)
{
	uint64_t in[2] = {seq, 0};
	return IOConnectCallScalarMethod(conn, 14, in, 2, NULL, NULL) == KERN_SUCCESS;
}

/* Wait until seq completed, at most ms milliseconds (UINT64_MAX: forever). */
static int
fence_wait(uint32_t seq, uint64_t ms)
{
	if (fence_passed(seq)) {
		return 0;
	}
	for (;;) {
		uint64_t chunk = ms > 5000 ? 5000 : ms;
		uint64_t in[2] = {seq, chunk};
		kern_return_t kr = IOConnectCallScalarMethod(conn, 14, in, 2, NULL, NULL);
		if (kr == KERN_SUCCESS || fence_passed(seq)) {
			return 0;
		}
		if (kr != kIOReturnTimeout) {
			sweet_drm_log("sweetgl: wait for %u failed: %x", seq, kr);
			return -EIO;
		}
		if (ms != UINT64_MAX) {
			ms -= chunk;
			if (ms == 0) {
				return -ETIMEDOUT;
			}
		}
	}
}

/* msm timeouts are absolute CLOCK_MONOTONIC times; -> milliseconds from now. */
static uint64_t
timeout_ms(const struct drm_msm_timespec *t)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t ns = (t->tv_sec - now.tv_sec) * 1000000000ll + (t->tv_nsec - now.tv_nsec);
	if (t->tv_sec > now.tv_sec + 3600) {
		return UINT64_MAX;
	}
	return ns <= 0 ? 0 : (uint64_t)(ns / 1000000) + 1;
}

static int
get_param(struct drm_msm_param *p)
{
	switch (p->param) {
	case MSM_PARAM_GPU_ID:
		/* from the kernel's chip id (core.major.minor): 0x05030002 -> 530, kugo's 0x05010000 -> 510 */
		p->value = ((chip_id >> 24) & 0xff) * 100 + ((chip_id >> 16) & 0xff) * 10 + ((chip_id >> 8) & 0xff);
		return 0;
	case MSM_PARAM_GMEM_SIZE:
		p->value = gmem_size;
		return 0;
	case MSM_PARAM_GMEM_BASE:
		p->value = gmem_base;
		return 0;
	case MSM_PARAM_CHIP_ID:
		p->value = chip_id;
		return 0;
	case MSM_PARAM_MAX_FREQ:
		p->value = max_freq;
		return 0;
	case MSM_PARAM_TIMESTAMP:
		/* the generic timer and RBBM_ALWAYSON both count XO (19.2 MHz) */
		p->value = mach_absolute_time();
		return 0;
	case MSM_PARAM_PRIORITIES:
		p->value = 1;
		return 0;
	case MSM_PARAM_FAULTS:
	case MSM_PARAM_SUSPENDS:
		p->value = 0;
		return 0;
	default:
		return -EINVAL;
	}
}

static int
gem_new(struct drm_msm_gem_new *r)
{
	uint64_t in = r->size, out[3];
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallScalarMethod(conn, 10, &in, 1, out, &n);
	if (kr != KERN_SUCCESS) {
		sweet_drm_log("sweetgl: bo new %llu failed: %x", r->size, kr);
		return -ENOMEM;
	}
	pthread_mutex_lock(&lock);
	int ret = sbo_add((uint32_t)out[0], out[1], out[2], (r->size + 16383) & ~16383ull);
	pthread_mutex_unlock(&lock);
	if (ret) {
		IOConnectCallScalarMethod(conn, 11, out, 1, NULL, NULL);
		return ret;
	}
	r->handle = (uint32_t)out[0];
	return 0;
}

static int
gem_close(uint32_t handle)
{
	pthread_mutex_lock(&lock);
	struct sbo *b = sbo(handle);
	if (b) {
		memset(b, 0, sizeof(*b));
	}
	pthread_mutex_unlock(&lock);
	uint64_t in = handle;
	return b && IOConnectCallScalarMethod(conn, 11, &in, 1, NULL, NULL) == KERN_SUCCESS ? 0 : -EINVAL;
}

static int
gem_info(struct drm_msm_gem_info *r)
{
	pthread_mutex_lock(&lock);
	struct sbo *b = sbo(r->handle);
	int ret = b ? 0 : -ENOENT;
	if (b) {
		switch (r->info) {
		case MSM_INFO_GET_OFFSET:
			r->value = (uint64_t)r->handle << 32;
			break;
		case MSM_INFO_GET_IOVA:
			r->value = b->iova;
			break;
		case MSM_INFO_SET_NAME:
		case MSM_INFO_SET_METADATA:
			break;
		case MSM_INFO_GET_FLAGS:
			r->value = MSM_BO_WC;
			break;
		default:
			ret = -EINVAL;
		}
	}
	pthread_mutex_unlock(&lock);
	return ret;
}

static int
cpu_prep(struct drm_msm_gem_cpu_prep *r)
{
	pthread_mutex_lock(&lock);
	struct sbo *b = sbo(r->handle);
	uint32_t seq = b ? b->seq : 0;
	int used = b && b->used;
	pthread_mutex_unlock(&lock);
	if (!b) {
		return -ENOENT;
	}
	if (used && !fence_passed(seq)) {
		if (r->op & MSM_PREP_NOSYNC) {
			return -EBUSY;
		}
		int e = fence_wait(seq, timeout_ms(&r->timeout));
		if (e) {
			return e;
		}
	}
	/* the bo is CPU-cached: drop stale lines over what the GPU wrote */
	uint64_t in = r->handle;
	return !used || IOConnectCallScalarMethod(conn, 16, &in, 1, NULL, NULL) == KERN_SUCCESS ? 0 : -EIO;
}

static int
gem_submit(struct drm_msm_gem_submit *r)
{
	if (r->flags & (MSM_SUBMIT_FENCE_FD_IN | MSM_SUBMIT_FENCE_FD_OUT | MSM_SUBMIT_SYNCOBJ_IN |
	    MSM_SUBMIT_SYNCOBJ_OUT | MSM_SUBMIT_FENCE_SN_IN)) {
		return -EINVAL;
	}
	const struct drm_msm_gem_submit_bo *sb = (const void *)(uintptr_t)r->bos;
	const struct drm_msm_gem_submit_cmd *sc = (const void *)(uintptr_t)r->cmds;
	size_t len = 8 + r->nr_cmds * 16 + r->nr_bos * 4;
	uint8_t *buf = malloc(len);
	if (buf == NULL) {
		return -ENOMEM;
	}
	uint32_t *hdr = (uint32_t *)buf, nib = 0;
	uint8_t *ib = buf + 8;
	pthread_mutex_lock(&lock);
	int ret = 0;
	for (uint32_t i = 0; i < r->nr_cmds && ret == 0; i++) {
		if (sc[i].type == MSM_SUBMIT_CMD_IB_TARGET_BUF) {
			continue;
		}
		struct sbo *b = sc[i].submit_idx < r->nr_bos ? sbo(sb[sc[i].submit_idx].handle) : NULL;
		if (b == NULL || sc[i].nr_relocs || (sc[i].size & 3) ||
		    (uint64_t)sc[i].submit_offset + sc[i].size > b->size) {
			ret = -EINVAL;
			break;
		}
		uint64_t va = b->iova + sc[i].submit_offset;
		uint32_t dw[4] = {(uint32_t)va, (uint32_t)(va >> 32), sc[i].size / 4, 0};
		memcpy(ib + 16 * nib++, dw, 16);
	}
	uint32_t *handles = (uint32_t *)(ib + 16 * nib);
	for (uint32_t i = 0; i < r->nr_bos && ret == 0; i++) {
		if (sbo(sb[i].handle) == NULL) {
			ret = -EINVAL;
		}
		handles[i] = sb[i].handle;
	}
	pthread_mutex_unlock(&lock);
	if (ret == 0 && nib == 0) {
		ret = -EINVAL;
	}
	uint64_t seq = 0;
	uint32_t n = 1;
	if (ret == 0) {
		hdr[0] = nib;
		hdr[1] = r->nr_bos;
		kern_return_t kr = IOConnectCallMethod(conn, 13, NULL, 0, buf, 8 + nib * 16 + r->nr_bos * 4,
		    &seq, &n, NULL, NULL);
		if (kr != KERN_SUCCESS) {
			sweet_drm_log("sweetgl: submit (%u IBs, %u bos) failed: %x", nib, r->nr_bos, kr);
			ret = kr == kIOReturnNotReady ? -ENODEV : -EINVAL;
		}
	}
	if (ret == 0) {
		pthread_mutex_lock(&lock);
		for (uint32_t i = 0; i < r->nr_bos; i++) {
			struct sbo *b = sbo(sb[i].handle);
			if (b) {
				b->seq = (uint32_t)seq;
				b->used = 1;
			}
		}
		pthread_mutex_unlock(&lock);
		r->fence = (uint32_t)seq;
	}
	free(buf);
	return ret;
}

static int
command(unsigned long index, void *data)
{
	if (!conn) {
		return -ENODEV;
	}
	switch (index) {
	case DRM_MSM_GET_PARAM:
		return get_param(data);
	case DRM_MSM_SET_PARAM:
		return 0;
	case DRM_MSM_GEM_NEW:
		return gem_new(data);
	case DRM_MSM_GEM_INFO:
		return gem_info(data);
	case DRM_MSM_GEM_CPU_PREP:
		return cpu_prep(data);
	case DRM_MSM_GEM_CPU_FINI:
		return 0;
	case DRM_MSM_GEM_SUBMIT:
		return gem_submit(data);
	case DRM_MSM_WAIT_FENCE: {
		struct drm_msm_wait_fence *r = data;
		return fence_wait(r->fence, timeout_ms(&r->timeout));
	}
	case DRM_MSM_GEM_MADVISE:
		((struct drm_msm_gem_madvise *)data)->retained = 1;
		return 0;
	case DRM_MSM_SUBMITQUEUE_NEW:
		((struct drm_msm_submitqueue *)data)->id = ++queues;
		return 0;
	case DRM_MSM_SUBMITQUEUE_CLOSE:
		return 0;
	case DRM_MSM_SUBMITQUEUE_QUERY: {
		struct drm_msm_submitqueue_query *q = data;
		if (q->param != MSM_SUBMITQUEUE_PARAM_FAULTS || q->len < 4) {
			return -EINVAL;
		}
		*(uint32_t *)(uintptr_t)q->data = 0;
		return 0;
	}
	default:
		sweet_drm_log("sweetgl: unhandled msm ioctl %lu", index);
		return -EINVAL;
	}
}

/* libdrm: 0 or -errno, errno set too */
int
drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	(void)size;
	int ret = command(index, data);
	if (ret) {
		errno = -ret;
	}
	return ret;
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	return drmCommandWrite(fd, index, data, size);
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	int ret;
	if (request == DRM_IOCTL_GEM_CLOSE) {
		ret = gem_close(((struct drm_gem_close *)arg)->handle);
	} else if (((request >> 8) & 0xff) == DRM_IOCTL_BASE && (request & 0xff) >= DRM_COMMAND_BASE &&
	    (request & 0xff) < DRM_COMMAND_END) {
		ret = command((request & 0xff) - DRM_COMMAND_BASE, arg);
	} else {
		sweet_drm_log("sweetgl: unhandled drm ioctl %lx", request);
		ret = -EINVAL;
	}
	if (ret) {
		errno = -ret;
		return -1;
	}
	return 0;
}

void *
sweet_drm_bo_map(int fd, uint64_t offset)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	struct sbo *b = sbo((uint32_t)(offset >> 32));
	void *p = b && b->map ? (void *)(uintptr_t)b->map : (void *)-1;
	pthread_mutex_unlock(&lock);
	return p;
}

int
sweet_drm_import_iosurface(uint32_t surface_id, uint64_t map, uint32_t *handle, uint64_t *size)
{
	if (!conn) {
		return -ENODEV;
	}
	uint64_t in = surface_id, out[3];
	uint32_t n = 3;
	if (IOConnectCallScalarMethod(conn, 12, &in, 1, out, &n) != KERN_SUCCESS) {
		return -ENOENT;
	}
	pthread_mutex_lock(&lock);
	int ret = sbo_add((uint32_t)out[0], out[1], map, out[2]);
	pthread_mutex_unlock(&lock);
	if (ret) {
		IOConnectCallScalarMethod(conn, 11, out, 1, NULL, NULL);
		return ret;
	}
	*handle = (uint32_t)out[0];
	*size = out[2];
	return 0;
}

static drmVersion version = {
	.version_major = 1, .version_minor = 7, .version_patchlevel = 0,
	.name_len = 3, .name = "msm", .date_len = 0, .date = "", .desc_len = 0, .desc = "",
};

drmVersionPtr
drmGetVersion(int fd)
{
	(void)fd;
	return conn ? &version : NULL;
}

void
drmFreeVersion(drmVersionPtr v)
{
	(void)v;
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return -EINVAL;
}

int drmPrimeHandleToFD(int fd, uint32_t h, uint32_t f, int *p) { (void)fd; (void)h; (void)f; (void)p; return -ENOSYS; }
int drmPrimeFDToHandle(int fd, int p, uint32_t *h) { (void)fd; (void)p; (void)h; return -ENOSYS; }
int drmSyncobjCreate(int fd, uint32_t f, uint32_t *h) { (void)fd; (void)f; (void)h; return -ENOSYS; }
int drmSyncobjDestroy(int fd, uint32_t h) { (void)fd; (void)h; return -ENOSYS; }
int drmSyncobjHandleToFD(int fd, uint32_t h, int *o) { (void)fd; (void)h; (void)o; return -ENOSYS; }
int drmSyncobjFDToHandle(int fd, int o, uint32_t *h) { (void)fd; (void)o; (void)h; return -ENOSYS; }
int drmSyncobjImportSyncFile(int fd, uint32_t h, int s) { (void)fd; (void)h; (void)s; return -ENOSYS; }
int drmSyncobjExportSyncFile(int fd, uint32_t h, int *s) { (void)fd; (void)h; (void)s; return -ENOSYS; }
int drmSyncobjReset(int fd, const uint32_t *h, uint32_t n) { (void)fd; (void)h; (void)n; return -ENOSYS; }
int drmSyncobjWait(int fd, uint32_t *h, unsigned n, int64_t t, unsigned f, uint32_t *s)
{ (void)fd; (void)h; (void)n; (void)t; (void)f; (void)s; return -ENOSYS; }
int drmSyncobjTimelineWait(int fd, uint32_t *h, uint64_t *p, unsigned n, int64_t t, unsigned f, uint32_t *s)
{ (void)fd; (void)h; (void)p; (void)n; (void)t; (void)f; (void)s; return -ENOSYS; }
