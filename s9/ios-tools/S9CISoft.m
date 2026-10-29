/*
 * S9CISoft: CoreImage on the CPU for the Galaxy S9 port (no GPU, no Metal).
 *
 * Without a Metal device, -[CIContext initWithOptions:] falls back to an
 * OpenGL ES context (+[CIContext internalContextWithEAGLContext:options:]).
 * iOS 16's GLES runs on Metal, so that context is broken and
 * CI::GLContext::init() calls a NULL function pointer (iOS 16.7.16 20H392,
 * CoreImage 0x194B3858C). Every process that makes a default CIContext dies
 * there: SpringBoard with Bold Text (CUIShapeEffectStack sharedCIContext),
 * Messages (app icon scaling, _MI_sharedIconCompositorContext).
 *
 * This ElleKit tweak adds kCIContextUseSoftwareRenderer = YES
 * ("software_renderer") to the options when there is no Metal device, so
 * CoreImage uses its CPU renderer instead. The Metal check happens once, on
 * the first CIContext. /var/mobile/Library/S9CISoft/disabled turns it off.
 *
 * Apps load CoreImage late (Messages: through IconServices), after
 * TweakInject has matched filters, so the filter is UIKit and the hook goes
 * in when CoreImage's classes appear: dyld's add-image callbacks run after
 * the ObjC runtime has registered an image's classes and released its lock
 * (objc_addLoadImageFunc would call back with that lock held, and
 * method_setImplementation takes it). What happened goes to log.txt in that
 * directory where the sandbox allows writing (SpringBoard yes, apps no).
 *
 * Built without ARC: the replacement stands in for an init method (consumes
 * self, returns +1), which a C function under ARC would get wrong.
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

static id (*orig_initWithOptions)(id, SEL, NSDictionary *);
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
		slog(none ? "no Metal device: CIContexts use the software renderer" : "Metal device present: CIContexts untouched");
	});
	return none;
}

static id s9_initWithOptions(id self, SEL _cmd, NSDictionary *options)
{
	if (no_metal() && options[@"software_renderer"] == nil) {
		NSMutableDictionary *o = options != nil ? [[options mutableCopy] autorelease] :
		    [NSMutableDictionary dictionary];
		o[@"software_renderer"] = @YES;
		options = o;
	}
	return orig_initWithOptions(self, _cmd, options);
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
	Method m = class_getInstanceMethod(c, @selector(initWithOptions:));
	if (m == NULL || atomic_exchange(&hooked, true))
		return;
	orig_initWithOptions = (id (*)(id, SEL, NSDictionary *))
	    method_setImplementation(m, (IMP)s9_initWithOptions);
	slog("hooked -[CIContext initWithOptions:]");
}

__attribute__((constructor)) static void init(void)
{
	if (access(DIR "disabled", F_OK) == 0)
		return;
	_dyld_register_func_for_add_image(image_added);
}
