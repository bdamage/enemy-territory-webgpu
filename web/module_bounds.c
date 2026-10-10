/*
 * module_bounds.c -- marks the start or end of a statically linked game
 * module's data. The Makefiles compile it twice per module (MODULE=cgame,
 * BOUND=begin / end) and put the two objects around that module's objects
 * on the link line; the linker keeps input order within .data and .bss, so
 * the markers bracket exactly the module's variables. See web_module_data.c.
 */
#define PASTE2( a, b ) a ## b
#define PASTE( a, b ) PASTE2( a, b )

char PASTE( MODULE, PASTE( _data_, BOUND ) )[1] = { 1 };    // .data
char PASTE( MODULE, PASTE( _bss_, BOUND ) )[1];             // .bss (built with -fno-common)
