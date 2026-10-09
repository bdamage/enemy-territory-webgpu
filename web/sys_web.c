/*
 * sys_web.c -- Sys_* platform layer for the browser (Emscripten).
 *
 * Filesystem layout (MEMFS/IDBFS, populated by web/pre.js before main):
 *   /et/etmain/pak*.pk3   game data   (fs_basepath = /et)
 *   /home/et           persistent  (fs_homepath, IndexedDB-backed)
 *
 * The engine runs one Com_Frame per requestAnimationFrame. Input and audio
 * live in web_input.c / web_snd.c.
 */
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include "../src/game/q_shared.h"
#include "../src/qcommon/qcommon.h"
#include "../src/client/client.h"
#include "web_local.h"

#define WEB_BASEPATH    "/et"
#define WEB_HOMEPATH    "/home/et"

static double sys_startTime;
static char sys_cmdline[MAX_STRING_CHARS];
static int sys_lastFrameTime;
static qboolean sys_quitting;

// console commands typed into the page's debug prompt
#define MAX_CONSOLE_INPUT 32
static char *consoleInput[MAX_CONSOLE_INPUT];
static int consoleHead, consoleTail;

// ---------------------------------------------------------------------------
// time

int Sys_Milliseconds( void ) {
	if ( sys_startTime == 0.0 ) {
		sys_startTime = emscripten_get_now();
	}
	return (int)( emscripten_get_now() - sys_startTime );
}

// ---------------------------------------------------------------------------
// filesystem

void Sys_Mkdir( const char *path ) {
	mkdir( path, 0777 );
}

char *strlwr( char *s ) {
	char *p = s;
	while ( p && *p ) {
		*p = tolower( *p );
		p++;
	}
	return s;
}

#define MAX_FOUND_FILES 0x1000

void Sys_ListFilteredFiles( const char *basedir, char *subdirs, char *filter, char **list, int *numfiles ) {
	char search[MAX_OSPATH], newsubdirs[MAX_OSPATH];
	char filename[MAX_OSPATH];
	DIR *fdir;
	struct dirent *d;
	struct stat st;

	if ( *numfiles >= MAX_FOUND_FILES - 1 ) {
		return;
	}
	if ( strlen( subdirs ) ) {
		Com_sprintf( search, sizeof( search ), "%s/%s", basedir, subdirs );
	} else {
		Com_sprintf( search, sizeof( search ), "%s", basedir );
	}
	if ( ( fdir = opendir( search ) ) == NULL ) {
		return;
	}
	while ( ( d = readdir( fdir ) ) != NULL ) {
		Com_sprintf( filename, sizeof( filename ), "%s/%s", search, d->d_name );
		if ( stat( filename, &st ) == -1 ) {
			continue;
		}
		if ( S_ISDIR( st.st_mode ) ) {
			if ( Q_stricmp( d->d_name, "." ) && Q_stricmp( d->d_name, ".." ) ) {
				if ( strlen( subdirs ) ) {
					Com_sprintf( newsubdirs, sizeof( newsubdirs ), "%s/%s", subdirs, d->d_name );
				} else {
					Com_sprintf( newsubdirs, sizeof( newsubdirs ), "%s", d->d_name );
				}
				Sys_ListFilteredFiles( basedir, newsubdirs, filter, list, numfiles );
			}
		}
		if ( *numfiles >= MAX_FOUND_FILES - 1 ) {
			break;
		}
		Com_sprintf( filename, sizeof( filename ), "%s/%s", subdirs, d->d_name );
		if ( !Com_FilterPath( filter, filename, qfalse ) ) {
			continue;
		}
		list[*numfiles] = CopyString( filename );
		( *numfiles )++;
	}
	closedir( fdir );
}

char **Sys_ListFiles( const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs ) {
	struct dirent *d;
	DIR *fdir;
	qboolean dironly = wantsubs;
	char search[MAX_OSPATH];
	int nfiles, i, isdir;
	char **listCopy;
	char *list[MAX_FOUND_FILES];
	struct stat st;

	if ( filter ) {
		nfiles = 0;
		Sys_ListFilteredFiles( directory, "", filter, list, &nfiles );
		list[nfiles] = 0;
		*numfiles = nfiles;
		if ( !nfiles ) {
			return NULL;
		}
		listCopy = Z_Malloc( ( nfiles + 1 ) * sizeof( *listCopy ) );
		for ( i = 0; i < nfiles; i++ ) {
			listCopy[i] = list[i];
		}
		listCopy[i] = NULL;
		return listCopy;
	}

	if ( !extension ) {
		extension = "";
	}
	if ( extension[0] == '/' && extension[1] == 0 ) {
		extension = "";
		dironly = qtrue;
	}

	nfiles = 0;
	if ( ( fdir = opendir( directory ) ) == NULL ) {
		*numfiles = 0;
		return NULL;
	}
	while ( ( d = readdir( fdir ) ) != NULL ) {
		Com_sprintf( search, sizeof( search ), "%s/%s", directory, d->d_name );
		if ( stat( search, &st ) == -1 ) {
			continue;
		}
		isdir = S_ISDIR( st.st_mode );
		if ( ( dironly && !isdir ) || ( !dironly && isdir ) ) {
			continue;
		}
		if ( *extension ) {
			if ( strlen( d->d_name ) < strlen( extension )
				 || Q_stricmp( d->d_name + strlen( d->d_name ) - strlen( extension ), extension ) ) {
				continue;
			}
		}
		if ( nfiles == MAX_FOUND_FILES - 1 ) {
			break;
		}
		list[nfiles] = CopyString( d->d_name );
		nfiles++;
	}
	list[nfiles] = 0;
	closedir( fdir );

	*numfiles = nfiles;
	if ( !nfiles ) {
		return NULL;
	}
	listCopy = Z_Malloc( ( nfiles + 1 ) * sizeof( *listCopy ) );
	for ( i = 0; i < nfiles; i++ ) {
		listCopy[i] = list[i];
	}
	listCopy[i] = NULL;
	return listCopy;
}

void Sys_FreeFileList( char **list ) {
	int i;

	if ( !list ) {
		return;
	}
	for ( i = 0; list[i]; i++ ) {
		Z_Free( list[i] );
	}
	Z_Free( list );
}

int Sys_FileTime( char *path ) {
	struct stat buf;

	if ( stat( path, &buf ) == -1 ) {
		return -1;
	}
	return buf.st_mtime;
}

void Sys_Chmod( char *file, int mode ) {
}

char *Sys_Cwd( void ) {
	return WEB_BASEPATH;
}

void Sys_SetDefaultCDPath( const char *path ) {
}

char *Sys_DefaultCDPath( void ) {
	return "";
}

char *Sys_DefaultBasePath( void ) {
	return WEB_BASEPATH;
}

void Sys_SetDefaultInstallPath( const char *path ) {
}

char *Sys_DefaultInstallPath( void ) {
	return WEB_BASEPATH;
}

void Sys_SetDefaultHomePath( const char *path ) {
}

char *Sys_DefaultHomePath( void ) {
	return WEB_HOMEPATH;
}

// Persist fs_homepath (configs, profiles) to IndexedDB.
EM_JS( void, Sys_JS_SyncFS, ( void ), {
	if ( Module['syncingFS'] ) {
		Module['syncAgain'] = true;
		return;
	}
	Module['syncingFS'] = true;
	FS.syncfs( false, function( err ) {
		Module['syncingFS'] = false;
		if ( err ) {
			console.warn( 'IDBFS sync failed', err );
		}
		if ( Module['syncAgain'] ) {
			Module['syncAgain'] = false;
			_Sys_SyncFS();
		}
	} );
} );

EMSCRIPTEN_KEEPALIVE void Sys_SyncFS( void ) {
	Sys_JS_SyncFS();
}

// ---------------------------------------------------------------------------
// background file streaming (synchronous here)

void Sys_InitStreamThread( void ) {
}

void Sys_ShutdownStreamThread( void ) {
}

void Sys_BeginStreamedFile( fileHandle_t f, int readAhead ) {
}

void Sys_EndStreamedFile( fileHandle_t f ) {
}

int Sys_StreamedRead( void *buffer, int size, int count, fileHandle_t f ) {
	return FS_Read( buffer, size * count, f );
}

void Sys_StreamSeek( fileHandle_t f, int offset, int origin ) {
	FS_Seek( f, offset, origin );
}

// ---------------------------------------------------------------------------
// misc

// matches the x87 fistp (round-to-nearest-even) snapping of the original
void Sys_SnapVector( float *v ) {
	v[0] = rintf( v[0] );
	v[1] = rintf( v[1] );
	v[2] = rintf( v[2] );
}

qboolean Sys_LowPhysicalMemory( void ) {
	return qfalse;
}

void Sys_BeginProfiling( void ) {
}

float Sys_GetCPUSpeed( void ) {
	return 2000.0f;
}

void Sys_ShowConsole( int visLevel, qboolean quitOnClose ) {
}

char *Sys_GetCurrentUser( void ) {
	return "player";
}

void *Sys_InitializeCriticalSection( void ) {
	return (void *)-1;
}

void Sys_EnterCriticalSection( void *ptr ) {
}

void Sys_LeaveCriticalSection( void *ptr ) {
}

qboolean Sys_CheckCD( void ) {
	return qtrue;
}

void Sys_AppActivate( void ) {
}

char *Sys_GetClipboardData( void ) {
	return NULL;
}

qboolean Sys_IsNumLockDown( void ) {
	return qfalse;
}

void Sys_ConfigureFPU( void ) {
}

void Sys_PrintBinVersion( const char *name ) {
}

void Sys_StartProcess( char *cmdline, qboolean doexit ) {
	Com_Printf( "Sys_StartProcess not supported: %s\n", cmdline );
}

EM_JS( void, Sys_JS_OpenURL, ( const char *url ), {
	window.open( UTF8ToString( url ), '_blank', 'noopener' );
} );

void Sys_OpenURL( const char *url, qboolean doexit ) {
	Sys_JS_OpenURL( url );
}

EM_JS( void, Sys_JS_Print, ( const char *msg ), {
	var s = UTF8ToString( msg );
	if ( Module['print'] ) {
		Module['print']( s );
	} else {
		console.log( s );
	}
} );

// Line-buffered: the engine prints partial lines, the browser console doesn't.
void Sys_Print( const char *msg ) {
	static char line[MAXPRINTMSG];
	static int len;
	int i;

	for ( i = 0; msg[i]; i++ ) {
		if ( Q_IsColorString( &msg[i] ) ) {     // strip Q3 color codes
			i++;
			continue;
		}
		if ( msg[i] == '\n' || len == (int)sizeof( line ) - 1 ) {
			line[len] = 0;
			Sys_JS_Print( line );
			len = 0;
			continue;
		}
		line[len++] = msg[i];
	}
}

EM_JS( void, Sys_JS_Fatal, ( const char *msg ), {
	var s = UTF8ToString( msg );
	console.error( s );
	if ( Module['onFatalError'] ) {
		Module['onFatalError']( s );
	}
} );

void Sys_Exit( int ex ) {
	emscripten_cancel_main_loop();
	Sys_SyncFS();
	emscripten_force_exit( ex );
}

void Sys_Quit( void ) {
	if ( sys_quitting ) {
		return;
	}
	sys_quitting = qtrue;
	CL_Shutdown();
	Sys_JS_Fatal( "Enemy Territory has quit." );
	Sys_Exit( 0 );
}

void Sys_Error( const char *error, ... ) {
	va_list argptr;
	char string[1024];

	va_start( argptr, error );
	Q_vsnprintf( string, sizeof( string ), error, argptr );
	va_end( argptr );

	Sys_JS_Fatal( string );
	emscripten_cancel_main_loop();
	emscripten_force_exit( 1 );
}

void Sys_Warn( char *warning, ... ) {
	va_list argptr;
	char string[1024];

	va_start( argptr, warning );
	Q_vsnprintf( string, sizeof( string ), warning, argptr );
	va_end( argptr );
	Sys_Print( va( "Warning: %s", string ) );
}

void Sys_In_Restart_f( void );

void Sys_Init( void ) {
	Cmd_AddCommand( "in_restart", Sys_In_Restart_f );
	Cvar_Set( "arch", "web-wasm32" );
	Cvar_Set( "username", Sys_GetCurrentUser() );
	IN_Init();
}

void Sys_In_Restart_f( void ) {
	IN_Shutdown();
	IN_Init();
}

// ---------------------------------------------------------------------------
// console input from the page

EMSCRIPTEN_KEEPALIVE void WEB_ConsoleCommand( const char *text ) {
	if ( consoleHead - consoleTail >= MAX_CONSOLE_INPUT ) {
		return;
	}
	consoleInput[consoleHead & ( MAX_CONSOLE_INPUT - 1 )] = strdup( text );
	consoleHead++;
}

char *Sys_ConsoleInput( void ) {
	static char text[MAX_STRING_CHARS];
	char *s;

	if ( consoleHead == consoleTail ) {
		return NULL;
	}
	s = consoleInput[consoleTail & ( MAX_CONSOLE_INPUT - 1 )];
	consoleTail++;
	Q_strncpyz( text, s, sizeof( text ) );
	free( s );
	return text;
}

// ---------------------------------------------------------------------------
// event queue

#define MAX_QUED_EVENTS     256
#define MASK_QUED_EVENTS    ( MAX_QUED_EVENTS - 1 )

static sysEvent_t eventQue[MAX_QUED_EVENTS];
static int eventHead, eventTail;
static byte sys_packetReceived[MAX_MSGLEN];

void Sys_QueEvent( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr ) {
	sysEvent_t *ev;

	ev = &eventQue[eventHead & MASK_QUED_EVENTS];
	if ( eventHead - eventTail >= MAX_QUED_EVENTS ) {
		Com_Printf( "Sys_QueEvent: overflow\n" );
		if ( ev->evPtr ) {
			Z_Free( ev->evPtr );
		}
		eventTail++;
	}
	eventHead++;

	if ( time == 0 ) {
		time = Sys_Milliseconds();
	}
	ev->evTime = time;
	ev->evType = type;
	ev->evValue = value;
	ev->evValue2 = value2;
	ev->evPtrLength = ptrLength;
	ev->evPtr = ptr;
}

void Sys_SendKeyEvents( void ) {
	// browser input events are queued as they arrive (web_input.c)
}

sysEvent_t Sys_GetEvent( void ) {
	sysEvent_t ev;
	char *s;
	msg_t netmsg;
	netadr_t adr;

	if ( eventHead > eventTail ) {
		eventTail++;
		return eventQue[( eventTail - 1 ) & MASK_QUED_EVENTS];
	}

	s = Sys_ConsoleInput();
	if ( s ) {
		char *b;
		int len;

		len = strlen( s ) + 1;
		b = Z_Malloc( len );
		strcpy( b, s );
		Sys_QueEvent( 0, SE_CONSOLE, 0, 0, len, b );
	}

	IN_Frame();

	MSG_Init( &netmsg, sys_packetReceived, sizeof( sys_packetReceived ) );
	if ( Sys_GetPacket( &adr, &netmsg ) ) {
		netadr_t *buf;
		int len;

		len = sizeof( netadr_t ) + netmsg.cursize;
		buf = Z_Malloc( len );
		*buf = adr;
		memcpy( buf + 1, netmsg.data, netmsg.cursize );
		Sys_QueEvent( 0, SE_PACKET, 0, 0, len, buf );
	}

	if ( eventHead > eventTail ) {
		eventTail++;
		return eventQue[( eventTail - 1 ) & MASK_QUED_EVENTS];
	}

	memset( &ev, 0, sizeof( ev ) );
	ev.evTime = Sys_Milliseconds();
	return ev;
}

// ---------------------------------------------------------------------------
// main loop

extern cvar_t *com_maxfps;

static void Sys_Frame( void ) {
	static int lastSync;
	int now = Sys_Milliseconds();

	// Com_Frame would busy-wait to honour com_maxfps; skip the frame instead
	// and let the browser call us again on the next animation frame
	if ( com_maxfps && com_maxfps->integer > 0 ) {
		int minMsec = 1000 / com_maxfps->integer;
		if ( now - sys_lastFrameTime < minMsec - 1 ) {
			return;
		}
	}
	sys_lastFrameTime = now;

	Com_Frame();

	if ( now - lastSync > 5000 ) {
		lastSync = now;
		Sys_SyncFS();
	}
}

int main( int argc, char **argv ) {
	int i;

	sys_cmdline[0] = 0;
	for ( i = 1; i < argc; i++ ) {
		if ( i > 1 ) {
			Q_strcat( sys_cmdline, sizeof( sys_cmdline ), " " );
		}
		Q_strcat( sys_cmdline, sizeof( sys_cmdline ), argv[i] );
	}

	Sys_Milliseconds();
	memset( eventQue, 0, sizeof( eventQue ) );

	Sys_Print( va( "command line: %s\n", sys_cmdline ) );
	Com_Init( sys_cmdline );
	NET_Init();

	emscripten_set_main_loop( Sys_Frame, 0, 0 );
	return 0;
}
