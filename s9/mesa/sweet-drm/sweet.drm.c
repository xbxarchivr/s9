/*
 * The libdrm of xf86drm.h (see there): Panfrost's requests carried out on the
 * SweetMali user client. The kernel hands out one linear block of GPU memory and
 * runs one job at a time, waiting for it, so most of Linux's driver lives here:
 * buffers are extents of that block, a handle is the extent's page number + 1,
 * and every fence has signalled as soon as the submit returns.
 *
 * Panfrost knows its device by a file descriptor; it gets /dev/null.
 */
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>

#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef mach_port_t io_connect_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallScalarMethod(mach_port_t connection, uint32_t selector, const uint64_t *input,
    uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);

#define kIOMapAnywhere  1
#define SEL_INFO        0
#define SEL_ALLOC       1
#define SEL_SUBMIT      2
#define SEL_READ_REG    5
#define PAGE            4096u
#define MAX_EXTENTS     4096

/* the whole block, in order: free or a buffer */
struct extent {
	uint32_t off, len;
	bool used;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct extent extents[MAX_EXTENTS];
static unsigned nextents;
static io_connect_t connection;
static volatile uint8_t *mem;
static uint64_t mem_va, mem_size;
static int connection_fd = -1;
static uint32_t next_syncobj = 1;

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	uint64_t info[4], in, out;
	uint32_t n = 4, one = 1;
	mach_vm_address_t address = 0;
	mach_vm_size_t size = 0;

	(void)busid;
	(void)type;
	if (strcmp(name, "panfrost") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("SweetMali"));
	if (service == MACH_PORT_NULL) {
		return -1;
	}
	kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
	IOObjectRelease(service);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "sweet drm: IOServiceOpen %#x\n", kr);
		return -1;
	}
	if ((kr = IOConnectCallScalarMethod(connection, SEL_INFO, NULL, 0, info, &n)) != KERN_SUCCESS ||
	    (kr = IOConnectMapMemory64(connection, 0, mach_task_self(), &address, &size, kIOMapAnywhere)) != KERN_SUCCESS) {
		fprintf(stderr, "sweet drm: info or map %#x\n", kr);
		return -1;
	}
	/* all of it from the kernel's allocator, which starts at the block's beginning */
	in = info[3];
	if ((kr = IOConnectCallScalarMethod(connection, SEL_ALLOC, &in, 1, &out, &one)) != KERN_SUCCESS || out != info[2]) {
		fprintf(stderr, "sweet drm: alloc %#x\n", kr);
		return -1;
	}
	mem = (volatile uint8_t *)(uintptr_t)address;
	mem_va = info[2];
	mem_size = info[3];
	extents[0] = (struct extent){ 0, (uint32_t)mem_size, false };
	nextents = 1;
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

static uint32_t
read_reg(uint32_t off)
{
	uint64_t in = off, out = 0;
	uint32_t n = 1;
	return IOConnectCallScalarMethod(connection, SEL_READ_REG, &in, 1, &out, &n) == KERN_SUCCESS ? (uint32_t)out : 0;
}

static uint64_t
read_reg64(uint32_t off)
{
	return read_reg(off) | (uint64_t)read_reg(off + 4) << 32;
}

static int
get_param(struct drm_panfrost_get_param *p)
{
	unsigned param = p->param;

	/* as Linux's panfrost_gpu_init_features(), read off the same registers */
	if (param >= DRM_PANFROST_PARAM_TEXTURE_FEATURES0 && param <= DRM_PANFROST_PARAM_TEXTURE_FEATURES3) {
		p->value = read_reg(0x0b0 + (param - DRM_PANFROST_PARAM_TEXTURE_FEATURES0) * 4);
		return 0;
	}
	if (param >= DRM_PANFROST_PARAM_JS_FEATURES0 && param <= DRM_PANFROST_PARAM_JS_FEATURES15) {
		p->value = read_reg(0x0c0 + (param - DRM_PANFROST_PARAM_JS_FEATURES0) * 4);
		return 0;
	}
	switch (param) {
	case DRM_PANFROST_PARAM_GPU_PROD_ID: p->value = read_reg(0x000) >> 16; break;
	case DRM_PANFROST_PARAM_GPU_REVISION: p->value = read_reg(0x000) & 0xffff; break;
	case DRM_PANFROST_PARAM_SHADER_PRESENT: p->value = read_reg64(0x100); break;
	case DRM_PANFROST_PARAM_TILER_PRESENT: p->value = read_reg64(0x110); break;
	case DRM_PANFROST_PARAM_L2_PRESENT: p->value = read_reg64(0x120); break;
	case DRM_PANFROST_PARAM_STACK_PRESENT: p->value = read_reg64(0xe00); break;
	case DRM_PANFROST_PARAM_AS_PRESENT: p->value = read_reg(0x018); break;
	case DRM_PANFROST_PARAM_JS_PRESENT: p->value = read_reg(0x01c); break;
	case DRM_PANFROST_PARAM_L2_FEATURES: p->value = read_reg(0x004); break;
	case DRM_PANFROST_PARAM_CORE_FEATURES: p->value = read_reg(0x008); break;
	case DRM_PANFROST_PARAM_TILER_FEATURES: p->value = read_reg(0x00c); break;
	case DRM_PANFROST_PARAM_MEM_FEATURES: p->value = read_reg(0x010); break;
	case DRM_PANFROST_PARAM_MMU_FEATURES: p->value = read_reg(0x014); break;
	case DRM_PANFROST_PARAM_THREAD_FEATURES: p->value = read_reg(0x0ac); break;
	case DRM_PANFROST_PARAM_MAX_THREADS: p->value = read_reg(0x0a0); break;
	case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ: p->value = read_reg(0x0a4); break;
	case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ: p->value = read_reg(0x0a8); break;
	case DRM_PANFROST_PARAM_COHERENCY_FEATURES: p->value = read_reg(0x300); break;
	case DRM_PANFROST_PARAM_NR_CORE_GROUPS: p->value = (uint64_t)__builtin_popcountll(read_reg64(0x120)); break;
	case DRM_PANFROST_PARAM_THREAD_TLS_ALLOC: p->value = read_reg(0x310); break;
	case DRM_PANFROST_PARAM_AFBC_FEATURES: p->value = read_reg(0x04c); break;
	case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP: p->value = mach_absolute_time(); break;
	case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY: {
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		p->value = 1000000000ULL * tb.denom / tb.numer;
		break;
	}
	default:
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int
create_bo(struct drm_panfrost_create_bo *bo)
{
	uint32_t len = (bo->size + PAGE - 1) & ~(PAGE - 1);
	int ret = -1;

	if (len == 0 || len < bo->size) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&lock);
	for (unsigned i = 0; i < nextents; i++) {
		struct extent *e = &extents[i];
		if (e->used || e->len < len) {
			continue;
		}
		if (e->len > len) {
			if (nextents == MAX_EXTENTS) {
				break;
			}
			memmove(e + 1, e, (nextents - i) * sizeof(*e));
			nextents++;
			e[1].off = e->off + len;
			e[1].len = e->len - len;
			e[1].used = false;
			e->len = len;
		}
		e->used = true;
		/* buffers come zeroed; word by word, this is not ordinary memory */
		volatile uint64_t *p = (volatile uint64_t *)(mem + e->off);
		for (uint32_t w = 0; w < len / 8; w++) {
			p[w] = 0;
		}
		bo->handle = e->off / PAGE + 1;
		bo->offset = mem_va + e->off;
		ret = 0;
		break;
	}
	pthread_mutex_unlock(&lock);
	if (ret != 0) {
		fprintf(stderr, "sweet drm: no room for a buffer of %u bytes\n", bo->size);
		errno = ENOMEM;
	}
	return ret;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	uint32_t off = (handle - 1) * PAGE;

	(void)fd;
	pthread_mutex_lock(&lock);
	for (unsigned i = 0; i < nextents; i++) {
		if (extents[i].off != off) {
			continue;
		}
		extents[i].used = false;
		if (i + 1 < nextents && !extents[i + 1].used) {
			extents[i].len += extents[i + 1].len;
			memmove(&extents[i + 1], &extents[i + 2], (nextents - i - 2) * sizeof(extents[0]));
			nextents--;
		}
		if (i > 0 && !extents[i - 1].used) {
			extents[i - 1].len += extents[i].len;
			memmove(&extents[i], &extents[i + 1], (nextents - i - 1) * sizeof(extents[0]));
			nextents--;
		}
		break;
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static int
submit(struct drm_panfrost_submit *s)
{
	/* Linux: fragment jobs on slot 0, the rest on slot 1 */
	uint64_t in[2] = { s->jc, (s->requirements & PANFROST_JD_REQ_FS) ? 0 : 1 }, out[6];
	uint32_t n = 6;
	kern_return_t kr = IOConnectCallScalarMethod(connection, SEL_SUBMIT, in, 2, out, &n);

	/* Linux reports job faults only in the kernel log, and so does SweetMali */
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "sweet drm: job %#llx slot %llu: %#x (kernel log has the details)\n",
		    (unsigned long long)s->jc, (unsigned long long)in[1], kr);
	}
	return 0;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	if (request == DRM_IOCTL_GEM_CLOSE) {
		return drmCloseBufferHandle(fd, ((struct drm_gem_close *)arg)->handle);
	}
	switch (request) {
	case DRM_IOCTL_PANFROST_SUBMIT:
		return submit(arg);
	case DRM_IOCTL_PANFROST_WAIT_BO:
		return 0;
	case DRM_IOCTL_PANFROST_CREATE_BO:
		return create_bo(arg);
	case DRM_IOCTL_PANFROST_MMAP_BO: {
		struct drm_panfrost_mmap_bo *m = arg;
		m->offset = (m->handle - 1) * (uint64_t)PAGE;
		return 0;
	}
	case DRM_IOCTL_PANFROST_GET_PARAM:
		return get_param(arg);
	case DRM_IOCTL_PANFROST_GET_BO_OFFSET: {
		struct drm_panfrost_get_bo_offset *o = arg;
		o->offset = mem_va + (o->handle - 1) * (uint64_t)PAGE;
		return 0;
	}
	case DRM_IOCTL_PANFROST_MADVISE:
		((struct drm_panfrost_madvise *)arg)->retained = 1;
		return 0;
	default:
		errno = ENOSYS;
		return -1;
	}
}

void *
sweet_drm_map(uint64_t offset)
{
	return (void *)(mem + offset);
}

drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = 2;
		version->name = strdup("panfrost");
		version->name_len = 8;
		version->date = strdup("");
		version->desc = strdup("SweetMali");
		version->desc_len = 9;
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	(void)flags;
	*handle = __atomic_fetch_add(&next_syncobj, 1, __ATOMIC_RELAXED);
	return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	(void)handle;
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	(void)handles;
	(void)count;
	return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	(void)handles;
	(void)count;
	(void)timeout_nsec;
	(void)flags;
	if (first_signaled != NULL) {
		*first_signaled = 0;
	}
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

#define U __attribute__((unused))
int drmPrimeHandleToFD(U int fd, U uint32_t handle, U uint32_t flags, U int *prime_fd) { return none(); }
int drmPrimeFDToHandle(U int fd, U int prime_fd, U uint32_t *handle) { return none(); }
int drmSyncobjFDToHandle(U int fd, U int obj_fd, U uint32_t *handle) { return none(); }
int drmSyncobjImportSyncFile(U int fd, U uint32_t handle, U int sync_file_fd) { return none(); }
int drmSyncobjExportSyncFile(U int fd, U uint32_t handle, U int *sync_file_fd) { return none(); }
/* panthor's, which is built but never chosen: the G72 is not a CSF GPU */
int drmSyncobjTimelineWait(U int fd, U uint32_t *handles, U uint64_t *points, U unsigned count, U int64_t timeout_nsec,
    U unsigned flags, U uint32_t *first_signaled) { return none(); }
int drmSyncobjTransfer(U int fd, U uint32_t dst_handle, U uint64_t dst_point, U uint32_t src_handle, U uint64_t src_point,
    U uint32_t flags) { return none(); }
