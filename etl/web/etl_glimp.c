/*
 * etl_glimp.c -- GLimp_* for ET: Legacy (replaces src/sdl/sdl_glimp.c) on
 * the shared WebGPU GL emulation (../../web/wgl_core.c).
 *
 * The render size follows the canvas; window modes, fullscreen and the
 * splash image don't apply in a browser tab.
 */
#include "../src/client/client.h"
#include "../src/sys/sys_local.h"
#include "wgl_local.h"

static cvar_t *r_msaa, *r_renderScale, *r_bloom, *r_sharpen, *r_vibrance;

static void PrintHook( const char *msg ) {
	Com_Printf( "%s", msg );
}

// per-frame options (wgl_engine.h); r_msaa and r_renderScale apply at vid_restart
static void UpdateOptions( void ) {
	wglOptions.bloom = r_bloom->value;
	wglOptions.sharpen = r_sharpen->value;
	wglOptions.vibrance = r_vibrance->value;
}

void GLimp_Init( glconfig_t *glConfig, const char *glConfigString ) {
	int width, height;

	Com_Printf( "Initializing WebGPU renderer\n" );
	Cvar_Get( "r_mode", "-2", CVAR_ARCHIVE | CVAR_LATCH | CVAR_UNSAFE );
	Cvar_Get( "r_fullscreen", "0", CVAR_ARCHIVE | CVAR_LATCH | CVAR_UNSAFE );
	r_msaa = Cvar_Get( "r_msaa", "4", CVAR_ARCHIVE | CVAR_LATCH );
	Cvar_SetDescription( r_msaa, "Multisample anti-aliasing: 0 off, 4 samples (WebGPU)" );
	r_renderScale = Cvar_Get( "r_renderScale", "1", CVAR_ARCHIVE | CVAR_LATCH );
	Cvar_SetDescription( r_renderScale, "Render resolution relative to the window, 0.25 - 2 (WebGPU)" );
	r_bloom = Cvar_Get( "r_bloom", "0.5", CVAR_ARCHIVE );
	Cvar_SetDescription( r_bloom, "Bloom strength, 0 off (WebGPU)" );
	r_sharpen = Cvar_Get( "r_sharpen", "0.3", CVAR_ARCHIVE );
	Cvar_SetDescription( r_sharpen, "Contrast adaptive sharpening, 0 off - 1 (WebGPU)" );
	r_vibrance = Cvar_Get( "r_vibrance", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( r_vibrance, "Saturation boost for dull colors, 0 off (WebGPU)" );
	wglOptions.msaa = r_msaa->integer;
	wglOptions.renderScale = r_renderScale->value;
	UpdateOptions();

	WGL_PrintHook = PrintHook;
	WGL_DeviceInit( &width, &height );

	glConfig->vidWidth = width;
	glConfig->vidHeight = height;
	glConfig->windowWidth = width;
	glConfig->windowHeight = height;
	glConfig->windowAspect = (float)width / (float)height;
	glConfig->colorBits = 32;
	glConfig->depthBits = 24;
	glConfig->stencilBits = 8;
	glConfig->isFullscreen = qfalse;
	glConfig->displayFrequency = 60;
	glConfig->deviceSupportsGamma = qtrue;   // applied in the final blit

	if ( !re.InitOpenGLSubSystem() ) {
		Com_Error( ERR_VID_FATAL, "GLimp_Init() - could not initialise the WebGPU renderer\n" );
	}
	re.InitOpenGL();
	Cvar_Get( "r_availableModes", "", CVAR_ROM );

	IN_Init();
}

void GLimp_Shutdown( void ) {
	IN_Shutdown();
	WGL_DeviceShutdown();
}

void GLimp_EndFrame( void ) {
	WGL_Present();
	UpdateOptions();
}

void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] ) {
	WGL_SetGammaRamp( red, green, blue );
}

qboolean GLimp_SplashImage( qboolean ( *LoadSplashImage )( const char *name, byte *data, unsigned int size,
														   unsigned int width, unsigned int height, uint8_t bytes ) ) {
	return qfalse;
}

void *GLimp_MainWindow( void ) {
	return NULL;
}

void GLimp_Minimize( void ) {
}

void GLimp_FlashWindow( int state ) {
}
