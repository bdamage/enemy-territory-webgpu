/*
 * wgl_engine.h -- what the WebGPU GL emulation offers the renderers beyond
 * GL 1.x. Included by qgl_wgpu.h, so every renderer file sees it.
 */
#ifndef WGL_ENGINE_H
#define WGL_ENGINE_H

// Asynchronous glReadPixels (wgl_readback.c): copies a rectangle of the
// current frame (GL window coordinates). done(arg, ok) runs at the end of a
// later frame, and glReadPixels calls made inside it return the copy.
// Returns 0, without calling done, if the copy couldn't be started.
int WGL_ReadPixelsAsync( int x, int y, int width, int height, void ( *done )( void *arg, int ok ), void *arg );

// Rendering options the GLimp layers set from cvars. msaa and renderScale
// take effect at WGL_DeviceInit (vid_restart); the rest every frame.
typedef struct {
	int msaa;               // 1 or 4 samples
	float renderScale;      // render size relative to the canvas (0.25 - 2)
	float bloom;            // bloom strength, 0 = off
	float sharpen;          // contrast adaptive sharpening, 0 = off .. 1
	float vibrance;         // saturation boost for dull colors, 0 = off
} wglOptions_t;

extern wglOptions_t wglOptions;

// Called by the renderer backends after drawing a view of the world (not
// portals, sky portals or UI models), before 2D drawing: post-processes that
// viewport (GL window coordinates).
void WGL_EndScene( int x, int y, int width, int height );

#endif
