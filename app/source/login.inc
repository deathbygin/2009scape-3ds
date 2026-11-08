// The login screen, built from the original's title art in the cache: the scene and logo on the top screen,
// and the stone login box on the touch screen. Logging in is always a press, and nothing here retries by itself.

#define TITLE_SCALE_H 240
#define LOGIN_FIELD_X 96
#define LOGIN_FIELD_W 200
#define LOGIN_FIELD_H 20
#define LOGIN_USER_Y 42
#define LOGIN_PASS_Y 70
#define LOGIN_SERVER_Y 98
#define LOGIN_BUTTON_Y 186
#define LOGIN_BUTTON_W 132
#define LOGIN_BUTTON_H 38
#define LOGIN_BUTTON_X 20
#define EXIT_BUTTON_X 168

enum { LOGIN_STAY, LOGIN_DONE, LOGIN_QUIT };

static Sprite titleBg, titleLogo, titleBox, titleButton;
static bool titleLoaded;
static char loginMessage[64];

static void titleFree(void) {
	if (!titleLoaded) return;
	sprite_free(&titleBg);
	sprite_free(&titleLogo);
	sprite_free(&titleBox);
	sprite_free(&titleButton);
	titleLoaded = false;
}

static void titleSprite(const char* name, Sprite* out) {
	memset(out, 0, sizeof(*out));
	if (js5_load_index(&world.cache, 8) != 0) return;
	int group = js5_find_group(&world.cache.index[8], name);
	if (group >= 0) sprite_load(&world.cache, group, out);
}

static void titleLoad(void) {
	if (titleLoaded) return;
	titleSprite("titlebg", &titleBg);
	titleSprite("logo", &titleLogo);
	titleSprite("titlebox", &titleBox);
	titleSprite("titlebutton", &titleButton);
	titleLoaded = true;
}

// Draws a sprite at another size, averaging neighbours so the shrunken art stays smooth.
static void drawScaled(u8* fb, int width, const Sprite* s, int dx, int dy, int dw, int dh) {
	if (!s->pixels) return;
	int w = s->innerWidth, h = s->innerHeight;
	for (int y = 0; y < dh; y++) {
		int screenY = dy + y;
		int sy = y * s->height / dh - s->yOffset;
		if (screenY < 0 || screenY >= SCREEN_H || sy < 0 || sy >= h) continue;
		int below = sy + 1 < h ? sy + 1 : sy;
		for (int x = 0; x < dw; x++) {
			int screenX = dx + x;
			int sx = x * s->width / dw - s->xOffset;
			if (screenX < 0 || screenX >= width || sx < 0 || sx >= w) continue;
			int next = sx + 1 < w ? sx + 1 : sx;
			u32 a = s->pixels[sy * w + sx], b = s->pixels[sy * w + next], c = s->pixels[below * w + sx], d = s->pixels[below * w + next];
			if ((a >> 24) < 128) continue;
			if ((b >> 24) < 128) b = a;
			if ((c >> 24) < 128) c = a;
			if ((d >> 24) < 128) d = a;
			putPixel(fb, width, screenX, screenY,
				(u8)((((a >> 16) & 255) + ((b >> 16) & 255) + ((c >> 16) & 255) + ((d >> 16) & 255)) >> 2),
				(u8)((((a >> 8) & 255) + ((b >> 8) & 255) + ((c >> 8) & 255) + ((d >> 8) & 255)) >> 2),
				(u8)(((a & 255) + (b & 255) + (c & 255) + (d & 255)) >> 2));
		}
	}
}

static void drawFontText(u8* fb, int width, const GameFont* f, const char* text, int x, int top, u8 r, u8 g, u8 b) {
	for (int pass = 0; pass < 2; pass++) {
		int pen = x + (pass == 0 ? 1 : 0), y = top + (pass == 0 ? 1 : 0);
		for (const char* at = text; *at; at++) {
			u8 c = (u8)*at;
			for (u32 i = f->first[c]; i < f->first[c + 1]; i++) {
				const FontRun* run = &f->runs[i];
				for (int k = 0; k < run->length; k++) {
					int px = pen + run->x + k, py = y + run->y;
					if (px >= 0 && px < width && py >= 0 && py < SCREEN_H) putPixel(fb, width, px, py, pass ? r : 0, pass ? g : 0, pass ? b : 0);
				}
			}
			pen += f->advance[c];
		}
	}
}

// Without the shadow, for dark text on parchment.
static void drawPlainFont(u8* fb, int width, const GameFont* f, const char* text, int x, int top, u8 r, u8 g, u8 b) {
	for (const char* at = text; *at; at++) {
		u8 c = (u8)*at;
		for (u32 i = f->first[c]; i < f->first[c + 1]; i++) {
			const FontRun* run = &f->runs[i];
			int py = top + run->y;
			for (int k = 0; k < run->length; k++) {
				int px = x + run->x + k;
				if (px >= 0 && px < width && py >= 0 && py < SCREEN_H) putPixel(fb, width, px, py, r, g, b);
			}
		}
		x += f->advance[c];
	}
}

static void drawCentred(u8* fb, int width, const GameFont* f, const char* text, int centre, int top, u8 r, u8 g, u8 b) {
	drawFontText(fb, width, f, text, centre - font_width(f, text) / 2, top, r, g, b);
}

static void drawLoginField(u8* fb, const char* label, const char* value, int y) {
	drawFontText(fb, 320, &overheadFont, label, 24, y + 3, 255, 255, 0);
	fillRect(fb, 320, LOGIN_FIELD_X, y, LOGIN_FIELD_W, LOGIN_FIELD_H, 30, 25, 18);
	fillRect(fb, 320, LOGIN_FIELD_X + 1, y + 1, LOGIN_FIELD_W - 2, LOGIN_FIELD_H - 2, 62, 53, 41);
	drawFontText(fb, 320, &overheadFont, value, LOGIN_FIELD_X + 5, y + 3, 255, 255, 255);
}

static void drawLoginButton(u8* fb, const char* label, int x) {
	drawScaled(fb, 320, &titleButton, x, LOGIN_BUTTON_Y, LOGIN_BUTTON_W, LOGIN_BUTTON_H);
	drawCentred(fb, 320, &overheadFont, label, x + LOGIN_BUTTON_W / 2, LOGIN_BUTTON_Y + 12, 255, 255, 255);
}

static void loginDrawTop(void) {
	// Both of the screen's buffers get it, since nothing else draws here until the game starts.
	for (int i = 0; i < 2; i++) {
		u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
		int scaled = titleBg.width * TITLE_SCALE_H / (titleBg.height ? titleBg.height : 1);
		fillRect(fb, 400, 0, 0, 400, SCREEN_H, 0, 0, 0);
		drawScaled(fb, 400, &titleBg, (400 - scaled) / 2, 0, scaled, TITLE_SCALE_H);
		drawScaled(fb, 400, &titleLogo, 56, 8, 288, 98);
		GSPGPU_FlushDataCache(fb, 400 * SCREEN_H * 3);
		gfxScreenSwapBuffers(GFX_TOP, false);
		gspWaitForVBlank();
	}
}

static void loginDrawBottom(void) {
	char stars[sizeof(password)];
	size_t length = strlen(password);
	memset(stars, '*', length);
	stars[length] = 0;
	int scaled = titleBg.width * TITLE_SCALE_H / (titleBg.height ? titleBg.height : 1);
	for (int i = 0; i < 2; i++) {
		u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
		fillRect(fb, 320, 0, 0, 320, SCREEN_H, 0, 0, 0);
		drawScaled(fb, 320, &titleBg, (320 - scaled) / 2, 0, scaled, TITLE_SCALE_H);
		drawScaled(fb, 320, &titleBox, 8, 6, 304, 172);
		drawCentred(fb, 320, &overheadFont, loginMessage[0] ? loginMessage : "Welcome to 2009scape", 160, 18, 255, 255, 0);
		drawLoginField(fb, "Username:", username, LOGIN_USER_Y);
		drawLoginField(fb, "Password:", stars, LOGIN_PASS_Y);
		drawLoginField(fb, "Server:", serverHost[0] ? serverHost : "Tap to enter", LOGIN_SERVER_Y);
		drawCentred(fb, 320, &smallFont, "Tap a box to type in it", 160, 128, 200, 190, 170);
		drawLoginButton(fb, "Log In", LOGIN_BUTTON_X);
		drawLoginButton(fb, "Exit", EXIT_BUTTON_X);
		GSPGPU_FlushDataCache(fb, 320 * SCREEN_H * 3);
		gfxScreenSwapBuffers(GFX_BOTTOM, false);
		gspWaitForVBlank();
	}
}

static void loginSay(const char* text) {
	snprintf(loginMessage, sizeof(loginMessage), "%s", text);
	loginDrawBottom();
}

static const char* loginReplyText(int reply) {
	switch (reply) {
	case 3: return "Invalid username or password.";
	case 4: return "Your account has been disabled.";
	case 5: return "That account is already logged in.";
	case 6: return "The game has been updated.";
	case 7: return "This world is full.";
	case 8: return "Login server offline.";
	case 9: return "Too many connections from here.";
	case 11: return "Login rejected. Try again.";
	case 13: return "Could not complete login.";
	case 14: return "The server is being updated.";
	case 16: return "Too many attempts. Wait a while.";
	case 17: return "You are in a members-only area.";
	case 18: return "Your account has been locked.";
	case 24: return "Could not load your profile.";
	case 26: return "This address has been blocked.";
	default: return "Could not reach the server.";
	}
}

static void saveServerFile(void) {
	FILE* f = fopen(SERVER_FILE, "w");
	if (!f) return;
	fprintf(f, "%s %d %s\n", serverHost, serverPort, username);
	fclose(f);
}

static bool loginAsk(const char* hint, char* out, size_t size, bool secret) {
	if (testKeyboardText) return askForText(hint, out, size, (int)size - 1);
	return askText(hint, out, size, secret);
}

// One deliberate press of Log In: a single attempt, and whatever happens is shown.
static int loginAttempt(bool* sceneReady) {
	if (!username[0] || !password[0]) {
		loginSay("Enter your username and password.");
		return LOGIN_STAY;
	}
	if (!serverHost[0]) {
		loginSay("Enter a server first.");
		return LOGIN_STAY;
	}
	if (connectJob.size && !connectJob.finished) {
		loginSay("Still waiting on the last attempt.");
		return LOGIN_STAY;
	}
	loginSay("Connecting...");
	if (!goOnline()) {
		loginSay(loginReplyText(net.reply));
		return LOGIN_STAY;
	}
	saveServerFile();
	loginShowing = false;
	titleFree();
	loginMessage[0] = 0;
	*sceneReady = loadFromServer();
	return LOGIN_DONE;
}

static int loginPress(int x, int y, bool* sceneReady) {
	if (y >= LOGIN_BUTTON_Y && y < LOGIN_BUTTON_Y + LOGIN_BUTTON_H) {
		if (x >= LOGIN_BUTTON_X && x < LOGIN_BUTTON_X + LOGIN_BUTTON_W) return loginAttempt(sceneReady);
		if (x >= EXIT_BUTTON_X && x < EXIT_BUTTON_X + LOGIN_BUTTON_W) return LOGIN_QUIT;
		return LOGIN_STAY;
	}
	if (x < LOGIN_FIELD_X || x >= LOGIN_FIELD_X + LOGIN_FIELD_W) return LOGIN_STAY;
	if (y >= LOGIN_USER_Y && y < LOGIN_USER_Y + LOGIN_FIELD_H) loginAsk("Username", username, sizeof(username), false);
	else if (y >= LOGIN_PASS_Y && y < LOGIN_PASS_Y + LOGIN_FIELD_H) loginAsk("Password", password, sizeof(password), true);
	else if (y >= LOGIN_SERVER_Y && y < LOGIN_SERVER_Y + LOGIN_FIELD_H) loginAsk("Server address", serverHost, sizeof(serverHost), false);
	else return LOGIN_STAY;
	loginDrawTop(); // the keyboard takes over both screens
	loginDrawBottom();
	return LOGIN_STAY;
}

static void loginOpen(const char* message) {
	titleLoad();
	loadFonts();
	loginShowing = true;
	snprintf(loginMessage, sizeof(loginMessage), "%s", message);
	loginDrawTop();
	loginDrawBottom();
}

// Returns true once logged in, and false if the player chose to leave.
static bool loginScreen(const char* message, bool* sceneReady) {
	loginOpen(message);
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START) return false;
		if (down & KEY_TOUCH) {
			touchPosition touch;
			hidTouchRead(&touch);
			int result = loginPress(touch.px, touch.py, sceneReady);
			if (result != LOGIN_STAY) return result == LOGIN_DONE;
		}
		gspWaitForVBlank();
	}
	return false;
}

// Puts everything back as it was before logging in, ready for another session.
static void endSession(void) {
	if (net.sock > 0) net_close(&net);
	online = false;
	if (menuOpen) closeMenu();
	usingSlot = usingItem = -1;
	castComponent = -1;
	bagPressSlot = -1;
	touchPending = false;
	listTouchCancel();
	panelTab = TAB_MAP;
	if (dialogLoaded) {
		iface_free(&dialogIface);
		dialogLoaded = false;
	}
	dialogRowCount = 0;
	dialogVersion = -1;
	bagVersion = -1;
	groundSeen = -1;
	statsSeen = -1;
	journalOpen = false;
	journalVersion = -1;
	journalRowCount = 0;
	walkFlagX = walkFlagY = -1;
	logoutPressed = false;
	resetGuide();
	resetStore();
	resetHead();
}
