#include "netclient.h"
#include "huffman.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define NET_WOULD_BLOCK (WSAGetLastError() == WSAEWOULDBLOCK)
#define NET_IN_PROGRESS (WSAGetLastError() == WSAEWOULDBLOCK)
#define NET_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define NET_WOULD_BLOCK (errno == EAGAIN || errno == EWOULDBLOCK)
// The 3DS socket service and the emulator do not agree on which of these a pending connect reports.
#define NET_IN_PROGRESS (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK || errno == EALREADY)
#define NET_CLOSE close
#endif

#include "packet_lengths.h"

#define OP_NO_TIMEOUT 93
#define OP_WALK 215
#define OP_CHEAT 44
#define OP_REBUILD_NORMAL 162
#define OP_REBUILD_REGION 214
#define OP_PLAYER_INFO 225
#define OP_NPC_INFO 32
#define OP_PLAYER_OPTION 44
#define OP_IF_OPEN 155
#define OP_IF_CLOSE 149
#define OP_IF_TEXT 171
#define OP_IF_MODEL 130
#define OP_IF_OBJECT 50
#define OP_IF_ANGLE 132
#define OP_IF_HIDE 21
#define OP_CONTINUE 132
#define OP_STAT 38
#define OP_OBJ_REVEAL 33
#define OP_OBJ_ADD 135
#define OP_OBJ_COUNT 14
#define OP_OBJ_DEL 240
#define OP_INV_FULL 105
#define OP_INV_PARTIAL 22
#define OP_INV_CLEAR 144
#define OP_LOGOUT 86
#define OP_MESSAGE 70
#define OP_ZONE_FOLLOWS 26
#define OP_ZONE_ENCLOSED 230
#define OP_ZONE_RESET 112
#define OP_LOC_ADD 179
#define OP_LOC_DELETE 195
#define OP_ROOT_INTERFACE 145
#define OP_BUTTON 155
#define OP_BUTTON_SECOND 196
#define OP_CLOSE_MODAL 184
#define OP_IF_NPC_HEAD 73
#define OP_IF_PLAYER_HEAD 66
#define OP_IF_ANIM 36
#define OP_RUN_SCRIPT 115
#define OP_ANSWER_NUMBER 23
#define OP_ANSWER_NAME 244
#define OP_ANSWER_TEXT 65
#define OP_PUBLIC_CHAT 237
#define OP_PRIVATE_CHAT 201
#define OP_CLAN_JOIN 104
#define OP_PRIVATE_IN 0
#define OP_PRIVATE_ECHO 71
#define OP_CLAN_MESSAGE 54
#define OP_CLAN_FULL 55
#define OP_CLAN_UPDATE 196
#define MAX_CHAT_BYTES 256
#define OP_FRIEND_UPDATE 62
#define OP_IGNORE_LIST 126
#define OP_FRIENDS_LOADED 197
#define OP_FRIEND_ADD 120
#define OP_FRIEND_DEL 57
#define OP_IGNORE_ADD 34
#define OP_IGNORE_DEL 213
#define OP_IF_BUTTON 10
#define OP_VARP_SMALL 60
#define OP_VARP_LARGE 226
#define OP_VARBIT_SMALL 37
#define OP_VARBIT_LARGE 84
#define OP_VARP_RESET 89
#define LOBBY_ROOT 549

// ---- RSA with the public key from the client source ----

#define LIMBS 33

static const uint8_t MODULUS[128] = {
	0x8a,0x1b,0x7d,0x89,0xcd,0x0f,0x51,0x06,0x07,0x67,0xc6,0x63,0x14,0x62,0x8e,0xb3,
	0x58,0x31,0x2b,0x8e,0x1c,0xa4,0xc5,0x5e,0x0c,0xff,0x3f,0xa6,0x3d,0x6d,0x3c,0xdd,
	0x02,0xca,0x15,0xb3,0x03,0x13,0xf3,0x80,0xfb,0x5d,0xb5,0x08,0x59,0xd2,0x85,0xe7,
	0xe9,0x16,0x6e,0xb7,0x29,0x2a,0x08,0xb1,0x90,0x87,0x09,0x40,0x0c,0x2a,0xa4,0x49,
	0x37,0xe2,0x1b,0x23,0x1e,0xb1,0x98,0xe9,0xf7,0x05,0xf5,0xaf,0x28,0x91,0xe1,0xcd,
	0x16,0xe1,0xd5,0xfb,0x87,0xde,0xf3,0x29,0xcf,0xb2,0x09,0x3d,0x14,0xec,0xcf,0x33,
	0x9b,0x35,0xe3,0x7a,0x49,0x7d,0x8e,0x7a,0x5e,0xa0,0x09,0x9b,0x99,0xdc,0x63,0x69,
	0x07,0x84,0xa4,0x5d,0xad,0xc5,0xac,0xea,0xd2,0xc4,0xd4,0x8c,0x03,0x82,0x81,0x53};

static void big_from_bytes(uint32_t* out, const uint8_t* bytes, int size) {
	memset(out, 0, sizeof(uint32_t) * LIMBS);
	for (int i = 0; i < size; i++) {
		int position = size - 1 - i;
		out[position / 4] |= (uint32_t)bytes[i] << (position % 4 * 8);
	}
}

static int big_compare(const uint32_t* a, const uint32_t* b) {
	for (int i = LIMBS - 1; i >= 0; i--) {
		if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
	}
	return 0;
}

static void big_subtract(uint32_t* a, const uint32_t* b) {
	uint64_t borrow = 0;
	for (int i = 0; i < LIMBS; i++) {
		uint64_t d = (uint64_t)a[i] - b[i] - borrow;
		a[i] = (uint32_t)d;
		borrow = d >> 63;
	}
}

static void big_add(uint32_t* a, const uint32_t* b) {
	uint64_t carry = 0;
	for (int i = 0; i < LIMBS; i++) {
		uint64_t s = (uint64_t)a[i] + b[i] + carry;
		a[i] = (uint32_t)s;
		carry = s >> 32;
	}
}

static void big_double(uint32_t* a) {
	uint32_t carry = 0;
	for (int i = 0; i < LIMBS; i++) {
		uint32_t next = a[i] >> 31;
		a[i] = a[i] << 1 | carry;
		carry = next;
	}
}

// out = a * b mod n, by adding and doubling. Only a few multiplies happen per login.
static void big_mulmod(uint32_t* out, const uint32_t* a, const uint32_t* b, const uint32_t* n) {
	uint32_t r[LIMBS];
	memset(r, 0, sizeof(r));
	for (int bit = 1023; bit >= 0; bit--) {
		big_double(r);
		if (big_compare(r, n) >= 0) big_subtract(r, n);
		if (a[bit / 32] >> (bit % 32) & 1) {
			big_add(r, b);
			if (big_compare(r, n) >= 0) big_subtract(r, n);
		}
	}
	memcpy(out, r, sizeof(r));
}

int net_rsa_encrypt(const uint8_t* data, int size, uint8_t* out) {
	uint32_t n[LIMBS], m[LIMBS], r[LIMBS];
	big_from_bytes(n, MODULUS, 128);
	big_from_bytes(m, data, size);
	memcpy(r, m, sizeof(r));
	for (int i = 0; i < 16; i++) big_mulmod(r, r, r, n); // exponent 65537 = 2^16 + 1
	big_mulmod(r, r, m, n);
	uint8_t bytes[128];
	for (int i = 0; i < 128; i++) bytes[127 - i] = (uint8_t)(r[i / 4] >> (i % 4 * 8));
	int first = 0;
	while (first < 127 && bytes[first] == 0) first++;
	int length = 0;
	if (bytes[first] & 0x80) out[length++] = 0; // a signed big-endian integer, as Java writes it
	memcpy(out + length, bytes + first, (size_t)(128 - first));
	return length + 128 - first;
}

// ---- packet writing ----

typedef struct {
	uint8_t* data;
	int size;
} Out;

static void p1(Out* o, int v) { o->data[o->size++] = (uint8_t)v; }
static void p2(Out* o, int v) { p1(o, v >> 8); p1(o, v); }
static void p4(Out* o, uint32_t v) { p2(o, (int)(v >> 16)); p2(o, (int)(v & 0xFFFF)); }
static void p8(Out* o, uint64_t v) { p4(o, (uint32_t)(v >> 32)); p4(o, (uint32_t)v); }
static void pstr(Out* o, const char* s) {
	while (*s) p1(o, *s++);
	p1(o, 0);
}

static uint64_t encode37(const char* name) {
	uint64_t hash = 0;
	for (int i = 0; name[i] && i < 12; i++) {
		char c = name[i];
		hash *= 37;
		if (c >= 'A' && c <= 'Z') hash += (uint64_t)(c + 1 - 'A');
		else if (c >= 'a' && c <= 'z') hash += (uint64_t)(c + 1 - 'a');
		else if (c >= '0' && c <= '9') hash += (uint64_t)(c + 27 - '0');
	}
	while (hash % 37 == 0 && hash != 0) hash /= 37;
	return hash;
}

// ---- socket helpers ----

static int send_all(NetClient* c, const uint8_t* data, int size) {
	int sent = 0;
	while (sent < size) {
		int n = (int)send(c->sock, (const char*)data + sent, (size_t)(size - sent), 0);
		if (n <= 0) {
			if (n < 0 && NET_WOULD_BLOCK) continue;
			c->connected = 0;
			return -1;
		}
		sent += n;
	}
	return 0;
}

// Waits up to timeoutMs for exactly size bytes.
static int recv_exact(NetClient* c, uint8_t* out, int size, int timeoutMs) {
	int got = 0;
	while (got < size) {
		fd_set set;
		FD_ZERO(&set);
		FD_SET(c->sock, &set);
		struct timeval tv = {timeoutMs / 1000, timeoutMs % 1000 * 1000};
		if (select(c->sock + 1, &set, NULL, NULL, &tv) <= 0) return -1;
		int n = (int)recv(c->sock, (char*)out + got, (size_t)(size - got), 0);
		if (n <= 0) return -1;
		got += n;
	}
	return 0;
}

static void set_blocking(int sock, int blocking) {
#ifdef _WIN32
	u_long on = blocking ? 0 : 1;
	ioctlsocket(sock, FIONBIO, &on);
#else
	int flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
#endif
}

static void set_nonblocking(int sock) {
	set_blocking(sock, 0);
}

int (*net_connect_hook)(int sock, const void* address, int addressSize, int timeoutMs) = NULL;

// Connects, giving up after timeoutMs so an absent server cannot hang the app. Returns 0 on success.
static int connect_timed(int sock, const struct sockaddr_in* address, int timeoutMs) {
	if (net_connect_hook) return net_connect_hook(sock, address, (int)sizeof(*address), timeoutMs);
	set_blocking(sock, 0);
	if (connect(sock, (const struct sockaddr*)address, sizeof(*address)) != 0) {
		if (!NET_IN_PROGRESS) return -1;
		fd_set writable, failed;
		FD_ZERO(&writable);
		FD_ZERO(&failed);
		FD_SET(sock, &writable);
		FD_SET(sock, &failed);
		struct timeval tv = {timeoutMs / 1000, timeoutMs % 1000 * 1000};
		if (select(sock + 1, NULL, &writable, &failed, &tv) <= 0 || FD_ISSET(sock, &failed)) return -1;
		int problem = 0;
		socklen_t size = sizeof(problem);
		if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&problem, &size) == 0 && problem != 0) return -1;
	}
	set_blocking(sock, 1);
	return 0;
}

// ---- packet reading ----

typedef struct {
	const uint8_t* data;
	int size, pos, bitPos;
} In;

static int g1(In* b) { return b->pos < b->size ? b->data[b->pos++] : 0; }
static int g2(In* b) { int a = g1(b); return a << 8 | g1(b); }
static int g2add(In* b) { int a = g1(b); return a << 8 | ((g1(b) - 128) & 0xFF); }
static int g2le_add(In* b) { int a = (g1(b) - 128) & 0xFF; return g1(b) << 8 | a; }
static int g2le(In* b) { int a = g1(b); return g1(b) << 8 | a; }
static int ig2add(In* b) { int a = (g1(b) - 128) & 0xFF; return g1(b) << 8 | a; }
static int32_t ig4(In* b) { int a = g1(b), v = g1(b), d = g1(b), e = g1(b); return (int32_t)((uint32_t)e << 24 | (uint32_t)d << 16 | (uint32_t)v << 8 | (uint32_t)a); }
static int g1add(In* b) { return (g1(b) - 128) & 0xFF; }
static int g1neg(In* b) { return (-g1(b)) & 0xFF; }
static int g1sub(In* b) { return (128 - g1(b)) & 0xFF; }

// Middle-endian, as the rebuild packet sends region keys.
static int32_t mg4(In* b) {
	int a = g1(b), v = g1(b), c = g1(b), d = g1(b);
	return (int32_t)((uint32_t)v << 24 | (uint32_t)a << 16 | (uint32_t)d << 8 | (uint32_t)c);
}

static int gbits(In* b, int count) {
	int value = 0;
	for (int i = 0; i < count; i++, b->bitPos++) {
		int byte = b->bitPos >> 3;
		int bit = byte < b->size ? b->data[byte] >> (7 - (b->bitPos & 7)) & 1 : 0;
		value = value << 1 | bit;
	}
	return value;
}

static NetPicture* picture_slot(NetClient* c, int id) {
	for (int i = 0; i < c->pictureCount; i++) if (c->pictures[i].id == id) return &c->pictures[i];
	int at = c->pictureCount < NET_MAX_PICTURES ? c->pictureCount++ : c->pictureNext;
	c->pictureNext = (at + 1) % NET_MAX_PICTURES;
	memset(&c->pictures[at], 0, sizeof(c->pictures[at]));
	c->pictures[at].id = id;
	return &c->pictures[at];
}

static void handle_rebuild(NetClient* c, const uint8_t* data, int length) {
	In b = {data, length, 0, 0};
	int oldZoneX = c->zoneX, oldZoneY = c->zoneY;
	int localX = g2add(&b);
	int count = (length - b.pos) / 16;
	if (count > NET_MAX_REGIONS) count = NET_MAX_REGIONS;
	for (int i = 0; i < count; i++)
		for (int k = 0; k < 4; k++) c->keys[i][k] = mg4(&b);
	c->plane = g1sub(&b);
	c->zoneX = g2(&b);
	c->zoneY = g2add(&b);
	c->localY = g2add(&b);
	c->localX = localX;
	c->regionCount = 0;
	for (int rx = (c->zoneX - 6) / 8; rx <= (c->zoneX + 6) / 8; rx++)
		for (int ry = (c->zoneY - 6) / 8; ry <= (c->zoneY + 6) / 8; ry++)
			if (c->regionCount < NET_MAX_REGIONS) c->regions[c->regionCount++] = rx << 8 | ry;
	// NPCs stay known across a rebuild, so their positions follow the new scene origin.
	int shiftX = (c->zoneX - oldZoneX) * 8, shiftY = (c->zoneY - oldZoneY) * 8;
	for (int i = 0; i < c->npcCount; i++) {
		NetNpc* n = &c->npcs[i];
		n->x -= shiftX;
		n->y -= shiftY;
		n->viewX -= (float)shiftX;
		n->viewY -= (float)shiftY;
	}
	for (int i = 0; i < c->playerCount; i++) {
		NetNpc* n = &c->players[i];
		n->x -= shiftX;
		n->y -= shiftY;
		n->viewX -= (float)shiftX;
		n->viewY -= (float)shiftY;
	}
	c->self.x = c->localX;
	c->self.y = c->localY;
	// The server sends the items of the new scene again.
	c->groundCount = 0;
	c->groundVersion++;
	c->rebuildPending = 1;
	c->rebuilds++;
	c->changeCount = 0; // anything older belonged to the previous scene
}

static void add_hit(NetNpc* n, int damage, int type) {
	if (n->newHitCount >= 4) return;
	n->newHitDamage[n->newHitCount] = (uint16_t)damage;
	n->newHitType[n->newHitCount++] = (uint8_t)type;
}

static int read_smart(In* b) {
	int first = g1(b);
	return first < 128 ? first : (first << 8 | g1(b)) - 0x8000;
}

static int g4(In* b) {
	int a = g2(b);
	return (int)((uint32_t)a << 16 | (uint32_t)g2(b));
}

static NetInv* inventory_for(NetClient* c, int id) {
	for (int i = 0; i < c->invCount; i++) if (c->invs[i].id == id) return &c->invs[i];
	if (c->invCount == NET_MAX_INVS) return NULL;
	NetInv* inv = &c->invs[c->invCount++];
	inv->id = id;
	inv->count = 0;
	return inv;
}

static void set_inventory_slot(NetInv* inv, int slot, int item, int amount) {
	if (slot < 0 || slot >= NET_INV_SIZE) return;
	while (inv->count <= slot) {
		inv->items[inv->count] = -1;
		inv->amounts[inv->count++] = 0;
	}
	inv->items[slot] = item;
	inv->amounts[slot] = item < 0 ? 0 : amount;
}

static void start_entity(NetNpc* n, int index) {
	memset(n, 0, sizeof(*n));
	n->index = index;
	n->pendingSeq = NET_NO_SEQ;
	n->faceEntity = n->seqId = n->moveSeq = -1;
}

static void move_npc(NetNpc* n, int direction) {
	static const int DX[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, DY[8] = {1, 1, 1, 0, 0, -1, -1, -1};
	static const int ANGLES[8] = {768, 1024, 1280, 512, 1536, 256, 0, 1792};
	n->x += DX[direction];
	n->y += DY[direction];
	n->angle = ANGLES[direction];
}

// Protocol.npcCombat. Damage, graphics and speech are skipped for now.
static void read_npc_extended(NetClient* c, NetNpc* n, In* b) {
	int flags = g1(b);
	if (flags & 0x8) flags += g1(b) << 8;
	if (flags & 0x40) {
		int damage = g1(b), type = g1neg(b);
		add_hit(n, damage, type);
		n->bar = (uint8_t)g1sub(b);
		n->barNew = 1;
	}
	if (flags & 0x2) {
		int damage = g1neg(b), type = g1sub(b);
		add_hit(n, damage, type);
	}
	if (flags & 0x10) {
		int seq = g2(b);
		n->pendingDelay = g1(b);
		n->pendingSeq = seq == 65535 ? -1 : seq;
		c->npcSeqUpdates++;
	}
	if (flags & 0x4) {
		n->faceEntity = g2add(b);
		if (n->faceEntity == 65535) n->faceEntity = -1;
		c->npcFaceUpdates++;
	}
	if (flags & 0x80) b->pos += 6;
	if (flags & 0x1) n->type = g2le(b);
	if (flags & 0x20) {
		int length = 0, letter;
		while (b->pos < b->size && (letter = g1(b)) != 0) {
			if (length < (int)sizeof(n->overhead) - 1) n->overhead[length++] = (char)letter;
		}
		n->overhead[length] = 0;
		n->overheadNew = 1;
	}
	if (flags & 0x100) {
		int count = g1neg(b);
		b->pos += count * 5;
	}
	if (flags & 0x200) {
		n->faceX = g2add(b);
		n->faceY = g2(b);
		c->npcFaceUpdates++;
	}
}

static void handle_npc_info(NetClient* c, const uint8_t* data, int length) {
	static const int ANGLES[8] = {768, 1024, 1280, 512, 1536, 256, 0, 1792};
	In b = {data, length, 0, 0};
	c->npcUpdates++;
	int count = gbits(&b, 8);
	if (count > c->npcCount) {
		c->npcErrors++;
		return;
	}
	int kept = 0, extendedCount = 0;
	static uint8_t extended[NET_MAX_NPCS];
	for (int i = 0; i < count; i++) {
		NetNpc n = c->npcs[i];
		if (gbits(&b, 1)) {
			int type = gbits(&b, 2);
			if (type == 3) continue;
			int hasExtended = type == 0;
			if (type == 1) {
				move_npc(&n, gbits(&b, 3));
				hasExtended = gbits(&b, 1);
			} else if (type == 2) {
				if (gbits(&b, 1)) {
					move_npc(&n, gbits(&b, 3));
					move_npc(&n, gbits(&b, 3));
				} else {
					move_npc(&n, gbits(&b, 3));
				}
				hasExtended = gbits(&b, 1);
			}
			if (hasExtended) extended[extendedCount++] = (uint8_t)kept;
		}
		c->npcs[kept++] = n;
	}
	c->npcCount = kept;
	while (length * 8 - b.bitPos >= 27) {
		int index = gbits(&b, 15);
		if (index == 32767) break;
		gbits(&b, 1);
		int angle = ANGLES[gbits(&b, 3)];
		int hasExtended = gbits(&b, 1);
		int dy = gbits(&b, 5);
		int type = gbits(&b, 14);
		int dx = gbits(&b, 5);
		if (dy > 15) dy -= 32;
		if (dx > 15) dx -= 32;
		if (c->npcCount == NET_MAX_NPCS) continue;
		NetNpc* n = &c->npcs[c->npcCount++];
		start_entity(n, index);
		n->type = type;
		n->x = c->localX + dx;
		n->y = c->localY + dy;
		n->angle = angle;
		if (hasExtended) extended[extendedCount++] = (uint8_t)(c->npcCount - 1);
	}
	b.pos = (b.bitPos + 7) >> 3;
	for (int i = 0; i < extendedCount; i++) read_npc_extended(c, &c->npcs[extended[i]], &b);
	if (b.pos != length) c->npcErrors++;
}

static void decode37(uint64_t value, char* out) {
	static const char TABLE[] = "_abcdefghijklmnopqrstuvwxyz0123456789";
	char reversed[13];
	int n = 0;
	while (value && n < 12) {
		reversed[n++] = TABLE[value % 37];
		value /= 37;
	}
	for (int i = 0; i < n; i++) {
		char ch = reversed[n - 1 - i];
		if (ch == '_') ch = ' ';
		else if (ch >= 'a' && ch <= 'z' && (i == 0 || out[i - 1] == ' ')) ch = (char)(ch - 32);
		out[i] = ch;
	}
	out[n] = 0;
}

// Player.decodeAppearance
static void read_look(NetLook* l, const uint8_t* data, int length) {
	In b = {data, length, 0, 0};
	int flags = g1(&b), showSkill = flags & 4;
	memset(l, 0, sizeof(*l));
	l->female = (uint8_t)(flags & 1);
	l->size = (uint8_t)((flags >> 3 & 7) + 1);
	l->npcId = -1;
	g1(&b);
	g1(&b);
	for (int i = 0; i < 12; i++) {
		int high = g1(&b);
		if (high == 0) continue;
		int value = high << 8 | g1(&b);
		if (i == 0 && value == 65535) {
			l->npcId = g2(&b);
			g1(&b);
			break;
		}
		l->slots[i] = (uint16_t)value;
	}
	for (int i = 0; i < 5; i++) l->colours[i] = (uint8_t)g1(&b);
	l->basId = g2(&b);
	uint64_t name = 0;
	for (int i = 0; i < 8; i++) name = name << 8 | (uint64_t)g1(&b);
	decode37(name, l->name);
	l->combat = (uint8_t)g1(&b);
	if (showSkill) l->skill = g2(&b);
	l->valid = 1;
}

static int gsmart_size(In* b) {
	return b->pos < b->size && b->data[b->pos] >= 128 ? 2 : 1;
}

// Protocol.readExtendedPlayerInfo. Chat, damage and graphics are skipped for now.
static void add_chat(NetClient* c, int kind, const char* who, const char* text) {
	NetChat* line = &c->chat[c->chatVersion % NET_CHAT_LINES];
	line->kind = (uint8_t)kind;
	snprintf(line->who, sizeof(line->who), "%s", who);
	snprintf(line->text, sizeof(line->text), "%s", text);
	c->chatVersion++;
}

static uint64_t g8(In* b) {
	uint64_t high = (uint32_t)g4(b);
	return high << 32 | (uint32_t)g4(b);
}

static int ignored(const NetClient* c, uint64_t key) {
	for (int i = 0; i < c->ignoreCount; i++) if (c->ignores[i] == key) return 1;
	return 0;
}

// A message: its length, then the packed text.
static void read_packed(In* b, char* out, int size) {
	int length = g1(b);
	if (length >= 128) length = (length - 128) << 8 | g1(b);
	if (length > size - 1) length = size - 1;
	int used = b->pos <= b->size ? huffman_decode(b->data + b->pos, (int)(b->size - b->pos), (uint8_t*)out, length) : -1;
	out[used < 0 ? 0 : length] = 0;
	if (used > 0) b->pos += (size_t)used;
}

static void read_string(In* b, char* out, int size) {
	int n = 0;
	while (b->pos < b->size && b->data[b->pos] != 0) {
		if (n < size - 1) out[n++] = (char)b->data[b->pos];
		b->pos++;
	}
	b->pos++;
	out[n] = 0;
}

// A chat update on a player: stored backwards, unlike other messages.
static void read_public_chat(NetClient* c, NetNpc* n, int id, In* b) {
	int effects = g1(b) | g1(b) << 8, rights = g1(b), length = g1(b);
	uint8_t reversed[256];
	for (int i = 0; i < length && b->pos + (size_t)i < b->size; i++) reversed[length - 1 - i] = b->data[b->pos + (size_t)i];
	b->pos += (size_t)length;
	if (effects & 0x8000) return; // a quick chat phrase, not supported yet
	char who[16], text[NET_CHAT_TEXT];
	In packed = {reversed, (size_t)length, 0, 0};
	read_packed(&packed, text, sizeof(text));
	if (id == NET_SELF_ID) net_name_from_key(c->selfKey, who, sizeof(who));
	else snprintf(who, sizeof(who), "%s", c->looks[id].name);
	if (rights <= 1 && ignored(c, net_name_key(who))) return;
	add_chat(c, NET_CHAT_PUBLIC, who, text);
	snprintf(n->overhead, sizeof(n->overhead), "%s", text);
	n->overheadNew = 1;
}

static void read_player_extended(NetClient* c, NetNpc* n, int id, In* b) {
	int flags = g1(b);
	if (flags & 0x10) flags += g1(b) << 8;
	if (flags & 0x80) read_public_chat(c, n, id, b);
	if (flags & 0x1) {
		int damage = read_smart(b), type = g1add(b);
		add_hit(n, damage, type);
		n->bar = (uint8_t)g1sub(b);
		n->barNew = 1;
	}
	if (flags & 0x8) {
		int seq = g2(b);
		n->pendingDelay = g1(b);
		n->pendingSeq = seq == 65535 ? -1 : seq;
		c->playerSeqUpdates++;
	}
	if (flags & 0x4) {
		int length = g1add(b);
		if (b->pos + length <= b->size) read_look(&c->looks[id], b->data + b->pos, length);
		b->pos += length;
		c->lookUpdates++;
	}
	if (flags & 0x2) {
		n->faceEntity = g2add(b);
		if (n->faceEntity == 65535) n->faceEntity = -1;
	}
	if (flags & 0x400) b->pos += 9;
	if (flags & 0x20) {
		int length = 0, letter;
		while (b->pos < b->size && (letter = g1(b)) != 0) {
			if (length < (int)sizeof(n->overhead) - 1) n->overhead[length++] = (char)letter;
		}
		n->overhead[length] = 0;
		n->overheadNew = 1;
	}
	if (flags & 0x200) {
		int damage = read_smart(b), type = g1sub(b);
		add_hit(n, damage, type);
	}
	if (flags & 0x800) {
		int count = g1neg(b);
		b->pos += count * 5;
	}
	if (flags & 0x100) b->pos += 6;
	if (flags & 0x40) {
		n->faceX = g2(b);
		n->faceY = g2le_add(b);
	}
}

// Reads one player's walk or run. Returns whether an extended update follows.
static int read_player_move(NetNpc* n, In* b, int type) {
	if (type == 1) {
		move_npc(n, gbits(b, 3));
		n->running = 0;
		return gbits(b, 1);
	}
	if (gbits(b, 1)) {
		move_npc(n, gbits(b, 3));
		move_npc(n, gbits(b, 3));
		n->running = 1;
	} else {
		move_npc(n, gbits(b, 3));
		n->running = 0;
	}
	return gbits(b, 1);
}

static void handle_player_info(NetClient* c, const uint8_t* data, int length) {
	static const int ANGLES[8] = {768, 1024, 1280, 512, 1536, 256, 0, 1792};
	static int16_t extended[NET_MAX_PLAYERS + 1]; // a position in c->players, or -1 for the local player
	In b = {data, length, 0, 0};
	int extendedCount = 0;
	c->playerUpdates++;
	c->self.x = c->localX;
	c->self.y = c->localY;
	if (gbits(&b, 1)) {
		int type = gbits(&b, 2);
		if (type == 0) {
			extended[extendedCount++] = -1;
		} else if (type == 3) {
			int y = gbits(&b, 7);
			gbits(&b, 1);
			c->plane = gbits(&b, 2);
			if (gbits(&b, 1)) extended[extendedCount++] = -1;
			c->self.x = gbits(&b, 7);
			c->self.y = y;
			c->teleports++;
		} else {
			if (read_player_move(&c->self, &b, type)) extended[extendedCount++] = -1;
			c->moves++;
		}
		c->localX = c->self.x;
		c->localY = c->self.y;
	}
	int count = gbits(&b, 8);
	if (count > c->playerCount) {
		c->playerErrors++;
		return;
	}
	int kept = 0;
	for (int i = 0; i < count; i++) {
		NetNpc n = c->players[i];
		if (gbits(&b, 1)) {
			int type = gbits(&b, 2);
			if (type == 3) continue;
			if (type == 0) extended[extendedCount++] = (int16_t)kept;
			else if (read_player_move(&n, &b, type)) extended[extendedCount++] = (int16_t)kept;
		}
		c->players[kept++] = n;
	}
	c->playerCount = kept;
	while (length * 8 - b.bitPos >= 11) {
		int index = gbits(&b, 11);
		if (index == 2047) break;
		int hasExtended = gbits(&b, 1);
		int dx = gbits(&b, 5);
		int angle = ANGLES[gbits(&b, 3)];
		gbits(&b, 1);
		int dy = gbits(&b, 5);
		if (dx > 15) dx -= 32;
		if (dy > 15) dy -= 32;
		if (c->playerCount == NET_MAX_PLAYERS) continue;
		NetNpc* n = &c->players[c->playerCount++];
		start_entity(n, index);
		n->x = c->localX + dx;
		n->y = c->localY + dy;
		n->angle = angle;
		if (hasExtended) extended[extendedCount++] = (int16_t)(c->playerCount - 1);
	}
	b.pos = (b.bitPos + 7) >> 3;
	for (int i = 0; i < extendedCount; i++) {
		if (extended[i] < 0) read_player_extended(c, &c->self, NET_SELF_ID, &b);
		else read_player_extended(c, &c->players[extended[i]], c->players[extended[i]].index, &b);
	}
	if (b.pos != length) c->playerErrors++;
}

static void ground_add(NetClient* c, int x, int y, int id, int amount) {
	if (x < 0 || y < 0 || x >= 104 || y >= 104 || c->groundCount == NET_MAX_GROUND) return;
	c->ground[c->groundCount++] = (NetGround){(uint8_t)x, (uint8_t)y, (uint8_t)c->plane, id, amount};
	c->groundVersion++;
}

// Reads one of the ground item packets. They use the same zone as object changes.
static void read_ground(NetClient* c, int opcode, In* b) {
	if (opcode == OP_OBJ_REVEAL) {
		int id = g2le(b), coords = g1(b), amount = g2add(b);
		ground_add(c, c->chunkX + (coords >> 4 & 7), c->chunkY + (coords & 7), id, amount);
	} else if (opcode == OP_OBJ_ADD) {
		int owner = g2le_add(b), coords = g1neg(b), amount = g2le(b), id = g2le(b);
		if (owner != c->selfId) ground_add(c, c->chunkX + (coords >> 4 & 7), c->chunkY + (coords & 7), id, amount);
	} else if (opcode == OP_OBJ_COUNT) {
		int coords = g1(b), id = g2(b) & 0x7FFF, before = g2(b), after = g2(b);
		int x = c->chunkX + (coords >> 4 & 7), y = c->chunkY + (coords & 7);
		for (int i = 0; i < c->groundCount; i++) {
			NetGround* g = &c->ground[i];
			if (g->x != x || g->y != y || g->plane != c->plane || g->id != id || g->amount != before) continue;
			g->amount = after;
			c->groundVersion++;
			break;
		}
	} else {
		int coords = g1sub(b), id = g2(b) & 0x7FFF;
		int x = c->chunkX + (coords >> 4 & 7), y = c->chunkY + (coords & 7);
		for (int i = 0; i < c->groundCount; i++) {
			NetGround* g = &c->ground[i];
			if (g->x != x || g->y != y || g->plane != c->plane || g->id != id) continue;
			memmove(g, g + 1, sizeof(*g) * (size_t)(c->groundCount - i - 1));
			c->groundCount--;
			c->groundVersion++;
			break;
		}
	}
}

static int is_ground_packet(int opcode) {
	return opcode == OP_OBJ_REVEAL || opcode == OP_OBJ_ADD || opcode == OP_OBJ_COUNT || opcode == OP_OBJ_DEL;
}

static void push_change(NetClient* c, int kind, int x, int y, int shape, int rotation, int id) {
	if (c->changeCount == NET_MAX_CHANGES) return;
	int slot = (c->changeHead + c->changeCount++) % NET_MAX_CHANGES;
	c->changes[slot] = (NetLocChange){(uint8_t)kind, (uint8_t)x, (uint8_t)y, (uint8_t)c->plane, (uint8_t)shape, (uint8_t)rotation, id};
}

int net_next_change(NetClient* c, NetLocChange* out) {
	if (c->changeCount == 0) return 0;
	*out = c->changes[c->changeHead];
	c->changeHead = (c->changeHead + 1) % NET_MAX_CHANGES;
	c->changeCount--;
	return 1;
}

static void read_loc_add(NetClient* c, In* b) {
	int packed = g1add(b), coords = g1(b), id = g2add(b);
	push_change(c, NET_CHANGE_ADD, (coords >> 4 & 7) + c->chunkX, (coords & 7) + c->chunkY, packed >> 2, packed & 3, id);
	c->locAdds++;
}

static void read_loc_delete(NetClient* c, In* b) {
	int packed = g1neg(b), coords = g1(b);
	push_change(c, NET_CHANGE_DELETE, (coords >> 4 & 7) + c->chunkX, (coords & 7) + c->chunkY, packed >> 2, packed & 3, -1);
	c->locDeletes++;
}

static void note_varp(NetClient* c, int id) {
	c->varpChanges[c->varpVersion % NET_CHANGE_RING] = id;
	c->varpVersion++;
}

// The server tags some messages for the client to act on. Those that need a press come later, with trading.
static void add_game_message(NetClient* c, const char* text) {
	static const struct {
		const char* tag;
		const char* ending;
	} TAGS[] = {{":tradereq:", " wishes to trade with you."}, {":chalreq:", " wishes to duel with you."}, {":assistreq:", " is requesting your assistance."}, {":clan:", ""}, {":trade:", ""}};
	size_t length = strlen(text);
	for (unsigned i = 0; i < sizeof(TAGS) / sizeof(TAGS[0]); i++) {
		size_t tag = strlen(TAGS[i].tag);
		if (length < tag || strcmp(text + length - tag, TAGS[i].tag) != 0) continue;
		char plain[NET_CHAT_TEXT];
		const char* colon = strchr(text, ':');
		int name = (int)(colon - text);
		snprintf(plain, sizeof(plain), "%.*s%s", name, text, TAGS[i].ending);
		if (TAGS[i].ending[0]) {
			char who[16];
			snprintf(who, sizeof(who), "%.*s", name, text);
			if (ignored(c, net_name_key(who))) return;
		}
		if (i == 0) {
			char who[16];
			snprintf(who, sizeof(who), "%.*s", name, text);
			add_chat(c, NET_CHAT_TRADE, who, "wishes to trade with you.");
		} else {
			add_chat(c, NET_CHAT_GAME, "", plain);
		}
		return;
	}
	add_chat(c, NET_CHAT_GAME, "", text);
}

static void read_clan_member(In* b, NetFriend* m) {
	m->key = g8(b);
	m->world = g2(b);
	m->rank = (int8_t)g1(b);
	read_string(b, m->worldName, sizeof(m->worldName));
}

static void handle_packet(NetClient* c, int opcode, const uint8_t* data, int length) {
	c->packets++;
	c->opcodeCounts[opcode]++;
	if (opcode == OP_REBUILD_NORMAL) handle_rebuild(c, data, length);
	else if (opcode == OP_PLAYER_INFO) handle_player_info(c, data, length);
	else if (opcode == OP_NPC_INFO) handle_npc_info(c, data, length);
	else if (opcode == OP_IF_MODEL) {
		In b = {data, (size_t)length, 0, 0};
		int id = ig4(&b);
		ig2add(&b);
		NetPicture* p = picture_slot(c, id);
		p->kind = NET_PICTURE_MODEL;
		p->value = g2add(&b);
		c->ifaceVersion++;
	} else if (opcode == OP_IF_OBJECT) {
		In b = {data, (size_t)length, 0, 0};
		int amount = g4(&b), id = mg4(&b);
		NetPicture* p = picture_slot(c, id);
		p->kind = NET_PICTURE_ITEM;
		p->amount = amount;
		p->value = ig2add(&b);
		c->ifaceVersion++;
	} else if (opcode == OP_IF_ANGLE) {
		In b = {data, (size_t)length, 0, 0};
		int pitch = g2(&b);
		g2add(&b);
		int zoom = ig2add(&b), yaw = ig2add(&b);
		NetPicture* p = picture_slot(c, g4(&b));
		p->zoom = zoom;
		p->pitch = pitch;
		p->yaw = yaw;
		c->ifaceVersion++;
	} else if (opcode == OP_IF_NPC_HEAD) {
		In b = {data, (size_t)length, 0, 0};
		c->headNpc = g2add(&b);
		int low = g2le(&b);
		c->headComponent = (int)((uint32_t)g2le(&b) << 16 | (uint32_t)low);
		c->headKind = c->headNpc == 65535 ? NET_HEAD_NONE : NET_HEAD_NPC;
		c->headVersion++;
	} else if (opcode == OP_IF_PLAYER_HEAD) {
		In b = {data, (size_t)length, 0, 0};
		g2(&b);
		int a = g1(&b), d = g1(&b), e = g1(&b), f = g1(&b);
		c->headComponent = (int)((uint32_t)e << 24 | (uint32_t)f << 16 | (uint32_t)a << 8 | (uint32_t)d);
		c->headKind = NET_HEAD_PLAYER;
		c->headVersion++;
	} else if (opcode == OP_IF_ANIM) {
		In b = {data, (size_t)length, 0, 0};
		int id = mg4(&b);
		int value = g2le(&b);
		c->headAnim = (int16_t)value;
		c->headAnimComponent = id;
	} else if (opcode == OP_RUN_SCRIPT) {
		// Most of what comes this way only arranges the original's window. Prompts need an answer.
		In b = {data, (size_t)length, 0, 0};
		char types[32], text[80] = "";
		g2(&b);
		read_string(&b, types, sizeof(types));
		for (int i = (int)strlen(types) - 1; i >= 0; i--) {
			if (types[i] == 's') read_string(&b, text, sizeof(text));
			else g4(&b);
		}
		int script = g4(&b);
		if (script >= 108 && script <= 110) {
			c->promptKind = script;
			snprintf(c->promptText, sizeof(c->promptText), "%s", text);
			c->promptVersion++;
		}
	} else if (opcode == OP_PRIVATE_IN || opcode == OP_PRIVATE_ECHO || opcode == OP_CLAN_MESSAGE) {
		In b = {data, (size_t)length, 0, 0};
		uint64_t key = g8(&b);
		int rights = 0;
		if (opcode == OP_CLAN_MESSAGE) {
			g1(&b);
			g8(&b);
		}
		if (opcode != OP_PRIVATE_ECHO) {
			b.pos += 5; // the message's id
			rights = g1(&b);
		}
		char who[16], text[NET_CHAT_TEXT];
		read_packed(&b, text, sizeof(text));
		net_name_from_key(key, who, sizeof(who));
		if (opcode == OP_PRIVATE_ECHO || rights > 1 || !ignored(c, key)) add_chat(c, opcode == OP_PRIVATE_IN ? NET_CHAT_FROM : opcode == OP_PRIVATE_ECHO ? NET_CHAT_TO : NET_CHAT_CLAN, who, text);
	} else if (opcode == OP_CLAN_FULL) {
		In b = {data, (size_t)length, 0, 0};
		c->clanOwner = g8(&b);
		c->clanCount = 0;
		c->clanName = 0;
		if (c->clanOwner != 0) {
			c->clanName = g8(&b);
			g1(&b);
			int size = g1(&b);
			if (size != 255) {
				for (int i = 0; i < size && i < NET_MAX_CLAN; i++) read_clan_member(&b, &c->clan[c->clanCount++]);
			}
		}
		c->socialVersion++;
	} else if (opcode == OP_CLAN_UPDATE) {
		In b = {data, (size_t)length, 0, 0};
		uint64_t key = g8(&b);
		int world = g2(&b), rank = (int8_t)g1(&b), leaving = (key >> 63) != 0, at = -1;
		key &= 0x7FFFFFFFFFFFFFFFull;
		for (int i = 0; i < c->clanCount; i++) if (c->clan[i].key == key) at = i;
		if (leaving) {
			if (at >= 0) {
				memmove(&c->clan[at], &c->clan[at + 1], sizeof(NetFriend) * (size_t)(c->clanCount - at - 1));
				c->clanCount--;
			}
		} else {
			if (at < 0 && c->clanCount < NET_MAX_CLAN) at = c->clanCount++;
			if (at >= 0) {
				c->clan[at].key = key;
				c->clan[at].world = world;
				c->clan[at].rank = rank;
				read_string(&b, c->clan[at].worldName, sizeof(c->clan[at].worldName));
			}
		}
		c->socialVersion++;
	} else if (opcode == OP_FRIEND_UPDATE) {
		In b = {data, length, 0, 0};
		uint64_t key = (uint64_t)(uint32_t)g4(&b) << 32;
		key |= (uint32_t)g4(&b);
		key &= 0x7FFFFFFFFFFFFFFFull;
		int world = g2(&b), rank = g1(&b), at = -1;
		for (int i = 0; i < c->friendCount; i++) if (c->friends[i].key == key) at = i;
		if (at < 0 && c->friendCount < NET_MAX_FRIENDS) at = c->friendCount++;
		if (at >= 0) {
			NetFriend* f = &c->friends[at];
			int n = 0;
			f->key = key;
			f->world = world;
			f->rank = rank;
			if (world > 0) {
				while (b.pos < length && data[b.pos] != 0) {
					if (n < (int)sizeof(f->worldName) - 1) f->worldName[n++] = (char)data[b.pos];
					b.pos++;
				}
			}
			f->worldName[n] = 0;
		}
		c->socialVersion++;
	} else if (opcode == OP_IGNORE_LIST) {
		In b = {data, length, 0, 0};
		c->ignoreCount = length / 8 < NET_MAX_IGNORES ? length / 8 : NET_MAX_IGNORES;
		for (int i = 0; i < c->ignoreCount; i++) {
			uint64_t key = (uint64_t)(uint32_t)g4(&b) << 32;
			c->ignores[i] = key | (uint32_t)g4(&b);
		}
		c->socialVersion++;
	} else if (opcode == OP_FRIENDS_LOADED && length > 0) {
		c->friendState = data[0];
		c->socialVersion++;
	} else if (opcode == OP_VARP_SMALL || opcode == OP_VARP_LARGE) {
		In b = {data, length, 0, 0};
		int id, value;
		if (opcode == OP_VARP_SMALL) {
			id = g2add(&b);
			value = (int8_t)g1neg(&b);
		} else {
			value = g4(&b);
			id = g2add(&b);
		}
		if (id < NET_MAX_VARPS) c->varps[id] = value;
		note_varp(c, id);
	} else if (opcode == OP_VARBIT_SMALL || opcode == OP_VARBIT_LARGE) {
		In b = {data, length, 0, 0};
		int id, value;
		if (opcode == OP_VARBIT_SMALL) {
			value = g1add(&b);
			id = g2le(&b);
		} else {
			int low = g2le(&b);
			value = low | g2le(&b) << 16;
			id = g2le_add(&b);
		}
		note_varp(c, c->onVarbit ? c->onVarbit(id, value) : -1);
	} else if (opcode == OP_VARP_RESET) {
		memset(c->varps, 0, sizeof(c->varps));
		note_varp(c, -1);
	} else if (opcode == OP_STAT) {
		In b = {data, length, 0, 0};
		int level = g1add(&b), first = g1(&b), second = g1(&b), third = g1(&b), fourth = g1(&b), skill = g1(&b);
		if (skill >= 0 && skill < 32) {
			c->skillLevel[skill] = level;
			c->skillXp[skill] = (int)((uint32_t)first << 8 | (uint32_t)second | (uint32_t)third << 24 | (uint32_t)fourth << 16);
			c->statChanges[c->statVersion % NET_CHANGE_RING] = skill;
			c->statVersion++;
		}
	} else if (is_ground_packet(opcode)) {
		In b = {data, length, 0, 0};
		read_ground(c, opcode, &b);
	} else if (opcode == OP_INV_FULL || opcode == OP_INV_PARTIAL) {
		In b = {data, length, 0, 0};
		int hash = (int)((uint32_t)g2(&b) << 16);
		hash |= g2(&b);
		int id = g2(&b);
		if (hash < -70000) id += 32768;
		NetInv* inv = inventory_for(c, id);
		if (inv) {
			inv->hash = hash;
			if (opcode == OP_INV_FULL) {
				int total = g2(&b);
				inv->count = 0;
				for (int slot = 0; slot < total; slot++) {
					int amount = g1sub(&b);
					if (amount == 255) amount = g4(&b);
					int item = g2(&b);
					set_inventory_slot(inv, slot, item - 1, amount);
				}
			} else {
				while (b.pos < length) {
					int slot = b.data[b.pos] >= 128 ? g2(&b) - 0x8000 : g1(&b);
					int item = g2(&b), amount = 0;
					if (item != 0) {
						amount = g1(&b);
						if (amount == 255) amount = g4(&b);
					}
					set_inventory_slot(inv, slot, item - 1, amount);
				}
			}
			c->invVersion++;
		}
	} else if (opcode == OP_INV_CLEAR) {
		In b = {data, length, 0, 0};
		int hash = mg4(&b);
		for (int i = 0; i < c->invCount; i++) {
			NetInv* inv = &c->invs[i];
			if (inv->hash != hash) continue;
			for (int k = 0; k < inv->count; k++) {
				inv->items[k] = -1;
				inv->amounts[k] = 0;
			}
		}
		c->invVersion++;
	} else if (opcode == OP_IF_OPEN) {
		In b = {data, length, 0, 0};
		g1(&b);
		int parent = mg4(&b);
		g2add(&b);
		int id = g2(&b), at = -1;
		for (int i = 0; i < c->subCount; i++) if (c->subs[i].parent == parent) at = i;
		if (at < 0 && c->subCount < NET_MAX_SUBS) at = c->subCount++;
		if (at >= 0) c->subs[at] = (NetSub){parent, id};
		c->ifaceVersion++;
	} else if (opcode == OP_IF_CLOSE) {
		In b = {data, length, 0, 0};
		g2(&b);
		int parent = (int)((uint32_t)g2(&b) << 16);
		parent |= g2(&b);
		for (int i = 0; i < c->subCount; i++) {
			if (c->subs[i].parent != parent) continue;
			c->subs[i] = c->subs[--c->subCount];
			break;
		}
		c->ifaceVersion++;
	} else if (opcode == OP_IF_TEXT) {
		In b = {data, length, 0, 0};
		int id = mg4(&b), at = -1, n = 0;
		for (int i = 0; i < c->textCount; i++) if (c->texts[i].id == id) at = i;
		if (at < 0) {
			at = c->textCount < NET_MAX_TEXTS ? c->textCount++ : c->textNext;
			c->textNext = (at + 1) % NET_MAX_TEXTS;
		}
		c->texts[at].id = id;
		while (b.pos < length && data[b.pos] != 0) {
			if (n < NET_TEXT_SIZE - 1) c->texts[at].text[n++] = (char)data[b.pos];
			b.pos++;
		}
		c->texts[at].text[n] = 0;
		c->ifaceVersion++;
	} else if (opcode == OP_IF_HIDE) {
		In b = {data, length, 0, 0};
		int hidden = g1neg(&b);
		g2(&b);
		uint32_t id = (uint32_t)g1(&b);
		id |= (uint32_t)g1(&b) << 8;
		id |= (uint32_t)g1(&b) << 16;
		id |= (uint32_t)g1(&b) << 24;
		int at = -1;
		for (int i = 0; i < c->hideCount; i++) if (c->hides[i].id == (int)id) at = i;
		if (at < 0) {
			at = c->hideCount < NET_MAX_HIDES ? c->hideCount++ : c->hideNext;
			c->hideNext = (at + 1) % NET_MAX_HIDES;
		}
		c->hides[at] = (NetHide){(int)id, hidden};
		c->ifaceVersion++;
	} else if (opcode == OP_PLAYER_OPTION) {
		In b = {data, length, 0, 0};
		g2le_add(&b);
		int top = g1(&b), slot = g1(&b) - 1;
		if (slot >= 0 && slot < NET_PLAYER_OPTIONS) {
			int n = length - b.pos;
			while (n > 0 && data[b.pos + n - 1] == 0) n--;
			if (n > (int)sizeof(c->playerOptions[0]) - 1) n = (int)sizeof(c->playerOptions[0]) - 1;
			memcpy(c->playerOptions[slot], data + b.pos, (size_t)n);
			c->playerOptions[slot][n] = 0;
			if (strcmp(c->playerOptions[slot], "null") == 0) c->playerOptions[slot][0] = 0;
			c->playerOptionLow[slot] = top == 0;
		}
	}
	else if (opcode == OP_ZONE_FOLLOWS) {
		In b = {data, length, 0, 0};
		c->chunkX = g1neg(&b);
		c->chunkY = g1(&b);
	} else if (opcode == OP_ZONE_RESET) {
		In b = {data, length, 0, 0};
		c->chunkX = g1(&b);
		c->chunkY = g1neg(&b);
		push_change(c, NET_CHANGE_RESET, c->chunkX, c->chunkY, 0, 0, -1);
		for (int i = c->groundCount - 1; i >= 0; i--) {
			NetGround* g = &c->ground[i];
			if (g->plane != c->plane || g->x < c->chunkX || g->x >= c->chunkX + 8 || g->y < c->chunkY || g->y >= c->chunkY + 8) continue;
			memmove(g, g + 1, sizeof(*g) * (size_t)(c->groundCount - i - 1));
			c->groundCount--;
		}
		c->groundVersion++;
	} else if (opcode == OP_LOC_ADD) {
		In b = {data, length, 0, 0};
		read_loc_add(c, &b);
	} else if (opcode == OP_LOC_DELETE) {
		In b = {data, length, 0, 0};
		read_loc_delete(c, &b);
	} else if (opcode == OP_ZONE_ENCLOSED) {
		In b = {data, length, 0, 0};
		c->chunkY = g1add(&b);
		c->chunkX = g1sub(&b);
		while (b.pos < length) {
			int sub = g1(&b);
			if (sub == OP_LOC_ADD) read_loc_add(c, &b);
			else if (sub == OP_LOC_DELETE) read_loc_delete(c, &b);
			else if (is_ground_packet(sub)) read_ground(c, sub, &b);
			else if (PACKET_LENGTHS[sub] >= 0) b.pos += PACKET_LENGTHS[sub];
			else break;
		}
	} else if (opcode == OP_ROOT_INTERFACE && length >= 2) {
		In b = {data, length, 0, 0};
		c->rootInterface = g2le_add(&b);
		c->inLobby = c->rootInterface == LOBBY_ROOT;
	} else if (opcode == OP_MESSAGE && length > 0) {
		int n = length < (int)sizeof(c->lastMessage) - 1 ? length : (int)sizeof(c->lastMessage) - 1;
		memcpy(c->lastMessage, data, (size_t)n);
		c->lastMessage[n] = 0;
		c->messages++;
		if (c->onMessage) c->onMessage(c->lastMessage);
		add_game_message(c, c->lastMessage);
	} else if (opcode == OP_LOGOUT) {
		c->connected = 0;
		c->loggedOut = 1;
		snprintf(c->error, sizeof(c->error), "the server logged us out");
	}
}

static void parse_packets(NetClient* c) {
	size_t pos = 0;
	while (pos < c->inLength) {
		int opcode = c->in[pos];
		int length = PACKET_LENGTHS[opcode];
		size_t header = 1;
		if (length == -1) {
			if (pos + 2 > c->inLength) break;
			length = c->in[pos + 1];
			header = 2;
		} else if (length == -2) {
			if (pos + 3 > c->inLength) break;
			length = c->in[pos + 1] << 8 | c->in[pos + 2];
			header = 3;
		}
		if (pos + header + (size_t)length > c->inLength) break;
		handle_packet(c, opcode, c->in + pos + header, length);
		pos += header + (size_t)length;
	}
	if (pos > 0) {
		memmove(c->in, c->in + pos, c->inLength - pos);
		c->inLength -= pos;
	}
}

// ---- public API ----

int net_login(NetClient* c, const char* host, int port, const char* username, const char* password) {
	memset(c, 0, sizeof(*c));
	start_entity(&c->self, -1);
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((uint16_t)port);
	address.sin_addr.s_addr = inet_addr(host);
	if (address.sin_addr.s_addr == INADDR_NONE) {
		struct hostent* h = gethostbyname(host);
		if (!h) {
			snprintf(c->error, sizeof(c->error), "cannot find server %s", host);
			return -1;
		}
		memcpy(&address.sin_addr, h->h_addr_list[0], 4);
	}
	c->sock = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (c->sock < 0 || connect_timed(c->sock, &address, 8000) != 0) {
		snprintf(c->error, sizeof(c->error), "no answer from %s:%d", host, port);
		return -2;
	}
	c->connected = 1;

	uint64_t name = encode37(username);
	c->selfKey = name;
	uint8_t buffer[512];
	Out o = {buffer, 0};
	p1(&o, 14);
	p1(&o, (int)(name >> 16 & 0x1F));
	uint8_t response[14];
	if (send_all(c, buffer, o.size) != 0 || recv_exact(c, response, 9, 10000) != 0) {
		snprintf(c->error, sizeof(c->error), "no handshake reply");
		return -3;
	}
	if (response[0] != 0) {
		c->reply = response[0];
		snprintf(c->error, sizeof(c->error), "handshake refused: %d", response[0]);
		return -4;
	}
	uint32_t seed[4] = {(uint32_t)(rand() % 99999999), (uint32_t)(rand() % 99999999), 0, 0};
	seed[2] = (uint32_t)response[1] << 24 | response[2] << 16 | response[3] << 8 | response[4];
	seed[3] = (uint32_t)response[5] << 24 | response[6] << 16 | response[7] << 8 | response[8];

	uint8_t plain[160], secret[130];
	Out s = {plain, 0};
	p1(&s, 10);
	for (int i = 0; i < 4; i++) p4(&s, seed[i]);
	p8(&s, name);
	pstr(&s, password);
	pstr(&s, "");
	pstr(&s, "");
	pstr(&s, "");
	if (s.size > 127) {
		snprintf(c->error, sizeof(c->error), "password too long");
		return -5;
	}
	int secretSize = net_rsa_encrypt(plain, s.size, secret);

	Out l = {buffer, 0};
	p1(&l, 16);
	p2(&l, 1 + secretSize + 1 + 163);
	p4(&l, 530);
	p1(&l, 0);  // login type
	p1(&l, 0);  // adverts
	p1(&l, 1);
	p1(&l, 0);  // window mode
	p2(&l, 765);
	p2(&l, 503);
	p1(&l, 0);  // anti-aliasing
	for (int i = 0; i < 24; i++) p1(&l, 0); // uid
	p1(&l, 0);  // settings string
	p4(&l, 0);  // affiliate
	p4(&l, 0);  // preferences
	p2(&l, 0);  // verify id
	for (int i = 0; i < 29; i++) p4(&l, 0); // archive checksums, which the server ignores
	p1(&l, secretSize);
	memcpy(buffer + l.size, secret, (size_t)secretSize);
	l.size += secretSize;
	if (send_all(c, buffer, l.size) != 0) {
		snprintf(c->error, sizeof(c->error), "login send failed");
		return -6;
	}
	if (recv_exact(c, response, 1, 15000) != 0) {
		snprintf(c->error, sizeof(c->error), "no login reply");
		return -7;
	}
	c->reply = response[0];
	if (response[0] != 2) {
		snprintf(c->error, sizeof(c->error), "login refused: reply %d", response[0]);
		return -8;
	}
	if (recv_exact(c, response, 14, 15000) != 0) {
		snprintf(c->error, sizeof(c->error), "login details missing");
		return -9;
	}
	c->rights = response[0];
	c->selfId = response[7] << 8 | response[8];
	c->playerMember = response[9];
	c->members = response[10];
	int length = response[12] << 8 | response[13];
	if (length > NET_BUFFER_SIZE || recv_exact(c, c->in, length, 15000) != 0) {
		snprintf(c->error, sizeof(c->error), "first map packet missing");
		return -10;
	}
	handle_packet(c, response[11], c->in, length);
	set_nonblocking(c->sock);
	return 0;
}

int net_poll(NetClient* c, long nowMs) {
	if (!c->connected) return -1;
	for (;;) {
		size_t room = NET_BUFFER_SIZE - c->inLength;
		if (room == 0) {
			c->connected = 0;
			snprintf(c->error, sizeof(c->error), "packet too large");
			return -1;
		}
		int n = (int)recv(c->sock, (char*)c->in + c->inLength, room, 0);
		if (n > 0) {
			c->inLength += (size_t)n;
			c->bytes += n;
			parse_packets(c);
			continue;
		}
		if (n < 0 && NET_WOULD_BLOCK) break;
		c->connected = 0;
		if (!c->error[0]) snprintf(c->error, sizeof(c->error), "connection closed");
		return -1;
	}
	// Unsigned, because the 3DS clock does not fit in a 32-bit long and can read as negative.
	if ((uint32_t)nowMs - (uint32_t)c->lastPingMs >= 1000) {
		uint8_t ping = OP_NO_TIMEOUT;
		c->lastPingMs = nowMs;
		send_all(c, &ping, 1);
	}
	return c->connected ? 0 : -1;
}

void net_close(NetClient* c) {
	if (c->sock > 0) NET_CLOSE(c->sock);
	c->sock = 0;
	c->connected = 0;
}

void net_walk(NetClient* c, int worldX, int worldY, int run) {
	uint8_t data[8];
	Out o = {data, 0};
	p1(&o, OP_WALK);
	p1(&o, 5);
	p1(&o, (run ? 1 : 0) + 128);
	p2(&o, worldX);
	p1(&o, worldY >> 8);
	p1(&o, (worldY + 128) & 0xFF);
	send_all(c, data, o.size);
	c->walksSent++;
}

static void ple(Out* o, int v) { p1(o, v); p1(o, v >> 8); }
static void pa(Out* o, int v) { p1(o, v >> 8); p1(o, v + 128); }
static void plea(Out* o, int v) { p1(o, v + 128); p1(o, v >> 8); }

void net_loc_action(NetClient* c, int option, int id, int worldX, int worldY) {
	uint8_t data[8];
	Out o = {data, 0};
	if (option == 0) {
		p1(&o, 254);
		ple(&o, worldX);
		pa(&o, id);
		p2(&o, worldY);
	} else if (option == 1) {
		p1(&o, 194);
		plea(&o, worldY);
		ple(&o, worldX);
		p2(&o, id);
	} else if (option == 2) {
		p1(&o, 84);
		plea(&o, id);
		plea(&o, worldY);
		ple(&o, worldX);
	} else if (option == 3) {
		p1(&o, 247);
		ple(&o, worldY);
		plea(&o, worldX);
		p2(&o, id);
	} else {
		p1(&o, 170);
		plea(&o, id);
		plea(&o, worldX);
		plea(&o, worldY);
	}
	send_all(c, data, o.size);
	c->objectActionsSent++;
}

void net_loc_examine(NetClient* c, int id) {
	uint8_t data[4];
	Out o = {data, 0};
	p1(&o, 94);
	plea(&o, id);
	send_all(c, data, o.size);
}

void net_npc_action(NetClient* c, int option, int index) {
	uint8_t data[4];
	Out o = {data, 0};
	if (option == 0) {
		p1(&o, 78);
		ple(&o, index);
	} else if (option == 1) {
		p1(&o, 3);
		plea(&o, index);
	} else if (option == 2) {
		p1(&o, 148);
		pa(&o, index);
	} else if (option == 3) {
		p1(&o, 30);
		p2(&o, index);
	} else {
		p1(&o, 218);
		ple(&o, index);
	}
	send_all(c, data, o.size);
	c->npcActionsSent++;
}

int net_player_action(NetClient* c, int option, int index) {
	static const uint8_t OPCODES[NET_PLAYER_OPTIONS] = {68, 0, 71, 180, 0, 0, 114, 175};
	if (option < 0 || option >= NET_PLAYER_OPTIONS || OPCODES[option] == 0) return 0;
	uint8_t data[4];
	Out o = {data, 0};
	p1(&o, OPCODES[option]);
	if (option == 7) pa(&o, index);
	else plea(&o, index);
	send_all(c, data, o.size);
	c->playerActionsSent++;
	return 1;
}

void net_npc_examine(NetClient* c, int type) {
	uint8_t data[4];
	Out o = {data, 0};
	p1(&o, 72);
	p2(&o, type);
	send_all(c, data, o.size);
}

// The byte orders the client uses for four-byte values.
static void p4le(Out* o, uint32_t v) { p1(o, (int)v); p1(o, (int)(v >> 8)); p1(o, (int)(v >> 16)); p1(o, (int)(v >> 24)); }
static void p4a(Out* o, uint32_t v) { p1(o, (int)(v >> 8)); p1(o, (int)v); p1(o, (int)(v >> 24)); p1(o, (int)(v >> 16)); }
static void p4b(Out* o, uint32_t v) { p1(o, (int)(v >> 16)); p1(o, (int)(v >> 24)); p1(o, (int)v); p1(o, (int)(v >> 8)); }

const NetInv* net_inventory(const NetClient* c, int id) {
	for (int i = 0; i < c->invCount; i++) if (c->invs[i].id == id) return &c->invs[i];
	return NULL;
}

void net_item_action(NetClient* c, int option, int component, int item, int slot) {
	uint8_t data[12];
	Out o = {data, 0};
	uint32_t hash = (uint32_t)component;
	if (option == 0) {
		p1(&o, 156);
		plea(&o, slot);
		pa(&o, item);
		p4le(&o, hash);
	} else if (option == 1) {
		p1(&o, 55);
		ple(&o, item);
		pa(&o, slot);
		p4a(&o, hash);
	} else if (option == 2) {
		p1(&o, 153);
		p4le(&o, hash);
		ple(&o, slot);
		ple(&o, item);
	} else if (option == 3) {
		p1(&o, 161);
		p4le(&o, hash);
		plea(&o, item);
		plea(&o, slot);
	} else {
		p1(&o, 135);
		pa(&o, item);
		pa(&o, slot);
		p4b(&o, hash);
	}
	send_all(c, data, o.size);
	c->itemActionsSent++;
}

void net_ground_action(NetClient* c, int option, int item, int worldX, int worldY) {
	uint8_t data[8];
	Out o = {data, 0};
	if (option == 2) {
		p1(&o, 66);
		ple(&o, worldX);
		p2(&o, item);
		plea(&o, worldY);
	} else if (option == 3) {
		p1(&o, 33);
		p2(&o, item);
		p2(&o, worldX);
		ple(&o, worldY);
	} else {
		return;
	}
	send_all(c, data, o.size);
	c->groundActionsSent++;
}

void net_use_item_on_ground(NetClient* c, int component, int slot, int item, int groundItem, int worldX, int worldY) {
	uint8_t data[16];
	Out o = {data, 0};
	p1(&o, 101);
	plea(&o, worldX);
	ple(&o, slot);
	ple(&o, item);
	ple(&o, groundItem);
	plea(&o, worldY);
	p4b(&o, (uint32_t)component);
	send_all(c, data, o.size);
	c->itemActionsSent++;
}

void net_move_item(NetClient* c, int component, int fromSlot, int toSlot) {
	uint8_t data[10];
	Out o = {data, 0};
	// The server treats the first slot as the item being moved and ignores the packet if that slot is empty,
	// so the item's own slot goes first. Otherwise a move into an empty slot would do nothing.
	p1(&o, 231);
	p2(&o, fromSlot);
	p4le(&o, (uint32_t)component);
	pa(&o, toSlot);
	p1(&o, 128); // not inserting: the two items swap places
	send_all(c, data, o.size);
	c->itemMovesSent++;
}

void net_worn_action(NetClient* c, int option, int item, int slot) {
	uint8_t data[12];
	Out o = {data, 0};
	if (option == 0) {
		p1(&o, 81);
		pa(&o, slot);
		p2(&o, item);
		p2(&o, 28);
		p2(&o, 387);
	} else {
		p1(&o, 206);
		pa(&o, item);
		ple(&o, slot);
		p4le(&o, 387u << 16 | 28u);
	}
	send_all(c, data, o.size);
	c->itemActionsSent++;
}

void net_item_examine(NetClient* c, int item) {
	uint8_t data[4];
	Out o = {data, 0};
	p1(&o, 92);
	plea(&o, item);
	send_all(c, data, o.size);
}

void net_use_item_on_item(NetClient* c, int component, int usedSlot, int usedItem, int slot, int item) {
	uint8_t data[20];
	Out o = {data, 0};
	p1(&o, 27);
	p2(&o, usedSlot);
	p4le(&o, (uint32_t)component);
	ple(&o, slot);
	p4le(&o, (uint32_t)component);
	plea(&o, usedItem);
	plea(&o, item);
	send_all(c, data, o.size);
	c->itemActionsSent++;
}

void net_use_item_on_npc(NetClient* c, int component, int slot, int item, int npc) {
	uint8_t data[12];
	Out o = {data, 0};
	p1(&o, 115);
	p4b(&o, (uint32_t)component);
	ple(&o, slot);
	ple(&o, npc);
	plea(&o, item);
	send_all(c, data, o.size);
	c->itemActionsSent++;
}

void net_use_item_on_player(NetClient* c, int component, int slot, int item, int player) {
	uint8_t data[12];
	Out o = {data, 0};
	p1(&o, 248);
	plea(&o, player);
	p2(&o, item);
	p2(&o, slot);
	p4b(&o, (uint32_t)component);
	send_all(c, data, o.size);
	c->itemActionsSent++;
}

void net_use_item_on_loc(NetClient* c, int component, int slot, int item, int id, int worldX, int worldY) {
	uint8_t data[16];
	Out o = {data, 0};
	p1(&o, 134);
	pa(&o, worldX);
	p2(&o, item);
	ple(&o, worldY);
	p2(&o, slot);
	p4b(&o, (uint32_t)component);
	pa(&o, id);
	send_all(c, data, o.size);
	c->itemActionsSent++;
}

void net_continue(NetClient* c, int component) {
	uint8_t data[7];
	Out o = {data, 0};
	uint32_t id = (uint32_t)component;
	p1(&o, OP_CONTINUE);
	p1(&o, (int)(id >> 8));
	p1(&o, (int)id);
	p1(&o, (int)(id >> 24));
	p1(&o, (int)(id >> 16));
	ple(&o, 65535);
	send_all(c, data, o.size);
	c->continuesSent++;
}

// A spell is not a component made by a script, so its slot is always this.
#define SPELL_SLOT 65535

void net_spell_on_npc(NetClient* c, int spell, int index) {
	uint8_t data[9];
	Out o = {data, 0};
	p1(&o, 239);
	p4le(&o, (uint32_t)spell);
	pa(&o, SPELL_SLOT);
	plea(&o, index);
	send_all(c, data, o.size);
	c->spellsSent++;
}

void net_spell_on_player(NetClient* c, int spell, int index) {
	uint8_t data[9];
	Out o = {data, 0};
	p1(&o, 195);
	pa(&o, SPELL_SLOT);
	p4le(&o, (uint32_t)spell);
	plea(&o, index);
	send_all(c, data, o.size);
	c->spellsSent++;
}

void net_spell_on_item(NetClient* c, int spell, int component, int slot, int item) {
	uint8_t data[15];
	Out o = {data, 0};
	p1(&o, 253);
	p4le(&o, (uint32_t)spell);
	plea(&o, slot);
	p4le(&o, (uint32_t)component);
	pa(&o, item);
	ple(&o, SPELL_SLOT);
	send_all(c, data, o.size);
	c->spellsSent++;
}

void net_spell_on_ground(NetClient* c, int spell, int item, int worldX, int worldY) {
	uint8_t data[13];
	Out o = {data, 0};
	p1(&o, 73);
	p4a(&o, (uint32_t)spell);
	p2(&o, worldY);
	plea(&o, item);
	plea(&o, worldX);
	ple(&o, SPELL_SLOT);
	send_all(c, data, o.size);
	c->spellsSent++;
}

void net_spell_on_loc(NetClient* c, int spell, int id, int worldX, int worldY) {
	uint8_t data[15];
	Out o = {data, 0};
	p1(&o, 233);
	plea(&o, worldY);
	pa(&o, worldX);
	plea(&o, SPELL_SLOT);
	p4a(&o, (uint32_t)spell);
	pa(&o, id);
	send_all(c, data, o.size);
	c->spellsSent++;
}

uint64_t net_name_key(const char* name) {
	return encode37(name);
}

void net_name_from_key(uint64_t key, char* out, size_t size) {
	static const char LETTERS[] = "_abcdefghijklmnopqrstuvwxyz0123456789";
	char reversed[16];
	int n = 0;
	while (key != 0 && n < 15) {
		reversed[n++] = LETTERS[key % 37];
		key /= 37;
	}
	size_t at = 0;
	int capital = 1;
	while (n > 0 && at + 1 < size) {
		char letter = reversed[--n];
		if (letter == '_') {
			out[at++] = ' ';
			capital = 1;
			continue;
		}
		out[at++] = capital && letter >= 'a' && letter <= 'z' ? (char)(letter - 32) : letter;
		capital = 0;
	}
	out[at] = 0;
}

static void send_name(NetClient* c, int opcode, uint64_t key) {
	uint8_t data[9];
	Out o = {data, 0};
	p1(&o, opcode);
	p8(&o, key);
	send_all(c, data, o.size);
	c->socialSent++;
	c->socialVersion++;
}

static int name_refused(const NetClient* c, uint64_t key, int friendList) {
	if (key == 0) return 4;
	for (int i = 0; i < c->friendCount; i++) if (c->friends[i].key == key) return friendList ? 2 : 3;
	for (int i = 0; i < c->ignoreCount; i++) if (c->ignores[i] == key) return friendList ? 3 : 2;
	return key == c->selfKey ? 4 : 0;
}

int net_add_friend(NetClient* c, uint64_t key) {
	if (c->friendCount >= (c->playerMember ? NET_MAX_FRIENDS : 100)) return 1;
	int refused = name_refused(c, key, 1);
	if (refused) return refused;
	NetFriend* f = &c->friends[c->friendCount++];
	memset(f, 0, sizeof(*f));
	f->key = key;
	send_name(c, OP_FRIEND_ADD, key);
	return 0;
}

int net_add_ignore(NetClient* c, uint64_t key) {
	if (c->ignoreCount >= NET_MAX_IGNORES) return 1;
	int refused = name_refused(c, key, 0);
	if (refused) return refused;
	c->ignores[c->ignoreCount++] = key;
	send_name(c, OP_IGNORE_ADD, key);
	return 0;
}

void net_del_friend(NetClient* c, uint64_t key) {
	for (int i = 0; i < c->friendCount; i++) {
		if (c->friends[i].key != key) continue;
		memmove(&c->friends[i], &c->friends[i + 1], sizeof(NetFriend) * (size_t)(c->friendCount - i - 1));
		c->friendCount--;
		send_name(c, OP_FRIEND_DEL, key);
		return;
	}
}

void net_del_ignore(NetClient* c, uint64_t key) {
	for (int i = 0; i < c->ignoreCount; i++) {
		if (c->ignores[i] != key) continue;
		memmove(&c->ignores[i], &c->ignores[i + 1], sizeof(uint64_t) * (size_t)(c->ignoreCount - i - 1));
		c->ignoreCount--;
		send_name(c, OP_IGNORE_DEL, key);
		return;
	}
}

// The packed message: its length, then the text.
static void put_packed(Out* o, const char* text) {
	static uint8_t packed[MAX_CHAT_BYTES];
	int length = (int)strlen(text);
	if (length > 80) length = 80;
	if (length < 128) p1(o, length);
	else p2(o, length + 32768);
	int size = huffman_encode((const uint8_t*)text, length, packed, MAX_CHAT_BYTES);
	for (int i = 0; i < size; i++) p1(o, packed[i]);
}

void net_chat_public(NetClient* c, const char* text) {
	uint8_t data[MAX_CHAT_BYTES + 16];
	Out o = {data, 0};
	p1(&o, OP_PUBLIC_CHAT);
	p1(&o, 0);
	p1(&o, 0); // no colour
	p1(&o, 0); // no effect
	put_packed(&o, text);
	data[1] = (uint8_t)(o.size - 2);
	send_all(c, data, o.size);
	c->chatSent++;
}

void net_chat_private(NetClient* c, uint64_t key, const char* text) {
	uint8_t data[MAX_CHAT_BYTES + 16];
	Out o = {data, 0};
	p1(&o, OP_PRIVATE_CHAT);
	p1(&o, 0);
	p8(&o, key);
	put_packed(&o, text);
	data[1] = (uint8_t)(o.size - 2);
	send_all(c, data, o.size);
	c->chatSent++;
}

void net_clan_join(NetClient* c, uint64_t owner) {
	uint8_t data[9];
	Out o = {data, 0};
	p1(&o, OP_CLAN_JOIN);
	p8(&o, owner);
	send_all(c, data, o.size);
	c->chatSent++;
}

void net_if_option(NetClient* c, int opcode, int interfaceId, int button, int slot) {
	uint8_t data[7];
	Out o = {data, 0};
	p1(&o, opcode);
	p4(&o, (uint32_t)interfaceId << 16 | (uint32_t)button);
	p2(&o, slot);
	send_all(c, data, o.size);
	c->buttonsSent++;
}

void net_answer_number(NetClient* c, int value) {
	uint8_t data[5];
	Out o = {data, 0};
	p1(&o, OP_ANSWER_NUMBER);
	p4(&o, (uint32_t)value);
	send_all(c, data, o.size);
}

void net_answer_name(NetClient* c, uint64_t key) {
	uint8_t data[9];
	Out o = {data, 0};
	p1(&o, OP_ANSWER_NAME);
	p8(&o, key);
	send_all(c, data, o.size);
}

void net_answer_text(NetClient* c, const char* text) {
	uint8_t data[100];
	Out o = {data, 0};
	p1(&o, OP_ANSWER_TEXT);
	p1(&o, (int)strlen(text) + 1);
	pstr(&o, text);
	send_all(c, data, o.size);
}

void net_close_modal(NetClient* c) {
	uint8_t data[1] = {OP_CLOSE_MODAL};
	send_all(c, data, 1);
	c->buttonsSent++;
}

int net_interface_is_open(const NetClient* c, int id) {
	for (int i = 0; i < c->subCount; i++) if (c->subs[i].id == id) return 1;
	return 0;
}

int net_open_interface(const NetClient* c, int parent) {
	for (int i = 0; i < c->subCount; i++) if (c->subs[i].parent == parent) return c->subs[i].id;
	return -1;
}

const char* net_component_text(const NetClient* c, int component) {
	for (int i = 0; i < c->textCount; i++) if (c->texts[i].id == component) return c->texts[i].text;
	return NULL;
}

int net_component_hidden(const NetClient* c, int component) {
	for (int i = 0; i < c->hideCount; i++) if (c->hides[i].id == component) return c->hides[i].hidden;
	return -1;
}

void net_button_slot(NetClient* c, int interfaceId, int button, int slot) {
	uint8_t data[7];
	Out o = {data, 0};
	p1(&o, OP_BUTTON);
	p4(&o, (uint32_t)interfaceId << 16 | (uint32_t)button);
	p2(&o, slot);
	send_all(c, data, o.size);
	c->buttonsSent++;
}

const NetPicture* net_component_picture(const NetClient* c, int component) {
	for (int i = 0; i < c->pictureCount; i++) if (c->pictures[i].id == component && c->pictures[i].kind) return &c->pictures[i];
	return NULL;
}

void net_button(NetClient* c, int interfaceId, int button) {
	net_button_slot(c, interfaceId, button, 65535); // not a component made by a script
}

void net_button_second(NetClient* c, int interfaceId, int button) {
	uint8_t data[7];
	Out o = {data, 0};
	p1(&o, OP_BUTTON_SECOND);
	p4(&o, (uint32_t)interfaceId << 16 | (uint32_t)button);
	p2(&o, 65535);
	send_all(c, data, o.size);
	c->buttonsSent++;
}

void net_if_button(NetClient* c, int interfaceId, int button) {
	uint8_t data[5];
	Out o = {data, 0};
	p1(&o, OP_IF_BUTTON);
	p4(&o, (uint32_t)interfaceId << 16 | (uint32_t)button);
	send_all(c, data, o.size);
	c->buttonsSent++;
}

void net_play(NetClient* c) {
	net_button(c, 378, 140);
	c->inLobby = 0;
}

void net_command(NetClient* c, const char* command) {
	uint8_t data[300];
	Out o = {data, 0};
	p1(&o, OP_CHEAT);
	p1(&o, (int)strlen(command) + 1);
	pstr(&o, command);
	send_all(c, data, o.size);
}

int net_world_x(const NetClient* c) {
	return (c->zoneX - 6) * 8 + c->localX;
}

int net_world_y(const NetClient* c) {
	return (c->zoneY - 6) * 8 + c->localY;
}
