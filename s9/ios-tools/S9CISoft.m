/*
 * S9CISoft: keep CoreImage off its broken OpenGL ES path on the Galaxy S9
 * port (no GPU, no Metal).
 *
 * -[CIContext initWithOptions:] (iOS 16.7.16 20H392) has two renderers:
 * Metal (CIMetalCopyDefaultDevice -> +internalContextWithMTLDevice:options:)
 * and, without a Metal device, GL (+internalGLContextWithOptions: ->
 * +internalContextWithEAGLContext:options:). iOS 16's GLES runs on Metal, so
 * that GL context is broken and CI::GLContext::init() calls a NULL function
 * pointer (CoreImage 0x194B3858C). Every process that makes a default
 * CIContext died there: SpringBoard with Bold Text (CUIShapeEffectStack
 * sharedCIContext), Messages (app icon scaling). There is no CPU renderer
 * branch: kCIContextUseSoftwareRenderer ("software_renderer") changes
 * nothing on this path (v1/v2 of this tweak tried it).
 *
 * So without a Metal device the two GL entry points return NULL. Both return
 * an internal CI::Context*, and initWithOptions: already handles NULL there
 * (cbz after the call at 0x1949A3118: release self, return nil), so callers
 * get a nil CIContext instead of a crash. The Metal check happens once.
 * /var/mobile/Library/S9CISoft/disabled turns it off.
 *
 * Apps load CoreImage late (Messages: through IconServices), after
 * TweakInject has matched filters, so the filter is UIKit and the hooks go
 * in when CoreImage's classes appear: dyld's add-image callbacks run after
 * the ObjC runtime has registered an image's classes and released its lock
 * (objc_addLoadImageFunc would call back with that lock held, and
 * method_setImplementation takes it). What happened goes to log.txt in that
 * directory where the sandbox allows writing (SpringBoard yes, apps no).
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#if __has_feature(objc_arc)
#error "build with -fno-objc-arc"
#endif

#define DIR "/var/mobile/Library/S9CISoft/"

typedef void *(*gl_options_fn)(id, SEL, NSDictionary *);
typedef void *(*gl_eagl_fn)(id, SEL, id, NSDictionary *);
static gl_options_fn orig_glWithOptions;
static gl_eagl_fn orig_glWithEAGL;
static atomic_bool hooked;

static void slog(const char *what)
{
	FILE *f = fopen(DIR "log.txt", "a");
	if (f != NULL) {
		fprintf(f, "%ld %s[%d] %s\n", (long)time(NULL), getprogname(), getpid(), what);
		fclose(f);
	}
}

static BOOL no_metal(void)
{
	static BOOL none;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		id (*create)(void) = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice");
		id dev = create != NULL ? create() : nil;
		none = dev == nil;
		[dev release];
		slog(none ? "no Metal device: CoreImage GL contexts refused" : "Metal device present: CoreImage untouched");
	});
	return none;
}

static void *s9_glWithOptions(id self, SEL _cmd, NSDictionary *options)
{
	if (no_metal()) {
		slog("refused +[CIContext internalGLContextWithOptions:]");
		return NULL;
	}
	return orig_glWithOptions(self, _cmd, options);
}

static void *s9_glWithEAGL(id self, SEL _cmd, id eagl, NSDictionary *options)
{
	if (no_metal()) {
		slog("refused +[CIContext internalContextWithEAGLContext:options:]");
		return NULL;
	}
	return orig_glWithEAGL(self, _cmd, eagl, options);
}

/* dyld calls this for every image already loaded and every later one */
static void image_added(const struct mach_header *mh, intptr_t slide)
{
	(void)mh;
	(void)slide;
	if (atomic_load(&hooked))
		return;
	Class c = objc_getClass("CIContext");
	if (c == Nil)
		return;
	Method a = class_getClassMethod(c, sel_registerName("internalGLContextWithOptions:"));
	Method b = class_getClassMethod(c, sel_registerName("internalContextWithEAGLContext:options:"));
	if ((a == NULL && b == NULL) || atomic_exchange(&hooked, true))
		return;
	if (a != NULL)
		orig_glWithOptions = (gl_options_fn)method_setImplementation(a, (IMP)s9_glWithOptions);
	if (b != NULL)
		orig_glWithEAGL = (gl_eagl_fn)method_setImplementation(b, (IMP)s9_glWithEAGL);
	slog(a != NULL && b != NULL ? "hooked CIContext GL entry points" :
	    a != NULL ? "hooked +internalGLContextWithOptions: only" : "hooked +internalContextWithEAGLContext:options: only");
}

__attribute__((constructor)) static void init(void)
{
	if (access(DIR "disabled", F_OK) == 0)
		return;
	_dyld_register_func_for_add_image(image_added);
}
