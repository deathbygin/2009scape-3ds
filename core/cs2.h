// The client's interface script engine, ported from ScriptRunner. Scripts here work out what a screen contains:
// text, colours, what is hidden and where things sit. Drawing stays native to the 3DS, so anything to do with
// pixels is left out. An instruction that is not ported stops the script and is reported, never skipped silently.
#ifndef CS2_H
#define CS2_H

#include "iface.h"

#define CS2_MAX_VARPS 4000

// varps is the game's variable table, kept by the caller.
void cs2_init(Js5Cache* cache, int32_t* varps);

// Finds a component by its interface << 16 | number id, loading the interface if needed. NULL if there is none.
IfaceComponent* cs2_component(int id);
Iface* cs2_interface(int id);

// Runs a script from a component's event data: the script id followed by its arguments.
// Returns 0 on success, or a negative instruction number if it stopped on one that is not ported.
int cs2_run(const int32_t* args, int count, int sourceId);

// The same for a script that answers with a number, which goes in out.
int cs2_call_int(const int32_t* args, int count, int* out);

// Runs every load script of an interface, as InterfaceList.runOnLoadScripts does.
int cs2_run_load(int interfaceId);

// Runs the scripts that watch variables. changed is the variable, or -1 to run them all.
int cs2_run_varp_change(int interfaceId, int changed);

// The same for several at once, running each script no more than once, as the client does.
// A script watching nothing in particular runs for any change. An id of -1 means everything changed.
int cs2_run_varp_changes(int interfaceId, const int* changed, int count);
int cs2_run_stat_changes(int interfaceId, const int* changed, int count);

// Runs the scripts that watch skills. changed is the skill, or -1 to run them all.
int cs2_run_stat_change(int interfaceId, int changed);

// Where scripts read skills from. base gives a skill's level before boosts.
void cs2_set_stats(const int* levels, const int* experience, int (*base)(int skill));

// What scripts ask about the game. members points at whether this is a members world.
void cs2_set_game(int (*invTotal)(int inv, int item), int (*objMembers)(int item), const int* members);

// Where scripts read item names from.
void cs2_set_item_names(const char* (*name)(int item));

// An enum's number for a key.
int cs2_enum_int(int id, int key);

// How to count the lines text takes in a font.
void cs2_set_text_measure(int (*lines)(const char* text, int width, int font));

// A component's size once its parent has been taken into account.
int cs2_width(const IfaceComponent* c);
int cs2_height(const IfaceComponent* c);

// Runs the scripts that watch containers. An id of -1 means all of them changed.
int cs2_run_inv_changes(int interfaceId, const int* changed, int count);

// A component a script made, or NULL if it has not made that one.
IfaceComponent* cs2_created(int parent, int child);

// Every component a script made under a parent, oldest first. Returns how many, up to max.
int cs2_created_list(int parent, IfaceComponent** out, int* children, int max);

// Reads and writes a varbit, which is part of a variable.
int cs2_get_varbit(int id);
int cs2_set_varbit(int id, int value); // returns the variable it is part of, or -1

long cs2_instructions(void); // how many have run, for timing

#endif
