// The stats tab: every skill with its level. The original's three columns are too tall for this screen,
// so the same reading order runs across six columns instead.

#define SKILL_COUNT 24
#define STATS_COLUMNS 6
#define STATS_CELL_W 52
#define STATS_CELL_H 38

typedef struct {
	int skill, icon;
	const char* name;
} SkillPlace;

// In the order the original lists them, reading across its rows. Icons are from interface 320.
static const SkillPlace SKILL_PLACES[SKILL_COUNT] = {
	{0, 197, "Attack"}, {3, 203, "Hitpoints"}, {14, 209, "Mining"},
	{2, 198, "Strength"}, {16, 204, "Agility"}, {13, 210, "Smithing"},
	{1, 199, "Defence"}, {15, 205, "Herblore"}, {10, 211, "Fishing"},
	{4, 200, "Ranged"}, {17, 206, "Thieving"}, {7, 212, "Cooking"},
	{5, 201, "Prayer"}, {12, 207, "Crafting"}, {11, 213, "Firemaking"},
	{6, 202, "Magic"}, {9, 208, "Fletching"}, {8, 214, "Woodcutting"},
	{20, 215, "Runecrafting"}, {18, 216, "Slayer"}, {19, 217, "Farming"},
	{22, 221, "Construction"}, {21, 220, "Hunter"}, {23, 222, "Summoning"},
};

// The button of each skill on interface 320, by skill number.
static const short SKILL_BUTTONS[SKILL_COUNT] = {125, 127, 126, 133, 128, 129, 130, 144, 146, 138, 143, 145, 137, 142, 141, 135, 134, 136, 139, 147, 131, 140, 132, 148};

static Sprite skillIcons[SKILL_COUNT];
static int skillXpTable[99];

static void loadStatsTab(void) {
	for (int i = 0; i < SKILL_COUNT; i++) sprite_load(&world.cache, SKILL_PLACES[i].icon, &skillIcons[i]);
	// PlayerSkillXpTable: the experience needed for each level.
	int total = 0;
	for (int level = 0; level < 99; level++) {
		int next = level + 1;
		total += (int)(pow(2.0, next / 7.0) * 300.0 + next);
		skillXpTable[level] = total / 4;
	}
}

// The level the experience has earned, not counting boosts.
static int baseLevel(int skill) {
	int level = 1;
	for (int i = 0; i < 98; i++) if (skillXpTable[i] <= net.skillXp[skill]) level = i + 2;
	return level;
}

static int totalLevel(void) {
	int total = 0;
	for (int i = 0; i < SKILL_COUNT; i++) total += baseLevel(i);
	return total;
}

static void statsCell(int place, int width, int* x, int* y) {
	*x = (width - STATS_COLUMNS * STATS_CELL_W) / 2 + (place % STATS_COLUMNS) * STATS_CELL_W;
	*y = CONTENT_TOP + 1 + (place / STATS_COLUMNS) * STATS_CELL_H;
}

static int statsPlaceAt(int px, int py, int width) {
	if (panelTab != TAB_STATS || !mapVisible() || dialogActive()) return -1;
	for (int i = 0; i < SKILL_COUNT; i++) {
		int x, y;
		statsCell(i, width, &x, &y);
		if (px >= x && py >= y && px < x + STATS_CELL_W && py < y + STATS_CELL_H) return i;
	}
	return -1;
}

static void drawStats(u8* fb, int width) {
	for (int i = 0; i < SKILL_COUNT; i++) {
		int x, y, skill = SKILL_PLACES[i].skill;
		statsCell(i, width, &x, &y);
		fillRect(fb, width, x + 1, y + 1, STATS_CELL_W - 2, STATS_CELL_H - 2, 74, 63, 48);
		drawSprite(fb, width, &skillIcons[i], x + (STATS_CELL_W - 25) / 2, y + 1);
		char text[12];
		snprintf(text, sizeof(text), "%d/%d", net.skillLevel[skill], baseLevel(skill));
		drawText(fb, width, x + (STATS_CELL_W - (int)strlen(text) * 8) / 2, y + 28, text, 255, 255, 0);
	}
	char line[64];
	snprintf(line, sizeof(line), "Total level: %d", totalLevel());
	drawText(fb, width, (width - (int)strlen(line) * 8) / 2, CONTENT_BOTTOM - 10, line, 255, 255, 255);
}

// Opens the guide for a skill, which the server decides: right after a level-up it shows that page instead.
static void pressStats(int place) {
	int skill = SKILL_PLACES[place].skill;
	net_button(&net, 320, SKILL_BUTTONS[skill]);
	actionsSent++;
	guideSkill = skill;
}
