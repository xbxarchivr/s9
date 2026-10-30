/*
 * s9drop: put files on the Exynos phone's iOS and run one of them, with no USB
 * network and no HFS+ in TWRP. Started once per boot by launchd.
 *
 * USERDATA holds the 16 GiB HFS+ root and then unused space, and /dev/rufsroot0
 * is the whole partition. tools/s9drop.py writes a package there from TWRP:
 *   16 GiB          header page + files (see struct drop_hdr)
 *   16 GiB + 240 MB result: struct res_hdr, then the command's output
 * A package runs once: its generation goes to the result area before the
 * command starts, so a command that panics the kernel does not loop the boot.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DISK            "/dev/rufsroot0"
#define DROP_OFF        0x400000000LL
#define RES_OFF         (DROP_OFF + 240LL * 1024 * 1024)
#define RES_MAX         (16u * 1024 * 1024 - 4096)
#define PAGE            4096u
#define OUT_PATH        "/private/var/ios-s9/s9drop.out"
#define TIMEOUT_S       300
#define MAX_FILES       16
#define MAX_ARGS        16

struct drop_file {
	char path[128];
	uint64_t off;           /* from DROP_OFF, page aligned */
	uint64_t size;
	uint32_t mode;
	uint32_t pad;
};

struct drop_hdr {
	char magic[8];          /* "S9DROP1\n" */
	uint32_t generation;
	uint32_t nfiles;
	char run[512];          /* command, spaces between arguments; may be empty */
	struct drop_file files[MAX_FILES];
};

struct res_hdr {
	char magic[8];          /* "S9RES01\n" */
	uint32_t generation;
	int32_t status;         /* -1 still running, else the wait status */
	uint32_t length;
	uint32_t state;         /* 1 installing, 2 running, 3 done */
	char note[256];
};

_Static_assert(sizeof(struct drop_hdr) <= PAGE, "header fits one page");
_Static_assert(sizeof(struct res_hdr) <= PAGE, "result header fits one page");

static int disk;
static struct res_hdr res;

static int
disk_io(void *buf, size_t len, off_t off, int wr)
{
	/* the raw device wants whole aligned blocks */
	ssize_t n = wr ? pwrite(disk, buf, len, off) : pread(disk, buf, len, off);
	return n == (ssize_t)len ? 0 : -1;
}

static void *
page_alloc(size_t len)
{
	void *p = NULL;
	return posix_memalign(&p, PAGE, len) == 0 ? p : NULL;
}

static void
res_write(void)
{
	static uint8_t *page;
	if (page == NULL && (page = page_alloc(PAGE)) == NULL) {
		return;
	}
	memset(page, 0, PAGE);
	memcpy(page, &res, sizeof(res));
	(void)disk_io(page, PAGE, RES_OFF, 1);
}

static void
res_note(const char *what, const char *arg)
{
	snprintf(res.note, sizeof(res.note), "%s %s: errno %d", what, arg, errno);
	res_write();
}

/* copy the output file so far into the result area */
static void
res_output(void)
{
	static uint8_t *buf;
	const size_t chunk = 256 * 1024;
	if (buf == NULL && (buf = page_alloc(chunk)) == NULL) {
		return;
	}
	int fd = open(OUT_PATH, O_RDONLY);
	if (fd < 0) {
		return;
	}
	uint32_t total = 0;
	for (;;) {
		memset(buf, 0, chunk);
		ssize_t n = read(fd, buf, chunk);
		if (n <= 0 || total >= RES_MAX) {
			break;
		}
		size_t whole = ((size_t)n + PAGE - 1) & ~(size_t)(PAGE - 1);
		if (disk_io(buf, whole, RES_OFF + PAGE + total, 1) != 0) {
			break;
		}
		total += (uint32_t)n;
	}
	close(fd);
	res.length = total;
	res_write();
}

static void
mkparents(const char *path)
{
	char p[128];
	strlcpy(p, path, sizeof(p));
	for (char *s = strchr(p + 1, '/'); s != NULL; s = strchr(s + 1, '/')) {
		*s = 0;
		(void)mkdir(p, 0755);
		*s = '/';
	}
}

static int
install(const struct drop_file *f)
{
	const size_t chunk = 1024 * 1024;
	char tmp[140];
	uint8_t *buf = page_alloc(chunk);
	if (buf == NULL) {
		return -1;
	}
	mkparents(f->path);
	snprintf(tmp, sizeof(tmp), "%s.new", f->path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		res_note("create", tmp);
		free(buf);
		return -1;
	}
	int ret = 0;
	for (uint64_t done = 0; done < f->size && ret == 0;) {
		size_t want = f->size - done < chunk ? (size_t)(f->size - done) : chunk;
		size_t whole = (want + PAGE - 1) & ~(size_t)(PAGE - 1);
		if (disk_io(buf, whole, DROP_OFF + (off_t)(f->off + done), 0) != 0 ||
		    write(fd, buf, want) != (ssize_t)want) {
			res_note("copy", f->path);
			ret = -1;
		}
		done += want;
	}
	if (ret == 0 && (fchmod(fd, f->mode & 07777) != 0 || fsync(fd) != 0)) {
		res_note("finish", f->path);
		ret = -1;
	}
	close(fd);
	if (ret == 0 && rename(tmp, f->path) != 0) {
		res_note("rename", f->path);
		ret = -1;
	}
	free(buf);
	return ret;
}

static void
run(char *cmd)
{
	char *argv[MAX_ARGS + 1];
	int argc = 0;
	extern char **environ;
	for (char *s = strtok(cmd, " "); s != NULL && argc < MAX_ARGS; s = strtok(NULL, " ")) {
		argv[argc++] = s;
	}
	argv[argc] = NULL;
	if (argc == 0) {
		return;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, OUT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	posix_spawn_file_actions_adddup2(&fa, 1, 2);
	pid_t pid;
	int err = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
	if (err != 0) {
		errno = err;
		res_note("spawn", argv[0]);
		return;
	}
	res.state = 2;
	res_write();
	/* output goes to the result area as it comes: a panic keeps what was there */
	time_t start = time(NULL);
	int st = -1;
	for (;;) {
		pid_t w = waitpid(pid, &st, WNOHANG);
		if (w == pid) {
			break;
		}
		if (w < 0) {
			st = -1;
			break;
		}
		res_output();
		if (time(NULL) - start > TIMEOUT_S) {
			kill(pid, SIGKILL);
		}
		sleep(1);
	}
	res.status = st;
	res_output();
}

int
main(void)
{
	struct drop_hdr *h = page_alloc(PAGE);
	struct res_hdr *old = page_alloc(PAGE);
	if (h == NULL || old == NULL) {
		return 1;
	}
	(void)mkdir("/private/var/ios-s9", 0755);
	disk = open(DISK, O_RDWR);
	if (disk < 0) {
		perror(DISK);
		return 1;
	}
	if (disk_io(h, PAGE, DROP_OFF, 0) != 0 || memcmp(h->magic, "S9DROP1\n", 8) != 0) {
		printf("no package\n");
		return 0;
	}
	if (disk_io(old, PAGE, RES_OFF, 0) == 0 && memcmp(old->magic, "S9RES01\n", 8) == 0 &&
	    old->generation == h->generation) {
		printf("generation %u already ran\n", h->generation);
		return 0;
	}
	memcpy(res.magic, "S9RES01\n", 8);
	res.generation = h->generation;
	res.status = -1;
	res.state = 1;
	res_write();
	if (h->nfiles > MAX_FILES) {
		errno = EINVAL;
		res_note("package", "nfiles");
		return 1;
	}
	for (uint32_t i = 0; i < h->nfiles; i++) {
		h->files[i].path[sizeof(h->files[i].path) - 1] = 0;
		if (install(&h->files[i]) != 0) {
			res.state = 3;
			res_write();
			return 1;
		}
	}
	h->run[sizeof(h->run) - 1] = 0;
	run(h->run);
	res.state = 3;
	res_write();
	return 0;
}
