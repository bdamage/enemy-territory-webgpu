/*
 * etl_input.c -- keyboard and mouse for ET: Legacy in the browser (replaces
 * src/sdl/sdl_input.c). Produces the same events as the SDL backend:
 * printable keys as their (US layout) ASCII code, everything else as named
 * keys, the console key as CONSOLE_KEY, text as SE_CHAR, and relative mouse
 * motion while the pointer is locked.
 */
#include <emscripten.h>
#include <emscripten/html5.h>

#include "../src/client/client.h"
#include "../src/sys/sys_local.h"
#include "web_js.h"

#define CTRL( a ) ( ( a ) - 'a' + 1 )

static int mouseDx, mouseDy;
static double wheelAccum;
static qboolean pointerLocked;
static qboolean inputInitialized;
static cvar_t *in_mouse;

typedef struct {
	const char *code;
	int key;
} keyMap_t;

static const keyMap_t keyMap[] = {
	{ "Escape", K_ESCAPE }, { "Tab", K_TAB }, { "Enter", K_ENTER }, { "Space", K_SPACE },
	{ "Backspace", K_BACKSPACE }, { "CapsLock", K_CAPSLOCK }, { "Pause", K_PAUSE },
	{ "ArrowUp", K_UPARROW }, { "ArrowDown", K_DOWNARROW }, { "ArrowLeft", K_LEFTARROW }, { "ArrowRight", K_RIGHTARROW },
	{ "AltLeft", K_LALT }, { "AltRight", K_RALT }, { "ControlLeft", K_LCTRL }, { "ControlRight", K_RCTRL },
	{ "ShiftLeft", K_LSHIFT }, { "ShiftRight", K_RSHIFT },
	{ "Insert", K_INS }, { "Delete", K_DEL }, { "PageDown", K_PGDN }, { "PageUp", K_PGUP },
	{ "Home", K_HOME }, { "End", K_END }, { "ScrollLock", K_SCROLLOCK }, { "ContextMenu", K_MENU },
	{ "F1", K_F1 }, { "F2", K_F2 }, { "F3", K_F3 }, { "F4", K_F4 }, { "F5", K_F5 }, { "F6", K_F6 },
	{ "F7", K_F7 }, { "F8", K_F8 }, { "F9", K_F9 }, { "F10", K_F10 }, { "F11", K_F11 }, { "F12", K_F12 },
	{ "F13", K_F13 }, { "F14", K_F14 }, { "F15", K_F15 },
	{ "Numpad7", K_KP_HOME }, { "Numpad8", K_KP_UPARROW }, { "Numpad9", K_KP_PGUP },
	{ "Numpad4", K_KP_LEFTARROW }, { "Numpad5", K_KP_5 }, { "Numpad6", K_KP_RIGHTARROW },
	{ "Numpad1", K_KP_END }, { "Numpad2", K_KP_DOWNARROW }, { "Numpad3", K_KP_PGDN },
	{ "NumpadEnter", K_KP_ENTER }, { "Numpad0", K_KP_INS }, { "NumpadDecimal", K_KP_DEL },
	{ "NumpadDivide", K_KP_SLASH }, { "NumpadSubtract", K_KP_MINUS }, { "NumpadAdd", K_KP_PLUS },
	{ "NumLock", K_KP_NUMLOCK }, { "NumpadMultiply", K_KP_STAR }, { "NumpadEqual", K_KP_EQUALS },
	{ "Backquote", CONSOLE_KEY }, { "Minus", '-' }, { "Equal", '=' }, { "BracketLeft", '[' }, { "BracketRight", ']' },
	{ "Backslash", '\\' }, { "Semicolon", ';' }, { "Quote", '\'' }, { "Comma", ',' }, { "Period", '.' },
	{ "Slash", '/' }, { "IntlBackslash", K_NONUSBACKSLASH },
	{ NULL, 0 }
};

static int MapKey( const EmscriptenKeyboardEvent *e ) {
	const keyMap_t *m;
	const char *code = e->code;

	if ( !strncmp( code, "Key", 3 ) && code[3] && !code[4] ) {
		return tolower( code[3] );
	}
	if ( !strncmp( code, "Digit", 5 ) && code[5] && !code[6] ) {
		return code[5];
	}
	for ( m = keyMap; m->code; m++ ) {
		if ( !strcmp( m->code, code ) ) {
			return m->key;
		}
	}
	return 0;
}

// UTF-8 key text -> one code point (0 if it isn't a single character)
static int KeyText( const char *s ) {
	const unsigned char *c = (const unsigned char *)s;
	int cp;

	if ( !c[0] ) {
		return 0;
	}
	if ( c[0] < 0x80 ) {
		cp = c[0];
		c += 1;
	} else if ( ( c[0] & 0xE0 ) == 0xC0 && c[1] ) {
		cp = ( ( c[0] & 0x1F ) << 6 ) | ( c[1] & 0x3F );
		c += 2;
	} else if ( ( c[0] & 0xF0 ) == 0xE0 && c[1] && c[2] ) {
		cp = ( ( c[0] & 0x0F ) << 12 ) | ( ( c[1] & 0x3F ) << 6 ) | ( c[2] & 0x3F );
		c += 3;
	} else {
		return 0;
	}
	return *c ? 0 : cp;   // multi-character keys ("Shift", "Enter", ...)
}

static EM_BOOL OnKey( int type, const EmscriptenKeyboardEvent *e, void *user ) {
	qboolean down = ( type == EMSCRIPTEN_EVENT_KEYDOWN );
	int key, ch;

	// leave browser shortcuts alone: cmd-*, reload, fullscreen, devtools,
	// and alt-enter (fullscreen in index.html)
	if ( e->metaKey || !strcmp( e->code, "F5" ) || !strcmp( e->code, "F11" ) || !strcmp( e->code, "F12" )
		 || ( e->altKey && !strcmp( e->code, "Enter" ) ) ) {
		return EM_FALSE;
	}

	key = MapKey( e );
	if ( !key ) {
		return EM_TRUE;
	}
	// like SDL: no auto-repeat during play, only in menus and the console
	if ( down && e->repeat && Key_GetCatcher() == 0 ) {
		return EM_TRUE;
	}

	Com_QueueEvent( 0, SE_KEY, key, down, 0, NULL );
	if ( !down || key == CONSOLE_KEY ) {
		return EM_TRUE;
	}

	if ( key == K_BACKSPACE ) {
		Com_QueueEvent( 0, SE_CHAR, CTRL( 'h' ), 0, 0, NULL );
	} else if ( key == K_ESCAPE ) {
		Com_QueueEvent( 0, SE_CHAR, key, 0, 0, NULL );
	} else if ( e->ctrlKey && key >= 'a' && key <= 'z' ) {
		Com_QueueEvent( 0, SE_CHAR, CTRL( key ), 0, 0, NULL );
	} else if ( !e->ctrlKey && ( ch = KeyText( e->key ) ) >= 32 ) {
		Com_QueueEvent( 0, SE_CHAR, ch, 0, 0, NULL );
	}
	return EM_TRUE;     // prevent scrolling, back navigation, find, ...
}

static EM_BOOL OnMouseMove( int type, const EmscriptenMouseEvent *e, void *user ) {
	if ( pointerLocked ) {
		mouseDx += e->movementX;
		mouseDy += e->movementY;
	}
	return EM_FALSE;
}

static int MouseButtonKey( int button ) {
	switch ( button ) {
	case 0: return K_MOUSE1;
	case 1: return K_MOUSE3;
	case 2: return K_MOUSE2;
	case 3: return K_MOUSE4;
	case 4: return K_MOUSE5;
	default: return 0;
	}
}

static EM_BOOL OnMouseButton( int type, const EmscriptenMouseEvent *e, void *user ) {
	int key = MouseButtonKey( e->button );

	if ( !pointerLocked ) {
		// the first click on the canvas only captures the mouse
		if ( type == EMSCRIPTEN_EVENT_MOUSEDOWN ) {
			IN_JS_RequestPointerLock();
			return EM_TRUE;
		}
		return EM_FALSE;
	}
	if ( key ) {
		Com_QueueEvent( 0, SE_KEY, key, type == EMSCRIPTEN_EVENT_MOUSEDOWN, 0, NULL );
	}
	return EM_TRUE;
}

// ~100 px per wheel notch, trackpads send streams of small deltas
static EM_BOOL OnWheel( int type, const EmscriptenWheelEvent *e, void *user ) {
	double dy = e->deltaY;
	int key;

	if ( e->deltaMode == DOM_DELTA_LINE ) {
		dy *= 33.0;
	} else if ( e->deltaMode == DOM_DELTA_PAGE ) {
		dy *= 100.0;
	}
	wheelAccum += dy;
	while ( wheelAccum <= -50.0 || wheelAccum >= 50.0 ) {
		key = wheelAccum < 0 ? K_MWHEELUP : K_MWHEELDOWN;
		Com_QueueEvent( 0, SE_KEY, key, qtrue, 0, NULL );
		Com_QueueEvent( 0, SE_KEY, key, qfalse, 0, NULL );
		wheelAccum += wheelAccum < 0 ? 50.0 : -50.0;
	}
	return EM_TRUE;
}

static EM_BOOL OnPointerLockChange( int type, const EmscriptenPointerlockChangeEvent *e, void *user ) {
	qboolean wasLocked = pointerLocked;

	pointerLocked = e->isActive ? qtrue : qfalse;
	mouseDx = mouseDy = 0;
	if ( wasLocked && !pointerLocked ) {
		// Escape releases pointer lock without reaching the page, so
		// forward it to bring up the in-game menu
		Key_ClearStates();
		if ( cls.state == CA_ACTIVE && !( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) ) ) {
			Com_QueueEvent( 0, SE_KEY, K_ESCAPE, qtrue, 0, NULL );
			Com_QueueEvent( 0, SE_KEY, K_ESCAPE, qfalse, 0, NULL );
		}
	}
	EM_ASM( { if ( Module['onPointerLock'] ) Module['onPointerLock']( $0 ); }, pointerLocked );
	return EM_FALSE;
}

static EM_BOOL OnBlur( int type, const EmscriptenFocusEvent *e, void *user ) {
	Key_ClearStates();
	return EM_FALSE;
}

void IN_Init( void ) {
	in_mouse = Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );

	if ( inputInitialized ) {
		return;
	}
	emscripten_set_keydown_callback( EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, OnKey );
	emscripten_set_keyup_callback( EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, OnKey );
	emscripten_set_mousemove_callback( EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, OnMouseMove );
	emscripten_set_mousedown_callback( "#canvas", NULL, 1, OnMouseButton );
	emscripten_set_mouseup_callback( EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, OnMouseButton );
	emscripten_set_wheel_callback( "#canvas", NULL, 1, OnWheel );
	emscripten_set_pointerlockchange_callback( EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, OnPointerLockChange );
	emscripten_set_blur_callback( EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, OnBlur );
	inputInitialized = qtrue;
}

void IN_Shutdown( void ) {
	mouseDx = mouseDy = 0;
}

void IN_Restart( void ) {
	IN_Shutdown();
	IN_Init();
}

void IN_Frame( void ) {
	if ( mouseDx || mouseDy ) {
		if ( in_mouse->integer ) {
			Com_QueueEvent( 0, SE_MOUSE, mouseDx, mouseDy, 0, NULL );
		}
		mouseDx = mouseDy = 0;
	}
}

EM_JS( void, IN_JS_SetClipboard, ( const char *text ), {
	if ( navigator.clipboard ) {
		navigator.clipboard.writeText( UTF8ToString( text ) ).catch( function() {} );
	}
} );

void IN_SetClipboardData( const char *text ) {
	IN_JS_SetClipboard( text );
}
