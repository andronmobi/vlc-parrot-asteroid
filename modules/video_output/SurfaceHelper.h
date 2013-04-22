/*****************************************************************************
 * SurfaceHelper.h: SurfaceHelper provides access to ISurface using friendly
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

#ifndef SURFACEHELPER_H_
#define SURFACEHELPER_H_

#include <surfaceflinger/Surface.h>
#include <ui/Overlay.h>

namespace android {

class Test {

public:
    static ISurface* getISurface(SurfaceControl *surfaceControl) {
        sp<ISurface> isurface = surfaceControl->getISurface();
        return isurface.get();
    }

    static ISurface* getISurface(Surface *surface) {
        sp<ISurface> isurface = surface->getISurface();
        return isurface.get();
    }

    static sp<Overlay> mOverlay;
};

} /* namespace android */

extern "C" void* getISurface(android::Surface *surface);
extern "C" void* createOverlay(android::Surface *surface, uint32_t w, uint32_t h, int32_t format, int32_t orientation);
extern "C" void setDisplay(android::Surface *surface, int displayId);

#endif /* SURFACEHELPER_H_ */
