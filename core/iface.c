#include "iface.h"

#include <stdlib.h>
#include <string.h>

#define ARCHIVE_INTERFACES 3

typedef struct {
	const uint8_t* data;
	size_t size, pos;
} Buf;

static int g1(Buf* b) { return b->pos < b->size ? b->data[b->pos++] : (b->pos++, 0); }
static int g1b(Buf* b) { return (int8_t)g1(b); }
static int g2(Buf* b) { int a = g1(b); return a << 8 | g1(b); }
static int g2b(Buf* b) { return (int16_t)g2(b); }
static int g3(Buf* b) { int a = g2(b); return a << 8 | g1(b); }
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

static void free_texts(char** texts, int count) {
	if (!texts) return;
	for (int i = 0; i < count; i++) free(texts[i]);
	free(texts);
}

static void skip_str(Buf* b) {
	while (b->pos < b->size && b->data[b->pos] != 0) b->pos++;
	b->pos++;
}

static void decode_if1(IfaceComponent* c, Buf* b) {
	c->type = (uint8_t)g1(b);
	c->buttonType = (uint8_t)g1(b);
	c->clientCode = g2(b);
	c->x = g2b(b);
	c->y = g2b(b);
	c->width = g2(b);
	c->height = g2(b);
	g1(b); // alpha
	c->parent = g2(b);
	c->parent = c->parent == 65535 ? -1 : c->parent + (int)((uint32_t)c->id & 0xFFFF0000u);
	g2(b); // hover layer
	int comparisons = g1(b);
	c->testVarp = -1;
	for (int i = 0; i < comparisons; i++) {
		int kind = g1(b), value = g2(b);
		if (i == 0) {
			c->testKind = (int8_t)kind;
			c->testValue = value;
		}
	}
	int scripts = g1(b);
	for (int i = 0; i < scripts; i++) {
		int length = g2(b);
		for (int k = 0; k < length; k++) {
			int word = g2(b);
			if (i == 0 && k == 0 && word != 5) c->testKind = 0;
			if (i == 0 && k == 1) c->testVarp = word;
		}
	}
	if (c->type == COMP_LAYER) {
		c->scrollMaxV = g2(b);
		c->hidden = g1(b) == 1;
	}
	if (c->type == 1) {
		g2(b);
		g1(b);
	}
	uint32_t events = 0;
	if (c->type == COMP_INVENTORY) {
		if (g1(b) == 1) events |= 0x10000000u;
		if (g1(b) == 1) events |= 0x40000000u;
		if (g1(b) == 1) events |= 0x80000000u;
		if (g1(b) == 1) events |= 0x20000000u;
		b->pos += 2; // margins
		for (int i = 0; i < 20; i++) if (g1(b) == 1) b->pos += 8;
		for (int i = 0; i < 5; i++) {
			if (b->pos < b->size && b->data[b->pos] != 0) events |= 1u << (i + 23);
			c->ops[i] = gstr(b);
		}
		c->opCount = 5;
	}
	if (c->type == COMP_RECT) c->filled = g1(b) == 1;
	if (c->type == COMP_TEXT || c->type == 1) {
		c->halign = (uint8_t)g1(b);
		c->valign = (uint8_t)g1(b);
		c->vpadding = (uint8_t)g1(b);
		c->font = g2(b);
		if (c->font == 65535) c->font = -1;
		c->shadowed = g1(b) == 1;
	}
	if (c->type == COMP_TEXT) {
		c->text = gstr(b);
		skip_str(b); // active text
	}
	if (c->type == 1 || c->type == COMP_RECT || c->type == COMP_TEXT) c->colour = g4(b);
	if (c->type == COMP_RECT || c->type == COMP_TEXT) b->pos += 12;
	if (c->type == COMP_SPRITE) {
		c->spriteId = g4(b);
		g4(b);
	}
	if (c->type == COMP_MODEL) {
		c->modelId = g2(b);
		if (c->modelId == 65535) c->modelId = -1;
		b->pos += 12;
	}
	if (c->type == COMP_INV_TEXT) {
		g1(b);
		c->font = g2(b);
		if (c->font == 65535) c->font = -1;
		c->shadowed = g1(b) == 1;
		c->colour = g4(b);
		b->pos += 4;
		if (g1(b) == 1) events |= 0x40000000u;
		for (int i = 0; i < 5; i++) {
			if (b->pos < b->size && b->data[b->pos] != 0) events |= 1u << (i + 23);
			skip_str(b);
		}
	}
	if (c->type == COMP_TOOLTIP) c->text = gstr(b);
	if (c->buttonType == BUTTON_TARGET || c->type == COMP_INVENTORY) {
		skip_str(b);
		skip_str(b);
		events |= (uint32_t)(g2(b) & 0x3F) << 11;
	}
	if (c->buttonType == BUTTON_OK || c->buttonType == BUTTON_TOGGLE || c->buttonType == BUTTON_SELECT || c->buttonType == BUTTON_CONTINUE) {
		c->option = gstr(b);
		if (c->option && !c->option[0]) {
			free(c->option);
			const char* fallback = c->buttonType == BUTTON_OK ? "Ok" : c->buttonType == BUTTON_CONTINUE ? "Continue" : "Select";
			c->option = malloc(strlen(fallback) + 1);
			if (c->option) strcpy(c->option, fallback);
		}
	}
	if (c->buttonType == BUTTON_OK || c->buttonType == BUTTON_TOGGLE || c->buttonType == BUTTON_SELECT) events |= 0x400000;
	if (c->buttonType == BUTTON_CONTINUE) events |= 1;
	c->events = events;
}

// Stops after the options. What follows is scripts, which are not used yet.
static void decode_if3(IfaceComponent* c, Buf* b) {
	c->if3 = 1;
	b->pos++;
	int type = g1(b);
	if (type & 0x80) {
		type &= 0x7F;
		skip_str(b);
	}
	c->type = (uint8_t)type;
	c->clientCode = g2(b);
	c->x = g2b(b);
	c->y = g2b(b);
	c->width = g2(b);
	c->height = g2(b);
	int dynamicWidth = g1b(b), dynamicHeight = g1b(b);
	c->dynamicWidth = (int8_t)dynamicWidth;
	c->dynamicHeight = (int8_t)dynamicHeight;
	g1b(b);
	g1b(b);
	c->parent = g2(b);
	c->parent = c->parent == 65535 ? -1 : (int)((uint32_t)c->id & 0xFFFF0000u) + c->parent;
	c->hidden = g1(b) == 1;
	if (c->type == COMP_LAYER) {
		c->scrollMaxH = g2(b);
		c->scrollMaxV = g2(b);
		g1(b);
	}
	if (c->type == COMP_SPRITE) {
		c->spriteId = g4(b);
		b->pos += 2 + 1 + 1 + 1 + 4 + 1 + 1;
	}
	if (c->type == COMP_MODEL) {
		c->modelId = g2(b);
		if (c->modelId == 65535) c->modelId = -1;
		b->pos += 2 + 2 + 2 + 2 + 2 + 2 + 2 + 1 + 2 + 2 + 1;
		if (dynamicWidth != 0) g2(b);
		if (dynamicHeight != 0) g2(b);
	}
	if (c->type == COMP_TEXT) {
		c->font = g2(b);
		if (c->font == 65535) c->font = -1;
		c->text = gstr(b);
		c->vpadding = (uint8_t)g1(b);
		c->halign = (uint8_t)g1(b);
		c->valign = (uint8_t)g1(b);
		c->shadowed = g1(b) == 1;
		c->colour = g4(b);
	}
	if (c->type == COMP_RECT) {
		c->colour = g4(b);
		c->filled = g1(b) == 1;
		g1(b);
	}
	if (c->type == COMP_LINE) {
		g1(b);
		c->colour = g4(b);
		g1(b);
	}
	c->events = (uint32_t)g3(b);
	int keys = g1(b);
	while (keys != 0) {
		g1(b);
		g1(b);
		g1(b);
		keys = g1(b);
	}
	c->option = gstr(b);
	int opFlags = g1(b);
	c->opCount = opFlags & 0xF;
	for (int i = 0; i < c->opCount; i++) {
		char* op = gstr(b);
		if (i < IFACE_MAX_OPS) c->ops[i] = op;
		else free(op);
	}
	int dragOps = opFlags >> 4;
	if (c->opCount > IFACE_MAX_OPS) c->opCount = IFACE_MAX_OPS;
	if (dragOps > 0) {
		g1(b);
		g2(b);
	}
	if (dragOps > 1) {
		g1(b);
		g2(b);
	}
	g1(b);
	g1(b);
	g1(b);
	skip_str(b);
	if (c->events >> 11 & 0x7F) {
		g2(b);
		g2(b);
		g2(b);
	}
	for (int handler = 0; handler < 20; handler++) {
		int count = g1(b);
		int32_t* args = count ? malloc(sizeof(int32_t) * (size_t)count) : NULL;
		char** texts = count ? calloc((size_t)count, sizeof(char*)) : NULL;
		for (int i = 0; i < count; i++) {
			int kind = g1(b), value = IFACE_STRING_ARG;
			if (kind == 0) value = g4(b);
			else if (kind == 1) {
				char* text = gstr(b);
				if (texts) texts[i] = text;
				else free(text);
			}
			if (args) args[i] = value;
		}
		if (handler == 0) {
			c->onLoad = args;
			c->onLoadCount = count;
			c->onLoadStrings = texts;
		} else if (handler == 5) {
			c->onVarpChange = args;
			c->onVarpChangeCount = count;
			c->onVarpStrings = texts;
		} else {
			if (handler == 6) {
				c->onInvChange = args;
				c->onInvChangeCount = count;
				c->onInvStrings = texts;
			} else if (handler == 7) {
				c->onStatChange = args;
				c->onStatChangeCount = count;
				c->onStatStrings = texts;
			} else {
				free(args);
				free_texts(texts, count);
			}
		}
	}
	int triggers = g1(b);
	if (triggers) c->varpTriggers = malloc(sizeof(int32_t) * (size_t)triggers);
	c->varpTriggerCount = triggers;
	for (int i = 0; i < triggers; i++) c->varpTriggers[i] = g4(b);
	triggers = g1(b);
	if (triggers) c->invTriggers = malloc(sizeof(int32_t) * (size_t)triggers);
	c->invTriggerCount = triggers;
	for (int i = 0; i < triggers; i++) c->invTriggers[i] = g4(b);
	triggers = g1(b);
	if (triggers) c->statTriggers = malloc(sizeof(int32_t) * (size_t)triggers);
	c->statTriggerCount = triggers;
	for (int i = 0; i < triggers; i++) c->statTriggers[i] = g4(b);
}

int iface_load(Js5Cache* cache, int id, Iface* out) {
	memset(out, 0, sizeof(*out));
	out->id = id;
	if (js5_load_index(cache, ARCHIVE_INTERFACES) != 0) return -1;
	const Js5Index* index = &cache->index[ARCHIVE_INTERFACES];
	if (id < 0 || (uint32_t)id >= index->capacity || !index->groups[id].present) return -2;
	const Js5Group* g = &index->groups[id];
	Js5Buffer group = {0, 0};
	if (js5_read_group(cache, ARCHIVE_INTERFACES, (uint32_t)id, NULL, &group) != 0) return -3;
	int capacity = 0;
	for (uint32_t i = 0; i < g->size; i++) {
		int file = g->fileIds ? (int)g->fileIds[i] : (int)i;
		if (file + 1 > capacity) capacity = file + 1;
	}
	out->components = calloc((size_t)capacity + 1, sizeof(IfaceComponent));
	if (!out->components) {
		free(group.data);
		return -4;
	}
	out->count = capacity;
	for (int i = 0; i < capacity; i++) out->components[i].type = 255;
	for (uint32_t i = 0; i < g->size; i++) {
		uint32_t file = g->fileIds ? g->fileIds[i] : i;
		Js5Buffer data = {0, 0};
		if (js5_group_file(index, (uint32_t)id, &group, file, &data) != 0) continue;
		IfaceComponent* c = &out->components[file];
		memset(c, 0, sizeof(*c));
		c->id = (int)((uint32_t)id << 16 | file);
		c->font = c->spriteId = -1;
		Buf b = {data.data, data.size, 0};
		if (data.size > 0 && data.data[0] == 255) decode_if3(c, &b);
		else decode_if1(c, &b);
		free(data.data);
	}
	free(group.data);
	return 0;
}

void iface_free(Iface* iface) {
	for (int i = 0; i < iface->count; i++) {
		IfaceComponent* c = &iface->components[i];
		free(c->text);
		free(c->option);
		for (int k = 0; k < c->opCount; k++) free(c->ops[k]);
		free(c->onLoad);
		free(c->onVarpChange);
		free(c->varpTriggers);
		free_texts(c->onLoadStrings, c->onLoadCount);
		free_texts(c->onVarpStrings, c->onVarpChangeCount);
		free_texts(c->onInvStrings, c->onInvChangeCount);
		free_texts(c->onStatStrings, c->onStatChangeCount);
		free(c->onInvChange);
		free(c->invTriggers);
		free(c->onStatChange);
		free(c->statTriggers);
	}
	free(iface->components);
	iface->components = NULL;
	iface->count = 0;
}
