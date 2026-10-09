/*
 * web_net.c -- networking for the browser build.
 *
 * Browsers can't send UDP, so only the engine's loopback channel works
 * (local listen server <-> client). Remote play would need a WebSocket or
 * WebRTC relay plugged in behind Sys_SendPacket/Sys_GetPacket.
 */
#include "../src/game/q_shared.h"
#include "../src/qcommon/qcommon.h"

static qboolean ParseIPv4( const char *s, byte ip[4], unsigned short *port ) {
	int parts[4], p = 0, n;
	const char *c = s;

	*port = 0;
	for ( n = 0; n < 4; n++ ) {
		if ( *c < '0' || *c > '9' ) {
			return qfalse;
		}
		parts[n] = 0;
		while ( *c >= '0' && *c <= '9' ) {
			parts[n] = parts[n] * 10 + ( *c++ - '0' );
		}
		if ( parts[n] > 255 ) {
			return qfalse;
		}
		if ( n < 3 && *c++ != '.' ) {
			return qfalse;
		}
	}
	if ( *c == ':' ) {
		p = atoi( c + 1 );
	} else if ( *c ) {
		return qfalse;
	}
	for ( n = 0; n < 4; n++ ) {
		ip[n] = (byte)parts[n];
	}
	*port = BigShort( (short)p );
	return qtrue;
}

qboolean Sys_StringToAdr( const char *s, netadr_t *a ) {
	memset( a, 0, sizeof( *a ) );
	if ( !ParseIPv4( s, a->ip, &a->port ) ) {
		return qfalse;
	}
	a->type = NA_IP;
	return qtrue;
}

qboolean Sys_GetPacket( netadr_t *net_from, msg_t *net_message ) {
	return qfalse;
}

void Sys_SendPacket( int length, const void *data, netadr_t to ) {
}

qboolean Sys_IsLANAddress( netadr_t adr ) {
	return adr.type == NA_LOOPBACK;
}

void Sys_ShowIP( void ) {
	Com_Printf( "No network interfaces (browser build: loopback only)\n" );
}

void NET_Init( void ) {
}

void NET_Shutdown( void ) {
}

void NET_Restart( void ) {
}

void NET_Config( qboolean enableNetworking ) {
}

void NET_Sleep( int msec ) {
}
