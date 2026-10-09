/*
 * etl_stubs.c -- engine hooks without a browser counterpart.
 */
#include "../src/qcommon/q_shared.h"
#include "../src/qcommon/qcommon.h"

// Translations need tinygettext (C++, not built here): text stays English.
qboolean doTranslateMod = qfalse;

void I18N_Init( void ) {
}

void I18N_SetLanguage( const char *language ) {
}

const char *I18N_Translate( const char *msgid ) {
	return msgid;
}

const char *I18N_TranslateMod( const char *msgid ) {
	return msgid;
}

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
