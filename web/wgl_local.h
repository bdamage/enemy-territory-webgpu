/*
 * wgl_local.h -- OpenGL 1.x fixed-function emulation on top of WebGPU.
 *
 * The ET renderer (src/renderer) talks to the GPU exclusively through qgl*
 * calls. web/qgl_wgpu.h routes those to the wg* functions implemented in
 * web/wgl_*.c, which track GL state and translate draws into WebGPU render
 * passes with cached pipelines, a streamed vertex/index/uniform ring and a
 * final gamma-correcting blit to the canvas.
 *
 *   wgl_device.c   device, surface, render targets, frame submit, GLimp_*
 *   wgl_state.c    enables, blend/depth/stencil, matrices, fog, queries
 *   wgl_texture.c  texture objects, uploads, samplers
 *   wgl_draw.c     client arrays, immediate mode, pipelines, draw/clear
 */
#ifndef WGL_LOCAL_H
#define WGL_LOCAL_H

#include <stdint.h>
#include <string.h>
#include <GL/gl.h>
#include <webgpu/webgpu.h>

#include "qgl_wgpu.h"

#define WGL_UNITS           2
#define WGL_MATRIX_DEPTH    32

// GL enums not present in Emscripten's gl.h
#ifndef GL_TEXTURE0_ARB
#define GL_TEXTURE0_ARB                     0x84C0
#define GL_TEXTURE1_ARB                     0x84C1
#endif
#ifndef GL_MAX_ACTIVE_TEXTURES_ARB
#define GL_MAX_ACTIVE_TEXTURES_ARB          0x84E2
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT       0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT   0x84FF
#endif
#ifndef GL_ADD
#define GL_ADD                              0x0104
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE                    0x812F
#endif

// ---------------------------------------------------------------------------
// GPU vertex format: every draw is converted to this interleaved layout

typedef struct {
	float xyz[3];
	uint8_t rgba[4];
	float st0[2];
	float st1[2];
} wglVertex_t;  // 32 bytes

// per-draw uniforms, must match the WGSL struct in wgl_draw.c
typedef struct {
	float mvp[16];
	float mv[16];
	float clip[4];          // eye-space clip plane
	float fogColor[4];
	float fogParams[4];     // start, end, density, mode (0 off, 1 linear, 2 exp, 3 exp2)
	float params[4];        // alphaRef, alphaFunc (0 off, 1 >, 2 <, 3 >=), env0, env1
	float flags[4];         // clip enabled, radial fog, -, -
} wglUniforms_t;

#define WGL_UNIFORM_STRIDE  256

// ---------------------------------------------------------------------------
// texture objects

typedef struct wglTexture_s {
	WGPUTexture tex;
	WGPUTextureView view;
	WGPUBindGroup bindGroup;        // (view, sampler) for bind group slots 1/2
	int width, height;
	int mipCount;                   // allocated levels
	int levels;                     // contiguous levels uploaded from 0
	GLenum minFilter, magFilter, wrapS, wrapT;
	float anisotropy;
	int dirty;                      // bind group needs rebuilding
	unsigned usedSerial;            // encoder serial that last referenced it
} wglTexture_t;

// ---------------------------------------------------------------------------
// client vertex arrays

typedef struct {
	int enabled;
	int size;
	GLenum type;
	int stride;
	const uint8_t *ptr;
} wglArray_t;

// ---------------------------------------------------------------------------
// GL state

typedef struct {
	// capabilities
	int blend, depthTest, cullFace, alphaTest, scissorTest, stencilTest;
	int fog, clipPlane0, polyOffsetFill, polyOffsetLine;
	int texture2D[WGL_UNITS];

	GLenum blendSrc, blendDst;
	GLenum depthFunc;
	int depthMask;
	int colorMask[4];
	GLenum cullFaceMode;
	GLenum alphaFunc;
	float alphaRef;
	float polyFactor, polyUnits;
	GLenum polygonMode;

	GLenum stencilFunc, stencilFail, stencilZFail, stencilZPass;
	int stencilRef;
	unsigned stencilFuncMask, stencilWriteMask;

	int viewport[4];                // GL window coordinates (bottom-left origin)
	int scissor[4];
	float depthNear, depthFar;

	float clearColor[4];
	float clearDepth;
	int clearStencil;

	// textures
	int activeUnit, clientUnit;
	GLuint bound[WGL_UNITS];
	GLenum texEnv[WGL_UNITS];

	// fog
	GLenum fogMode;
	float fogStart, fogEnd, fogDensity;
	float fogColor[4];
	int fogRadial;

	float clipPlane[4];             // eye space

	// matrices (column-major)
	GLenum matrixMode;
	float modelview[WGL_MATRIX_DEPTH][16];
	float projection[WGL_MATRIX_DEPTH][16];
	float texmat[WGL_UNITS][WGL_MATRIX_DEPTH][16];
	int mvDepth, projDepth, texDepth[WGL_UNITS];

	// client arrays
	wglArray_t vertexArray, colorArray, texArray[WGL_UNITS];

	// current attributes (immediate mode / disabled arrays)
	float color[4];
	float texCoord[WGL_UNITS][2];
} wglState_t;

extern wglState_t gls;

// ---------------------------------------------------------------------------
// device (wgl_device.c)

typedef struct {
	WGPUInstance instance;
	WGPUDevice device;
	WGPUQueue queue;
	WGPUSurface surface;
	WGPUTextureFormat surfaceFormat;

	int width, height;              // render target size

	WGPUTexture colorTex;           // offscreen scene target (GL "back buffer")
	WGPUTextureView colorView;
	WGPUTexture depthTex;
	WGPUTextureView depthView;

	WGPUCommandEncoder encoder;
	unsigned encoderSerial;
} wglDevice_t;

extern wglDevice_t wgl;

// engine-facing API (wgl_core.c)
void    WGL_DeviceInit( int *width, int *height );
void    WGL_DeviceShutdown( void );
void    WGL_Present( void );
void    WGL_SetGammaRamp( const unsigned char *red, const unsigned char *green, const unsigned char *blue );
extern void ( *WGL_PrintHook )( const char *msg );
void    WGL_InitState( int width, int height );

WGPUCommandEncoder WGL_Encoder( void );
void    WGL_Flush( void );          // end pass, upload streams, submit
void    WGL_SubmitEncoder( void );
WGPUShaderModule WGL_CreateShader( const char *wgsl, const char *label );
void    WGL_Printf( const char *fmt, ... );

// draw (wgl_draw.c)
void    WGL_InitDraw( void );
void    WGL_ShutdownDraw( void );
void    WGL_EndPass( void );        // end the open render pass (if any)
void    WGL_ApplyPendingClears( void );
void    WGL_UploadStreams( void );  // write CPU stream data to GPU buffers
void    WGL_ResetStreams( void );
int     WGL_PassOpen( void );

// textures (wgl_texture.c)
void    WGL_InitTextures( void );
void    WGL_ShutdownTextures( void );
wglTexture_t *WGL_GetTexture( GLuint name );
WGPUBindGroup WGL_TextureBindGroup( wglTexture_t *t );
WGPUBindGroupLayout WGL_TextureBindGroupLayout( void );
wglTexture_t *WGL_WhiteTexture( void );

// matrix helpers (wgl_state.c)
void    WGL_MatMul( float out[16], const float a[16], const float b[16] );
float  *WGL_CurrentMatrix( void );

#endif
