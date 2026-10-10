/*
 * wgl_post.c -- post-processing for views of the world: bloom, contrast
 * adaptive sharpening and vibrance.
 *
 * The renderers call WGL_EndScene after drawing a world view, before the
 * HUD, so 2D drawing stays untouched. The view's rectangle is copied out of
 * the scene target; bloom runs a bright-pass and a dual-filter blur chain
 * (half resolution down to 1/32) in rgba16float; a final pass draws the
 * sharpened scene plus bloom back into the scene target over the same
 * rectangle. Strengths come from wglOptions (r_bloom, r_sharpen, r_vibrance).
 */
#include "wgl_local.h"

#define BLOOM_LEVELS    5
#define PARAM_SLOTS     64              // uniform slots per frame (8 per scene)
#define PARAM_STRIDE    256

typedef struct {
	float rect[4];          // source rectangle in source texels: x, y, w, h
	float srcSize[4];       // source texture: w, h, 1/w, 1/h
	float params[4];        // threshold, knee, bloom strength, sharpness
	float params2[4];       // vibrance, -, target w, target h
} postParams_t;

static const char *postShader =
"struct P { rect : vec4f, srcSize : vec4f, params : vec4f, params2 : vec4f };\n"
"@group(0) @binding(0) var<uniform> p : P;\n"
"@group(0) @binding(1) var texA : texture_2d<f32>;\n"
"@group(0) @binding(2) var texB : texture_2d<f32>;\n"
"@group(0) @binding(3) var smp : sampler;\n"
"\n"
"@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {\n"
"  let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
"  return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n"
"\n"
// normalized coordinate in texA for the target pixel at pos, mapping the
// whole target onto p.rect
"fn srcUV(pos : vec2f, dstSize : vec2f) -> vec2f {\n"
"  return (p.rect.xy + pos / dstSize * p.rect.zw) * p.srcSize.zw;\n"
"}\n"
"fn tap(uv : vec2f) -> vec3f {\n"
"  let lo = (p.rect.xy + 0.5) * p.srcSize.zw;\n"
"  let hi = (p.rect.xy + p.rect.zw - 0.5) * p.srcSize.zw;\n"
"  return textureSampleLevel(texA, smp, clamp(uv, lo, hi), 0.0).rgb;\n"
"}\n"
"\n"
// bright pass at half resolution: a 4x4 texel average, soft threshold
"@fragment fn prefilter(@builtin(position) pos : vec4f) -> @location(0) vec4f {\n"
"  let dstSize = p.params2.zw;\n"
"  let uv = srcUV(pos.xy, dstSize);\n"
"  let d = p.srcSize.zw;\n"
"  let c = (tap(uv + vec2f(-d.x, -d.y)) + tap(uv + vec2f(d.x, -d.y))\n"
"         + tap(uv + vec2f(-d.x, d.y)) + tap(uv + vec2f(d.x, d.y))) * 0.25;\n"
"  let br = max(c.r, max(c.g, c.b));\n"
"  let t = p.params.x;\n"
"  let k = p.params.y;\n"
"  var soft = clamp(br - t + k, 0.0, 2.0 * k);\n"
"  soft = soft * soft / (4.0 * k + 1e-4);\n"
"  let w = max(soft, br - t) / max(br, 1e-4);\n"
"  return vec4f(c * w, 1.0);\n"
"}\n"
"\n"
// dual filter (Kawase) down- and upsampling
"@fragment fn down(@builtin(position) pos : vec4f) -> @location(0) vec4f {\n"
"  let dstSize = p.params2.zw;\n"
"  let uv = srcUV(pos.xy, dstSize);\n"
"  let h = p.srcSize.zw;\n"
"  var c = tap(uv) * 4.0;\n"
"  c += tap(uv - h) + tap(uv + h) + tap(uv + vec2f(h.x, -h.y)) + tap(uv - vec2f(h.x, -h.y));\n"
"  return vec4f(c / 8.0, 1.0);\n"
"}\n"
"@fragment fn up(@builtin(position) pos : vec4f) -> @location(0) vec4f {\n"
"  let dstSize = p.params2.zw;\n"
"  let uv = srcUV(pos.xy, dstSize);\n"
"  let h = p.srcSize.zw;\n"
"  var c = tap(uv + vec2f(-2.0 * h.x, 0.0)) + tap(uv + vec2f(2.0 * h.x, 0.0));\n"
"  c += tap(uv + vec2f(0.0, -2.0 * h.y)) + tap(uv + vec2f(0.0, 2.0 * h.y));\n"
"  c += (tap(uv + vec2f(-h.x, h.y)) + tap(uv + vec2f(h.x, h.y))\n"
"      + tap(uv + vec2f(-h.x, -h.y)) + tap(uv + vec2f(h.x, -h.y))) * 2.0;\n"
"  return vec4f(c / 12.0, 1.0);\n"
"}\n"
"\n"
// scene texel, clamped to the view rectangle
"fn px(c : vec2i) -> vec3f {\n"
"  let lo = vec2i(p.rect.xy);\n"
"  let hi = lo + vec2i(p.rect.zw) - 1;\n"
"  return textureLoad(texA, clamp(c, lo, hi), 0).rgb;\n"
"}\n"
"@fragment fn composite(@builtin(position) pos : vec4f) -> @location(0) vec4f {\n"
"  let ip = vec2i(pos.xy);\n"
"  let e4 = textureLoad(texA, ip, 0);\n"
"  var c = e4.rgb;\n"
"  let s = p.params.w;\n"
"  if (s > 0.0) {\n"
// AMD FidelityFX CAS, cross-shaped variant
"    let b = px(ip + vec2i(0, -1));\n"
"    let d = px(ip + vec2i(-1, 0));\n"
"    let f = px(ip + vec2i(1, 0));\n"
"    let h = px(ip + vec2i(0, 1));\n"
"    let mn = min(min(min(b, d), min(f, h)), c);\n"
"    let mx = max(max(max(b, d), max(f, h)), c);\n"
"    let amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3f(1e-4)), vec3f(0.0), vec3f(1.0)));\n"
"    let w = amp * (-1.0 / mix(8.0, 5.0, s));\n"
"    c = clamp(((b + d + f + h) * w + c) / (1.0 + 4.0 * w), vec3f(0.0), vec3f(1.0));\n"
"  }\n"
"  if (p.params.z > 0.0) {\n"
"    let uv = (pos.xy - p.rect.xy) / p.rect.zw;\n"
"    c += textureSampleLevel(texB, smp, uv, 0.0).rgb * p.params.z;\n"
"  }\n"
"  let v = p.params2.x;\n"
"  if (v > 0.0) {\n"
"    let l = dot(c, vec3f(0.299, 0.587, 0.114));\n"
"    let sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));\n"
"    c = mix(vec3f(l), c, 1.0 + v * (1.0 - sat));\n"
"  }\n"
"  return vec4f(clamp(c, vec3f(0.0), vec3f(1.0)), e4.a);\n"
"}\n";

static WGPUShaderModule module;
static WGPUBindGroupLayout layout;
static WGPUPipelineLayout pipelineLayout;
static WGPURenderPipeline prefilterPipe, downPipe, upPipe, compositePipe;
static WGPUSampler sampler;
static WGPUBuffer paramBuffer;
static int paramSlot;

static WGPUTexture sceneCopy;
static WGPUTextureView sceneCopyView;

// bloom chain, sized for the last view rectangle
static int bloomW, bloomH;
static WGPUTexture bloomTex[BLOOM_LEVELS];
static WGPUTextureView bloomView[BLOOM_LEVELS];
static WGPUBindGroup prefilterGroup, compositeGroup;
static WGPUBindGroup downGroup[BLOOM_LEVELS], upGroup[BLOOM_LEVELS];

// ---------------------------------------------------------------------------

static WGPURenderPipeline CreatePipeline( const char *entry, WGPUTextureFormat format, int samples, int additive ) {
	WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
	WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
	WGPUBlendState bs = WGPU_BLEND_STATE_INIT;

	d.label.data = entry;
	d.label.length = WGPU_STRLEN;
	d.layout = pipelineLayout;
	d.vertex.module = module;
	d.vertex.entryPoint.data = "vs";
	d.vertex.entryPoint.length = WGPU_STRLEN;
	d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	d.multisample.count = samples;
	d.multisample.mask = 0xFFFFFFFF;
	ct.format = format;
	ct.writeMask = WGPUColorWriteMask_All;
	if ( additive ) {
		bs.color.operation = WGPUBlendOperation_Add;
		bs.color.srcFactor = WGPUBlendFactor_One;
		bs.color.dstFactor = WGPUBlendFactor_One;
		bs.alpha = bs.color;
		ct.blend = &bs;
	}
	fs.module = module;
	fs.entryPoint.data = entry;
	fs.entryPoint.length = WGPU_STRLEN;
	fs.targetCount = 1;
	fs.targets = &ct;
	d.fragment = &fs;
	return wgpuDeviceCreateRenderPipeline( wgl.device, &d );
}

static WGPUTexture CreateTexture( int w, int h, WGPUTextureFormat format, WGPUTextureUsage usage, const char *label ) {
	WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;

	d.label.data = label;
	d.label.length = WGPU_STRLEN;
	d.usage = usage;
	d.dimension = WGPUTextureDimension_2D;
	d.size.width = w;
	d.size.height = h;
	d.size.depthOrArrayLayers = 1;
	d.format = format;
	d.mipLevelCount = 1;
	d.sampleCount = 1;
	return wgpuDeviceCreateTexture( wgl.device, &d );
}

static WGPUBindGroup CreateGroup( WGPUTextureView a, WGPUTextureView b ) {
	WGPUBindGroupEntry e[4] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
								WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
	WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;

	e[0].binding = 0;
	e[0].buffer = paramBuffer;
	e[0].size = sizeof( postParams_t );
	e[1].binding = 1;
	e[1].textureView = a;
	e[2].binding = 2;
	e[2].textureView = b;
	e[3].binding = 3;
	e[3].sampler = sampler;
	d.layout = layout;
	d.entryCount = 4;
	d.entries = e;
	return wgpuDeviceCreateBindGroup( wgl.device, &d );
}

static void ReleaseBloom( void ) {
	int i;

	if ( prefilterGroup ) wgpuBindGroupRelease( prefilterGroup );
	if ( compositeGroup ) wgpuBindGroupRelease( compositeGroup );
	prefilterGroup = compositeGroup = NULL;
	for ( i = 0; i < BLOOM_LEVELS; i++ ) {
		if ( downGroup[i] ) wgpuBindGroupRelease( downGroup[i] );
		if ( upGroup[i] ) wgpuBindGroupRelease( upGroup[i] );
		if ( bloomView[i] ) wgpuTextureViewRelease( bloomView[i] );
		if ( bloomTex[i] ) { wgpuTextureDestroy( bloomTex[i] ); wgpuTextureRelease( bloomTex[i] ); }
		downGroup[i] = upGroup[i] = NULL;
		bloomView[i] = NULL;
		bloomTex[i] = NULL;
	}
	bloomW = bloomH = 0;
}

static int LevelSize( int size, int level ) {
	size >>= level + 1;
	return size > 0 ? size : 1;
}

// (Re)creates the blur chain for a view of w x h pixels.
static void AllocBloom( int w, int h ) {
	int i;

	if ( w == bloomW && h == bloomH ) {
		return;
	}
	ReleaseBloom();
	for ( i = 0; i < BLOOM_LEVELS; i++ ) {
		bloomTex[i] = CreateTexture( LevelSize( w, i ), LevelSize( h, i ), WGPUTextureFormat_RGBA16Float,
									 WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, "bloom" );
		bloomView[i] = wgpuTextureCreateView( bloomTex[i], NULL );
	}
	prefilterGroup = CreateGroup( sceneCopyView, sceneCopyView );
	for ( i = 1; i < BLOOM_LEVELS; i++ ) {
		downGroup[i] = CreateGroup( bloomView[i - 1], sceneCopyView );
	}
	for ( i = 0; i < BLOOM_LEVELS - 1; i++ ) {
		upGroup[i] = CreateGroup( bloomView[i + 1], sceneCopyView );
	}
	compositeGroup = CreateGroup( sceneCopyView, bloomView[0] );
	bloomW = w;
	bloomH = h;
}

// ---------------------------------------------------------------------------

void WGL_InitPost( void ) {
	WGPUBindGroupLayoutEntry e[4] = { WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
									  WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT };
	WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	int i;

	module = WGL_CreateShader( postShader, "wgl post" );

	e[0].binding = 0;
	e[0].visibility = WGPUShaderStage_Fragment;
	e[0].buffer.type = WGPUBufferBindingType_Uniform;
	e[0].buffer.hasDynamicOffset = 1;
	e[0].buffer.minBindingSize = sizeof( postParams_t );
	for ( i = 1; i <= 2; i++ ) {
		e[i].binding = i;
		e[i].visibility = WGPUShaderStage_Fragment;
		e[i].texture.sampleType = WGPUTextureSampleType_Float;
		e[i].texture.viewDimension = WGPUTextureViewDimension_2D;
	}
	e[3].binding = 3;
	e[3].visibility = WGPUShaderStage_Fragment;
	e[3].sampler.type = WGPUSamplerBindingType_Filtering;
	ld.entryCount = 4;
	ld.entries = e;
	layout = wgpuDeviceCreateBindGroupLayout( wgl.device, &ld );
	pd.bindGroupLayoutCount = 1;
	pd.bindGroupLayouts = &layout;
	pipelineLayout = wgpuDeviceCreatePipelineLayout( wgl.device, &pd );

	prefilterPipe = CreatePipeline( "prefilter", WGPUTextureFormat_RGBA16Float, 1, 0 );
	downPipe = CreatePipeline( "down", WGPUTextureFormat_RGBA16Float, 1, 0 );
	upPipe = CreatePipeline( "up", WGPUTextureFormat_RGBA16Float, 1, 1 );
	compositePipe = CreatePipeline( "composite", WGPUTextureFormat_RGBA8Unorm, wgl.samples, 0 );

	sd.magFilter = WGPUFilterMode_Linear;
	sd.minFilter = WGPUFilterMode_Linear;
	sampler = wgpuDeviceCreateSampler( wgl.device, &sd );

	bd.label.data = "wgl post params";
	bd.label.length = WGPU_STRLEN;
	bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
	bd.size = PARAM_SLOTS * PARAM_STRIDE;
	paramBuffer = wgpuDeviceCreateBuffer( wgl.device, &bd );
	paramSlot = 0;

	sceneCopy = CreateTexture( wgl.width, wgl.height, WGPUTextureFormat_RGBA8Unorm,
							   WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, "scene copy" );
	sceneCopyView = wgpuTextureCreateView( sceneCopy, NULL );
}

void WGL_ShutdownPost( void ) {
	ReleaseBloom();
	if ( sceneCopyView ) wgpuTextureViewRelease( sceneCopyView );
	if ( sceneCopy ) { wgpuTextureDestroy( sceneCopy ); wgpuTextureRelease( sceneCopy ); }
	if ( paramBuffer ) wgpuBufferRelease( paramBuffer );
	if ( sampler ) wgpuSamplerRelease( sampler );
	if ( prefilterPipe ) wgpuRenderPipelineRelease( prefilterPipe );
	if ( downPipe ) wgpuRenderPipelineRelease( downPipe );
	if ( upPipe ) wgpuRenderPipelineRelease( upPipe );
	if ( compositePipe ) wgpuRenderPipelineRelease( compositePipe );
	if ( pipelineLayout ) wgpuPipelineLayoutRelease( pipelineLayout );
	if ( layout ) wgpuBindGroupLayoutRelease( layout );
	if ( module ) wgpuShaderModuleRelease( module );
	sceneCopyView = NULL;
	sceneCopy = NULL;
	paramBuffer = NULL;
	sampler = NULL;
	prefilterPipe = downPipe = upPipe = compositePipe = NULL;
	pipelineLayout = NULL;
	layout = NULL;
	module = NULL;
}

void WGL_PostFrame( void ) {
	paramSlot = 0;
}

// ---------------------------------------------------------------------------

static uint32_t PushParams( const postParams_t *p ) {
	uint32_t offset = (uint32_t)paramSlot * PARAM_STRIDE;

	wgpuQueueWriteBuffer( wgl.queue, paramBuffer, offset, p, sizeof( *p ) );
	paramSlot++;
	return offset;
}

static void Pass( WGPUTextureView target, WGPUTextureView resolve, WGPULoadOp load, WGPURenderPipeline pipe,
				  WGPUBindGroup group, uint32_t offset, const int *rect ) {
	WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	WGPURenderPassEncoder pass;

	ca.view = target;
	ca.resolveTarget = resolve;
	ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
	ca.loadOp = load;
	ca.storeOp = WGPUStoreOp_Store;
	rp.colorAttachmentCount = 1;
	rp.colorAttachments = &ca;
	pass = wgpuCommandEncoderBeginRenderPass( WGL_Encoder(), &rp );
	wgpuRenderPassEncoderSetPipeline( pass, pipe );
	wgpuRenderPassEncoderSetBindGroup( pass, 0, group, 1, &offset );
	if ( rect ) {
		wgpuRenderPassEncoderSetViewport( pass, 0.0f, 0.0f, (float)wgl.width, (float)wgl.height, 0.0f, 1.0f );
		wgpuRenderPassEncoderSetScissorRect( pass, rect[0], rect[1], rect[2], rect[3] );
	}
	wgpuRenderPassEncoderDraw( pass, 3, 1, 0, 0 );
	wgpuRenderPassEncoderEnd( pass );
	wgpuRenderPassEncoderRelease( pass );
}

// Source rectangle and texture size, and the size of the target drawn.
static void SetSource( postParams_t *p, float x, float y, float w, float h, int texW, int texH, int dstW, int dstH ) {
	p->params2[2] = (float)dstW;
	p->params2[3] = (float)dstH;
	p->rect[0] = x;
	p->rect[1] = y;
	p->rect[2] = w;
	p->rect[3] = h;
	p->srcSize[0] = (float)texW;
	p->srcSize[1] = (float)texH;
	p->srcSize[2] = 1.0f / texW;
	p->srcSize[3] = 1.0f / texH;
}

void WGL_EndScene( int x, int y, int width, int height ) {
	WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	WGPUExtent3D size;
	postParams_t p;
	int rect[4], i, bloom;

	bloom = wglOptions.bloom > 0.0f;
	if ( !wgl.device || !sceneCopy || ( !bloom && wglOptions.sharpen <= 0.0f && wglOptions.vibrance <= 0.0f ) ) {
		return;
	}
	if ( paramSlot + 2 * BLOOM_LEVELS + 1 > PARAM_SLOTS ) {
		return;     // a dozen views in one frame: leave the rest alone
	}

	// GL window coordinates -> target coordinates
	rect[0] = x < 0 ? 0 : x;
	rect[1] = wgl.height - ( y + height );
	if ( rect[1] < 0 ) rect[1] = 0;
	rect[2] = ( x + width > wgl.width ? wgl.width : x + width ) - rect[0];
	rect[3] = ( wgl.height - y > wgl.height ? wgl.height : wgl.height - y ) - rect[1];
	if ( rect[2] < 8 || rect[3] < 8 ) {
		return;
	}

	WGL_ApplyPendingClears();
	WGL_EndPass();

	// the view, as drawn so far (colorTex is resolved at every pass end)
	src.texture = wgl.colorTex;
	src.origin.x = rect[0];
	src.origin.y = rect[1];
	src.aspect = WGPUTextureAspect_All;
	dst.texture = sceneCopy;
	dst.origin.x = rect[0];
	dst.origin.y = rect[1];
	dst.aspect = WGPUTextureAspect_All;
	size.width = rect[2];
	size.height = rect[3];
	size.depthOrArrayLayers = 1;
	wgpuCommandEncoderCopyTextureToTexture( WGL_Encoder(), &src, &dst, &size );

	memset( &p, 0, sizeof( p ) );
	p.params[0] = 0.72f;                // threshold (before the gamma ramp)
	p.params[1] = 0.2f;                 // soft knee
	p.params[2] = wglOptions.bloom;
	p.params[3] = wglOptions.sharpen > 1.0f ? 1.0f : wglOptions.sharpen;
	p.params2[0] = wglOptions.vibrance;

	if ( bloom ) {
		AllocBloom( rect[2], rect[3] );

		SetSource( &p, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], wgl.width, wgl.height,
				   LevelSize( bloomW, 0 ), LevelSize( bloomH, 0 ) );
		Pass( bloomView[0], NULL, WGPULoadOp_Clear, prefilterPipe, prefilterGroup, PushParams( &p ), NULL );
		for ( i = 1; i < BLOOM_LEVELS; i++ ) {
			int w = LevelSize( bloomW, i - 1 ), h = LevelSize( bloomH, i - 1 );
			SetSource( &p, 0.0f, 0.0f, (float)w, (float)h, w, h, LevelSize( bloomW, i ), LevelSize( bloomH, i ) );
			Pass( bloomView[i], NULL, WGPULoadOp_Clear, downPipe, downGroup[i], PushParams( &p ), NULL );
		}
		for ( i = BLOOM_LEVELS - 2; i >= 0; i-- ) {
			int w = LevelSize( bloomW, i + 1 ), h = LevelSize( bloomH, i + 1 );
			SetSource( &p, 0.0f, 0.0f, (float)w, (float)h, w, h, LevelSize( bloomW, i ), LevelSize( bloomH, i ) );
			Pass( bloomView[i], NULL, WGPULoadOp_Load, upPipe, upGroup[i], PushParams( &p ), NULL );
		}
		// level 0 sums BLOOM_LEVELS blurred layers
		p.params[2] = wglOptions.bloom / BLOOM_LEVELS;
	} else {
		AllocBloom( 8, 8 );     // composite still needs something bound
		p.params[2] = 0.0f;
	}

	SetSource( &p, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], wgl.width, wgl.height,
			   wgl.width, wgl.height );
	if ( wgl.samples > 1 ) {
		Pass( wgl.msaaView, wgl.colorView, WGPULoadOp_Load, compositePipe, compositeGroup, PushParams( &p ), rect );
	} else {
		Pass( wgl.colorView, NULL, WGPULoadOp_Load, compositePipe, compositeGroup, PushParams( &p ), rect );
	}
}
