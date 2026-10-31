/*
 * s9gltest: OpenGL ES through Mesa's Panfrost on the Mali-G72, with no window
 * server. Checks a clear and a shaded triangle by reading the pixels back, then
 * times frames. Run with the argument "soft", it does the same on softpipe, and
 * with no argument it runs itself that way afterwards to compare.
 */
#include <mach/mach_time.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* GL/osmesa.h, as patched */
#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_DEPTH_BITS               0x30
#define OSMESA_STENCIL_BITS             0x31
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_CONTEXT_MINOR_VERSION    0x37
#define OSMESA_ES_PROFILE               0x38

typedef struct osmesa_context *OSMesaContext;
extern OSMesaContext OSMesaCreateContextAttribs(const int *attributes, OSMesaContext share);
extern unsigned char OSMesaMakeCurrent(OSMesaContext context, void *buffer, unsigned type, int width, int height);

#define GL_UNSIGNED_BYTE        0x1401
#define GL_FLOAT                0x1406
#define GL_RGBA                 0x1908
#define GL_VENDOR               0x1f00
#define GL_RENDERER             0x1f01
#define GL_VERSION              0x1f02
#define GL_COLOR_BUFFER_BIT     0x4000
#define GL_TRIANGLES            0x0004
#define GL_FRAGMENT_SHADER      0x8b30
#define GL_VERTEX_SHADER        0x8b31
#define GL_COMPILE_STATUS       0x8b81
#define GL_LINK_STATUS          0x8b82
#define GL_ARRAY_BUFFER         0x8892
#define GL_STATIC_DRAW          0x88e4

extern const unsigned char *glGetString(unsigned name);
extern unsigned glGetError(void);
extern void glViewport(int x, int y, int w, int h);
extern void glClearColor(float r, float g, float b, float a);
extern void glClear(unsigned mask);
extern void glFinish(void);
extern void glReadPixels(int x, int y, int w, int h, unsigned format, unsigned type, void *pixels);
extern unsigned glCreateShader(unsigned type);
extern void glShaderSource(unsigned shader, int count, const char *const *string, const int *length);
extern void glCompileShader(unsigned shader);
extern void glGetShaderiv(unsigned shader, unsigned pname, int *params);
extern void glGetShaderInfoLog(unsigned shader, int size, int *length, char *log);
extern unsigned glCreateProgram(void);
extern void glAttachShader(unsigned program, unsigned shader);
extern void glLinkProgram(unsigned program);
extern void glGetProgramiv(unsigned program, unsigned pname, int *params);
extern void glUseProgram(unsigned program);
extern int glGetUniformLocation(unsigned program, const char *name);
extern void glUniform4f(int location, float a, float b, float c, float d);
extern void glGenBuffers(int n, unsigned *buffers);
extern void glBindBuffer(unsigned target, unsigned buffer);
extern void glBufferData(unsigned target, long size, const void *data, unsigned usage);
extern void glEnableVertexAttribArray(unsigned index);
extern void glVertexAttribPointer(unsigned index, int size, unsigned type, unsigned char normalized, int stride,
    const void *pointer);
extern void glDrawArrays(unsigned mode, int first, int count);

#define W 256
#define H 256
#define FRAMES 100

static unsigned char window[W * H * 4];
static int failures;

static void
check_pixel(const char *what, int x, int y, unsigned r, unsigned g, unsigned b)
{
	unsigned char p[4] = { 0 };
	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
	bool ok = abs(p[0] - (int)r) <= 2 && abs(p[1] - (int)g) <= 2 && abs(p[2] - (int)b) <= 2;
	printf("%s at %d,%d: %u %u %u %u (want %u %u %u): %s\n", what, x, y, p[0], p[1], p[2], p[3], r, g, b,
	    ok ? "ok" : "WRONG");
	failures += !ok;
}

static unsigned
compile(unsigned type, const char *source)
{
	int status = 0;
	char log[1024] = "";
	unsigned s = glCreateShader(type);
	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &status);
	if (!status) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		printf("shader did not compile: %s\n", log);
		failures++;
	}
	return s;
}

static int
run(void)
{
	static const int attributes[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 0, OSMESA_STENCIL_BITS, 0,
		OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, 3, OSMESA_CONTEXT_MINOR_VERSION, 0,
		0
	};
	static const float triangle[] = { -1, -1, 3, -1, -1, 3 };
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);

	OSMesaContext context = OSMesaCreateContextAttribs(attributes, NULL);
	if (context == NULL || !OSMesaMakeCurrent(context, window, GL_UNSIGNED_BYTE, W, H)) {
		printf("FAIL: no context\n");
		return 1;
	}
	printf("vendor %s, renderer %s, version %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
	    glGetString(GL_VERSION));
	glViewport(0, 0, W, H);

	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	check_pixel("clear", W / 2, H / 2, 0, 255, 0);

	unsigned program = glCreateProgram();
	glAttachShader(program, compile(GL_VERTEX_SHADER,
	    "#version 300 es\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n"));
	glAttachShader(program, compile(GL_FRAGMENT_SHADER,
	    "#version 300 es\nprecision mediump float;\nuniform vec4 c;\nout vec4 o;\nvoid main() { o = c; }\n"));
	glLinkProgram(program);
	int linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
		printf("FAIL: program did not link\n");
		return 1;
	}
	glUseProgram(program);
	int colour = glGetUniformLocation(program, "c");
	unsigned buffer;
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, NULL);

	glUniform4f(colour, 1, 0, 0, 1);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	check_pixel("red triangle", W / 2, H / 2, 255, 0, 0);

	glClearColor(0, 0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glUniform4f(colour, 1, 1, 0, 1);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	check_pixel("yellow after a clear", 10, 10, 255, 255, 0);
	printf("GL error %#x\n", glGetError());

	uint64_t t0 = mach_absolute_time();
	for (int i = 0; i < FRAMES; i++) {
		glClear(GL_COLOR_BUFFER_BIT);
		glUniform4f(colour, (float)i / FRAMES, 0, 1, 1);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glFinish();
	}
	uint64_t ns = (mach_absolute_time() - t0) * tb.numer / tb.denom;
	printf("%d frames of %dx%d: %llu us each\n", FRAMES, W, H, ns / 1000 / FRAMES);
	check_pixel("last frame", W / 2, H / 2, (FRAMES - 1) * 255 / FRAMES, 0, 255);
	printf(failures ? "FAIL\n" : "PASS\n");
	return failures != 0;
}

int
main(int argc, char **argv)
{
	extern char **environ;
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc > 1 && strcmp(argv[1], "soft") == 0) {
		setenv("OSMESA_SOFTWARE", "1", 1);
		return run();
	}
	printf("== GPU\n");
	int ret = run();
	printf("== softpipe\n");
	char *child[] = { argv[0], "soft", NULL };
	pid_t pid;
	int status = 0;
	if (posix_spawn(&pid, argv[0], NULL, NULL, child, environ) == 0) {
		waitpid(pid, &status, 0);
	}
	return ret;
}
