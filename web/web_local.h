/*
 * web_local.h -- declarations shared by the browser platform files.
 */
#ifndef WEB_LOCAL_H
#define WEB_LOCAL_H

void        Sys_QueEvent( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr );
qboolean    Sys_GetPacket( netadr_t *net_from, msg_t *net_message );
void        Sys_SyncFS( void );

void        IN_Init( void );
void        IN_Frame( void );
void        IN_Shutdown( void );

#endif
