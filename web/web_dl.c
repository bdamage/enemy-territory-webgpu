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

static char dlLocalName[MAX_OSPATH];
static qboolean dlActive;

// state: 0 running, 1 done, 2 failed
EM_JS( int, DL_JS_Begin, ( const char *url ), {
	var relay = Module['etRelay'];
	var target = UTF8ToString( url );
	if ( !relay || typeof fetch === 'undefined' ) {
		return 0;
	}
	// ws(s)://host/relay -> http(s)://host/dl (no regex literals: EM_JS goes
	// through the C preprocessor, which mangles backslashes)
	var base = relay.indexOf( 'ws' ) === 0 ? 'http' + relay.substring( 2 ) : relay;
	if ( base.substring( base.length - 6 ) === '/relay' ) {
		base = base.substring( 0, base.length - 6 );
	}
	var proxy = base + '/dl?url=' + encodeURIComponent( target );
	var dl = Module['etDownload'] = { state: 0, received: 0, total: 0, data: null };
	fetch( proxy ).then( function( res ) {
		if ( !res.ok ) {
			throw new Error( 'HTTP ' + res.status );
		}
		dl.total = +res.headers.get( 'Content-Length' ) || 0;
		var reader = res.body.getReader();
		var chunks = [];
		function pump() {
			return reader.read().then( function( r ) {
				if ( r.done ) {
					var out = new Uint8Array( dl.received ), off = 0;
					for ( var i = 0; i < chunks.length; i++ ) {
						out.set( chunks[i], off );
						off += chunks[i].length;
					}
					dl.data = out;
					dl.state = 1;
					return;
				}
				chunks.push( r.value );
				dl.received += r.value.length;
				return pump();
			} );
		}
		return pump();
	} ).catch( function( e ) {
		console.warn( 'download failed: ' + target + ': ' + e.message );
		dl.state = 2;
	} );
	return 1;
} );

EM_JS( int, DL_JS_Poll, ( int *received ), {
	var dl = Module['etDownload'];
	if ( !dl ) {
		return 2;
	}
	HEAP32[received >> 2] = dl.received;
	return dl.state;
} );

// Writes the finished download to path; returns 1 on success.
EM_JS( int, DL_JS_Save, ( const char *path ), {
	var dl = Module['etDownload'];
	Module['etDownload'] = null;
	if ( !dl || !dl.data ) {
		return 0;
	}
	try {
		FS.writeFile( UTF8ToString( path ), dl.data );
		return 1;
	} catch ( e ) {
		console.warn( 'download write failed: ' + e );
		return 0;
	}
} );

EM_JS( void, DL_JS_Cancel, ( void ), {
	Module['etDownload'] = null;
} );

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
