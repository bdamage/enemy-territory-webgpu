/*
 * etl_sys.c -- browser replacement for ET: Legacy's src/sys/sys_main.c.
 *
 * sys_unix.c (POSIX file/time helpers) is used as-is; this file provides the
 * rest: paths, console input from the page, printing/errors, the statically
 * linked game modules and a main loop driven by requestAnimationFrame
 * (fixed-rate game frames, see ../../web/web_frame.c).
 *
 * Filesystem (populated by web/pre.js before main):
 *   /et/etmain/pak0-2.pk3, /et/legacy/legacy_v2.86.1.pk3   fs_basepath = /et
 *   $HOME/.etlegacy (IndexedDB)                             fs_homepath
 */
#include <emscripten.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "../src/qcommon/q_shared.h"
#include "../src/qcommon/qcommon.h"
#include "../src/sys/sys_local.h"
#include "web_js.h"

#define WEB_BASEPATH    "/et"

static char binaryPath[MAX_OSPATH] = WEB_BASEPATH;
static char installPath[MAX_OSPATH] = WEB_BASEPATH;
static qboolean quitting;

// ---------------------------------------------------------------------------
// paths

void Sys_SetBinaryPath( const char *path ) {
	Q_strncpyz( binaryPath, path, sizeof( binaryPath ) );
}

char *Sys_BinaryPath( void ) {
	return binaryPath;
}

void Sys_SetDefaultInstallPath( const char *path ) {
	Q_strncpyz( installPath, path, sizeof( installPath ) );
}

char *Sys_DefaultInstallPath( void ) {
	return installPath;
}

qboolean Sys_WritePIDFile( void ) {
	// tabs never exit cleanly, so a crash marker would trigger every visit
	return qtrue;
}

// replaces sys_unix.c's: the engine clock is paced by web_frame.c
int64_t Sys_Microseconds( void ) {
	return WEB_Microseconds();
}

int Sys_FileTime( char *path ) {
	struct stat buf;

	if ( stat( path, &buf ) == -1 ) {
		return -1;
	}
	return buf.st_mtime;
}

// ---------------------------------------------------------------------------
// console

#define MAX_CONSOLE_INPUT 32
static char *consoleInput[MAX_CONSOLE_INPUT];
static int consoleHead, consoleTail;

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

void Sys_In_Restart_f( void ) {
	IN_Restart();
}

// Line-buffered, color codes stripped: the browser console is the output.
void Sys_Print( const char *msg ) {
	static char line[4096];
	static int len;
	int i;

	CON_LogWrite( msg );
	for ( i = 0; msg[i]; i++ ) {
		if ( Q_IsColorString( &msg[i] ) ) {
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

void Sys_AnsiColorPrint( const char *msg ) {
	Sys_Print( msg );
}

// ---------------------------------------------------------------------------
// lifecycle

void Sys_Init( void ) {
	Cmd_AddCommand( "in_restart", Sys_In_Restart_f, "Restarts all the input drivers." );
	Cvar_Set( "arch", OS_STRING " " ARCH_STRING );
	Cvar_Set( "username", Sys_GetCurrentUser() );
}

void Sys_Quit( void ) {
	if ( !quitting ) {
		quitting = qtrue;
		CL_Shutdown();
		Com_Shutdown( qfalse );
	}
	WEB_SyncFS();
	Sys_JS_Fatal( "ET: Legacy has quit." );
	emscripten_cancel_main_loop();
	emscripten_force_exit( 0 );
}

void QDECL Sys_Error( const char *error, ... ) {
	va_list argptr;
	char string[1024];

	va_start( argptr, error );
	Q_vsnprintf( string, sizeof( string ), error, argptr );
	va_end( argptr );

	Sys_JS_Fatal( string );
	emscripten_cancel_main_loop();
	emscripten_force_exit( 1 );
}

void Sys_ParseArgs( int argc, char **argv ) {
}

// ---------------------------------------------------------------------------
// statically linked game modules (see ../Makefile and tools/gen_prefix.sh)

typedef intptr_t ( QDECL *syscall_t )( intptr_t, ... );

#define VMMAIN_12 intptr_t command, intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4, intptr_t a5, \
	intptr_t a6, intptr_t a7, intptr_t a8, intptr_t a9, intptr_t a10, intptr_t a11
intptr_t cgame__vmMain( VMMAIN_12 );
intptr_t ui__vmMain( VMMAIN_12 );
intptr_t cgame283__vmMain( VMMAIN_12 );
intptr_t ui283__vmMain( VMMAIN_12 );
intptr_t qagame__vmMain( intptr_t command, intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4,
						 intptr_t a5, intptr_t a6 );
void cgame__dllEntry( syscall_t syscallptr );
void ui__dllEntry( syscall_t syscallptr );
void cgame283__dllEntry( syscall_t syscallptr );
void ui283__dllEntry( syscall_t syscallptr );
void qagame__dllEntry( syscall_t syscallptr );

// VM_EntryPoint_t takes 16 arguments; the modules' vmMain take fewer, and
// WebAssembly traps on signature mismatches, hence the adapters.
#define ENTRY_ARGS int cmd, intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4, intptr_t a5, \
	intptr_t a6, intptr_t a7, intptr_t a8, intptr_t a9, intptr_t a10, intptr_t a11, intptr_t a12, \
	intptr_t a13, intptr_t a14, intptr_t a15

static intptr_t QDECL CgameEntry( ENTRY_ARGS ) {
	return cgame__vmMain( cmd, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 );
}

static intptr_t QDECL UiEntry( ENTRY_ARGS ) {
	return ui__vmMain( cmd, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 );
}

static intptr_t QDECL Cgame283Entry( ENTRY_ARGS ) {
	return cgame283__vmMain( cmd, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 );
}

static intptr_t QDECL Ui283Entry( ENTRY_ARGS ) {
	return ui283__vmMain( cmd, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 );
}

static intptr_t QDECL QagameEntry( ENTRY_ARGS ) {
	return qagame__vmMain( cmd, a0, a1, a2, a3, a4, a5, a6 );
}

typedef struct {
	const char *name;
	const char *build;          // its symbol prefix (see ../Makefile, MODULES)
	int maxMinor;               // newest legacy mod v2.x it's used for
	VM_EntryPoint_t entry;
	void ( *dllEntry )( syscall_t );
} staticModule_t;

// Newest first. v2.84 changed the network protocol, so servers still on an
// older legacy mod get that release's client code, as native clients do by
// loading it from the server's mod pak.
static staticModule_t modules[] = {
	{ "cgame", "cgame", 99, CgameEntry, cgame__dllEntry },
	{ "cgame", "cgame283", 83, Cgame283Entry, cgame283__dllEntry },
	{ "ui", "ui", 99, UiEntry, ui__dllEntry },
	{ "ui", "ui283", 83, Ui283Entry, ui283__dllEntry },
	{ "qagame", "qagame", 99, QagameEntry, qagame__dllEntry },
};

static void WEB_RegisterModuleData( void ) {
	WEB_MODULE_DATA( cgame );
	WEB_MODULE_DATA( cgame283 );
	WEB_MODULE_DATA( ui );
	WEB_MODULE_DATA( ui283 );
	WEB_MODULE_DATA( qagame );
}

// The legacy mod release whose pak supplies the module's native binary
// (legacy_v2.83.2.pk3 -> 83), or 99 if it isn't a legacy pak.
static int ModPakMinor( const char *name ) {
	const char *pak = FS_PakContaining( va( Sys_GetDLLName( "%s" ), name ) );
	int major, minor;

	if ( pak && !Q_stricmpn( pak, "legacy_v", 8 ) && sscanf( pak + 8, "%d.%d", &major, &minor ) == 2 && major == 2 ) {
		return minor;
	}
	return 99;
}

void *Sys_LoadGameDll( const char *name, qboolean extract, VM_EntryPoint_t *entryPoint,
					   intptr_t ( *systemcalls )( intptr_t, ... ) ) {
	const char *gamedir = Cvar_VariableString( "fs_game" );
	staticModule_t *m = NULL;
	int i, minor;

	if ( gamedir[0] && Q_stricmp( gamedir, DEFAULT_MODGAME ) && !Q_stricmp( name, "cgame" ) ) {
		// Only the legacy mod's code is compiled in. (The ui falls back to
		// legacy's, as on desktop.) Return to the default mod first so the
		// menus shown with the error are the normal ones.
		char mod[MAX_QPATH];

		Q_strncpyz( mod, gamedir, sizeof( mod ) );
		Cvar_Set( "fs_game", DEFAULT_MODGAME );
		FS_PureServerSetLoadedPaks( "", "" );
		FS_Restart( 0 );
		Com_Error( ERR_DROP, "This server runs the mod \"%s\", which needs its own client code.\n"
				   "The browser version of ET: Legacy can only join servers running the legacy mod.", mod );
	}

	minor = Q_stricmp( name, "qagame" ) ? ModPakMinor( name ) : 99;
	for ( i = 0; i < (int)ARRAY_LEN( modules ); i++ ) {
		if ( !Q_stricmp( name, modules[i].name ) && minor <= modules[i].maxMinor ) {
			m = &modules[i];    // the oldest release that still covers this pak
		}
	}
	if ( !m ) {
		Com_Printf( "Sys_LoadGameDll(%s): no such module\n", name );
		return NULL;
	}

	Com_Printf( "Sys_LoadGameDll(%s): statically linked module (%s)\n", name, m->build );
	if ( Q_stricmp( name, "qagame" ) ) {
		// read the mod pk3's native cgame/ui binary once so the pak counts
		// as referenced for pure server checks (see Sys_GetDLLName in
		// qcommon.h)
		fileHandle_t f;
		FS_FOpenFileRead( va( Sys_GetDLLName( "%s" ), name ), &f, qfalse );
		if ( f ) {
			FS_FCloseFile( f );
		}
	}
	WEB_ModuleDataReset( m->build );    // a fresh copy, as a dlopen would give
	m->dllEntry( systemcalls );
	*entryPoint = m->entry;
	return m;
}

void *Sys_LoadDll( const char *name, qboolean useSystemLib ) {
	Com_Printf( "Sys_LoadDll(%s): dynamic libraries aren't available in the browser\n", name );
	return NULL;
}

void Sys_UnloadDll( void *dllHandle ) {
}

// ---------------------------------------------------------------------------
// main loop

static void Sys_Frame( void ) {
	static int lastSync;
	int now;

	WEB_RunFrames( Cvar_VariableIntegerValue( "com_maxfps" ), Com_Frame );

	now = Sys_Milliseconds();
	if ( now - lastSync > 5000 ) {
		lastSync = now;
		WEB_SyncFS();
	}
}

int main( int argc, char **argv ) {
	char commandLine[MAX_STRING_CHARS] = { 0 };
	int i;

	WEB_RegisterModuleData();

	for ( i = 1; i < argc; i++ ) {
		qboolean containsSpaces = strchr( argv[i], ' ' ) != NULL;

		if ( i > 1 ) {
			Q_strcat( commandLine, sizeof( commandLine ), " " );
		}
		if ( containsSpaces ) {
			Q_strcat( commandLine, sizeof( commandLine ), "\"" );
		}
		Q_strcat( commandLine, sizeof( commandLine ), argv[i] );
		if ( containsSpaces ) {
			Q_strcat( commandLine, sizeof( commandLine ), "\"" );
		}
	}
	Sys_Print( va( "command line: %s\n", commandLine ) );

	Sys_PlatformInit();
	Sys_Milliseconds();
	CON_Init();
	Com_Init( commandLine );
	NET_Init();

	emscripten_set_main_loop( Sys_Frame, 0, 0 );
	return 0;
}
