#!/usr/bin/env python3
"""Make a Mesa 25.0.7 tree build Panfrost for iOS on the Galaxy S9.

  patch-mesa.py MESA_TREE

Each change must find its text exactly once, so another Mesa version fails here
rather than building something else. What is changed, and the OSMesa ES profile,
follow HakiOS's tools/mesa (MIT, https://github.com/oosexgang/hackios-xnu-kernel).
"""
import os
import shutil
import sys

tree = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))


def change(path, old, new):
    full = os.path.join(tree, path)
    with open(full, encoding='utf-8', newline='') as f:
        text = f.read()
    if text.count(old) != 1:
        sys.exit(f'{path}: found {text.count(old)} times: {old!r}')
    with open(full, 'w', encoding='utf-8', newline='') as f:
        f.write(text.replace(old, new))


shutil.copytree(os.path.join(here, 'sweet-drm'), os.path.join(tree, 'src/sweet-drm'), dirs_exist_ok=True)

# no DRM here: the stand-in libdrm
change('meson.build', """if dep_libdrm.found()
  pre_args += '-DHAVE_LIBDRM'
""", """if host_machine.system() == 'darwin' and with_gallium_panfrost
  dep_libdrm = declare_dependency(
    include_directories : include_directories('src/sweet-drm'),
    link_args : ['-framework', 'IOKit', '-framework', 'CoreFoundation'],
  )
elif dep_libdrm.found()
  pre_args += '-DHAVE_LIBDRM'
""")
change('src/panfrost/lib/kmod/meson.build', """  'panthor_kmod.c',
)
""", """  'panthor_kmod.c',
  '../../../sweet-drm/sweet.drm.c',
)
""")

# no mmap() on a device: a buffer is a part of the block already mapped
change('src/panfrost/lib/kmod/pan_kmod.h', """   host_addr = os_mmap(host_addr, size, prot, flags, bo->dev->fd,
                       mmap_offset + bo_offset);
""", """   host_addr = sweet_drm_map(mmap_offset + bo_offset);
""")
change('src/panfrost/lib/kmod/pan_kmod.h', """#include "util/os_mman.h"
""", """#include "util/os_mman.h"
#include <xf86drm.h>
""")
change('src/gallium/drivers/panfrost/pan_bo.c', """   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {
      mesa_loge("munmap failed: %s", strerror(errno));
      abort();
   }

""", "")

# all of a tiler heap is mapped here, nothing grows on a fault, and there are 30 MB
change('src/gallium/drivers/panfrost/pan_device.c',
       'dev, 128 * 1024 * 1024, PAN_BO_INVISIBLE | PAN_BO_GROWABLE, "Tiler heap");',
       'dev, 4 * 1024 * 1024, PAN_BO_INVISIBLE | PAN_BO_GROWABLE, "Tiler heap");')

# OSMesa on the GPU where the kernel has one, and an OpenGL ES profile
change('include/GL/osmesa.h', """#define OSMESA_CONTEXT_MINOR_VERSION 0x37
""", """#define OSMESA_CONTEXT_MINOR_VERSION 0x37
#define OSMESA_ES_PROFILE            0x38  /* OpenGL ES 2 or 3, by the version asked for */
""")
change('src/gallium/frontends/osmesa/osmesa.c', """             profile != OSMESA_COMPAT_PROFILE)
""", """             profile != OSMESA_COMPAT_PROFILE &&
             profile != OSMESA_ES_PROFILE)
""")
change('src/gallium/frontends/osmesa/osmesa.c', """   attribs.profile = (profile == OSMESA_CORE_PROFILE)
      ? API_OPENGL_CORE : API_OPENGL_COMPAT;
""", """   attribs.profile = (profile == OSMESA_ES_PROFILE) ? API_OPENGLES2 :
      (profile == OSMESA_CORE_PROFILE) ? API_OPENGL_CORE : API_OPENGL_COMPAT;
""")
change('src/gallium/targets/osmesa/meson.build', """    dep_ws2_32, dep_thread, dep_clock, dep_unwind, driver_swrast, idep_mesautil,
""", """    dep_ws2_32, dep_thread, dep_clock, dep_unwind, driver_swrast, idep_mesautil,
    driver_panfrost, idep_xmlconfig, dep_libdrm,
""")
change('src/gallium/targets/osmesa/osmesa_target.c', """#include "sw/null/null_sw_winsys.h"

""", """#include "sw/null/null_sw_winsys.h"

#ifdef GALLIUM_PANFROST
#include "panfrost/drm/panfrost_drm_public.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include <xf86drm.h>

static const driOptionDescription panfrost_options[] = {
#include "panfrost/driinfo_panfrost.h"
};

/* NULL: no SweetMali service, or OSMESA_SOFTWARE=1 */
static struct pipe_screen *
osmesa_create_gpu_screen(void)
{
   static driOptionCache info, cache;
   static struct pipe_screen_config config;
   struct pipe_screen *screen;
   int fd;

   if (debug_get_bool_option("OSMESA_SOFTWARE", false))
      return NULL;
   fd = drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER);
   if (fd < 0)
      return NULL;
   driParseOptionInfo(&info, panfrost_options, ARRAY_SIZE(panfrost_options));
   driParseConfigFiles(&cache, &info, 0, "panfrost", NULL, NULL, NULL, 0, NULL, 0);
   config.options = &cache;
   config.options_info = &info;
   screen = panfrost_drm_screen_create(fd, &config);
   return screen ? debug_screen_wrap(screen) : NULL;
}
#endif

""")
change('src/gallium/targets/osmesa/osmesa_target.c', """   struct pipe_screen *screen;

   /* We use a null software winsys""", """   struct pipe_screen *screen;

#ifdef GALLIUM_PANFROST
   screen = osmesa_create_gpu_screen();
   if (screen)
      return screen;
#endif

   /* We use a null software winsys""")
