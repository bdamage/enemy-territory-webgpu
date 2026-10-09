/*
 * web_snd.c -- SNDDMA backend on Web Audio.
 *
 * The engine's software mixer (snd_dma.c / snd_mix.c) writes 16-bit stereo
 * into a ring buffer in wasm memory. A ScriptProcessorNode on the main
 * thread drains it; its read position is what SNDDMA_GetDMAPos reports.
 * The mixer's timing assumes dma.speed matches s_khz, so the AudioContext is
 * created at that rate (with linear resampling if the browser refuses).
 */
#include <emscripten.h>

#include "../src/game/q_shared.h"
#include "../src/qcommon/qcommon.h"
#include "../src/client/snd_local.h"

#define DMA_SAMPLES     32768   // mono samples (16384 stereo frames)

static byte *dmaBuffer;

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

EM_JS( int, SND_JS_GetPos, ( void ), {
	var snd = Module['etSound'];
	return snd ? snd.pos * 2 : 0;
} );

EM_JS( void, SND_JS_Shutdown, ( void ), {
	var snd = Module['etSound'];
	if ( snd ) {
		snd.node.disconnect();
		snd.ctx.close();
		Module['etSound'] = null;
	}
} );

qboolean SNDDMA_Init( void ) {
	cvar_t *s_khz = Cvar_Get( "s_khz", "22", CVAR_ARCHIVE | CVAR_LATCH );
	int rate;

	switch ( s_khz->integer ) {
	case 44: rate = 44100; break;
	case 11: rate = 11025; break;
	default: rate = 22050; break;
	}

	if ( !dmaBuffer ) {
		dmaBuffer = calloc( DMA_SAMPLES, 2 );
	}
	memset( dmaBuffer, 0, DMA_SAMPLES * 2 );

	if ( !SND_JS_Init( dmaBuffer, DMA_SAMPLES / 2, rate ) ) {
		Com_Printf( "SNDDMA_Init: Web Audio not available\n" );
		return qfalse;
	}

	dma.channels = 2;
	dma.samples = DMA_SAMPLES;
	dma.submission_chunk = 1;
	dma.samplebits = 16;
	dma.speed = rate;
	dma.samplepos = 0;
	dma.buffer = dmaBuffer;
	Com_Printf( "Web Audio: %d Hz stereo\n", rate );
	return qtrue;
}

int SNDDMA_GetDMAPos( void ) {
	return SND_JS_GetPos();
}

void SNDDMA_Shutdown( void ) {
	SND_JS_Shutdown();
	dma.buffer = NULL;
}

void SNDDMA_BeginPainting( void ) {
}

void SNDDMA_Submit( void ) {
}

void SNDDMA_Activate( void ) {
}
