/*
 * etl_snd.c -- SNDDMA backend on Web Audio for ET: Legacy (replaces
 * src/sdl/sdl_snd.c). The engine's mixer writes 16-bit stereo into a ring
 * buffer that the shared Web Audio helper (web_js.c) drains.
 */
#include "../src/client/snd_local.h"
#include "web_js.h"

#define DMA_SAMPLES     65536   // mono samples (32768 stereo frames, ~0.74 s at 44.1 kHz)

cvar_t *s_khz;

static byte *dmaBuffer;

qboolean SNDDMA_Init( void ) {
	int rate;

	s_khz = Cvar_Get( "s_khz", "44", CVAR_LATCH | CVAR_ARCHIVE_ND );
	switch ( s_khz->integer ) {
	case 48: rate = 48000; break;
	case 22: rate = 22050; break;
	case 11: rate = 11025; break;
	default: rate = 44100; break;
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
