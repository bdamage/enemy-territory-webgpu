/*
 * web_js.h -- browser platform helpers shared by the 2.60b and ET: Legacy
 * builds. The JavaScript halves (relay socket, Web Audio, downloads, pointer
 * lock, console output, IndexedDB sync) are in web_js.c; frame pacing and
 * loading screens in web_frame.c; game module data in web_module_data.c.
 */
#ifndef WEB_JS_H
#define WEB_JS_H

typedef unsigned char webbyte_t;

void Sys_JS_SyncFS( void );
void Sys_JS_OpenURL( const char *url );
void WEB_OfferFile( const char *name, const void *data, int len );
void Sys_JS_Print( const char *msg );
void Sys_JS_Fatal( const char *msg );
void IN_JS_RequestPointerLock( void );
int SND_JS_Init( webbyte_t *buffer, int frames, int rate );
int SND_JS_GetPos( void );
void SND_JS_Shutdown( void );
int NET_JS_Open( void );
void NET_JS_Close( void );
void NET_JS_Send( const webbyte_t *data, int len );
int NET_JS_Recv( webbyte_t *buf, int max );
int DL_JS_Begin( const char *url );
int DL_JS_Poll( int *received );
int DL_JS_Save( const char *path );
void DL_JS_Cancel( void );

// IndexedDB sync of the home directory (also callable from JS as _WEB_SyncFS)
void WEB_SyncFS( void );

// fixed-rate game frames (web_frame.c)
#include <stdint.h>
extern int web_skipRender;          // set while running a frame that shouldn't render
extern int web_frameUpdate;         // set around CL_Frame's own screen update
int64_t WEB_Microseconds( void );   // engine clock
void WEB_RunFrames( int maxfps, void ( *frame )( void ) );
void WEB_LoadingUpdate( void );     // after drawing a loading screen
int WEB_JS_CanSuspend( void );
void WEB_JS_WaitForRefresh( void );

// fresh variables for each game module load (web_module_data.c). Each
// build registers its modules at startup with WEB_MODULE_DATA( prefix ).
void WEB_ModuleDataRegister( const char *name, char *dataBegin, char *dataEnd, char *bssBegin, char *bssEnd );
void WEB_ModuleDataReset( const char *name );
#define WEB_MODULE_DATA( m ) do { \
		extern char m ## _data_begin[], m ## _data_end[], m ## _bss_begin[], m ## _bss_end[]; \
		WEB_ModuleDataRegister( #m, m ## _data_begin, m ## _data_end, m ## _bss_begin, m ## _bss_end ); \
	} while ( 0 )

#endif
