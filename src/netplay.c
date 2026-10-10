/*
 * netplay.c - the relay transport. See netplay.h and
 * docs/subsystems/netplay-relay.txt.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "netplay.h"
#include "log.h"

/*
 * LINKED HERE, NOT IN THE PROJECT SETTINGS. The dependency belongs
 * next to the code that needs it; nothing else in the build wants
 * sockets, and an entry in the .vcxproj would apply to every file.
 */
#pragma comment(lib, "ws2_32.lib")

#define NET_HEADER_SIZE   8
#define NET_FRAME_MAX     (NET_HEADER_SIZE + NET_PAYLOAD_MAX)
#define NET_RXBUF_SIZE    (NET_FRAME_MAX * 4)
#define NET_TXBUF_SIZE    (NET_FRAME_MAX * 8)
#define NET_QUEUE_MAX     32
#define NET_KEEPALIVE_MS  15000
#define NET_CONNECT_MS    8000
#define NET_RELAY_PORT    27500

typedef struct {
	int           src;
	int           length;
	unsigned char data[NET_PAYLOAD_MAX];
} MR_NetInbound;

static SOCKET        s_sock = INVALID_SOCKET;
static int           s_wsaUp;
static MR_NetState   s_state;
static int           s_refusal;
static int           s_self;
static int           s_isHost;
static int           s_hostPeer;
static int           s_wantHost;
static char          s_name[16];
static int           s_roomId;

static unsigned int  s_lastTick[NET_PEER_MAX];
static MR_NetRoom    s_rooms[NET_ROOMS_MAX];
static int           s_roomCount;
static char          s_relayHost[128];
static int           s_relayPort = NET_RELAY_PORT;

static int           s_peers[NET_PEER_MAX];
static int           s_peerCount;

static unsigned char s_rx[NET_RXBUF_SIZE];
static int           s_rxUsed;
static unsigned char s_tx[NET_TXBUF_SIZE];
static int           s_txUsed;

static MR_NetInbound s_queue[NET_QUEUE_MAX];
static int           s_queueHead;
static int           s_queueCount;

static DWORD         s_connectStart;
static DWORD         s_lastSend;

static void net_room_reset(void);
static void net_forget_slot(int slot);


/* ORIGIN  NONE - ours */
static void net_fail(int reason)
{
	s_state   = NET_FAILED;
	s_refusal = reason;
}

/* ORIGIN  NONE - ours */
static void net_drop_socket(void)
{
	if (s_sock != INVALID_SOCKET) {
		closesocket(s_sock);
		s_sock = INVALID_SOCKET;
	}
	s_rxUsed    = 0;
	s_txUsed    = 0;
	s_peerCount = 0;
	s_self      = 0;
	s_isHost    = 0;
	s_hostPeer  = 0;
	s_roomId    = 0;
	net_room_reset();
	s_roomCount = 0;
}

/* ORIGIN  NONE - ours */
void netplay_set_relay(const char *host, int port)
{
	if (host && *host) {
		strncpy(s_relayHost, host, sizeof(s_relayHost) - 1);
		s_relayHost[sizeof(s_relayHost) - 1] = 0;
	}
	if (port > 0 && port < 65536)
		s_relayPort = port;
}

/* ORIGIN  NONE - ours */
int netplay_open(void)
{
	WSADATA wsa;

	if (s_wsaUp)
		return 1;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		mrlog_err("net: WSAStartup failed\n");
		return 0;
	}
	s_wsaUp = 1;
	s_state = NET_IDLE;
	mrlog("net: winsock %d.%d up\n", LOBYTE(wsa.wVersion),
	      HIBYTE(wsa.wVersion));
	return 1;
}

/* ORIGIN  NONE - ours */
void netplay_close(void)
{
	net_drop_socket();
	s_queueHead  = 0;
	s_queueCount = 0;
	s_state      = NET_IDLE;
	s_refusal    = 0;

	if (s_wsaUp) {
		WSACleanup();
		s_wsaUp = 0;
	}
}

/*
 * ORIGIN  NONE - ours
 *
 * Queue bytes for the relay. The flush is netplay_poll's job, because
 * a non-blocking send can take part of a buffer and refuse the rest.
 */
static int net_push_tx(const unsigned char *b, int n)
{
	if (n <= 0 || s_txUsed + n > NET_TXBUF_SIZE)
		return 0;
	memcpy(s_tx + s_txUsed, b, (size_t)n);
	s_txUsed += n;
	return 1;
}

/*
 * ORIGIN  NONE - ours
 *
 * LITTLE-ENDIAN ON THE WIRE, deliberately, NOT network byte order.
 * Both ends of this are x86 and the payload it carries is a game
 * message full of native-order ints that nobody is going to byte-swap
 * field by field. Swapping only the header would be a half measure
 * that reads as correctness. netplay-relay.txt 2.1.
 */
static int net_frame(int kind, int flags, int dst,
                     const void *payload, int length)
{
	unsigned char h[NET_HEADER_SIZE];

	if (length < 0 || length > NET_PAYLOAD_MAX)
		return 0;

	h[0] = (unsigned char)kind;
	h[1] = (unsigned char)flags;
	h[2] = (unsigned char)(dst & 0xff);
	h[3] = (unsigned char)((dst >> 8) & 0xff);
	/*
	 * src GOES OUT AS ZERO. The relay stamps it and ignores whatever
	 * we put here, so writing our own id would be decoration that
	 * later reads as meaningful. 2.3.
	 */
	h[4] = 0;
	h[5] = 0;
	h[6] = (unsigned char)(length & 0xff);
	h[7] = (unsigned char)((length >> 8) & 0xff);

	if (!net_push_tx(h, NET_HEADER_SIZE))
		return 0;
	if (length > 0 && !net_push_tx((const unsigned char *)payload, length)) {
		s_txUsed -= NET_HEADER_SIZE;    /* unsend the header */
		return 0;
	}
	s_lastSend = GetTickCount();
	return 1;
}

/* ORIGIN  NONE - ours */
static int net_dial(void)
{
	struct addrinfo  hints;
	struct addrinfo *res = NULL;
	char             port[16];
	u_long           nb = 1;
	int              rc;

	if (!s_wsaUp || !s_relayHost[0]) {
		mrlog_err("net: no relay configured\n");
		return 0;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	_snprintf(port, sizeof(port), "%d", s_relayPort);

	if (getaddrinfo(s_relayHost, port, &hints, &res) != 0 || !res) {
		mrlog_err("net: cannot resolve %s\n", s_relayHost);
		return 0;
	}

	s_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (s_sock == INVALID_SOCKET) {
		freeaddrinfo(res);
		return 0;
	}

	ioctlsocket(s_sock, FIONBIO, &nb);

	/*
	 * NAGLE OFF. The state packet is one small frame per tick and
	 * coalescing it with the next one is exactly the latency this
	 * whole design is trying to avoid.
	 */
	{
		BOOL on = TRUE;
		setsockopt(s_sock, IPPROTO_TCP, TCP_NODELAY,
		           (const char *)&on, sizeof(on));
	}

	rc = connect(s_sock, res->ai_addr, (int)res->ai_addrlen);
	freeaddrinfo(res);

	if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
		mrlog_err("net: connect to %s:%d failed\n", s_relayHost,
		          s_relayPort);
		net_drop_socket();
		return 0;
	}

	s_connectStart = GetTickCount();
	s_state        = NET_CONNECTING;
	mrlog("net: dialling %s:%d\n", s_relayHost, s_relayPort);
	return 1;
}

/*
 * ORIGIN  NONE - ours
 *
 * THE NAME IS THE IDENTITY. There is no code and no password: a room
 * is listed under whoever created it and joining is trusting that
 * name - netplay-relay.txt 4.
 */
static void net_send_hello(void)
{
	unsigned char b[2 + 9];

	memset(b, 0, sizeof(b));
	b[0] = NET_PROTOCOL_VERSION;
	b[1] = 0;
	memcpy(b + 2, s_name, 8);

	net_frame(NET_KIND_HELLO, NET_FLAG_RELIABLE, NET_PEER_NONE,
	          b, (int)sizeof(b));
	s_state = NET_HANDSHAKING;
}

/* ORIGIN  NONE - ours */
int netplay_connect(const char *name)
{
	int i;

	if (s_state != NET_IDLE)
		return 0;

	memset(s_name, 0, sizeof(s_name));
	for (i = 0; i < 8 && name && name[i]; i++)
		s_name[i] = name[i];
	if (!s_name[0])
		memcpy(s_name, "PLAYER", 7);

	return net_dial();
}

/* ORIGIN  NONE - ours */
int netplay_create_room(void)
{
	if (s_state != NET_BROWSING)
		return 0;
	return net_frame(NET_KIND_CREATE_ROOM, NET_FLAG_RELIABLE,
	                 NET_PEER_NONE, NULL, 0);
}

/* ORIGIN  NONE - ours */
int netplay_join_room(int roomId)
{
	unsigned char b[2];

	if (s_state != NET_BROWSING || roomId <= 0)
		return 0;
	s_refusal = 0;      /* page 7 shows the LATEST attempt */
	b[0] = (unsigned char)(roomId & 0xff);
	b[1] = (unsigned char)((roomId >> 8) & 0xff);
	return net_frame(NET_KIND_JOIN_ROOM, NET_FLAG_RELIABLE,
	                 NET_PEER_NONE, b, 2);
}

/* ORIGIN  NONE - ours */
int netplay_leave_room(void)
{
	if (s_state != NET_JOINED)
		return 0;
	return net_frame(NET_KIND_LEAVE_ROOM, NET_FLAG_RELIABLE,
	                 NET_PEER_NONE, NULL, 0);
}

/* ORIGIN  NONE - ours */
int netplay_request_list(void)
{
	if (s_state != NET_BROWSING && s_state != NET_JOINED)
		return 0;
	return net_frame(NET_KIND_LIST_REQ, NET_FLAG_RELIABLE,
	                 NET_PEER_NONE, NULL, 0);
}

/* ORIGIN  NONE - ours */
int netplay_set_room_racing(int racing)
{
	unsigned char b[1];

	if (s_state != NET_JOINED || !s_isHost)
		return 0;
	b[0] = (unsigned char)(racing ? NET_ROOM_RACING : NET_ROOM_WAITING);
	return net_frame(NET_KIND_ROOM_STATE, NET_FLAG_RELIABLE,
	                 NET_PEER_NONE, b, 1);
}

/* ORIGIN  NONE - ours */
int netplay_room_count(void) { return s_roomCount; }

/* ORIGIN  NONE - ours */
const MR_NetRoom *netplay_room_at(int index)
{
	if (index < 0 || index >= s_roomCount)
		return NULL;
	return &s_rooms[index];
}

/* ORIGIN  NONE - ours */
int netplay_room_id(void) { return s_roomId; }

/* ORIGIN  NONE - ours */
static void net_peer_add(int id)
{
	int i;

	if (id == NET_PEER_NONE || id == s_self)
		return;
	for (i = 0; i < s_peerCount; i++)
		if (s_peers[i] == id)
			return;
	if (s_peerCount < NET_PEER_MAX)
		s_peers[s_peerCount++] = id;
}

/* ORIGIN  NONE - ours */
static void net_peer_remove(int id)
{
	int i;

	for (i = 0; i < s_peerCount; i++) {
		if (s_peers[i] == id) {
			s_peers[i] = s_peers[--s_peerCount];
			return;
		}
	}
}

/*
 * ORIGIN  NONE - ours
 *
 * WELCOME: our peer id, the host flag, the code, then a count and
 * that many peer ids.
 */
static void net_on_welcome(const unsigned char *p, int n)
{
	int count, i;

	if (n < 8) {
		net_fail(NET_REFUSE_VERSION);
		return;
	}

	s_self     = p[0] | (p[1] << 8);
	s_isHost   = p[2] ? 1 : 0;
	s_hostPeer = p[3] | (p[4] << 8);
	s_roomId   = p[5] | (p[6] << 8);
	net_room_reset();
	count      = p[7];

	s_peerCount = 0;
	for (i = 0; i < count; i++) {
		int off = 8 + i * 2;

		if (off + 1 >= n)
			break;
		net_peer_add(p[off] | (p[off + 1] << 8));
	}

	/*
	 * ROOM 0 MEANS BROWSING. One WELCOME serves connecting, creating
	 * and joining, and the room id is what distinguishes them - so
	 * there is one parse rather than three.
	 */
	s_state = (s_roomId != 0) ? NET_JOINED : NET_BROWSING;
	if (s_roomId != 0)
		mrlog("net: in room %d as peer %d%s, host %d, %d other(s)\n",
	          s_roomId, s_self, s_isHost ? " (host)" : "", s_hostPeer,
	          s_peerCount);
	else
		mrlog("net: on the relay as peer %d, browsing\n", s_self);
}

/*
 * ORIGIN  NONE - ours
 *
 * count, then id(2) state(1) players(1) name(9) per room.
 */
static void net_on_room_list(const unsigned char *p, int n)
{
	int count, i;

	if (n < 1)
		return;
	count = p[0];
	if (count > NET_ROOMS_MAX)
		count = NET_ROOMS_MAX;

	s_roomCount = 0;
	for (i = 0; i < count; i++) {
		int off = 1 + i * (4 + 9);
		MR_NetRoom *r;

		if (off + 4 + 9 > n)
			break;
		r = &s_rooms[s_roomCount++];
		r->id      = p[off] | (p[off + 1] << 8);
		r->state   = p[off + 2];
		r->players = p[off + 3];
		memcpy(r->name, p + off + 4, 8);
		r->name[8] = 0;
	}
}

/* ORIGIN  NONE - ours */
static void net_on_frame(int kind, int src,
                         const unsigned char *payload, int length)
{
	switch (kind) {
	case NET_KIND_PAYLOAD:
		if (s_queueCount >= NET_QUEUE_MAX) {
			/*
			 * DROP THE OLDEST, not the newest. These are game
			 * messages and a backlog means we are behind; the
			 * stale end is the one worth losing.
			 */
			s_queueHead = (s_queueHead + 1) % NET_QUEUE_MAX;
			s_queueCount--;
		}
		{
			int slot = (s_queueHead + s_queueCount) % NET_QUEUE_MAX;

			s_queue[slot].src    = src;
			s_queue[slot].length = length;
			memcpy(s_queue[slot].data, payload, (size_t)length);
			s_queueCount++;
		}
		break;

	case NET_KIND_WELCOME:
		net_on_welcome(payload, length);
		break;

	case NET_KIND_ROOM_LIST:
		net_on_room_list(payload, length);
		break;

	case NET_KIND_PEER_JOINED:
		if (length >= 2)
			net_peer_add(payload[0] | (payload[1] << 8));
		break;

	case NET_KIND_PEER_LEFT:
		/* The relay's pid IS the slot, so one id serves both. */
		if (length >= 2) {
			int gone = payload[0] | (payload[1] << 8);

			net_peer_remove(gone);
			net_forget_slot(gone);
		}
		break;

	case NET_KIND_ROOM_GONE:
		/*
		 * THE ROOM WENT, NOT THE CONNECTION. The relay puts us back
		 * to browsing and sends a fresh list, so tearing the socket
		 * down here would throw away a working link.
		 */
		mrlog("net: room closed by the relay\n");
		s_roomId    = 0;
		net_room_reset();
		s_isHost    = 0;
		s_peerCount = 0;
		s_state     = NET_BROWSING;
		break;

	case NET_KIND_REFUSED: {
		int why = (length >= 1) ? payload[0] : 0;

		/* A ROOM REFUSAL IS NOT A LINK FAILURE; page 7 has the
		 * status lines and NET_FAILED bounces past them. */
		s_refusal = why;
		if (why == NET_REFUSE_VERSION)
			net_fail(why);
		else
			s_state = NET_BROWSING;
		mrlog_err("net: refused, reason %d\n", why);
		break;
	}

	case NET_KIND_KEEPALIVE:
		break;

	default:
		/* An unknown kind is the relay being newer than us. */
		break;
	}
}

/*
 * ORIGIN  NONE - ours
 *
 * TCP IS A STREAM, NOT A MESSAGE QUEUE, and this is the trap that
 * eats every first attempt at a framed protocol: one recv can return
 * half a frame, three frames, or two and a half. Nothing may be
 * handled until its whole header AND its whole payload have arrived.
 */
static void net_drain_rx(void)
{
	int off = 0;

	while (s_rxUsed - off >= NET_HEADER_SIZE) {
		const unsigned char *h = s_rx + off;
		int kind   = h[0];
		/*
		 * src IS AT +4, NOT +2 - the header is kind, flags, dst,
		 * src, length. Read dst as the sender and a direct message
		 * appears to come from the peer it was ADDRESSED TO, a
		 * broadcast from peer 0. Both fail silently.
		 */
		int src    = h[4] | (h[5] << 8);
		int length = h[6] | (h[7] << 8);

		if (length > NET_PAYLOAD_MAX) {
			mrlog_err("net: frame of %d bytes, dropping link\n",
			          length);
			net_drop_socket();
			net_fail(NET_REFUSE_VERSION);
			return;
		}
		if (s_rxUsed - off < NET_HEADER_SIZE + length)
			break;              /* the rest is still in flight */

		net_on_frame(kind, src, h + NET_HEADER_SIZE, length);
		off += NET_HEADER_SIZE + length;

		if (s_sock == INVALID_SOCKET)
			return;             /* a handler tore the link down */
	}

	if (off > 0) {
		s_rxUsed -= off;
		if (s_rxUsed > 0)
			memmove(s_rx, s_rx + off, (size_t)s_rxUsed);
	}
}

/* ORIGIN  NONE - ours */
static int net_connect_done(void)
{
	fd_set  wr, ex;
	TIMEVAL tv;
	int     err = 0;
	int     len = (int)sizeof(err);

	FD_ZERO(&wr);
	FD_ZERO(&ex);
	FD_SET(s_sock, &wr);
	FD_SET(s_sock, &ex);
	tv.tv_sec  = 0;
	tv.tv_usec = 0;

	if (select(0, NULL, &wr, &ex, &tv) <= 0)
		return 0;

	if (FD_ISSET(s_sock, &ex)) {
		net_drop_socket();
		net_fail(NET_REFUSE_NOROOM);
		return 0;
	}
	if (getsockopt(s_sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0 &&
	    err != 0) {
		net_drop_socket();
		net_fail(NET_REFUSE_NOROOM);
		return 0;
	}
	return 1;
}

/* ORIGIN  NONE - ours */
static void net_flush_tx(void)
{
	while (s_txUsed > 0) {
		int sent = send(s_sock, (const char *)s_tx, s_txUsed, 0);

		if (sent == SOCKET_ERROR) {
			if (WSAGetLastError() == WSAEWOULDBLOCK)
				return;     /* flush the rest next frame */
			mrlog_err("net: send failed, link down\n");
			net_drop_socket();
			s_state = NET_IDLE;
			return;
		}
		s_txUsed -= sent;
		if (s_txUsed > 0)
			memmove(s_tx, s_tx + sent, (size_t)s_txUsed);
	}
}

/* ORIGIN  NONE - ours */
void netplay_poll(void)
{
	if (s_sock == INVALID_SOCKET)
		return;

	if (s_state == NET_CONNECTING) {
		if (GetTickCount() - s_connectStart > NET_CONNECT_MS) {
			mrlog_err("net: connect timed out\n");
			net_drop_socket();
			net_fail(NET_REFUSE_NOROOM);
			return;
		}
		if (!net_connect_done())
			return;
		net_send_hello();
	}

	net_flush_tx();
	if (s_sock == INVALID_SOCKET)
		return;

	for (;;) {
		int space = NET_RXBUF_SIZE - s_rxUsed;
		int got;

		if (space <= 0)
			break;

		got = recv(s_sock, (char *)(s_rx + s_rxUsed), space, 0);
		if (got > 0) {
			s_rxUsed += got;
			continue;
		}
		if (got == 0) {
			mrlog("net: relay closed the link\n");
			net_drop_socket();
			s_state = NET_IDLE;
			return;
		}
		if (WSAGetLastError() != WSAEWOULDBLOCK) {
			mrlog_err("net: recv failed, link down\n");
			net_drop_socket();
			s_state = NET_IDLE;
			return;
		}
		break;
	}

	net_drain_rx();

	if (s_sock != INVALID_SOCKET &&
	    (s_state == NET_JOINED || s_state == NET_BROWSING) &&
	    GetTickCount() - s_lastSend > NET_KEEPALIVE_MS) {
		net_frame(NET_KIND_KEEPALIVE, 0, NET_PEER_NONE, NULL, 0);
		net_flush_tx();
	}
}

/* ORIGIN  NONE - ours */
int netplay_send(int dst, int reliable, const void *payload, int length)
{
	if (s_state != NET_JOINED || dst == NET_PEER_NONE)
		return 0;
	return net_frame(NET_KIND_PAYLOAD,
	                 reliable ? NET_FLAG_RELIABLE : 0,
	                 dst, payload, length);
}

/* ORIGIN  NONE - ours */
int netplay_broadcast(int reliable, const void *payload, int length)
{
	int flags = NET_FLAG_BROADCAST;

	if (s_state != NET_JOINED)
		return 0;
	if (reliable)
		flags |= NET_FLAG_RELIABLE;
	return net_frame(NET_KIND_PAYLOAD, flags, NET_PEER_NONE,
	                 payload, length);
}

/* ORIGIN  NONE - ours */
int netplay_recv(int *src, void *payload, int capacity)
{
	MR_NetInbound *in;
	int            n;

	if (s_queueCount <= 0)
		return 0;

	in = &s_queue[s_queueHead];
	n  = in->length;

	s_queueHead = (s_queueHead + 1) % NET_QUEUE_MAX;
	s_queueCount--;

	if (n > capacity)
		return -1;

	if (src)
		*src = in->src;
	if (n > 0)
		memcpy(payload, in->data, (size_t)n);
	return n;
}

/* ORIGIN  NONE - ours */
MR_NetState netplay_state(void) { return s_state; }

/* ORIGIN  NONE - ours */
int netplay_refusal(void) { return s_refusal; }

/* ORIGIN  NONE - ours */
int netplay_self(void) { return s_self; }

/* ORIGIN  NONE - ours */
int netplay_is_host(void) { return s_isHost; }

/* ORIGIN  NONE - ours */
int netplay_host_peer(void) { return s_hostPeer; }

/* ORIGIN  NONE - ours */
int netplay_peer_count(void) { return s_peerCount; }

/* ORIGIN  NONE - ours */
int netplay_peer_at(int index)
{
	if (index < 0 || index >= s_peerCount)
		return NET_PEER_NONE;
	return s_peers[index];
}


/* ================================================================== */
/* THE MESSAGE LAYER.  OURS - netplay-relay.txt 5.                    */
/* ================================================================== */

#define NET_CHAT_LINES   16
#define NET_TIME_SAMPLES 8
#define NET_CHAT_MIN_GAP 400      /* ms between our own lines */

static MR_NetHandlers s_h;
static MR_NetLobby    s_lobby;

static struct {
	int  slot;
	char text[NET_CHAT_MAX];
} s_chat[NET_CHAT_LINES];
static int s_chatCount;
static DWORD s_chatLastSent;

/*
 * THE HOST CLOCK OFFSET, in milliseconds, as hostMs - ourMs. Taken as
 * the sample with the SMALLEST round trip rather than an average: a
 * delayed sample is biased one way only, so the minimum is the least
 * wrong estimate. netplay-relay.txt 5.5.
 */
static int   s_clockOffset;
static int   s_clockSynced;
static int   s_clockBestRtt;
static DWORD s_lastTimePing;



/* ORIGIN  NONE - ours */
void netplay_set_handlers(const MR_NetHandlers *h)
{
	if (h)
		s_h = *h;
	else
		memset(&s_h, 0, sizeof(s_h));
}

/* ORIGIN  NONE - ours */
void netplay_get_handlers(MR_NetHandlers *out)
{
	if (out)
		*out = s_h;
}

/* ORIGIN  NONE - ours */
const MR_NetLobby *netplay_lobby(void)
{
	return s_lobby.valid ? &s_lobby : NULL;
}

/*
 * ORIGIN  NONE - ours
 *
 * A ROOM IS A NEW SEQUENCE AND THE ROSTER MUST NOT OUTLIVE IT: a
 * new room opened holding the last one's players, and
 * menu_net_on_join republishes netplay_lobby(). journal 540.
 */
static void net_room_reset(void)
{
	memset(s_lastTick, 0, sizeof s_lastTick);
	memset(&s_lobby, 0, sizeof s_lobby);
}

/*
 * ORIGIN  NONE - ours
 *
 * PEER_LEFT and NET_MSG_LEAVE both land here; dropping only the
 * transport peer left the player in the roster.
 */
static void net_forget_slot(int slot)
{
	if (slot < 1 || slot >= NET_PEER_MAX)
		return;
	memset(&s_lobby.players[slot], 0, sizeof(s_lobby.players[slot]));
	s_lobby.bikes[slot] = 0;
	s_lastTick[slot]    = 0;
	if (s_h.onLeave)
		s_h.onLeave(slot);
}

/* ORIGIN  NONE - ours */
int netplay_chat_count(void) { return s_chatCount; }

/* ORIGIN  NONE - ours */
const char *netplay_chat_at(int index, int *slot)
{
	if (index < 0 || index >= s_chatCount)
		return NULL;
	if (slot)
		*slot = s_chat[index].slot;
	return s_chat[index].text;
}

/* ORIGIN  NONE - ours */
int netplay_clock_synced(void) { return s_clockSynced; }

/* ORIGIN  NONE - ours */
unsigned int netplay_host_now(void)
{
	return (unsigned int)((int)GetTickCount() + s_clockOffset);
}

/*
 * ORIGIN  NONE - ours
 *
 * A SLOT IS A RACER INDEX, and slot 0 is never a player - every scan
 * in the original starts at slot 1. netplay.txt 2.1.
 */
static int net_slot_ok(int slot)
{
	return slot >= 1 && slot < NET_PEER_MAX;
}

/*
 * ORIGIN  NONE - ours
 *
 * NUL-TERMINATE WHAT CAME OFF THE WIRE, and bound it. The original's
 * 0x64 copies a 20-byte table name into a nine-byte racer field with
 * no bound at all - netplay-relay.txt 5.5. Nothing here trusts a
 * remote string to be terminated.
 */
static void net_copy_text(char *dst, int cap, const char *src)
{
	int i;

	for (i = 0; i < cap - 1 && src[i]; i++) {
		/* Printable only; a control byte in a name or a line is either
		   corruption or an attempt at one. */
		dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? src[i] : '?';
	}
	dst[i] = 0;
}

/* ORIGIN  NONE - ours */
static void net_chat_push(int slot, const char *text)
{
	int i;

	if (s_chatCount >= NET_CHAT_LINES) {
		for (i = 1; i < NET_CHAT_LINES; i++)
			s_chat[i - 1] = s_chat[i];
		s_chatCount = NET_CHAT_LINES - 1;
	}
	s_chat[s_chatCount].slot = slot;
	net_copy_text(s_chat[s_chatCount].text, NET_CHAT_MAX, text);
	s_chatCount++;
}

/*
 * ORIGIN  NONE - ours
 *
 * Send one message. The host broadcasts; a client can only talk to
 * the host, which is what makes it the only authority.
 */
static int net_msg_send(int type, const void *body, int length,
                        int reliable)
{
	MR_NetMsgHead *h = (MR_NetMsgHead *)body;

	if (netplay_state() != NET_JOINED)
		return 0;

	h->type     = (unsigned short)type;
	h->reserved = 0;

	if (netplay_is_host())
		return netplay_broadcast(reliable, body, length);

	/*
	 * A CLIENT SENDS TO THE HOST, not to the room. Broadcasting from a
	 * client would let it drive the others directly and there would be
	 * no single view to disagree with - see netplay-relay.txt 5.4 on
	 * the host rebuilding state.
	 */
	/*
	 * ADDRESSED BY THE TRANSPORT'S HOST PEER, NOT s_lobby.hostSlot:
	 * the lobby does not exist until the host publishes one, so using
	 * it here makes JOIN unsendable - and JOIN is what starts the
	 * whole exchange.
	 */
	if (netplay_host_peer() == NET_PEER_NONE)
		return 0;
	return netplay_send(netplay_host_peer(), reliable, body, length);
}

/* ORIGIN  NONE - ours */
int netplay_send_join(const char *name, int bike)
{
	MR_NetMsgJoin m;

	memset(&m, 0, sizeof(m));
	m.version = NET_PROTOCOL_VERSION;
	m.bike    = bike;
	net_copy_text(m.name, NET_NAME_MAX, name ? name : "");
	return net_msg_send(NET_MSG_JOIN, &m, (int)sizeof(m), 1);
}

/* ORIGIN  NONE - ours */
int netplay_send_ready(int ready, int bike)
{
	MR_NetMsgReady m;

	memset(&m, 0, sizeof(m));
	m.ready = ready ? 1 : 0;
	m.bike  = bike;
	return net_msg_send(NET_MSG_READY, &m, (int)sizeof(m), 1);
}

/* ORIGIN  NONE - ours */
int netplay_send_chat(const char *text)
{
	MR_NetMsgChat m;
	DWORD         now = GetTickCount();

	if (!text || !*text)
		return 0;
	/* Rate limit our own sending; the receive side limits theirs. */
	if (s_chatLastSent && now - s_chatLastSent < NET_CHAT_MIN_GAP)
		return 0;

	memset(&m, 0, sizeof(m));
	m.slot = netplay_self();
	net_copy_text(m.text, NET_CHAT_MAX, text);

	if (!net_msg_send(NET_MSG_CHAT, &m, (int)sizeof(m), 1))
		return 0;

	s_chatLastSent = now;
	/* Echo locally; a broadcast excludes the sender, so nothing
	   else will show us our own line. */
	net_chat_push(m.slot, m.text);
	if (s_h.onChat)
		s_h.onChat(m.slot, m.text);
	return 1;
}

/* ORIGIN  NONE - ours */
int netplay_send_state(const MR_NetMsgState *st)
{
	MR_NetMsgState m;

	if (!st)
		return 0;
	m = *st;
	return net_msg_send(NET_MSG_STATE, &m, (int)sizeof(m), 0);
}

/* ORIGIN  NONE - ours */
int netplay_send_collision(const MR_NetMsgCollision *c)
{
	MR_NetMsgCollision m;

	if (!c)
		return 0;
	m = *c;
	return net_msg_send(NET_MSG_COLLISION, &m, (int)sizeof(m), 0);
}

/* ORIGIN  NONE - ours */
int netplay_send_leave(void)
{
	MR_NetMsgLeave m;

	memset(&m, 0, sizeof(m));
	m.slot = netplay_self();
	return net_msg_send(NET_MSG_LEAVE, &m, (int)sizeof(m), 1);
}

/* ORIGIN  NONE - ours */
int netplay_publish_lobby(const MR_NetLobby *lobby)
{
	MR_NetMsgLobby m;

	if (!lobby || !netplay_is_host())
		return 0;

	memset(&m, 0, sizeof(m));
	m.hostSlot   = netplay_self();
	m.track      = lobby->track;
	m.laps       = lobby->laps;
	m.mode       = lobby->mode;
	m.difficulty = lobby->difficulty;
	m.pocket     = lobby->pocket;
	m.reverse    = lobby->reverse;
	memcpy(m.players, lobby->players, sizeof(m.players));
	memcpy(m.bikes,   lobby->bikes,   sizeof(m.bikes));

	s_lobby          = *lobby;
	s_lobby.valid    = 1;
	s_lobby.hostSlot = m.hostSlot;
	return net_msg_send(NET_MSG_LOBBY, &m, (int)sizeof(m), 1);
}

/* ORIGIN  NONE - ours */
int netplay_send_start(unsigned int hostStartMs)
{
	MR_NetMsgStart m;

	if (!netplay_is_host())
		return 0;
	memset(&m, 0, sizeof(m));
	m.startMs = hostStartMs;
	return net_msg_send(NET_MSG_START, &m, (int)sizeof(m), 1);
}

/* ORIGIN  NONE - ours */
int netplay_send_place(const MR_NetMsgPlace *p)
{
	MR_NetMsgPlace m;

	if (!p || !netplay_is_host())
		return 0;
	m = *p;
	return net_msg_send(NET_MSG_PLACE, &m, (int)sizeof(m), 1);
}

/* ORIGIN  NONE - ours */
int netplay_send_race_over(void)
{
	MR_NetMsgHead m;

	if (!netplay_is_host())
		return 0;
	memset(&m, 0, sizeof(m));
	return net_msg_send(NET_MSG_RACE_OVER, &m, (int)sizeof(m), 1);
}

/* ORIGIN  NONE - ours */
int netplay_send_results(int slot, const unsigned char *blob, int length)
{
	MR_NetMsgResults m;

	if (!netplay_is_host() || !blob || length <= 0 ||
	    length > NET_RESULT_LEN || !net_slot_ok(slot))
	    return 0;
	memset(&m, 0, sizeof(m));
	m.slot = slot;
	memcpy(m.blob, blob, (size_t)length);
	return net_msg_send(NET_MSG_RESULTS, &m, (int)sizeof(m), 1);
}

/* ORIGIN  NONE - ours */
static void net_send_time_ping(void)
{
	MR_NetMsgTime m;

	memset(&m, 0, sizeof(m));
	m.clientMs = GetTickCount();
	net_msg_send(NET_MSG_TIME_PING, &m, (int)sizeof(m), 0);
	s_lastTimePing = m.clientMs;
}

/*
 * ORIGIN  NONE - ours
 *
 * IS THIS MESSAGE ALLOWED FROM THIS SENDER? The original trusts the
 * racer slot carried INSIDE the packet, so any peer could claim to be
 * any racer, and writes received values straight into gameplay
 * globals. With a relay-stamped src we can do better - 5.5.
 */
static int net_sender_may(int type, int src, int slot)
{
	switch (type) {
	/* Host authority. Only the host decides these. */
	case NET_MSG_LOBBY:
	case NET_MSG_START:
	case NET_MSG_JOIN_ACK:
	case NET_MSG_RACE_OVER:
	case NET_MSG_RESULTS:
	case NET_MSG_PLACE:
		return s_lobby.valid && src == s_lobby.hostSlot;

	/*
	 * A STATE NAMES A SLOT, so it must come from the peer that owns
	 * that slot - OR from the host, which relays a rebuilt one for
	 * everybody. netplay-relay.txt 3 and 5.4.
	 */
	case NET_MSG_STATE:
	case NET_MSG_COLLISION:
		if (s_lobby.valid && src == s_lobby.hostSlot)
			return 1;
		return src == slot;

	default:
		return 1;
	}
}

/* ORIGIN  NONE - ours. Our FUN_00455280. */
static void net_dispatch(int src, const unsigned char *p, int length)
{
	const MR_NetMsgHead *head = (const MR_NetMsgHead *)p;
	int type;

	if (length < (int)sizeof(MR_NetMsgHead))
		return;
	type = head->type;

	switch (type) {
	case NET_MSG_JOIN: {
		const MR_NetMsgJoin *m = (const MR_NetMsgJoin *)p;
		char name[NET_NAME_MAX];

		if (length < (int)sizeof(*m) || !netplay_is_host())
			break;
		if (m->version != NET_PROTOCOL_VERSION) {
			mrlog_err("net: peer %d speaks version %u, we speak %d\n",
				      src, (unsigned)m->version, NET_PROTOCOL_VERSION);
			break;
		}
		if (!net_slot_ok(src))
			break;
		net_copy_text(name, NET_NAME_MAX, m->name);
		if (s_h.onJoin)
			s_h.onJoin(src, name, m->bike);
		break;
	}

	case NET_MSG_JOIN_ACK: {
		const MR_NetMsgJoinAck *m = (const MR_NetMsgJoinAck *)p;

		if (length < (int)sizeof(*m))
			break;
		if (m->result != 0)
			mrlog_err("net: join refused, reason %d\n", m->result);
		break;
	}

	case NET_MSG_LOBBY: {
		const MR_NetMsgLobby *m = (const MR_NetMsgLobby *)p;
		int i;

		if (length < (int)sizeof(*m))
			break;
		/*
		 * THE FIRST LOBBY IS WHAT ESTABLISHES THE HOST, so it cannot
		 * be checked against s_lobby.hostSlot - there is nothing to
		 * check against yet. Afterwards it can.
		 */
		if (s_lobby.valid && src != s_lobby.hostSlot)
			break;
		if (!net_slot_ok(m->hostSlot) || m->hostSlot != src)
			break;

		s_lobby.valid      = 1;
		s_lobby.hostSlot   = m->hostSlot;
		s_lobby.track      = m->track;
		s_lobby.laps       = m->laps;
		s_lobby.mode       = m->mode;
		s_lobby.difficulty = m->difficulty;
		s_lobby.pocket     = m->pocket;
		s_lobby.reverse    = m->reverse;
		memcpy(s_lobby.players, m->players, sizeof(s_lobby.players));
		memcpy(s_lobby.bikes,   m->bikes,   sizeof(s_lobby.bikes));
		for (i = 0; i < NET_PEER_MAX; i++)
			net_copy_text(s_lobby.players[i].name,
			              (int)sizeof(s_lobby.players[i].name),
			              m->players[i].name);
		if (s_h.onLobby)
			s_h.onLobby(&s_lobby);
		break;
	}

	case NET_MSG_READY: {
		const MR_NetMsgReady *m = (const MR_NetMsgReady *)p;

		if (length < (int)sizeof(*m) || !netplay_is_host())
			break;
		if (!net_slot_ok(src))
			break;
		/*
		 * HANDED UP, NOT APPLIED HERE. The host owns the roster and has
		 * to republish it. [open] the bike lands in racer+0x0078 in the
		 * original (0xcf); MR_NetPlayer has no field for it yet.
		 */
		if (s_h.onReady)
			s_h.onReady(src, m->ready ? 1 : 0, m->bike);
		break;
	}

	case NET_MSG_START: {
		const MR_NetMsgStart *m = (const MR_NetMsgStart *)p;

		if (length < (int)sizeof(*m) ||
			!net_sender_may(type, src, 0))
			break;
		/*
		 * HOST TIME INTO OUR TIME. Without a sync we have no offset
		 * and the best we can do is start now; the countdown absorbs
		 * a small error.
		 */
		if (s_h.onStart)
			s_h.onStart((unsigned int)((int)m->startMs - s_clockOffset));
		break;
	}

	case NET_MSG_CHAT: {
		const MR_NetMsgChat *m = (const MR_NetMsgChat *)p;
		char text[NET_CHAT_MAX];
		int  who;

		if (length < (int)sizeof(*m))
			break;
		/*
		 * THE SLOT IS THE SENDER'S, not whatever the message claims -
		 * except on a host relay, where the host speaks for the
		 * original author.
		 */
		who = (s_lobby.valid && src == s_lobby.hostSlot) ? m->slot : src;
		if (!net_slot_ok(who))
			break;
		net_copy_text(text, NET_CHAT_MAX, m->text);
		net_chat_push(who, text);
		if (s_h.onChat)
			s_h.onChat(who, text);
		/*
		 * NOT BACK TO THE AUTHOR. A broadcast excludes whoever SENT
		 * the frame - the host - not whoever wrote the line, so it
		 * would echo a guest's own line back. Per peer,
		 * skipping the author.
		 */
		if (netplay_is_host()) {
			MR_NetMsgChat out;
			int           i;

			memset(&out, 0, sizeof(out));
			out.h.type = NET_MSG_CHAT;
			out.slot   = who;
			net_copy_text(out.text, NET_CHAT_MAX, text);
			for (i = 0; i < s_peerCount; i++) {
				if (s_peers[i] == who)
					continue;
				netplay_send(s_peers[i], 1, &out,
					         (int)sizeof(out));
			}
		}
		break;
	}

	case NET_MSG_LEAVE: {
		const MR_NetMsgLeave *m = (const MR_NetMsgLeave *)p;
		int who;

		if (length < (int)sizeof(*m))
			break;
		who = (s_lobby.valid && src == s_lobby.hostSlot) ? m->slot : src;
		if (!net_slot_ok(who))
			break;
		net_forget_slot(who);
		break;
	}

	case NET_MSG_STATE: {
		const MR_NetMsgState *m = (const MR_NetMsgState *)p;

		if (length < (int)sizeof(*m))
			break;
		if (!net_slot_ok(m->slot) ||
			!net_sender_may(type, src, m->slot))
			break;
		/*
		 * STALE STATE IS WORSE THAN NONE. Without this the bike
		 * jumps backwards whenever a packet arrives late, which is
		 * the whole reason for the tick - 5.5.
		 */
		if (s_lastTick[m->slot] != 0 && m->tick <= s_lastTick[m->slot])
			break;
		s_lastTick[m->slot] = m->tick;

		if (s_h.onState)
			s_h.onState(m);

		/*
		 * THE HOST REBUILDS AND FORWARDS - it does NOT pass these
		 * bytes on. netplay.txt 3.1: what the others receive is the
		 * host's own view of that racer, which is what makes every
		 * client agree. The rebuild belongs to the race layer, so
		 * the forward is its job too, from onState.
		 */
		break;
	}

	case NET_MSG_COLLISION: {
		const MR_NetMsgCollision *m = (const MR_NetMsgCollision *)p;

		if (length < (int)sizeof(*m))
			break;
		if (!net_slot_ok(m->slot) ||
			!net_sender_may(type, src, m->slot))
			break;
		if (s_h.onCollision)
			s_h.onCollision(m);
		break;
	}

	case NET_MSG_PLACE: {
		const MR_NetMsgPlace *m = (const MR_NetMsgPlace *)p;

		if (length < (int)sizeof(*m))
			break;
		if (!net_slot_ok(m->slot) ||
			!net_sender_may(type, src, m->slot))
			break;
		if (s_h.onPlace)
			s_h.onPlace(m);
		break;
	}

	case NET_MSG_RACE_OVER:
		if (!net_sender_may(type, src, 0))
			break;
		if (s_h.onRaceOver)
			s_h.onRaceOver();
		break;

	case NET_MSG_RESULTS: {
		const MR_NetMsgResults *m = (const MR_NetMsgResults *)p;

		if (length < (int)sizeof(*m) || !net_sender_may(type, src, 0))
			break;
		if (!net_slot_ok(m->slot))
			break;
		if (s_h.onResults)
			s_h.onResults(m->slot, m->blob, NET_RESULT_LEN);
		break;
	}

	case NET_MSG_TIME_PING: {
		const MR_NetMsgTime *m = (const MR_NetMsgTime *)p;
		MR_NetMsgTime out;

		if (length < (int)sizeof(*m) || !netplay_is_host())
			break;
		memset(&out, 0, sizeof(out));
		out.clientMs = m->clientMs;      /* echoed untouched */
		out.hostMs   = GetTickCount();
		/*
		 * SENT DIRECT, not through net_msg_send, because that
		 * broadcasts from a host and this is a reply to one peer -
		 * so the type is stamped here.
		 */
		out.h.type = NET_MSG_TIME_PONG;
		netplay_send(src, 0, &out, (int)sizeof(out));
		break;
	}

	case NET_MSG_TIME_PONG: {
		const MR_NetMsgTime *m = (const MR_NetMsgTime *)p;
		DWORD now;
		int   rtt;

		if (length < (int)sizeof(*m))
			break;
		now = GetTickCount();
		rtt = (int)(now - m->clientMs);
		if (rtt < 0)
			break;
		/*
		 * THE SMALLEST ROUND TRIP WINS, not an average. Delay is
		 * one-sided - a sample can only be late, never early - so the
		 * minimum is the least contaminated estimate. The offset
		 * assumes the two legs are equal, which is the one thing this
		 * cannot measure.
		 */
		if (!s_clockSynced || rtt < s_clockBestRtt) {
			s_clockBestRtt = rtt;
			s_clockOffset  = (int)m->hostMs - (int)(m->clientMs + rtt / 2);
			s_clockSynced  = 1;
		}
		break;
	}

	default:
		/* A newer peer. Ignore rather than guess. */
		break;
	}
}

/* ORIGIN  NONE - ours */
void netplay_pump(void)
{
	unsigned char buf[NET_PAYLOAD_MAX];
	int           src;
	int           n;

	if (netplay_state() != NET_JOINED)
		return;

	while ((n = netplay_recv(&src, buf, (int)sizeof(buf))) != 0) {
		if (n < 0)
			continue;          /* did not fit; netplay_recv dropped it */
		net_dispatch(src, buf, n);
	}

	/*
	 * KEEP THE CLOCK ESTIMATE FRESH while we are not the host. One
	 * sample a second costs nothing and the minimum-RTT rule only
	 * improves with more of them.
	 */
	if (!netplay_is_host() &&
	    GetTickCount() - s_lastTimePing > 1000)
	    net_send_time_ping();
}
