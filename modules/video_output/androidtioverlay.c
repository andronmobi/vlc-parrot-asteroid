/*****************************************************************************
 * androidtioverlay.c: android video output using TI Overlay
 *****************************************************************************
 * Copyright © 2013 Andrei Mandychev <andron.mobi@gmail.com>
 *
 * Authors: Andrei Mandychev <andron.mobi@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_picture_pool.h>

#include <dlfcn.h>


#ifndef ANDROID_SYM_S_CREATE_OVR
# define ANDROID_SYM_S_CREATE_OVR "_ZN7android7Surface7isValidEv"
#endif

#define ANDROID_SYM_OVR_DESTROY "_ZN7android7Overlay7destroyEv"
#define ANDROID_SYM_OVR_DEQUEUE_BUF "_ZN7android7Overlay13dequeueBufferEPPv"
#define ANDROID_SYM_OVR_QUEUE_BUF "_ZN7android7Overlay11queueBufferEPv"
#define ANDROID_SYM_OVR_GET_BUF_ADDR "_ZN7android7Overlay16getBufferAddressEPv"
#define ANDROID_SYM_OVR_GET_BUF_COUNT "_ZNK7android7Overlay14getBufferCountEv"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define CHROMA_TEXT N_("Chroma used")
#define CHROMA_LONGTEXT N_(\
    "Force use of a specific chroma for output. Default is UYVY.")

#define CFG_PREFIX "androidtioverlay-"

static int  Open (vlc_object_t *);
static void Close(vlc_object_t *);

vlc_module_begin()
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_shortname("AndroidTIOverlay")
    set_description(N_("Android TI Overlay video output"))
    set_capability("vout display", 155)
    add_shortcut("androidtioverlay", "android")
    add_string(CFG_PREFIX "chroma", NULL, CHROMA_TEXT, CHROMA_LONGTEXT, true)
    set_callbacks(Open, Close)
vlc_module_end()

/*****************************************************************************
 * JNI prototypes
 *****************************************************************************/

extern void *jni_LockAndGetAndroidSurface();
extern void  jni_UnlockAndroidSurface();
extern void  jni_SetAndroidSurfaceSize(int width, int height, int sar_num, int sar_den);

typedef void ISurface;
typedef void Overlay;
typedef int32_t status_t;

typedef struct {
    int fd;
    unsigned int length;
    uint32_t offset;
    void *ptr;
} mapping_data_t;

typedef ISurface* (*getISurface)(void *);
typedef Overlay* (*createOverlay)(void *, uint32_t, uint32_t, int32_t, int32_t);
typedef void (*releaseOverlay)();
typedef void (*setDisplay)(void *, int);

// _ZN7android7Overlay7destroyEv
typedef void (*Overlay_destroy)(void *);
// ZN7android7Overlay13dequeueBufferEPPv
typedef status_t (*Overlay_dequeueBuffer)(void *, int *);
// ZN7android7Overlay11queueBufferEPv
typedef status_t (*Overlay_queueBuffer)(void *, int);
// ZN7android7Overlay16getBufferAddressEPv
typedef void* (*Overlay_getBufferAddress)(void *, int);
// ZNK7android7Overlay14getBufferCountEv
typedef int32_t (*Overlay_getBufferCount)(void *);

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

static picture_pool_t   *Pool  (vout_display_t *, unsigned);
static void             Display(vout_display_t *, picture_t *, subpicture_t *);
static int              Control(vout_display_t *, int, va_list);

static int  LockPicture(picture_t *);
static void UnlockPicture(picture_t *);

/* */
struct vout_display_sys_t {
    picture_pool_t *pool;

    // libsurfacehelper.so library and its methods.
    void *p_libsurfacehelper;
    getISurface getISurface;
    createOverlay createOverlay;
    releaseOverlay releaseOverlay;
    setDisplay setDisplay;

    // libui.so library and its overlay methods.
    void *p_libui;
    Overlay_destroy o_destroy;
    Overlay_dequeueBuffer o_dequeueBuffer;
    Overlay_queueBuffer o_queueBuffer;
    Overlay_getBufferAddress o_getBufferAddress;
    Overlay_getBufferCount o_getBufferCount;

    // Android framework/base/include/ui/Overlay.h object
    Overlay* overlay;

    // Overlay's buffer information
    int buffer_count;
    int buffer_queued_count;
    void** buffer_ptrs;
    int buffer_idx;

    /* density */
    int i_sar_num;
    int i_sar_den;
};

struct picture_sys_t {
    vout_display_sys_t *sys;
};

static vlc_mutex_t single_instance = VLC_STATIC_MUTEX;

static inline void *LoadSurfaceHelper(const char *psz_lib, vout_display_sys_t *sys) {
    void *p_library = dlopen(psz_lib, RTLD_NOW);
    if (!p_library)
        return NULL;

    sys->getISurface = (getISurface)(dlsym(p_library, "getISurface"));
    sys->createOverlay = (createOverlay)(dlsym(p_library, "createOverlay"));
    sys->releaseOverlay = (releaseOverlay)(dlsym(p_library, "releaseOverlay"));
    sys->setDisplay = (setDisplay)(dlsym(p_library, "setDisplay"));

    if (sys->getISurface && sys->createOverlay && sys->setDisplay)
        return p_library;

    dlclose(p_library);
    return NULL;
}

static inline void *LoadOverlay(const char *psz_lib, vout_display_sys_t *sys)
{
    void *p_library = dlopen(psz_lib, RTLD_NOW);
    if (!p_library)
        return NULL;

    sys->o_destroy = (Overlay_destroy)(dlsym(p_library, ANDROID_SYM_OVR_DESTROY));
    sys->o_dequeueBuffer = (Overlay_dequeueBuffer)(dlsym(p_library, ANDROID_SYM_OVR_DEQUEUE_BUF));
    sys->o_queueBuffer = (Overlay_queueBuffer)(dlsym(p_library, ANDROID_SYM_OVR_QUEUE_BUF));
    sys->o_getBufferAddress = (Overlay_getBufferAddress)(dlsym(p_library, ANDROID_SYM_OVR_GET_BUF_ADDR));
    sys->o_getBufferCount = (Overlay_getBufferCount)(dlsym(p_library, ANDROID_SYM_OVR_GET_BUF_COUNT));

    if (sys->o_destroy && sys->o_dequeueBuffer && sys->o_queueBuffer &&
            sys->o_getBufferAddress && sys->o_getBufferCount)
        return p_library;

    dlclose(p_library);
    return NULL;
}

static int InitLibraries(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;
    sys->p_libsurfacehelper = LoadSurfaceHelper("/data/data/org.videolan.vlc/lib/libsurfacehelper.so", sys);
    if (!sys->p_libsurfacehelper) {
        msg_Err(vd, "libsurfacehelper.so library is not loaded");
        return VLC_EGENERIC;
    }

    sys->p_libui = LoadOverlay("libui.so", sys);
    if (!sys->p_libui) {
        msg_Err(vd, "libui.so library is not loaded");
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static int OpenOverlay(vout_display_t *vd, video_format_t *fmt) {

    vout_display_sys_t *sys = vd->sys;

    void* surf = jni_LockAndGetAndroidSurface();
    if (unlikely(!surf)) {
        msg_Err(vd, "surf is bad"); // TODO
        jni_UnlockAndroidSurface();
        return VLC_EGENERIC;
    }
    msg_Dbg(vd, "OpenOverlay creating overlay... w=%d, h=%d", fmt->i_width, fmt->i_height);
    sys->overlay = sys->createOverlay(surf, fmt->i_width, fmt->i_height, 0x16, 0); // TODO 0x16
    if (!sys->overlay) {
        msg_Err(vd, "Can't create overlay");
        jni_UnlockAndroidSurface();
        return VLC_EGENERIC;
    }
    sys->buffer_count = sys->o_getBufferCount(sys->overlay);
    if (sys->buffer_count <= 0) {
        msg_Err(vd, "There is no any overlay buffer");
        jni_UnlockAndroidSurface();
        return VLC_EGENERIC;
    }
    sys->buffer_idx = 0;
    sys->buffer_queued_count = 0;
    sys->buffer_ptrs = calloc(sys->buffer_count, sizeof(void*));
    for (int i = 0; i < sys->buffer_count; i++) {
        mapping_data_t* data = sys->o_getBufferAddress(sys->overlay, i);
        sys->buffer_ptrs[i] = data->ptr;
    }

    //////////////////////////////////////////////////////////////
    sys->setDisplay(surf, 2);
    msg_Info(vd, "Set display is done");
    //////////////////////////////////////////////////////////////

    msg_Dbg(vd, "Open surface is OK");
    jni_UnlockAndroidSurface();

    return VLC_SUCCESS;
}

static int Open(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t *)p_this;

    /* */
    if (vlc_mutex_trylock(&single_instance) != 0) {
        msg_Err(vd, "Can't start more than one instance at a time");
        return VLC_EGENERIC;
    }

    /* Allocate structure */
    vout_display_sys_t *sys = (struct vout_display_sys_t*) calloc(1, sizeof(*sys));
    if (!sys) {
        vlc_mutex_unlock(&single_instance);
        return VLC_ENOMEM;
    }

    /* */
    if (InitLibraries(vd) != VLC_SUCCESS) {
        msg_Err(vd, "Could not initialize libui.so/libsurfacehelper.so!");
        if (sys->p_libsurfacehelper) dlclose(sys->p_libsurfacehelper);
        if (sys->p_libui) dlclose(sys->p_libui);
        free(sys);
        vlc_mutex_unlock(&single_instance);
        return VLC_EGENERIC;
    }

    /* Setup chroma */
    video_format_t fmt = vd->fmt;
    fmt.i_chroma = VLC_CODEC_UYVY;
    msg_Dbg(vd, "Pixel format %4.4s", (char*)&fmt.i_chroma);

    /* Setup vout_display */
    vd->sys     = sys;
    vd->fmt     = fmt;
    vd->pool    = Pool;
    vd->display = Display;
    vd->control = Control;
    vd->prepare = NULL;
    vd->manage  = NULL;

    /* Fix initial state */
    vout_display_SendEventFullscreen(vd, false);

    sys->i_sar_num = vd->source.i_sar_num;
    sys->i_sar_den = vd->source.i_sar_den;

    if (OpenOverlay(vd, &fmt) != VLC_SUCCESS) {
        msg_Err(vd, "Could not open overlay");
        dlclose(sys->p_libsurfacehelper);
        dlclose(sys->p_libui);
        free(sys);
        vlc_mutex_unlock(&single_instance);
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t *)p_this;
    vout_display_sys_t *sys = vd->sys;

    sys->o_destroy(sys->overlay);
    sys->releaseOverlay();
    free(sys->buffer_ptrs);
    picture_pool_Delete(sys->pool); // free memory of the pool and its picture(s)

    dlclose(sys->p_libsurfacehelper);
    dlclose(sys->p_libui);
    free(sys);
    vlc_mutex_unlock(&single_instance);
}

static picture_pool_t *Pool(vout_display_t *vd, unsigned count)
{
    VLC_UNUSED(count);
    vout_display_sys_t *sys = vd->sys;

    if (!sys->pool) {

        // Create a picture.
        picture_resource_t rsc;
        memset(&rsc, 0, sizeof(rsc));
        rsc.p_sys = malloc(sizeof(*rsc.p_sys));
        rsc.p_sys->sys = sys;
        rsc.p[0].p_pixels = NULL;  // picture buffer will be set later
        rsc.p[0].i_lines  = 0;
        rsc.p[0].i_pitch = 0;
        picture_t *picture = picture_NewFromResource(&vd->fmt, &rsc); // allocate memory for the picture
        if (!picture) {
            msg_Err(vd, "Can't create a picture");
            return NULL;
        }

        // Create a pool with one picture.
        picture_pool_configuration_t pool_cfg;
        memset(&pool_cfg, 0, sizeof(pool_cfg));
        pool_cfg.picture_count = 1;
        pool_cfg.picture = &picture;
        pool_cfg.lock = LockPicture;
        pool_cfg.unlock = UnlockPicture;
        sys->pool = picture_pool_NewExtended(&pool_cfg);
        if (!sys->pool) {
            picture_Release(picture); // free memory of the picture
            msg_Err(vd, "Can't create a pool");
            return NULL;
        }
    }

    return sys->pool;
}

static int LockPicture(picture_t *picture) {

    vout_display_sys_t *sys = picture->p_sys->sys;

    picture->p[0].p_pixels = sys->buffer_ptrs[sys->buffer_idx]; // Set a picture buffer
    picture->p[0].i_lines = picture->p[0].i_visible_lines;
    picture->p[0].i_pitch = picture->p[0].i_visible_pitch;

    return VLC_SUCCESS;
}

static void Display(vout_display_t *vd, picture_t *picture, subpicture_t *subpicture)
{
    VLC_UNUSED(vd);
    VLC_UNUSED(subpicture);

    vout_display_sys_t *sys = vd->sys;

    if (sys->buffer_count > 0 && sys->buffer_queued_count < sys->buffer_count) {
        vd->sys->o_queueBuffer(sys->overlay, sys->buffer_idx);
        //msg_Dbg(vd, "queue buffer[%d]", sys->buffer_idx);
        sys->buffer_idx++;
        sys->buffer_queued_count++;
        if (sys->buffer_idx == sys->buffer_count)
            sys->buffer_idx = 0;
    }

    if (sys->buffer_queued_count) {
        int i;
        status_t status = vd->sys->o_dequeueBuffer(sys->overlay, &i);
        if (status == 0) {
            sys->buffer_queued_count--;
            //msg_Dbg(vd, "dequeue buffer[%d]", i);
        }
    }

    /* we don't free memory of the picture in this method
     * because picture->gc.pf_destroy was overloaded in picture_pool_NewExtended.
     * Unlock method of this picture (i.e. UnlockPicture) will be called.
     * */
    picture_Release(picture);
}

static void UnlockPicture(picture_t *picture) {
    picture->p[0].p_pixels = NULL;
}

static int Control(vout_display_t *vd, int query, va_list args)
{
    VLC_UNUSED(args);

    msg_Dbg(vd, "Control androidtioverlay, query: %d", query);

    switch (query) {
    case VOUT_DISPLAY_HIDE_MOUSE:
        return VLC_SUCCESS;

    default:
        msg_Err(vd, "Unknown request in android vout display");

    case VOUT_DISPLAY_CHANGE_FULLSCREEN:
    case VOUT_DISPLAY_CHANGE_WINDOW_STATE:
    case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
    case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
    case VOUT_DISPLAY_CHANGE_ZOOM:
    case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
    case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
    case VOUT_DISPLAY_GET_OPENGL:
        return VLC_EGENERIC;
    }
}
