/*
 * web_js.h -- browser-side (JavaScript) helpers shared by the 2.60b and
 * ET: Legacy builds: relay socket, Web Audio, downloads, pointer lock,
 * console output and IndexedDB sync. Implemented in web_js.c.
 */
#ifndef WEB_JS_H
#define WEB_JS_H

typedef unsigned char webbyte_t;

void Sys_JS_SyncFS( void );
void Sys_JS_OpenURL( const char *url );
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

#endif
