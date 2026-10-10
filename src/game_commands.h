/*
Prince of Persia — native save/load command dispatcher.
Copyright (C) 2026  The Prince of Persia macOS port authors.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef GAME_COMMANDS_H
#define GAME_COMMANDS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	GAME_SAVE_NAMED = 0,
	GAME_LOAD_NAMED,
	GAME_QUICKSAVE,
	GAME_QUICKLOAD,
} GameCommandKind;

typedef enum {
	GAME_SAFE_TITLE = 0,
	GAME_SAFE_FRAME,
} GameSafePoint;

// Enqueue a command; callers must only enqueue (never open panels or install
// state). Returns false if a command or panel is already in flight.
bool game_request_command(GameCommandKind kind);

// Service the pending command at a safe point (title, or complete-frame
// boundary). This is where panels are opened and saves/loads committed.
void game_service_commands(GameSafePoint point);

// Records the result of a native panel opened by the dispatcher. Stale
// completions (older generations) are ignored.
void game_complete_panel(uint64_t generation, const char *utf8_path, bool cancelled);

// Called from process_events() to drain/suppress command-triggering input so
// keys typed or held in a panel cannot become gameplay input.
void game_commands_consume_ui_keys(void);

#ifdef __cplusplus
}
#endif

#endif
