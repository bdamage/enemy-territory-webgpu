/*
 * glimp_vanilla.c -- GLimp_* for the ET 2.60b renderer (src/renderer) on
 * top of the WebGPU GL emulation (wgl_core.c).
 */
#include "../src/renderer/tr_local.h"
#include "wgl_local.h"

int gl_NormalFontBase = 0;     // display-list font for r_debugSurface text (unsupported)

static void PrintHook( const char *msg ) {
	ri.Printf( PRINT_ALL, "%s", msg );
}

void GLimp_Init( void ) {
	int width, height;

	ri.Printf( PRINT_ALL, "Initializing WebGPU renderer\n" );
	WGL_PrintHook = PrintHook;
	WGL_DeviceInit( &width, &height );

	glConfig.vidWidth = width;
	glConfig.vidHeight = height;
	glConfig.windowAspect = (float)width / (float)height;
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
}

void GLimp_Shutdown( void ) {
	WGL_DeviceShutdown();
	memset( &glConfig, 0, sizeof( glConfig ) );
}

void GLimp_EndFrame( void ) {
	WGL_Present();
}

void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] ) {
	WGL_SetGammaRamp( red, green, blue );
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
