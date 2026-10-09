/*
 * etl_stubs.c -- engine hooks without a browser counterpart.
 */
#include "../src/qcommon/q_shared.h"
#include "../src/qcommon/qcommon.h"

// no signals in a browser tab
void Sys_SigHandler( int signal ) {
}

// reading the clipboard needs an async permission prompt; pasting into the
// console isn't supported
char *IN_GetClipboardData( void ) {
	return NULL;
}

qboolean IN_IsNumLockDown( void ) {
	return qfalse;
}
