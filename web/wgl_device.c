/*
 * wgl_device.c -- WebGPU device/surface management and the GLimp_* layer.
 *
 * The scene is rendered into an offscreen RGBA8 target (the GL "back
 * buffer"). GLimp_EndFrame blits it to the canvas through a 256-entry gamma
 * ramp, which stands in for the hardware gamma ramp the original renderer
 * relies on for r_gamma and overbright lighting.
 */
#include <stdarg.h>
#include <stdio.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include "../src/renderer/tr_local.h"
#include "wgl_local.h"

wglDevice_t wgl;
int gl_NormalFontBase = 0;     // display-list font for r_debugSurface text (unsupported)

void WGL_InitState( int width, int height );

static WGPURenderPipeline blitPipeline;
static WGPUBindGroup blitBindGroup;
static WGPUTexture gammaTex;
static WGPUTextureView gammaView;
static int deviceLost;

static const char *blitShader =
"@group(0) @binding(0) var scene : texture_2d<f32>;\n"
"@group(0) @binding(1) var ramp : texture_2d<f32>;\n"
"@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {\n"
"  let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
"  return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n"
"@fragment fn fs(@builtin(position) p : vec4f) -> @location(0) vec4f {\n"
"  let c = textureLoad(scene, vec2i(p.xy), 0);\n"
"  let r = textureLoad(ramp, vec2i(i32(round(c.r * 255.0)), 0), 0).r;\n"
"  let g = textureLoad(ramp, vec2i(i32(round(c.g * 255.0)), 0), 0).g;\n"
"  let b = textureLoad(ramp, vec2i(i32(round(c.b * 255.0)), 0), 0).b;\n"
"  return vec4f(r, g, b, 1.0);\n"
"}\n";

// ---------------------------------------------------------------------------
// JS helpers

EM_JS( int, WGL_JS_PreferredFormatIsBGRA, ( void ), {
	return navigator.gpu.getPreferredCanvasFormat() === 'bgra8unorm' ? 1 : 0;
} );

// Sizes the canvas backing store to its CSS size times the device pixel
// ratio (capped) and returns the result through the out pointers.
EM_JS( void, WGL_JS_SizeCanvas, ( int *w, int *h ), {
	var canvas = Module['canvas'];
	var dpr = Math.min(window.devicePixelRatio || 1, Module['maxPixelRatio'] || 2);
	var rect = canvas.getBoundingClientRect();
	var cw = Math.max(320, Math.round(rect.width * dpr));
	var ch = Math.max(240, Math.round(rect.height * dpr));
	canvas.width = cw;
	canvas.height = ch;
	HEAP32[w >> 2] = cw;
	HEAP32[h >> 2] = ch;
} );

// ---------------------------------------------------------------------------

void WGL_Printf( const char *fmt, ... ) {
	va_list ap;
	char msg[1024];

	va_start( ap, fmt );
	Q_vsnprintf( msg, sizeof( msg ), fmt, ap );
	va_end( ap );
	if ( ri.Printf ) {
		ri.Printf( PRINT_ALL, "%s", msg );
	} else {
		fputs( msg, stdout );
	}
}

WGPUShaderModule WGL_CreateShader( const char *wgsl, const char *label ) {
	WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
	WGPUShaderModuleDescriptor d = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;

	src.code.data = wgsl;
	src.code.length = WGPU_STRLEN;
	d.nextInChain = &src.chain;
	d.label.data = label;
	d.label.length = WGPU_STRLEN;
	return wgpuDeviceCreateShaderModule( wgl.device, &d );
}

WGPUCommandEncoder WGL_Encoder( void ) {
	if ( !wgl.encoder ) {
		WGPUCommandEncoderDescriptor d = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
		wgl.encoder = wgpuDeviceCreateCommandEncoder( wgl.device, &d );
		wgl.encoderSerial++;
	}
	return wgl.encoder;
}

void WGL_SubmitEncoder( void ) {
	WGPUCommandBuffer cb;

	if ( !wgl.encoder ) {
		return;
	}
	cb = wgpuCommandEncoderFinish( wgl.encoder, NULL );
	wgpuQueueSubmit( wgl.queue, 1, &cb );
	wgpuCommandBufferRelease( cb );
	wgpuCommandEncoderRelease( wgl.encoder );
	wgl.encoder = NULL;
}

void WGL_Flush( void ) {
	WGL_EndPass();
	WGL_UploadStreams();
	WGL_SubmitEncoder();
	WGL_ResetStreams();
}

// ---------------------------------------------------------------------------
// render targets

static WGPUTexture CreateTarget( WGPUTextureFormat format, WGPUTextureUsage usage, const char *label ) {
	WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;

	d.label.data = label;
	d.label.length = WGPU_STRLEN;
	d.usage = usage;
	d.dimension = WGPUTextureDimension_2D;
	d.size.width = wgl.width;
	d.size.height = wgl.height;
	d.size.depthOrArrayLayers = 1;
	d.format = format;
	d.mipLevelCount = 1;
	d.sampleCount = 1;
	return wgpuDeviceCreateTexture( wgl.device, &d );
}

static void CreateTargets( void ) {
	WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;

	cfg.device = wgl.device;
	cfg.format = wgl.surfaceFormat;
	cfg.usage = WGPUTextureUsage_RenderAttachment;
	cfg.width = wgl.width;
	cfg.height = wgl.height;
	cfg.alphaMode = WGPUCompositeAlphaMode_Opaque;
	cfg.presentMode = WGPUPresentMode_Fifo;
	wgpuSurfaceConfigure( wgl.surface, &cfg );

	wgl.colorTex = CreateTarget( WGPUTextureFormat_RGBA8Unorm,
								 WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding
								 | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst, "scene color" );
	wgl.colorView = wgpuTextureCreateView( wgl.colorTex, NULL );
	wgl.depthTex = CreateTarget( WGPUTextureFormat_Depth24PlusStencil8, WGPUTextureUsage_RenderAttachment, "scene depth" );
	wgl.depthView = wgpuTextureCreateView( wgl.depthTex, NULL );
}

static void DestroyTargets( void ) {
	if ( wgl.colorView ) wgpuTextureViewRelease( wgl.colorView );
	if ( wgl.colorTex ) { wgpuTextureDestroy( wgl.colorTex ); wgpuTextureRelease( wgl.colorTex ); }
	if ( wgl.depthView ) wgpuTextureViewRelease( wgl.depthView );
	if ( wgl.depthTex ) { wgpuTextureDestroy( wgl.depthTex ); wgpuTextureRelease( wgl.depthTex ); }
	wgl.colorView = wgl.depthView = NULL;
	wgl.colorTex = wgl.depthTex = NULL;
	if ( wgl.surface ) {
		wgpuSurfaceUnconfigure( wgl.surface );
	}
}

static void WriteGammaRamp( const unsigned char *r, const unsigned char *g, const unsigned char *b ) {
	WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
	WGPUExtent3D size = { 256, 1, 1 };
	uint8_t ramp[256 * 4];
	int i;

	for ( i = 0; i < 256; i++ ) {
		ramp[i * 4 + 0] = r[i];
		ramp[i * 4 + 1] = g[i];
		ramp[i * 4 + 2] = b[i];
		ramp[i * 4 + 3] = 255;
	}
	dst.texture = gammaTex;
	dst.aspect = WGPUTextureAspect_All;
	layout.bytesPerRow = 256 * 4;
	layout.rowsPerImage = 1;
	wgpuQueueWriteTexture( wgl.queue, &dst, ramp, sizeof( ramp ), &layout, &size );
}

static void CreateBlit( void ) {
	WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
	WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
	WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
	WGPUBindGroupEntry e[2] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
	WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	WGPUBindGroupLayout layout;
	WGPUShaderModule module;
	unsigned char identity[256];
	int i;

	module = WGL_CreateShader( blitShader, "wgl blit" );
	d.label.data = "wgl blit";
	d.label.length = WGPU_STRLEN;
	d.layout = NULL;    // auto
	d.vertex.module = module;
	d.vertex.entryPoint.data = "vs";
	d.vertex.entryPoint.length = WGPU_STRLEN;
	d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	d.multisample.count = 1;
	d.multisample.mask = 0xFFFFFFFF;
	ct.format = wgl.surfaceFormat;
	ct.writeMask = WGPUColorWriteMask_All;
	fs.module = module;
	fs.entryPoint.data = "fs";
	fs.entryPoint.length = WGPU_STRLEN;
	fs.targetCount = 1;
	fs.targets = &ct;
	d.fragment = &fs;
	blitPipeline = wgpuDeviceCreateRenderPipeline( wgl.device, &d );
	wgpuShaderModuleRelease( module );

	td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	td.dimension = WGPUTextureDimension_2D;
	td.size.width = 256;
	td.size.height = 1;
	td.size.depthOrArrayLayers = 1;
	td.format = WGPUTextureFormat_RGBA8Unorm;
	td.mipLevelCount = 1;
	td.sampleCount = 1;
	gammaTex = wgpuDeviceCreateTexture( wgl.device, &td );
	gammaView = wgpuTextureCreateView( gammaTex, NULL );
	for ( i = 0; i < 256; i++ ) {
		identity[i] = (unsigned char)i;
	}
	WriteGammaRamp( identity, identity, identity );

	layout = wgpuRenderPipelineGetBindGroupLayout( blitPipeline, 0 );
	e[0].binding = 0;
	e[0].textureView = wgl.colorView;
	e[1].binding = 1;
	e[1].textureView = gammaView;
	bd.layout = layout;
	bd.entryCount = 2;
	bd.entries = e;
	blitBindGroup = wgpuDeviceCreateBindGroup( wgl.device, &bd );
	wgpuBindGroupLayoutRelease( layout );
}

static void DestroyBlit( void ) {
	if ( blitBindGroup ) wgpuBindGroupRelease( blitBindGroup );
	if ( blitPipeline ) wgpuRenderPipelineRelease( blitPipeline );
	if ( gammaView ) wgpuTextureViewRelease( gammaView );
	if ( gammaTex ) wgpuTextureRelease( gammaTex );
	blitBindGroup = NULL;
	blitPipeline = NULL;
	gammaView = NULL;
	gammaTex = NULL;
}

// ---------------------------------------------------------------------------
// GLimp interface

void GLimp_Init( void ) {
	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSrc = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
	WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;

	ri.Printf( PRINT_ALL, "Initializing WebGPU renderer\n" );

	if ( !wgl.device ) {
		wgl.instance = wgpuCreateInstance( NULL );
		wgl.device = emscripten_webgpu_get_device();
		if ( !wgl.device ) {
			ri.Error( ERR_FATAL, "GLimp_Init: no WebGPU device" );
		}
		wgl.queue = wgpuDeviceGetQueue( wgl.device );
		canvasSrc.selector.data = "#canvas";
		canvasSrc.selector.length = WGPU_STRLEN;
		sd.nextInChain = &canvasSrc.chain;
		wgl.surface = wgpuInstanceCreateSurface( wgl.instance, &sd );
		wgl.surfaceFormat = WGL_JS_PreferredFormatIsBGRA() ? WGPUTextureFormat_BGRA8Unorm : WGPUTextureFormat_RGBA8Unorm;
	}

	WGL_JS_SizeCanvas( &wgl.width, &wgl.height );
	CreateTargets();
	WGL_InitTextures();
	WGL_InitDraw();
	CreateBlit();
	WGL_InitState( wgl.width, wgl.height );

	glConfig.vidWidth = wgl.width;
	glConfig.vidHeight = wgl.height;
	glConfig.windowAspect = (float)wgl.width / (float)wgl.height;
	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;
	glConfig.isFullscreen = qfalse;
	glConfig.displayFrequency = 60;
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	glConfig.deviceSupportsGamma = qtrue;
	glConfig.textureCompression = TC_NONE;
	glConfig.textureEnvAddAvailable = qtrue;
	glConfig.maxActiveTextures = WGL_UNITS;
	glConfig.anisotropicAvailable = qtrue;
	glConfig.NVFogAvailable = qfalse;
	glConfig.smpActive = qfalse;
	Q_strncpyz( glConfig.vendor_string, (const char *)wgGetString( GL_VENDOR ), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, (const char *)wgGetString( GL_RENDERER ), sizeof( glConfig.renderer_string ) );
	Q_strncpyz( glConfig.version_string, (const char *)wgGetString( GL_VERSION ), sizeof( glConfig.version_string ) );
	Q_strncpyz( glConfig.extensions_string, (const char *)wgGetString( GL_EXTENSIONS ), sizeof( glConfig.extensions_string ) );

	ri.Cvar_Set( "r_lastValidRenderer", glConfig.renderer_string );
	ri.Printf( PRINT_ALL, "WebGPU: %dx%d, canvas format %s\n", wgl.width, wgl.height,
			   wgl.surfaceFormat == WGPUTextureFormat_BGRA8Unorm ? "bgra8unorm" : "rgba8unorm" );
}

void GLimp_Shutdown( void ) {
	if ( !wgl.device ) {
		return;
	}
	WGL_EndPass();
	WGL_SubmitEncoder();
	WGL_ResetStreams();
	DestroyBlit();
	WGL_ShutdownDraw();
	WGL_ShutdownTextures();
	DestroyTargets();
	memset( &glConfig, 0, sizeof( glConfig ) );
}

void GLimp_EndFrame( void ) {
	WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
	WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	WGPURenderPassEncoder bp;
	WGPUTextureView view;

	if ( deviceLost ) {
		return;
	}

	WGL_ApplyPendingClears();
	WGL_EndPass();

	wgpuSurfaceGetCurrentTexture( wgl.surface, &st );
	if ( st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal
		 || st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal ) {
		view = wgpuTextureCreateView( st.texture, NULL );
		ca.view = view;
		ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
		ca.loadOp = WGPULoadOp_Clear;
		ca.storeOp = WGPUStoreOp_Store;
		rp.colorAttachmentCount = 1;
		rp.colorAttachments = &ca;
		bp = wgpuCommandEncoderBeginRenderPass( WGL_Encoder(), &rp );
		wgpuRenderPassEncoderSetPipeline( bp, blitPipeline );
		wgpuRenderPassEncoderSetBindGroup( bp, 0, blitBindGroup, 0, NULL );
		wgpuRenderPassEncoderDraw( bp, 3, 1, 0, 0 );
		wgpuRenderPassEncoderEnd( bp );
		wgpuRenderPassEncoderRelease( bp );
		wgpuTextureViewRelease( view );
	}
	if ( st.texture ) {
		wgpuTextureRelease( st.texture );
	}

	WGL_UploadStreams();
	WGL_SubmitEncoder();
	WGL_ResetStreams();
	// presentation happens when control returns to the browser
}

void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] ) {
	if ( gammaTex ) {
		WriteGammaRamp( red, green, blue );
	}
}

void GLimp_LogComment( char *comment ) {
}

// no SMP on the web
qboolean GLimp_SpawnRenderThread( void ( *function )( void ) ) {
	return qfalse;
}

void *GLimp_RendererSleep( void ) {
	return NULL;
}

void GLimp_FrontEndSleep( void ) {
}

void GLimp_WakeRenderer( void *data ) {
}

// called from JS if the GPU device goes away
EMSCRIPTEN_KEEPALIVE void WGL_DeviceLost( void ) {
	deviceLost = 1;
}
