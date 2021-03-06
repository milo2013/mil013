// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "cbasetypes.h"
#include "mmo.h"
#include "timer.h"
#include "malloc.h"
#include "showmsg.h"
#include "strlib.h"
#include "socket.h"

#include <stdlib.h>

#ifdef WIN32
	#include "winapi.h"
#else
	#include <errno.h>
#include <netinet/tcp.h>
	#include <net/if.h>
	#include <unistd.h>
#include <sys/ioctl.h>
	#include <netdb.h>
	#include <arpa/inet.h>

	#ifndef SIOCGIFCONF
	#include <sys/sockio.h> // SIOCGIFCONF on Solaris, maybe others? [Shinomori]
	#endif
	#ifndef FIONBIO
	#include <sys/filio.h> // FIONBIO on Solaris [FlavioJS]
	#endif

	#ifdef HAVE_SETRLIMIT
	#include <sys/resource.h>
	#endif
#endif

/////////////////////////////////////////////////////////////////////
#if defined(WIN32)
/////////////////////////////////////////////////////////////////////
// windows portability layer

typedef int socklen_t;

#define sErrno WSAGetLastError()
#define S_ENOTSOCK WSAENOTSOCK
#define S_EWOULDBLOCK WSAEWOULDBLOCK
#define S_EINTR WSAEINTR
#define S_ECONNABORTED WSAECONNABORTED

#define SHUT_RD   SD_RECEIVE
#define SHUT_WR   SD_SEND
#define SHUT_RDWR SD_BOTH

// global array of sockets (emulating linux)
// fd is the position in the array
static SOCKET sock_arr[FD_SETSIZE];
static int sock_arr_len = 0;

/// Returns the socket associated with the target fd.
///
/// @param fd Target fd.
/// @return Socket
#define fd2sock(fd) sock_arr[fd]

/// Returns the first fd associated with the socket.
/// Returns -1 if the socket is not found.
///
/// @param s Socket
/// @return Fd or -1
int sock2fd(SOCKET s)
{
	int fd;

	// search for the socket
	for( fd = 1; fd < sock_arr_len; ++fd )
		if( sock_arr[fd] == s )
			break;// found the socket
	if( fd == sock_arr_len )
		return -1;// not found
	return fd;
}


/// Inserts the socket into the global array of sockets.
/// Returns a new fd associated with the socket.
/// If there are too many sockets it closes the socket, sets an error and
//  returns -1 instead.
/// Since fd 0 is reserved, it returns values in the range [1,FD_SETSIZE[.
///
/// @param s Socket
/// @return New fd or -1
int sock2newfd(SOCKET s)
{
	int fd;

	// find an empty position
	for( fd = 1; fd < sock_arr_len; ++fd )
		if( sock_arr[fd] == INVALID_SOCKET )
			break;// empty position
	if( fd == ARRAYLENGTH(sock_arr) )
	{// too many sockets
		closesocket(s);
		WSASetLastError(WSAEMFILE);
		return -1;
	}
	sock_arr[fd] = s;
	if( sock_arr_len <= fd )
		sock_arr_len = fd+1;
	return fd;
}

int sAccept(int fd, struct sockaddr* addr, int* addrlen)
{
	SOCKET s;

	// accept connection
	s = accept(fd2sock(fd), addr, addrlen);
	if( s == INVALID_SOCKET )
		return -1;// error
	return sock2newfd(s);
}

int sClose(int fd)
{
	int ret = closesocket(fd2sock(fd));
	fd2sock(fd) = INVALID_SOCKET;
	return ret;
}

int sSocket(int af, int type, int protocol)
{
	SOCKET s;

	// create socket
	s = socket(af,type,protocol);
	if( s == INVALID_SOCKET )
		return -1;// error
	return sock2newfd(s);
}

char* sErr(int code)
{
	static char sbuf[512];
	// strerror does not handle socket codes
	if( FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
			code, MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), (LPTSTR)&sbuf, sizeof(sbuf), NULL) == 0 )
		snprintf(sbuf, sizeof(sbuf), "unknown error");
	return sbuf;
}

#define sBind(fd,name,namelen) bind(fd2sock(fd),name,namelen)
#define sConnect(fd,name,namelen) connect(fd2sock(fd),name,namelen)
#define sIoctl(fd,cmd,argp) ioctlsocket(fd2sock(fd),cmd,argp)
#define sListen(fd,backlog) listen(fd2sock(fd),backlog)
#define sRecv(fd,buf,len,flags) recv(fd2sock(fd),buf,len,flags)
#define sSelect select
#define sSend(fd,buf,len,flags) send(fd2sock(fd),buf,len,flags)
#define sSetsockopt(fd,level,optname,optval,optlen) setsockopt(fd2sock(fd),level,optname,optval,optlen)
#define sShutdown(fd,how) shutdown(fd2sock(fd),how)
#define sFD_SET(fd,set) FD_SET(fd2sock(fd),set)
#define sFD_CLR(fd,set) FD_CLR(fd2sock(fd),set)
#define sFD_ISSET(fd,set) FD_ISSET(fd2sock(fd),set)
#define sFD_ZERO FD_ZERO

/////////////////////////////////////////////////////////////////////
#else
/////////////////////////////////////////////////////////////////////
// nix portability layer

#define SOCKET_ERROR (-1)

#define sErrno errno
#define S_ENOTSOCK EBADF
#define S_EWOULDBLOCK EAGAIN
#define S_EINTR EINTR
#define S_ECONNABORTED ECONNABORTED

#define sAccept accept
#define sClose close
#define sSocket socket
#define sErr strerror

#define sBind bind
#define sConnect connect
#define sIoctl ioctl
#define sListen listen
#define sRecv recv
#define sSelect select
#define sSend send
#define sSetsockopt setsockopt
#define sShutdown shutdown
#define sFD_SET FD_SET
#define sFD_CLR FD_CLR
#define sFD_ISSET FD_ISSET
#define sFD_ZERO FD_ZERO

/////////////////////////////////////////////////////////////////////
#endif
/////////////////////////////////////////////////////////////////////

#ifndef MSG_NOSIGNAL
	#define MSG_NOSIGNAL 0
#endif

fd_set readfds;
int fd_max;
time_t last_tick;
time_t stall_time = 60;

uint32 addr_[16];   // ip addresses of local host (host byte order)
int naddr_ = 0;   // # of ip addresses

// Maximum packet size in bytes, which the client is able to handle.
// Larger packets cause a buffer overflow and stack corruption.
#if PACKETVER < 20131223
static size_t socket_max_client_packet = 0x6000;
#else
static size_t socket_max_client_packet = USHRT_MAX;
#endif

#ifdef SHOW_SERVER_STATS
// Data I/O statistics
static size_t socket_data_i = 0, socket_data_ci = 0, socket_data_qi = 0;
static size_t socket_data_o = 0, socket_data_co = 0, socket_data_qo = 0;
static time_t socket_data_last_tick = 0;
#endif

// initial recv buffer size (this will also be the max. size)
// biggest known packet: S 0153 <len>.w <emblem data>.?B -> 24x24 256 color .bmp (0153 + len.w + 1618/1654/1756 bytes)
#define RFIFO_SIZE (2*1024)
// initial send buffer size (will be resized as needed)
#define WFIFO_SIZE (16*1024)

// Maximum size of pending data in the write fifo. (for non-server connections)
// The connection is closed if it goes over the limit.
#define WFIFO_MAX (1*1024*1024)

struct socket_data* session[FD_SETSIZE];

#ifdef SEND_SHORTLIST
int send_shortlist_array[FD_SETSIZE];// we only support FD_SETSIZE sockets, limit the array to that
int send_shortlist_count = 0;// how many fd's are in the shortlist
uint32 send_shortlist_set[(FD_SETSIZE+31)/32];// to know if specific fd's are already in the shortlist
#endif

static int create_session(int fd, RecvFunc func_recv, SendFunc func_send, ParseFunc func_parse);

#ifndef MINICORE
	int ip_rules = 1;
	static int connect_check(uint32 ip);
#endif

const char* error_msg(void)
{
	static char buf[512];
	int code = sErrno;
	snprintf(buf, sizeof(buf), "error %d: %s", code, sErr(code));
	return buf;
}

/*======================================
 *	CORE : Default processing functions
 *--------------------------------------*/
int null_recv(int fd) { return 0; }
int null_send(int fd) { return 0; }
int null_parse(int fd) { return 0; }

ParseFunc default_func_parse = null_parse;

void set_defaultparse(ParseFunc defaultparse)
{
	default_func_parse = defaultparse;
}


/*======================================
 *	CORE : Socket options
 *--------------------------------------*/
void set_nonblocking(int fd, unsigned long yes)
{
	// FIONBIO Use with a nonzero argp parameter to enable the nonblocking mode of socket s.
	// The argp parameter is zero if nonblocking is to be disabled.
	if( sIoctl(fd, FIONBIO, &yes) != 0 )
		ShowError("set_nonblocking: Failed to set socket #%d to non-blocking mode (%s) - Please report this!!!\n", fd, error_msg());
}

void setsocketopts(int fd,int delay_timeout){
	int yes = 1; // reuse fix

#if !defined(WIN32)
	// set SO_REAUSEADDR to true, unix only. on windows this option causes
	// the previous owner of the socket to give up, which is not desirable
	// in most cases, neither compatible with unix.
	sSetsockopt(fd,SOL_SOCKET,SO_REUSEADDR,(char *)&yes,sizeof(yes));
#ifdef SO_REUSEPORT
	sSetsockopt(fd,SOL_SOCKET,SO_REUSEPORT,(char *)&yes,sizeof(yes));
#endif
#endif

	// Set the socket into no-delay mode; otherwise packets get delayed for up to 200ms, likely creating server-side lag.
	// The RO protocol is mainly single-packet request/response, plus the FIFO model already does packet grouping anyway.
	sSetsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&yes, sizeof(yes));

	// force the socket into no-wait, graceful-close mode (should be the default, but better make sure)
	//(https://msdn.microsoft.com/en-us/library/windows/desktop/ms737582%28v=vs.85%29.aspx)
	{
		struct linger opt;
		opt.l_onoff = 0; // SO_DONTLINGER
		opt.l_linger = 0; // Do not care
		if( sSetsockopt(fd, SOL_SOCKET, SO_LINGER, (char*)&opt, sizeof(opt)) )
			ShowWarning("setsocketopts: Unable to set SO_LINGER mode for connection #%d!\n", fd);
	}
	if(delay_timeout){
#if defined(WIN32)
		int timeout = delay_timeout * 1000;
#else
		struct timeval timeout;
		timeout.tv_sec = delay_timeout;
		timeout.tv_usec = 0;
#endif

		if (sSetsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout,sizeof(timeout)) < 0)
			ShowError("setsocketopts: Unable to set SO_RCVTIMEO timeout for connection #%d!\n");
		if (sSetsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout,sizeof(timeout)) < 0)
			ShowError("setsocketopts: Unable to set SO_SNDTIMEO timeout for connection #%d!\n");
	}
}

/*======================================
 *	CORE : Socket Sub Function
 *--------------------------------------*/
void set_eof(int fd)
{
	if( session_isActive(fd) )
	{
#ifdef SEND_SHORTLIST
		// Add this socket to the shortlist for eof handling.
		send_shortlist_add_fd(fd);
#endif
		session[fd]->flag.eof = 1;
	}
}

int recv_to_fifo(int fd)
{
	int len;

	if( !session_isActive(fd) )
		return -1;

	len = sRecv(fd, (char *) session[fd]->rdata + session[fd]->rdata_size, (int)RFIFOSPACE(fd), 0);

	if( len == SOCKET_ERROR )
	{//An exception has occured
		if( sErrno != S_EWOULDBLOCK ) {
			//ShowDebug("recv_to_fifo: %s, closing connection #%d\n", error_msg(), fd);
			set_eof(fd);
		}
		return 0;
	}

	if( len == 0 )
	{//Normal connection end.
		set_eof(fd);
		return 0;
	}

	session[fd]->rdata_size += len;
	session[fd]->rdata_tick = last_tick;
#ifdef SHOW_SERVER_STATS
	socket_data_i += len;
	socket_data_qi += len;
	if (!session[fd]->flag.server)
	{
		socket_data_ci += len;
	}
#endif
	return 0;
}

int send_from_fifo(int fd)
{
	int len;

	if( !session_isValid(fd) )
		return -1;

	if( session[fd]->wdata_size == 0 )
		return 0; // nothing to send

	len = sSend(fd, (const char *) session[fd]->wdata, (int)session[fd]->wdata_size, MSG_NOSIGNAL);

	if( len == SOCKET_ERROR )
	{//An exception has occured
		if( sErrno != S_EWOULDBLOCK ) {
			//ShowDebug("send_from_fifo: %s, ending connection #%d\n", error_msg(), fd);
#ifdef SHOW_SERVER_STATS
			socket_data_qo -= session[fd]->wdata_size;
#endif
			session[fd]->wdata_size = 0; //Clear the send queue as we can't send anymore. [Skotlex]
			set_eof(fd);
		}
		return 0;
	}

	if( len > 0 )
	{
		// some data could not be transferred?
		// shift unsent data to the beginning of the queue
		if( (size_t)len < session[fd]->wdata_size )
			memmove(session[fd]->wdata, session[fd]->wdata + len, session[fd]->wdata_size - len);

		session[fd]->wdata_size -= len;
#ifdef SHOW_SERVER_STATS
		socket_data_o += len;
		socket_data_qo -= len;
		if (!session[fd]->flag.server)
		{
			socket_data_co += len;
		}
#endif
	}

	return 0;
}

/// Best effort - there's no warranty that the data will be sent.
void flush_fifo(int fd)
{
	if(session[fd] != NULL)
		session[fd]->func_send(fd);
}

void flush_fifos(void)
{
	int i;
	for(i = 1; i < fd_max; i++)
		flush_fifo(i);
}

/*======================================
 *	CORE : Connection functions
 *--------------------------------------*/
int connect_client(int listen_fd)
{
	int fd;
	struct sockaddr_in client_address;
	socklen_t len;

	len = sizeof(client_address);

	fd = sAccept(listen_fd, (struct sockaddr*)&client_address, &len);
	if ( fd == -1 ) {
		ShowError("connect_client: accept failed (%s)!\n", error_msg());
		return -1;
	}
	if( fd == 0 )
	{// reserved
		ShowError("connect_client: Socket #0 is reserved - Please report this!!!\n");
		sClose(fd);
		return -1;
	}
	if( fd >= FD_SETSIZE )
	{// socket number too big
		ShowError("connect_client: New socket #%d is greater than can we handle! Increase the value of FD_SETSIZE (currently %d) for your OS to fix this!\n", fd, FD_SETSIZE);
		sClose(fd);
		return -1;
	}

	setsocketopts(fd,0);
	set_nonblocking(fd, 1);

#ifndef MINICORE
	if( ip_rules && !connect_check(ntohl(client_address.sin_addr.s_addr)) ) {
		do_close(fd);
		return -1;
	}
#endif

	if( fd_max <= fd ) fd_max = fd + 1;
	sFD_SET(fd,&readfds);

	create_session(fd, recv_to_fifo, send_from_fifo, default_func_parse);
	session[fd]->client_addr = ntohl(client_address.sin_addr.s_addr);

	return fd;
}

int make_listen_bind(uint32 ip, uint16 port)
{
	struct sockaddr_in server_address;
	int fd;
	int result;

	fd = sSocket(AF_INET, SOCK_STREAM, 0);

	if( fd == -1 )
	{
		ShowError("make_listen_bind: socket creation failed (%s)!\n", error_msg());
		exit(EXIT_FAILURE);
	}
	if( fd == 0 )
	{// reserved
		ShowError("make_listen_bind: Socket #0 is reserved - Please report this!!!\n");
		sClose(fd);
		return -1;
	}
	if( fd >= FD_SETSIZE )
	{// socket number too big
		ShowError("make_listen_bind: New socket #%d is greater than can we handle! Increase the value of FD_SETSIZE (currently %d) for your OS to fix this!\n", fd, FD_SETSIZE);
		sClose(fd);
		return -1;
	}

	setsocketopts(fd,0);
	set_nonblocking(fd, 1);

	server_address.sin_family      = AF_INET;
	server_address.sin_addr.s_addr = htonl(ip);
	server_address.sin_port        = htons(port);

	result = sBind(fd, (struct sockaddr*)&server_address, sizeof(server_address));
	if( result == SOCKET_ERROR ) {
		ShowError("make_listen_bind: bind failed (socket #%d, %s)!\n", fd, error_msg());
		exit(EXIT_FAILURE);
	}
	result = sListen(fd,5);
	if( result == SOCKET_ERROR ) {
		ShowError("make_listen_bind: listen failed (socket #%d, %s)!\n", fd, error_msg());
		exit(EXIT_FAILURE);
	}

	if(fd_max <= fd) fd_max = fd + 1;
	sFD_SET(fd, &readfds);

	create_session(fd, connect_client, null_send, null_parse);
	session[fd]->client_addr = 0; // just listens
	session[fd]->rdata_tick = 0; // disable timeouts on this socket

	return fd;
}

int make_connection(uint32 ip, uint16 port, bool silent,int timeout) {
	struct sockaddr_in remote_address;
	int fd;
	int result;

	fd = sSocket(AF_INET, SOCK_STREAM, 0);

	if (fd == -1) {
		ShowError("make_connection: socket creation failed (%s)!\n", error_msg());
		return -1;
	}
	if( fd == 0 )
	{// reserved
		ShowError("make_connection: Socket #0 is reserved - Please report this!!!\n");
		sClose(fd);
		return -1;
	}
	if( fd >= FD_SETSIZE )
	{// socket number too big
		ShowError("make_connection: New socket #%d is greater than can we handle! Increase the value of FD_SETSIZE (currently %d) for your OS to fix this!\n", fd, FD_SETSIZE);
		sClose(fd);
		return -1;
	}

	setsocketopts(fd,timeout);

	remote_address.sin_family      = AF_INET;
	remote_address.sin_addr.s_addr = htonl(ip);
	remote_address.sin_port        = htons(port);

	if( !silent )
		ShowStatus("Connecting to %d.%d.%d.%d:%i\n", CONVIP(ip), port);
#ifdef WIN32
	// On Windows we have to set the socket non-blocking before the connection to make timeout work. [Lemongrass]
	set_nonblocking(fd, 1);

	result = sConnect(fd, (struct sockaddr *)(&remote_address), sizeof(struct sockaddr_in));

	// Only enter if a socket error occurred
	// Create a pseudo scope to be able to break out in case of successful connection
	while( result == SOCKET_ERROR ) {
		// Specially handle the error number for connection attempts that would block, because we want to use a timeout
		if( sErrno == S_EWOULDBLOCK ){
			fd_set writeSet;
			struct timeval tv;

			sFD_ZERO(&writeSet);
			sFD_SET(fd,&writeSet);

			tv.tv_sec = timeout;
			tv.tv_usec = 0;

			result = sSelect(0, NULL, &writeSet, NULL, &tv);

			// Connection attempt timed out
			if( result == 0 ){
				if( !silent ){
					// Needs special handling, because it does not set an error code and therefore does not provide an error message from the API
					ShowError("make_connection: connection failed (socket #%d, timeout after %ds)!\n", fd, timeout);
				}

				do_close(fd);
				return -1;
			// If the select operation did not return an error
			}else if( result != SOCKET_ERROR ){
				// Check if it is really writeable
				if( sFD_ISSET(fd, &writeSet) != 0 ){
					// Our socket is writeable now => we have connected successfully
					break; // leave the pseudo scope
				}

				if( !silent ){
					// Needs special handling, because it does not set an error code and therefore does not provide an error message from the API
					ShowError("make_connection: connection failed (socket #%d, not writeable)!\n", fd);
				}

				do_close(fd);
				return -1;
			}
			// The select operation failed
		}

		if( !silent )
			ShowError("make_connection: connect failed (socket #%d, %s)!\n", fd, error_msg());

		do_close(fd);
		return -1;
	}
	// Keep the socket in non-blocking mode, since we would set it to non-blocking here on unix. [Lemongrass]
#else
	result = sConnect(fd, (struct sockaddr *)(&remote_address), sizeof(struct sockaddr_in));
	if( result == SOCKET_ERROR ) {
		if( !silent )
			ShowError("make_connection: connect failed (socket #%d, %s)!\n", fd, error_msg());
		do_close(fd);
		return -1;
	}

	//Now the socket can be made non-blocking. [Skotlex]
	set_nonblocking(fd, 1);
#endif

	if (fd_max <= fd) fd_max = fd + 1;
	sFD_SET(fd,&readfds);

	create_session(fd, recv_to_fifo, send_from_fifo, default_func_parse);
	session[fd]->client_addr = ntohl(remote_address.sin_addr.s_addr);

	return fd;
}

static int create_session(int fd, RecvFunc func_recv, SendFunc func_send, ParseFunc func_parse)
{
	CREATE(session[fd], struct socket_data, 1);
	CREATE(session[fd]->rdata, unsigned char, RFIFO_SIZE);
	CREATE(session[fd]->wdata, unsigned char, WFIFO_SIZE);
	session[fd]->max_rdata  = RFIFO_SIZE;
	session[fd]->max_wdata  = WFIFO_SIZE;
	session[fd]->func_recv  = func_recv;
	session[fd]->func_send  = func_send;
	session[fd]->func_parse = func_parse;
	session[fd]->rdata_tick = last_tick;
	return 0;
}

static void delete_session(int fd)
{
	if( session_isValid(fd) )
	{
#ifdef SHOW_SERVER_STATS
		socket_data_qi -= session[fd]->rdata_size - session[fd]->rdata_pos;
		socket_data_qo -= session[fd]->wdata_size;
#endif
		aFree(session[fd]->rdata);
		aFree(session[fd]->wdata);
		aFree(session[fd]->session_data);
		aFree(session[fd]);
		session[fd] = NULL;
	}
}

int realloc_fifo(int fd, unsigned int rfifo_size, unsigned int wfifo_size)
{
	if( !session_isValid(fd) )
		return 0;

	if( session[fd]->max_rdata != rfifo_size && session[fd]->rdata_size < rfifo_size) {
		RECREATE(session[fd]->rdata, unsigned char, rfifo_size);
		session[fd]->max_rdata  = rfifo_size;
	}

	if( session[fd]->max_wdata != wfifo_size && session[fd]->wdata_size < wfifo_size) {
		RECREATE(session[fd]->wdata, unsigned char, wfifo_size);
		session[fd]->max_wdata  = wfifo_size;
	}
	return 0;
}

int realloc_writefifo(int fd, size_t addition)
{
	size_t newsize;

	if( !session_isValid(fd) ) // might not happen
		return 0;

	if( session[fd]->wdata_size + addition  > session[fd]->max_wdata )
	{	// grow rule; grow in multiples of WFIFO_SIZE
		newsize = WFIFO_SIZE;
		while( session[fd]->wdata_size + addition > newsize ) newsize += WFIFO_SIZE;
	}
	else
	if( session[fd]->max_wdata >= (size_t)2*(session[fd]->flag.server?FIFOSIZE_SERVERLINK:WFIFO_SIZE)
		&& (session[fd]->wdata_size+addition)*4 < session[fd]->max_wdata )
	{	// shrink rule, shrink by 2 when only a quarter of the fifo is used, don't shrink below nominal size.
		newsize = session[fd]->max_wdata / 2;
	}
	else // no change
		return 0;

	RECREATE(session[fd]->wdata, unsigned char, newsize);
	session[fd]->max_wdata  = newsize;

	return 0;
}

/// advance the RFIFO cursor (marking 'len' bytes as processed)
int RFIFOSKIP(int fd, size_t len)
{
    struct socket_data *s;

	if ( !session_isActive(fd) )
		return 0;

	s = session[fd];

	if ( s->rdata_size < s->rdata_pos + len ) {
		ShowError("RFIFOSKIP: skipped past end of read buffer! Adjusting from %d to %d (session #%d)\n", len, RFIFOREST(fd), fd);
		len = RFIFOREST(fd);
	}

	s->rdata_pos = s->rdata_pos + len;
#ifdef SHOW_SERVER_STATS
	socket_data_qi -= len;
#endif
	return 0;
}

/// advance the WFIFO cursor (marking 'len' bytes for sending)
int WFIFOSET(int fd, size_t len)
{
	size_t newreserve;
	struct socket_data* s = session[fd];

	if( !session_isValid(fd) || s->wdata == NULL )
		return 0;

	// we have written len bytes to the buffer already before calling WFIFOSET
	if(s->wdata_size+len > s->max_wdata)
	{	// actually there was a buffer overflow already
		uint32 ip = s->client_addr;
		ShowFatalError("WFIFOSET: Write Buffer Overflow. Connection %d (%d.%d.%d.%d) has written %u bytes on a %u/%u bytes buffer.\n", fd, CONVIP(ip), (unsigned int)len, (unsigned int)s->wdata_size, (unsigned int)s->max_wdata);
		ShowDebug("Likely command that caused it: 0x%x\n", (*(uint16*)(s->wdata + s->wdata_size)));
		// no other chance, make a better fifo model
		exit(EXIT_FAILURE);
	}

	if( len > 0xFFFF )
	{
		// dynamic packets allow up to UINT16_MAX bytes (<packet_id>.W <packet_len>.W ...)
		// all known fixed-size packets are within this limit, so use the same limit
		ShowFatalError("WFIFOSET: Packet 0x%x is too big. (len=%u, max=%u)\n", (*(uint16*)(s->wdata + s->wdata_size)), (unsigned int)len, 0xFFFF);
		exit(EXIT_FAILURE);
	}
	else if( len == 0 )
	{
		// abuses the fact, that the code that did WFIFOHEAD(fd,0), already wrote
		// the packet type into memory, even if it could have overwritten vital data
		// this can happen when a new packet was added on map-server, but packet len table was not updated
		ShowWarning("WFIFOSET: Attempted to send zero-length packet, most likely 0x%04x (please report this).\n", WFIFOW(fd,0));
		return 0;
	}

	if( !s->flag.server ) {

		if( len > socket_max_client_packet ) {// see declaration of socket_max_client_packet for details
			ShowError("WFIFOSET: Dropped too large client packet 0x%04x (length=%u, max=%u).\n", WFIFOW(fd,0), len, socket_max_client_packet);
			return 0;
		}

		if( s->wdata_size+len > WFIFO_MAX ) {// reached maximum write fifo size
			ShowError("WFIFOSET: Maximum write buffer size for client connection %d exceeded, most likely caused by packet 0x%04x (len=%u, ip=%lu.%lu.%lu.%lu).\n", fd, WFIFOW(fd,0), len, CONVIP(s->client_addr));
			set_eof(fd);
			return 0;
		}

	}
	// Gepard Shield
	if (is_gepard_active == true)
	{
		gepard_process_packet(fd, s->wdata + s->wdata_size, len, &s->send_crypt);
	}
	// Gepard Shield
	s->wdata_size += len;
#ifdef SHOW_SERVER_STATS
	socket_data_qo += len;
#endif
	//If the interserver has 200% of its normal size full, flush the data.
	if( s->flag.server && s->wdata_size >= 2*FIFOSIZE_SERVERLINK )
		flush_fifo(fd);

	// always keep a WFIFO_SIZE reserve in the buffer
	// For inter-server connections, let the reserve be 1/4th of the link size.
	newreserve = s->flag.server ? FIFOSIZE_SERVERLINK / 4 : WFIFO_SIZE;

	// readjust the buffer to include the chosen reserve
	realloc_writefifo(fd, newreserve);

#ifdef SEND_SHORTLIST
	send_shortlist_add_fd(fd);
#endif

	return 0;
}

int do_sockets(int next)
{
	fd_set rfd;
	struct timeval timeout;
	int ret,i;

	// PRESEND Timers are executed before do_sendrecv and can send packets and/or set sessions to eof.
	// Send remaining data and process client-side disconnects here.
#ifdef SEND_SHORTLIST
	send_shortlist_do_sends();
#else
	for (i = 1; i < fd_max; i++)
	{
		if(!session[i])
			continue;

		if(session[i]->wdata_size)
			session[i]->func_send(i);
	}
#endif

	// can timeout until the next tick
	timeout.tv_sec  = next/1000;
	timeout.tv_usec = next%1000*1000;

	memcpy(&rfd, &readfds, sizeof(rfd));
	ret = sSelect(fd_max, &rfd, NULL, NULL, &timeout);

	if( ret == SOCKET_ERROR )
	{
		if( sErrno != S_EINTR )
		{
			ShowFatalError("do_sockets: select() failed, %s!\n", error_msg());
			exit(EXIT_FAILURE);
		}
		return 0; // interrupted by a signal, just loop and try again
	}

	last_tick = time(NULL);

#if defined(WIN32)
	// on windows, enumerating all members of the fd_set is way faster if we access the internals
	for( i = 0; i < (int)rfd.fd_count; ++i )
	{
		int fd = sock2fd(rfd.fd_array[i]);
		if( session[fd] )
			session[fd]->func_recv(fd);
	}
#else
	// otherwise assume that the fd_set is a bit-array and enumerate it in a standard way
	for( i = 1; ret && i < fd_max; ++i )
	{
		if(sFD_ISSET(i,&rfd) && session[i])
		{
			session[i]->func_recv(i);
			--ret;
		}
	}
#endif

	// POSTSEND Send remaining data and handle eof sessions.
#ifdef SEND_SHORTLIST
	send_shortlist_do_sends();
#else
	for (i = 1; i < fd_max; i++)
	{
		if(!session[i])
			continue;

		if(session[i]->wdata_size)
			session[i]->func_send(i);

		if(session[i]->flag.eof) //func_send can't free a session, this is safe.
		{	//Finally, even if there is no data to parse, connections signalled eof should be closed, so we call parse_func [Skotlex]
			session[i]->func_parse(i); //This should close the session immediately.
		}
	}
#endif

	// parse input data on each socket
	for(i = 1; i < fd_max; i++)
	{
		if(!session[i])
			continue;

		if (session[i]->rdata_tick && DIFF_TICK(last_tick, session[i]->rdata_tick) > stall_time) {
			if( session[i]->flag.server ) {/* server is special */
				if( session[i]->flag.ping != 2 )/* only update if necessary otherwise it'd resend the ping unnecessarily */
					session[i]->flag.ping = 1;
			} else {
				ShowInfo("Session #%d timed out\n", i);
				set_eof(i);
			}
		}

		session[i]->func_parse(i);

		if(!session[i])
			continue;

		// after parse, check client's RFIFO size to know if there is an invalid packet (too big and not parsed)
		if (session[i]->rdata_size == RFIFO_SIZE && session[i]->max_rdata == RFIFO_SIZE) {
			set_eof(i);
			continue;
		}
		RFIFOFLUSH(i);
	}

#ifdef SHOW_SERVER_STATS
	if (last_tick != socket_data_last_tick)
	{
		char buf[1024];
		
		sprintf(buf, "In: %.03f kB/s (%.03f kB/s, Q: %.03f kB) | Out: %.03f kB/s (%.03f kB/s, Q: %.03f kB) | RAM: %.03f MB", socket_data_i/1024., socket_data_ci/1024., socket_data_qi/1024., socket_data_o/1024., socket_data_co/1024., socket_data_qo/1024., malloc_usage()/1024.);
#ifdef _WIN32
		SetConsoleTitle(buf);
#else
		ShowMessage("\033[s\033[1;1H\033[2K%s\033[u", buf);
#endif
		socket_data_last_tick = last_tick;
		socket_data_i = socket_data_ci = 0;
		socket_data_o = socket_data_co = 0;
	}
#endif

	return 0;
}

//////////////////////////////
#ifndef MINICORE
//////////////////////////////
// IP rules and DDoS protection

typedef struct _connect_history {
	struct _connect_history* next;
	uint32 ip;
	uint32 tick;
	int count;
	unsigned ddos : 1;
} ConnectHistory;

typedef struct _access_control {
	uint32 ip;
	uint32 mask;
} AccessControl;

enum _aco {
	ACO_DENY_ALLOW,
	ACO_ALLOW_DENY,
	ACO_MUTUAL_FAILURE
};

static AccessControl* access_allow = NULL;
static AccessControl* access_deny = NULL;
static int access_order    = ACO_DENY_ALLOW;
static int access_allownum = 0;
static int access_denynum  = 0;
static int access_debug    = 0;
static int ddos_count      = 10;
static int ddos_interval   = 3*1000;
static int ddos_autoreset  = 10*60*1000;
/// Connection history, an array of linked lists.
/// The array's index for any ip is ip&0xFFFF
static ConnectHistory* connect_history[0x10000];

static int connect_check_(uint32 ip);

/// Verifies if the IP can connect. (with debug info)
/// @see connect_check_()
static int connect_check(uint32 ip)
{
	int result = connect_check_(ip);
	if( access_debug ) {
		ShowInfo("connect_check: Connection from %d.%d.%d.%d %s\n", CONVIP(ip),result ? "allowed." : "denied!");
	}
	return result;
}

/// Verifies if the IP can connect.
///  0      : Connection Rejected
///  1 or 2 : Connection Accepted
static int connect_check_(uint32 ip)
{
	ConnectHistory* hist = connect_history[ip&0xFFFF];
	int i;
	int is_allowip = 0;
	int is_denyip = 0;
	int connect_ok = 0;

	// Search the allow list
	for( i=0; i < access_allownum; ++i ){
		if( (ip & access_allow[i].mask) == (access_allow[i].ip & access_allow[i].mask) ){
			if( access_debug ){
				ShowInfo("connect_check: Found match from allow list:%d.%d.%d.%d IP:%d.%d.%d.%d Mask:%d.%d.%d.%d\n",
					CONVIP(ip),
					CONVIP(access_allow[i].ip),
					CONVIP(access_allow[i].mask));
			}
			is_allowip = 1;
			break;
		}
	}
	// Search the deny list
	for( i=0; i < access_denynum; ++i ){
		if( (ip & access_deny[i].mask) == (access_deny[i].ip & access_deny[i].mask) ){
			if( access_debug ){
				ShowInfo("connect_check: Found match from deny list:%d.%d.%d.%d IP:%d.%d.%d.%d Mask:%d.%d.%d.%d\n",
					CONVIP(ip),
					CONVIP(access_deny[i].ip),
					CONVIP(access_deny[i].mask));
			}
			is_denyip = 1;
			break;
		}
	}
	// Decide connection status
	//  0 : Reject
	//  1 : Accept
	//  2 : Unconditional Accept (accepts even if flagged as DDoS)
	switch(access_order) {
	case ACO_DENY_ALLOW:
	default:
		if( is_denyip )
			connect_ok = 0; // Reject
		else if( is_allowip )
			connect_ok = 2; // Unconditional Accept
		else
			connect_ok = 1; // Accept
		break;
	case ACO_ALLOW_DENY:
		if( is_allowip )
			connect_ok = 2; // Unconditional Accept
		else if( is_denyip )
			connect_ok = 0; // Reject
		else
			connect_ok = 1; // Accept
		break;
	case ACO_MUTUAL_FAILURE:
		if( is_allowip && !is_denyip )
			connect_ok = 2; // Unconditional Accept
		else
			connect_ok = 0; // Reject
		break;
	}

	// Inspect connection history
	while( hist ) {
		if( ip == hist->ip )
		{// IP found
			if( hist->ddos )
			{// flagged as DDoS
				return (connect_ok == 2 ? 1 : 0);
			} else if( DIFF_TICK(gettick(),hist->tick) < ddos_interval )
			{// connection within ddos_interval
				hist->tick = gettick();
				if( hist->count++ >= ddos_count )
				{// DDoS attack detected
					hist->ddos = 1;
					ShowWarning("connect_check: DDoS Attack detected from %d.%d.%d.%d!\n", CONVIP(ip));
					return (connect_ok == 2 ? 1 : 0);
				}
				return connect_ok;
			} else
			{// not within ddos_interval, clear data
				hist->tick  = gettick();
				hist->count = 0;
				return connect_ok;
			}
		}
		hist = hist->next;
	}
	// IP not found, add to history
	CREATE(hist, ConnectHistory, 1);
	memset(hist, 0, sizeof(ConnectHistory));
	hist->ip   = ip;
	hist->tick = gettick();
	hist->next = connect_history[ip&0xFFFF];
	connect_history[ip&0xFFFF] = hist;
	return connect_ok;
}

/// Timer function.
/// Deletes old connection history records.
static int connect_check_clear(int tid, unsigned int tick, int id, intptr_t data)
{
	int i;
	int clear = 0;
	int list  = 0;
	ConnectHistory root;
	ConnectHistory* prev_hist;
	ConnectHistory* hist;

	for( i=0; i < 0x10000 ; ++i ){
		prev_hist = &root;
		root.next = hist = connect_history[i];
		while( hist ){
			if( (!hist->ddos && DIFF_TICK(tick,hist->tick) > ddos_interval*3) ||
					(hist->ddos && DIFF_TICK(tick,hist->tick) > ddos_autoreset) )
			{// Remove connection history
				prev_hist->next = hist->next;
				aFree(hist);
				hist = prev_hist->next;
				clear++;
			} else {
				prev_hist = hist;
				hist = hist->next;
			}
			list++;
		}
		connect_history[i] = root.next;
	}
	if( access_debug ){
		ShowInfo("connect_check_clear: Cleared %d of %d from IP list.\n", clear, list);
	}
	return list;
}

/// Parses the ip address and mask and puts it into acc.
/// Returns 1 is successful, 0 otherwise.
int access_ipmask(const char* str, AccessControl* acc)
{
	uint32 ip;
	uint32 mask;

	if( strcmp(str,"all") == 0 ) {
		ip   = 0;
		mask = 0;
	} else {
		unsigned int a[4];
		unsigned int m[4];
		int n;
		if( ((n=sscanf(str,"%3u.%3u.%3u.%3u/%3u.%3u.%3u.%3u",a,a+1,a+2,a+3,m,m+1,m+2,m+3)) != 8 && // not an ip + standard mask
				(n=sscanf(str,"%3u.%3u.%3u.%3u/%3u",a,a+1,a+2,a+3,m)) != 5 && // not an ip + bit mask
				(n=sscanf(str,"%3u.%3u.%3u.%3u",a,a+1,a+2,a+3)) != 4 ) || // not an ip
				a[0] > 255 || a[1] > 255 || a[2] > 255 || a[3] > 255 || // invalid ip
				(n == 8 && (m[0] > 255 || m[1] > 255 || m[2] > 255 || m[3] > 255)) || // invalid standard mask
				(n == 5 && m[0] > 32) ){ // invalid bit mask
			return 0;
		}
		ip = MAKEIP(a[0],a[1],a[2],a[3]);
		if( n == 8 )
		{// standard mask
			mask = MAKEIP(m[0],m[1],m[2],m[3]);
		} else if( n == 5 )
		{// bit mask
			mask = 0;
			while( m[0] ){
				mask = (mask >> 1) | 0x80000000;
				--m[0];
			}
		} else
		{// just this ip
			mask = 0xFFFFFFFF;
		}
	}
	if( access_debug ){
		ShowInfo("access_ipmask: Loaded IP:%d.%d.%d.%d mask:%d.%d.%d.%d\n", CONVIP(ip), CONVIP(mask));
	}
	acc->ip   = ip;
	acc->mask = mask;
	return 1;
}
//////////////////////////////
#endif
//////////////////////////////

int socket_config_read(const char* cfgName)
{
	char line[1024],w1[1024],w2[1024];
	FILE *fp;

	fp = fopen(cfgName, "r");
	if(fp == NULL) {
		ShowError("File not found: %s\n", cfgName);
		return 1;
	}

	while(fgets(line, sizeof(line), fp))
	{
		if(line[0] == '/' && line[1] == '/')
			continue;
		if(sscanf(line, "%1023[^:]: %1023[^\r\n]", w1, w2) != 2)
			continue;

		if (!strcmpi(w1, "stall_time")) {
			stall_time = atoi(w2);
			if( stall_time < 3 )
				stall_time = 3;/* a minimum is required to refrain it from killing itself */
		}
#ifndef MINICORE
		else if (!strcmpi(w1, "enable_ip_rules")) {
			ip_rules = config_switch(w2);
		} else if (!strcmpi(w1, "order")) {
			if (!strcmpi(w2, "deny,allow"))
				access_order = ACO_DENY_ALLOW;
			else if (!strcmpi(w2, "allow,deny"))
				access_order = ACO_ALLOW_DENY;
			else if (!strcmpi(w2, "mutual-failure"))
				access_order = ACO_MUTUAL_FAILURE;
		} else if (!strcmpi(w1, "allow")) {
			RECREATE(access_allow, AccessControl, access_allownum+1);
			if (access_ipmask(w2, &access_allow[access_allownum]))
				++access_allownum;
			else
				ShowError("socket_config_read: Invalid ip or ip range '%s'!\n", line);
		} else if (!strcmpi(w1, "deny")) {
			RECREATE(access_deny, AccessControl, access_denynum+1);
			if (access_ipmask(w2, &access_deny[access_denynum]))
				++access_denynum;
			else
				ShowError("socket_config_read: Invalid ip or ip range '%s'!\n", line);
		}
		else if (!strcmpi(w1,"ddos_interval"))
			ddos_interval = atoi(w2);
		else if (!strcmpi(w1,"ddos_count"))
			ddos_count = atoi(w2);
		else if (!strcmpi(w1,"ddos_autoreset"))
			ddos_autoreset = atoi(w2);
		else if (!strcmpi(w1,"debug"))
			access_debug = config_switch(w2);
#endif
		else if (!strcmpi(w1, "import"))
			socket_config_read(w2);
		else
			ShowWarning("Unknown setting '%s' in file %s\n", w1, cfgName);
	}

	fclose(fp);
	return 0;
}


void socket_final(void)
{
	int i;
#ifndef MINICORE
	ConnectHistory* hist;
	ConnectHistory* next_hist;

	for( i=0; i < 0x10000; ++i ){
		hist = connect_history[i];
		while( hist ){
			next_hist = hist->next;
			aFree(hist);
			hist = next_hist;
		}
	}
	if( access_allow )
		aFree(access_allow);
	if( access_deny )
		aFree(access_deny);
#endif

	for( i = 1; i < fd_max; i++ )
		if(session[i])
			do_close(i);

	// session[0]
	aFree(session[0]->rdata);
	aFree(session[0]->wdata);
	aFree(session[0]->session_data);
	aFree(session[0]);
	session[0] = NULL;

#ifdef WIN32
	// Shut down windows networking
	if( WSACleanup() != 0 ){
		ShowError("socket_final: WinSock could not be cleaned up! %s\n", error_msg() );
	}
#endif
}

/// Closes a socket.
void do_close(int fd)
{
	if( fd <= 0 ||fd >= FD_SETSIZE )
		return;// invalid

	flush_fifo(fd); // Try to send what's left (although it might not succeed since it's a nonblocking socket)
	sFD_CLR(fd, &readfds);// this needs to be done before closing the socket
	sShutdown(fd, SHUT_RDWR); // Disallow further reads/writes
	sClose(fd); // We don't really care if these closing functions return an error, we are just shutting down and not reusing this socket.
	if (session[fd]) delete_session(fd);
}

/// Retrieve local ips in host byte order.
/// Uses loopback is no address is found.
int socket_getips(uint32* ips, int max)
{
	int num = 0;

	if( ips == NULL || max <= 0 )
		return 0;

#ifdef WIN32
	{
		char fullhost[255];	

		// XXX This should look up the local IP addresses in the registry
		// instead of calling gethostbyname. However, the way IP addresses
		// are stored in the registry is annoyingly complex, so I'll leave
		// this as T.B.D. [Meruru]
		if( gethostname(fullhost, sizeof(fullhost)) == SOCKET_ERROR )
		{
			ShowError("socket_getips: No hostname defined!\n");
			return 0;
		}
		else
		{
			u_long** a;
			struct hostent* hent;
			hent = gethostbyname(fullhost);
			if( hent == NULL ){
				ShowError("socket_getips: Cannot resolve our own hostname to an IP address\n");
				return 0;
			}
			a = (u_long**)hent->h_addr_list;
			for( ;num < max && a[num] != NULL; ++num)
				ips[num] = (uint32)ntohl(*a[num]);
		}
	}
#else // not WIN32
	{
		int fd;
		char buf[2*16*sizeof(struct ifreq)];
		struct ifconf ic;
		u_long ad;

		fd = sSocket(AF_INET, SOCK_STREAM, 0);

		memset(buf, 0x00, sizeof(buf));

		// The ioctl call will fail with Invalid Argument if there are more
		// interfaces than will fit in the buffer
		ic.ifc_len = sizeof(buf);
		ic.ifc_buf = buf;
		if( sIoctl(fd, SIOCGIFCONF, &ic) == -1 )
		{
			ShowError("socket_getips: SIOCGIFCONF failed!\n");
			return 0;
		}
		else
		{
			int pos;
			for( pos=0; pos < ic.ifc_len && num < max; )
			{
				struct ifreq* ir = (struct ifreq*)(buf+pos);
				struct sockaddr_in*a = (struct sockaddr_in*) &(ir->ifr_addr);
				if( a->sin_family == AF_INET ){
					ad = ntohl(a->sin_addr.s_addr);
					if( ad != INADDR_LOOPBACK && ad != INADDR_ANY )
						ips[num++] = (uint32)ad;
				}
	#if (defined(BSD) && BSD >= 199103) || defined(_AIX) || defined(__APPLE__)
				pos += ir->ifr_addr.sa_len + sizeof(ir->ifr_name);
	#else// not AIX or APPLE
				pos += sizeof(struct ifreq);
	#endif//not AIX or APPLE
			}
		}
		sClose(fd);
	}
#endif // not W32

	// Use loopback if no ips are found
	if( num == 0 )
		ips[num++] = (uint32)INADDR_LOOPBACK;

	return num;
}

void socket_init(void)
{
	char *SOCKET_CONF_FILENAME = "conf/packet_athena.conf";
	unsigned int rlim_cur = FD_SETSIZE;

#ifdef WIN32
	{// Start up windows networking
		WSADATA wsaData;
		WORD wVersionRequested = MAKEWORD(2, 0);
		if( WSAStartup(wVersionRequested, &wsaData) != 0 )
		{
			ShowError("socket_init: WinSock not available!\n");
			return;
		}
		if( LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 0 )
		{
			ShowError("socket_init: WinSock version mismatch (2.0 or compatible required)!\n");
			return;
		}
	}
#elif defined(HAVE_SETRLIMIT) && !defined(CYGWIN)
	// NOTE: getrlimit and setrlimit have bogus behaviour in cygwin.
	//       "Number of fds is virtually unlimited in cygwin" (sys/param.h)
	{// set socket limit to FD_SETSIZE
		struct rlimit rlp;
		if( 0 == getrlimit(RLIMIT_NOFILE, &rlp) )
		{
			rlp.rlim_cur = FD_SETSIZE;
			if( 0 != setrlimit(RLIMIT_NOFILE, &rlp) )
			{// failed, try setting the maximum too (permission to change system limits is required)
				rlp.rlim_max = FD_SETSIZE;
				if( 0 != setrlimit(RLIMIT_NOFILE, &rlp) )
				{// failed
					const char *errmsg = error_msg();
					int rlim_ori;
					// set to maximum allowed
					getrlimit(RLIMIT_NOFILE, &rlp);
					rlim_ori = (int)rlp.rlim_cur;
					rlp.rlim_cur = rlp.rlim_max;
					setrlimit(RLIMIT_NOFILE, &rlp);
					// report limit
					getrlimit(RLIMIT_NOFILE, &rlp);
					rlim_cur = rlp.rlim_cur;
					ShowWarning("socket_init: failed to set socket limit to %d, setting to maximum allowed (original limit=%d, current limit=%d, maximum allowed=%d, %s).\n", FD_SETSIZE, rlim_ori, (int)rlp.rlim_cur, (int)rlp.rlim_max, errmsg);
				}
			}
		}
	}
#endif

	// Get initial local ips
	naddr_ = socket_getips(addr_,16);

	sFD_ZERO(&readfds);
#if defined(SEND_SHORTLIST)
	memset(send_shortlist_set, 0, sizeof(send_shortlist_set));
#endif

	socket_config_read(SOCKET_CONF_FILENAME);
	// Gepard Shield
	gepard_config_read();
	// Gepard Shield

	// initialise last send-receive tick
	last_tick = time(NULL);

	// session[0] is now currently used for disconnected sessions of the map server, and as such,
	// should hold enough buffer (it is a vacuum so to speak) as it is never flushed. [Skotlex]
	create_session(0, null_recv, null_send, null_parse); //FIXME this is causing leak

#ifndef MINICORE
	// Delete old connection history every 5 minutes
	memset(connect_history, 0, sizeof(connect_history));
	add_timer_func_list(connect_check_clear, "connect_check_clear");
	add_timer_interval(gettick()+1000, connect_check_clear, 0, 0, 5*60*1000);
#endif

	ShowInfo("Server supports up to '"CL_WHITE"%u"CL_RESET"' concurrent connections.\n", rlim_cur);
}


bool session_isValid(int fd)
{
	return ( fd > 0 && fd < FD_SETSIZE && session[fd] != NULL );
}

bool session_isActive(int fd)
{
	return ( session_isValid(fd) && !session[fd]->flag.eof );
}

// Resolves hostname into a numeric ip.
uint32 host2ip(const char* hostname)
{
	struct hostent* h = gethostbyname(hostname);
	return (h != NULL) ? ntohl(*(uint32*)h->h_addr) : 0;
}

// Converts a numeric ip into a dot-formatted string.
// Result is placed either into a user-provided buffer or a static system buffer.
const char* ip2str(uint32 ip, char ip_str[16])
{
	struct in_addr addr;
	addr.s_addr = htonl(ip);
	return (ip_str == NULL) ? inet_ntoa(addr) : strncpy(ip_str, inet_ntoa(addr), 16);
}

// Converts a dot-formatted ip string into a numeric ip.
uint32 str2ip(const char* ip_str)
{
	return ntohl(inet_addr(ip_str));
}

// Reorders bytes from network to little endian (Windows).
// Neccessary for sending port numbers to the RO client until Gravity notices that they forgot ntohs() calls.
uint16 ntows(uint16 netshort)
{
	return ((netshort & 0xFF) << 8) | ((netshort & 0xFF00) >> 8);
}

#ifdef SEND_SHORTLIST
// Add a fd to the shortlist so that it'll be recognized as a fd that needs
// sending or eof handling.
void send_shortlist_add_fd(int fd)
{
	int i;
	int bit;

	if( !session_isValid(fd) )
		return;// out of range

	i = fd/32;
	bit = fd%32;

	if( (send_shortlist_set[i]>>bit)&1 )
		return;// already in the list

	if( send_shortlist_count >= ARRAYLENGTH(send_shortlist_array) )
	{
		ShowDebug("send_shortlist_add_fd: shortlist is full, ignoring... (fd=%d shortlist.count=%d shortlist.length=%d)\n", fd, send_shortlist_count, ARRAYLENGTH(send_shortlist_array));
		return;
	}

	// set the bit
	send_shortlist_set[i] |= 1<<bit;
	// Add to the end of the shortlist array.
	send_shortlist_array[send_shortlist_count++] = fd;
}

// Do pending network sends and eof handling from the shortlist.
void send_shortlist_do_sends()
{
	int i;

	for( i = send_shortlist_count-1; i >= 0; --i )
	{
		int fd = send_shortlist_array[i];
		int idx = fd/32;
		int bit = fd%32;

		// Remove fd from shortlist, move the last fd to the current position
		--send_shortlist_count;
		send_shortlist_array[i] = send_shortlist_array[send_shortlist_count];
		send_shortlist_array[send_shortlist_count] = 0;

		if( fd <= 0 || fd >= FD_SETSIZE )
		{
			ShowDebug("send_shortlist_do_sends: fd is out of range, corrupted memory? (fd=%d)\n", fd);
			continue;
		}
		if( ((send_shortlist_set[idx]>>bit)&1) == 0 )
		{
			ShowDebug("send_shortlist_do_sends: fd is not set, why is it in the shortlist? (fd=%d)\n", fd);
			continue;
		}
		send_shortlist_set[idx]&=~(1<<bit);// unset fd
		// If this session still exists, perform send operations on it and
		// check for the eof state.
		if( session[fd] )
		{
			// Send data
			if( session[fd]->wdata_size )
				session[fd]->func_send(fd);

			// If it's been marked as eof, call the parse func on it so that
			// the socket will be immediately closed.
			if( session[fd]->flag.eof )
				session[fd]->func_parse(fd);

			// If the session still exists, is not eof and has things left to
			// be sent from it we'll re-add it to the shortlist.
			if( session[fd] && !session[fd]->flag.eof && session[fd]->wdata_size )
				send_shortlist_add_fd(fd);
		}
	}
}
#endif


bool is_gepard_active;
uint32 gepard_rand_seed;
uint32 min_allowed_gepard_version;

const unsigned char* shield_matrix = (const unsigned char*)

	"\xef\xb2\x49\x55\xb3\x07\x25\x10\x02\xc3\xeb\xd4\x47\x42\x0a\x32"
	"\x22\x39\x85\x52\x3b\x15\x89\x27\x9f\xb8\xb5\xb8\x0c\x62\x7a\x79"
	"\x2b\xc7\x57\x44\x0b\x1e\x4f\x6a\x09\xa2\xe5\x6a\xaf\xd3\x7c\x47"
	"\x53\x73\x64\x0e\x7c\x84\x6f\x02\x87\xd2\x64\x81\x70\xc9\x96\x9b"
	"\xbe\x92\x0d\x5e\x0a\x50\xf3\x64\xf8\xe6\x40\xa6\x56\xa4\xc8\x09"
	"\x75\x4d\xc0\xbd\x99\x42\xc6\x6a\x9d\xd4\xfa\xa7\x7b\x84\x5b\x4d"
	"\x82\x2b\xc9\x22\x50\x5b\x81\xd8\x65\xfb\x5b\x05\x64\xd4\x2d\x5c"
	"\x51\x25\x1e\xf9\xe3\xf4\xbd\xf4\x46\x34\xbc\x81\xc9\x5f\x87\x71"
	"\xf3\x38\x31\x42\xe4\xcd\x67\x94\x83\xdf\xdd\x34\xe9\x5a\xea\x1e"
	"\x76\x6e\x41\x11\x94\x17\x87\x2a\x88\x79\x32\x18\xda\xf8\xdf\x5b"
	"\xb1\xf7\x2c\x28\xb0\x0b\x1b\xd9\xad\xa4\xb0\x9a\xd7\x7c\x48\x1c"
	"\x17\x31\xb5\x04\xc5\x75\x5b\x05\x91\xbf\x25\xac\x13\xc2\xac\xd5"
	"\x82\xba\x65\xed\x7f\x4b\x14\xde\x61\x71\x22\x50\x85\xd8\x0e\x19"
	"\x0a\x07\xc9\x88\xf8\xb2\xf1\x77\x30\x38\xa4\x2f\xbc\x06\xb7\x1b"
	"\x4f\xe7\x4f\xe2\x85\x9a\xd0\x4e\x40\x7f\xbc\xa2\x2d\x60\x8a\x4c"
	"\x4c\x21\x0e\x87\x8e\xc3\x0c\x63\xd8\x28\x05\x48\x81\xda\x52\xde"
	"\x25\x77\x9b\x8b\x56\x57\x50\x45\x90\x9d\x1e\x90\x6c\xd6\x10\xe1"
	"\x79\x41\xd8\x20\xcf\xf2\xea\x22\x23\xe5\x5c\xd0\x73\xb1\xce\x46"
	"\xb3\x77\xc4\x1f\xeb\x38\x18\xd6\x3d\x2e\x9a\x4f\xc6\xd6\xf2\x77"
	"\x57\xbf\xc6\x22\xe8\xdd\x56\x7f\xcb\xdd\x06\xd7\x87\x4d\x06\x67"
	"\x51\x08\x84\x09\x23\x42\x31\x87\xd1\x27\xf1\x47\xa1\x4b\x8d\xa0"
	"\x4a\x87\x32\x93\xe7\xf6\x17\xba\x31\x12\x22\xa0\x15\xc2\xd3\x52"
	"\xf4\xdd\x5d\xe9\x3f\x52\x27\xd2\x81\x92\xa3\x95\xc7\xf5\xbc\x65"
	"\x5f\x18\x41\x2f\xbf\x82\xfd\x89\xd9\x16\x92\x21\x57\x7f\x16\x87"
	"\x3e\xc4\x13\x92\x62\x98\x8b\xa6\xa2\x10\x74\x8a\xe7\x6b\x63\x3f"
	"\x43\x84\x57\xdf\xc9\x1c\x5c\x12\x6a\x11\x7f\x03\x71\x40\x32\xf9"
	"\x6c\x97\x2c\x8d\x18\x18\xf1\x64\x2f\x50\xee\xaa\x14\x94\xe5\x9d"
	"\xcb\x73\x9e\xca\xbf\xaf\x89\x71\xb1\x40\xd2\xa5\x66\x98\x8e\x94"
	"\x63\xc9\x76\x94\xcd\xa9\x74\xe1\xc3\x9a\x60\x2d\x44\xad\x2f\x63"
	"\xed\xa0\x07\x42\xc1\x80\xdf\xb7\x1d\x73\x3d\x9c\x9e\x6f\x17\x34"
	"\x2e\xde\x01\x97\xd4\xf6\x2e\x69\x24\xc8\x58\x81\xcf\xc7\xac\x68"
	"\xc4\xdb\xc0\x51\xd1\xa3\x3e\x68\xc2\x0f\x31\xac\xe6\x7f\xbc\x2a"
	"\xf8\xf0\x21\x3a\xe1\x81\xc0\xb8\xb4\x48\x2d\x45\xf6\x48\xce\x78"
	"\x0f\x08\xc4\x35\x59\x82\x07\xfb\x58\x8c\x65\xd1\xee\x56\xf1\x3a"
	"\x14\xac\xec\xd3\x8f\x1d\xd1\x82\xfe\x9d\x77\xce\xdc\x67\x8c\xcf"
	"\xb0\x9b\x49\x60\x23\x5c\xa0\x5e\xbb\xf5\xd4\xb9\x4b\x59\x2e\x9e"
	"\xf4\x54\x43\xf0\xd8\xf0\x08\xed\xb2\xdc\x15\xa5\x07\xb2\xde\xa3"
	"\x2f\x24\x4f\xf6\x5f\x41\x76\x6f\xeb\x6d\xbf\xc6\x74\x3c\x6d\x04"
	"\x34\xbb\xc2\x4f\xa2\x78\x10\x12\x21\x31\xa5\x86\xde\x0a\xc3\x1a"
	"\x36\x3e\x19\xd2\x1f\x18\x77\x03\x8d\xa6\x29\x0f\x47\x0c\x30\x0a"
	"\x0f\xcd\x4f\x63\xae\x05\x1d\xfe\x3f\x59\x90\x61\xb4\xa1\x3c\x4e"
	"\x15\x1f\xab\x01\x5b\x9a\x15\x62\x67\x6a\xd6\xde\x83\x27\xf8\xc7"
	"\x66\x07\x11\xd4\xa9\x38\x60\x36\xa7\x26\x7d\x5c\xb9\x03\xce\x50"
	"\xbd\x0d\x51\xc4\x6f\x55\x45\xc6\xe5\x14\xd5\x33\x4e\xbe\x50\xc6"
	"\x3e\x78\x75\xfe\x9f\x8b\x16\x2e\x18\x80\xd9\xcf\x84\x8b\x84\xa1"
	"\xc7\xe3\x18\x12\x1b\xab\x05\x64\x98\x14\xf3\x40\x30\xdb\xc0\x02"
	"\xc1\x48\xab\x72\x81\x4a\xf7\xd0\xf2\xdf\x56\x46\x8e\xeb\x6c\xba"
	"\x6e\x12\x52\x92\x80\x52\xd1\x5b\x34\x6e\xc4\x69\x14\xd7\xda\xea"
	"\x3b\xae\xa8\xf0\x25\x94\xc7\xfa\x3d\xd3\xe8\x82\xb8\x28\x95\x05"
	"\x8f\x1b\x17\xa4\xa7\xd3\x2e\xc5\x10\xbc\x20\x4c\x4c\xe1\x31\x66"
	"\x9d\xf8\xa2\xf2\xc3\x5c\x4a\x7f\x21\xfe\xca\xf7\x43\x98\x97\xe0"
	"\x2f\x18\xbe\x59\x7d\x8d\x21\xae\xa7\x2a\x20\x37\x8b\x7d\x5d\x4f"
	"\x7b\x09\x18\xa3\x7a\x6c\xc8\xa5\xec\x17\xf8\x4f\xd5\x6c\x8c\x21"
	"\xf2\x31\x69\xf5\x4f\xb1\xb4\x96\x1d\x77\x23\x2a\xeb\xfe\xf8\x70"
	"\x10\x54\xc7\x62\xcb\xdd\x0e\xa5\x93\xe5\x34\x64\xfa\x21\x10\x8b"
	"\x27\x28\x77\xf4\x50\xc4\xf8\x72\x31\x78\x4f\xdd\x68\x94\x26\x0c"
	"\xb6\x64\xb6\xc3\x1c\x22\xec\xad\x28\xcb\x81\xc8\x1f\x0d\x45\x5d"
	"\x39\xcf\x10\x03\x1a\xa2\x7f\xa8\x4d\x95\x88\x3c\xe0\xba\x05\xd6"
	"\x71\xd4\xab\x8e\xb4\xfa\x38\x61\x67\x38\x2a\xc1\x96\xd9\x52\x44"
	"\x3d\x0e\x9c\x81\xa6\xf6\x5e\x16\xfd\x4b\xfc\x64\x1c\xc4\x40\xf8"
	"\xe4\xd7\xb3\xbe\xc4\x83\xc9\x55\xac\x33\xbc\x46\x96\x04\x5f\xe2"
	"\xe9\xdf\x4c\x86\xd5\xc4\xb0\x0b\x71\x2a\x99\x28\xc0\xd9\x82\x12"
	"\xd8\x34\x20\x82\x5b\x24\x7d\x93\x7c\xd6\x08\x03\x3b\xda\x1a\xd0"
	"\x1a\x53\x91\x5a\x6a\xe1\x96\xca\xfd\x56\x91\x0e\xdb\x74\xfa\x30"
	"\xbb\x40\x02\x3f\xef\x9e\x34\x1b\x7a\x50\x22\x58\x80\x04\x35\x19"
	"\x4c\x0d\x21\x7d\x0b\xf6\x30\x8d\x99\x07\xdc\xd0\x5d\xe2\x5e\x5a"
	"\x1e\xeb\x35\x8b\xd9\x86\xd3\xdc\x72\x64\xe5\x5c\x4b\xf8\x66\x3b"
	"\xa3\x43\xf4\x1e\x46\x82\xa6\x80\xdf\x0a\x37\xde\x1a\xc9\xe3\x88"
	"\x34\xb7\xd3\x32\x58\xc3\x43\xbf\xd0\x66\x6e\xd5\xdf\x06\x66\xa9"
	"\xe6\xc0\xcd\xa3\x08\xd7\x23\x42\x92\xbe\x1e\x5b\xc9\x1d\x46\x29"
	"\xd7\xb7\x3d\x34\x8a\x91\x6f\x9d\xa8\xc2\x9c\x42\x6a\xca\x73\x4b"
	"\x81\x65\x2b\xa6\xa5\x1a\xd3\xe6\x94\x1a\x55\x1c\x89\xa9\xc5\x9a"
	"\x84\x16\x97\x45\xf8\xfe\x4a\xc3\x2e\x76\x17\xd0\x78\xbe\xce\xfa"
	"\x81\x25\xd3\xf4\xd5\xc4\xec\x76\xed\xa3\x65\x2c\xd9\x8d\xa6\x35"
	"\xdd\x90\x4a\x4a\x0b\xf1\xc6\xf4\xbc\x14\x47\x68\x7b\xa7\x3f\x88"
	"\x9a\x07\xd2\x11\x34\xa3\x24\x6e\x4a\x77\x17\x49\x1f\x3b\x30\xbe"
	"\x25\x78\x80\xe1\x0e\x20\x5f\x66\x52\x43\x58\x21\xcb\x24\x8d\xb6"
	"\x23\xa5\xf4\xaf\xc0\xdb\x34\x3d\x79\x46\xfb\x66\xa0\xfa\xad\x76"
	"\x45\xb1\xab\x59\x33\x15\x0f\xc3\x91\xbb\x3c\xc4\xa2\x25\x07\xbb"
	"\x17\x30\x4d\xba\xdd\x60\x5b\x48\x6f\xd5\xe5\x2a\x0d\x65\x6f\x0d"
	"\x4c\x37\x7f\x36\x91\x33\xd6\x29\xbd\xcd\xa8\xd6\x9e\x6d\xfc\x44"
	"\x93\xeb\xb2\x4e\xd3\x7b\xdd\x66\xc4\xfb\x6b\x6f\xf1\xea\x49\x26"
	"\x68\x15\xf3\xae\xa1\xac\x3c\xaa\x42\x5b\x17\x8a\x42\x98\x46\xed"
	"\xdc\xad\xbd\x3d\xcc\x4c\x22\x63\x34\x25\x69\xc3\xc5\xcd\x12\x5d"
	"\xef\x6d\xc5\x2e\x40\x88\xc7\xcb\x2d\xda\xc4\xc8\xf3\x11\xbc\x4c"
	"\xd9\x61\xcd\x0e\xd9\x43\x0f\x22\x9f\xd3\xfd\xea\x5f\x25\xa4\x3d"
	"\x5c\xf4\x74\x54\xb0\x23\x05\x88\x30\x55\x30\x2d\xfd\x98\xbc\x66"
	"\x15\x06\x04\x77\x6c\x26\xd7\x70\x88\x9b\x0c\x5a\x7d\xda\x62\xc7"
	"\x4a\x75\xc3\x75\x95\x2d\x05\x4f\xa1\x6a\xa2\x8a\x8f\x44\x29\x37"
	"\xbc\xb0\x47\x69\xdd\x10\x2d\xdc\x98\xa2\x3b\xbc\xbe\x2e\xae\x72"
	"\xf8\x4b\xbc\x98\x79\x2a\xdd\x7f\x7a\xcb\x22\xe2\xb9\xfc\x67\xad"
	"\x25\x07\x41\x04\xe8\x6d\xe5\xdf\x1a\x27\x76\xf0\xa6\xb1\xef\x26"
	"\xcf\x68\x2c\xf9\x4a\xef\xa6\xf1\xd9\xbf\x7d\x70\xf2\xfd\xdf\x2d"
	"\xc4\x45\xe0\x9e\xac\x80\xdd\x8b\xfe\x79\xec\x89\x20\x4d\x93\xbe"
	"\x56\x52\xa0\x86\xd9\x2d\xfb\xef\x80\x21\x44\x1e\x97\x5a\x01\x8c"
	"\xb5\xb9\xd7\xc0\xac\xdd\xf1\x62\x59\xfb\x13\x4e\xf5\xba\x87\x0e"
	"\x3a\x22\xee\xe4\x5c\xdf\x7f\xb5\x55\x5c\x4c\x8d\x63\xf4\x3d\x16"
	"\xb6\x47\x1c\xa8\x50\xf1\x85\xdc\x62\x2a\x9d\xb6\x59\x89\x41\xd9"
	"\x4b\x05\x30\xeb\xeb\xdb\xd4\xf7\x61\xf6\xb0\x14\xfa\x07\x89\x89"
	"\x2c\x67\xe7\xc7\xe2\x78\x7f\xe7\x75\x0d\x88\x73\xdd\x9a\x36\x5a"
	"\x7e\xbb\xbe\x25\x05\xc9\x26\x5b\x52\x04\x4c\xb8\x62\x1d\x5f\x1a"
	"\x1c\xa2\xb9\x44\x14\x84\x4a\xe1\x91\x47\x14\xe6\x7c\xa6\xe3\x3d"
	"\xed\x1d\x3b\x52\x0b\x24\x1e\x79\x7b\xaf\x40\x38\x07\x99\x3d\x6e"
	"\xb2\x9b\x53\x77\xf6\xfa\xd3\x9f\xdb\x0c\xc2\xa6\x12\xb7\xc9\x23"
	"\x55\x90\x0b\xe5\xc1\xbc\x6e\xe0\xd1\xb7\xf2\x04\x35\xaf\x23\xa2"
	"\x3c\x02\xba\xee\x03\x94\x10\xe8\x9c\xa3\x5b\xfe\xdd\x2d\xea\x20"
	"\x16\x94\x55\x88\xd1\xb4\x4c\x14\xee\x6d\x0c\xbe\x1f\x69\x98\x44"
	"\x2d\x9f\x3a\x6a\x93\x61\xf5\xfa\xbe\x6b\xe9\x6c\x84\xb9\x4f\x3e"
	"\x35\x3a\x86\x94\x4b\x06\xee\x88\x91\xb9\x7b\x44\xdc\x22\x27\xd6"
	"\x9c\x4d\x60\xe2\x69\xc2\xfd\x84\xd1\x51\x3c\xa3\x8e\xe2\x02\xfa"
	"\x5c\xa5\xce\x99\x9d\x7b\x94\xa7\x1b\x14\xee\x9d\x67\x0a\xda\xd0"
	"\xc5\x7d\x22\x7e\xa5\x6c\x29\x2a\x0b\x5b\xe6\x88\x69\x04\x13\x46"
	"\x56\x12\xa1\x5c\x9e\x35\xfd\xd1\x91\x0c\x5d\x8d\x1e\xa8\xc5\x1f"
	"\x06\x31\x2d\x9c\xd1\x6a\x78\x04\x41\x22\xc0\xb7\xe3\xcc\x16\x85"
	"\x15\x4b\x38\xd2\x87\x26\xed\x56\x9f\xc3\x80\x07\x42\x55\x7e\x9e"
	"\xe0\x80\x43\x4e\x57\x99\x73\x9d\x71\x51\xe3\x7f\x34\xbf\x23\x92"
	"\x2f\xb3\x8d\x29\xf5\x17\xad\x7c\x91\x72\xd4\xb4\x7b\xbb\x9e\x23"
	"\xfe\x96\xe0\x59\x07\xab\x24\x76\x3a\x29\x30\xe0\x70\x33\xd5\x39"
	"\xda\xbf\xe2\xbe\x6b\xa3\x8b\x7e\xd9\xdf\x1a\x70\x53\xdd\xc2\x74"
	"\x28\x33\x67\x36\x13\x26\x99\x84\xdf\x7d\xc9\x14\x17\x51\x4d\x3b"
	"\xf6\xf7\xbc\xa7\xcc\x3b\xd5\x09\x8e\x6d\x58\xcd\x38\x90\x0e\xcc"
	"\x4e\xa7\x7d\x14\x94\x63\x66\xad\x4b\xb6\x95\x82\x08\x1a\xac\xcd"
	"\x83\xfb\xe1\x2b\x65\xa2\x63\x40\xea\x0b\x56\x8e\xfd\xfb\xa5\xda"
	"\x03\x5e\x8d\x54\x06\x12\x22\x4f\x86\x52\x41\x4c\x07\xe0\x9d\x19"
	"\xa4\xfa\x5f\xc3\xdf\x70\x8a\xb8\x4a\x40\xa2\xac\x58\x1e\x32\x43"
	"\xfc\x50\x44\x0b\xc6\x31\x65\x39\xc3\x5f\x3a\x42\x3d\x49\x49\xba"
	"\x25\x3b\x84\x23\xce\x10\xaa\xfc\x31\xa6\x0d\x54\xe3\xc5\xe1\x1a"
	"\x96\x0d\x94\x82\x9a\x99\x50\xaf\xd4\x03\xb3\x6b\x35\xd1\xe1\x44";

void gepard_config_read()
{
	char* conf_name = "conf/gepard_shield.conf";
	char line[1024], w1[1024], w2[1024];

	FILE* fp = fopen(conf_name, "r");

	is_gepard_active = false;

	if (fp == NULL) 
	{
		ShowError("Gepard configuration file (%s) not found. Shield disabled.\n", conf_name);
		return;
	}

	while(fgets(line, sizeof(line), fp))
	{
		if (line[0] == '/' && line[1] == '/')
			continue;

		if (sscanf(line, "%[^:]: %[^\r\n]", w1, w2) < 2)
			continue;

		if (!strcmpi(w1, "gepard_shield_enabled"))
		{
			is_gepard_active = (bool)config_switch(w2);
		}
	}

	fclose(fp);

	conf_name = "conf/gepard_version.txt";

	if ((fp = fopen(conf_name, "r")) == NULL)
	{
		min_allowed_gepard_version = 0;
		ShowError("Gepard version file (%s) not found.\n", conf_name);
		return;
	}

	fscanf(fp, "%u", &min_allowed_gepard_version);

	fclose(fp);
}

bool gepard_process_packet(int fd, uint8* packet_data, uint32 packet_size, struct gepard_crypt_link* link)
{
	uint16 packet_id = RBUFW(packet_data, 0);

	switch (packet_id)
	{
		case CS_GEPARD_SYNC:
		{
			uint32 control_value;

			if (RFIFOREST(fd) < 6)
			{
				return true;
			}

			gepard_enc_dec(packet_data + 2, packet_data + 2, 4, &session[fd]->sync_crypt);

			control_value = RFIFOL(fd, 2);

			if (control_value == 0xDDCCBBAA)
			{
				session[fd]->gepard_info.sync_tick = gettick();
			}

			RFIFOSKIP(fd, 6);

			return true;
		}
		break;

		case CS_LOGIN_PACKET_1:
		case CS_LOGIN_PACKET_2:
		case CS_LOGIN_PACKET_3:
		case CS_LOGIN_PACKET_4:
		case CS_LOGIN_PACKET_5:
		{
			set_eof(fd);
			return true;
		}
		break;

		case CS_LOGIN_PACKET:
		{
			if (RFIFOREST(fd) < 55)
			{
				return false;
			}

			if (session[fd]->gepard_info.is_init_ack_received == false)
			{
				RFIFOSKIP(fd, RFIFOREST(fd));
				gepard_init(fd, GEPARD_LOGIN);	
				return true;
			}

			gepard_enc_dec(packet_data + 2, packet_data + 2, RFIFOREST(fd) - 2, link);
		}
		break;

		case CS_LOGIN_PACKET_6:
		{
			if (RFIFOREST(fd) < 4 || RFIFOREST(fd) < (packet_size = RBUFW(packet_data, 2)) || packet_size < 4)
			{
				return true;
			}

			if (session[fd]->gepard_info.is_init_ack_received == false)
			{
				RFIFOSKIP(fd, RFIFOREST(fd));
				gepard_init(fd, GEPARD_LOGIN);	
				return true;
			}

			gepard_enc_dec(packet_data + 4, packet_data + 4, RFIFOREST(fd) - 4, link);
		}
		break;

		case CS_WHISPER_TO:
		{
			if (RFIFOREST(fd) < 4 || RFIFOREST(fd) < (packet_size = RBUFW(packet_data, 2)) || packet_size < 4)
			{
				return true;
			}

			gepard_enc_dec(packet_data + 4, packet_data + 4, packet_size - 4, link);
		}
		break;

		case CS_WALK_TO_XY:
		case CS_USE_SKILL_TO_ID:
		case CS_USE_SKILL_TO_POS:
		{
			if (packet_size < 2 || RFIFOREST(fd) < packet_size)
			{
				return true;
			}

			gepard_enc_dec(packet_data + 2, packet_data + 2, packet_size - 2, link);
		}
		break;

		case SC_WHISPER_FROM:
		case SC_SET_UNIT_IDLE:
		case SC_SET_UNIT_WALKING:
		{
			if (&session[fd]->send_crypt != link)
			{
				return true;
			}

			gepard_enc_dec(packet_data + 4, packet_data + 4, packet_size - 4, link);
		}
		break;

		case CS_GEPARD_INIT_ACK:
		{
			uint32 unique_id, unique_id_, shield_ver;

			if (RFIFOREST(fd) < 4 || RFIFOREST(fd) < (packet_size = RFIFOW(fd, 2)))
			{
				return true;
			}

			if (packet_size < 16)
			{
				ShowWarning("gepard_process_packet: invalid size of CS_GEPARD_INIT_ACK packet: %u\n", packet_size);
				set_eof(fd);
				return true;
			}

			gepard_enc_dec(packet_data + 4, packet_data + 4, packet_size - 4, link);

			unique_id  = RFIFOL(fd, 4);
			shield_ver = RFIFOL(fd, 8);
			unique_id_ = RFIFOL(fd, 12) ^ UNIQUE_ID_XOR;

			RFIFOSKIP(fd, packet_size);

			if (!unique_id || !unique_id_ || unique_id != unique_id_)
			{
				WFIFOHEAD(fd, 6);
				WFIFOW(fd, 0) = SC_GEPARD_INFO;
				WFIFOL(fd, 2) = 3;
				WFIFOSET(fd, 6);
				set_eof(fd);
			}

			session[fd]->gepard_info.is_init_ack_received = true;
			session[fd]->gepard_info.unique_id = unique_id;
			session[fd]->gepard_info.gepard_shield_version = shield_ver;

			return true;
		}
		break;
	}

	return false;
}

inline void gepard_srand(unsigned int seed)
{
	gepard_rand_seed = seed;
}

inline unsigned int gepard_rand()
{
	return (((gepard_rand_seed = gepard_rand_seed * 214013L + 2531011L) >> 16) & 0x7fff);
}

void gepard_session_init(int fd, unsigned int recv_key, unsigned int send_key, unsigned int sync_key)
{
	uint32 i;
	uint8 random_1 = RAND_1_START;
	uint8 random_2 = RAND_2_START;

	session[fd]->recv_crypt.pos_1 = session[fd]->send_crypt.pos_1 = session[fd]->sync_crypt.pos_1 = POS_1_START;
	session[fd]->recv_crypt.pos_2 = session[fd]->send_crypt.pos_2 = session[fd]->sync_crypt.pos_2 = POS_2_START;
	session[fd]->recv_crypt.pos_3 = session[fd]->send_crypt.pos_3 = session[fd]->sync_crypt.pos_3 = 0;

	gepard_srand(recv_key ^ SRAND_CONST);

	for (i = 0; i < (KEY_SIZE-1); ++i)
	{
		random_1 ^= shield_matrix[gepard_rand() % (MATRIX_SIZE-1)];
		random_1 += (8 * random_2) + 5;
		random_2 ^= shield_matrix[gepard_rand() % (MATRIX_SIZE-1)];
		random_2 += (6 * random_1) - 2;
		random_1 += random_2 ^ shield_matrix[gepard_rand() % (MATRIX_SIZE-1)];
		session[fd]->recv_crypt.key[i] = random_1;
	}

	random_1 = RAND_1_START;
	random_2 = RAND_2_START;	
	gepard_srand(send_key | SRAND_CONST);

	for (i = 0; i < (KEY_SIZE-1); ++i)
	{
		random_1 ^= shield_matrix[gepard_rand() % (MATRIX_SIZE-1)];
		random_1 += (7 * random_2) - 2;
		random_2 ^= shield_matrix[gepard_rand() % (MATRIX_SIZE-1)];
		random_2 -= (2 * random_1) + 9;
		random_1 += random_2 ^ shield_matrix[gepard_rand() % (MATRIX_SIZE-1)];
		session[fd]->send_crypt.key[i] = random_1;
	}

	random_1 = RAND_1_START;
	random_2 = RAND_2_START;	
	gepard_srand(sync_key | SRAND_CONST);

	for (i = 0; i < (KEY_SIZE-1); ++i)
	{
		random_1 ^= shield_matrix[gepard_rand() % (MATRIX_SIZE-1)];
		random_1 -= (4 * random_2) - 9;
		random_2 ^= shield_matrix[gepard_rand() % (MATRIX_SIZE-1)];
		random_2 += (3 * random_1) - 5;
		random_1 += random_2 ^ shield_matrix[gepard_rand() % (MATRIX_SIZE-1)];
		session[fd]->sync_crypt.key[i] = random_1;
	}
}

void gepard_init(int fd, uint16 server_type)
{
	const uint16 init_packet_size = 20;
	uint16 recv_key = (gepard_rand() % 0xFFFF);
	uint16 send_key = (gepard_rand() % 0xFFFF);
	uint16 sync_key = (gepard_rand() % 0xFFFF);

	gepard_srand((unsigned)time(NULL) ^ clock());

	WFIFOHEAD(fd, init_packet_size);
	WFIFOW(fd, 0) = SC_GEPARD_INIT;
	WFIFOW(fd, 2) = init_packet_size;
	WFIFOW(fd, 4) = recv_key;
	WFIFOW(fd, 6) = send_key;
	WFIFOW(fd, 8) = server_type;
	WFIFOL(fd, 10) = GEPARD_ID;
	WFIFOL(fd, 14) = min_allowed_gepard_version;
	WFIFOW(fd, 18) = sync_key;
	WFIFOSET(fd, init_packet_size);

	gepard_session_init(fd, recv_key, send_key, sync_key);
}

void gepard_enc_dec(uint8* in_data, uint8* out_data, uint32 data_size, struct gepard_crypt_link* link)
{	
	uint32 i;

	for(i = 0; i < data_size; ++i)
	{
		link->pos_1 += link->key[link->pos_3 % (KEY_SIZE-1)];
		link->pos_2 += (61 - link->pos_1) * 7;
		link->key[link->pos_2 % (KEY_SIZE-1)] ^= link->pos_1;
		link->pos_1 += (link->pos_2 + link->pos_3) / 4;
		link->key[link->pos_3 % (KEY_SIZE-1)] ^= link->pos_1;
		out_data[i] = in_data[i] ^ link->pos_1;
		link->pos_1 -= 25;
		link->pos_2 -= data_size % 0xFF;
		link->pos_3++;
	}
}

void gepard_send_info(int fd, unsigned short info_type, char* message)
{
	int message_len = strlen(message) + 1;
	int packet_len = 2 + 2 + 2 + message_len;

	WFIFOHEAD(fd, packet_len);
	WFIFOW(fd, 0) = SC_GEPARD_INFO;
	WFIFOW(fd, 2) = packet_len;
	WFIFOW(fd, 4) = info_type;
	safestrncpy((char*)WFIFOP(fd, 6), message, message_len);
	WFIFOSET(fd, packet_len);
}

