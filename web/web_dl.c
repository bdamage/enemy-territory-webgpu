/*
 * web_dl.c -- HTTP ("www") downloads of server pk3s, replacing the curl
 * implementation (qcommon/dl_main_curl.c).
 *
 * Map download hosts don't send CORS headers, so requests go through the
 * relay server's /dl proxy (tools/server.mjs), derived from Module.etRelay.
 * The fetch runs asynchronously; DL_DownloadLoop polls it once per frame and
 * writes the file when it completes. On failure the engine tells the server
 * "wwwdl fail" and falls back to the in-band UDP download.
 */
#include <stdio.h>
#include <emscripten.h>

#include "../src/game/q_shared.h"
#include "../src/qcommon/qcommon.h"
#include "../src/qcommon/dl_public.h"
#include "web_js.h"

static char dlLocalName[MAX_OSPATH];
static qboolean dlActive;





void DL_InitDownload( void ) {
}

void DL_Shutdown( void ) {
	if ( dlActive ) {
		DL_JS_Cancel();
		dlActive = qfalse;
	}
}

int DL_BeginDownload( const char *localName, const char *remoteName, int debug ) {
	if ( dlActive ) {
		Com_Printf( "WARNING: DL_BeginDownload called while a download is in progress\n" );
		return 0;
	}
	if ( Q_strncmp( remoteName, "http://", 7 ) && Q_strncmp( remoteName, "https://", 8 ) ) {
		Com_Printf( "DL_BeginDownload: unsupported URL %s\n", remoteName );
		return 0;
	}
	if ( !DL_JS_Begin( remoteName ) ) {
		Com_Printf( "DL_BeginDownload: no relay server for HTTP downloads\n" );
		return 0;
	}
	Com_Printf( "URL: %s\n", remoteName );
	Q_strncpyz( dlLocalName, localName, sizeof( dlLocalName ) );
	FS_CreatePath( dlLocalName );
	Cvar_Set( "cl_downloadName", remoteName );
	Cvar_Set( "ui_dl_running", "1" );
	dlActive = qtrue;
	return 1;
}

dlStatus_t DL_DownloadLoop( void ) {
	int received = 0, state;

	if ( !dlActive ) {
		return DL_DONE;
	}
	state = DL_JS_Poll( &received );
	Cvar_SetValue( "cl_downloadCount", (float)received );
	if ( state == 0 ) {
		return DL_CONTINUE;
	}

	dlActive = qfalse;
	Cvar_Set( "ui_dl_running", "0" );
	if ( state == 1 && DL_JS_Save( dlLocalName ) ) {
		return DL_DONE;
	}
	DL_JS_Cancel();
	return DL_FAILED;
}
