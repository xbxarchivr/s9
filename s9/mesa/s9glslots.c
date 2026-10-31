/*
 * s9glslots: are eagl_slots.txt's offsets right for this iOS? The file was read
 * off OpenGLES of iOS 16.5 (20F66) and this phone runs 16.7.16 (20H392). Each gl*
 * function of OpenGLES.framework loads the table from thread-local storage and
 * jumps through one offset, so the offset can be read from its code here.
 *
 *   s9glslots /usr/local/share/eagl_slots.txt [--kick]
 *
 * It also reads how QuartzCore uses the table, and --kick restarts backboardd.
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);

/* the offset of the LDR Xt, [Xn, #imm] that gives the register BR jumps through, or -1 */
static long
offset_of(const uint32_t *code)
{
	for (int i = 0; i < 48; i++) {
		/* RET first: functions with stack arguments call and return, and past it is the next function */
		if (code[i] == 0xd65f03c0) {
			return -1;
		}
		if ((code[i] & 0xfffffc1f) != 0xd61f0000 && (code[i] & 0xfffffc1f) != 0xd63f0000) {     /* BR, BLR Xn */
			continue;
		}
		unsigned reg = (code[i] >> 5) & 31;
		for (int k = i - 1; k >= 0; k--) {
			if ((code[k] & 0xffc00000) == 0xf9400000 && (code[k] & 31) == reg) {    /* LDR (unsigned offset) */
				return (long)((code[k] >> 10) & 0xfff) * 8;
			}
		}
		return -1;
	}
	return -1;
}

static unsigned ldr_count[0x1000];

/*
 * OpenGLES's own functions reach the table at P + offset with P from thread-local
 * storage, and the context at P + 0x20. QuartzCore gets a pointer from
 * -getMacroContextPrivate instead. If that pointer is P, QuartzCore's loads use the
 * offsets of eagl_slots.txt; if it is P + 0x20, they use those less 0x20. Offsets
 * from 0x1000 up are rare enough among all the loads in its code to tell.
 */
static void
scan_quartzcore(const char *slots)
{
	char line[512];
	unsigned at_file = 0, at_less = 0, slots_at_file = 0, slots_at_less = 0;

	dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		if (strstr(_dyld_get_image_name(i), "/QuartzCore") == NULL) {
			continue;
		}
		unsigned long size = 0;
		const uint32_t *text = (const uint32_t *)getsectiondata((const struct mach_header_64 *)_dyld_get_image_header(i),
		    "__TEXT", "__text", &size);
		printf("QuartzCore __text %lu bytes\n", size);
		for (unsigned long w = 0; text != NULL && w < size / 4; w++) {
			if ((text[w] & 0xffc00000) == 0xf9400000) {
				ldr_count[(text[w] >> 10) & 0xfff]++;
			}
		}
	}
	FILE *f = fopen(slots, "r");
	while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
		char *end;
		long off = strtol(line, &end, 16);
		if (strncmp(line, "0x", 2) != 0 || strstr(end, " * ") == NULL || off < 0x1000) {
			continue;
		}
		unsigned a = ldr_count[off / 8], b = ldr_count[(off - 0x20) / 8];
		at_file += a;
		at_less += b;
		slots_at_file += a != 0;
		slots_at_less += b != 0;
		printf("  %#lx: %u loads at it, %u at it less 0x20:%s", off, a, b, end);
	}
	printf("QuartzCore: %u loads on %u used slots at file offsets, %u on %u at those less 0x20; loads of #0x20: %u, of #0: %u\n",
	    at_file, slots_at_file, at_less, slots_at_less, ldr_count[0x20 / 8], ldr_count[0]);

	Class c = objc_getClass("EAGLContext");
	if (c != Nil) {
		const uint32_t *imp = (const uint32_t *)class_getMethodImplementation(c, sel_registerName("getMacroContextPrivate"));
		printf("-getMacroContextPrivate:");
		for (int i = 0; i < 16; i++) {
			printf(" %08x", imp[i]);
			if (imp[i] == 0xd65f03c0) {
				break;
			}
		}
		printf("\n");
	}
}

/*
 * ElleKit's loader runs after the first backboardd has started, so that one has no
 * tweaks. Killed, launchd starts another, which has them. Only on request and once.
 */
static void
kick_backboardd(void)
{
	static int pids[4096];
	char name[64];

	printf("waiting for ElleKit's loader\n");
	sleep(30);
	int bytes = proc_listallpids(pids, sizeof(pids));
	for (int i = 0; i < bytes; i++) {
		if (proc_name(pids[i], name, sizeof(name)) > 0 && strcmp(name, "backboardd") == 0) {
			printf("killing backboardd %d: %s\n", pids[i], kill(pids[i], SIGKILL) == 0 ? "ok" : "failed");
			return;
		}
	}
	printf("no backboardd found\n");
}

int
main(int argc, char **argv)
{
	char line[512];
	int checked = 0, missing = 0, unread = 0, wrong = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2 || !dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW)) {
		printf("FAIL: usage, or no OpenGLES: %s\n", dlerror());
		return 1;
	}
	void *gles = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW);
	/* the code itself, to read how the table is reached */
	static const char *const sample[] = { "glFlush", "glClear", "glTexImage2D", "glCreateProgram" };
	for (unsigned s = 0; s < sizeof(sample) / sizeof(sample[0]); s++) {
		const uint32_t *code = dlsym(gles, sample[s]);
		printf("%s:", sample[s]);
		for (int i = 0; code != NULL && i < 24; i++) {
			printf(" %08x", code[i]);
			if (code[i] == 0xd65f03c0) {
				break;
			}
		}
		printf("\n");
	}
	FILE *f = fopen(argv[1], "r");
	if (f == NULL) {
		printf("FAIL: cannot open %s\n", argv[1]);
		return 1;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char *hash = strchr(line, '#');
		if (hash != NULL) {
			*hash = 0;
		}
		char *word = strtok(line, " \t\n");
		if (word == NULL) {
			continue;
		}
		long want = strtol(word, NULL, 16);
		char *used = strtok(NULL, " \t\n");
		char *name;
		while ((name = strtok(NULL, " \t\n")) != NULL) {
			const uint32_t *code = dlsym(gles, name);
			if (code == NULL) {
				missing++;
				continue;
			}
			long got = offset_of(code);
			checked++;
			if (got < 0) {
				unread++;
				if (unread <= 10) {
					printf("unread %s (table %#lx%s): %08x %08x %08x %08x\n", name, want, used[0] == '*' ? ", used" : "",
					    code[0], code[1], code[2], code[3]);
				}
			} else if (got != want) {
				wrong++;
				printf("WRONG %s: file %#lx, this iOS %#lx%s\n", name, want, got, used[0] == '*' ? " (used by QuartzCore)" : "");
			}
		}
	}
	printf("%d functions checked, %d not found, %d could not be read, %d differ\n", checked, missing, unread, wrong);
	printf(wrong || checked == 0 ? "FAIL\n" : "PASS\n");
	scan_quartzcore(argv[1]);
	if (argc > 2 && strcmp(argv[2], "--kick") == 0) {
		kick_backboardd();
	}
	return wrong != 0;
}
