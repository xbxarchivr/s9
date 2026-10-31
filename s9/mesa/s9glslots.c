/*
 * s9glslots: are eagl_slots.txt's offsets right for this iOS? The file was read
 * off OpenGLES of iOS 16.5 (20F66) and this phone runs 16.7.16 (20H392). Each gl*
 * function of OpenGLES.framework loads the table from thread-local storage and
 * jumps through one offset, so the offset can be read from its code here.
 *
 *   s9glslots /usr/local/share/eagl_slots.txt
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	return wrong != 0;
}
