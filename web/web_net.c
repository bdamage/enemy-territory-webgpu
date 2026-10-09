/*
 * web_net.c -- networking for the browser build.
 *
 * Browsers can't send UDP, so IP packets go through a WebSocket relay
 * (tools/server.mjs) that owns one UDP socket per connection. The engine's
 * own loopback channel still handles the local listen server <-> client path.
 *
 * Hostnames can't be resolved synchronously in a browser, so Sys_StringToAdr
 * hands out placeholder addresses in 240.0.0.0/8 (reserved, never routable)
 * and tells the relay which hostname each stands for; the relay resolves it
 * and translates in both directions.
 *
 * The relay URL comes from Module.etRelay (set by index.html, default
 * ws[s]://<page host>/relay). Without a relay only local play works.
 */
#include <emscripten.h>

#include "../src/game/q_shared.h"
#include "../src/qcommon/qcommon.h"
#include "web_js.h"

#define RELAY_DATA      0x00
#define RELAY_RESOLVE   0x01
#define RELAY_BIND      0x02
#define RELAY_BOUND     0x03

#define MAX_NAMES       256

static char names[MAX_NAMES][256];
static int numNames;

static cvar_t *net_port;
static int boundPort;

// ---------------------------------------------------------------------------
// JS side: the socket and an incoming message queue





// ---------------------------------------------------------------------------

static void SendControl( int type, const void *body, int len ) {
	byte msg[512];

	if ( len > (int)sizeof( msg ) - 1 ) {
		return;
	}
	msg[0] = (byte)type;
	memcpy( msg + 1, body, len );
	NET_JS_Send( msg, len + 1 );
}

static qboolean ParseIPv4( const char *s, byte ip[4] ) {
	int parts[4], n;
	const char *c = s;

	for ( n = 0; n < 4; n++ ) {
		if ( *c < '0' || *c > '9' ) {
			return qfalse;
		}
		parts[n] = 0;
		while ( *c >= '0' && *c <= '9' ) {
			parts[n] = parts[n] * 10 + ( *c++ - '0' );
			if ( parts[n] > 255 ) {
				return qfalse;
			}
		}
		if ( n < 3 && *c++ != '.' ) {
			return qfalse;
		}
	}
	if ( *c ) {
		return qfalse;
	}
	for ( n = 0; n < 4; n++ ) {
		ip[n] = (byte)parts[n];
	}
	return qtrue;
}

// Returns the placeholder address for a hostname, registering it with the
// relay the first time it's seen.
static qboolean NameToAdr( const char *host, byte ip[4] ) {
	byte body[4 + 256];
	int i, len;
	qboolean isNew = qfalse;

	len = strlen( host );
	if ( len == 0 || len >= (int)sizeof( names[0] ) ) {
		return qfalse;
	}
	for ( i = 0; i < len; i++ ) {
		char c = host[i];
		if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '-' || c == '.' ) ) {
			return qfalse;
		}
	}

	for ( i = 0; i < numNames; i++ ) {
		if ( !Q_stricmp( names[i], host ) ) {
			break;
		}
	}
	if ( i == numNames ) {
		if ( numNames == MAX_NAMES ) {
			return qfalse;
		}
		Q_strncpyz( names[numNames++], host, sizeof( names[0] ) );
		isNew = qtrue;
	}

	ip[0] = 240;
	ip[1] = 0;
	ip[2] = (byte)( ( i + 1 ) >> 8 );
	ip[3] = (byte)( ( i + 1 ) & 0xff );

	if ( isNew ) {
		memcpy( body, ip, 4 );
		memcpy( body + 4, host, len );
		SendControl( RELAY_RESOLVE, body, 4 + len );
	}
	return qtrue;
}

// ---------------------------------------------------------------------------
// Sys_* network interface

qboolean Sys_StringToAdr( const char *s, netadr_t *a ) {
	memset( a, 0, sizeof( *a ) );
	if ( !ParseIPv4( s, a->ip ) && !NameToAdr( s, a->ip ) ) {
		return qfalse;
	}
	a->type = NA_IP;
	return qtrue;
}

qboolean Sys_GetPacket( netadr_t *net_from, msg_t *net_message ) {
	static byte buf[MAX_MSGLEN + 16];
	int len;

	for ( ;; ) {
		len = NET_JS_Recv( buf, sizeof( buf ) );
		if ( len < 0 ) {
			return qfalse;
		}
		if ( len >= 1 && buf[0] == RELAY_BOUND && len >= 3 ) {
			boundPort = ( buf[1] << 8 ) | buf[2];
			if ( boundPort ) {
				Com_Printf( "Relay: UDP port %d\n", boundPort );
			} else {
				Com_Printf( "Relay: could not bind UDP port %d\n", net_port ? net_port->integer : 0 );
			}
			continue;
		}
		if ( len < 7 || buf[0] != RELAY_DATA ) {
			continue;
		}
		if ( len - 7 > net_message->maxsize ) {
			continue;
		}
		memset( net_from, 0, sizeof( *net_from ) );
		net_from->type = NA_IP;
		memcpy( net_from->ip, buf + 1, 4 );
		net_from->port = BigShort( (short)( ( buf[5] << 8 ) | buf[6] ) );
		memcpy( net_message->data, buf + 7, len - 7 );
		net_message->cursize = len - 7;
		net_message->readcount = 0;
		return qtrue;
	}
}

void Sys_SendPacket( int length, const void *data, netadr_t to ) {
	static byte msg[MAX_MSGLEN + 16];
	int port;

	if ( to.type != NA_IP ) {
		return;     // no broadcast or IPX through the relay
	}
	if ( length <= 0 || length > MAX_MSGLEN ) {
		return;
	}
	port = (unsigned short)BigShort( to.port );
	msg[0] = RELAY_DATA;
	memcpy( msg + 1, to.ip, 4 );
	msg[5] = (byte)( port >> 8 );
	msg[6] = (byte)( port & 0xff );
	memcpy( msg + 7, data, length );
	NET_JS_Send( msg, length + 7 );
}

qboolean Sys_IsLANAddress( netadr_t adr ) {
	if ( adr.type == NA_LOOPBACK ) {
		return qtrue;
	}
	if ( adr.type != NA_IP ) {
		return qfalse;
	}
	return adr.ip[0] == 10 || adr.ip[0] == 127 || ( adr.ip[0] == 192 && adr.ip[1] == 168 )
		   || ( adr.ip[0] == 172 && adr.ip[1] >= 16 && adr.ip[1] < 32 );
}

void Sys_ShowIP( void ) {
	int i;

	if ( boundPort ) {
		Com_Printf( "Relay UDP port: %d\n", boundPort );
	} else {
		Com_Printf( "Not connected to a relay\n" );
	}
	for ( i = 0; i < numNames; i++ ) {
		Com_Printf( "  240.0.%d.%d = %s\n", ( i + 1 ) >> 8, ( i + 1 ) & 0xff, names[i] );
	}
}

// ---------------------------------------------------------------------------

static void NET_OpenRelay( void ) {
	byte body[2];
	int port;

	if ( !NET_JS_Open() ) {
		return;
	}
	// a fixed port (net_port) lets others reach a server hosted in this tab;
	// clients just take an ephemeral one
	port = net_port->integer;
	if ( port < 0 || port > 65535 ) {
		port = 0;
	}
	body[0] = (byte)( port >> 8 );
	body[1] = (byte)( port & 0xff );
	SendControl( RELAY_BIND, body, 2 );
}

void NET_Init( void ) {
	net_port = Cvar_Get( "net_port", "0", CVAR_LATCH );
	NET_OpenRelay();
}

void NET_Shutdown( void ) {
	NET_JS_Close();
	boundPort = 0;
}

void NET_Restart( void ) {
	NET_Shutdown();
	NET_Init();
}

void NET_Config( qboolean enableNetworking ) {
}

void NET_Sleep( int msec ) {
}
