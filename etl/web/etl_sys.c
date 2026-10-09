/*
 * etl_sys.c -- browser replacement for ET: Legacy's src/sys/sys_main.c.
 *
 * sys_unix.c (POSIX file/time helpers) is used as-is; this file provides the
 * rest: paths, console input from the page, printing/errors, the statically
 * linked game modules and a main loop driven by requestAnimationFrame.
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
intptr_t qagame__vmMain( intptr_t command, intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4,
						 intptr_t a5, intptr_t a6 );
void cgame__dllEntry( syscall_t syscallptr );
void ui__dllEntry( syscall_t syscallptr );
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

static intptr_t QDECL QagameEntry( ENTRY_ARGS ) {
	return qagame__vmMain( cmd, a0, a1, a2, a3, a4, a5, a6 );
}

typedef struct {
	const char *name;
	VM_EntryPoint_t entry;
	void ( *dllEntry )( syscall_t );
} staticModule_t;

static staticModule_t modules[] = {
	{ "cgame", CgameEntry, cgame__dllEntry },
	{ "ui", UiEntry, ui__dllEntry },
	{ "qagame", QagameEntry, qagame__dllEntry },
};

void *Sys_LoadGameDll( const char *name, qboolean extract, VM_EntryPoint_t *entryPoint,
					   intptr_t ( *systemcalls )( intptr_t, ... ) ) {
	const char *gamedir = Cvar_VariableString( "fs_game" );
	int i;

	if ( gamedir[0] && Q_stricmp( gamedir, DEFAULT_MODGAME ) ) {
		// only the legacy mod's code is compiled in
		Com_Printf( "Sys_LoadGameDll(%s): mod \"%s\" is not available in the browser build\n", name, gamedir );
		return NULL;
	}
	for ( i = 0; i < (int)ARRAY_LEN( modules ); i++ ) {
		if ( !Q_stricmp( name, modules[i].name ) ) {
			Com_Printf( "Sys_LoadGameDll(%s): statically linked module\n", name );
			modules[i].dllEntry( systemcalls );
			*entryPoint = modules[i].entry;
			return &modules[i];
		}
	}
	Com_Printf( "Sys_LoadGameDll(%s): no such module\n", name );
	return NULL;
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
	static int lastSync, lastFrame;
	int now = Sys_Milliseconds();
	int maxfps = Cvar_VariableIntegerValue( "com_maxfps" );

	// honour com_maxfps on high refresh rate displays by skipping frames
	// (Com_Frame no longer waits; see qcommon/common.c)
	if ( maxfps > 0 && now - lastFrame < 1000 / maxfps - 1 ) {
		return;
	}
	lastFrame = now;

	Com_Frame();

	if ( now - lastSync > 5000 ) {
		lastSync = now;
		WEB_SyncFS();
	}
}

int main( int argc, char **argv ) {
	char commandLine[MAX_STRING_CHARS] = { 0 };
	int i;

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
