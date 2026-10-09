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

static void PrintHook( const char *msg ) {
	Com_Printf( "%s", msg );
}

void GLimp_Init( glconfig_t *glConfig, const char *glConfigString ) {
	int width, height;

	Com_Printf( "Initializing WebGPU renderer\n" );
	Cvar_Get( "r_mode", "-2", CVAR_ARCHIVE | CVAR_LATCH | CVAR_UNSAFE );
	Cvar_Get( "r_fullscreen", "0", CVAR_ARCHIVE | CVAR_LATCH | CVAR_UNSAFE );

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
