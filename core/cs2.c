#include "cs2.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARCHIVE_SCRIPTS 12
#define ARCHIVE_ENUMS 17
#define ARCHIVE_VARBITS 22
#define STACK_SIZE 1000
#define MAX_FRAMES 50
#define MAX_CYCLES 2000000
#define MAX_TEMP_STRINGS 4096
#define MAX_LOADED 48
#define SCRIPT_BUCKETS 256
#define ENUM_BUCKETS 64
#define MAX_VARCS 2000
#define MAX_ARRAYS 5
#define ARRAY_SIZE 5000

typedef struct {
	int count;
	int32_t* keys;
	int32_t* offsets;
} SwitchTable;

typedef struct Script {
	int id, intLocals, stringLocals, intArgs, stringArgs, instructions, switchCount;
	uint16_t* ops;
	int32_t* operands;
	char** strings;
	SwitchTable* switches;
	struct Script* next;
} Script;

typedef struct EnumDef {
	int id, keyType, valueType, defaultInt, count;
	char* defaultString;
	int32_t* keys;
	int32_t* ints;
	char** strings;
	struct EnumDef* next;
} EnumDef;

typedef struct {
	const Script* script;
	int pc;
	int32_t* intLocals;
	const char** stringLocals;
} Frame;

typedef struct {
	const uint8_t* data;
	size_t size, pos;
} Buf;

static Js5Cache* cache = NULL;
static int32_t* varps = NULL;
static int32_t varcs[MAX_VARCS];
static int32_t arrays[MAX_ARRAYS][ARRAY_SIZE];
static int arrayLengths[MAX_ARRAYS];
static Script* scripts[SCRIPT_BUCKETS];
static EnumDef* enums[ENUM_BUCKETS];
static Iface loaded[MAX_LOADED];
static uint8_t loadedUsed[MAX_LOADED];
static char* tempStrings[MAX_TEMP_STRINGS];
static int tempCount = 0;
static long instructionCount = 0;
static const int* statLevels = NULL;
static const int* statExperience = NULL;
static int (*statBase)(int skill) = NULL;
static int (*gameInvTotal)(int inv, int item) = NULL;
static int (*gameObjMembers)(int item) = NULL;
static const int* gameMembers = NULL;
static int (*textLines)(const char* text, int width, int font) = NULL;

// Components made by scripts belong to a parent. They only need to exist so the script can run.
typedef struct Created {
	IfaceComponent component;
	int parent, child;
	struct Created* next;
} Created;
static Created* created = NULL;

static int g1(Buf* b) { return b->pos < b->size ? b->data[b->pos++] : (b->pos++, 0); }
static int g2(Buf* b) { int a = g1(b); return a << 8 | g1(b); }
static int g4(Buf* b) { int a = g2(b); return (int)((uint32_t)a << 16 | (uint32_t)g2(b)); }

static char* gstr(Buf* b) {
	size_t start = b->pos;
	while (b->pos < b->size && b->data[b->pos] != 0) b->pos++;
	size_t length = b->pos - start;
	b->pos++;
	char* out = malloc(length + 1);
	if (out) {
		memcpy(out, b->data + start, length);
		out[length] = 0;
	}
	return out;
}

void cs2_init(Js5Cache* c, int32_t* v) {
	cache = c;
	varps = v;
}

void cs2_set_stats(const int* levels, const int* experience, int (*base)(int skill)) {
	statLevels = levels;
	statExperience = experience;
	statBase = base;
}

void cs2_set_game(int (*invTotal)(int inv, int item), int (*objMembers)(int item), const int* members) {
	gameInvTotal = invTotal;
	gameObjMembers = objMembers;
	gameMembers = members;
}

void cs2_set_text_measure(int (*lines)(const char* text, int width, int font)) {
	textLines = lines;
}

long cs2_instructions(void) {
	return instructionCount;
}

Iface* cs2_interface(int id) {
	int free = -1;
	for (int i = 0; i < MAX_LOADED; i++) {
		if (loadedUsed[i] && loaded[i].id == id) return &loaded[i];
		if (!loadedUsed[i] && free < 0) free = i;
	}
	if (free < 0 || iface_load(cache, id, &loaded[free]) != 0) return NULL;
	loadedUsed[free] = 1;
	return &loaded[free];
}

IfaceComponent* cs2_component(int id) {
	Iface* f = cs2_interface((int)((uint32_t)id >> 16));
	int number = id & 0xFFFF;
	if (!f || number >= f->count || f->components[number].type == 255) return NULL;
	return &f->components[number];
}

static IfaceComponent* created_component(int parent, int child) {
	for (Created* c = created; c; c = c->next) if (c->parent == parent && c->child == child) return &c->component;
	return NULL;
}

IfaceComponent* cs2_created(int parent, int child) {
	return created_component(parent, child);
}

int cs2_created_list(int parent, IfaceComponent** out, int* children, int max) {
	int count = 0;
	for (Created* c = created; c; c = c->next) if (c->parent == parent) count++;
	if (count > max) count = max;
	int at = count;
	// The list is newest first, and walking it once matters: a music list has hundreds.
	for (Created* c = created; c && at > 0; c = c->next) {
		if (c->parent != parent) continue;
		at--;
		out[at] = &c->component;
		children[at] = c->child;
	}
	return count;
}

// ---- scripts and enums from the cache ----

static const Script* get_script(int id) {
	int bucket = id & (SCRIPT_BUCKETS - 1);
	for (Script* s = scripts[bucket]; s; s = s->next) if (s->id == id) return s;
	Js5Buffer group = {0, 0}, file = {0, 0};
	if (js5_load_index(cache, ARCHIVE_SCRIPTS) != 0 || js5_read_group(cache, ARCHIVE_SCRIPTS, (uint32_t)id, NULL, &group) != 0) return NULL;
	int rc = js5_group_file(&cache->index[ARCHIVE_SCRIPTS], (uint32_t)id, &group, 0, &file);
	free(group.data);
	if (rc != 0) return NULL;
	Buf b = {file.data, file.size, file.size - 2};
	int trailerLength = g2(&b);
	size_t trailer = file.size - (size_t)trailerLength - 12 - 2;
	b.pos = trailer;
	Script* s = calloc(1, sizeof(Script));
	if (!s) {
		free(file.data);
		return NULL;
	}
	s->id = id;
	s->instructions = g4(&b);
	s->intLocals = g2(&b);
	s->stringLocals = g2(&b);
	s->intArgs = g2(&b);
	s->stringArgs = g2(&b);
	s->switchCount = g1(&b);
	if (s->switchCount > 0) {
		s->switches = calloc((size_t)s->switchCount, sizeof(SwitchTable));
		for (int i = 0; i < s->switchCount; i++) {
			SwitchTable* t = &s->switches[i];
			t->count = g2(&b);
			t->keys = malloc(sizeof(int32_t) * (size_t)(t->count + 1));
			t->offsets = malloc(sizeof(int32_t) * (size_t)(t->count + 1));
			for (int k = 0; k < t->count; k++) {
				t->keys[k] = g4(&b);
				t->offsets[k] = g4(&b);
			}
		}
	}
	b.pos = 0;
	while (b.pos < file.size && file.data[b.pos] != 0) b.pos++;
	b.pos++;
	size_t n = (size_t)s->instructions + 1;
	s->ops = calloc(n, 2);
	s->operands = calloc(n, 4);
	s->strings = calloc(n, sizeof(char*));
	for (int i = 0; i < s->instructions && b.pos < trailer; i++) {
		int op = g2(&b);
		if (op == 3) s->strings[i] = gstr(&b);
		else if (op >= 100 || op == 21 || op == 38 || op == 39) s->operands[i] = g1(&b);
		else s->operands[i] = g4(&b);
		s->ops[i] = (uint16_t)op;
	}
	free(file.data);
	s->next = scripts[bucket];
	scripts[bucket] = s;
	return s;
}

static const char* (*gameObjName)(int item);

void cs2_set_item_names(const char* (*name)(int item)) {
	gameObjName = name;
}

static const EnumDef* get_enum(int id) {
	int bucket = id & (ENUM_BUCKETS - 1);
	for (EnumDef* e = enums[bucket]; e; e = e->next) if (e->id == id) return e;
	Js5Buffer group = {0, 0}, file = {0, 0};
	uint32_t g = (uint32_t)id >> 8;
	if (js5_load_index(cache, ARCHIVE_ENUMS) != 0 || js5_read_group(cache, ARCHIVE_ENUMS, g, NULL, &group) != 0) return NULL;
	int rc = js5_group_file(&cache->index[ARCHIVE_ENUMS], g, &group, (uint32_t)(id & 255), &file);
	free(group.data);
	if (rc != 0) return NULL;
	EnumDef* e = calloc(1, sizeof(EnumDef));
	if (!e) {
		free(file.data);
		return NULL;
	}
	e->id = id;
	Buf b = {file.data, file.size, 0};
	while (b.pos < b.size) {
		int op = g1(&b);
		if (op == 0) break;
		if (op == 1) e->keyType = g1(&b);
		else if (op == 2) e->valueType = g1(&b);
		else if (op == 3) e->defaultString = gstr(&b);
		else if (op == 4) e->defaultInt = g4(&b);
		else if (op == 5 || op == 6) {
			e->count = g2(&b);
			e->keys = malloc(sizeof(int32_t) * (size_t)(e->count + 1));
			if (op == 5) e->strings = calloc((size_t)e->count + 1, sizeof(char*));
			else e->ints = malloc(sizeof(int32_t) * (size_t)(e->count + 1));
			for (int i = 0; i < e->count; i++) {
				e->keys[i] = g4(&b);
				if (op == 5) e->strings[i] = gstr(&b);
				else e->ints[i] = g4(&b);
			}
		}
	}
	free(file.data);
	e->next = enums[bucket];
	enums[bucket] = e;
	return e;
}

static int enum_int(const EnumDef* e, int key) {
	if (e->ints) for (int i = 0; i < e->count; i++) if (e->keys[i] == key) return e->ints[i];
	return e->defaultInt;
}

int cs2_enum_int(int id, int key) {
	const EnumDef* e = get_enum(id);
	return e ? enum_int(e, key) : -1;
}

static const char* enum_string(const EnumDef* e, int key) {
	if (e->strings) for (int i = 0; i < e->count; i++) if (e->keys[i] == key) return e->strings[i];
	return e->defaultString ? e->defaultString : "";
}

// ---- variables ----

static int varbit_read(int id, int* base, int* start, int* end) {
	// Neighbouring varbits share a group, and unpacking one for each would be slow on the 3DS.
	static Js5Buffer group = {0, 0};
	static uint32_t groupId = 0xFFFFFFFFu;
	uint32_t g = (uint32_t)id >> 10;
	Js5Buffer file = {0, 0};
	if (g != groupId) {
		free(group.data);
		group.data = NULL;
		groupId = 0xFFFFFFFFu;
		if (js5_load_index(cache, ARCHIVE_VARBITS) != 0 || js5_read_group(cache, ARCHIVE_VARBITS, g, NULL, &group) != 0) return -1;
		groupId = g;
	}
	int rc = js5_group_file(&cache->index[ARCHIVE_VARBITS], g, &group, (uint32_t)(id & 1023), &file);
	if (rc != 0 || file.size < 5 || file.data[0] != 1) {
		free(file.data);
		return -1;
	}
	Buf b = {file.data, file.size, 1};
	*base = g2(&b);
	*start = g1(&b);
	*end = g1(&b);
	free(file.data);
	return 0;
}

#define VARBIT_CACHE 512
typedef struct {
	int id, base, start, end;
} VarbitEntry;
static VarbitEntry varbitCache[VARBIT_CACHE];
static int varbitCount = 0;

static const VarbitEntry* varbit(int id) {
	for (int i = 0; i < varbitCount; i++) if (varbitCache[i].id == id) return &varbitCache[i];
	VarbitEntry v = {id, 0, 0, 0};
	if (varbit_read(id, &v.base, &v.start, &v.end) != 0) return NULL;
	if (varbitCount == VARBIT_CACHE) varbitCount = 0;
	varbitCache[varbitCount] = v;
	return &varbitCache[varbitCount++];
}

static uint32_t bit_mask(int bits) {
	return bits >= 31 ? 0x7FFFFFFFu | (bits >= 32 ? 0x80000000u : 0) : (1u << bits) - 1;
}

int cs2_get_varbit(int id) {
	const VarbitEntry* v = varbit(id);
	if (!v || v->base >= CS2_MAX_VARPS) return 0;
	return (int)(((uint32_t)varps[v->base] >> v->start) & bit_mask(v->end - v->start + 1));
}

int cs2_set_varbit(int id, int value) {
	const VarbitEntry* v = varbit(id);
	if (!v || v->base >= CS2_MAX_VARPS) return -1;
	uint32_t mask = bit_mask(v->end - v->start + 1);
	if (value < 0 || (uint32_t)value > mask) value = 0;
	mask <<= v->start;
	varps[v->base] = (int32_t)(((uint32_t)value << v->start & mask) | ((uint32_t)varps[v->base] & ~mask));
	return v->base;
}

// ---- strings ----

static const char* temp_string(size_t size, char** out) {
	char* s = malloc(size + 1);
	if (!s) s = NULL;
	if (s && tempCount < MAX_TEMP_STRINGS) tempStrings[tempCount++] = s;
	*out = s;
	return s;
}

static const char* temp_concat(const char* a, const char* b) {
	char* s;
	size_t la = strlen(a), lb = strlen(b);
	if (!temp_string(la + lb, &s)) return "";
	memcpy(s, a, la);
	memcpy(s + la, b, lb + 1);
	return s;
}

static const char* temp_int(int v) {
	char number[16];
	snprintf(number, sizeof(number), "%d", v);
	return temp_concat(number, "");
}

// A script can replace the watcher it is running from, so the old text has to last until the scripts finish.
static void retire_strings(char** texts, int count) {
	if (!texts) return;
	for (int i = 0; i < count; i++) {
		if (texts[i] && tempCount < MAX_TEMP_STRINGS) tempStrings[tempCount++] = texts[i];
	}
	free(texts);
}

static void free_temp_strings(void) {
	for (int i = 0; i < tempCount; i++) free(tempStrings[i]);
	tempCount = 0;
}

static void set_text(IfaceComponent* c, const char* text) {
	size_t n = strlen(text);
	char* copy = malloc(n + 1);
	if (!copy) return;
	memcpy(copy, text, n + 1);
	free(c->text);
	c->text = copy;
}

static void set_op(IfaceComponent* c, int index, const char* text) {
	if (index < 0 || index >= IFACE_MAX_OPS) return;
	size_t n = strlen(text);
	char* copy = malloc(n + 1);
	if (!copy) return;
	memcpy(copy, text, n + 1);
	free(c->ops[index]);
	c->ops[index] = copy;
	if (c->opCount <= index) c->opCount = index + 1;
}

// A size can be fixed, or taken from the parent.
static int layout_size(const IfaceComponent* c, int vertical, int depth) {
	int base = vertical ? c->height : c->width, mode = vertical ? c->dynamicHeight : c->dynamicWidth;
	if (mode == 0 || depth > 16) return base;
	int parentSize = vertical ? 334 : 512;
	if (c->parent != -1) {
		const IfaceComponent* p = cs2_component(c->parent);
		if (p) parentSize = layout_size(p, vertical, depth + 1);
	}
	if (mode == 1) return parentSize - base;
	if (mode == 2) return parentSize * base >> 14;
	return base;
}

int cs2_width(const IfaceComponent* c) {
	return layout_size(c, 0, 0);
}

int cs2_height(const IfaceComponent* c) {
	return layout_size(c, 1, 0);
}

// ---- the interpreter ----

// Reads one file of the config archive.
static int read_config(uint32_t group, uint32_t id, Js5Buffer* file) {
	Js5Buffer data = {0, 0};
	if (js5_load_index(cache, 2) != 0 || js5_read_group(cache, 2, group, NULL, &data) != 0) return -1;
	int rc = js5_group_file(&cache->index[2], group, &data, id, file);
	free(data.data);
	return rc;
}

// Reads a parameter of a struct, falling back to the parameter's default as the client does.
static int struct_param(int structId, int paramId, int* value, const char** text) {
	Js5Buffer file = {0, 0};
	int isString = 0;
	*value = 0;
	*text = "";
	if (paramId >= 0 && read_config(11, (uint32_t)paramId, &file) == 0) {
		Buf b = {file.data, file.size, 0};
		while (b.pos < b.size) {
			int op = g1(&b);
			if (op == 0) break;
			if (op == 1) isString = g1(&b) == 's';
			else if (op == 2) *value = g4(&b);
			else if (op == 5) {
				char* t = gstr(&b);
				const char* kept = temp_concat(t ? t : "", "");
				free(t);
				*text = kept;
			}
		}
		free(file.data);
	}
	file.data = NULL;
	if (structId < 0 || read_config(26, (uint32_t)structId, &file) != 0) return isString;
	Buf b = {file.data, file.size, 0};
	while (b.pos < b.size) {
		int op = g1(&b);
		if (op != 249) break;
		int count = g1(&b);
		for (int i = 0; i < count; i++) {
			int stringValue = g1(&b), key = g1(&b) << 16;
			key |= g2(&b);
			if (stringValue) {
				char* t = gstr(&b);
				if (key == paramId) *text = temp_concat(t ? t : "", "");
				free(t);
			} else {
				int v = g4(&b);
				if (key == paramId) *value = v;
			}
		}
	}
	free(file.data);
	return isString;
}

static int lastReturn;

static int run(const int32_t* args, int count, char** strings, int sourceId) {
	static int32_t intStack[STACK_SIZE];
	static const char* stringStack[STACK_SIZE];
	static Frame frames[MAX_FRAMES];
	const Script* script = get_script(args[0]);
	if (!script) return -1;
	IfaceComponent* active[2] = {NULL, NULL}; // active[1] is the one operand 1 selects
	int isp = 0, ssp = 0, pc = -1, fp = 0, result = 0;
	long cycles = 0;
	int32_t* intLocals = calloc((size_t)script->intLocals + 1, 4);
	const char** stringLocals = calloc((size_t)script->stringLocals + 1, sizeof(char*));
	int intArg = 0, stringArg = 0;
	for (int i = 0; i < script->stringLocals; i++) stringLocals[i] = "";
	for (int i = 1; i < count; i++) {
		int32_t v = args[i];
		if (v == IFACE_STRING_ARG) {
			if (stringArg < script->stringLocals) stringLocals[stringArg++] = strings && strings[i] ? strings[i] : "";
			continue;
		}
		if (v == (int32_t)0x80000003) v = sourceId;
		else if (v == (int32_t)0x80000005 || v == (int32_t)0x80000006 || v == (int32_t)0x80000007) v = -1;
		else if (v == (int32_t)0x80000001 || v == (int32_t)0x80000002 || v == (int32_t)0x80000004 || v == (int32_t)0x80000008 || v == (int32_t)0x80000009) v = 0;
		if (intArg <= script->intLocals) intLocals[intArg++] = v;
	}

	for (;;) {
		if (++cycles > MAX_CYCLES) {
			result = -2;
			break;
		}
		instructionCount++;
		pc++;
		if (pc >= script->instructions) {
			result = -3;
			break;
		}
		int op = script->ops[pc], operand = script->operands[pc], a, b2, c2;
		const char* s1;
		const char* s2;
		IfaceComponent* comp;

		if (op < 100) {
			switch (op) {
			case 0: intStack[isp++] = operand; continue;
			case 1: intStack[isp++] = operand < CS2_MAX_VARPS ? varps[operand] : 0; continue;
			case 2:
				isp--;
				if (operand < CS2_MAX_VARPS) varps[operand] = intStack[isp];
				continue;
			case 3: stringStack[ssp++] = script->strings[pc] ? script->strings[pc] : ""; continue;
			case 6: pc += operand; continue;
			case 7: isp -= 2; if (intStack[isp] != intStack[isp + 1]) pc += operand; continue;
			case 8: isp -= 2; if (intStack[isp + 1] == intStack[isp]) pc += operand; continue;
			case 9: isp -= 2; if (intStack[isp] < intStack[isp + 1]) pc += operand; continue;
			case 10: isp -= 2; if (intStack[isp + 1] < intStack[isp]) pc += operand; continue;
			case 21: {
				if (fp == 0) goto finished;
				free(intLocals);
				free((void*)stringLocals);
				Frame* f = &frames[--fp];
				script = f->script;
				intLocals = f->intLocals;
				stringLocals = f->stringLocals;
				pc = f->pc;
				continue;
			}
			case 25: intStack[isp++] = cs2_get_varbit(operand); continue;
			case 27: isp--; cs2_set_varbit(operand, intStack[isp]); continue;
			case 31: isp -= 2; if (intStack[isp + 1] >= intStack[isp]) pc += operand; continue;
			case 32: isp -= 2; if (intStack[isp] >= intStack[isp + 1]) pc += operand; continue;
			case 33: intStack[isp++] = intLocals[operand]; continue;
			case 34: isp--; intLocals[operand] = intStack[isp]; continue;
			case 35: stringStack[ssp++] = stringLocals[operand]; continue;
			case 36: ssp--; stringLocals[operand] = stringStack[ssp]; continue;
			case 37: {
				ssp -= operand;
				const char* joined = "";
				for (int i = 0; i < operand; i++) joined = temp_concat(joined, stringStack[ssp + i]);
				stringStack[ssp++] = joined;
				continue;
			}
			case 38: isp--; continue;
			case 39: ssp--; continue;
			case 40: {
				const Script* next = get_script(operand);
				if (!next || fp >= MAX_FRAMES) {
					result = -40;
					goto finished;
				}
				int32_t* nextInts = calloc((size_t)next->intLocals + 1, 4);
				const char** nextStrings = calloc((size_t)next->stringLocals + 1, sizeof(char*));
				for (int i = 0; i < next->stringLocals; i++) nextStrings[i] = "";
				for (int i = 0; i < next->intArgs; i++) nextInts[i] = intStack[i + isp - next->intArgs];
				for (int i = 0; i < next->stringArgs; i++) nextStrings[i] = stringStack[i + ssp - next->stringArgs];
				isp -= next->intArgs;
				ssp -= next->stringArgs;
				frames[fp++] = (Frame){script, pc, intLocals, stringLocals};
				script = next;
				pc = -1;
				intLocals = nextInts;
				stringLocals = nextStrings;
				continue;
			}
			case 42: intStack[isp++] = operand < MAX_VARCS ? varcs[operand] : 0; continue;
			case 43: isp--; if (operand < MAX_VARCS) varcs[operand] = intStack[isp]; continue;
			case 44: {
				int id = operand >> 16, type = operand & 0xFFFF, size = intStack[--isp];
				if (id >= MAX_ARRAYS || size < 0 || size > ARRAY_SIZE) {
					result = -44;
					goto finished;
				}
				arrayLengths[id] = size;
				for (int i = 0; i < size; i++) arrays[id][i] = type == 105 ? 0 : -1;
				continue;
			}
			case 45:
				a = intStack[--isp];
				if (operand >= MAX_ARRAYS || a < 0 || a >= arrayLengths[operand]) {
					result = -45;
					goto finished;
				}
				intStack[isp++] = arrays[operand][a];
				continue;
			case 46:
				isp -= 2;
				a = intStack[isp];
				if (operand >= MAX_ARRAYS || a < 0 || a >= arrayLengths[operand]) {
					result = -46;
					goto finished;
				}
				arrays[operand][a] = intStack[isp + 1];
				continue;
			case 51: {
				const SwitchTable* t = &script->switches[operand];
				a = intStack[--isp];
				for (int i = 0; i < t->count; i++) {
					if (t->keys[i] == a) {
						pc += t->offsets[i];
						break;
					}
				}
				continue;
			}
			default:
				result = -op - 10000;
				goto finished;
			}
		}

		int useFirst = operand == 1;
		IfaceComponent** slot = &active[useFirst ? 1 : 0];

		if (op == 100) {
			isp -= 3;
			// A script making a component again replaces the old one, as the client's array does.
			for (Created** at = &created; *at; at = &(*at)->next) {
				if ((*at)->parent != intStack[isp] || (*at)->child != intStack[isp + 2]) continue;
				Created* gone = *at;
				*at = gone->next;
				free(gone->component.text);
				free(gone);
				break;
			}
			Created* n = calloc(1, sizeof(Created));
			if (!n) {
				result = -100;
				goto finished;
			}
			n->parent = intStack[isp];
			n->child = intStack[isp + 2];
			n->component.id = n->parent;
			n->component.type = (uint8_t)intStack[isp + 1];
			n->component.parent = n->parent;
			n->component.font = n->component.spriteId = -1;
			n->next = created;
			created = n;
			*slot = &n->component;
			continue;
		}
		if (op == 101) continue;
		if (op == 102) {
			isp--;
			Created** at = &created;
			while (*at) {
				if ((*at)->parent == intStack[isp]) {
					Created* gone = *at;
					*at = gone->next;
					free(gone->component.text);
					free(gone);
				} else {
					at = &(*at)->next;
				}
			}
			continue;
		}
		if (op == 200) {
			isp -= 2;
			comp = created_component(intStack[isp], intStack[isp + 1]);
			intStack[isp++] = comp ? 1 : 0;
			if (comp) *slot = comp;
			continue;
		}
		if (op == 201) {
			comp = cs2_component(intStack[isp - 1]);
			intStack[isp - 1] = comp ? 1 : 0;
			if (comp) *slot = comp;
			continue;
		}

		// Setters work on the active component, or on one from the stack in the 2000 range.
		if ((op >= 1000 && op < 1500) || (op >= 2000 && op < 2500)) {
			if (op >= 2000) {
				comp = cs2_component(intStack[--isp]);
				op -= 1000;
			} else {
				comp = *slot;
			}
			static IfaceComponent scratch;
			if (!comp) {
				memset(&scratch, 0, sizeof(scratch));
				comp = &scratch;
			}
			if (op >= 1400) {
				// Hooks. Only the variable watch matters here, since nothing else can fire without a mouse.
				int32_t* triggers = NULL;
				int triggerCount = 0;
				s1 = stringStack[--ssp];
				size_t length = strlen(s1);
				if (length > 0 && s1[length - 1] == 'Y') {
					triggerCount = intStack[--isp];
					if (triggerCount > 0) triggers = malloc(sizeof(int32_t) * (size_t)triggerCount);
					for (int i = triggerCount - 1; i >= 0; i--) {
						isp--;
						if (triggers) triggers[i] = intStack[isp];
					}
					length--;
				}
				int32_t* hook = malloc(sizeof(int32_t) * (length + 1));
				char** texts = NULL;
				for (int i = (int)length; i >= 1; i--) {
					if (s1[i - 1] == 's') {
						ssp--;
						if (hook) hook[i] = IFACE_STRING_ARG;
						if (!texts) texts = calloc(length + 1, sizeof(char*));
						if (texts) {
							texts[i] = malloc(strlen(stringStack[ssp]) + 1);
							if (texts[i]) strcpy(texts[i], stringStack[ssp]);
						}
					} else {
						isp--;
						if (hook) hook[i] = intStack[isp];
					}
				}
				a = intStack[--isp];
				if ((op == 1407 || op == 1414 || op == 1415) && comp != &scratch) {
					int32_t** hookArgs = op == 1407 ? &comp->onVarpChange : op == 1414 ? &comp->onInvChange : &comp->onStatChange;
					int* hookCount = op == 1407 ? &comp->onVarpChangeCount : op == 1414 ? &comp->onInvChangeCount : &comp->onStatChangeCount;
					int32_t** hookTriggers = op == 1407 ? &comp->varpTriggers : op == 1414 ? &comp->invTriggers : &comp->statTriggers;
					int* hookTriggerCount = op == 1407 ? &comp->varpTriggerCount : op == 1414 ? &comp->invTriggerCount : &comp->statTriggerCount;
					char*** hookTexts = op == 1407 ? &comp->onVarpStrings : op == 1414 ? &comp->onInvStrings : &comp->onStatStrings;
					retire_strings(*hookTexts, *hookCount);
					*hookTexts = a == -1 ? NULL : texts;
					if (a == -1) texts = NULL;
					free(*hookArgs);
					free(*hookTriggers);
					*hookArgs = a == -1 ? NULL : hook;
					*hookCount = a == -1 ? 0 : (int)length + 1;
					if (a != -1 && hook) hook[0] = a;
					*hookTriggers = triggers;
					*hookTriggerCount = triggers ? triggerCount : 0;
					if (a == -1) free(hook);
				} else {
					free(hook);
					free(triggers);
				}
				if (texts && !((op == 1407 || op == 1414 || op == 1415) && comp != &scratch)) {
					for (size_t i = 0; i <= length; i++) free(texts[i]);
					free(texts);
				}
				continue;
			}
			switch (op) {
			case 1000:
				isp -= 4;
				comp->x = intStack[isp];
				comp->y = intStack[isp + 1];
				continue;
			case 1001:
				isp -= 4;
				comp->width = intStack[isp];
				comp->height = intStack[isp + 1];
				comp->dynamicWidth = (int8_t)intStack[isp + 2];
				comp->dynamicHeight = (int8_t)intStack[isp + 3];
				continue;
			case 1003: comp->hidden = intStack[--isp] == 1; continue;
			case 1004: isp -= 2; continue;
			case 1005: isp--; continue;
			case 1100:
				isp -= 2;
				comp->scrollX = intStack[isp];
				comp->scrollY = intStack[isp + 1];
				if (comp->scrollY > comp->scrollMaxV - layout_size(comp, 1, 0)) comp->scrollY = comp->scrollMaxV - layout_size(comp, 1, 0);
				if (comp->scrollY < 0) comp->scrollY = 0;
				continue;
			case 1101: comp->colour = intStack[--isp]; continue;
			case 1102: comp->filled = intStack[--isp] == 1; continue;
			case 1105: comp->spriteId = intStack[--isp]; continue;
			case 1108: comp->modelId = intStack[--isp]; continue;
			case 1103: case 1104: case 1106: case 1107: case 1110: case 1111: case 1115: case 1116: case 1117: case 1118: case 1119: case 1122: case 1123:
				isp--;
				continue;
			case 1109: isp -= 6; continue;
			case 1112: set_text(comp, stringStack[--ssp]); continue;
			case 1113: comp->font = intStack[--isp]; continue;
			case 1114:
				isp -= 3;
				comp->halign = (uint8_t)intStack[isp];
				comp->valign = (uint8_t)intStack[isp + 1];
				comp->vpadding = (uint8_t)intStack[isp + 2];
				continue;
			case 1120:
				isp -= 2;
				comp->scrollMaxH = intStack[isp];
				comp->scrollMaxV = intStack[isp + 1];
				continue;
			case 1200: case 1205:
				isp -= 2;
				comp->itemPlusOne = intStack[isp] + 1;
				comp->itemAmount = intStack[isp + 1];
				continue;
			case 1201: case 1203: case 1204: isp--; continue;
			case 1202: continue;
			case 1300:
				a = intStack[--isp] - 1;
				s1 = stringStack[--ssp];
				set_op(comp, a, s1);
				continue;
			case 1301: isp -= 2; continue;
			case 1302: case 1303: case 1304: isp--; continue;
			case 1305: {
				s1 = stringStack[--ssp];
				char* name = malloc(strlen(s1) + 1);
				if (name) {
					strcpy(name, s1);
					free(comp->option);
					comp->option = name;
				}
				continue;
			}
			case 1306: ssp--; continue;
			case 1307: comp->opCount = 0; continue;
			case 1308: case 1309: isp -= 2; continue;
			default:
				result = -(op < 2000 && script->ops[pc] >= 2000 ? script->ops[pc] : op);
				goto finished;
			}
		}

		// Getters: the active component in 1500 to 1899, or one from the stack in 2500 to 2899.
		if ((op >= 1500 && op < 1900) || (op >= 2500 && op < 2900)) {
			if (op >= 2500) {
				comp = cs2_component(intStack[--isp]);
				op -= 1000;
			} else {
				comp = *slot;
			}
			if (!comp) {
				result = -op;
				goto finished;
			}
			switch (op) {
			case 1500: intStack[isp++] = comp->x; continue;
			case 1501: intStack[isp++] = comp->y; continue;
			case 1502: intStack[isp++] = layout_size(comp, 0, 0); continue;
			case 1503: intStack[isp++] = layout_size(comp, 1, 0); continue;
			case 1504: intStack[isp++] = comp->hidden; continue;
			case 1505: intStack[isp++] = comp->parent; continue;
			case 1600: intStack[isp++] = comp->scrollX; continue;
			case 1601: intStack[isp++] = comp->scrollY; continue;
			case 1602: stringStack[ssp++] = temp_concat(comp->text ? comp->text : "", ""); continue; // a copy, as the text can be replaced
			case 1603: intStack[isp++] = comp->scrollMaxH; continue;
			case 1604: intStack[isp++] = comp->scrollMaxV; continue;
			case 1612: intStack[isp++] = comp->spriteId; continue;
			case 1800: intStack[isp++] = (int)(comp->events >> 11 & 0x7F); continue;
			default:
				result = -op;
				goto finished;
			}
		}

		switch (op) {
		case 3200: isp -= 3; continue; // a sound, and there is no audio yet
		case 3303:
			isp -= 2;
			intStack[isp] = gameInvTotal ? gameInvTotal(intStack[isp], intStack[isp + 1]) : 0;
			isp++;
			continue;
		case 3312: intStack[isp++] = gameMembers ? *gameMembers : 0; continue;
		case 4200: isp--; stringStack[ssp++] = gameObjName ? gameObjName(intStack[isp]) : ""; continue;
		case 4207: intStack[isp - 1] = gameObjMembers ? gameObjMembers(intStack[isp - 1]) : 0; continue;
		case 4500: {
			isp -= 2;
			int value;
			const char* text;
			if (struct_param(intStack[isp], intStack[isp + 1], &value, &text)) stringStack[ssp++] = text;
			else intStack[isp++] = value;
			continue;
		}
		case 3305: case 3306: case 3307:
			a = intStack[isp - 1];
			if (!statLevels || a < 0 || a >= 32) {
				result = -op;
				goto finished;
			}
			intStack[isp - 1] = op == 3305 ? statLevels[a] : op == 3306 ? statBase(a) : statExperience[a];
			continue;
		case 3400:
			isp -= 2;
			{
				const EnumDef* e = get_enum(intStack[isp]);
				if (!e) {
					result = -3400;
					goto finished;
				}
				if (e->valueType == 's') stringStack[ssp++] = enum_string(e, intStack[isp + 1]);
				else {
					result = -3400;
					goto finished;
				}
			}
			continue;
		case 3408:
			isp -= 4;
			{
				const EnumDef* e = get_enum(intStack[isp + 2]);
				int key = intStack[isp + 3], valueType = intStack[isp + 1];
				if (!e || e->keyType != intStack[isp] || e->valueType != valueType) {
					result = -3408;
					goto finished;
				}
				if (valueType == 's') stringStack[ssp++] = enum_string(e, key);
				else intStack[isp++] = enum_int(e, key);
			}
			continue;
		case 3411:
			{
				const EnumDef* e = get_enum(intStack[isp - 1]);
				intStack[isp - 1] = e ? e->count : 0;
			}
			continue;
		case 4000: isp -= 2; intStack[isp] = intStack[isp] + intStack[isp + 1]; isp++; continue;
		case 4001: isp -= 2; intStack[isp] = intStack[isp] - intStack[isp + 1]; isp++; continue;
		case 4002: isp -= 2; intStack[isp] = intStack[isp] * intStack[isp + 1]; isp++; continue;
		case 4003: isp -= 2; intStack[isp] = intStack[isp + 1] ? intStack[isp] / intStack[isp + 1] : 0; isp++; continue;
		case 4008: isp -= 2; intStack[isp] = intStack[isp] | 1 << intStack[isp + 1]; isp++; continue;
		case 4009: isp -= 2; intStack[isp] = (-(1 << intStack[isp + 1]) - 1) & intStack[isp]; isp++; continue;
		case 4010: isp -= 2; intStack[isp] = (intStack[isp] & 1 << intStack[isp + 1]) == 0 ? 0 : 1; isp++; continue;
		case 4011: isp -= 2; intStack[isp] = intStack[isp + 1] ? intStack[isp] % intStack[isp + 1] : 0; isp++; continue;
		case 4014: isp -= 2; intStack[isp] = intStack[isp] & intStack[isp + 1]; isp++; continue;
		case 4015: isp -= 2; intStack[isp] = intStack[isp] | intStack[isp + 1]; isp++; continue;
		case 4016: isp -= 2; intStack[isp] = intStack[isp] < intStack[isp + 1] ? intStack[isp] : intStack[isp + 1]; isp++; continue;
		case 4017: isp -= 2; intStack[isp] = intStack[isp + 1] >= intStack[isp] ? intStack[isp + 1] : intStack[isp]; isp++; continue;
		case 4018:
			isp -= 3;
			a = intStack[isp];
			b2 = intStack[isp + 1];
			c2 = intStack[isp + 2];
			intStack[isp++] = b2 ? (int)((long long)a * c2 / b2) : 0;
			continue;
		case 4100:
			s1 = stringStack[--ssp];
			a = intStack[--isp];
			stringStack[ssp++] = temp_concat(s1, temp_int(a));
			continue;
		case 4101:
			ssp -= 2;
			stringStack[ssp] = temp_concat(stringStack[ssp], stringStack[ssp + 1]);
			ssp++;
			continue;
		case 4103: {
			char* lower;
			s1 = stringStack[--ssp];
			if (!temp_string(strlen(s1), &lower)) lower = (char*)"";
			else {
				size_t i = 0;
				for (; s1[i]; i++) lower[i] = (char)tolower((unsigned char)s1[i]);
				lower[i] = 0;
			}
			stringStack[ssp++] = lower;
			continue;
		}
		case 4106: a = intStack[--isp]; stringStack[ssp++] = temp_int(a); continue;
		case 4107:
			ssp -= 2;
			s1 = stringStack[ssp];
			s2 = stringStack[ssp + 1];
			intStack[isp++] = strcmp(s1, s2);
			continue;
		case 4117: s1 = stringStack[--ssp]; intStack[isp++] = (int)strlen(s1); continue;
		case 4108:
			s1 = stringStack[--ssp];
			isp -= 2;
			a = textLines ? textLines(s1, intStack[isp], intStack[isp + 1]) : 1;
			intStack[isp++] = a;
			continue;
		default:
			result = -op;
			goto finished;
		}
	}
finished:
	lastReturn = isp > 0 ? intStack[isp - 1] : 0;
	free(intLocals);
	free((void*)stringLocals);
	while (fp > 0) {
		fp--;
		free(frames[fp].intLocals);
		free((void*)frames[fp].stringLocals);
	}
	// Text set by a script is copied, so temporary strings can go now.
	free_temp_strings();
	return result;
}

int cs2_run(const int32_t* args, int count, int sourceId) {
	if (!cache || count < 1) return -1;
	return run(args, count, NULL, sourceId);
}

int cs2_call_int(const int32_t* args, int count, int* out) {
	if (!cache || count < 1) return -1;
	int rc = run(args, count, NULL, -1);
	*out = lastReturn;
	return rc;
}

int cs2_run_load(int interfaceId) {
	Iface* f = cs2_interface(interfaceId);
	if (!f) return -1;
	int result = 0;
	for (int i = 0; i < f->count; i++) {
		IfaceComponent* c = &f->components[i];
		if (c->type == 255 || !c->onLoad) continue;
		int rc = run(c->onLoad, c->onLoadCount, c->onLoadStrings, c->id);
		if (rc != 0) result = rc;
	}
	return result;
}

static int watches(const int32_t* triggers, int triggerCount, const int* changed, int count) {
	if (triggerCount == 0) return count > 0;
	for (int i = 0; i < count; i++) {
		if (changed[i] == -1) return 1;
		for (int k = 0; k < triggerCount; k++) if (triggers[k] == changed[i]) return 1;
	}
	return 0;
}

int cs2_run_varp_changes(int interfaceId, const int* changed, int count) {
	Iface* f = cs2_interface(interfaceId);
	if (!f) return -1;
	int result = 0;
	for (int i = 0; i < f->count; i++) {
		IfaceComponent* c = &f->components[i];
		if (c->type == 255 || !c->onVarpChange || !watches(c->varpTriggers, c->varpTriggerCount, changed, count)) continue;
		int rc = run(c->onVarpChange, c->onVarpChangeCount, c->onVarpStrings, c->id);
		if (rc != 0) result = rc;
	}
	return result;
}

int cs2_run_stat_changes(int interfaceId, const int* changed, int count) {
	Iface* f = cs2_interface(interfaceId);
	if (!f) return -1;
	int result = 0;
	for (int i = 0; i < f->count; i++) {
		IfaceComponent* c = &f->components[i];
		if (c->type == 255 || !c->onStatChange || !watches(c->statTriggers, c->statTriggerCount, changed, count)) continue;
		int rc = run(c->onStatChange, c->onStatChangeCount, c->onStatStrings, c->id);
		if (rc != 0) result = rc;
	}
	return result;
}

int cs2_run_inv_changes(int interfaceId, const int* changed, int count) {
	Iface* f = cs2_interface(interfaceId);
	if (!f) return -1;
	int result = 0;
	for (int i = 0; i < f->count; i++) {
		IfaceComponent* c = &f->components[i];
		if (c->type == 255 || !c->onInvChange || !watches(c->invTriggers, c->invTriggerCount, changed, count)) continue;
		int rc = run(c->onInvChange, c->onInvChangeCount, c->onInvStrings, c->id);
		if (rc != 0) result = rc;
	}
	return result;
}

int cs2_run_stat_change(int interfaceId, int changed) {
	return cs2_run_stat_changes(interfaceId, &changed, 1);
}

int cs2_run_varp_change(int interfaceId, int changed) {
	return cs2_run_varp_changes(interfaceId, &changed, 1);
}
