/*
 * web_js.c -- browser-side helpers shared by the 2.60b and ET: Legacy builds.
 * See web_js.h.
 */
#include <emscripten.h>
#include "web_js.h"

typedef webbyte_t byte;

// from sys_web.c
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
			_WEB_SyncFS();
		}
	} );
} );

// from sys_web.c
EM_JS( void, Sys_JS_OpenURL, ( const char *url ), {
	window.open( UTF8ToString( url ), '_blank', 'noopener' );
} );

// Hands a file the game wrote (a screenshot, say) to the user as a browser
// download; files in the IndexedDB-backed home directory aren't reachable
// otherwise.
EM_JS( void, WEB_OfferFile, ( const char *name, const void *data, int len ), {
	var fileName = UTF8ToString( name ).replace( /^.*[\\/]/, '' );
	var ext = fileName.split( '.' ).pop().toLowerCase();
	var types = { jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png', tga: 'image/x-tga' };
	var blob = new Blob( [ HEAPU8.slice( data, data + len ) ], { type: types[ext] || 'application/octet-stream' } );
	var a = document.createElement( 'a' );
	a.href = URL.createObjectURL( blob );
	a.download = fileName;
	document.body.appendChild( a );
	a.click();
	a.remove();
	setTimeout( function() { URL.revokeObjectURL( a.href ); }, 10000 );
} );

// from sys_web.c
EM_JS( void, Sys_JS_Print, ( const char *msg ), {
	var s = UTF8ToString( msg );
	if ( Module['print'] ) {
		Module['print']( s );
	} else {
		console.log( s );
	}
} );

// from sys_web.c
EM_JS( void, Sys_JS_Fatal, ( const char *msg ), {
	var s = UTF8ToString( msg );
	console.error( s );
	if ( Module['onFatalError'] ) {
		Module['onFatalError']( s );
	}
} );

// from web_input.c
// Pointer lock with raw (unaccelerated) motion where supported. Must run
// inside the mousedown handler to have user activation.
EM_JS( void, IN_JS_RequestPointerLock, ( void ), {
	var canvas = Module['canvas'];
	var p;
	try {
		p = canvas.requestPointerLock( { unadjustedMovement: true } );
	} catch ( e ) {
		p = null;
	}
	if ( p && p.catch ) {
		p.catch( function() { canvas.requestPointerLock(); } );
	} else if ( !p && document.pointerLockElement !== canvas ) {
		canvas.requestPointerLock();
	}
} );

// from web_snd.c
EM_JS( int, SND_JS_Init, ( byte *buffer, int frames, int rate ), {
	var AC = window.AudioContext || window.webkitAudioContext;
	if ( !AC ) {
		return 0;
	}
	var ctx;
	try {
		ctx = new AC( { sampleRate: rate, latencyHint: 'interactive' } );
	} catch ( e ) {
		ctx = new AC( { latencyHint: 'interactive' } );
	}
	var snd = {
		ctx: ctx,
		buffer: buffer,
		frames: frames,
		pos: 0,         // read position in frames
		frac: 0.0,
		step: rate / ctx.sampleRate
	};
	var node = ctx.createScriptProcessor( 1024, 0, 2 );
	node.onaudioprocess = function( ev ) {
		var outL = ev.outputBuffer.getChannelData( 0 );
		var outR = ev.outputBuffer.getChannelData( 1 );
		var heap = HEAP16;
		var base = snd.buffer >> 1;
		var n = outL.length, pos = snd.pos, frac = snd.frac, step = snd.step, frames = snd.frames;
		for ( var i = 0; i < n; i++ ) {
			var i0 = base + pos * 2;
			var next = pos + 1 === frames ? 0 : pos + 1;
			var i1 = base + next * 2;
			outL[i] = ( heap[i0] + ( heap[i1] - heap[i0] ) * frac ) / 32768;
			outR[i] = ( heap[i0 + 1] + ( heap[i1 + 1] - heap[i0 + 1] ) * frac ) / 32768;
			frac += step;
			while ( frac >= 1.0 ) {
				frac -= 1.0;
				pos = pos + 1 === frames ? 0 : pos + 1;
			}
		}
		snd.pos = pos;
		snd.frac = frac;
	};
	node.connect( ctx.destination );
	snd.node = node;
	Module['etSound'] = snd;

	// autoplay policy: audio starts on the first user gesture
	var resume = function() {
		if ( ctx.state !== 'running' ) {
			ctx.resume();
		}
	};
	document.addEventListener( 'pointerdown', resume, true );
	document.addEventListener( 'keydown', resume, true );

	// the engine stops mixing while the tab is hidden (no animation frames),
	// so stop playback instead of looping the stale ring buffer
	document.addEventListener( 'visibilitychange', function() {
		if ( document.hidden ) {
			ctx.suspend();
		} else {
			resume();
		}
	} );
	return 1;
} );

// from web_snd.c
EM_JS( int, SND_JS_GetPos, ( void ), {
	var snd = Module['etSound'];
	return snd ? snd.pos * 2 : 0;
} );

// from web_snd.c
EM_JS( void, SND_JS_Shutdown, ( void ), {
	var snd = Module['etSound'];
	if ( snd ) {
		snd.node.disconnect();
		snd.ctx.close();
		Module['etSound'] = null;
	}
} );

// from web_net.c
EM_JS( int, NET_JS_Open, ( void ), {
	var url = Module['etRelay'];
	if ( !url || typeof WebSocket === 'undefined' ) {
		return 0;
	}
	var net = Module['etNet'];
	if ( net && net.ws && net.ws.readyState <= 1 ) {
		return 1;
	}
	net = Module['etNet'] = { ws: null, queue: [], outbox: [], open: false };
	var ws;
	try {
		ws = new WebSocket( url );
	} catch ( e ) {
		console.warn( 'relay: ' + e );
		return 0;
	}
	ws.binaryType = 'arraybuffer';
	ws.onopen = function() {
		net.open = true;
		for ( var i = 0; i < net.outbox.length; i++ ) {
			ws.send( net.outbox[i] );
		}
		net.outbox = [];
	};
	ws.onmessage = function( ev ) {
		if ( net.queue.length < 4096 ) {
			net.queue.push( new Uint8Array( ev.data ) );
		}
	};
	ws.onclose = function() {
		if ( net.open ) {
			console.warn( 'relay connection closed' );
		} else {
			console.warn( 'relay unavailable at ' + url + ' (online play disabled)' );
		}
		net.open = false;
	};
	net.ws = ws;
	return 1;
} );

// from web_net.c
EM_JS( void, NET_JS_Close, ( void ), {
	var net = Module['etNet'];
	if ( net && net.ws ) {
		net.ws.onclose = null;
		net.ws.close();
	}
	Module['etNet'] = null;
} );

// from web_net.c
EM_JS( void, NET_JS_Send, ( const byte *data, int len ), {
	var net = Module['etNet'];
	if ( !net || !net.ws ) {
		return;
	}
	var msg = HEAPU8.slice( data, data + len );
	if ( net.open ) {
		if ( net.ws.bufferedAmount < 1048576 ) {
			net.ws.send( msg );
		}
	} else if ( net.ws.readyState === 0 && net.outbox.length < 256 ) {
		net.outbox.push( msg );
	}
} );

// from web_net.c
// Copies the next queued relay message into buf; returns its length or -1.
EM_JS( int, NET_JS_Recv, ( byte *buf, int max ), {
	var net = Module['etNet'];
	if ( !net || !net.queue.length ) {
		return -1;
	}
	var m = net.queue.shift();
	var n = Math.min( m.length, max );
	HEAPU8.set( m.subarray( 0, n ), buf );
	return n;
} );

// from web_dl.c
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

// from web_dl.c
EM_JS( int, DL_JS_Poll, ( int *received ), {
	var dl = Module['etDownload'];
	if ( !dl ) {
		return 2;
	}
	HEAP32[received >> 2] = dl.received;
	return dl.state;
} );

// from web_dl.c
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

// from web_dl.c
EM_JS( void, DL_JS_Cancel, ( void ), {
	Module['etDownload'] = null;
} );

EMSCRIPTEN_KEEPALIVE void WEB_SyncFS( void ) {
	Sys_JS_SyncFS();
}

// ---------------------------------------------------------------------------
// loading screens (web_frame.c)

// Asyncify can only suspend through functions it instrumented, which the
// build lists in ASYNCIFY_ONLY (also handed to the page as
// Module.asyncifyFunctions). Checks that every wasm frame on the current
// stack is one of them, and logs the ones that aren't once per call path.
EM_JS( int, WEB_JS_CanSuspend, ( void ), {
	var list = Module['asyncifyFunctions'];
	if ( !list || typeof Asyncify == 'undefined' ) {
		return 0;
	}
	if ( !Module['asyncifyAllowed'] ) {
		// exact names, plus "prefix*" patterns (static functions get
		// build-specific suffixes when names collide)
		var exact = new Set(), prefixes = [];
		list.forEach( function( n ) {
			if ( n.charAt( n.length - 1 ) == '*' ) {
				prefixes.push( n.slice( 0, -1 ) );
			} else {
				exact.add( n );
			}
		} );
		Module['asyncifyAllowed'] = function( n ) {
			return exact.has( n ) || prefixes.some( function( p ) { return n.indexOf( p ) == 0; } );
		};
	}
	var allowed = Module['asyncifyAllowed'];
	var limit = Error.stackTraceLimit;
	Error.stackTraceLimit = 1000;
	var stack = new Error().stack || '';
	Error.stackTraceLimit = limit;

	var lines = stack.split( '\n' ), names = [];
	for ( var i = 0; i < lines.length; i++ ) {
		var line = lines[i].trim(), name;
		if ( line.indexOf( '.wasm:' ) < 0 && line.indexOf( 'wasm-function' ) < 0 && line.indexOf( '[wasm code]' ) < 0 ) {
			continue;   // a JS frame (this function, the main loop runner)
		}
		// V8: "at $name (.../etl.wasm:1:123)" or "at etl.wasm.name (...)";
		// SpiderMonkey and JavaScriptCore: "name@..."
		if ( line.indexOf( 'at ' ) == 0 ) {
			name = line.slice( 3, line.indexOf( ' ', 3 ) );
		} else {
			name = line.slice( 0, line.indexOf( '@' ) );
		}
		if ( name.indexOf( '.wasm.' ) >= 0 ) {
			name = name.slice( name.indexOf( '.wasm.' ) + 6 );
		}
		if ( name.charAt( 0 ) == '$' ) {
			name = name.slice( 1 );
		}
		if ( !name ) {
			return 0;
		}
		names.push( name );
	}
	if ( !names.length ) {
		return 0;
	}
	var missing = names.filter( function( n ) { return !allowed( n ); } );
	if ( missing.length ) {
		// once per distinct stack, for maintaining the list
		var key = names.join( ' < ' );
		Module['asyncifyMissing'] = Module['asyncifyMissing'] || {};
		if ( !Module['asyncifyMissing'][key] ) {
			Module['asyncifyMissing'][key] = true;
			console.debug( 'loading screen: not suspendable through ' + missing.join( ', ' ) + ' (stack: ' + key + ')' );
		}
		return 0;
	}
	return 1;
} );

EM_ASYNC_JS( void, WEB_JS_WaitForRefresh, ( void ), {
	Module['loadingRefreshes'] = ( Module['loadingRefreshes'] || 0 ) + 1;
	await new Promise( function( resolve ) {
		requestAnimationFrame( function() { resolve(); } );
	} );
} );
