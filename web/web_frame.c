/*
 * web_frame.c -- fixed-rate game frames on top of requestAnimationFrame.
 * Shared by the 2.60b and ET: Legacy builds.
 *
 * Natively the engine runs com_maxfps frames a second, and player movement
 * depends on that rate (jump height at 125 or 333 fps, for example). The
 * browser calls us once per display refresh instead, so each animation
 * frame runs every game frame that has come due at 1000/com_maxfps ms
 * spacing and only the last of them renders.
 *
 * The engine clock (WEB_Microseconds) reports each game frame's scheduled
 * time rather than the wall clock, so frame times come out evenly spaced
 * (8 ms at 125 fps), as they do natively. Within a frame the clock advances
 * in real time; between frames it holds, so input events are stamped no
 * later than the frame that processes them.
 *
 * Loading screens: a map load runs inside a single game frame, and the
 * browser only shows what was drawn once control returns to it. While a load
 * keeps the frame busy, WEB_LoadingUpdate suspends it after each loading
 * screen update (Asyncify) until the next display refresh. Only functions in
 * the build's ASYNCIFY_ONLY list can be suspended, so WEB_JS_CanSuspend checks
 * the call stack first and the update is skipped on any other path.
 */
#include <emscripten.h>
#include "web_js.h"

#define MAX_FRAMES_PER_TICK 10      // backlog beyond this is dropped

int web_skipRender;
int web_frameUpdate;

static double realBase = -1.0;
static int started;
static int inFrame;
static double frameStart;           // engine clock at the current frame's start (us)
static double frameRealStart;       // wall clock at the same moment (us)
static double clockHeld;            // engine clock between frames (us)
static double nextFrame;            // scheduled engine time of the next frame (us)
static double lastBreak;            // wall clock when the browser last got control (us)

static double RealMicroseconds( void ) {
	if ( realBase < 0.0 ) {
		realBase = emscripten_get_now();
	}
	return ( emscripten_get_now() - realBase ) * 1000.0;
}

int64_t WEB_Microseconds( void ) {
	if ( !started ) {
		return (int64_t)RealMicroseconds();     // startup: plain wall clock
	}
	if ( inFrame ) {
		return (int64_t)( frameStart + ( RealMicroseconds() - frameRealStart ) );
	}
	return (int64_t)clockHeld;
}

static void RunFrame( double scheduled, int render, void ( *frame )( void ) ) {
	// never step backwards if the previous frame overran its slot
	frameStart = scheduled > clockHeld ? scheduled : clockHeld;
	frameRealStart = RealMicroseconds();
	lastBreak = frameRealStart;
	inFrame = 1;
	web_skipRender = !render;
	frame();
	web_skipRender = 0;
	clockHeld = (double)WEB_Microseconds();
	inFrame = 0;
}

void WEB_RunFrames( int maxfps, void ( *frame )( void ) ) {
	double now = RealMicroseconds();
	double interval, slack, first;
	int i, count;

	if ( !started ) {
		started = 1;
		clockHeld = now;
		nextFrame = now;
	}

	if ( maxfps <= 0 ) {
		// uncapped: one frame per animation frame
		nextFrame = now;
		RunFrame( now, 1, frame );
		return;
	}

	interval = 1000000.0 / maxfps;
	// animation frame timing jitters; treat frames due within this as due now
	slack = interval * 0.25 < 2000.0 ? interval * 0.25 : 2000.0;

	first = nextFrame;
	count = 0;
	while ( nextFrame <= now + slack && count < MAX_FRAMES_PER_TICK ) {
		count++;
		nextFrame += interval;
	}
	if ( nextFrame <= now ) {
		// fell behind (tab was hidden, long load, slow device): drop the backlog
		nextFrame = now + interval;
	}

	for ( i = 0; i < count; i++ ) {
		RunFrame( first + i * interval, i == count - 1, frame );
	}
}

// Called after a loading screen has been drawn and submitted. If the frame
// has been busy for a while, give the browser a display refresh to show it.
void WEB_LoadingUpdate( void ) {
	if ( !inFrame || RealMicroseconds() - lastBreak < 50000.0 ) {
		return;
	}
	if ( WEB_JS_CanSuspend() ) {
		WEB_JS_WaitForRefresh();
	}
	lastBreak = RealMicroseconds();
}
