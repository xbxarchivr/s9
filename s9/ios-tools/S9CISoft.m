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
 * This ElleKit tweak (filter: com.apple.CoreImage) adds
 * kCIContextUseSoftwareRenderer = YES ("software_renderer") to the options
 * when there is no Metal device, so CoreImage uses its CPU renderer instead.
 * Nothing runs at load except the hook; the Metal check happens once, on the
 * first CIContext. /var/mobile/Library/S9CISoft/disabled turns it off.
 *
 * Built without ARC: the replacement stands in for an init method (consumes
 * self, returns +1), which a C function under ARC would get wrong.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <unistd.h>

#if __has_feature(objc_arc)
#error "build with -fno-objc-arc"
#endif

static id (*orig_initWithOptions)(id, SEL, NSDictionary *);

static BOOL no_metal(void)
{
	static BOOL none;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		id (*create)(void) = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice");
		id dev = create != NULL ? create() : nil;
		none = dev == nil;
		[dev release];
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

__attribute__((constructor)) static void init(void)
{
	if (access("/var/mobile/Library/S9CISoft/disabled", F_OK) == 0)
		return;
	Method m = class_getInstanceMethod(objc_getClass("CIContext"), @selector(initWithOptions:));
	if (m != NULL)
		orig_initWithOptions = (id (*)(id, SEL, NSDictionary *))
		    method_setImplementation(m, (IMP)s9_initWithOptions);
}
