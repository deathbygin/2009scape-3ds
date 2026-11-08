// The Summoning tab, interface 662. The server only opens it while there is a follower, and its scripts work out
// the name, the timer and which special move this follower has. Drawing is native. Every button is one press
// and sends one packet, and a special move that needs a target goes through the same aiming as a spell.

#define SUMMON_INTERFACE 662
#define SUMMON_NAME 55
#define SUMMON_POINTS 43
#define SUMMON_TIMER 45
#define SUMMON_GROWTH 47
#define SUMMON_HUNGER 49
#define SUMMON_CALL 51
#define SUMMON_DISMISS 53
#define SUMMON_SCROLLS 61
#define SUMMON_PET_GROUP 62
#define SUMMON_FAMILIAR_GROUP 63
#define SUMMON_TAKE_BOB 67
#define SUMMON_NO_SPECIAL 47 // the enum's answer for a follower with no special move
#define VARP_FOLLOWER 1174
#define ENUM_SPECIAL_GROUP 1282
#define SUMMON_BUTTON_H 24
#define SUMMON_BUTTON_GAP 3
#define SUMMON_BUTTONS 5
#define SUMMON_TOP (CONTENT_TOP + 75)
#define SUMMON_HEAD_SIZE 72 // the same as HEAD_SIZE, which comes later
#define SUMMON_TEXT_X 84

typedef struct {
	const char* label;
	int component;
	bool second, special; // second is the button's second option
} SummonButton;

static TabScript summonScript = {SUMMON_INTERFACE};
static char summonSpecialName[40];
static u32 summonHead[SUMMON_HEAD_SIZE * SUMMON_HEAD_SIZE];
static int summonHeadFor = -1;
static bool summonHeadReady;

static bool summonOpen(void) {
	return net_interface_is_open(&net, SUMMON_INTERFACE);
}

static const IfaceComponent* summonPart(int child) {
	return cs2_component(SUMMON_INTERFACE << 16 | child);
}

static const char* summonText(int child) {
	const IfaceComponent* c = summonPart(child);
	return c && c->text ? c->text : "";
}

static bool summonIsPet(void) {
	const IfaceComponent* c = summonPart(SUMMON_PET_GROUP);
	return c && !c->hidden;
}

// The follower's special move button, or -1. An enum names the group it sits in, as script 751 reads it. Earlier
// followers leave their own groups showing, so the groups cannot be told apart by what is hidden.
static int summonSpecial(void) {
	int group = cs2_enum_int(ENUM_SPECIAL_GROUP, net.varps[VARP_FOLLOWER]);
	Iface* f = cs2_interface(SUMMON_INTERFACE);
	if (!f || summonIsPet() || group == (SUMMON_INTERFACE << 16 | SUMMON_NO_SPECIAL)) return -1;
	for (int i = 0; i < f->count; i++) {
		const IfaceComponent* c = &f->components[i];
		if (c->type == COMP_SPRITE && c->parent == group && !c->hidden) return i;
	}
	return -1;
}

static int summonButtons(SummonButton* out) {
	int count = 0, special = summonSpecial();
	if (special >= 0) {
		const IfaceComponent* c = summonPart(special);
		plainText(c->option && c->option[0] ? c->option : "Special Move", summonSpecialName, sizeof(summonSpecialName));
		out[count++] = (SummonButton){summonSpecialName, special, false, true};
	}
	out[count++] = (SummonButton){"Call Follower", SUMMON_CALL, false, false};
	if (!summonIsPet()) out[count++] = (SummonButton){"Take BoB", SUMMON_TAKE_BOB, false, false};
	out[count++] = (SummonButton){"Dismiss", SUMMON_DISMISS, false, false};
	out[count++] = (SummonButton){"Dismiss Now", SUMMON_DISMISS, true, false};
	return count;
}

// The special move takes a whole row, and the rest sit two to a row.
static void summonPlace(const SummonButton* buttons, int index, int width, int* x, int* y, int* w) {
	int left = 12, full = width - 24, half = (full - 6) / 2, row = 0, plain = index;
	if (buttons[0].special) {
		if (index == 0) {
			*x = left;
			*y = SUMMON_TOP;
			*w = full;
			return;
		}
		row = 1;
		plain = index - 1;
	}
	*x = left + (plain % 2) * (half + 6);
	*y = SUMMON_TOP + (row + plain / 2) * (SUMMON_BUTTON_H + SUMMON_BUTTON_GAP);
	*w = half;
}

static int summonButtonAt(int px, int py, int width) {
	if (panelTab != TAB_SUMMONING || !summonOpen() || !mapVisible() || dialogActive() || journalActive()) return -1;
	SummonButton buttons[SUMMON_BUTTONS];
	int count = summonButtons(buttons);
	for (int i = 0; i < count; i++) {
		int x, y, w;
		summonPlace(buttons, i, width, &x, &y, &w);
		if (px >= x && py >= y && px < x + w && py < y + SUMMON_BUTTON_H) return i;
	}
	return -1;
}

static void pressSummon(int index) {
	SummonButton buttons[SUMMON_BUTTONS];
	summonButtons(buttons);
	const SummonButton* b = &buttons[index];
	const IfaceComponent* c = summonPart(b->component);
	int mask = (int)(c->events >> 11 & 0x7F);
	panelDirty = true;
	if (b->special && !(c->events >> 1 & 1) && mask != 0) {
		// It needs a target, so this press only picks it up, and a second press on it puts it down.
		if (castComponent == b->component && castInterface == SUMMON_INTERFACE) {
			cancelCast();
			return;
		}
		castInterface = SUMMON_INTERFACE;
		castComponent = b->component;
		castMask = mask;
		snprintf(castName, sizeof(castName), "%s", b->label);
		if ((mask & 0x10) && !(mask & 0x0A)) panelTab = TAB_BAG;
		return;
	}
	if (castComponent >= 0) cancelCast();
	if (b->second) net_button_second(&net, SUMMON_INTERFACE, b->component);
	else net_button(&net, SUMMON_INTERFACE, b->component);
	actionsSent++;
}

// Runs every frame. The server closes the tab when the follower goes, and a new one needs its scripts run again.
static void updateSummon(void) {
	if (!summonOpen()) {
		if (summonScript.loaded) panelDirty = true;
		summonScript.loaded = false;
		if (castComponent >= 0 && castInterface == SUMMON_INTERFACE) cancelCast();
		summonHeadFor = -1;
		summonHeadReady = false;
		return;
	}
	if (refreshTabScript(&summonScript)) panelDirty = true;
	// Drawn only when first needed. It stays still, because a moving head would redraw the panel all the time.
	int follower = net.varps[VARP_FOLLOWER];
	if (panelTab == TAB_SUMMONING && mapVisible() && follower >= 0 && follower != summonHeadFor) {
		u32 start = (u32)nowMs();
		summonHeadFor = follower;
		summonHeadReady = renderNpcHead(follower, summonHead);
		char line[64];
		snprintf(line, sizeof(line), "summon head npc=%d ok=%d ms=%d", follower, summonHeadReady, (int)((u32)nowMs() - start));
		logLine(line);
		panelDirty = true;
	}
}

static void drawSummonValue(u8* fb, int x, int y, int width, const char* name, const char* value) {
	char line[40];
	char plain[24];
	plainText(value, plain, sizeof(plain));
	snprintf(line, sizeof(line), "%s %s", name, plain);
	drawText(fb, width, x, y, line, 255, 255, 255);
}

static void drawSummon(u8* fb, int width) {
	if (!summonOpen()) {
		const char* text = "You have no follower";
		drawText(fb, width, (width - (int)strlen(text) * 8) / 2, 116, text, 200, 190, 170);
		return;
	}
	char name[32];
	plainText(summonText(SUMMON_NAME), name, sizeof(name));
	if (summonHeadReady) {
		for (int y = 0; y < SUMMON_HEAD_SIZE; y++) {
			for (int x = 0; x < SUMMON_HEAD_SIZE; x++) {
				u32 p = summonHead[y * SUMMON_HEAD_SIZE + x];
				if (p) putPixel(fb, width, 8 + x, CONTENT_TOP + 1 + y, (u8)(p >> 16), (u8)(p >> 8), (u8)p);
			}
		}
	}
	drawText(fb, width, SUMMON_TEXT_X, CONTENT_TOP + 6, name, 255, 152, 31);
	int y = CONTENT_TOP + 22;
	if (summonIsPet()) {
		drawSummonValue(fb, SUMMON_TEXT_X, y, width, "Hunger", summonText(SUMMON_HUNGER));
		drawSummonValue(fb, SUMMON_TEXT_X, y + 13, width, "Growth", summonText(SUMMON_GROWTH));
	} else {
		drawSummonValue(fb, SUMMON_TEXT_X, y, width, "Time", summonText(SUMMON_TIMER));
		drawSummonValue(fb, SUMMON_TEXT_X, y + 13, width, "Points", summonText(SUMMON_POINTS));
		drawSummonValue(fb, SUMMON_TEXT_X, y + 26, width, "Scrolls", summonText(SUMMON_SCROLLS));
	}
	SummonButton buttons[SUMMON_BUTTONS];
	int count = summonButtons(buttons);
	for (int i = 0; i < count; i++) {
		const SummonButton* b = &buttons[i];
		int x, by, w;
		summonPlace(buttons, i, width, &x, &by, &w);
		bool lit = b->special && castComponent == b->component && castInterface == SUMMON_INTERFACE;
		fillRect(fb, width, x, by, w, SUMMON_BUTTON_H, 110, 90, 60);
		fillRect(fb, width, x + 2, by + 2, w - 4, SUMMON_BUTTON_H - 4, lit ? 120 : 84, lit ? 60 : 68, lit ? 40 : 46);
		drawText(fb, width, x + (w - (int)strlen(b->label) * 8) / 2, by + 8, b->label, 255, 255, 255);
	}
}
