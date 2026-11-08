// Game connection: login, packet framing, player position and walking.
// Plain BSD sockets, so it runs on PC and 3DS. The caller starts the socket service.
#ifndef NETCLIENT_H
#define NETCLIENT_H

#include <stddef.h>
#include <stdint.h>

#define NET_MAX_REGIONS 32
#define NET_BUFFER_SIZE 65536

enum { NET_HEAD_NONE, NET_HEAD_NPC, NET_HEAD_PLAYER };
enum { NET_CHANGE_DELETE, NET_CHANGE_ADD, NET_CHANGE_RESET };
#define NET_MAX_CHANGES 256

// An object change from the server. For a reset, x and y are the corner of the chunk.
typedef struct {
	uint8_t kind, x, y, plane, shape, rotation;
	int id;
} NetLocChange;

#define NET_MAX_NPCS 256
#define NET_NO_SEQ (-2)
#define NET_MAX_PLAYERS 256
#define NET_SELF_ID 2047 // where the local player's look is kept
#define NET_PLAYER_OPTIONS 8
#define NET_MAX_SUBS 64
#define NET_MAX_TEXTS 512 // a quest journal alone sets 311
#define NET_MAX_VARPS 4000
#define NET_MAX_FRIENDS 200
#define NET_MAX_CLAN 100
#define NET_CHAT_LINES 100
#define NET_CHAT_TEXT 104

enum { NET_CHAT_GAME, NET_CHAT_PUBLIC, NET_CHAT_FROM, NET_CHAT_TO, NET_CHAT_CLAN, NET_CHAT_TRADE };
#define NET_MAX_IGNORES 100
#define NET_CHANGE_RING 32 // recent variables and skills that changed, as the client keeps them
#define NET_MAX_HIDES 128
#define NET_TEXT_SIZE 200
#define NET_MAX_GROUND 512
#define NET_MAX_INVS 8
#define NET_INV_SIZE 512

// An item lying on a scene tile.
typedef struct {
	uint8_t x, y, plane;
	int id, amount;
} NetGround;

// A container of items, such as the bag. An empty slot has item -1.
typedef struct {
	int id, hash, count;
	int32_t items[NET_INV_SIZE], amounts[NET_INV_SIZE];
} NetInv;

// A model or item the server has put on a component. These stay until replaced, because the server often sends
// them just before it opens the interface, so opening one must not clear them.
enum { NET_PICTURE_MODEL = 1, NET_PICTURE_ITEM = 2 };
#define NET_MAX_PICTURES 64
typedef struct {
	int id, kind, value, amount;
	int zoom, pitch, yaw;
} NetPicture;

// An interface the server has opened inside another. parent is interface << 16 | component.
typedef struct {
	int parent, id;
} NetSub;

typedef struct {
	int id;
	char text[NET_TEXT_SIZE];
} NetText;

typedef struct {
	int id, hidden;
} NetHide;

// Someone on the friends list. world is 0 while they are offline.
typedef struct {
	uint64_t key;
	int world, rank;
	char worldName[24];
} NetFriend;

// One line of the chat log. who is empty for messages from the game.
typedef struct {
	uint8_t kind;
	char who[16];
	char text[NET_CHAT_TEXT];
} NetChat;

// What a player looks like, from the server. Kept after they leave view, since the server may not resend it.
typedef struct {
	uint16_t slots[12]; // 0 empty, below 32768 a body part + 256, otherwise a worn item + 32768
	uint8_t colours[5], female, size, combat, valid;
	int basId, npcId, skill;
	char name[13];
} NetLook;

typedef struct {
	int index, type; // the server's id for this NPC, and its definition
	int x, y;        // scene tile
	int angle;       // the way it should face, 0 to 2047
	int pendingSeq, pendingDelay; // an action from the server, NET_NO_SEQ if none
	int faceEntity;  // -1, an NPC index, or 32768 plus a player index
	int faceX, faceY; // in half tiles, 0 if none
	uint16_t newHitDamage[4]; // hits waiting for the app to show them
	uint8_t newHitType[4], newHitCount;
	uint8_t bar, barNew;      // health as a fraction of 255, and whether it just changed
	uint16_t hitDamage[4];    // from here on, kept by the app
	uint8_t hitType[4];
	uint32_t hitUntil[4], barUntil;
	char overhead[84]; // the last thing said
	uint8_t overheadNew; // set when it changes, and cleared by the app
	uint32_t overheadUntil; // kept by the app

	// Kept by the app.
	float viewX, viewY, viewAngle; // where it is drawn
	uint8_t placed;  // the view values are set
	uint8_t running; // the last move was two tiles
	uint8_t hasModel;
	uint64_t modelKey;
	int seqId, seqDelay, seqFrame, seqClock, seqReplay;
	int moveSeq, moveFrame, moveClock;
	int turnCounter;
	const void* pose; // positions of the current animation frame
	uint64_t poseKey;
} NetNpc;

typedef struct {
	int sock;
	int connected;
	int loggedOut; // the server ended the session because the player asked it to
	int reply;
	char error[96];

	uint8_t in[NET_BUFFER_SIZE];
	size_t inLength;
	long lastPingMs;

	int selfId, rights, members; // members is whether this is a members world
	int playerMember;
	uint64_t selfKey;
	int plane, localX, localY; // the player's position within the scene
	int zoneX, zoneY;          // the zone the scene is centred on
	int rebuildPending;        // set when the server asks for a new scene
	int rootInterface;
	int inLobby;               // the welcome screen is showing
	int regionCount;
	int regions[NET_MAX_REGIONS];
	int32_t keys[NET_MAX_REGIONS][4];

	NetLocChange changes[NET_MAX_CHANGES];
	int changeHead, changeCount;
	int chunkX, chunkY; // the zone the next object changes apply to
	long locAdds, locDeletes;

	NetNpc npcs[NET_MAX_NPCS]; // in the server's order, which the update packet relies on
	int npcCount;
	long npcUpdates, npcErrors, npcSeqUpdates, npcFaceUpdates;

	NetNpc players[NET_MAX_PLAYERS]; // other players, in the server's order
	int playerCount;
	NetNpc self;
	NetLook looks[2048];
	char playerOptions[NET_PLAYER_OPTIONS][24];
	uint8_t playerOptionLow[NET_PLAYER_OPTIONS]; // listed below Walk here
	long playerErrors, playerSeqUpdates, lookUpdates, playerActionsSent;

	NetSub subs[NET_MAX_SUBS];
	int subCount;
	NetText texts[NET_MAX_TEXTS]; // the server's text for components, replacing the cache's
	int textCount, textNext;
	NetHide hides[NET_MAX_HIDES];
	int hideCount, hideNext;
	NetPicture pictures[NET_MAX_PICTURES];
	int pictureCount, pictureNext;
	long ifaceVersion; // changes whenever any of the above does
	long continuesSent;

	NetInv invs[NET_MAX_INVS];
	int invCount;
	long invVersion, itemActionsSent, itemMovesSent;

	NetFriend friends[NET_MAX_FRIENDS];
	int friendCount, friendState; // friendState is 2 once the server has sent the list
	uint64_t ignores[NET_MAX_IGNORES];
	int ignoreCount;
	long socialVersion, socialSent;

	NetFriend clan[NET_MAX_CLAN]; // the people in the clan chat
	int clanCount;
	uint64_t clanName, clanOwner; // 0 when not in one

	NetChat chat[NET_CHAT_LINES]; // a ring, and chatVersion counts every line ever added
	long chatVersion, chatSent;

	int32_t varps[NET_MAX_VARPS]; // the game's variables, which interface scripts read
	long varpVersion; // counts changes, and varpChanges[version % NET_CHANGE_RING] is where the next goes
	int varpChanges[NET_CHANGE_RING]; // -1 means all of them
	int (*onVarbit)(int id, int value); // varbits need the cache to place, so the caller does it. Returns the variable.

	int skillLevel[32], skillXp[32]; // current level, which boosts change, and experience
	long statVersion;
	int statChanges[NET_CHANGE_RING];

	NetGround ground[NET_MAX_GROUND];
	int groundCount;
	long groundVersion, groundActionsSent;

	int headKind, headNpc, headComponent; // the talking head the server wants
	int headAnim, headAnimComponent; // an expression, which can arrive before or after the head it is for
	long headVersion;

	int promptKind;     // the script that asked: 108 for a number, 109 for a name, 110 for text
	char promptText[80];
	long promptVersion; // counts prompts

	char lastMessage[128];
	long messages;
	void (*onMessage)(const char* text);

	long packets, bytes, opcodeCounts[256];
	long playerUpdates, moves, teleports, rebuilds;
	long walksSent, buttonsSent, objectActionsSent, npcActionsSent, spellsSent;
} NetClient;

// Set this to replace how the socket connects, for systems where a non-blocking connect is unreliable.
// It gets the socket, a struct sockaddr_in and a time limit, and returns 0 once connected.
extern int (*net_connect_hook)(int sock, const void* address, int addressSize, int timeoutMs);

// Connects and logs in, blocking until the server answers. Returns 0 on success.
int net_login(NetClient* c, const char* host, int port, const char* username, const char* password);

// Reads and handles whatever has arrived, and keeps the connection alive. Returns 0 while connected.
int net_poll(NetClient* c, long nowMs);

void net_close(NetClient* c);

// Takes the oldest queued object change. Returns 0 when there are none.
int net_next_change(NetClient* c, NetLocChange* out);

// Asks the server to walk the player to a world tile. The server finds the path.
void net_walk(NetClient* c, int worldX, int worldY, int run);

// Uses option 0 to 4 on the object at a world tile. The server walks the player there.
void net_loc_action(NetClient* c, int option, int id, int worldX, int worldY);

// Asks the server for an object's examine text.
void net_loc_examine(NetClient* c, int id);

// Uses option 0 to 4 on an NPC. The server walks the player there.
void net_npc_action(NetClient* c, int option, int index);

// Asks the server for an NPC's examine text.
void net_npc_examine(NetClient* c, int type);

// Uses one of the options the server gave for players. Returns 0 if the server has no such option.
int net_player_action(NetClient* c, int option, int index);

// Presses a component that continues a dialogue or picks an option.
void net_continue(NetClient* c, int component);

// Presses a button of the older kind, which has no options of its own.
void net_if_button(NetClient* c, int interfaceId, int button);

// Presses a row of a list that a script made.
void net_button_slot(NetClient* c, int interfaceId, int button, int slot);

// Casts a spell on a target. spell is interface << 16 | component.
void net_spell_on_npc(NetClient* c, int spell, int index);
void net_spell_on_player(NetClient* c, int spell, int index);
void net_spell_on_item(NetClient* c, int spell, int component, int slot, int item);
void net_spell_on_ground(NetClient* c, int spell, int item, int worldX, int worldY);
void net_spell_on_loc(NetClient* c, int spell, int id, int worldX, int worldY);

// Names as the game stores them, and back to how they are shown.
uint64_t net_name_key(const char* name);
void net_name_from_key(uint64_t key, char* out, size_t size);

// Change the lists, which update at once as in the original.
// Adding returns 0 if sent, 1 if the list is full, 2 if already on it, 3 if on the other list, 4 if it is the player.
int net_add_friend(NetClient* c, uint64_t key);
int net_add_ignore(NetClient* c, uint64_t key);
void net_del_friend(NetClient* c, uint64_t key);
void net_del_ignore(NetClient* c, uint64_t key);

// Says something out loud, or to one player.
void net_chat_public(NetClient* c, const char* text);
void net_chat_private(NetClient* c, uint64_t key, const char* text);

// Joins the clan chat of an owner. A key of 0 leaves.
void net_clan_join(NetClient* c, uint64_t owner);

// Picks an option of a component with slots, such as a bank. opcode says which option.
void net_if_option(NetClient* c, int opcode, int interfaceId, int button, int slot);

// Answers a prompt from the server.
void net_answer_number(NetClient* c, int value);
void net_answer_name(NetClient* c, uint64_t key);
void net_answer_text(NetClient* c, const char* text);

// Closes the window in the middle of the screen, as its close button does.
void net_close_modal(NetClient* c);

// 1 if this interface is open anywhere.
int net_interface_is_open(const NetClient* c, int id);

// The interface open at this parent, or -1.
int net_open_interface(const NetClient* c, int parent);

// The server's text for a component, or NULL if it has not set any.
const char* net_component_text(const NetClient* c, int component);

// What the server has put on a component, or NULL.
const NetPicture* net_component_picture(const NetClient* c, int component);

// 1 if the server hid this component, 0 if it showed it, -1 if it has not said.
int net_component_hidden(const NetClient* c, int component);

// The container with this id, or NULL if the server has not sent it.
const NetInv* net_inventory(const NetClient* c, int id);

// Uses option 0 to 4 of the item in a slot.
void net_item_action(NetClient* c, int option, int component, int item, int slot);
void net_item_examine(NetClient* c, int item);

// Swaps two slots of a container.
void net_move_item(NetClient* c, int component, int fromSlot, int toSlot);

// Uses ground option 2 or 3 on an item at a world tile. The server walks the player there.
void net_ground_action(NetClient* c, int option, int item, int worldX, int worldY);

// Takes off a worn item, or uses its Operate option.
void net_worn_action(NetClient* c, int option, int item, int slot);

// Uses a chosen item on something else. Each is a single action.
void net_use_item_on_ground(NetClient* c, int component, int slot, int item, int groundItem, int worldX, int worldY);
void net_use_item_on_item(NetClient* c, int component, int usedSlot, int usedItem, int slot, int item);
void net_use_item_on_npc(NetClient* c, int component, int slot, int item, int npc);
void net_use_item_on_player(NetClient* c, int component, int slot, int item, int player);
void net_use_item_on_loc(NetClient* c, int component, int slot, int item, int id, int worldX, int worldY);

// Presses an interface button.
void net_button(NetClient* c, int interfaceId, int button);

// The same for a button's second option.
void net_button_second(NetClient* c, int interfaceId, int button);

// Presses Play on the welcome screen.
void net_play(NetClient* c);

// Sends a chat command without its "::" prefix.
void net_command(NetClient* c, const char* command);

int net_world_x(const NetClient* c);
int net_world_y(const NetClient* c);

// Public for testing: RSA-encrypts data with the server's public key. Returns the output length.
int net_rsa_encrypt(const uint8_t* data, int size, uint8_t* out);

#endif
