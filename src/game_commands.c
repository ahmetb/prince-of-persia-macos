/*
Prince of Persia — native save/load command dispatcher.
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

static struct {
	GameCommandKind kind;
	bool has_pending;
	bool busy;
	bool native_panel_active;
	uint64_t generation;
	struct {
		bool have;
		bool cancelled;
		char path[PATH_MAX];
	} result;
	int suppress_scancode;
} g = { .suppress_scancode = -1 };

static bool replay_locked(void) {
#ifdef USE_REPLAY
	return (replaying || recording) != 0;
#else
	return false;
#endif
}

static void set_suppressed_scancode(int scancode) {
	g.suppress_scancode = scancode;
}

bool game_request_command(GameCommandKind kind) {
	if (g.busy || g.has_pending) return false;
	if (replay_locked()) return false; // Save/Load disabled during replay sessions.
	g.kind = kind;
	g.has_pending = true;
	return true;
}

// Pumps window/quit events and repaints the frozen frame while a panel is open.
static void panel_pump(void) {
	process_events();
	update_screen();
}

// Returns true when simulation may now proceed; resets host timer origins while
// preserving logical timer lengths, and drains held input.
static void resume_after_ui(void) {
	for (int i = 0; i < NUM_TIMERS; ++i) reset_timer(i);
	clear_kbd_buf();
	set_suppressed_scancode(-1);
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

static void do_load_named(bool at_title) {
	// Open the load panel.
	g.busy = true;
	g.native_panel_active = true;
	g.result.have = false;
	++g.generation;
	audio_set_panel_suspended(true);
	macos_run_game_panel(g.generation, false, panel_pump);
	audio_set_panel_suspended(false);
	g.native_panel_active = false;
	g.busy = false;
	resume_after_ui();

	if (!g.result.have || g.result.cancelled) return; // cancel leaves session intact
	if (at_title) {
		title_load(g.result.path);
	} else {
		if (do_load_from(g.result.path)) {
			redraw_loaded_snapshot();
		}
	}
}

static void do_save_named(void) {
	g.busy = true;
	g.native_panel_active = true;
	g.result.have = false;
	++g.generation;
	audio_set_panel_suspended(true);
	macos_run_game_panel(g.generation, true, panel_pump);
	audio_set_panel_suspended(false);
	g.native_panel_active = false;
	g.busy = false;
	resume_after_ui();

	if (!g.result.have || g.result.cancelled) return;
	do_save_to(g.result.path);
}

void game_complete_panel(uint64_t generation, const char *utf8_path, bool cancelled) {
	if (generation != g.generation) return; // stale completion
	g.result.have = true;
	g.result.cancelled = cancelled;
	g.result.path[0] = '\0';
	if (utf8_path != NULL && !cancelled) {
		strncpy(g.result.path, utf8_path, sizeof(g.result.path) - 1);
		g.result.path[sizeof(g.result.path) - 1] = '\0';
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

// Called after SDL has pumped events. Suppresses the triggering held shortcut
// until release and consumes any command-modified key events that would
// otherwise reach gameplay input while panel UI is active.
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
			// Prevent menu shortcuts from being interpreted as gameplay input.
			for (int i = 0; i < SDL_NUM_SCANCODES; ++i) {
				key_states[i] &= ~(KEYSTATE_HELD_NEW);
			}
		}
	}
}
