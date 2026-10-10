/*
Prince of Persia — native macOS app support layer.
Copyright (C) 2026  The Prince of Persia macOS port authors.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef MACOS_APP_H
#define MACOS_APP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stable, checked root directories owned by the application. Roots are
// resolved beneath ~/Library/Application Support/local.princeofpersia.metal/
// at startup and cached; no path resolves against the CWD or $HOME.
typedef enum {
	MACOS_GAME_DATA,
	MACOS_RESOURCES,
	MACOS_SETTINGS,
	MACOS_SAVES,
	MACOS_REPLAYS,
	MACOS_SCREENSHOTS,
	MACOS_SCORES
} MacosPathRoot;

// Main-thread pre-game startup: initializes SDL video (which creates the
// NSApplication, delegate and Cocoa event pump), resolves the storage roots,
// and runs the first-launch asset acquisition gate. Returns true when the app
// may continue to pop_main(); returns false to exit without starting the game.
bool macos_prepare_application(void);

// Copies the absolute UTF-8 path for `relative` beneath `root` into `out`.
// Returns false (and leaves `out` empty) when the result would be truncated.
bool macos_copy_path(MacosPathRoot root, const char *relative, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
