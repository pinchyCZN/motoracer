/*
 * netplay.h - the relay transport. See docs/subsystems/netplay-relay.txt.
 *
 * ORIGIN  NONE - ours. DirectPlay is imported by ordinal and nothing
 * below the message layer was ever read, so there is no original to
 * transcribe here. The MESSAGE layer above it is reconstructed and
 * documented - subsystems/netplay.txt.
 *
 * This file carries bytes between peers and knows nothing about what
 * they mean. Every machine dials OUT to one relay, so no player opens
 * a port or edits a firewall.
 */

#ifndef MRACER_NETPLAY_H
#define MRACER_NETPLAY_H

#include <stddef.h>     /* offsetof, for the layout assertions */

#ifdef __cplusplus
extern "C" {
#endif

/* Frame flags, netplay-relay.txt 2.1. */
#define NET_FLAG_RELIABLE   0x01
#define NET_FLAG_BROADCAST  0x02

/* Frame kinds. 0 is a game message; the rest are relay control, 2.2. */
#define NET_KIND_PAYLOAD      0
#define NET_KIND_HELLO        1
#define NET_KIND_WELCOME      2
#define NET_KIND_PEER_JOINED  3
#define NET_KIND_PEER_LEFT    4
#define NET_KIND_ROOM_GONE    5
#define NET_KIND_REFUSED      6
#define NET_KIND_KEEPALIVE    7
#define NET_KIND_LIST_REQ     8
#define NET_KIND_ROOM_LIST    9
#define NET_KIND_CREATE_ROOM  10
#define NET_KIND_JOIN_ROOM    11
#define NET_KIND_LEAVE_ROOM   12
#define NET_KIND_ROOM_STATE   13

/* Reasons the relay can refuse. */
#define NET_REFUSE_VERSION    1
#define NET_REFUSE_NOROOM     2
#define NET_REFUSE_FULL       3
#define NET_REFUSE_STATE      4    /* that game is already racing */

#define NET_PROTOCOL_VERSION  2
#define NET_PAYLOAD_MAX       1024
#define NET_ROOMS_MAX         24   /* rooms one ROOM_LIST can carry */

/* A room's state, as the host sets it. */
#define NET_ROOM_WAITING      0
#define NET_ROOM_RACING       1

/*
 * NINE, from the protocol and not from taste: 0xcb carries the whole
 * player table as 0x144 bytes at a stride of 0x24. netplay.txt.
 */
#define NET_PEER_MAX          9

#define NET_PEER_NONE         0     /* also the broadcast dst */

/*
 * THE PLAYER TABLE, g_netPlayers at 0x00508030 - nine slots of 0x24
 * [verified]. 0xcb carries the whole thing as 0x144 bytes, which is
 * 9 * 0x24 exactly, so the slot count is the protocol's and not a
 * guess. netplay.txt.
 *
 * SLOT 0 IS SKIPPED BY EVERY SCAN. They start at 0x00508054, which is
 * slot 1, and the index they return is used directly as the racer
 * index - racer 1 being the local player.
 */
typedef struct {
	int  dpId;          /* +0x00  0 marks the slot empty            */
	char name[20];      /* +0x04  copied to racer+0x2dd4 on 0x64    */
	int  state;         /* +0x18  3 = racing, 4 = ready to start    */
	int  ready;         /* +0x1c  set by 0xca, at 0x00455ce1        */
	int  unk_20;        /* +0x20  unexamined                        */
} MR_NetPlayer;

typedef char MR_NetPlayer_size
        [(sizeof(MR_NetPlayer) == 0x24) ? 1 : -1];
typedef char MR_NetPlayer_off_state
        [(offsetof(MR_NetPlayer, state) == 0x18) ? 1 : -1];
typedef char MR_NetPlayer_off_ready
        [(offsetof(MR_NetPlayer, ready) == 0x1c) ? 1 : -1];

typedef enum {
	NET_IDLE = 0,
	NET_CONNECTING,     /* the TCP connect is in flight       */
	NET_HANDSHAKING,    /* HELLO sent, waiting for WELCOME    */
	NET_BROWSING,       /* on the relay, in no room yet       */
	NET_JOINED,         /* in a room; payloads may flow       */
	NET_FAILED          /* see netplay_refusal                */
} MR_NetState;

/*
 * ONE ENTRY OF THE DIRECTORY. The relay names a room after whoever
 * created it - there are no codes and no passwords, and joining is a
 * matter of trusting the name. netplay-relay.txt 4.
 */
typedef struct {
	int  id;            /* what netplay_join_room takes        */
	int  state;         /* NET_ROOM_WAITING or NET_ROOM_RACING */
	int  players;
	char name[9];       /* the host's, as racer+0x2dd4 holds it */
} MR_NetRoom;

/*
 * netplay_set_relay - where to dial. Call before netplay_host or
 * netplay_join.
 *
 * [open] THE [NET] INI KEYS ARE NOT WIRED. netplay-relay.txt 4.4
 * specifies RelayHost, RelayPort and RoomCode; config.c owns the ini
 * and should call this rather than this file learning to parse one.
 */
void netplay_set_relay(const char *host, int port);

int  netplay_open(void);     /* Winsock up. 1 on success. */
void netplay_close(void);    /* drops the room and the socket */

/*
 * netplay_connect - dial the relay and announce `name`.
 *
 * Returns 1 when the connect has been STARTED, not when it has
 * completed: pump netplay_poll and wait for NET_BROWSING.
 */
int  netplay_connect(const char *name);

/*
 * In a room, or out of one. create and join need NET_BROWSING; the
 * state follows to NET_JOINED when the relay answers.
 */
int  netplay_create_room(void);
int  netplay_join_room(int roomId);
int  netplay_leave_room(void);

/* Ask for a fresh directory. The relay also pushes one on a change. */
int  netplay_request_list(void);

/* Host only: stop the room accepting joins once the race begins. */
int  netplay_set_room_racing(int racing);

int               netplay_room_count(void);
const MR_NetRoom *netplay_room_at(int index);
int               netplay_room_id(void);   /* 0 while browsing */

/* Once a frame. Drives the connect, the handshake and the reads. */
void netplay_poll(void);

MR_NetState netplay_state(void);
int         netplay_refusal(void);   /* NET_REFUSE_*, or 0 */

int netplay_self(void);        /* our peer id, 0 until joined */
int netplay_is_host(void);

/*
 * WHO the host is, not just whether we are. The relay assigns it
 * and says so in WELCOME, which is the only way a client can
 * address its JOIN before any lobby exists.
 */
int netplay_host_peer(void);

/*
 * WHO the host is, not just whether we are. The relay assigns it
 * and says so in WELCOME, which is the only way a client can
 * address its JOIN before any lobby exists.
 */
int netplay_host_peer(void);
int netplay_peer_count(void);  /* others, not counting us */
int netplay_peer_at(int index);

/*
 * netplay_send - one game message to one peer.
 * netplay_broadcast - to every peer in the room except us.
 *
 * `reliable` picks the delivery class, and the choice is NOT free:
 * netplay.txt has the game sending 0x64, 0x65, 0x67, 0x6a, 0x6c and
 * everything from 0xc8 up as guaranteed, and 0x66, 0x68 and 0x69
 * fire-and-forget. Marking the per-tick state reliable would queue
 * stale positions behind each other; see netplay-relay.txt 2.4.
 *
 * Return 1 if the frame was accepted for sending.
 */
int netplay_send(int dst, int reliable, const void *payload, int length);
int netplay_broadcast(int reliable, const void *payload, int length);

/*
 * netplay_recv - take one received game message, oldest first.
 *
 * Returns its length, 0 when the queue is empty, and -1 if the frame
 * did not fit `capacity` (in which case it is dropped). `src` is the
 * sender's peer id as the RELAY stamped it, which is the only form
 * worth trusting - netplay-relay.txt 2.3.
 */
int netplay_recv(int *src, void *payload, int capacity);


/* ================================================================== */
/* THE MESSAGE LAYER.  OURS - netplay-relay.txt 5.                    */
/*                                                                    */
/* Fourteen messages against the original's twenty-four. That table   */
/* carries the original numbers; none of its wire format survives,    */
/* and nothing here can interoperate with retail.                     */
/* ================================================================== */

typedef enum {
	NET_MSG_NONE      = 0,
	NET_MSG_JOIN      = 1,    /* 0x64 */
	NET_MSG_JOIN_ACK  = 2,    /* 0x65 */
	NET_MSG_LOBBY     = 3,    /* 0xc8 0xcb 0x6b 0xd0 0xd1 0xd2 0xd4 0xd5 */
	NET_MSG_READY     = 4,    /* 0xca 0xcf */
	NET_MSG_START     = 5,    /* 0x6a 0xce */
	NET_MSG_CHAT      = 6,    /* 0xd3 */
	NET_MSG_LEAVE     = 7,    /* 0x67 0xcc */
	NET_MSG_STATE     = 8,    /* 0x66 0x6d */
	NET_MSG_COLLISION = 9,    /* 0x6e */
	NET_MSG_PLACE     = 10,   /* 0xc9 */
	NET_MSG_RACE_OVER = 11,   /* 0x6c */
	NET_MSG_RESULTS   = 12,   /* 0xd6 */
	NET_MSG_TIME_PING = 13,   /* 0xcd */
	NET_MSG_TIME_PONG = 14    /* 0xce */
} MR_NetMsgType;

/*
 * NINE, BECAUSE THAT IS WHAT THE RACER HOLDS. racer+0x2dd4 playerName
 * is nine bytes with raceClockMs immediately after it at +0x2ddd, and
 * the original copies a 20-byte table name into it unbounded - see
 * netplay-relay.txt 5.5. Never accept more than fits.
 */
#define NET_NAME_MAX   9
#define NET_CHAT_MAX   96
#define NET_RESULT_LEN 0xf9   /* 0xd6's payload; its layout is [open] */

#pragma pack(push, 1)

typedef struct {
	unsigned short type;        /* MR_NetMsgType */
	unsigned short reserved;    /* keeps the bodies 4-byte aligned */
} MR_NetMsgHead;

typedef struct {
	MR_NetMsgHead  h;
	unsigned short version;
	unsigned short pad;
	int            bike;
	char           name[NET_NAME_MAX];
	char           pad2[3];
} MR_NetMsgJoin;

typedef struct {
	MR_NetMsgHead h;
	int           result;      /* 0 = accepted, else NET_REFUSE_* */
	int           slot;        /* the racer slot assigned */
} MR_NetMsgJoinAck;

/*
 * THE WHOLE LOBBY, EVERY TIME. Idempotent and self-healing; an
 * incremental scheme drifts for good if one update is lost.
 */
typedef struct {
	MR_NetMsgHead h;
	int           hostSlot;
	int           track;
	int           laps;
	int           mode;
	int           difficulty;
	int           pocket;
	int           reverse;
	MR_NetPlayer  players[NET_PEER_MAX];
	/*
	 * BESIDE THE ROSTER, NOT INSIDE IT. MR_NetPlayer mirrors the
	 * original's 0x24 table layout and is size-asserted, so a bike
	 * field cannot go in it - and +0x20 is unexamined, not spare.
	 */
	int           bikes[NET_PEER_MAX];
} MR_NetMsgLobby;

typedef struct {
	MR_NetMsgHead h;
	int           ready;
	int           bike;
} MR_NetMsgReady;

/*
 * A SCHEDULED INSTANT, not a busy-wait. startMs is on the HOST clock;
 * a client adds its own measured offset. netplay-relay.txt 5.5.
 */
typedef struct {
	MR_NetMsgHead h;
	unsigned int  startMs;
} MR_NetMsgStart;

typedef struct {
	MR_NetMsgHead h;
	int           slot;        /* who spoke; the host validates it */
	char          text[NET_CHAT_MAX];
} MR_NetMsgChat;

typedef struct {
	MR_NetMsgHead h;
	int           slot;
} MR_NetMsgLeave;

/*
 * THE SIMULATION CONTRACT, and the one message whose MEANING had to
 * be preserved exactly - netplay.txt 3 and 4. The original packs
 * eight of these into a u32 to save modem bytes; they are the same
 * fields, named.
 *
 * ONE SPEED FEEDS BOTH racer+0x0124 AND racer+0x00ec, so a remote
 * racer's engine speed IS its road speed and no drivetrain runs for
 * it [verified, netplay.txt 3].
 */
typedef struct {
	MR_NetMsgHead h;
	int           slot;
	unsigned int  tick;        /* OURS - netplay-relay.txt 5.5 */
	float         speed;
	float         aimDir[3];
	float         position[3];
	int           raceClockMs;
	int           bestLapMs;
	int           raceState;
	int           stuntOrOverspeed;
	int           lapCounter;
	int           checkpointIndex;
	int           finishPosition;
	int           tickInputWord;
	int           crash;       /* was flag bit 3 */
	float         crashDir[3]; /* read only when crash is set */
} MR_NetMsgState;

typedef struct {
	MR_NetMsgHead h;
	int           slot;
	int           blend;       /* 0 = set outright, else mix in */
	int           crash;
	float         dir[3];
	float         scale;
	float         crashDir[3];
} MR_NetMsgCollision;

/*
 * A TELEPORT OR RESPAWN. The original rebuilds the orientation from
 * two angles through g_cosTable (0xc9); we send the basis, which
 * needs no table and cannot disagree about handedness.
 */
typedef struct {
	MR_NetMsgHead h;
	int           slot;
	float         position[3];
	float         basis[9];
	int           raceClockMs;
} MR_NetMsgPlace;

typedef struct {
	MR_NetMsgHead h;
	int           slot;
	unsigned char blob[NET_RESULT_LEN];
	unsigned char pad[3];
} MR_NetMsgResults;

typedef struct {
	MR_NetMsgHead h;
	unsigned int  clientMs;    /* echoed back untouched */
	unsigned int  hostMs;      /* filled by the host on the pong */
} MR_NetMsgTime;

#pragma pack(pop)

/*
 * The decoded lobby as the host publishes it. A client reads this
 * rather than being driven page to page - 5.3 on why 0xd4 is gone.
 */
typedef struct {
	int          valid;
	int          hostSlot;
	int          track;
	int          laps;
	int          mode;
	int          difficulty;
	int          pocket;
	int          reverse;
	MR_NetPlayer players[NET_PEER_MAX];
	int          bikes[NET_PEER_MAX];   /* see MR_NetMsgLobby */
} MR_NetLobby;

/*
 * WHAT THE GAME REGISTERS. netplay.c decodes and validates; it does
 * NOT reach into racer or race state, so this file stays transport
 * and protocol and the gameplay side owns the applying. It is also
 * what makes a two-instance comparison possible, since every handler
 * is a place to log.
 */
typedef struct {
	void (*onJoin)(int slot, const char *name, int bike);
	void (*onLeave)(int slot);
	void (*onLobby)(const MR_NetLobby *lobby);
	/*
	 * HOST ONLY. The host owns the roster, so it has to be what
	 * republishes it; netplay.c cannot call
	 * netplay_publish_lobby without owning lobby policy.
	 */
	void (*onReady)(int slot, int ready, int bike);
	void (*onStart)(unsigned int localStartMs);
	void (*onChat)(int slot, const char *text);
	void (*onState)(const MR_NetMsgState *st);
	void (*onCollision)(const MR_NetMsgCollision *c);
	void (*onPlace)(const MR_NetMsgPlace *p);
	void (*onRaceOver)(void);
	void (*onResults)(int slot, const unsigned char *blob, int length);
} MR_NetHandlers;

void netplay_set_handlers(const MR_NetHandlers *h);

/*
 * READ THE CURRENT SET BACK. Two files fill this struct - race.c
 * the race half, menu.c the lobby half - and whichever installs
 * second must not clear the other's.
 */
void netplay_get_handlers(MR_NetHandlers *out);

/*
 * netplay_pump - drain the transport and dispatch. Call once a frame
 * AFTER netplay_poll. Ours for FUN_00455280.
 */
void netplay_pump(void);

/* Senders. Each returns 1 when the message was accepted for sending. */
int netplay_send_join(const char *name, int bike);
int netplay_send_ready(int ready, int bike);
int netplay_send_chat(const char *text);
int netplay_send_state(const MR_NetMsgState *st);
int netplay_send_collision(const MR_NetMsgCollision *c);
int netplay_send_leave(void);

/* Host only - refused when we are not the host. */
int netplay_publish_lobby(const MR_NetLobby *lobby);
int netplay_send_start(unsigned int hostStartMs);
int netplay_send_place(const MR_NetMsgPlace *p);
int netplay_send_race_over(void);
int netplay_send_results(int slot, const unsigned char *blob, int length);

/* The host clock as this machine estimates it. */
int          netplay_clock_synced(void);
unsigned int netplay_host_now(void);

const MR_NetLobby *netplay_lobby(void);

/* The chat scrollback, newest last. */
int         netplay_chat_count(void);
const char *netplay_chat_at(int index, int *slot);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_NETPLAY_H */
