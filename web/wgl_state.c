/*
 * wgl_state.c -- GL state tracking for the WebGPU fixed-function emulation.
 * Nothing here touches the GPU; draws read this state in wgl_draw.c.
 */
#include <math.h>
#include "wgl_local.h"

wglState_t gls;

static const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

void WGL_InitState( int width, int height ) {
	int i;

	memset( &gls, 0, sizeof( gls ) );
	gls.blendSrc = GL_ONE;
	gls.blendDst = GL_ZERO;
	gls.depthFunc = GL_LESS;
	gls.depthMask = 1;
	gls.colorMask[0] = gls.colorMask[1] = gls.colorMask[2] = gls.colorMask[3] = 1;
	gls.cullFaceMode = GL_BACK;
	gls.alphaFunc = GL_ALWAYS;
	gls.polygonMode = GL_FILL;
	gls.stencilFunc = GL_ALWAYS;
	gls.stencilFail = gls.stencilZFail = gls.stencilZPass = GL_KEEP;
	gls.stencilFuncMask = gls.stencilWriteMask = 0xff;
	gls.viewport[2] = gls.scissor[2] = width;
	gls.viewport[3] = gls.scissor[3] = height;
	gls.depthFar = 1.0f;
	gls.clearDepth = 1.0f;
	gls.fogMode = GL_EXP;
	gls.fogEnd = 1.0f;
	gls.fogDensity = 1.0f;
	gls.matrixMode = GL_MODELVIEW;
	memcpy( gls.modelview[0], identity, sizeof( identity ) );
	memcpy( gls.projection[0], identity, sizeof( identity ) );
	for ( i = 0; i < WGL_UNITS; i++ ) {
		memcpy( gls.texmat[i][0], identity, sizeof( identity ) );
		gls.texEnv[i] = GL_MODULATE;
	}
	gls.color[0] = gls.color[1] = gls.color[2] = gls.color[3] = 1.0f;
}

// ---------------------------------------------------------------------------
// capabilities

static void SetCap( GLenum cap, int on ) {
	switch ( cap ) {
	case GL_BLEND: gls.blend = on; break;
	case GL_DEPTH_TEST: gls.depthTest = on; break;
	case GL_CULL_FACE: gls.cullFace = on; break;
	case GL_ALPHA_TEST: gls.alphaTest = on; break;
	case GL_SCISSOR_TEST: gls.scissorTest = on; break;
	case GL_STENCIL_TEST: gls.stencilTest = on; break;
	case GL_FOG: gls.fog = on; break;
	case GL_CLIP_PLANE0: gls.clipPlane0 = on; break;
	case GL_POLYGON_OFFSET_FILL: gls.polyOffsetFill = on; break;
	case GL_POLYGON_OFFSET_LINE: gls.polyOffsetLine = on; break;
	case GL_TEXTURE_2D: gls.texture2D[gls.activeUnit] = on; break;
	default: break;     // dithering, line smoothing, PN triangles, ...
	}
}

void wgEnable( GLenum cap ) { SetCap( cap, 1 ); }
void wgDisable( GLenum cap ) { SetCap( cap, 0 ); }

GLboolean wgIsEnabled( GLenum cap ) {
	switch ( cap ) {
	case GL_BLEND: return gls.blend;
	case GL_DEPTH_TEST: return gls.depthTest;
	case GL_CULL_FACE: return gls.cullFace;
	case GL_ALPHA_TEST: return gls.alphaTest;
	case GL_SCISSOR_TEST: return gls.scissorTest;
	case GL_STENCIL_TEST: return gls.stencilTest;
	case GL_FOG: return gls.fog;
	case GL_TEXTURE_2D: return gls.texture2D[gls.activeUnit];
	default: return GL_FALSE;
	}
}

void wgEnableClientState( GLenum array ) {
	switch ( array ) {
	case GL_VERTEX_ARRAY: gls.vertexArray.enabled = 1; break;
	case GL_COLOR_ARRAY: gls.colorArray.enabled = 1; break;
	case GL_TEXTURE_COORD_ARRAY: gls.texArray[gls.clientUnit].enabled = 1; break;
	default: break;
	}
}

void wgDisableClientState( GLenum array ) {
	switch ( array ) {
	case GL_VERTEX_ARRAY: gls.vertexArray.enabled = 0; break;
	case GL_COLOR_ARRAY: gls.colorArray.enabled = 0; break;
	case GL_TEXTURE_COORD_ARRAY: gls.texArray[gls.clientUnit].enabled = 0; break;
	default: break;
	}
}

// ---------------------------------------------------------------------------
// fixed-function state

void wgBlendFunc( GLenum sfactor, GLenum dfactor ) { gls.blendSrc = sfactor; gls.blendDst = dfactor; }
void wgDepthFunc( GLenum func ) { gls.depthFunc = func; }
void wgDepthMask( GLboolean flag ) { gls.depthMask = flag ? 1 : 0; }
void wgCullFace( GLenum mode ) { gls.cullFaceMode = mode; }
void wgAlphaFunc( GLenum func, GLclampf ref ) { gls.alphaFunc = func; gls.alphaRef = ref; }
void wgPolygonOffset( GLfloat factor, GLfloat units ) { gls.polyFactor = factor; gls.polyUnits = units; }
void wgPolygonMode( GLenum face, GLenum mode ) { gls.polygonMode = mode; }

void wgColorMask( GLboolean r, GLboolean g, GLboolean b, GLboolean a ) {
	gls.colorMask[0] = r ? 1 : 0;
	gls.colorMask[1] = g ? 1 : 0;
	gls.colorMask[2] = b ? 1 : 0;
	gls.colorMask[3] = a ? 1 : 0;
}

void wgStencilFunc( GLenum func, GLint ref, GLuint mask ) {
	gls.stencilFunc = func;
	gls.stencilRef = ref;
	gls.stencilFuncMask = mask & 0xff;
}

void wgStencilOp( GLenum fail, GLenum zfail, GLenum zpass ) {
	gls.stencilFail = fail;
	gls.stencilZFail = zfail;
	gls.stencilZPass = zpass;
}

void wgStencilMask( GLuint mask ) { gls.stencilWriteMask = mask & 0xff; }

void wgViewport( GLint x, GLint y, GLsizei width, GLsizei height ) {
	gls.viewport[0] = x; gls.viewport[1] = y;
	gls.viewport[2] = width; gls.viewport[3] = height;
}

void wgScissor( GLint x, GLint y, GLsizei width, GLsizei height ) {
	gls.scissor[0] = x; gls.scissor[1] = y;
	gls.scissor[2] = width; gls.scissor[3] = height;
}

void wgDepthRange( GLclampd zNear, GLclampd zFar ) {
	gls.depthNear = (float)zNear;
	gls.depthFar = (float)zFar;
}

void wgClearColor( GLclampf r, GLclampf g, GLclampf b, GLclampf a ) {
	gls.clearColor[0] = r; gls.clearColor[1] = g;
	gls.clearColor[2] = b; gls.clearColor[3] = a;
}
void wgClearDepth( GLclampd depth ) { gls.clearDepth = (float)depth; }
void wgClearStencil( GLint s ) { gls.clearStencil = s; }

void wgTexEnvf( GLenum target, GLenum pname, GLfloat param ) {
	if ( target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE ) {
		gls.texEnv[gls.activeUnit] = (GLenum)param;
	}
}
void wgTexEnvi( GLenum target, GLenum pname, GLint param ) { wgTexEnvf( target, pname, (GLfloat)param ); }

void wgActiveTextureARB( GLenum texture ) {
	int unit = (int)texture - GL_TEXTURE0_ARB;
	if ( unit >= 0 && unit < WGL_UNITS ) {
		gls.activeUnit = unit;
	}
}

void wgClientActiveTextureARB( GLenum texture ) {
	int unit = (int)texture - GL_TEXTURE0_ARB;
	if ( unit >= 0 && unit < WGL_UNITS ) {
		gls.clientUnit = unit;
	}
}

// ---------------------------------------------------------------------------
// fog

void wgFogf( GLenum pname, GLfloat param ) {
	switch ( pname ) {
	case GL_FOG_MODE: gls.fogMode = (GLenum)param; break;
	case GL_FOG_DENSITY: gls.fogDensity = param; break;
	case GL_FOG_START: gls.fogStart = param; break;
	case GL_FOG_END: gls.fogEnd = param; break;
	case 0x855A: /* GL_FOG_DISTANCE_MODE_NV */
		gls.fogRadial = ( (GLenum)param == 0x855B /* GL_EYE_RADIAL_NV */ );
		break;
	default: break;
	}
}

void wgFogi( GLenum pname, GLint param ) { wgFogf( pname, (GLfloat)param ); }

void wgFogfv( GLenum pname, const GLfloat *params ) {
	if ( pname == GL_FOG_COLOR ) {
		memcpy( gls.fogColor, params, sizeof( gls.fogColor ) );
	} else {
		wgFogf( pname, params[0] );
	}
}

// ---------------------------------------------------------------------------
// matrices

void WGL_MatMul( float out[16], const float a[16], const float b[16] ) {
	float r[16];
	int i, j;

	for ( i = 0; i < 4; i++ ) {         // column
		for ( j = 0; j < 4; j++ ) {     // row
			r[i * 4 + j] = a[0 * 4 + j] * b[i * 4 + 0] + a[1 * 4 + j] * b[i * 4 + 1]
						 + a[2 * 4 + j] * b[i * 4 + 2] + a[3 * 4 + j] * b[i * 4 + 3];
		}
	}
	memcpy( out, r, sizeof( r ) );
}

static int Mat4Invert( const float m[16], float out[16] ) {
	float inv[16], det;
	int i;

	inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
	inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
	inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
	inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
	inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
	inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if ( det == 0.0f ) {
		return 0;
	}
	det = 1.0f / det;
	for ( i = 0; i < 16; i++ ) {
		out[i] = inv[i] * det;
	}
	return 1;
}

float *WGL_CurrentMatrix( void ) {
	switch ( gls.matrixMode ) {
	case GL_PROJECTION: return gls.projection[gls.projDepth];
	case GL_TEXTURE: return gls.texmat[gls.activeUnit][gls.texDepth[gls.activeUnit]];
	default: return gls.modelview[gls.mvDepth];
	}
}

void wgMatrixMode( GLenum mode ) { gls.matrixMode = mode; }
void wgLoadIdentity( void ) { memcpy( WGL_CurrentMatrix(), identity, sizeof( identity ) ); }
void wgLoadMatrixf( const GLfloat *m ) { memcpy( WGL_CurrentMatrix(), m, sizeof( float ) * 16 ); }

void wgMultMatrixf( const GLfloat *m ) {
	float *cur = WGL_CurrentMatrix();
	WGL_MatMul( cur, cur, m );
}

void wgPushMatrix( void ) {
	int *depth;
	float ( *stack )[16];

	switch ( gls.matrixMode ) {
	case GL_PROJECTION: depth = &gls.projDepth; stack = gls.projection; break;
	case GL_TEXTURE: depth = &gls.texDepth[gls.activeUnit]; stack = gls.texmat[gls.activeUnit]; break;
	default: depth = &gls.mvDepth; stack = gls.modelview; break;
	}
	if ( *depth + 1 < WGL_MATRIX_DEPTH ) {
		memcpy( stack[*depth + 1], stack[*depth], sizeof( float ) * 16 );
		( *depth )++;
	}
}

void wgPopMatrix( void ) {
	int *depth;

	switch ( gls.matrixMode ) {
	case GL_PROJECTION: depth = &gls.projDepth; break;
	case GL_TEXTURE: depth = &gls.texDepth[gls.activeUnit]; break;
	default: depth = &gls.mvDepth; break;
	}
	if ( *depth > 0 ) {
		( *depth )--;
	}
}

void wgTranslatef( GLfloat x, GLfloat y, GLfloat z ) {
	float t[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1 };
	wgMultMatrixf( t );
}

void wgScalef( GLfloat x, GLfloat y, GLfloat z ) {
	float s[16] = { x,0,0,0, 0,y,0,0, 0,0,z,0, 0,0,0,1 };
	wgMultMatrixf( s );
}

void wgRotatef( GLfloat angle, GLfloat x, GLfloat y, GLfloat z ) {
	float r[16], c, s, len, ic;

	len = sqrtf( x * x + y * y + z * z );
	if ( len == 0.0f ) {
		return;
	}
	x /= len; y /= len; z /= len;
	c = cosf( angle * (float)M_PI / 180.0f );
	s = sinf( angle * (float)M_PI / 180.0f );
	ic = 1.0f - c;
	r[0] = x * x * ic + c;     r[4] = x * y * ic - z * s; r[8] = x * z * ic + y * s;  r[12] = 0;
	r[1] = y * x * ic + z * s; r[5] = y * y * ic + c;     r[9] = y * z * ic - x * s;  r[13] = 0;
	r[2] = x * z * ic - y * s; r[6] = y * z * ic + x * s; r[10] = z * z * ic + c;     r[14] = 0;
	r[3] = 0;                  r[7] = 0;                  r[11] = 0;                  r[15] = 1;
	wgMultMatrixf( r );
}

void wgOrtho( GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f ) {
	float m[16];

	memset( m, 0, sizeof( m ) );
	m[0] = (float)( 2.0 / ( r - l ) );
	m[5] = (float)( 2.0 / ( t - b ) );
	m[10] = (float)( -2.0 / ( f - n ) );
	m[12] = (float)( -( r + l ) / ( r - l ) );
	m[13] = (float)( -( t + b ) / ( t - b ) );
	m[14] = (float)( -( f + n ) / ( f - n ) );
	m[15] = 1.0f;
	wgMultMatrixf( m );
}

void wgFrustum( GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f ) {
	float m[16];

	memset( m, 0, sizeof( m ) );
	m[0] = (float)( 2.0 * n / ( r - l ) );
	m[5] = (float)( 2.0 * n / ( t - b ) );
	m[8] = (float)( ( r + l ) / ( r - l ) );
	m[9] = (float)( ( t + b ) / ( t - b ) );
	m[10] = (float)( -( f + n ) / ( f - n ) );
	m[11] = -1.0f;
	m[14] = (float)( -2.0 * f * n / ( f - n ) );
	wgMultMatrixf( m );
}

// GL transforms the plane by the inverse of the current modelview at call time
void wgClipPlane( GLenum plane, const GLdouble *equation ) {
	float inv[16], p[4];
	int i;

	if ( plane != GL_CLIP_PLANE0 ) {
		return;
	}
	for ( i = 0; i < 4; i++ ) {
		p[i] = (float)equation[i];
	}
	if ( !Mat4Invert( gls.modelview[gls.mvDepth], inv ) ) {
		memcpy( gls.clipPlane, p, sizeof( p ) );
		return;
	}
	// row vector p times inv
	for ( i = 0; i < 4; i++ ) {
		gls.clipPlane[i] = p[0] * inv[i * 4 + 0] + p[1] * inv[i * 4 + 1] + p[2] * inv[i * 4 + 2] + p[3] * inv[i * 4 + 3];
	}
}

// ---------------------------------------------------------------------------
// queries

void wgGetIntegerv( GLenum pname, GLint *params ) {
	switch ( pname ) {
	case GL_MAX_TEXTURE_SIZE: params[0] = 4096; break;
	case GL_MAX_ACTIVE_TEXTURES_ARB: params[0] = WGL_UNITS; break;
	case GL_VIEWPORT: memcpy( params, gls.viewport, sizeof( gls.viewport ) ); break;
	case GL_DEPTH_BITS: params[0] = 24; break;
	case GL_STENCIL_BITS: params[0] = 8; break;
	case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: params[0] = 8; break;
	default: params[0] = 0; break;
	}
}

void wgGetFloatv( GLenum pname, GLfloat *params ) {
	switch ( pname ) {
	case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: params[0] = 16.0f; break;
	case GL_MODELVIEW_MATRIX: memcpy( params, gls.modelview[gls.mvDepth], sizeof( float ) * 16 ); break;
	case GL_PROJECTION_MATRIX: memcpy( params, gls.projection[gls.projDepth], sizeof( float ) * 16 ); break;
	default: params[0] = 0.0f; break;
	}
}

GLenum wgGetError( void ) { return GL_NO_ERROR; }

const GLubyte *wgGetString( GLenum name ) {
	switch ( name ) {
	case GL_VENDOR: return (const GLubyte *)"WebGPU";
	case GL_RENDERER: return (const GLubyte *)"WebGPU fixed-function emulation";
	case GL_VERSION: return (const GLubyte *)"1.3 WebGPU";
	case GL_EXTENSIONS: return (const GLubyte *)"GL_ARB_multitexture GL_EXT_compiled_vertex_array "
								"GL_EXT_texture_env_add GL_EXT_texture_filter_anisotropic GL_NV_fog_distance";
	default: return (const GLubyte *)"";
	}
}

// ---------------------------------------------------------------------------
// accepted and ignored

void wgShadeModel( GLenum mode ) { }
void wgHint( GLenum target, GLenum mode ) { }
void wgLineWidth( GLfloat width ) { }
void wgPointSize( GLfloat size ) { }
void wgFinish( void ) { }
void wgFlush( void ) { }
void wgDrawBuffer( GLenum mode ) { }
void wgReadBuffer( GLenum mode ) { }
void wgLockArraysEXT( GLint first, GLint count ) { }
void wgUnlockArraysEXT( void ) { }
void wgPNTrianglesiATI( GLenum pname, GLint param ) { }
void wgPNTrianglesfATI( GLenum pname, GLfloat param ) { }
void wgPushAttrib( GLbitfield mask ) { }
void wgPopAttrib( void ) { }
void wgListBase( GLuint base ) { }
void wgCallList( GLuint list ) { }
void wgCallLists( GLsizei n, GLenum type, const GLvoid *lists ) { }
void wgRasterPos3fv( const GLfloat *v ) { }
void wgPixelStorei( GLenum pname, GLint param ) { }
