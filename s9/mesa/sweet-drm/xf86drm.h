/*
 * What Mesa's Panfrost takes from libdrm, for iOS on the Galaxy S9: the kernel
 * has no DRM, only the SweetMali user client. The approach (a stand-in xf86drm.h
 * in Mesa's tree, a descriptor that means nothing) follows HakiOS's
 * tools/mesa/hakios-drm (MIT). The requests and structures are Linux's
 * (Mesa's include/drm-uapi/panfrost_drm.h).
 */
#ifndef SWEET_XF86DRM_H
#define SWEET_XF86DRM_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "drm-uapi/drm.h"

#define DRM_NODE_PRIMARY        0
#define DRM_NODE_RENDER         2
#define DRM_CLOEXEC             O_CLOEXEC
#define DRM_RDWR                O_RDWR

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmCloseBufferHandle(int fd, uint32_t handle);

/* No dma-bufs and no sync files here. */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);

/* Submission waits for the job, so every fence has signalled by the time anyone asks. */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags);

/* Not libdrm's: a buffer's memory in this process, in place of mmap() on the device. */
void *sweet_drm_map(uint64_t offset);

#endif
