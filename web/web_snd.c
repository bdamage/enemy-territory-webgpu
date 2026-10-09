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
#include "web_js.h"

#define DMA_SAMPLES     32768   // mono samples (16384 stereo frames)

static byte *dmaBuffer;




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
