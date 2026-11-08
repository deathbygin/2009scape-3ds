// Draws the action menu as large rows on the touch screen.

static void menuPresent(void) {
	u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	const int width = 320;
	fillRect(fb, width, 0, 0, width, SCREEN_H, 40, 34, 26);
	for (int i = 0; i < menuPageSize(); i++) {
		int index = menuPage * menuPageSize() + i, y = MENU_TOP + i * MENU_ROW_HEIGHT;
		if (index >= menuCount) break;
		bool selected = index == menuSelected;
		fillRect(fb, width, 6, y, width - 12, MENU_ROW_HEIGHT - 3, selected ? 120 : 84, selected ? 96 : 68, selected ? 50 : 46);
		const MenuEntry* e = &menuEntries[index];
		drawText(fb, width, 14, y + 9, e->label, e->kind == ENTRY_CANCEL ? 255 : 255, e->kind == ENTRY_CANCEL ? 170 : 255, e->kind == ENTRY_CANCEL ? 120 : 255);
	}
	if (menuCount > MENU_VISIBLE) {
		char more[32];
		int y = MENU_TOP + (MENU_VISIBLE - 1) * MENU_ROW_HEIGHT;
		snprintf(more, sizeof(more), "More (%d/%d)", menuPage + 1, menuPageCount());
		fillRect(fb, width, 6, y, width - 12, MENU_ROW_HEIGHT - 3, 60, 50, 36);
		drawText(fb, width, 14, y + 9, more, 255, 200, 60);
	}
	GSPGPU_FlushDataCache(fb, (u32)(width * SCREEN_H * 3));
	gfxScreenSwapBuffers(GFX_BOTTOM, false);
}
