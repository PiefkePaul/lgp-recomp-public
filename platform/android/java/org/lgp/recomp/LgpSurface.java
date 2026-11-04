// SDL's surface view, told to the native side before the surface goes
// (P7.3): WebGPU stops presenting to the activity's window before SDL
// releases it (SdlWindow::surface_lost, platform/desktop/src/sdl_window.cpp).
package org.lgp.recomp;

import android.content.Context;
import android.view.SurfaceHolder;

import org.libsdl.app.SDLSurface;

public class LgpSurface extends SDLSurface {
    private static native void nativeSurfaceDestroying();
    private static native void nativeSurfaceReady();

    public LgpSurface(Context context) {
        super(context);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        try {
            nativeSurfaceDestroying();
        } catch (UnsatisfiedLinkError e) {
            // A program without an SDL window: nothing presents.
        }
        super.surfaceDestroyed(holder);
    }

    // SDL stores the new ANativeWindow in surfaceCreated; only then may the
    // native side build a WebGPU surface on it again.
    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        super.surfaceChanged(holder, format, width, height);
        try {
            nativeSurfaceReady();
        } catch (UnsatisfiedLinkError e) {
            // A program without an SDL window.
        }
    }
}
