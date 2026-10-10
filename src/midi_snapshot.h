/*
MIDI/OPL snapshot hooks (implemented by midi.c).
Copyright (C) 2026  The Prince of Persia macOS port authors.
*/

#ifndef MIDI_SNAPSHOT_H
#define MIDI_SNAPSHOT_H

#include "game_snapshot.h"

void midi_snapshot_capture(MidiAudioState *out);
SaveResult midi_snapshot_prepare(const MidiAudioState *src, SaveError *error);
void midi_snapshot_commit(const MidiAudioState *prepared);
void midi_snapshot_dispose(void);

#endif
