/*
 * web_module_data.c -- gives each game module load fresh variables.
 *
 * Natively every cgame/ui/qagame load is a new dlopen, so the module starts
 * with its initial globals and statics. Linked into the engine, the modules'
 * variables would carry over from the previous load instead (ET: Legacy's
 * loading screen, for one, then skips registering its fonts on the second
 * map and crashes). The modules' .data and .bss lie between the markers from
 * module_bounds.c: WEB_ModuleDataRegister copies the initial .data at startup,
 * and WEB_ModuleDataReset puts it back and clears .bss before each load.
 */
#include <stdlib.h>
#include <string.h>
#include "web_js.h"

#define MAX_MODULES 8

typedef struct {
	const char *name;
	char *dataBegin, *dataEnd, *bssBegin, *bssEnd;
	void *initialData;
} moduleData_t;

static moduleData_t modules[MAX_MODULES];
static int numModules;

// Called at startup, before any module code runs (see WEB_MODULE_DATA).
void WEB_ModuleDataRegister( const char *name, char *dataBegin, char *dataEnd, char *bssBegin, char *bssEnd ) {
	moduleData_t *m;

	if ( numModules == MAX_MODULES ) {
		return;
	}
	m = &modules[numModules++];
	m->name = name;
	m->dataBegin = dataBegin;
	m->dataEnd = dataEnd;
	m->bssBegin = bssBegin;
	m->bssEnd = bssEnd;
	m->initialData = malloc( dataEnd - dataBegin );
	memcpy( m->initialData, dataBegin, dataEnd - dataBegin );
}

void WEB_ModuleDataReset( const char *name ) {
	int i;

	for ( i = 0; i < numModules; i++ ) {
		moduleData_t *m = &modules[i];

		if ( !strcmp( m->name, name ) ) {
			memcpy( m->dataBegin, m->initialData, m->dataEnd - m->dataBegin );
			memset( m->bssBegin, 0, m->bssEnd - m->bssBegin );
		}
	}
}
