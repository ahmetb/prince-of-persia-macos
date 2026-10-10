/*
Prince of Persia — exact-state gameplay snapshot codec.
Copyright (C) 2026  The Prince of Persia macOS port authors.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef GAME_SNAPSHOT_H
#define GAME_SNAPSHOT_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "types.h"
#include "opl3.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POP_SAVE_FORMAT_VERSION 1
#define PROFILE_DOS_SAVE_V1 1
#define POP_FRAME_W 320
#define POP_FRAME_H 200
#define POP_FRAME_RGB_BYTES (POP_FRAME_W * POP_FRAME_H * 3)
#define POP_SAVE_MAX_DOCUMENT (1 << 20) // 1 MiB hard cap
#define POP_MIDI_MAX_TRACKS 16

// ---------------------------------------------------------------------------
// Result / error reporting
// ---------------------------------------------------------------------------

typedef enum {
	SAVE_OK = 0,
	SAVE_IO_ERROR,
	SAVE_BAD_FORMAT,
	SAVE_UNSUPPORTED_VERSION,
	SAVE_INCOMPATIBLE_PROFILE,
	SAVE_INVALID_STATE,
	SAVE_PREPARE_FAILED,
} SaveResult;

typedef struct {
	int section;          // 0-based section index, or -1 for the header
	char message[256];    // human-readable reason
} SaveError;

static inline void save_error_set(SaveError *error, int section, const char *message) {
	if (error == NULL) return;
	error->section = section;
	snprintf(error->message, sizeof(error->message), "%s", message ? message : "");
}

// ---------------------------------------------------------------------------
// Base state (mirrors quick_process/game_state_fields.def exactly)
// ---------------------------------------------------------------------------

typedef struct BaseState {
#define POP_BASE_FIELD(name, c_type, kind, decl) c_type decl;
#include "game_state_fields.def"
#undef POP_BASE_FIELD
} BaseState;

// ---------------------------------------------------------------------------
// Native extension fields
// ---------------------------------------------------------------------------

typedef struct NativeExtension {
	word exit_room_timer;
	word text_time_total;
	word text_time_remaining;
	word is_show_time;
	word next_room;
	word seamless;
	short roomleave_result;
	word resurrect_time;
	short start_level;
	word dont_reset_time;
	word is_restart_level;
	word seed_was_init;
	short next_sound;
	word current_sound;
	short jumped_through_mirror;
	word is_blind_mode;
	byte sound_mode;
	byte is_sound_on;
	byte enable_music;
	word different_room;
	word need_full_redraw;
	word need_redraw_because_flipped;
} NativeExtension;

typedef struct TimerState {
	word wait_time[NUM_TIMERS];
} TimerState;

// ---------------------------------------------------------------------------
// OPL synthesizer continuation (symbolic pointer links)
// ---------------------------------------------------------------------------

enum {
	OPL_REF_SLOT_FBMOD = 0,
	OPL_REF_SLOT_OUT = 1,
	OPL_REF_ZEROMOD = 2,
};

typedef struct SerialOplSlot {
	uint8_t channel_index; // slot.channel -> channel index
	uint8_t mod_kind;      // OPL_REF_*
	uint8_t mod_index;     // slot index (for SLOT_FBMOD / SLOT_OUT)
	uint8_t trem_kind;     // 0 = chip tremolo, 1 = chip zeromod
	int16_t out;
	int16_t fbmod;
	int16_t prout;
	int16_t eg_rout;
	int16_t eg_out;
	uint8_t eg_inc;
	uint8_t eg_gen;
	uint8_t eg_rate;
	uint8_t eg_ksl;
	uint8_t reg_vib;
	uint8_t reg_type;
	uint8_t reg_ksr;
	uint8_t reg_mult;
	uint8_t reg_ksl;
	uint8_t reg_tl;
	uint8_t reg_ar;
	uint8_t reg_dr;
	uint8_t reg_sl;
	uint8_t reg_rr;
	uint8_t reg_wf;
	uint8_t key;
	uint32_t pg_phase;
	uint32_t timer;
} SerialOplSlot;

typedef struct SerialOplChannel {
	uint16_t slot0;       // slots[0] -> slot index
	uint16_t slot1;       // slots[1] -> slot index
	int16_t pair;         // pair -> channel index, or -1
	uint8_t out_kind[4];  // OPL_REF_SLOT_OUT or OPL_REF_ZEROMOD
	uint8_t out_index[4]; // slot index for OPL_REF_SLOT_OUT
	uint8_t chtype;
	uint16_t f_num;
	uint8_t block;
	uint8_t fb;
	uint8_t con;
	uint8_t alg;
	uint8_t ksv;
	uint16_t cha;
	uint16_t chb;
} SerialOplChannel;

typedef struct {
	uint64_t time;
	uint16_t reg;
	uint8_t data;
} SerOplWritebuf;

typedef struct SerialOplChip {
	uint16_t timer;
	uint8_t newm;
	uint8_t nts;
	uint8_t rhy;
	uint8_t vibpos;
	uint8_t vibshift;
	uint8_t tremolo;
	uint8_t tremolopos;
	uint8_t tremoloshift;
	uint32_t noise;
	int16_t zeromod;
	int32_t mixbuff[2];
	int32_t rateratio;
	int32_t samplecnt;
	int16_t oldsamples[2];
	int16_t samples[2];
	uint64_t writebuf_samplecnt;
	uint32_t writebuf_cur;
	uint32_t writebuf_last;
	uint64_t writebuf_lasttime;
	SerOplWritebuf writebuf[OPL_WRITEBUF_SIZE];
	SerialOplSlot slot[36];
	SerialOplChannel channel[18];
} SerialOplChip;

// ---------------------------------------------------------------------------
// Audio continuation
// ---------------------------------------------------------------------------

typedef struct SpeakerAudioState {
	int16_t resource_id; // -1 when none
	int16_t note_index;
	int32_t samples_emitted_in_note;
	float square_phase;  // square_wave_samples_since_last_flip
	int16_t square_state; // square_wave_state
	uint8_t playing;
} SpeakerAudioState;

typedef struct DigiAudioState {
	int16_t resource_id; // -1 when none
	uint32_t byte_offset;
	uint32_t remaining_length;
	uint8_t playing;
} DigiAudioState;

typedef struct MidiAudioState {
	int16_t resource_id; // -1 when none
	uint8_t playing;
	int16_t num_tracks;
	struct {
		int32_t event_index;
		int64_t next_pause_tick;
	} tracks[POP_MIDI_MAX_TRACKS];
	int64_t current_pos;
	float current_pos_fract;
	int32_t ticks_to_next_pause;
	uint32_t us_per_beat;
	uint32_t ticks_per_beat;
	int32_t mixing_freq;
	int8_t semitones_higher;
	float tempo_modifier;
	uint8_t channel_instrument[16];
	uint8_t voice_note[18];
	uint8_t voice_instrument[18];
	uint8_t voice_channel[18];
	int16_t last_used_voice;
	SerialOplChip opl;
} MidiAudioState;

typedef struct AudioSnapshot {
	SpeakerAudioState speaker;
	DigiAudioState digi;
	MidiAudioState midi;
} AudioSnapshot;

// ---------------------------------------------------------------------------
// Full snapshot + prepared continuation
// ---------------------------------------------------------------------------

typedef struct GameSnapshot {
	BaseState base;
	NativeExtension native;
	TimerState timer;
	AudioSnapshot audio;
	byte frame[POP_FRAME_RGB_BYTES];
} GameSnapshot;

// Prepared resources owned entirely by the continuation (no live globals).
typedef struct PreparedLevelGraphics {
	int level;
	chtab_type *env_chtab;
	chtab_type *envwall_chtab;
	chtab_type *guard_chtab;
	word curr_guard_color;
	byte env_pal[0x10 * 3];
	byte wall_pal[0x10 * 3];
	bool has_env_pal;
	bool has_wall_pal;
} PreparedLevelGraphics;

typedef struct PreparedAudioState {
	// Prepared speaker/digi/midi continuation resources.
	SpeakerAudioState speaker;
	DigiAudioState digi;
	MidiAudioState midi;
	// Owned prepared objects.
	struct _PreparedAudioObjects *objects;
} PreparedAudioState;

typedef struct PreparedSnapshot {
	GameSnapshot snapshot;
	PreparedLevelGraphics *level_graphics;
	PreparedAudioState audio;
} PreparedSnapshot;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

void game_snapshot_capture(GameSnapshot *out);
SaveResult game_snapshot_encode(const GameSnapshot *src, uint8_t *dst,
                                size_t capacity, size_t *written, SaveError *error);
SaveResult game_snapshot_decode(const uint8_t *src, size_t length,
                                GameSnapshot *out, SaveError *error);
SaveResult game_snapshot_prepare(const GameSnapshot *src,
                                 PreparedSnapshot *out, SaveError *error);
void game_snapshot_commit(PreparedSnapshot *prepared);
void game_snapshot_dispose(PreparedSnapshot *prepared);
SaveResult game_save_file(const char *utf8_path, SaveError *error);
SaveResult game_load_file_prepare(const char *utf8_path,
                                  PreparedSnapshot *out, SaveError *error);

// Audio capture/prepare/commit (implemented over seg009.c/midi.c state).
void audio_state_capture(AudioSnapshot *out);
SaveResult audio_state_prepare(const AudioSnapshot *src,
                               PreparedAudioState *out, SaveError *error);
void audio_state_commit(PreparedAudioState *prepared);

// Panel suspension freezes audio cursors while native modal UI is open.
void audio_set_panel_suspended(bool suspended);

// Restore-only presentation of a freshly committed snapshot.
void redraw_loaded_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif
