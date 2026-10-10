/*
 * wgl_readback.c -- asynchronous framebuffer readback (screenshots).
 *
 * WebGPU can't read GPU memory synchronously, so glReadPixels can't return
 * the frame being drawn. The engine calls WGL_ReadPixelsAsync instead: the
 * rectangle is copied into a mappable buffer as part of the current frame,
 * and once the buffer is mapped (usually a frame or two later) the engine's
 * callback runs at the start of WGL_Present. glReadPixels calls made from
 * inside that callback return the copied pixels with GL's conventions
 * (bottom row first, GL_PACK_ALIGNMENT row padding), so the renderers'
 * screenshot writers run unchanged.
 */
#include <stdlib.h>
#include "wgl_local.h"

#define MAX_READBACKS   4

typedef enum {
	RB_FREE,
	RB_COPIED,      // copy recorded, waiting for the submit
	RB_MAPPING,
	RB_READY,
	RB_FAILED
} readbackState_t;

typedef struct {
	readbackState_t state;
	unsigned serial;                // tells map callbacks of released buffers apart
	WGPUBuffer buffer;
	uint64_t size;
	int x, y, width, height;        // GL window rectangle (bottom-left origin)
	int bytesPerRow;
	void ( *done )( void *arg, int ok );
	void *arg;
} readback_t;

static readback_t readbacks[MAX_READBACKS];
static unsigned nextSerial = 1;
static const readback_t *serving;   // the readback glReadPixels returns, inside done()
static const uint8_t *servingData;

static void MapDone( WGPUMapAsyncStatus status, WGPUStringView message, void *userdata1, void *userdata2 ) {
	readback_t *r = userdata1;

	if ( r->state == RB_MAPPING && r->serial == (unsigned)(uintptr_t)userdata2 ) {
		r->state = status == WGPUMapAsyncStatus_Success ? RB_READY : RB_FAILED;
	}
}

// Copies a rectangle of the scene target (GL window coordinates) to the CPU.
// done(arg, ok) runs at the start of a later WGL_Present. Returns 0 if the
// copy couldn't be started (done is not called).
int WGL_ReadPixelsAsync( int x, int y, int width, int height, void ( *done )( void *arg, int ok ), void *arg ) {
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	WGPUExtent3D size;
	readback_t *r = NULL;
	int i;

	if ( !wgl.device || !wgl.colorTex ) {
		return 0;
	}
	if ( x < 0 ) { width += x; x = 0; }
	if ( y < 0 ) { height += y; y = 0; }
	if ( x + width > wgl.width ) width = wgl.width - x;
	if ( y + height > wgl.height ) height = wgl.height - y;
	if ( width <= 0 || height <= 0 ) {
		return 0;
	}
	for ( i = 0; i < MAX_READBACKS; i++ ) {
		if ( readbacks[i].state == RB_FREE ) {
			r = &readbacks[i];
			break;
		}
	}
	if ( !r ) {
		WGL_Printf( "wgl: too many readbacks in flight\n" );
		return 0;
	}

	r->serial = nextSerial++;
	r->x = x;
	r->y = y;
	r->width = width;
	r->height = height;
	r->bytesPerRow = ( width * 4 + 255 ) & ~255;
	r->size = (uint64_t)r->bytesPerRow * height;
	r->done = done;
	r->arg = arg;

	bd.label.data = "wgl readback";
	bd.label.length = WGPU_STRLEN;
	bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
	bd.size = r->size;
	r->buffer = wgpuDeviceCreateBuffer( wgl.device, &bd );

	WGL_ApplyPendingClears();
	WGL_EndPass();
	src.texture = wgl.colorTex;
	src.origin.x = x;
	src.origin.y = wgl.height - ( y + height );
	src.aspect = WGPUTextureAspect_All;
	dst.buffer = r->buffer;
	dst.layout.offset = 0;
	dst.layout.bytesPerRow = r->bytesPerRow;
	dst.layout.rowsPerImage = height;
	size.width = width;
	size.height = height;
	size.depthOrArrayLayers = 1;
	wgpuCommandEncoderCopyTextureToBuffer( WGL_Encoder(), &src, &dst, &size );

	r->state = RB_COPIED;
	return 1;
}

// After a submit: start mapping the buffers whose copies it contained.
void WGL_ReadbackSubmitted( void ) {
	WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
	int i;

	for ( i = 0; i < MAX_READBACKS; i++ ) {
		readback_t *r = &readbacks[i];
		if ( r->state != RB_COPIED ) {
			continue;
		}
		r->state = RB_MAPPING;
		cb.mode = WGPUCallbackMode_AllowSpontaneous;
		cb.callback = MapDone;
		cb.userdata1 = r;
		cb.userdata2 = (void *)(uintptr_t)r->serial;
		wgpuBufferMapAsync( r->buffer, WGPUMapMode_Read, 0, (size_t)r->size, cb );
	}
}

static void Release( readback_t *r ) {
	if ( r->buffer ) {
		wgpuBufferRelease( r->buffer );
	}
	memset( r, 0, sizeof( *r ) );
}

// Start of WGL_Present: hand finished readbacks to their callbacks.
void WGL_ReadbackFrame( void ) {
	int i;

	for ( i = 0; i < MAX_READBACKS; i++ ) {
		readback_t *r = &readbacks[i];

		if ( r->state == RB_READY ) {
			servingData = wgpuBufferGetConstMappedRange( r->buffer, 0, (size_t)r->size );
			serving = r;
			r->done( r->arg, servingData != NULL );
			serving = NULL;
			servingData = NULL;
			wgpuBufferUnmap( r->buffer );
			Release( r );
		} else if ( r->state == RB_FAILED ) {
			WGL_Printf( "wgl: framebuffer readback failed\n" );
			r->done( r->arg, 0 );
			Release( r );
		}
	}
}

// Device shutdown (vid_restart): readbacks in flight are lost.
void WGL_ShutdownReadback( void ) {
	int i;

	for ( i = 0; i < MAX_READBACKS; i++ ) {
		readback_t *r = &readbacks[i];
		if ( r->state != RB_FREE ) {
			r->done( r->arg, 0 );
			Release( r );
		}
	}
}

// Outside a readback callback there is nothing to return: zeroes.
void wgReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels ) {
	static int warned;
	int bpp = ( format == GL_RGBA ) ? 4 : ( format == GL_RGB ) ? 3 : ( format == GL_DEPTH_COMPONENT && type == GL_FLOAT ) ? 4 : 1;
	int align = gls.packAlignment > 0 ? gls.packAlignment : 1;
	int stride = ( width * bpp + align - 1 ) / align * align;
	int i, j;

	if ( width <= 0 || height <= 0 ) {
		return;
	}
	memset( pixels, 0, (size_t)stride * height );

	if ( !serving || !servingData || ( format != GL_RGB && format != GL_RGBA ) || type != GL_UNSIGNED_BYTE ) {
		if ( !warned ) {
			WGL_Printf( "wgl: glReadPixels only works inside WGL_ReadPixelsAsync callbacks\n" );
			warned = 1;
		}
		return;
	}

	for ( j = 0; j < height; j++ ) {
		int gy = y + j;     // GL rows count up from the bottom
		const uint8_t *row;
		uint8_t *out = (uint8_t *)pixels + (size_t)j * stride;

		if ( gy < serving->y || gy >= serving->y + serving->height ) {
			continue;
		}
		// the buffer holds the rectangle top row first
		row = servingData + (size_t)( serving->y + serving->height - 1 - gy ) * serving->bytesPerRow;
		for ( i = 0; i < width; i++ ) {
			int gx = x + i;
			if ( gx < serving->x || gx >= serving->x + serving->width ) {
				continue;
			}
			memcpy( out + i * bpp, row + ( gx - serving->x ) * 4, bpp );
		}
	}
}
