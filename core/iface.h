// Interface definitions from the cache, ported from the revision 530 client's Component decoder.
// Scripts are not kept yet: only what is needed to show a component and press it.
#ifndef IFACE_H
#define IFACE_H

#include "js5.h"

enum { COMP_LAYER = 0, COMP_INVENTORY = 2, COMP_RECT = 3, COMP_TEXT = 4, COMP_SPRITE = 5, COMP_MODEL = 6, COMP_INV_TEXT = 7, COMP_TOOLTIP = 8, COMP_LINE = 9 };
enum { BUTTON_NONE = 0, BUTTON_OK = 1, BUTTON_TARGET = 2, BUTTON_CLOSE = 3, BUTTON_TOGGLE = 4, BUTTON_SELECT = 5, BUTTON_CONTINUE = 6 };

#define IFACE_MAX_OPS 10
#define IFACE_STRING_ARG 0x7FFFFFFE // stands in for a text argument in event data

typedef struct {
	int id; // interface << 16 | component
	uint8_t if3, type, buttonType, hidden, filled, shadowed, halign, valign, vpadding;
	int clientCode, x, y, width, height, parent; // parent is -1 for none
	int8_t dynamicWidth, dynamicHeight; // how width and height relate to the parent
	int itemPlusOne, itemAmount; // an item a script put here, one more than its id so that 0 means none
	int scrollX, scrollY, scrollMaxH, scrollMaxV;
	int font, colour, spriteId, modelId;
	uint32_t events; // what pressing it does: bit 0 continues a dialogue, bits 1 to 10 enable ops
	char* text;
	char* option;
	char* ops[IFACE_MAX_OPS];
	int opCount;
	int32_t* onLoad;        // script id then its number arguments, NULL if none
	int onLoadCount;
	int32_t* onVarpChange;  // the same, run when a watched variable changes
	int onVarpChangeCount;
	int32_t* varpTriggers;  // the variables watched
	int varpTriggerCount;
	char** onLoadStrings;   // text arguments, at the same places, or NULL
	char** onVarpStrings;
	char** onInvStrings;
	char** onStatStrings;
	int32_t* onInvChange;   // the same, run when a watched container changes
	int onInvChangeCount;
	int32_t* invTriggers;
	int invTriggerCount;
	int32_t* onStatChange;  // the same, run when a watched skill changes
	int onStatChangeCount;
	int32_t* statTriggers;  // the skills watched
	int statTriggerCount;
	// Old interfaces test a variable to light a button. Only the simple form buttons use is kept.
	int8_t testKind;        // how to compare, 0 for no test
	int testValue, testVarp; // testVarp is -1 if the test is not on a variable
} IfaceComponent;

typedef struct {
	int id, count;
	IfaceComponent* components; // indexed by component number; type 255 marks a missing one
} Iface;

// Returns 0 on success.
int iface_load(Js5Cache* cache, int id, Iface* out);
void iface_free(Iface* iface);

#endif
