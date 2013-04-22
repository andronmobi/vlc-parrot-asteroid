/*****************************************************************************
 * SurfaceHelper.cpp: SurfaceHelper provides access to ISurface using friendly
 * android::Test class and allows to create an overlay.
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

#include "SurfaceHelper.h"
#include <surfaceflinger/ISurface.h>

using namespace android;

sp<Overlay> Test::mOverlay;

extern "C" void* getISurface(Surface *surface) {
    return (void *) Test::getISurface(surface);
}

extern "C" void* createOverlay(Surface *surface, uint32_t w, uint32_t h, int32_t format, int32_t orientation) {
    sp<ISurface> isurface = Test::getISurface(surface);
    sp<OverlayRef> overlayRef;
#ifdef OMAP_ENHANCEMENT
    bool isS3d = false;
# ifdef PARROT_MEDIA_OUT
    overlayRef = isurface->createOverlay(w, h, format, orientation, isS3d, 0);
# else
# error NOT PARROT_MEDIA_OUT
    overlayRef = isurface->createOverlay(w, h, format, orientation, isS3d);
# endif
#else
#error NOT OMAP_ENHANCEMENT
    overlayRef = isurface->createOverlay(w, h, format, orientation);
#endif
    Test::mOverlay = new Overlay(overlayRef); // keep the strong pointer to Overlay
    return Test::mOverlay.get();
}

extern "C" void setDisplay(Surface *surface, int displayId) {
    sp<ISurface> isurface = Test::getISurface(surface);
    isurface->setDisplayId(displayId);
}
