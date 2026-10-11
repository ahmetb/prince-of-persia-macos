/*
Prince of Persia — native save/load command dispatcher and in-game slot UI.
Copyright (C) 2026  The Prince of Persia macOS port authors.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "common.h"
#include "macos_app.h"
#include "game_snapshot.h"
#include "game_commands.h"

// Enter the loaded frame-boundary continuation directly (title load bypass).
void play_level_continue(void);

#define GAME_SLOTS 10

static struct {
	GameCommandKind kind;
	bool has_pending;
	bool busy;
	bool native_panel_active;
	uint64_t generation;
	int suppress_scancode;
} g = { .suppress_scancode = -1 };

static bool replay_locked(void) {
#ifdef USE_REPLAY
	return (replaying || recording) != 0;
#else
	return false;
#endif
}

bool game_request_command(GameCommandKind kind) {
	if (g.busy || g.has_pending) return false;
	if (replay_locked()) return false; // Save/Load disabled during replay sessions.
	g.kind = kind;
	g.has_pending = true;
	return true;
}

static void report_error(const char *what, SaveError *error) {
	char message[512];
	snprintf(message, sizeof(message), "%s: %s", what,
	         (error != NULL && error->message[0]) ? error->message : "unknown error");
	macos_present_error("Save/Load", message);
}

static char *path_for_quick_slot(void) {
	static char quick_path[PATH_MAX];
	if (macos_copy_path(MACOS_SAVES, "Quicksave.popsave", quick_path, sizeof(quick_path))) {
		return quick_path;
	}
	return NULL;
}

static void slot_path(int index, char *out, size_t capacity) {
	char name[32];
	snprintf(name, sizeof(name), "slot-%d.popsave", index + 1);
	macos_copy_path(MACOS_SAVES, name, out, capacity);
}

static bool slot_occupied(int index) {
	char path[PATH_MAX];
	slot_path(index, path, sizeof(path));
	return file_exists(path);
}

static void slot_label(int index, char *out, size_t capacity) {
	char path[PATH_MAX];
	slot_path(index, path, sizeof(path));
	FILE *f = fopen(path, "rb");
	if (f == NULL) { snprintf(out, capacity, "[EMPTY]"); return; }
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); snprintf(out, capacity, "[unreadable]"); return; }
	long sz = ftell(f);
	rewind(f);
	if (sz < 84 || sz > (long)POP_SAVE_MAX_DOCUMENT) {
		fclose(f); snprintf(out, capacity, "[invalid]"); return;
	}
	uint8_t *buf = malloc((size_t)sz);
	if (buf == NULL || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf); fclose(f); snprintf(out, capacity, "[unreadable]"); return;
	}
	fclose(f);
	GameSnapshot snap;
	SaveError err;
	bool ok = (game_snapshot_decode(buf, (size_t)sz, &snap, &err) == SAVE_OK);
	free(buf);
	if (!ok) { snprintf(out, capacity, "[invalid]"); return; }
	int level = (int)snap.base.current_level;
	int minutes = (int)snap.base.rem_min;
	int seconds = (int)snap.base.rem_tick / 12;
	if (minutes < 0) minutes = 0;
	snprintf(out, capacity, "L%d  %02d:%02d", level, minutes, seconds);
}

// In-game slot picker: rendered with the engine's own text primitives on the
// frozen game frame, and driven by the game's own event pump / keyboard.
// No AppKit window is used, so focus never leaves the game.
//
// Returns the chosen slot index (0-based), or -1 on cancel.
static int slot_picker(bool is_save) {
	static byte *backup = NULL;
	if (backup == NULL) {
		backup = malloc(POP_FRAME_RGB_BYTES);
	}
	// Preserve the frozen frame so we can restore it on exit.
	SDL_Surface *surface = get_final_surface();
	if (backup != NULL && surface != NULL) {
		const byte *pix = (const byte *)surface->pixels;
		for (int row = 0; row < POP_FRAME_H; ++row) {
			memcpy(backup + row * POP_FRAME_W * 3,
			       pix + (size_t)row * (size_t)surface->pitch, POP_FRAME_W * 3);
		}
	}

	font_type *saved_font = textstate.ptr_font;
	textstate.ptr_font = &hc_small_font;

	int sel = 0;
	int choice = -1;
	while (choice < 0) {
		// Draw the dialog over the frozen frame.
		const rect_type border = {26, 36, 176, 284};
		const rect_type shadow = {30, 40, 180, 288};
		rect_type black = {border.top + 1, border.left + 1, border.bottom - 1, border.right - 1};
		rect_type title = {border.top + 2, border.left + 4, border.top + 12, border.right - 4};
		rect_type hint = {border.bottom - 11, border.left + 4, border.bottom - 2, border.right - 4};

		draw_rect(&shadow, color_8_darkgray);
		draw_rect(&border, color_15_brightwhite);
		draw_rect(&black, color_0_black);
		show_text_with_color(&title, halign_center, valign_top,
		                     is_save ? "SAVE GAME" : "LOAD GAME", color_15_brightwhite);

		for (int i = 0; i < GAME_SLOTS; ++i) {
			rect_type row = {border.top + 15 + i * 9, border.left + 5,
			                 border.top + 15 + i * 9 + 8, border.right - 5};
			char label[48];
			char full[64];
			slot_label(i, label, sizeof(label));
			snprintf(full, sizeof(full), "%d  %s", i + 1, label);
			if (i == sel) {
				draw_rect(&row, color_15_brightwhite);
				show_text_with_color(&row, halign_left, valign_top, full, color_0_black);
			} else {
				show_text_with_color(&row, halign_left, valign_top, full, color_15_brightwhite);
			}
		}
		show_text_with_color(&hint, halign_center, valign_top,
		                     "1-0/UP/DOWN + ENTER    ESC: CANCEL", color_8_darkgray);
		update_screen();

		process_events();
		int key = read_key();
		if (key == 0) {
			SDL_Delay(10);
			continue;
		}
		int sc = key & ~0xE000; // strip WITH_SHIFT/WITH_CTRL/WITH_ALT flags

		if (key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_BACKSPACE) {
			choice = -1;
			break;
		}
		if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
			choice = sel;
			break;
		}
		if (sc == SDL_SCANCODE_UP) { sel = (sel + GAME_SLOTS - 1) % GAME_SLOTS; }
		else if (sc == SDL_SCANCODE_DOWN) { sel = (sel + 1) % GAME_SLOTS; }
		else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) { sel = sc - SDL_SCANCODE_1; }
		else if (sc == SDL_SCANCODE_0) { sel = 9; }
	}

	// Restore the frozen frame and present once, so no dialog remnant remains.
	if (backup != NULL && surface != NULL) {
		byte *pix = (byte *)surface->pixels;
		for (int row = 0; row < POP_FRAME_H; ++row) {
			memcpy(pix + (size_t)row * (size_t)surface->pitch,
			       backup + row * POP_FRAME_W * 3, POP_FRAME_W * 3);
		}
		update_screen();
	}
	textstate.ptr_font = saved_font;
	clear_kbd_buf();
	return choice;
}

// Resets host timer origins while preserving logical timer lengths, and drains
// physical keyboard/controller state so keys typed in the slot UI cannot
// become gameplay input; the confirming Enter is suppressed until release.
static void resume_after_ui(void) {
	for (int i = 0; i < NUM_TIMERS; ++i) reset_timer(i);
	for (int i = 0; i < SDL_NUM_SCANCODES; ++i) {
		key_states[i] &= ~(KEYSTATE_HELD | KEYSTATE_HELD_NEW);
	}
	clear_kbd_buf();
	g.suppress_scancode = SDL_SCANCODE_RETURN;
}

static void do_save_to(const char *path) {
	SaveError error;
	memset(&error, 0, sizeof(error));
	SaveResult r = game_save_file(path, &error);
	if (r != SAVE_OK) report_error("Could not save game", &error);
}

// Loads `path`, prepares the continuation and commits it. Returns true on a
// successful load (state installed, continuation ready), false otherwise.
static bool do_load_from(const char *path) {
	SaveError error;
	memset(&error, 0, sizeof(error));
	PreparedSnapshot prepared;
	SaveResult r = game_load_file_prepare(path, &prepared, &error);
	if (r == SAVE_OK) {
		game_snapshot_commit(&prepared);
		game_snapshot_dispose(&prepared);
		return true;
	}
	report_error("Could not load game", &error);
	return false;
}

// Title load: bypasses title/cutscene/fresh-level presentation and enters the
// saved frame-boundary continuation.
static void title_load(const char *path) {
	if (!do_load_from(path)) return; // failed/cancelled: remain on title
	release_title_images();
	if (offscreen_surface != NULL) {
		free_surface(offscreen_surface);
		offscreen_surface = NULL;
	}
	is_cutscene = 0;
	is_ending_sequence = false;
	redraw_loaded_snapshot();
	play_level_continue();
}

static void do_save_named(void) {
	g.busy = true;
	g.native_panel_active = true;
	audio_set_panel_suspended(true);
	int slot = slot_picker(true);
	audio_set_panel_suspended(false);
	g.native_panel_active = false;
	g.busy = false;
	resume_after_ui();

	if (slot < 0) return; // cancel leaves session intact
	char path[PATH_MAX];
	slot_path(slot, path, sizeof(path));
	do_save_to(path);
}

static void do_load_named(bool at_title) {
	g.busy = true;
	g.native_panel_active = true;
	audio_set_panel_suspended(true);
	int slot = slot_picker(false);
	audio_set_panel_suspended(false);
	g.native_panel_active = false;
	g.busy = false;
	resume_after_ui();

	if (slot < 0) return; // cancel leaves session intact
	char path[PATH_MAX];
	slot_path(slot, path, sizeof(path));
	if (!slot_occupied(slot)) return; // nothing to load in this slot

	if (at_title) {
		title_load(path);
	} else {
		if (do_load_from(path)) {
			redraw_loaded_snapshot();
		}
	}
}

void game_service_commands(GameSafePoint point) {
	if (g.busy || !g.has_pending) return;
	GameCommandKind kind = g.kind;
	g.has_pending = false;

	if (replay_locked()) return; // Save/Load disabled during replay sessions.

	if (point == GAME_SAFE_TITLE) {
		// Title: save disabled; load enabled.
		if (kind == GAME_QUICKLOAD) {
			const char *path = path_for_quick_slot();
			if (path != NULL) title_load(path);
		} else if (kind == GAME_LOAD_NAMED) {
			do_load_named(true);
		}
		return;
	}

	// GAME_SAFE_FRAME (ordinary gameplay, including falling/death/pending
	// transitions).
	switch (kind) {
		case GAME_SAVE_NAMED:
			do_save_named();
			break;
		case GAME_LOAD_NAMED:
			do_load_named(false);
			break;
		case GAME_QUICKSAVE: {
			const char *path = path_for_quick_slot();
			if (path != NULL) do_save_to(path);
			break;
		}
		case GAME_QUICKLOAD: {
			const char *path = path_for_quick_slot();
			if (path != NULL && do_load_from(path)) {
				redraw_loaded_snapshot();
			}
			break;
		}
	}
}

void game_complete_panel(uint64_t generation, const char *utf8_path, bool cancelled) {
	// Retained for API compatibility; the in-game slot UI is synchronous and
	// does not use asynchronous panel completion.
	(void)generation; (void)utf8_path; (void)cancelled;
}

// Called after SDL has pumped events. Suppresses the triggering held shortcut
// until release and consumes any command-modified key events that would
// otherwise reach gameplay input while save/load UI is active.
void game_commands_consume_ui_keys(void) {
	if (g.suppress_scancode >= 0) {
		if (key_states[g.suppress_scancode] & KEYSTATE_HELD) {
			key_states[g.suppress_scancode] &= ~(KEYSTATE_HELD | KEYSTATE_HELD_NEW);
		} else {
			g.suppress_scancode = -1;
		}
	}
	if (g.busy || g.native_panel_active) {
		SDL_Keymod mod = SDL_GetModState();
		if (mod & KMOD_GUI) {
			for (int i = 0; i < SDL_NUM_SCANCODES; ++i) {
				key_states[i] &= ~(KEYSTATE_HELD_NEW);
			}
		}
	}
}
