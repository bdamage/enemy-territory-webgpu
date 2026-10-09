/*
 * etl_net.c -- networking for ET: Legacy in the browser (replaces
 * src/qcommon/net_ip.c). Same design as the 2.60b build (../../web/web_net.c):
 * UDP goes through the WebSocket relay in tools/server.mjs, hostnames are
 * resolved there behind placeholder addresses in 240.0.0.0/8, and a fixed
 * relay port (net_port) lets a tab host a server. IPv4 only.
 */
#include "../src/qcommon/q_shared.h"
#include "../src/qcommon/qcommon.h"
#include "web_js.h"

#define RELAY_DATA      0x00
#define RELAY_RESOLVE   0x01
#define RELAY_BIND      0x02
#define RELAY_BOUND     0x03

#define MAX_NAMES       256

static char names[MAX_NAMES][256];
static int numNames;
static int boundPort;
static cvar_t *net_port;
static cvar_t *net_enabled;

// ---------------------------------------------------------------------------
// address helpers (IPv4 versions of net_ip.c's)

char *NET_ErrorString( void ) {
	return "relay error";
}

qboolean NET_CompareBaseAdrMask( const netadr_t *a, const netadr_t *b, int netmask ) {
	int curbyte = 0;
	byte cmpmask;

	if ( a->type != b->type ) {
		return qfalse;
	}
	if ( a->type == NA_LOOPBACK ) {
		return qtrue;
	}
	if ( a->type != NA_IP ) {
		return qfalse;
	}
	if ( netmask < 0 || netmask > 32 ) {
		netmask = 32;
	}
	while ( netmask > 7 ) {
		if ( a->ip[curbyte] != b->ip[curbyte] ) {
			return qfalse;
		}
		curbyte++;
		netmask -= 8;
	}
	if ( netmask ) {
		cmpmask = (byte)( ( ( 1 << netmask ) - 1 ) << ( 8 - netmask ) );
		return ( a->ip[curbyte] & cmpmask ) == ( b->ip[curbyte] & cmpmask );
	}
	return qtrue;
}

qboolean NET_CompareBaseAdr( const netadr_t *a, const netadr_t *b ) {
	return NET_CompareBaseAdrMask( a, b, -1 );
}

qboolean NET_CompareAdr( const netadr_t *a, const netadr_t *b ) {
	if ( !NET_CompareBaseAdr( a, b ) ) {
		return qfalse;
	}
	return a->type != NA_IP || a->port == b->port;
}

const char *NET_AdrToStringNoPort( const netadr_t *a ) {
	static char s[NET_ADDRSTRMAXLEN];

	switch ( a->type ) {
	case NA_LOOPBACK: Com_sprintf( s, sizeof( s ), "loopback" ); break;
	case NA_BOT: Com_sprintf( s, sizeof( s ), "bot" ); break;
	case NA_IP: Com_sprintf( s, sizeof( s ), "%u.%u.%u.%u", a->ip[0], a->ip[1], a->ip[2], a->ip[3] ); break;
	case NA_BAD: Com_sprintf( s, sizeof( s ), "invalid" ); break;
	default: Com_sprintf( s, sizeof( s ), "unknown" ); break;
	}
	return s;
}

const char *NET_AdrToString( const netadr_t *a ) {
	static char s[NET_ADDRSTRMAXLEN_EXT];

	if ( a->type == NA_IP ) {
		Com_sprintf( s, sizeof( s ), "%u.%u.%u.%u:%hu", a->ip[0], a->ip[1], a->ip[2], a->ip[3], BigShort( a->port ) );
		return s;
	}
	return NET_AdrToStringNoPort( a );
}

qboolean NET_IsLocalAddress( const netadr_t *adr ) {
	return adr->type == NA_LOOPBACK;
}

qboolean NET_IsLocalAddressString( const char *address ) {
	return !Q_stricmp( address, "loopback" ) || !Q_stricmp( address, "localhost" );
}

qboolean Sys_IsLANAddress( const netadr_t *adr ) {
	if ( adr->type == NA_LOOPBACK ) {
		return qtrue;
	}
	if ( adr->type != NA_IP ) {
		return qfalse;
	}
	return adr->ip[0] == 10 || adr->ip[0] == 127 || ( adr->ip[0] == 192 && adr->ip[1] == 168 )
		   || ( adr->ip[0] == 172 && adr->ip[1] >= 16 && adr->ip[1] < 32 );
}

// ---------------------------------------------------------------------------
// relay transport

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

// Placeholder address for a hostname; the relay is told what it stands for.
static qboolean NameToAdr( const char *host, byte ip[4] ) {
	byte body[4 + 256];
	int i, len = strlen( host );
	qboolean isNew = qfalse;

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

qboolean Sys_StringToAdr( const char *s, netadr_t *a, netadrtype_t family ) {
	Com_Memset( a, 0, sizeof( *a ) );
	if ( family == NA_IP6 ) {
		return qfalse;
	}
	if ( !ParseIPv4( s, a->ip ) && !NameToAdr( s, a->ip ) ) {
		return qfalse;
	}
	a->type = NA_IP;
	return qtrue;
}

void Sys_SendPacket( int length, const void *data, const netadr_t *to ) {
	static byte msg[MAX_MSGLEN + 16];
	int port;

	if ( to->type != NA_IP || length <= 0 || length > MAX_MSGLEN ) {
		return;     // no broadcast/IPv6 through the relay
	}
	port = (unsigned short)BigShort( to->port );
	msg[0] = RELAY_DATA;
	memcpy( msg + 1, to->ip, 4 );
	msg[5] = (byte)( port >> 8 );
	msg[6] = (byte)( port & 0xff );
	memcpy( msg + 7, data, length );
	NET_JS_Send( msg, length + 7 );
}

// Delivers everything the relay has queued (called once per frame).
void NET_Sleep( int64_t usec ) {
	static byte buf[MAX_MSGLEN + 16];
	byte data[MAX_MSGLEN + 1];
	netadr_t from;
	msg_t netmsg;
	int len;

	while ( ( len = NET_JS_Recv( buf, sizeof( buf ) ) ) >= 0 ) {
		if ( len >= 3 && buf[0] == RELAY_BOUND ) {
			boundPort = ( buf[1] << 8 ) | buf[2];
			if ( boundPort ) {
				Com_Printf( "Relay: UDP port %d\n", boundPort );
			} else {
				Com_Printf( "Relay: could not bind UDP port %d\n", net_port ? net_port->integer : 0 );
			}
			continue;
		}
		if ( len < 7 || buf[0] != RELAY_DATA || len - 7 > MAX_MSGLEN ) {
			continue;
		}
		Com_Memset( &from, 0, sizeof( from ) );
		from.type = NA_IP;
		memcpy( from.ip, buf + 1, 4 );
		from.port = BigShort( (short)( ( buf[5] << 8 ) | buf[6] ) );

		MSG_Init( &netmsg, data, sizeof( data ) );
		memcpy( netmsg.data, buf + 7, len - 7 );
		netmsg.cursize = len - 7;

		if ( com_sv_running->integer ) {
			Com_RunAndTimeServerPacket( &from, &netmsg );
		} else {
			CL_PacketEvent( &from, &netmsg );
		}
	}
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

static void NET_Open( void ) {
	byte body[2];
	int port;

	if ( !net_enabled->integer || !NET_JS_Open() ) {
		return;
	}
	// a fixed port lets others reach a server hosted in this tab;
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
	net_enabled = Cvar_Get( "net_enabled", "1", CVAR_LATCH | CVAR_ARCHIVE_ND );
	net_port = Cvar_Get( "net_port", "0", CVAR_LATCH );
	NET_Open();
}

void NET_Shutdown( void ) {
	NET_JS_Close();
	boundPort = 0;
}

void NET_Restart_f( void ) {
	NET_Shutdown();
	NET_Open();
}
