/*
 * wgl_texture.c -- GL texture objects backed by WebGPU textures.
 *
 * Every texture is RGBA8. A full mip chain is allocated on the level 0
 * upload; the view only exposes the contiguous levels uploaded so far, so
 * textures without mips sample their base level. Each texture owns the bind
 * group (view + sampler) used for both texture slots of the draw pipeline.
 */
#include <stdlib.h>
#include "wgl_local.h"

static wglTexture_t **textures;
static int numTextures;             // size of the textures[] table
static GLuint nextName = 1;

static WGPUBindGroupLayout texLayout;
static wglTexture_t whiteTexture;

typedef struct {
	GLenum minFilter, magFilter, wrapS, wrapT;
	int aniso;
	WGPUSampler sampler;
} wglSampler_t;

#define MAX_SAMPLERS 64
static wglSampler_t samplers[MAX_SAMPLERS];
static int numSamplers;

static uint8_t *convertBuffer;
static int convertSize;

// ---------------------------------------------------------------------------

WGPUBindGroupLayout WGL_TextureBindGroupLayout( void ) {
	return texLayout;
}

wglTexture_t *WGL_GetTexture( GLuint name ) {
	if ( name == 0 || name >= (GLuint)numTextures ) {
		return NULL;
	}
	return textures[name];
}

static wglTexture_t *CreateTextureObject( GLuint name ) {
	wglTexture_t *t;

	if ( name >= (GLuint)numTextures ) {
		int newSize = numTextures ? numTextures : 1024;
		while ( newSize <= (int)name ) {
			newSize *= 2;
		}
		textures = realloc( textures, newSize * sizeof( *textures ) );
		memset( textures + numTextures, 0, ( newSize - numTextures ) * sizeof( *textures ) );
		numTextures = newSize;
	}
	if ( !textures[name] ) {
		t = calloc( 1, sizeof( *t ) );
		t->minFilter = GL_NEAREST_MIPMAP_LINEAR;
		t->magFilter = GL_LINEAR;
		t->wrapS = t->wrapT = GL_REPEAT;
		t->anisotropy = 1.0f;
		t->dirty = 1;
		textures[name] = t;
	}
	if ( name >= nextName ) {
		nextName = name + 1;
	}
	return textures[name];
}

static void ReleaseTextureGPU( wglTexture_t *t ) {
	if ( t->bindGroup ) {
		wgpuBindGroupRelease( t->bindGroup );
		t->bindGroup = NULL;
	}
	if ( t->view ) {
		wgpuTextureViewRelease( t->view );
		t->view = NULL;
	}
	if ( t->tex ) {
		wgpuTextureRelease( t->tex );
		t->tex = NULL;
	}
	t->width = t->height = t->mipCount = t->levels = 0;
	t->dirty = 1;
}

static wglTexture_t *BoundTexture( void ) {
	GLuint name = gls.bound[gls.activeUnit];
	if ( !name ) {
		return NULL;
	}
	return CreateTextureObject( name );
}

// ---------------------------------------------------------------------------
// samplers

static WGPUFilterMode FilterMode( GLenum f ) {
	switch ( f ) {
	case GL_NEAREST:
	case GL_NEAREST_MIPMAP_NEAREST:
	case GL_NEAREST_MIPMAP_LINEAR:
		return WGPUFilterMode_Nearest;
	default:
		return WGPUFilterMode_Linear;
	}
}

static WGPUAddressMode AddressMode( GLenum w ) {
	return w == GL_REPEAT ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
}

static WGPUSampler GetSampler( GLenum minF, GLenum magF, GLenum wrapS, GLenum wrapT, float anisotropy ) {
	WGPUSamplerDescriptor d = WGPU_SAMPLER_DESCRIPTOR_INIT;
	int i, aniso, mipmapped, mipLinear;

	mipmapped = ( minF != GL_NEAREST && minF != GL_LINEAR );
	mipLinear = ( minF == GL_NEAREST_MIPMAP_LINEAR || minF == GL_LINEAR_MIPMAP_LINEAR );
	aniso = (int)anisotropy;
	// WebGPU only allows anisotropy with all-linear filtering: mipmapped
	// bilinear textures (2.60b's default GL_LINEAR_MIPMAP_NEAREST) become
	// trilinear rather than losing it
	if ( aniso < 1 || FilterMode( minF ) != WGPUFilterMode_Linear || FilterMode( magF ) != WGPUFilterMode_Linear
		 || !mipmapped ) {
		aniso = 1;
	} else if ( aniso > 1 ) {
		mipLinear = 1;
	}
	if ( aniso > 16 ) {
		aniso = 16;
	}

	for ( i = 0; i < numSamplers; i++ ) {
		wglSampler_t *s = &samplers[i];
		if ( s->minFilter == minF && s->magFilter == magF && s->wrapS == wrapS && s->wrapT == wrapT && s->aniso == aniso ) {
			return s->sampler;
		}
	}

	d.addressModeU = AddressMode( wrapS );
	d.addressModeV = AddressMode( wrapT );
	d.addressModeW = WGPUAddressMode_ClampToEdge;
	d.magFilter = FilterMode( magF );
	d.minFilter = FilterMode( minF );
	d.mipmapFilter = mipLinear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
	d.lodMinClamp = 0.0f;
	d.lodMaxClamp = mipmapped ? 32.0f : 0.0f;
	d.maxAnisotropy = (uint16_t)aniso;

	if ( numSamplers == MAX_SAMPLERS ) {
		// shouldn't happen: the renderer only uses a handful of combinations
		wgpuSamplerRelease( samplers[0].sampler );
		memmove( samplers, samplers + 1, sizeof( samplers[0] ) * ( MAX_SAMPLERS - 1 ) );
		numSamplers--;
	}
	samplers[numSamplers].minFilter = minF;
	samplers[numSamplers].magFilter = magF;
	samplers[numSamplers].wrapS = wrapS;
	samplers[numSamplers].wrapT = wrapT;
	samplers[numSamplers].aniso = aniso;
	samplers[numSamplers].sampler = wgpuDeviceCreateSampler( wgl.device, &d );
	return samplers[numSamplers++].sampler;
}

// ---------------------------------------------------------------------------
// bind groups

WGPUBindGroup WGL_TextureBindGroup( wglTexture_t *t ) {
	WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	WGPUBindGroupEntry e[2] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };

	if ( !t || !t->tex || t->levels == 0 ) {
		t = &whiteTexture;
	}
	t->usedSerial = wgl.encoderSerial;
	if ( !t->dirty && t->bindGroup ) {
		return t->bindGroup;
	}

	if ( t->bindGroup ) {
		wgpuBindGroupRelease( t->bindGroup );
	}
	if ( t->view ) {
		wgpuTextureViewRelease( t->view );
	}
	vd.format = WGPUTextureFormat_RGBA8Unorm;
	vd.dimension = WGPUTextureViewDimension_2D;
	vd.baseMipLevel = 0;
	vd.mipLevelCount = t->levels;
	vd.baseArrayLayer = 0;
	vd.arrayLayerCount = 1;
	vd.aspect = WGPUTextureAspect_All;
	t->view = wgpuTextureCreateView( t->tex, &vd );

	e[0].binding = 0;
	e[0].textureView = t->view;
	e[1].binding = 1;
	e[1].sampler = GetSampler( t->minFilter, t->magFilter, t->wrapS, t->wrapT, t->anisotropy );
	bd.layout = texLayout;
	bd.entryCount = 2;
	bd.entries = e;
	t->bindGroup = wgpuDeviceCreateBindGroup( wgl.device, &bd );
	t->dirty = 0;
	return t->bindGroup;
}

wglTexture_t *WGL_WhiteTexture( void ) {
	return &whiteTexture;
}

// ---------------------------------------------------------------------------
// storage

static void AllocStorage( wglTexture_t *t, int width, int height ) {
	WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
	int mips = 1, m = width > height ? width : height;

	while ( m > 1 ) {
		m >>= 1;
		mips++;
	}

	ReleaseTextureGPU( t );
	d.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
	d.dimension = WGPUTextureDimension_2D;
	d.size.width = width;
	d.size.height = height;
	d.size.depthOrArrayLayers = 1;
	d.format = WGPUTextureFormat_RGBA8Unorm;
	d.mipLevelCount = mips;
	d.sampleCount = 1;
	t->tex = wgpuDeviceCreateTexture( wgl.device, &d );
	t->width = width;
	t->height = height;
	t->mipCount = mips;
	t->levels = 0;
	t->dirty = 1;
}

static int IsRGBFormat( GLint internalFormat ) {
	switch ( internalFormat ) {
	case 3:
	case GL_RGB:
	case GL_RGB5:
	case GL_RGB8:
	case 0x83A1:    // GL_RGB4_S3TC
	case 0x83F0:    // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
		return 1;
	default:
		return 0;
	}
}

static uint8_t *ConvertBuffer( int size ) {
	if ( size > convertSize ) {
		convertBuffer = realloc( convertBuffer, size );
		convertSize = size;
	}
	return convertBuffer;
}

// Returns tightly packed RGBA8 for the given client pixels.
static const uint8_t *ToRGBA( int width, int height, GLenum format, GLenum type, const void *pixels, int forceOpaque ) {
	const uint8_t *src = pixels;
	uint8_t *dst;
	int i, count = width * height;

	if ( type != GL_UNSIGNED_BYTE ) {
		WGL_Printf( "wgl: unsupported texture type 0x%x\n", type );
		return NULL;
	}
	if ( format == GL_RGBA && !forceOpaque ) {
		return src;
	}
	dst = ConvertBuffer( count * 4 );
	switch ( format ) {
	case GL_RGBA:
		memcpy( dst, src, count * 4 );
		break;
	case GL_RGB:
		for ( i = 0; i < count; i++ ) {
			dst[i * 4 + 0] = src[i * 3 + 0];
			dst[i * 4 + 1] = src[i * 3 + 1];
			dst[i * 4 + 2] = src[i * 3 + 2];
			dst[i * 4 + 3] = 255;
		}
		break;
	case GL_LUMINANCE:
		for ( i = 0; i < count; i++ ) {
			dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = src[i];
			dst[i * 4 + 3] = 255;
		}
		break;
	case GL_ALPHA:
		for ( i = 0; i < count; i++ ) {
			dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = 255;
			dst[i * 4 + 3] = src[i];
		}
		break;
	case GL_LUMINANCE_ALPHA:
		for ( i = 0; i < count; i++ ) {
			dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = src[i * 2];
			dst[i * 4 + 3] = src[i * 2 + 1];
		}
		break;
	default:
		WGL_Printf( "wgl: unsupported texture format 0x%x\n", format );
		return NULL;
	}
	if ( forceOpaque ) {
		for ( i = 0; i < count; i++ ) {
			dst[i * 4 + 3] = 255;
		}
	}
	return dst;
}

static void WriteLevel( wglTexture_t *t, int level, int x, int y, int width, int height, const uint8_t *rgba ) {
	WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
	WGPUExtent3D size = { (uint32_t)width, (uint32_t)height, 1 };

	// queue writes execute before the not-yet-submitted frame commands, so
	// flush first if this texture was already drawn with in this encoder
	if ( t->usedSerial == wgl.encoderSerial && wgl.encoder ) {
		WGL_Flush();
	}

	dst.texture = t->tex;
	dst.mipLevel = level;
	dst.origin.x = x;
	dst.origin.y = y;
	dst.origin.z = 0;
	dst.aspect = WGPUTextureAspect_All;
	layout.offset = 0;
	layout.bytesPerRow = width * 4;
	layout.rowsPerImage = height;
	wgpuQueueWriteTexture( wgl.queue, &dst, rgba, (size_t)width * height * 4, &layout, &size );
}

// ---------------------------------------------------------------------------
// GL entry points

void wgGenTextures( GLsizei n, GLuint *names ) {
	int i;
	for ( i = 0; i < n; i++ ) {
		while ( WGL_GetTexture( nextName ) ) {
			nextName++;
		}
		names[i] = nextName;
		CreateTextureObject( nextName );
	}
}

void wgDeleteTextures( GLsizei n, const GLuint *names ) {
	int i, u;

	for ( i = 0; i < n; i++ ) {
		wglTexture_t *t = WGL_GetTexture( names[i] );
		if ( !t ) {
			continue;
		}
		ReleaseTextureGPU( t );
		free( t );
		textures[names[i]] = NULL;
		for ( u = 0; u < WGL_UNITS; u++ ) {
			if ( gls.bound[u] == names[i] ) {
				gls.bound[u] = 0;
			}
		}
	}
}

void wgBindTexture( GLenum target, GLuint texture ) {
	gls.bound[gls.activeUnit] = texture;
	if ( texture ) {
		CreateTextureObject( texture );
	}
}

void wgTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
				   GLint border, GLenum format, GLenum type, const GLvoid *pixels ) {
	wglTexture_t *t = BoundTexture();
	const uint8_t *rgba;

	if ( !t || width <= 0 || height <= 0 ) {
		return;
	}

	if ( level == 0 ) {
		if ( !t->tex || t->width != width || t->height != height ) {
			AllocStorage( t, width, height );
		}
	} else {
		if ( !t->tex || level >= t->mipCount
			 || ( t->width >> level ? t->width >> level : 1 ) != width
			 || ( t->height >> level ? t->height >> level : 1 ) != height ) {
			return;
		}
	}

	if ( pixels ) {
		rgba = ToRGBA( width, height, format, type, pixels, IsRGBFormat( internalformat ) );
		if ( !rgba ) {
			return;
		}
		WriteLevel( t, level, 0, 0, width, height, rgba );
	}

	if ( level == t->levels ) {
		t->levels = level + 1;
		t->dirty = 1;
	}
}

void wgTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
					  GLenum format, GLenum type, const GLvoid *pixels ) {
	wglTexture_t *t = BoundTexture();
	const uint8_t *rgba;

	if ( !t || !t->tex || level >= t->mipCount || !pixels || width <= 0 || height <= 0 ) {
		return;
	}
	rgba = ToRGBA( width, height, format, type, pixels, 0 );
	if ( rgba ) {
		WriteLevel( t, level, xoffset, yoffset, width, height, rgba );
	}
}

static void SetParam( GLenum pname, GLfloat param ) {
	wglTexture_t *t = BoundTexture();
	GLenum e = (GLenum)param;

	if ( !t ) {
		return;
	}
	switch ( pname ) {
	case GL_TEXTURE_MIN_FILTER:
		if ( t->minFilter != e ) { t->minFilter = e; t->dirty = 1; }
		break;
	case GL_TEXTURE_MAG_FILTER:
		if ( t->magFilter != e ) { t->magFilter = e; t->dirty = 1; }
		break;
	case GL_TEXTURE_WRAP_S:
		if ( e == GL_LINEAR ) e = GL_CLAMP_TO_EDGE;     // RB_RenderToTexture passes GL_LINEAR here
		if ( t->wrapS != e ) { t->wrapS = e; t->dirty = 1; }
		break;
	case GL_TEXTURE_WRAP_T:
		if ( e == GL_LINEAR ) e = GL_CLAMP_TO_EDGE;
		if ( t->wrapT != e ) { t->wrapT = e; t->dirty = 1; }
		break;
	case GL_TEXTURE_MAX_ANISOTROPY_EXT:
		if ( t->anisotropy != param ) { t->anisotropy = param; t->dirty = 1; }
		break;
	default:
		break;
	}
}

void wgTexParameterf( GLenum target, GLenum pname, GLfloat param ) { SetParam( pname, param ); }
void wgTexParameteri( GLenum target, GLenum pname, GLint param ) { SetParam( pname, (GLfloat)param ); }
void wgTexParameterfv( GLenum target, GLenum pname, const GLfloat *params ) { SetParam( pname, params[0] ); }

// Copies a region of the scene target into the bound texture. GL's window
// origin is bottom-left, so the source rectangle is flipped into WebGPU's
// top-left coordinates; the copied image is therefore stored top row first.
static void CopyFromFramebuffer( wglTexture_t *t, int level, int dx, int dy, int x, int y, int width, int height ) {
	WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	WGPUExtent3D size;
	int sy;

	if ( x < 0 ) { width += x; dx -= x; x = 0; }
	if ( y < 0 ) { height += y; dy -= y; y = 0; }
	if ( x + width > wgl.width ) width = wgl.width - x;
	if ( y + height > wgl.height ) height = wgl.height - y;
	if ( width <= 0 || height <= 0 ) {
		return;
	}
	sy = wgl.height - ( y + height );

	WGL_ApplyPendingClears();
	WGL_EndPass();
	src.texture = wgl.colorTex;
	src.origin.x = x;
	src.origin.y = sy;
	src.aspect = WGPUTextureAspect_All;
	dst.texture = t->tex;
	dst.mipLevel = level;
	dst.origin.x = dx;
	dst.origin.y = dy;
	dst.aspect = WGPUTextureAspect_All;
	size.width = width;
	size.height = height;
	size.depthOrArrayLayers = 1;
	wgpuCommandEncoderCopyTextureToTexture( WGL_Encoder(), &src, &dst, &size );
}

void wgCopyTexImage2D( GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
					   GLsizei width, GLsizei height, GLint border ) {
	wglTexture_t *t = BoundTexture();

	if ( !t || level != 0 || width <= 0 || height <= 0 ) {
		return;
	}
	if ( !t->tex || t->width != width || t->height != height ) {
		AllocStorage( t, width, height );
	}
	CopyFromFramebuffer( t, 0, 0, 0, x, y, width, height );
	if ( t->levels == 0 ) {
		t->levels = 1;
		t->dirty = 1;
	}
}

void wgCopyTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
						  GLsizei width, GLsizei height ) {
	wglTexture_t *t = BoundTexture();

	if ( !t || !t->tex || level >= t->mipCount ) {
		return;
	}
	CopyFromFramebuffer( t, level, xoffset, yoffset, x, y, width, height );
}

// ---------------------------------------------------------------------------

void WGL_InitTextures( void ) {
	WGPUBindGroupLayoutEntry e[2] = { WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT };
	WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	static const uint8_t white[4] = { 255, 255, 255, 255 };

	e[0].binding = 0;
	e[0].visibility = WGPUShaderStage_Fragment;
	e[0].texture.sampleType = WGPUTextureSampleType_Float;
	e[0].texture.viewDimension = WGPUTextureViewDimension_2D;
	e[1].binding = 1;
	e[1].visibility = WGPUShaderStage_Fragment;
	e[1].sampler.type = WGPUSamplerBindingType_Filtering;
	d.entryCount = 2;
	d.entries = e;
	texLayout = wgpuDeviceCreateBindGroupLayout( wgl.device, &d );

	memset( &whiteTexture, 0, sizeof( whiteTexture ) );
	whiteTexture.minFilter = GL_NEAREST;
	whiteTexture.magFilter = GL_NEAREST;
	whiteTexture.wrapS = whiteTexture.wrapT = GL_REPEAT;
	whiteTexture.anisotropy = 1.0f;
	AllocStorage( &whiteTexture, 1, 1 );
	WriteLevel( &whiteTexture, 0, 0, 0, 1, 1, white );
	whiteTexture.levels = 1;
	whiteTexture.dirty = 1;
}

void WGL_ShutdownTextures( void ) {
	int i;

	for ( i = 0; i < numTextures; i++ ) {
		if ( textures[i] ) {
			ReleaseTextureGPU( textures[i] );
			free( textures[i] );
		}
	}
	free( textures );
	textures = NULL;
	numTextures = 0;
	nextName = 1;
	for ( i = 0; i < numSamplers; i++ ) {
		wgpuSamplerRelease( samplers[i].sampler );
	}
	numSamplers = 0;
	ReleaseTextureGPU( &whiteTexture );
	if ( texLayout ) {
		wgpuBindGroupLayoutRelease( texLayout );
		texLayout = NULL;
	}
}
