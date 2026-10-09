/*
 * wgl_draw.c -- draw submission for the WebGPU fixed-function emulation.
 *
 * Every GL draw (glDrawElements, glBegin/glEnd) is converted to indexed
 * triangles/lines/points in a common 32-byte vertex format and appended to
 * per-frame CPU streams. The streams are uploaded with one writeBuffer each
 * right before the command buffer is submitted (or earlier when they fill
 * up). Fixed-function state maps onto:
 *   - a cached render pipeline (blend, depth, stencil, cull, color mask,
 *     polygon offset, topology)
 *   - a 256-byte uniform slot (matrices, texenv, alpha test, fog, clip plane)
 *   - two texture bind groups
 *   - viewport/scissor/stencil reference set on the pass.
 * Render passes are opened lazily so full-screen glClear()s become load ops.
 */
#include <stdlib.h>
#include "wgl_local.h"

#define VERTEX_CAP      ( 1 << 19 )     // 512K vertices (16 MB)
#define INDEX_CAP       ( 1 << 21 )     // 2M indices (8 MB)
#define UNIFORM_CAP     8192            // 8192 slots (2 MB)
#define IMM_CAP         ( 1 << 16 )

static const char *shaderSource =
"struct U {\n"
"  mvp : mat4x4f,\n"
"  mv : mat4x4f,\n"
"  clip : vec4f,\n"
"  fogColor : vec4f,\n"
"  fogParams : vec4f,\n"
"  params : vec4f,\n"
"  flags : vec4f,\n"
"};\n"
"@group(0) @binding(0) var<uniform> u : U;\n"
"@group(1) @binding(0) var t0 : texture_2d<f32>;\n"
"@group(1) @binding(1) var s0 : sampler;\n"
"@group(2) @binding(0) var t1 : texture_2d<f32>;\n"
"@group(2) @binding(1) var s1 : sampler;\n"
"\n"
"struct VIn {\n"
"  @location(0) pos : vec3f,\n"
"  @location(1) col : vec4f,\n"
"  @location(2) tc0 : vec2f,\n"
"  @location(3) tc1 : vec2f,\n"
"};\n"
"struct VOut {\n"
"  @builtin(position) pos : vec4f,\n"
"  @location(0) col : vec4f,\n"
"  @location(1) tc0 : vec2f,\n"
"  @location(2) tc1 : vec2f,\n"
"  @location(3) eye : vec4f,\n"
"};\n"
"\n"
"@vertex fn vs(v : VIn) -> VOut {\n"
"  var o : VOut;\n"
"  let p = vec4f(v.pos, 1.0);\n"
"  var c = u.mvp * p;\n"
"  c.z = (c.z + c.w) * 0.5;\n"              // GL clip space z [-w,w] -> [0,w]
"  o.pos = c;\n"
"  o.col = v.col;\n"
"  o.tc0 = v.tc0;\n"
"  o.tc1 = v.tc1;\n"
"  o.eye = u.mv * p;\n"
"  return o;\n"
"}\n"
"\n"
"fn texenv(mode : f32, c : vec4f, t : vec4f) -> vec4f {\n"
"  if (mode < 1.5) { return c * t; }\n"                                    // GL_MODULATE
"  if (mode < 2.5) { return t; }\n"                                        // GL_REPLACE
"  if (mode < 3.5) { return vec4f(mix(c.rgb, t.rgb, t.a), c.a); }\n"       // GL_DECAL
"  return vec4f(min(c.rgb + t.rgb, vec3f(1.0)), c.a * t.a);\n"             // GL_ADD
"}\n"
"\n"
"@fragment fn fs(i : VOut) -> @location(0) vec4f {\n"
"  let a = textureSample(t0, s0, i.tc0);\n"
"  let b = textureSample(t1, s1, i.tc1);\n"
"  if (u.flags.x > 0.5 && dot(i.eye, u.clip) < 0.0) { discard; }\n"
"  var c = i.col;\n"
"  if (u.params.z > 0.5) { c = texenv(u.params.z, c, a); }\n"
"  if (u.params.w > 0.5) { c = texenv(u.params.w, c, b); }\n"
"  let af = u.params.y;\n"
"  if (af > 0.5) {\n"
"    if (af < 1.5) { if (!(c.a > u.params.x)) { discard; } }\n"
"    else if (af < 2.5) { if (!(c.a < u.params.x)) { discard; } }\n"
"    else { if (!(c.a >= u.params.x)) { discard; } }\n"
"  }\n"
"  let fm = u.fogParams.w;\n"
"  if (fm > 0.5) {\n"
"    var z = abs(i.eye.z);\n"
"    if (u.flags.y > 0.5) { z = length(i.eye.xyz); }\n"
"    var f : f32;\n"
"    if (fm < 1.5) { f = (u.fogParams.y - z) / max(u.fogParams.y - u.fogParams.x, 0.0001); }\n"
"    else if (fm < 2.5) { f = exp(-u.fogParams.z * z); }\n"
"    else { let d = u.fogParams.z * z; f = exp(-d * d); }\n"
"    f = clamp(f, 0.0, 1.0);\n"
"    c = vec4f(mix(u.fogColor.rgb, c.rgb, f), c.a);\n"
"  }\n"
"  return c;\n"
"}\n";

// ---------------------------------------------------------------------------
// pipelines

typedef struct {
	uint8_t topology;
	uint8_t blend, srcFactor, dstFactor;
	uint8_t depthCompare, depthWrite;
	uint8_t cullMode, colorMask;
	uint8_t stencilCompare, stencilFail, stencilZFail, stencilPass;
	uint8_t stencilRead, stencilWrite;
	uint8_t pad[2];
	int32_t biasUnits;
	float biasSlope;
} pipeKey_t;

typedef struct {
	pipeKey_t key;
	WGPURenderPipeline pipeline;
} pipeEntry_t;

#define PIPE_HASH 1024
static pipeEntry_t pipeTable[PIPE_HASH];
static int numPipelines;

static WGPUShaderModule shaderModule;
static WGPUBindGroupLayout uniformLayout;
static WGPUPipelineLayout pipelineLayout;
static WGPUBindGroup uniformBindGroup;

// ---------------------------------------------------------------------------
// streams

static WGPUBuffer vertexBuffer, indexBuffer, uniformBuffer;
static wglVertex_t *cpuVerts;
static uint32_t *cpuIndexes;
static uint8_t *cpuUniforms;
static int numVerts, numIndexes, numUniforms;
static int uploadedVerts, uploadedIndexes, uploadedUniforms;

static wglUniforms_t lastUniforms;
static int lastUniformSlot = -1;

// ---------------------------------------------------------------------------
// render pass

static WGPURenderPassEncoder pass;
static WGPURenderPipeline curPipeline;
static WGPUBindGroup curTex[2];
static int curUniformSlot;
static int curViewport[4];
static float curDepthRange[2];
static int curScissor[4];
static int curStencilRef;

static struct {
	int color, depth, stencil;
	float colorValue[4];
	float depthValue;
	int stencilValue;
} pending;

// ---------------------------------------------------------------------------
// immediate mode

static wglVertex_t *immVerts;
static int immCount;
static GLenum immMode;
static int inBeginEnd;
static uint32_t *scratchIndexes;
static int scratchSize;

// ---------------------------------------------------------------------------

int WGL_PassOpen( void ) {
	return pass != NULL;
}

static uint32_t *ScratchIndexes( int count ) {
	if ( count > scratchSize ) {
		scratchSize = count + 1024;
		scratchIndexes = realloc( scratchIndexes, scratchSize * sizeof( uint32_t ) );
	}
	return scratchIndexes;
}

static uint8_t FloatToByte( float f ) {
	if ( f <= 0.0f ) {
		return 0;
	}
	if ( f >= 1.0f ) {
		return 255;
	}
	return (uint8_t)( f * 255.0f + 0.5f );
}

static WGPUBlendFactor BlendFactor( GLenum f ) {
	switch ( f ) {
	case GL_ZERO: return WGPUBlendFactor_Zero;
	case GL_ONE: return WGPUBlendFactor_One;
	case GL_SRC_COLOR: return WGPUBlendFactor_Src;
	case GL_ONE_MINUS_SRC_COLOR: return WGPUBlendFactor_OneMinusSrc;
	case GL_SRC_ALPHA: return WGPUBlendFactor_SrcAlpha;
	case GL_ONE_MINUS_SRC_ALPHA: return WGPUBlendFactor_OneMinusSrcAlpha;
	case GL_DST_ALPHA: return WGPUBlendFactor_DstAlpha;
	case GL_ONE_MINUS_DST_ALPHA: return WGPUBlendFactor_OneMinusDstAlpha;
	case GL_DST_COLOR: return WGPUBlendFactor_Dst;
	case GL_ONE_MINUS_DST_COLOR: return WGPUBlendFactor_OneMinusDst;
	case GL_SRC_ALPHA_SATURATE: return WGPUBlendFactor_SrcAlphaSaturated;
	default: return WGPUBlendFactor_One;
	}
}

static WGPUCompareFunction CompareFunc( GLenum f ) {
	switch ( f ) {
	case GL_NEVER: return WGPUCompareFunction_Never;
	case GL_LESS: return WGPUCompareFunction_Less;
	case GL_EQUAL: return WGPUCompareFunction_Equal;
	case GL_LEQUAL: return WGPUCompareFunction_LessEqual;
	case GL_GREATER: return WGPUCompareFunction_Greater;
	case GL_NOTEQUAL: return WGPUCompareFunction_NotEqual;
	case GL_GEQUAL: return WGPUCompareFunction_GreaterEqual;
	default: return WGPUCompareFunction_Always;
	}
}

static WGPUStencilOperation StencilOp( GLenum op ) {
	switch ( op ) {
	case GL_ZERO: return WGPUStencilOperation_Zero;
	case GL_REPLACE: return WGPUStencilOperation_Replace;
	case GL_INCR: return WGPUStencilOperation_IncrementClamp;
	case GL_DECR: return WGPUStencilOperation_DecrementClamp;
	case GL_INVERT: return WGPUStencilOperation_Invert;
	case 0x8507: return WGPUStencilOperation_IncrementWrap;    // GL_INCR_WRAP
	case 0x8508: return WGPUStencilOperation_DecrementWrap;    // GL_DECR_WRAP
	default: return WGPUStencilOperation_Keep;
	}
}

static unsigned HashKey( const pipeKey_t *k ) {
	const uint8_t *p = (const uint8_t *)k;
	unsigned h = 2166136261u;
	size_t i;

	for ( i = 0; i < sizeof( *k ); i++ ) {
		h = ( h ^ p[i] ) * 16777619u;
	}
	return h;
}

static WGPURenderPipeline CreatePipeline( const pipeKey_t *k ) {
	WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	WGPUVertexAttribute attrs[4] = {
		WGPU_VERTEX_ATTRIBUTE_INIT, WGPU_VERTEX_ATTRIBUTE_INIT, WGPU_VERTEX_ATTRIBUTE_INIT, WGPU_VERTEX_ATTRIBUTE_INIT
	};
	WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
	WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
	WGPUBlendState bs = WGPU_BLEND_STATE_INIT;
	WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
	WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
	WGPUStencilFaceState sf;

	attrs[0].format = WGPUVertexFormat_Float32x3; attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
	attrs[1].format = WGPUVertexFormat_Unorm8x4;  attrs[1].offset = 12; attrs[1].shaderLocation = 1;
	attrs[2].format = WGPUVertexFormat_Float32x2; attrs[2].offset = 16; attrs[2].shaderLocation = 2;
	attrs[3].format = WGPUVertexFormat_Float32x2; attrs[3].offset = 24; attrs[3].shaderLocation = 3;
	vbl.stepMode = WGPUVertexStepMode_Vertex;
	vbl.arrayStride = sizeof( wglVertex_t );
	vbl.attributeCount = 4;
	vbl.attributes = attrs;

	d.label.data = "wgl draw";
	d.label.length = WGPU_STRLEN;
	d.layout = pipelineLayout;
	d.vertex.module = shaderModule;
	d.vertex.entryPoint.data = "vs";
	d.vertex.entryPoint.length = WGPU_STRLEN;
	d.vertex.bufferCount = 1;
	d.vertex.buffers = &vbl;

	d.primitive.topology = (WGPUPrimitiveTopology)k->topology;
	d.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
	d.primitive.frontFace = WGPUFrontFace_CCW;
	d.primitive.cullMode = (WGPUCullMode)k->cullMode;

	ds.format = WGPUTextureFormat_Depth24PlusStencil8;
	ds.depthWriteEnabled = k->depthWrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
	ds.depthCompare = (WGPUCompareFunction)k->depthCompare;
	sf.compare = (WGPUCompareFunction)k->stencilCompare;
	sf.failOp = (WGPUStencilOperation)k->stencilFail;
	sf.depthFailOp = (WGPUStencilOperation)k->stencilZFail;
	sf.passOp = (WGPUStencilOperation)k->stencilPass;
	ds.stencilFront = sf;
	ds.stencilBack = sf;
	ds.stencilReadMask = k->stencilRead;
	ds.stencilWriteMask = k->stencilWrite;
	ds.depthBias = k->biasUnits;
	ds.depthBiasSlopeScale = k->biasSlope;
	ds.depthBiasClamp = 0.0f;
	d.depthStencil = &ds;

	d.multisample.count = 1;
	d.multisample.mask = 0xFFFFFFFF;

	bs.color.operation = WGPUBlendOperation_Add;
	bs.color.srcFactor = (WGPUBlendFactor)k->srcFactor;
	bs.color.dstFactor = (WGPUBlendFactor)k->dstFactor;
	bs.alpha = bs.color;
	ct.format = WGPUTextureFormat_RGBA8Unorm;
	ct.blend = k->blend ? &bs : NULL;
	ct.writeMask = (WGPUColorWriteMask)k->colorMask;
	fs.module = shaderModule;
	fs.entryPoint.data = "fs";
	fs.entryPoint.length = WGPU_STRLEN;
	fs.targetCount = 1;
	fs.targets = &ct;
	d.fragment = &fs;

	numPipelines++;
	return wgpuDeviceCreateRenderPipeline( wgl.device, &d );
}

static WGPURenderPipeline GetPipeline( const pipeKey_t *k ) {
	unsigned h = HashKey( k ) & ( PIPE_HASH - 1 );
	int i;

	for ( i = 0; i < PIPE_HASH; i++ ) {
		pipeEntry_t *e = &pipeTable[( h + i ) & ( PIPE_HASH - 1 )];
		if ( !e->pipeline ) {
			e->key = *k;
			e->pipeline = CreatePipeline( k );
			return e->pipeline;
		}
		if ( !memcmp( &e->key, k, sizeof( *k ) ) ) {
			return e->pipeline;
		}
	}
	WGL_Printf( "wgl: pipeline table full\n" );
	return pipeTable[h].pipeline;
}

// Builds the pipeline key for the current GL state.
static void StateKey( pipeKey_t *k, WGPUPrimitiveTopology topology ) {
	memset( k, 0, sizeof( *k ) );
	k->topology = (uint8_t)topology;

	if ( gls.blend ) {
		k->blend = 1;
		k->srcFactor = (uint8_t)BlendFactor( gls.blendSrc );
		k->dstFactor = (uint8_t)BlendFactor( gls.blendDst );
	}

	// with depth testing disabled GL neither tests nor writes depth
	if ( gls.depthTest ) {
		k->depthCompare = (uint8_t)CompareFunc( gls.depthFunc );
		k->depthWrite = (uint8_t)gls.depthMask;
	} else {
		k->depthCompare = (uint8_t)WGPUCompareFunction_Always;
	}

	if ( gls.cullFace && topology == WGPUPrimitiveTopology_TriangleList ) {
		if ( gls.cullFaceMode == GL_FRONT ) {
			k->cullMode = (uint8_t)WGPUCullMode_Front;
		} else if ( gls.cullFaceMode == GL_BACK ) {
			k->cullMode = (uint8_t)WGPUCullMode_Back;
		} else {
			// GL_FRONT_AND_BACK culls everything; draws are skipped by the caller
			k->cullMode = (uint8_t)WGPUCullMode_None;
		}
	} else {
		k->cullMode = (uint8_t)WGPUCullMode_None;
	}

	k->colorMask = ( gls.colorMask[0] ? WGPUColorWriteMask_Red : 0 ) | ( gls.colorMask[1] ? WGPUColorWriteMask_Green : 0 )
				   | ( gls.colorMask[2] ? WGPUColorWriteMask_Blue : 0 ) | ( gls.colorMask[3] ? WGPUColorWriteMask_Alpha : 0 );

	if ( gls.stencilTest ) {
		k->stencilCompare = (uint8_t)CompareFunc( gls.stencilFunc );
		k->stencilFail = (uint8_t)StencilOp( gls.stencilFail );
		k->stencilZFail = (uint8_t)StencilOp( gls.stencilZFail );
		k->stencilPass = (uint8_t)StencilOp( gls.stencilZPass );
		k->stencilRead = (uint8_t)gls.stencilFuncMask;
		k->stencilWrite = (uint8_t)gls.stencilWriteMask;
	} else {
		k->stencilCompare = (uint8_t)WGPUCompareFunction_Always;
		k->stencilFail = k->stencilZFail = k->stencilPass = (uint8_t)WGPUStencilOperation_Keep;
		k->stencilRead = 0xff;
		k->stencilWrite = 0;
	}

	if ( topology == WGPUPrimitiveTopology_TriangleList && gls.polyOffsetFill ) {
		k->biasUnits = (int32_t)gls.polyUnits;
		k->biasSlope = gls.polyFactor;
	} else if ( topology == WGPUPrimitiveTopology_LineList && gls.polyOffsetLine ) {
		// WebGPU only allows depth bias on triangles: lines get none
	}
}

static float EnvCode( GLenum env ) {
	switch ( env ) {
	case GL_REPLACE: return 2.0f;
	case GL_DECAL: return 3.0f;
	case GL_ADD: return 4.0f;
	default: return 1.0f;
	}
}

static void StateUniforms( wglUniforms_t *u ) {
	const float *mv = gls.modelview[gls.mvDepth];

	memset( u, 0, sizeof( *u ) );
	WGL_MatMul( u->mvp, gls.projection[gls.projDepth], mv );
	memcpy( u->mv, mv, sizeof( u->mv ) );

	if ( gls.clipPlane0 ) {
		memcpy( u->clip, gls.clipPlane, sizeof( u->clip ) );
		u->flags[0] = 1.0f;
	}
	if ( gls.fog ) {
		memcpy( u->fogColor, gls.fogColor, sizeof( u->fogColor ) );
		u->fogParams[0] = gls.fogStart;
		u->fogParams[1] = gls.fogEnd;
		u->fogParams[2] = gls.fogDensity;
		u->fogParams[3] = gls.fogMode == GL_LINEAR ? 1.0f : gls.fogMode == GL_EXP2 ? 3.0f : 2.0f;
		u->flags[1] = gls.fogRadial ? 1.0f : 0.0f;
	}
	if ( gls.alphaTest ) {
		u->params[0] = gls.alphaRef;
		switch ( gls.alphaFunc ) {
		case GL_GREATER: u->params[1] = 1.0f; break;
		case GL_LESS: u->params[1] = 2.0f; break;
		case GL_GEQUAL: u->params[1] = 3.0f; break;
		default: break;
		}
	}
	u->params[2] = gls.texture2D[0] ? EnvCode( gls.texEnv[0] ) : 0.0f;
	u->params[3] = gls.texture2D[1] ? EnvCode( gls.texEnv[1] ) : 0.0f;
}

// ---------------------------------------------------------------------------
// render pass management

static void OpenPass( void ) {
	WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	WGPURenderPassDepthStencilAttachment da = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;

	ca.view = wgl.colorView;
	ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
	ca.loadOp = pending.color ? WGPULoadOp_Clear : WGPULoadOp_Load;
	ca.storeOp = WGPUStoreOp_Store;
	ca.clearValue.r = pending.colorValue[0];
	ca.clearValue.g = pending.colorValue[1];
	ca.clearValue.b = pending.colorValue[2];
	ca.clearValue.a = pending.colorValue[3];

	da.view = wgl.depthView;
	da.depthLoadOp = pending.depth ? WGPULoadOp_Clear : WGPULoadOp_Load;
	da.depthStoreOp = WGPUStoreOp_Store;
	da.depthClearValue = pending.depthValue;
	da.stencilLoadOp = pending.stencil ? WGPULoadOp_Clear : WGPULoadOp_Load;
	da.stencilStoreOp = WGPUStoreOp_Store;
	da.stencilClearValue = pending.stencilValue;

	rp.colorAttachmentCount = 1;
	rp.colorAttachments = &ca;
	rp.depthStencilAttachment = &da;

	pass = wgpuCommandEncoderBeginRenderPass( WGL_Encoder(), &rp );
	pending.color = pending.depth = pending.stencil = 0;

	wgpuRenderPassEncoderSetVertexBuffer( pass, 0, vertexBuffer, 0, WGPU_WHOLE_SIZE );
	wgpuRenderPassEncoderSetIndexBuffer( pass, indexBuffer, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE );
	curPipeline = NULL;
	curTex[0] = curTex[1] = NULL;
	curUniformSlot = -1;
	curViewport[2] = curViewport[3] = -1;
	curScissor[2] = curScissor[3] = -1;
	curStencilRef = -1;
}

void WGL_EndPass( void ) {
	if ( pass ) {
		wgpuRenderPassEncoderEnd( pass );
		wgpuRenderPassEncoderRelease( pass );
		pass = NULL;
	}
}

// opens and closes a pass if clears are still pending, so they take effect
void WGL_ApplyPendingClears( void ) {
	if ( pending.color || pending.depth || pending.stencil ) {
		OpenPass();
		WGL_EndPass();
	}
}

// ---------------------------------------------------------------------------
// streams

void WGL_UploadStreams( void ) {
	if ( numVerts > uploadedVerts ) {
		wgpuQueueWriteBuffer( wgl.queue, vertexBuffer, uploadedVerts * sizeof( wglVertex_t ),
							  cpuVerts + uploadedVerts, ( numVerts - uploadedVerts ) * sizeof( wglVertex_t ) );
		uploadedVerts = numVerts;
	}
	if ( numIndexes > uploadedIndexes ) {
		wgpuQueueWriteBuffer( wgl.queue, indexBuffer, uploadedIndexes * sizeof( uint32_t ),
							  cpuIndexes + uploadedIndexes, ( numIndexes - uploadedIndexes ) * sizeof( uint32_t ) );
		uploadedIndexes = numIndexes;
	}
	if ( numUniforms > uploadedUniforms ) {
		wgpuQueueWriteBuffer( wgl.queue, uniformBuffer, uploadedUniforms * WGL_UNIFORM_STRIDE,
							  cpuUniforms + uploadedUniforms * WGL_UNIFORM_STRIDE,
							  ( numUniforms - uploadedUniforms ) * WGL_UNIFORM_STRIDE );
		uploadedUniforms = numUniforms;
	}
}

void WGL_ResetStreams( void ) {
	numVerts = numIndexes = numUniforms = 0;
	uploadedVerts = uploadedIndexes = uploadedUniforms = 0;
	lastUniformSlot = -1;
}

// Makes room for a draw, flushing the frame so far if the streams are full.
static int Reserve( int verts, int indexes ) {
	if ( verts > VERTEX_CAP || indexes > INDEX_CAP ) {
		WGL_Printf( "wgl: draw too large (%d verts, %d indexes)\n", verts, indexes );
		return 0;
	}
	if ( numVerts + verts > VERTEX_CAP || numIndexes + indexes > INDEX_CAP || numUniforms + 1 > UNIFORM_CAP ) {
		WGL_Flush();
	}
	return 1;
}

static int PushUniforms( const wglUniforms_t *u ) {
	if ( lastUniformSlot >= 0 && !memcmp( u, &lastUniforms, sizeof( *u ) ) ) {
		return lastUniformSlot;
	}
	memcpy( cpuUniforms + numUniforms * WGL_UNIFORM_STRIDE, u, sizeof( *u ) );
	lastUniforms = *u;
	lastUniformSlot = numUniforms++;
	return lastUniformSlot;
}

// GL window rect (bottom-left origin) -> WebGPU rect (top-left), clamped
static int ToTargetRect( const int r[4], int out[4] ) {
	int x0 = r[0], y0 = wgl.height - ( r[1] + r[3] );
	int x1 = r[0] + r[2], y1 = wgl.height - r[1];

	if ( x0 < 0 ) x0 = 0;
	if ( y0 < 0 ) y0 = 0;
	if ( x1 > wgl.width ) x1 = wgl.width;
	if ( y1 > wgl.height ) y1 = wgl.height;
	out[0] = x0;
	out[1] = y0;
	out[2] = x1 - x0;
	out[3] = y1 - y0;
	return out[2] > 0 && out[3] > 0;
}

// ---------------------------------------------------------------------------
// draw submission

typedef struct {
	pipeKey_t key;
	wglUniforms_t uniforms;
	WGPUBindGroup tex[2];
	int viewport[4];        // target coordinates
	float depthRange[2];
	int scissor[4];
	int stencilRef;
} drawState_t;

static int CaptureState( drawState_t *s, WGPUPrimitiveTopology topology ) {
	wglTexture_t *t0, *t1;
	int full[4] = { 0, 0, wgl.width, wgl.height };

	if ( gls.cullFace && gls.cullFaceMode == GL_FRONT_AND_BACK && topology == WGPUPrimitiveTopology_TriangleList ) {
		return 0;
	}
	if ( !ToTargetRect( gls.viewport, s->viewport ) ) {
		return 0;
	}
	if ( gls.scissorTest ) {
		if ( !ToTargetRect( gls.scissor, s->scissor ) ) {
			return 0;
		}
	} else {
		memcpy( s->scissor, full, sizeof( full ) );
	}
	s->depthRange[0] = gls.depthNear < gls.depthFar ? gls.depthNear : gls.depthFar;
	s->depthRange[1] = gls.depthNear < gls.depthFar ? gls.depthFar : gls.depthNear;
	s->stencilRef = gls.stencilRef & 0xff;

	StateKey( &s->key, topology );
	StateUniforms( &s->uniforms );

	t0 = gls.texture2D[0] ? WGL_GetTexture( gls.bound[0] ) : NULL;
	t1 = gls.texture2D[1] ? WGL_GetTexture( gls.bound[1] ) : NULL;
	s->tex[0] = WGL_TextureBindGroup( t0 );
	s->tex[1] = WGL_TextureBindGroup( t1 );
	return 1;
}

// Issues the draw for indexes already written at [firstIndex, firstIndex + count).
static void Draw( const drawState_t *s, int firstIndex, int count ) {
	WGPURenderPipeline pipeline;
	int slot;
	uint32_t offset;

	if ( !pass ) {
		OpenPass();
	}

	pipeline = GetPipeline( &s->key );
	if ( pipeline != curPipeline ) {
		wgpuRenderPassEncoderSetPipeline( pass, pipeline );
		curPipeline = pipeline;
	}

	slot = PushUniforms( &s->uniforms );
	if ( slot != curUniformSlot ) {
		offset = (uint32_t)slot * WGL_UNIFORM_STRIDE;
		wgpuRenderPassEncoderSetBindGroup( pass, 0, uniformBindGroup, 1, &offset );
		curUniformSlot = slot;
	}
	if ( s->tex[0] != curTex[0] ) {
		wgpuRenderPassEncoderSetBindGroup( pass, 1, s->tex[0], 0, NULL );
		curTex[0] = s->tex[0];
	}
	if ( s->tex[1] != curTex[1] ) {
		wgpuRenderPassEncoderSetBindGroup( pass, 2, s->tex[1], 0, NULL );
		curTex[1] = s->tex[1];
	}
	if ( memcmp( s->viewport, curViewport, sizeof( curViewport ) )
		 || s->depthRange[0] != curDepthRange[0] || s->depthRange[1] != curDepthRange[1] ) {
		wgpuRenderPassEncoderSetViewport( pass, (float)s->viewport[0], (float)s->viewport[1],
										  (float)s->viewport[2], (float)s->viewport[3],
										  s->depthRange[0], s->depthRange[1] );
		memcpy( curViewport, s->viewport, sizeof( curViewport ) );
		curDepthRange[0] = s->depthRange[0];
		curDepthRange[1] = s->depthRange[1];
	}
	if ( memcmp( s->scissor, curScissor, sizeof( curScissor ) ) ) {
		wgpuRenderPassEncoderSetScissorRect( pass, s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3] );
		memcpy( curScissor, s->scissor, sizeof( curScissor ) );
	}
	if ( s->stencilRef != curStencilRef ) {
		wgpuRenderPassEncoderSetStencilReference( pass, s->stencilRef );
		curStencilRef = s->stencilRef;
	}

	wgpuRenderPassEncoderDrawIndexed( pass, count, 1, firstIndex, 0, 0 );
}

// Converts a GL primitive over vertices [0, n) into indexes for a WebGPU
// list topology. Returns the index count (0 if nothing to draw).
static int BuildIndexes( GLenum mode, int n, uint32_t *out, WGPUPrimitiveTopology *topology ) {
	int i, c = 0;
	int lines = ( gls.polygonMode == GL_LINE );

	switch ( mode ) {
	case GL_POINTS:
		*topology = WGPUPrimitiveTopology_PointList;
		for ( i = 0; i < n; i++ ) out[c++] = i;
		return c;
	case GL_LINES:
		*topology = WGPUPrimitiveTopology_LineList;
		for ( i = 0; i + 1 < n; i += 2 ) { out[c++] = i; out[c++] = i + 1; }
		return c;
	case GL_LINE_STRIP:
	case GL_LINE_LOOP:
		*topology = WGPUPrimitiveTopology_LineList;
		for ( i = 0; i + 1 < n; i++ ) { out[c++] = i; out[c++] = i + 1; }
		if ( mode == GL_LINE_LOOP && n > 2 ) { out[c++] = n - 1; out[c++] = 0; }
		return c;
	default:
		break;
	}

	*topology = lines ? WGPUPrimitiveTopology_LineList : WGPUPrimitiveTopology_TriangleList;
#define TRI( a, b, d ) do { \
		if ( lines ) { out[c++] = a; out[c++] = b; out[c++] = b; out[c++] = d; out[c++] = d; out[c++] = a; } \
		else { out[c++] = a; out[c++] = b; out[c++] = d; } \
	} while ( 0 )

	switch ( mode ) {
	case GL_TRIANGLES:
		for ( i = 0; i + 2 < n; i += 3 ) TRI( i, i + 1, i + 2 );
		break;
	case GL_TRIANGLE_STRIP:
	case GL_QUAD_STRIP:
		for ( i = 0; i + 2 < n; i++ ) {
			if ( i & 1 ) TRI( i + 1, i, i + 2 );
			else TRI( i, i + 1, i + 2 );
		}
		break;
	case GL_TRIANGLE_FAN:
	case GL_POLYGON:
		for ( i = 1; i + 1 < n; i++ ) TRI( 0, i, i + 1 );
		break;
	case GL_QUADS:
		for ( i = 0; i + 3 < n; i += 4 ) {
			TRI( i, i + 1, i + 2 );
			TRI( i, i + 2, i + 3 );
		}
		break;
	default:
		return 0;
	}
#undef TRI
	return c;
}

// Draws vertices that are already in GPU layout.
static void SubmitVertices( GLenum mode, const wglVertex_t *verts, int n ) {
	WGPUPrimitiveTopology topology;
	drawState_t s;
	uint32_t *idx;
	int count, i, first;

	if ( n <= 0 ) {
		return;
	}
	idx = ScratchIndexes( n * 6 + 6 );
	count = BuildIndexes( mode, n, idx, &topology );
	if ( !count || !Reserve( n, count ) || !CaptureState( &s, topology ) ) {
		return;
	}
	memcpy( cpuVerts + numVerts, verts, n * sizeof( wglVertex_t ) );
	first = numIndexes;
	for ( i = 0; i < count; i++ ) {
		cpuIndexes[numIndexes++] = idx[i] + numVerts;
	}
	numVerts += n;
	Draw( &s, first, count );
}

// ---------------------------------------------------------------------------
// client arrays

static void ArrayPointer( wglArray_t *a, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) {
	int elem = ( type == GL_UNSIGNED_BYTE ) ? 1 : ( type == GL_SHORT ) ? 2 : 4;
	a->size = size;
	a->type = type;
	a->stride = stride ? stride : size * elem;
	a->ptr = ptr;
}

void wgVertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) {
	ArrayPointer( &gls.vertexArray, size, type, stride, ptr );
}
void wgColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) {
	ArrayPointer( &gls.colorArray, size, type, stride, ptr );
}
void wgTexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) {
	ArrayPointer( &gls.texArray[gls.clientUnit], size, type, stride, ptr );
}
void wgNormalPointer( GLenum type, GLsizei stride, const GLvoid *ptr ) { }

static void FetchVertex( wglVertex_t *v, int i ) {
	const wglArray_t *a;
	int u;

	a = &gls.vertexArray;
	if ( a->ptr ) {
		const float *p = (const float *)( a->ptr + i * a->stride );
		v->xyz[0] = p[0];
		v->xyz[1] = p[1];
		v->xyz[2] = a->size > 2 ? p[2] : 0.0f;
	}

	a = &gls.colorArray;
	if ( a->enabled && a->ptr ) {
		if ( a->type == GL_UNSIGNED_BYTE ) {
			const uint8_t *p = a->ptr + i * a->stride;
			v->rgba[0] = p[0]; v->rgba[1] = p[1]; v->rgba[2] = p[2];
			v->rgba[3] = a->size > 3 ? p[3] : 255;
		} else {
			const float *p = (const float *)( a->ptr + i * a->stride );
			v->rgba[0] = FloatToByte( p[0] ); v->rgba[1] = FloatToByte( p[1] ); v->rgba[2] = FloatToByte( p[2] );
			v->rgba[3] = a->size > 3 ? FloatToByte( p[3] ) : 255;
		}
	} else {
		v->rgba[0] = FloatToByte( gls.color[0] ); v->rgba[1] = FloatToByte( gls.color[1] );
		v->rgba[2] = FloatToByte( gls.color[2] ); v->rgba[3] = FloatToByte( gls.color[3] );
	}

	for ( u = 0; u < WGL_UNITS; u++ ) {
		float *st = u ? v->st1 : v->st0;
		a = &gls.texArray[u];
		if ( a->enabled && a->ptr ) {
			const float *p = (const float *)( a->ptr + i * a->stride );
			st[0] = p[0];
			st[1] = p[1];
		} else {
			st[0] = gls.texCoord[u][0];
			st[1] = gls.texCoord[u][1];
		}
	}
}

void wgDrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices ) {
	const uint32_t *idx32 = indices;
	const uint16_t *idx16 = indices;
	WGPUPrimitiveTopology topology;
	drawState_t s;
	uint32_t minIndex = 0xffffffffu, maxIndex = 0, ix;
	int i, n, first, c;

	if ( count <= 0 || !gls.vertexArray.ptr ) {
		return;
	}

	for ( i = 0; i < count; i++ ) {
		ix = ( type == GL_UNSIGNED_INT ) ? idx32[i] : idx16[i];
		if ( ix < minIndex ) minIndex = ix;
		if ( ix > maxIndex ) maxIndex = ix;
	}
	n = (int)( maxIndex - minIndex + 1 );

	if ( mode == GL_TRIANGLES && gls.polygonMode != GL_LINE ) {
		topology = WGPUPrimitiveTopology_TriangleList;
		if ( !Reserve( n, count ) || !CaptureState( &s, topology ) ) {
			return;
		}
		for ( i = 0; i < n; i++ ) {
			FetchVertex( &cpuVerts[numVerts + i], (int)minIndex + i );
		}
		first = numIndexes;
		for ( i = 0; i < count; i++ ) {
			ix = ( type == GL_UNSIGNED_INT ) ? idx32[i] : idx16[i];
			cpuIndexes[numIndexes++] = ix - minIndex + numVerts;
		}
		numVerts += n;
		Draw( &s, first, count );
		return;
	}

	// uncommon modes: expand to a vertex list and convert
	{
		wglVertex_t *verts = malloc( count * sizeof( wglVertex_t ) );
		for ( i = 0; i < count; i++ ) {
			ix = ( type == GL_UNSIGNED_INT ) ? idx32[i] : idx16[i];
			FetchVertex( &verts[i], (int)ix );
		}
		c = count;
		SubmitVertices( mode, verts, c );
		free( verts );
	}
}

void wgDrawArrays( GLenum mode, GLint first, GLsizei count ) {
	wglVertex_t *verts;
	int i;

	if ( count <= 0 || !gls.vertexArray.ptr ) {
		return;
	}
	verts = malloc( count * sizeof( wglVertex_t ) );
	for ( i = 0; i < count; i++ ) {
		FetchVertex( &verts[i], first + i );
	}
	SubmitVertices( mode, verts, count );
	free( verts );
}

// ---------------------------------------------------------------------------
// immediate mode

void wgBegin( GLenum mode ) {
	immMode = mode;
	immCount = 0;
	inBeginEnd = 1;
}

void wgEnd( void ) {
	inBeginEnd = 0;
	SubmitVertices( immMode, immVerts, immCount );
	immCount = 0;
}

static void ImmVertex( float x, float y, float z ) {
	wglVertex_t *v;

	if ( immCount >= IMM_CAP ) {
		return;
	}
	v = &immVerts[immCount++];
	v->xyz[0] = x;
	v->xyz[1] = y;
	v->xyz[2] = z;
	v->rgba[0] = FloatToByte( gls.color[0] );
	v->rgba[1] = FloatToByte( gls.color[1] );
	v->rgba[2] = FloatToByte( gls.color[2] );
	v->rgba[3] = FloatToByte( gls.color[3] );
	v->st0[0] = gls.texCoord[0][0];
	v->st0[1] = gls.texCoord[0][1];
	v->st1[0] = gls.texCoord[1][0];
	v->st1[1] = gls.texCoord[1][1];
}

void wgVertex2f( GLfloat x, GLfloat y ) { ImmVertex( x, y, 0.0f ); }
void wgVertex2i( GLint x, GLint y ) { ImmVertex( (float)x, (float)y, 0.0f ); }
void wgVertex3f( GLfloat x, GLfloat y, GLfloat z ) { ImmVertex( x, y, z ); }
void wgVertex3fv( const GLfloat *v ) { ImmVertex( v[0], v[1], v[2] ); }

void wgArrayElement( GLint i ) {
	if ( immCount < IMM_CAP ) {
		FetchVertex( &immVerts[immCount++], i );
	}
}

void wgColor3f( GLfloat r, GLfloat g, GLfloat b ) {
	gls.color[0] = r; gls.color[1] = g; gls.color[2] = b; gls.color[3] = 1.0f;
}
void wgColor3fv( const GLfloat *v ) { wgColor3f( v[0], v[1], v[2] ); }
void wgColor4f( GLfloat r, GLfloat g, GLfloat b, GLfloat a ) {
	gls.color[0] = r; gls.color[1] = g; gls.color[2] = b; gls.color[3] = a;
}
void wgColor4fv( const GLfloat *v ) { wgColor4f( v[0], v[1], v[2], v[3] ); }
void wgColor4ubv( const GLubyte *v ) {
	wgColor4f( v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f );
}
void wgColor4ub( GLubyte r, GLubyte g, GLubyte b, GLubyte a ) {
	wgColor4f( r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f );
}

void wgTexCoord2f( GLfloat s, GLfloat t ) { gls.texCoord[0][0] = s; gls.texCoord[0][1] = t; }
void wgTexCoord2fv( const GLfloat *v ) { wgTexCoord2f( v[0], v[1] ); }

void wgMultiTexCoord2fARB( GLenum texture, GLfloat s, GLfloat t ) {
	// tr_shade.c passes 0/1 here rather than GL_TEXTUREn_ARB
	int unit = texture >= GL_TEXTURE0_ARB ? (int)texture - GL_TEXTURE0_ARB : (int)texture;
	if ( unit >= 0 && unit < WGL_UNITS ) {
		gls.texCoord[unit][0] = s;
		gls.texCoord[unit][1] = t;
	}
}

// ---------------------------------------------------------------------------
// clears

void wgClear( GLbitfield mask ) {
	int rect[4], full;
	int colorAll, clearColor, clearDepth, clearStencil;

	if ( inBeginEnd ) {
		return;
	}

	clearColor = ( mask & GL_COLOR_BUFFER_BIT ) && ( gls.colorMask[0] || gls.colorMask[1] || gls.colorMask[2] || gls.colorMask[3] );
	clearDepth = ( mask & GL_DEPTH_BUFFER_BIT ) && gls.depthMask;
	clearStencil = ( mask & GL_STENCIL_BUFFER_BIT ) && gls.stencilWriteMask;
	if ( !clearColor && !clearDepth && !clearStencil ) {
		return;
	}

	if ( gls.scissorTest ) {
		if ( !ToTargetRect( gls.scissor, rect ) ) {
			return;
		}
	} else {
		rect[0] = rect[1] = 0;
		rect[2] = wgl.width;
		rect[3] = wgl.height;
	}
	full = ( rect[0] == 0 && rect[1] == 0 && rect[2] == wgl.width && rect[3] == wgl.height );
	colorAll = gls.colorMask[0] && gls.colorMask[1] && gls.colorMask[2] && gls.colorMask[3];

	if ( full && ( !clearColor || colorAll ) && ( !clearStencil || gls.stencilWriteMask == 0xff ) ) {
		// becomes a load op on the next render pass
		WGL_EndPass();
		if ( clearColor ) {
			pending.color = 1;
			memcpy( pending.colorValue, gls.clearColor, sizeof( pending.colorValue ) );
		}
		if ( clearDepth ) {
			pending.depth = 1;
			pending.depthValue = gls.clearDepth;
		}
		if ( clearStencil ) {
			pending.stencil = 1;
			pending.stencilValue = gls.clearStencil;
		}
		return;
	}

	// partial clear: draw a quad over the scissor rect that overwrites the
	// requested aspects unconditionally
	{
		drawState_t s;
		wglVertex_t quad[4];
		float z = gls.clearDepth * 2.0f - 1.0f;     // undone by the vertex shader's z remap
		uint8_t col[4];
		int i, first;
		static const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
		static const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

		if ( !Reserve( 4, 6 ) ) {
			return;
		}
		memset( &s, 0, sizeof( s ) );
		s.key.topology = (uint8_t)WGPUPrimitiveTopology_TriangleList;
		s.key.cullMode = (uint8_t)WGPUCullMode_None;
		s.key.depthCompare = (uint8_t)WGPUCompareFunction_Always;
		s.key.depthWrite = (uint8_t)clearDepth;
		s.key.colorMask = clearColor ? ( ( gls.colorMask[0] ? WGPUColorWriteMask_Red : 0 )
										 | ( gls.colorMask[1] ? WGPUColorWriteMask_Green : 0 )
										 | ( gls.colorMask[2] ? WGPUColorWriteMask_Blue : 0 )
										 | ( gls.colorMask[3] ? WGPUColorWriteMask_Alpha : 0 ) ) : 0;
		s.key.stencilCompare = (uint8_t)WGPUCompareFunction_Always;
		s.key.stencilFail = s.key.stencilZFail = (uint8_t)WGPUStencilOperation_Keep;
		s.key.stencilPass = (uint8_t)( clearStencil ? WGPUStencilOperation_Replace : WGPUStencilOperation_Keep );
		s.key.stencilRead = 0xff;
		s.key.stencilWrite = clearStencil ? (uint8_t)gls.stencilWriteMask : 0;
		memcpy( s.uniforms.mvp, identity, sizeof( identity ) );
		memcpy( s.uniforms.mv, identity, sizeof( identity ) );
		s.tex[0] = s.tex[1] = WGL_TextureBindGroup( NULL );
		memcpy( s.viewport, rect, sizeof( rect ) );
		memcpy( s.scissor, rect, sizeof( rect ) );
		s.depthRange[0] = 0.0f;
		s.depthRange[1] = 1.0f;
		s.stencilRef = gls.clearStencil & 0xff;

		for ( i = 0; i < 4; i++ ) {
			col[i] = FloatToByte( gls.clearColor[i] );
		}
		for ( i = 0; i < 4; i++ ) {
			quad[i].xyz[0] = corners[i][0];
			quad[i].xyz[1] = corners[i][1];
			quad[i].xyz[2] = z;
			memcpy( quad[i].rgba, col, 4 );
			quad[i].st0[0] = quad[i].st0[1] = quad[i].st1[0] = quad[i].st1[1] = 0.0f;
		}
		memcpy( cpuVerts + numVerts, quad, sizeof( quad ) );
		first = numIndexes;
		cpuIndexes[numIndexes++] = numVerts + 0;
		cpuIndexes[numIndexes++] = numVerts + 1;
		cpuIndexes[numIndexes++] = numVerts + 2;
		cpuIndexes[numIndexes++] = numVerts + 0;
		cpuIndexes[numIndexes++] = numVerts + 2;
		cpuIndexes[numIndexes++] = numVerts + 3;
		numVerts += 4;
		Draw( &s, first, 6 );
	}
}

// Synchronous readback isn't possible in WebGPU; screenshots and the
// r_measureOverdraw stencil readback get zeroes.
void wgReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels ) {
	static int warned;
	int bpp = ( format == GL_RGBA ) ? 4 : ( format == GL_RGB ) ? 3 : ( format == GL_DEPTH_COMPONENT ) ? 4 : 1;

	if ( !warned ) {
		WGL_Printf( "wgl: glReadPixels is not supported by the WebGPU backend\n" );
		warned = 1;
	}
	memset( pixels, 0, (size_t)width * height * bpp );
}

// ---------------------------------------------------------------------------

static WGPUBuffer CreateBuffer( WGPUBufferUsage usage, uint64_t size, const char *label ) {
	WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
	d.label.data = label;
	d.label.length = WGPU_STRLEN;
	d.usage = usage | WGPUBufferUsage_CopyDst;
	d.size = size;
	return wgpuDeviceCreateBuffer( wgl.device, &d );
}

void WGL_InitDraw( void ) {
	WGPUBindGroupLayoutEntry le = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	WGPUBindGroupEntry be = WGPU_BIND_GROUP_ENTRY_INIT;
	WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	WGPUBindGroupLayout layouts[3];

	shaderModule = WGL_CreateShader( shaderSource, "wgl fixed function" );

	le.binding = 0;
	le.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
	le.buffer.type = WGPUBufferBindingType_Uniform;
	le.buffer.hasDynamicOffset = 1;
	le.buffer.minBindingSize = sizeof( wglUniforms_t );
	ld.entryCount = 1;
	ld.entries = &le;
	uniformLayout = wgpuDeviceCreateBindGroupLayout( wgl.device, &ld );

	layouts[0] = uniformLayout;
	layouts[1] = WGL_TextureBindGroupLayout();
	layouts[2] = WGL_TextureBindGroupLayout();
	pd.bindGroupLayoutCount = 3;
	pd.bindGroupLayouts = layouts;
	pipelineLayout = wgpuDeviceCreatePipelineLayout( wgl.device, &pd );

	vertexBuffer = CreateBuffer( WGPUBufferUsage_Vertex, (uint64_t)VERTEX_CAP * sizeof( wglVertex_t ), "wgl vertices" );
	indexBuffer = CreateBuffer( WGPUBufferUsage_Index, (uint64_t)INDEX_CAP * sizeof( uint32_t ), "wgl indexes" );
	uniformBuffer = CreateBuffer( WGPUBufferUsage_Uniform, (uint64_t)UNIFORM_CAP * WGL_UNIFORM_STRIDE, "wgl uniforms" );

	be.binding = 0;
	be.buffer = uniformBuffer;
	be.offset = 0;
	be.size = sizeof( wglUniforms_t );
	bd.layout = uniformLayout;
	bd.entryCount = 1;
	bd.entries = &be;
	uniformBindGroup = wgpuDeviceCreateBindGroup( wgl.device, &bd );

	cpuVerts = malloc( (size_t)VERTEX_CAP * sizeof( wglVertex_t ) );
	cpuIndexes = malloc( (size_t)INDEX_CAP * sizeof( uint32_t ) );
	cpuUniforms = calloc( UNIFORM_CAP, WGL_UNIFORM_STRIDE );
	immVerts = malloc( IMM_CAP * sizeof( wglVertex_t ) );
	WGL_ResetStreams();
	memset( &pending, 0, sizeof( pending ) );
}

void WGL_ShutdownDraw( void ) {
	int i;

	WGL_EndPass();
	for ( i = 0; i < PIPE_HASH; i++ ) {
		if ( pipeTable[i].pipeline ) {
			wgpuRenderPipelineRelease( pipeTable[i].pipeline );
		}
	}
	memset( pipeTable, 0, sizeof( pipeTable ) );
	numPipelines = 0;
	wgpuBindGroupRelease( uniformBindGroup );
	wgpuBufferRelease( vertexBuffer );
	wgpuBufferRelease( indexBuffer );
	wgpuBufferRelease( uniformBuffer );
	wgpuPipelineLayoutRelease( pipelineLayout );
	wgpuBindGroupLayoutRelease( uniformLayout );
	wgpuShaderModuleRelease( shaderModule );
	free( cpuVerts );
	free( cpuIndexes );
	free( cpuUniforms );
	free( immVerts );
	cpuVerts = NULL;
	cpuIndexes = NULL;
	cpuUniforms = NULL;
	immVerts = NULL;
}
