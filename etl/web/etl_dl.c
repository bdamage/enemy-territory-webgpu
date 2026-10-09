/*
 * etl_dl.c -- HTTP downloads for ET: Legacy in the browser (replaces
 * src/qcommon/dl_main_curl.c).
 *
 * Server pk3 downloads (sv_wwwBaseURL) run through the shared fetch() helper
 * and the relay's /dl proxy (web_js.c, tools/server.mjs). Other web
 * requests (ET: Legacy's online services) aren't available and complete
 * immediately with REQUEST_NOK, which the engine handles as "offline".
 */
#include "../src/qcommon/q_shared.h"
#include "../src/qcommon/qcommon.h"
#include "../src/qcommon/dl_public.h"
#include "web_js.h"

static webRequest_t download;
static qboolean active;

unsigned int DL_BeginDownload( const char *localName, const char *remoteName, void *userData,
							   webCallbackFunc_t complete, webProgressCallbackFunc_t progress ) {
	if ( active ) {
		Com_Printf( S_COLOR_RED "DL_BeginDownload: Error - called with a download request already active\n" );
		return 0;
	}
	if ( !localName[0] || !remoteName[0] ) {
		Com_Printf( S_COLOR_RED "DL_BeginDownload: Error - empty download URL or empty local file name\n" );
		return 0;
	}
	if ( Q_stricmpn( remoteName, "http://", 7 ) && Q_stricmpn( remoteName, "https://", 8 ) ) {
		Com_Printf( S_COLOR_RED "DL_BeginDownload: Error - unsupported URL %s\n", remoteName );
		return 0;
	}
	if ( FS_CreatePath( localName ) ) {
		Com_Printf( S_COLOR_RED "DL_BeginDownload: Error - unable to create directory (%s).\n", localName );
		return 0;
	}

	Com_Memset( &download, 0, sizeof( download ) );
	download.id = FILE_DOWNLOAD_ID;
	download.userData = userData;
	download.complete_clb = complete;
	download.progress_clb = progress;
	Q_strncpyz( download.url, remoteName, sizeof( download.url ) );
	Q_strncpyz( download.data.name, localName, sizeof( download.data.name ) );

	if ( !DL_JS_Begin( remoteName ) ) {
		Com_Printf( S_COLOR_RED "DL_BeginDownload: Error - no relay server for HTTP downloads\n" );
		return 0;
	}
	active = qtrue;
	Cvar_Set( "cl_downloadName", remoteName );
	return download.id;
}

unsigned int Web_CreateRequest( const char *url, const char *authToken, webUploadData_t *upload, void *userData,
								webCallbackFunc_t complete, webProgressCallbackFunc_t progress ) {
	static webRequest_t request;

	Com_DPrintf( "Web_CreateRequest(%s): not available in the browser build\n", url );
	Com_Memset( &request, 0, sizeof( request ) );
	Q_strncpyz( request.url, url ? url : "", sizeof( request.url ) );
	request.userData = userData;
	request.uploadData = upload;
	if ( complete ) {
		complete( &request, REQUEST_NOK );
	}
	return 0;
}

void DL_DownloadLoop( void ) {
	int received = 0, state;
	webRequestResult result;

	if ( !active ) {
		return;
	}
	state = DL_JS_Poll( &received );
	if ( download.progress_clb ) {
		download.progress_clb( &download, (double)received, 0.0 );
	}
	if ( state == 0 && !download.abort ) {
		return;
	}

	active = qfalse;
	if ( download.abort ) {
		DL_JS_Cancel();
		result = REQUEST_ABORT;
	} else if ( state == 1 && DL_JS_Save( download.data.name ) ) {
		download.httpCode = 200;
		result = REQUEST_OK;
	} else {
		DL_JS_Cancel();
		Com_Printf( S_COLOR_RED "DL_DownloadLoop: Error - download of %s failed\n", download.url );
		result = REQUEST_NOK;
	}
	if ( download.complete_clb ) {
		download.complete_clb( &download, result );
	}
}

void DL_AbortAll( qboolean block, qboolean allowContinue ) {
	if ( active ) {
		download.abort = qtrue;
		DL_DownloadLoop();
	}
}

void DL_Shutdown( void ) {
	DL_AbortAll( qtrue, qfalse );
}
