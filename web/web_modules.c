/*
 * web_modules.c -- statically linked game modules.
 *
 * On desktop, qagame/cgame/ui are shared libraries loaded with dlopen. Here
 * they are compiled into the same wasm binary with their global symbols
 * prefixed (see tools/gen_prefix.sh), and Sys_LoadDll just hands out their
 * entry points.
 *
 * VM_Call invokes entryPoint through a variadic pointer, while the modules'
 * vmMain functions take a fixed number of ints. WebAssembly traps on call
 * signature mismatches, so each module gets a variadic adapter.
 */
#include <stdarg.h>
#include "../src/game/q_shared.h"
#include "../src/qcommon/qcommon.h"

typedef int ( *syscall_t )( int, ... );

int qagame__vmMain( int command, int arg0, int arg1, int arg2, int arg3, int arg4, int arg5, int arg6 );
void qagame__dllEntry( syscall_t syscallptr );
int cgame__vmMain( int command, int arg0, int arg1, int arg2, int arg3, int arg4, int arg5, int arg6,
				   int arg7, int arg8, int arg9, int arg10, int arg11 );
void cgame__dllEntry( syscall_t syscallptr );
int ui__vmMain( int command, int arg0, int arg1, int arg2, int arg3, int arg4, int arg5, int arg6,
				int arg7, int arg8, int arg9, int arg10, int arg11 );
void ui__dllEntry( syscall_t syscallptr );

#define READ_ARGS( a, cmd ) do { \
		va_list ap; int i_; \
		va_start( ap, cmd ); \
		for ( i_ = 0; i_ < 12; i_++ ) a[i_] = va_arg( ap, int ); \
		va_end( ap ); \
	} while ( 0 )

static int QDECL QagameEntry( int command, ... ) {
	int a[12];
	READ_ARGS( a, command );
	return qagame__vmMain( command, a[0], a[1], a[2], a[3], a[4], a[5], a[6] );
}

static int QDECL CgameEntry( int command, ... ) {
	int a[12];
	READ_ARGS( a, command );
	return cgame__vmMain( command, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11] );
}

static int QDECL UiEntry( int command, ... ) {
	int a[12];
	READ_ARGS( a, command );
	return ui__vmMain( command, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11] );
}

typedef struct {
	const char *name;
	int ( QDECL *entry )( int, ... );
	void ( *dllEntry )( syscall_t );
} staticModule_t;

static staticModule_t modules[] = {
	{ "qagame", QagameEntry, qagame__dllEntry },
	{ "cgame", CgameEntry, cgame__dllEntry },
	{ "ui", UiEntry, ui__dllEntry },
};

char *Sys_GetDLLName( const char *name );

void *QDECL Sys_LoadDll( const char *name, char *fqpath, int( QDECL **entryPoint ) ( int, ... ),
						 int ( QDECL *systemcalls )( int, ... ) ) {
	int i;

	*fqpath = 0;
	for ( i = 0; i < (int)( sizeof( modules ) / sizeof( modules[0] ) ); i++ ) {
		if ( !Q_stricmp( name, modules[i].name ) ) {
			Com_Printf( "Sys_LoadDll(%s): statically linked module\n", name );
			Q_strncpyz( fqpath, va( "static:%s", name ), MAX_QPATH );
#ifndef DEDICATED
			// Mods that ship their own cgame binary in their pk3s can't
			// run here: only the stock game code is compiled in.
			// Server-side-only mods fall back to it, as on desktop. (The
			// stock ui copes with mod menus, so it isn't checked -- and
			// erroring there would recurse during error recovery.)
			if ( i == 1 && FS_ModPakHasFile( Sys_GetDLLName( name ) ) ) {
				char mod[MAX_QPATH];

				// back to the base game first, so the menus shown with the
				// error (and afterwards) aren't the mod's
				Q_strncpyz( mod, Cvar_VariableString( "fs_game" ), sizeof( mod ) );
				Cvar_Set( "fs_game", "" );
				FS_PureServerSetLoadedPaks( "", "" );
				FS_Restart( 0 );
				Com_Error( ERR_DROP, "This server runs the mod \"%s\", which needs its own client code.\n"
						   "The browser version can only join servers running the standard game (etmain) "
						   "or server-side-only mods.", mod );
			}
			// Pure servers expect the client to have read its cgame/ui
			// binary from mp_bin.pk3; touching it marks that pak as
			// referenced so the checksums the client reports match.
			if ( i > 0 ) {
				fileHandle_t f;
				FS_FOpenFileRead( Sys_GetDLLName( name ), &f, qfalse );
				if ( f ) {
					FS_FCloseFile( f );
				}
			}
#endif
			modules[i].dllEntry( systemcalls );
			*entryPoint = modules[i].entry;
			return &modules[i];
		}
	}
	Com_Printf( "Sys_LoadDll(%s): no such module\n", name );
	return NULL;
}

// referenced by vm.c; only native (static) modules exist in this build
#include "../src/qcommon/vm_local.h"

void VM_Compile( vm_t *vm, vmHeader_t *header ) {
	Com_Error( ERR_FATAL, "VM_Compile: QVM compilation is not supported" );
}

int VM_CallCompiled( vm_t *vm, int *args ) {
	Com_Error( ERR_FATAL, "VM_CallCompiled: QVM compilation is not supported" );
	return 0;
}

void Sys_UnloadDll( void *dllHandle ) {
}

// the Linux names, which is what mp_bin.pk3 and pure servers know about
char *Sys_GetDLLName( const char *name ) {
	return va( "%s.mp.i386.so", name );
}
