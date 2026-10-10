/*
Prince of Persia — exact-state gameplay snapshot codec.
Copyright (C) 2026  The Prince of Persia macOS port authors.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "common.h"
#include "macos_app.h"
#include "game_snapshot.h"
#include "midi_snapshot.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Byte buffer / cursor primitives (all bounds-checked, little-endian)
// ---------------------------------------------------------------------------

typedef struct {
	uint8_t *data;
	size_t len;
	size_t cap;
} Bytes;

static bool bytes_init(Bytes *b, size_t reserve) {
	b->cap = reserve ? reserve : 256;
	b->len = 0;
	b->data = malloc(b->cap);
	return b->data != NULL;
}

static bool bytes_reserve(Bytes *b, size_t extra) {
	if (b->len + extra <= b->cap) return true;
	size_t ncap = b->cap;
	while (ncap < b->len + extra) ncap *= 2;
	uint8_t *nd = realloc(b->data, ncap);
	if (nd == NULL) return false;
	b->data = nd;
	b->cap = ncap;
	return true;
}

static bool put_bytes(Bytes *b, const void *p, size_t n) {
	if (!bytes_reserve(b, n)) return false;
	memcpy(b->data + b->len, p, n);
	b->len += n;
	return true;
}

#define PUT_FIXED(fn, ty) \
	static bool fn(Bytes *b, ty v) { v = (ty)v; return put_bytes(b, &v, sizeof(ty)); }

typedef struct {
	const uint8_t *data;
	size_t len;
	size_t pos;
} Cursor;

static void cursor_init(Cursor *c, const uint8_t *data, size_t len) {
	c->data = data; c->len = len; c->pos = 0;
}
static bool get_bytes(Cursor *c, void *p, size_t n) {
	// remaining-length subtraction, checked
	if (c->pos > c->len || n > c->len - c->pos) return false;
	memcpy(p, c->data + c->pos, n);
	c->pos += n;
	return true;
}
#define GET_FIXED(fn, ty) \
	static bool fn(Cursor *c, ty *out) { \
		uint8_t tmp[sizeof(ty)]; \
		if (!get_bytes(c, tmp, sizeof(tmp))) return false; \
		ty v = 0; \
		for (size_t i = 0; i < sizeof(ty); ++i) v |= ((ty)tmp[i]) << (8 * i); \
		*out = v; \
		return true; \
	}

static bool put_u16(Bytes *b, uint16_t v) { v = (uint16_t)v; return put_bytes(b, &v, 2); }
static bool put_i16(Bytes *b, int16_t v) { v = (int16_t)v; return put_bytes(b, &v, 2); }
static bool put_u32(Bytes *b, uint32_t v) { v = (uint32_t)v; return put_bytes(b, &v, 4); }
static bool put_i32(Bytes *b, int32_t v) { v = (int32_t)v; return put_bytes(b, &v, 4); }
static bool put_u64(Bytes *b, uint64_t v) { v = (uint64_t)v; return put_bytes(b, &v, 8); }
static bool put_i64(Bytes *b, int64_t v) { v = (int64_t)v; return put_bytes(b, &v, 8); }
static bool put_f32bits(Bytes *b, float v) { uint32_t u; memcpy(&u, &v, 4); return put_u32(b, u); }

static bool get_u16(Cursor *c, uint16_t *o) {
	uint8_t t[2]; if (!get_bytes(c, t, 2)) return false;
	*o = (uint16_t)(t[0] | (t[1] << 8)); return true;
}
static bool get_i16(Cursor *c, int16_t *o) {
	uint8_t t[2]; if (!get_bytes(c, t, 2)) return false;
	uint16_t u = (uint16_t)(t[0] | (t[1] << 8));
	memcpy(o, &u, 2); return true;
}
static bool get_u32(Cursor *c, uint32_t *o) {
	uint8_t t[4]; if (!get_bytes(c, t, 4)) return false;
	*o = (uint32_t)t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
	return true;
}
static bool get_i32(Cursor *c, int32_t *o) {
	uint32_t u; if (!get_u32(c, &u)) return false;
	memcpy(o, &u, 4); return true;
}
static bool get_u64(Cursor *c, uint64_t *o) {
	uint8_t t[8]; if (!get_bytes(c, t, 8)) return false;
	uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= ((uint64_t)t[i]) << (8 * i);
	*o = v; return true;
}
static bool get_i64(Cursor *c, int64_t *o) {
	uint64_t u; if (!get_u64(c, &u)) return false;
	memcpy(o, &u, 8); return true;
}
static bool get_f32bits(Cursor *c, float *o) {
	uint32_t u; if (!get_u32(c, &u)) return false;
	memcpy(o, &u, 4); return true;
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

static void sha256_bytes(const uint8_t *data, size_t len, uint8_t out[CC_SHA256_DIGEST_LENGTH]) {
	CC_SHA256(data, (CC_LONG)len, out);
}

// ---------------------------------------------------------------------------
// Base field codecs (member-by-member, fixed-width little-endian)
// ---------------------------------------------------------------------------

static SaveResult enc_LEVEL(const level_type *p, Bytes *b) {
	if (!put_bytes(b, p->fg, sizeof(p->fg))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->bg, sizeof(p->bg))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->doorlinks1, sizeof(p->doorlinks1))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->doorlinks2, sizeof(p->doorlinks2))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->roomlinks, sizeof(p->roomlinks))) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->used_rooms, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->roomxs, sizeof(p->roomxs))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->roomys, sizeof(p->roomys))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->fill_1, sizeof(p->fill_1))) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->start_room, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->start_pos, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->start_dir, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->fill_2, sizeof(p->fill_2))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->guards_tile, sizeof(p->guards_tile))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->guards_dir, sizeof(p->guards_dir))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->guards_x, sizeof(p->guards_x))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->guards_seq_lo, sizeof(p->guards_seq_lo))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->guards_skill, sizeof(p->guards_skill))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->guards_seq_hi, sizeof(p->guards_seq_hi))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->guards_color, sizeof(p->guards_color))) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->fill_3, sizeof(p->fill_3))) return SAVE_IO_ERROR;
	return SAVE_OK;
}

static SaveResult dec_LEVEL(Cursor *c, level_type *p) {
	if (!get_bytes(c, p->fg, sizeof(p->fg))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->bg, sizeof(p->bg))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->doorlinks1, sizeof(p->doorlinks1))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->doorlinks2, sizeof(p->doorlinks2))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->roomlinks, sizeof(p->roomlinks))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->used_rooms, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->roomxs, sizeof(p->roomxs))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->roomys, sizeof(p->roomys))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->fill_1, sizeof(p->fill_1))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->start_room, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->start_pos, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->start_dir, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->fill_2, sizeof(p->fill_2))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->guards_tile, sizeof(p->guards_tile))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->guards_dir, sizeof(p->guards_dir))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->guards_x, sizeof(p->guards_x))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->guards_seq_lo, sizeof(p->guards_seq_lo))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->guards_skill, sizeof(p->guards_skill))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->guards_seq_hi, sizeof(p->guards_seq_hi))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->guards_color, sizeof(p->guards_color))) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->fill_3, sizeof(p->fill_3))) return SAVE_BAD_FORMAT;
	return SAVE_OK;
}

static SaveResult enc_CHAR(const char_type *p, Bytes *b) {
	if (!put_bytes(b, &p->frame, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->x, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->y, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->direction, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->curr_col, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->curr_row, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->action, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->fall_x, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->fall_y, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->room, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->repeat, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->charid, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->sword, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->alive, 1)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->curr_seq)) return SAVE_IO_ERROR;
	return SAVE_OK;
}

static SaveResult dec_CHAR(Cursor *c, char_type *p) {
	if (!get_bytes(c, &p->frame, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->x, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->y, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->direction, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->curr_col, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->curr_row, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->action, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->fall_x, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->fall_y, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->room, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->repeat, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->charid, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->sword, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->alive, 1)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->curr_seq)) return SAVE_BAD_FORMAT;
	return SAVE_OK;
}

static SaveResult enc_MOBS(const mob_type (*p)[14], Bytes *b) {
	for (int i = 0; i < 14; ++i) {
		const mob_type *m = &(*p)[i];
		if (!put_bytes(b, &m->xh, 1) || !put_bytes(b, &m->y, 1) ||
		    !put_bytes(b, &m->room, 1) || !put_bytes(b, &m->speed, 1) ||
		    !put_bytes(b, &m->type, 1) || !put_bytes(b, &m->row, 1)) return SAVE_IO_ERROR;
	}
	return SAVE_OK;
}
static SaveResult dec_MOBS(Cursor *c, mob_type (*p)[14]) {
	for (int i = 0; i < 14; ++i) {
		mob_type *m = &(*p)[i];
		if (!get_bytes(c, &m->xh, 1) || !get_bytes(c, &m->y, 1) ||
		    !get_bytes(c, &m->room, 1) || !get_bytes(c, &m->speed, 1) ||
		    !get_bytes(c, &m->type, 1) || !get_bytes(c, &m->row, 1)) return SAVE_BAD_FORMAT;
	}
	return SAVE_OK;
}

static SaveResult enc_TROBS(const trob_type (*p)[TROBS_MAX], Bytes *b) {
	for (int i = 0; i < TROBS_MAX; ++i) {
		const trob_type *t = &(*p)[i];
		if (!put_bytes(b, &t->tilepos, 1) || !put_bytes(b, &t->room, 1) ||
		    !put_bytes(b, &t->type, 1)) return SAVE_IO_ERROR;
	}
	return SAVE_OK;
}
static SaveResult dec_TROBS(Cursor *c, trob_type (*p)[TROBS_MAX]) {
	for (int i = 0; i < TROBS_MAX; ++i) {
		trob_type *t = &(*p)[i];
		if (!get_bytes(c, &t->tilepos, 1) || !get_bytes(c, &t->room, 1) ||
		    !get_bytes(c, &t->type, 1)) return SAVE_BAD_FORMAT;
	}
	return SAVE_OK;
}

// sbyte[10] and byte[10] arrays
static SaveResult enc_S8A10(const sbyte (*p)[10], Bytes *b) { return put_bytes(b, *p, 10) ? SAVE_OK : SAVE_IO_ERROR; }
static SaveResult dec_S8A10(Cursor *c, sbyte (*p)[10]) { return get_bytes(c, *p, 10) ? SAVE_OK : SAVE_BAD_FORMAT; }
static SaveResult enc_U8A10(const byte (*p)[10], Bytes *b) { return put_bytes(b, *p, 10) ? SAVE_OK : SAVE_IO_ERROR; }
static SaveResult dec_U8A10(Cursor *c, byte (*p)[10]) { return get_bytes(c, *p, 10) ? SAVE_OK : SAVE_BAD_FORMAT; }

// torch_colors: byte[25][30]
static SaveResult enc_TCOL(const byte (*p)[25][30], Bytes *b) { return put_bytes(b, *p, 25 * 30) ? SAVE_OK : SAVE_IO_ERROR; }
static SaveResult dec_TCOL(Cursor *c, byte (*p)[25][30]) { return get_bytes(c, *p, 25 * 30) ? SAVE_OK : SAVE_BAD_FORMAT; }

static SaveResult enc_U16(const word *p, Bytes *b) { return put_u16(b, *p) ? SAVE_OK : SAVE_IO_ERROR; }
static SaveResult dec_U16(Cursor *c, word *p) { uint16_t v; if (!get_u16(c, &v)) return SAVE_BAD_FORMAT; *p = v; return SAVE_OK; }
static SaveResult enc_I16(const short *p, Bytes *b) { return put_i16(b, *p) ? SAVE_OK : SAVE_IO_ERROR; }
static SaveResult dec_I16(Cursor *c, short *p) { int16_t v; if (!get_i16(c, &v)) return SAVE_BAD_FORMAT; *p = v; return SAVE_OK; }
static SaveResult enc_U32(const dword *p, Bytes *b) { return put_u32(b, *p) ? SAVE_OK : SAVE_IO_ERROR; }
static SaveResult dec_U32(Cursor *c, dword *p) { uint32_t v; if (!get_u32(c, &v)) return SAVE_BAD_FORMAT; *p = v; return SAVE_OK; }
static SaveResult enc_I8(const sbyte *p, Bytes *b) { return put_bytes(b, p, 1) ? SAVE_OK : SAVE_IO_ERROR; }
static SaveResult dec_I8(Cursor *c, sbyte *p) { return get_bytes(c, p, 1) ? SAVE_OK : SAVE_BAD_FORMAT; }
static SaveResult enc_U8(const byte *p, Bytes *b) { return put_bytes(b, p, 1) ? SAVE_OK : SAVE_IO_ERROR; }
static SaveResult dec_U8(Cursor *c, byte *p) { return get_bytes(c, p, 1) ? SAVE_OK : SAVE_BAD_FORMAT; }

static SaveResult base_encode(const BaseState *st, Bytes *b) {
	SaveResult r = SAVE_OK;
#define POP_BASE_FIELD(name, c_type, kind, decl) \
	{ r = enc_##kind(&(st->name), b); if (r != SAVE_OK) return r; }
#include "game_state_fields.def"
#undef POP_BASE_FIELD
	return r;
}

static SaveResult base_decode(Cursor *c, BaseState *st) {
	SaveResult r = SAVE_OK;
#define POP_BASE_FIELD(name, c_type, kind, decl) \
	{ r = dec_##kind(c, &(st->name)); if (r != SAVE_OK) return r; }
#include "game_state_fields.def"
#undef POP_BASE_FIELD
	return r;
}

// ---------------------------------------------------------------------------
// Native extension / timer codecs
// ---------------------------------------------------------------------------

static SaveResult native_encode(const NativeExtension *p, Bytes *b) {
	if (!put_u16(b, p->exit_room_timer)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->text_time_total)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->text_time_remaining)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->is_show_time)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->next_room)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->seamless)) return SAVE_IO_ERROR;
	if (!put_i16(b, p->roomleave_result)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->resurrect_time)) return SAVE_IO_ERROR;
	if (!put_i16(b, p->start_level)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->dont_reset_time)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->is_restart_level)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->seed_was_init)) return SAVE_IO_ERROR;
	if (!put_i16(b, p->next_sound)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->current_sound)) return SAVE_IO_ERROR;
	if (!put_i16(b, p->jumped_through_mirror)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->is_blind_mode)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->sound_mode, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->is_sound_on, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->enable_music, 1)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->different_room)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->need_full_redraw)) return SAVE_IO_ERROR;
	if (!put_u16(b, p->need_redraw_because_flipped)) return SAVE_IO_ERROR;
	return SAVE_OK;
}

static SaveResult native_decode(Cursor *c, NativeExtension *p) {
	if (!get_u16(c, &p->exit_room_timer)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->text_time_total)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->text_time_remaining)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->is_show_time)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->next_room)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->seamless)) return SAVE_BAD_FORMAT;
	if (!get_i16(c, &p->roomleave_result)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->resurrect_time)) return SAVE_BAD_FORMAT;
	if (!get_i16(c, &p->start_level)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->dont_reset_time)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->is_restart_level)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->seed_was_init)) return SAVE_BAD_FORMAT;
	if (!get_i16(c, &p->next_sound)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->current_sound)) return SAVE_BAD_FORMAT;
	if (!get_i16(c, &p->jumped_through_mirror)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->is_blind_mode)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->sound_mode, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->is_sound_on, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->enable_music, 1)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->different_room)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->need_full_redraw)) return SAVE_BAD_FORMAT;
	if (!get_u16(c, &p->need_redraw_because_flipped)) return SAVE_BAD_FORMAT;
	return SAVE_OK;
}

static SaveResult timer_encode(const TimerState *p, Bytes *b) {
	for (int i = 0; i < NUM_TIMERS; ++i) if (!put_u16(b, p->wait_time[i])) return SAVE_IO_ERROR;
	return SAVE_OK;
}
static SaveResult timer_decode(Cursor *c, TimerState *p) {
	for (int i = 0; i < NUM_TIMERS; ++i) if (!get_u16(c, &p->wait_time[i])) return SAVE_BAD_FORMAT;
	return SAVE_OK;
}

// ---------------------------------------------------------------------------
// Audio codec
// ---------------------------------------------------------------------------

static SaveResult speaker_encode(const SpeakerAudioState *p, Bytes *b) {
	if (!put_i16(b, p->resource_id)) return SAVE_IO_ERROR;
	if (!put_i16(b, p->note_index)) return SAVE_IO_ERROR;
	if (!put_i32(b, p->samples_emitted_in_note)) return SAVE_IO_ERROR;
	if (!put_f32bits(b, p->square_phase)) return SAVE_IO_ERROR;
	if (!put_i16(b, p->square_state)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->playing, 1)) return SAVE_IO_ERROR;
	return SAVE_OK;
}
static SaveResult speaker_decode(Cursor *c, SpeakerAudioState *p) {
	int32_t se;
	if (!get_i16(c, &p->resource_id)) return SAVE_BAD_FORMAT;
	if (!get_i16(c, &p->note_index)) return SAVE_BAD_FORMAT;
	if (!get_i32(c, &se)) return SAVE_BAD_FORMAT; p->samples_emitted_in_note = se;
	if (!get_f32bits(c, &p->square_phase)) return SAVE_BAD_FORMAT;
	if (!get_i16(c, &p->square_state)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->playing, 1)) return SAVE_BAD_FORMAT;
	return SAVE_OK;
}

static SaveResult digi_encode(const DigiAudioState *p, Bytes *b) {
	if (!put_i16(b, p->resource_id)) return SAVE_IO_ERROR;
	if (!put_u32(b, p->byte_offset)) return SAVE_IO_ERROR;
	if (!put_u32(b, p->remaining_length)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->playing, 1)) return SAVE_IO_ERROR;
	return SAVE_OK;
}
static SaveResult digi_decode(Cursor *c, DigiAudioState *p) {
	if (!get_i16(c, &p->resource_id)) return SAVE_BAD_FORMAT;
	if (!get_u32(c, &p->byte_offset)) return SAVE_BAD_FORMAT;
	if (!get_u32(c, &p->remaining_length)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->playing, 1)) return SAVE_BAD_FORMAT;
	return SAVE_OK;
}

static SaveResult opl_encode(const SerialOplChip *p, Bytes *b) {
	if (!put_u16(b, p->timer)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->newm, 1) || !put_bytes(b, &p->nts, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->rhy, 1) || !put_bytes(b, &p->vibpos, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->vibshift, 1) || !put_bytes(b, &p->tremolo, 1)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->tremolopos, 1) || !put_bytes(b, &p->tremoloshift, 1)) return SAVE_IO_ERROR;
	if (!put_u32(b, p->noise)) return SAVE_IO_ERROR;
	if (!put_i16(b, p->zeromod)) return SAVE_IO_ERROR;
	if (!put_i32(b, p->mixbuff[0]) || !put_i32(b, p->mixbuff[1])) return SAVE_IO_ERROR;
	if (!put_i32(b, p->rateratio) || !put_i32(b, p->samplecnt)) return SAVE_IO_ERROR;
	if (!put_i16(b, p->oldsamples[0]) || !put_i16(b, p->oldsamples[1])) return SAVE_IO_ERROR;
	if (!put_i16(b, p->samples[0]) || !put_i16(b, p->samples[1])) return SAVE_IO_ERROR;
	if (!put_u64(b, p->writebuf_samplecnt)) return SAVE_IO_ERROR;
	if (!put_u32(b, p->writebuf_cur) || !put_u32(b, p->writebuf_last)) return SAVE_IO_ERROR;
	if (!put_u64(b, p->writebuf_lasttime)) return SAVE_IO_ERROR;
	for (int i = 0; i < OPL_WRITEBUF_SIZE; ++i) {
		const SerOplWritebuf *w = &p->writebuf[i];
		if (!put_u64(b, w->time) || !put_u16(b, w->reg) || !put_bytes(b, &w->data, 1)) return SAVE_IO_ERROR;
	}
	for (int i = 0; i < 36; ++i) {
		const SerialOplSlot *s = &p->slot[i];
		if (!put_bytes(b, &s->channel_index, 1) || !put_bytes(b, &s->mod_kind, 1) ||
		    !put_bytes(b, &s->mod_index, 1) || !put_bytes(b, &s->trem_kind, 1)) return SAVE_IO_ERROR;
		if (!put_i16(b, s->out) || !put_i16(b, s->fbmod) || !put_i16(b, s->prout)) return SAVE_IO_ERROR;
		if (!put_i16(b, s->eg_rout) || !put_i16(b, s->eg_out)) return SAVE_IO_ERROR;
		if (!put_bytes(b, &s->eg_inc, 1) || !put_bytes(b, &s->eg_gen, 1) ||
		    !put_bytes(b, &s->eg_rate, 1) || !put_bytes(b, &s->eg_ksl, 1)) return SAVE_IO_ERROR;
		if (!put_bytes(b, &s->reg_vib, 1) || !put_bytes(b, &s->reg_type, 1) ||
		    !put_bytes(b, &s->reg_ksr, 1) || !put_bytes(b, &s->reg_mult, 1)) return SAVE_IO_ERROR;
		if (!put_bytes(b, &s->reg_ksl, 1) || !put_bytes(b, &s->reg_tl, 1) ||
		    !put_bytes(b, &s->reg_ar, 1) || !put_bytes(b, &s->reg_dr, 1)) return SAVE_IO_ERROR;
		if (!put_bytes(b, &s->reg_sl, 1) || !put_bytes(b, &s->reg_rr, 1) ||
		    !put_bytes(b, &s->reg_wf, 1) || !put_bytes(b, &s->key, 1)) return SAVE_IO_ERROR;
		if (!put_u32(b, s->pg_phase) || !put_u32(b, s->timer)) return SAVE_IO_ERROR;
	}
	for (int i = 0; i < 18; ++i) {
		const SerialOplChannel *ch = &p->channel[i];
		if (!put_u16(b, ch->slot0) || !put_u16(b, ch->slot1)) return SAVE_IO_ERROR;
		if (!put_i16(b, ch->pair)) return SAVE_IO_ERROR;
		if (!put_bytes(b, ch->out_kind, 4) || !put_bytes(b, ch->out_index, 4)) return SAVE_IO_ERROR;
		if (!put_bytes(b, &ch->chtype, 1) || !put_u16(b, ch->f_num)) return SAVE_IO_ERROR;
		if (!put_bytes(b, &ch->block, 1) || !put_bytes(b, &ch->fb, 1) ||
		    !put_bytes(b, &ch->con, 1) || !put_bytes(b, &ch->alg, 1) ||
		    !put_bytes(b, &ch->ksv, 1)) return SAVE_IO_ERROR;
		if (!put_u16(b, ch->cha) || !put_u16(b, ch->chb)) return SAVE_IO_ERROR;
	}
	return SAVE_OK;
}

static SaveResult opl_decode(Cursor *c, SerialOplChip *p) {
	if (!get_u16(c, &p->timer)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->newm, 1) || !get_bytes(c, &p->nts, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->rhy, 1) || !get_bytes(c, &p->vibpos, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->vibshift, 1) || !get_bytes(c, &p->tremolo, 1)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->tremolopos, 1) || !get_bytes(c, &p->tremoloshift, 1)) return SAVE_BAD_FORMAT;
	if (!get_u32(c, &p->noise)) return SAVE_BAD_FORMAT;
	if (!get_i16(c, &p->zeromod)) return SAVE_BAD_FORMAT;
	int32_t m0, m1;
	if (!get_i32(c, &m0)) return SAVE_BAD_FORMAT; p->mixbuff[0] = m0;
	if (!get_i32(c, &m1)) return SAVE_BAD_FORMAT; p->mixbuff[1] = m1;
	int32_t r0, r1;
	if (!get_i32(c, &r0)) return SAVE_BAD_FORMAT; p->rateratio = r0;
	if (!get_i32(c, &r1)) return SAVE_BAD_FORMAT; p->samplecnt = r1;
	if (!get_i16(c, &p->oldsamples[0]) || !get_i16(c, &p->oldsamples[1])) return SAVE_BAD_FORMAT;
	if (!get_i16(c, &p->samples[0]) || !get_i16(c, &p->samples[1])) return SAVE_BAD_FORMAT;
	if (!get_u64(c, &p->writebuf_samplecnt)) return SAVE_BAD_FORMAT;
	if (!get_u32(c, &p->writebuf_cur) || !get_u32(c, &p->writebuf_last)) return SAVE_BAD_FORMAT;
	if (!get_u64(c, &p->writebuf_lasttime)) return SAVE_BAD_FORMAT;
	for (int i = 0; i < OPL_WRITEBUF_SIZE; ++i) {
		SerOplWritebuf *w = &p->writebuf[i];
		if (!get_u64(c, &w->time) || !get_u16(c, &w->reg) || !get_bytes(c, &w->data, 1)) return SAVE_BAD_FORMAT;
	}
	for (int i = 0; i < 36; ++i) {
		SerialOplSlot *s = &p->slot[i];
		if (!get_bytes(c, &s->channel_index, 1) || !get_bytes(c, &s->mod_kind, 1) ||
		    !get_bytes(c, &s->mod_index, 1) || !get_bytes(c, &s->trem_kind, 1)) return SAVE_BAD_FORMAT;
		if (!get_i16(c, &s->out) || !get_i16(c, &s->fbmod) || !get_i16(c, &s->prout)) return SAVE_BAD_FORMAT;
		if (!get_i16(c, &s->eg_rout) || !get_i16(c, &s->eg_out)) return SAVE_BAD_FORMAT;
		if (!get_bytes(c, &s->eg_inc, 1) || !get_bytes(c, &s->eg_gen, 1) ||
		    !get_bytes(c, &s->eg_rate, 1) || !get_bytes(c, &s->eg_ksl, 1)) return SAVE_BAD_FORMAT;
		if (!get_bytes(c, &s->reg_vib, 1) || !get_bytes(c, &s->reg_type, 1) ||
		    !get_bytes(c, &s->reg_ksr, 1) || !get_bytes(c, &s->reg_mult, 1)) return SAVE_BAD_FORMAT;
		if (!get_bytes(c, &s->reg_ksl, 1) || !get_bytes(c, &s->reg_tl, 1) ||
		    !get_bytes(c, &s->reg_ar, 1) || !get_bytes(c, &s->reg_dr, 1)) return SAVE_BAD_FORMAT;
		if (!get_bytes(c, &s->reg_sl, 1) || !get_bytes(c, &s->reg_rr, 1) ||
		    !get_bytes(c, &s->reg_wf, 1) || !get_bytes(c, &s->key, 1)) return SAVE_BAD_FORMAT;
		if (!get_u32(c, &s->pg_phase) || !get_u32(c, &s->timer)) return SAVE_BAD_FORMAT;
	}
	for (int i = 0; i < 18; ++i) {
		SerialOplChannel *ch = &p->channel[i];
		if (!get_u16(c, &ch->slot0) || !get_u16(c, &ch->slot1)) return SAVE_BAD_FORMAT;
		if (!get_i16(c, &ch->pair)) return SAVE_BAD_FORMAT;
		if (!get_bytes(c, ch->out_kind, 4) || !get_bytes(c, ch->out_index, 4)) return SAVE_BAD_FORMAT;
		if (!get_bytes(c, &ch->chtype, 1) || !get_u16(c, &ch->f_num)) return SAVE_BAD_FORMAT;
		if (!get_bytes(c, &ch->block, 1) || !get_bytes(c, &ch->fb, 1) ||
		    !get_bytes(c, &ch->con, 1) || !get_bytes(c, &ch->alg, 1) ||
		    !get_bytes(c, &ch->ksv, 1)) return SAVE_BAD_FORMAT;
		if (!get_u16(c, &ch->cha) || !get_u16(c, &ch->chb)) return SAVE_BAD_FORMAT;
	}
	return SAVE_OK;
}

static SaveResult midi_encode(const MidiAudioState *p, Bytes *b) {
	if (!put_i16(b, p->resource_id)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->playing, 1)) return SAVE_IO_ERROR;
	if (!put_i16(b, p->num_tracks)) return SAVE_IO_ERROR;
	for (int i = 0; i < POP_MIDI_MAX_TRACKS; ++i) {
		if (!put_i32(b, p->tracks[i].event_index)) return SAVE_IO_ERROR;
		if (!put_i64(b, p->tracks[i].next_pause_tick)) return SAVE_IO_ERROR;
	}
	if (!put_i64(b, p->current_pos)) return SAVE_IO_ERROR;
	if (!put_f32bits(b, p->current_pos_fract)) return SAVE_IO_ERROR;
	if (!put_i32(b, p->ticks_to_next_pause)) return SAVE_IO_ERROR;
	if (!put_u32(b, p->us_per_beat)) return SAVE_IO_ERROR;
	if (!put_u32(b, p->ticks_per_beat)) return SAVE_IO_ERROR;
	if (!put_i32(b, p->mixing_freq)) return SAVE_IO_ERROR;
	if (!put_bytes(b, &p->semitones_higher, 1)) return SAVE_IO_ERROR;
	if (!put_f32bits(b, p->tempo_modifier)) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->channel_instrument, 16)) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->voice_note, 18)) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->voice_instrument, 18)) return SAVE_IO_ERROR;
	if (!put_bytes(b, p->voice_channel, 18)) return SAVE_IO_ERROR;
	if (!put_i16(b, p->last_used_voice)) return SAVE_IO_ERROR;
	return opl_encode(&p->opl, b);
}
static SaveResult midi_decode(Cursor *c, MidiAudioState *p) {
	int32_t tmp;
	if (!get_i16(c, &p->resource_id)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, &p->playing, 1)) return SAVE_BAD_FORMAT;
	if (!get_i16(c, &p->num_tracks)) return SAVE_BAD_FORMAT;
	for (int i = 0; i < POP_MIDI_MAX_TRACKS; ++i) {
		if (!get_i32(c, &tmp)) return SAVE_BAD_FORMAT; p->tracks[i].event_index = tmp;
		if (!get_i64(c, &p->tracks[i].next_pause_tick)) return SAVE_BAD_FORMAT;
	}
	if (!get_i64(c, &p->current_pos)) return SAVE_BAD_FORMAT;
	if (!get_f32bits(c, &p->current_pos_fract)) return SAVE_BAD_FORMAT;
	if (!get_i32(c, &tmp)) return SAVE_BAD_FORMAT; p->ticks_to_next_pause = tmp;
	if (!get_u32(c, &p->us_per_beat)) return SAVE_BAD_FORMAT;
	if (!get_u32(c, &p->ticks_per_beat)) return SAVE_BAD_FORMAT;
	if (!get_i32(c, &tmp)) return SAVE_BAD_FORMAT; p->mixing_freq = tmp;
	if (!get_bytes(c, &p->semitones_higher, 1)) return SAVE_BAD_FORMAT;
	if (!get_f32bits(c, &p->tempo_modifier)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->channel_instrument, 16)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->voice_note, 18)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->voice_instrument, 18)) return SAVE_BAD_FORMAT;
	if (!get_bytes(c, p->voice_channel, 18)) return SAVE_BAD_FORMAT;
	if (!get_i16(c, &p->last_used_voice)) return SAVE_BAD_FORMAT;
	return opl_decode(c, &p->opl);
}

static SaveResult audio_encode(const AudioSnapshot *p, Bytes *b) {
	SaveResult r;
	r = speaker_encode(&p->speaker, b); if (r) return r;
	r = digi_encode(&p->digi, b); if (r) return r;
	return midi_encode(&p->midi, b);
}
static SaveResult audio_decode(Cursor *c, AudioSnapshot *p) {
	SaveResult r;
	r = speaker_decode(c, &p->speaker); if (r) return r;
	r = digi_decode(c, &p->digi); if (r) return r;
	return midi_decode(c, &p->midi);
}

// ---------------------------------------------------------------------------
// Encode / decode documents
// ---------------------------------------------------------------------------

enum { SEC_BASE = 0, SEC_NATIVE, SEC_TIMER, SEC_AUDIO, SEC_FRAME, SEC_COUNT };

static SaveResult section_encode(int idx, const GameSnapshot *s, Bytes *b) {
	switch (idx) {
		case SEC_BASE: return base_encode(&s->base, b);
		case SEC_NATIVE: return native_encode(&s->native, b);
		case SEC_TIMER: return timer_encode(&s->timer, b);
		case SEC_AUDIO: return audio_encode(&s->audio, b);
		case SEC_FRAME: return put_bytes(b, s->frame, POP_FRAME_RGB_BYTES) ? SAVE_OK : SAVE_IO_ERROR;
		default: return SAVE_INVALID_STATE;
	}
}

static SaveResult section_decode(int idx, Cursor *c, GameSnapshot *s) {
	if (idx == SEC_FRAME) return get_bytes(c, s->frame, POP_FRAME_RGB_BYTES) ? SAVE_OK : SAVE_BAD_FORMAT;
	switch (idx) {
		case SEC_BASE: return base_decode(c, &s->base);
		case SEC_NATIVE: return native_decode(c, &s->native);
		case SEC_TIMER: return timer_decode(c, &s->timer);
		case SEC_AUDIO: return audio_decode(c, &s->audio);
		default: return SAVE_INVALID_STATE;
	}
}

// SHA-256 of the pinned PERSIA.zip payload (353211 bytes).
static const unsigned char pop_archive_sha256[32] = {
	0x99, 0x97, 0x35, 0x55, 0xd3, 0xa2, 0xdb, 0x46,
	0xd0, 0xa7, 0xbe, 0x1d, 0xcf, 0x17, 0xf4, 0x83,
	0x4e, 0x85, 0xd0, 0xb4, 0xd0, 0xbc, 0xa2, 0xad,
	0xb6, 0xba, 0xd4, 0x8a, 0xc3, 0x33, 0xdb, 0x40,
};

SaveResult game_snapshot_encode(const GameSnapshot *src, uint8_t *dst,
                                size_t capacity, size_t *written, SaveError *error) {
	const unsigned char *archive = pop_archive_sha256;

	// Build the payload: five ordered, length-prefixed sections.
	Bytes payload;
	if (!bytes_init(&payload, 256)) { save_error_set(error, -1, "out of memory"); return SAVE_IO_ERROR; }
	SaveResult r = SAVE_OK;
	for (int i = 0; i < SEC_COUNT && r == SAVE_OK; ++i) {
		Bytes sec;
		if (!bytes_init(&sec, 64)) { r = SAVE_IO_ERROR; break; }
		r = section_encode(i, src, &sec);
		if (r == SAVE_OK) {
			if (!put_u32(&payload, (uint32_t)sec.len) || !put_bytes(&payload, sec.data, sec.len)) r = SAVE_IO_ERROR;
		}
		free(sec.data);
	}
	if (r != SAVE_OK) { free(payload.data); save_error_set(error, -1, "encode failed"); return r; }

	unsigned char payload_hash[CC_SHA256_DIGEST_LENGTH];
	sha256_bytes(payload.data, payload.len, payload_hash);

	const size_t total = 84 + payload.len;
	if (total > capacity) { free(payload.data); save_error_set(error, -1, "buffer too small"); return SAVE_IO_ERROR; }

	static const uint8_t magic[8] = {'P','O','P','M','A','C','S','\0'};
	memcpy(dst, magic, 8);
	// version
	dst[8] = POP_SAVE_FORMAT_VERSION & 0xFF; dst[9] = dst[10] = dst[11] = 0;
	// profile id
	dst[12] = PROFILE_DOS_SAVE_V1 & 0xFF; dst[13] = dst[14] = dst[15] = 0;
	memcpy(dst + 16, archive, 32);
	dst[48] = (uint8_t)(payload.len & 0xFF);
	dst[49] = (uint8_t)((payload.len >> 8) & 0xFF);
	dst[50] = (uint8_t)((payload.len >> 16) & 0xFF);
	dst[51] = (uint8_t)((payload.len >> 24) & 0xFF);
	memcpy(dst + 52, payload_hash, 32);
	memcpy(dst + 84, payload.data, payload.len);
	free(payload.data);
	if (written) *written = total;
	return SAVE_OK;
}

SaveResult game_snapshot_decode(const uint8_t *src, size_t length,
                                GameSnapshot *out, SaveError *error) {
	if (src == NULL || out == NULL) { save_error_set(error, -1, "null argument"); return SAVE_BAD_FORMAT; }
	if (length > POP_SAVE_MAX_DOCUMENT) { save_error_set(error, -1, "document too large"); return SAVE_BAD_FORMAT; }
	if (length < 84) { save_error_set(error, -1, "document too short"); return SAVE_BAD_FORMAT; }

	static const uint8_t magic[8] = {'P','O','P','M','A','C','S','\0'};
	if (memcmp(src, magic, 8) != 0) { save_error_set(error, -1, "bad magic"); return SAVE_BAD_FORMAT; }

	uint32_t version = (uint32_t)src[8] | ((uint32_t)src[9] << 8) | ((uint32_t)src[10] << 16) | ((uint32_t)src[11] << 24);
	if (version != POP_SAVE_FORMAT_VERSION) { save_error_set(error, -1, "unsupported format version"); return SAVE_UNSUPPORTED_VERSION; }

	uint32_t profile = (uint32_t)src[12] | ((uint32_t)src[13] << 8) | ((uint32_t)src[14] << 16) | ((uint32_t)src[15] << 24);
	if (profile != PROFILE_DOS_SAVE_V1) { save_error_set(error, -1, "incompatible gameplay profile"); return SAVE_INCOMPATIBLE_PROFILE; }

	unsigned char archive[32];
	memcpy(archive, pop_archive_sha256, 32);
	if (memcmp(src + 16, archive, 32) != 0) { save_error_set(error, -1, "archive identity mismatch"); return SAVE_INCOMPATIBLE_PROFILE; }

	uint32_t payload_len = (uint32_t)src[48] | ((uint32_t)src[49] << 8) | ((uint32_t)src[50] << 16) | ((uint32_t)src[51] << 24);
	if (length != 84 + (size_t)payload_len) { save_error_set(error, -1, "trailing bytes or bad payload length"); return SAVE_BAD_FORMAT; }

	unsigned char payload_hash[CC_SHA256_DIGEST_LENGTH];
	sha256_bytes(src + 84, payload_len, payload_hash);
	if (memcmp(src + 52, payload_hash, 32) != 0) { save_error_set(error, -1, "payload checksum mismatch"); return SAVE_BAD_FORMAT; }

	// Decode the five sections, each into the output candidate (not live globals).
	GameSnapshot decoded;
	memset(&decoded, 0, sizeof(decoded));
	Cursor c;
	cursor_init(&c, src + 84, payload_len);
	for (int i = 0; i < SEC_COUNT; ++i) {
		uint32_t sec_len;
		if (!get_u32(&c, &sec_len)) { save_error_set(error, (int)i, "missing section"); return SAVE_BAD_FORMAT; }
		if (sec_len > c.len - c.pos) { save_error_set(error, (int)i, "section length overflow"); return SAVE_BAD_FORMAT; }
		Cursor s;
		cursor_init(&s, c.data + c.pos, sec_len);
		SaveResult r = section_decode(i, &s, &decoded);
		if (r != SAVE_OK) { save_error_set(error, (int)i, "section decode failed"); return r; }
		if (s.pos != s.len) { save_error_set(error, (int)i, "section has trailing bytes"); return SAVE_BAD_FORMAT; }
		c.pos += sec_len;
	}
	if (c.pos != c.len) { save_error_set(error, -1, "payload has trailing bytes"); return SAVE_BAD_FORMAT; }

	*out = decoded;
	return SAVE_OK;
}

// ---------------------------------------------------------------------------
// Validation of the decoded candidate
// ---------------------------------------------------------------------------

static SaveResult validate_snapshot(const GameSnapshot *s, SaveError *error) {
	const NativeExtension *n = &s->native;
	if (s->base.current_level >= 15 && s->base.current_level != 15) { save_error_set(error, SEC_BASE, "current_level out of range"); return SAVE_INVALID_STATE; }
	if (s->base.level.used_rooms > ROOMCOUNT) { save_error_set(error, SEC_BASE, "used_rooms out of range"); return SAVE_INVALID_STATE; }
	for (int i = 0; i < ROOMCOUNT; ++i) {
		const link_type *l = &s->base.level.roomlinks[i];
		if (l->left > ROOMCOUNT || l->right > ROOMCOUNT || l->up > ROOMCOUNT || l->down > ROOMCOUNT) {
			save_error_set(error, SEC_BASE, "room link out of range"); return SAVE_INVALID_STATE;
		}
	}
	if (s->base.mobs_count < 0 || s->base.mobs_count > 14) { save_error_set(error, SEC_BASE, "mobs_count out of range"); return SAVE_INVALID_STATE; }
	if (s->base.trobs_count < 0 || s->base.trobs_count > TROBS_MAX) { save_error_set(error, SEC_BASE, "trobs_count out of range"); return SAVE_INVALID_STATE; }
	if ((short)n->current_sound < -1 || n->current_sound >= 58) { save_error_set(error, SEC_NATIVE, "current_sound out of range"); return SAVE_INVALID_STATE; }
	if ((short)n->next_sound < -1 || n->next_sound >= 58) { save_error_set(error, SEC_NATIVE, "next_sound out of range"); return SAVE_INVALID_STATE; }
	// Audio: active resource id domain (0..57 or -1).
	if (s->audio.speaker.resource_id < -1 || s->audio.speaker.resource_id > 57) { save_error_set(error, SEC_AUDIO, "speaker resource out of range"); return SAVE_INVALID_STATE; }
	if (s->audio.digi.resource_id < -1 || s->audio.digi.resource_id > 57) { save_error_set(error, SEC_AUDIO, "digi resource out of range"); return SAVE_INVALID_STATE; }
	if (s->audio.midi.resource_id < -1 || s->audio.midi.resource_id > 57) { save_error_set(error, SEC_AUDIO, "midi resource out of range"); return SAVE_INVALID_STATE; }
	if (s->audio.midi.num_tracks < 0 || s->audio.midi.num_tracks > POP_MIDI_MAX_TRACKS) {
		save_error_set(error, SEC_AUDIO, "midi track count out of range"); return SAVE_INVALID_STATE;
	}
	// OPL symbolic reference domains.
	for (int i = 0; i < 36; ++i) {
		const SerialOplSlot *sl = &s->audio.midi.opl.slot[i];
		if (sl->channel_index >= 18) { save_error_set(error, SEC_AUDIO, "opl slot channel out of range"); return SAVE_INVALID_STATE; }
		if (sl->mod_kind > OPL_REF_ZEROMOD) { save_error_set(error, SEC_AUDIO, "opl slot mod kind invalid"); return SAVE_INVALID_STATE; }
		if ((sl->mod_kind == OPL_REF_SLOT_FBMOD || sl->mod_kind == OPL_REF_SLOT_OUT) && sl->mod_index >= 36) {
			save_error_set(error, SEC_AUDIO, "opl slot mod index out of range"); return SAVE_INVALID_STATE;
		}
		if (sl->trem_kind > 1) { save_error_set(error, SEC_AUDIO, "opl slot trem kind invalid"); return SAVE_INVALID_STATE; }
	}
	for (int i = 0; i < 18; ++i) {
		const SerialOplChannel *ch = &s->audio.midi.opl.channel[i];
		if (ch->slot0 >= 36 || ch->slot1 >= 36) { save_error_set(error, SEC_AUDIO, "opl channel slot out of range"); return SAVE_INVALID_STATE; }
		if (ch->pair >= 18) { save_error_set(error, SEC_AUDIO, "opl channel pair out of range"); return SAVE_INVALID_STATE; }
		for (int k = 0; k < 4; ++k) {
			if (ch->out_kind[k] > OPL_REF_ZEROMOD) { save_error_set(error, SEC_AUDIO, "opl channel out kind invalid"); return SAVE_INVALID_STATE; }
			if (ch->out_kind[k] == OPL_REF_SLOT_OUT && ch->out_index[k] >= 36) {
				save_error_set(error, SEC_AUDIO, "opl channel out index out of range"); return SAVE_INVALID_STATE;
			}
		}
	}
	return SAVE_OK;
}

// ---------------------------------------------------------------------------
// Capture (called at the top of play_level_2, complete-frame boundary)
// ---------------------------------------------------------------------------

// Live seg009.c audio globals.
extern short speaker_playing;
extern speaker_type* current_speaker_sound;
extern int speaker_note_index;
extern int current_speaker_note_samples_already_emitted;
extern float square_wave_samples_since_last_flip;
extern short square_wave_state;
extern short digi_playing;
extern byte* digi_remaining_pos;
extern int digi_remaining_length;
extern byte* digi_buffer;
extern int playing_speaker_id;
extern int playing_digi_id;
extern short ogg_playing;
extern int wait_time[NUM_TIMERS];
extern word need_redraw_because_flipped;

void game_snapshot_capture(GameSnapshot *out) {
	memset(out, 0, sizeof(*out));

	// Base state: copy each field from live globals.
#define POP_BASE_FIELD(name, c_type, kind, decl) memcpy(&out->base.name, &name, sizeof(name));
#include "game_state_fields.def"
#undef POP_BASE_FIELD

	// Native extension fields.
	out->native.exit_room_timer = exit_room_timer;
	out->native.text_time_total = text_time_total;
	out->native.text_time_remaining = text_time_remaining;
	out->native.is_show_time = is_show_time;
	out->native.next_room = next_room;
	out->native.seamless = seamless;
	out->native.roomleave_result = roomleave_result;
	out->native.resurrect_time = resurrect_time;
	out->native.start_level = start_level;
	out->native.dont_reset_time = dont_reset_time;
	out->native.is_restart_level = is_restart_level;
	out->native.seed_was_init = seed_was_init;
	out->native.next_sound = next_sound;
	out->native.current_sound = current_sound;
	out->native.jumped_through_mirror = jumped_through_mirror;
	out->native.is_blind_mode = is_blind_mode;
	out->native.sound_mode = sound_mode;
	out->native.is_sound_on = is_sound_on;
	out->native.enable_music = enable_music;
	out->native.different_room = different_room;
	out->native.need_full_redraw = need_full_redraw;
	out->native.need_redraw_because_flipped = need_redraw_because_flipped;

	// Timer state.
	for (int i = 0; i < NUM_TIMERS; ++i) out->timer.wait_time[i] = wait_time[i];

	audio_state_capture(&out->audio);

	// Last presented 320x200 RGB24 frame, packed row by row without pitch.
	SDL_Surface *surface = get_final_surface();
	if (surface != NULL) {
		const byte *pixels = (const byte *)surface->pixels;
		int pitch = surface->pitch;
		for (int row = 0; row < POP_FRAME_H; ++row) {
			memcpy(out->frame + row * POP_FRAME_W * 3, pixels + row * pitch, POP_FRAME_W * 3);
		}
	}
}

// ---------------------------------------------------------------------------
// Audio capture / prepare / commit (speaker + digi live here; MIDI via midi.c)
// ---------------------------------------------------------------------------

static bool g_panel_suspended = false;

void audio_set_panel_suspended(bool suspended) {
	g_panel_suspended = suspended;
	if (suspended) {
		SDL_PauseAudio(1);
	} else {
		SDL_PauseAudio(0);
	}
}

void audio_state_capture(AudioSnapshot *out) {
	memset(out, 0, sizeof(*out));
	out->speaker.resource_id = -1;
	out->digi.resource_id = -1;
	out->midi.resource_id = -1;
	SDL_LockAudio();
	out->speaker.playing = (speaker_playing != 0);
	out->speaker.resource_id = playing_speaker_id;
	out->speaker.note_index = speaker_note_index;
	out->speaker.samples_emitted_in_note = current_speaker_note_samples_already_emitted;
	out->speaker.square_phase = square_wave_samples_since_last_flip;
	out->speaker.square_state = square_wave_state;

	out->digi.playing = (digi_playing != 0);
	out->digi.resource_id = playing_digi_id;
	if (digi_buffer != NULL && digi_remaining_pos != NULL) {
		out->digi.byte_offset = (uint32_t)(digi_remaining_pos - digi_buffer);
	} else {
		out->digi.byte_offset = 0;
	}
	out->digi.remaining_length = (uint32_t)(digi_remaining_length > 0 ? digi_remaining_length : 0);

	midi_snapshot_capture(&out->midi);
	SDL_UnlockAudio();
}

static bool valid_sound_id(int16_t id) { return id >= 0 && id < 58; }

SaveResult audio_state_prepare(const AudioSnapshot *src,
                               PreparedAudioState *out, SaveError *error) {
	memset(out, 0, sizeof(*out));
	out->speaker = src->speaker;
	out->digi = src->digi;
	out->midi = src->midi;

	// Validate and resolve speaker/digi resources without mutating live audio.
	if (src->speaker.playing) {
		if (!valid_sound_id(src->speaker.resource_id)) { save_error_set(error, SEC_AUDIO, "speaker resource invalid"); return SAVE_INVALID_STATE; }
		if (sound_pointers[src->speaker.resource_id] == NULL ||
		    sound_pointers[src->speaker.resource_id]->type != sound_speaker) {
			save_error_set(error, SEC_AUDIO, "speaker resource is not a speaker sound"); return SAVE_PREPARE_FAILED;
		}
	}
	if (src->digi.playing) {
		if (!valid_sound_id(src->digi.resource_id)) { save_error_set(error, SEC_AUDIO, "digi resource invalid"); return SAVE_INVALID_STATE; }
		sound_buffer_type *buf = sound_pointers[src->digi.resource_id];
		if (buf == NULL || (buf->type & 7) != sound_digi_converted) {
			save_error_set(error, SEC_AUDIO, "digi resource is not a converted digital sound"); return SAVE_PREPARE_FAILED;
		}
		if (src->digi.byte_offset > (uint32_t)buf->converted.length) {
			save_error_set(error, SEC_AUDIO, "digi byte offset out of range"); return SAVE_INVALID_STATE;
		}
		if (src->digi.remaining_length > (uint32_t)buf->converted.length - src->digi.byte_offset) {
			save_error_set(error, SEC_AUDIO, "digi remaining length out of range"); return SAVE_INVALID_STATE;
		}
	}
	SaveResult r = midi_snapshot_prepare(&src->midi, error);
	if (r != SAVE_OK) return r;
	return SAVE_OK;
}

void audio_state_commit(PreparedAudioState *prepared) {
	SDL_LockAudio();
	if (prepared->speaker.playing) {
		current_speaker_sound = &sound_pointers[prepared->speaker.resource_id]->speaker;
		speaker_note_index = prepared->speaker.note_index;
		current_speaker_note_samples_already_emitted = prepared->speaker.samples_emitted_in_note;
		speaker_playing = 1;
		playing_speaker_id = prepared->speaker.resource_id;
	} else {
		speaker_playing = 0;
		current_speaker_sound = NULL;
		speaker_note_index = 0;
		current_speaker_note_samples_already_emitted = 0;
		playing_speaker_id = -1;
	}
	square_wave_samples_since_last_flip = prepared->speaker.square_phase;
	square_wave_state = prepared->speaker.square_state;

	if (prepared->digi.playing) {
		byte *base = (byte *)sound_pointers[prepared->digi.resource_id]->converted.samples;
		digi_buffer = base;
		digi_remaining_pos = base + prepared->digi.byte_offset;
		digi_remaining_length = prepared->digi.remaining_length;
		digi_playing = 1;
		playing_digi_id = prepared->digi.resource_id;
	} else {
		digi_playing = 0;
		digi_buffer = NULL;
		digi_remaining_pos = NULL;
		digi_remaining_length = 0;
		playing_digi_id = -1;
	}

	midi_snapshot_commit(&prepared->midi);
	SDL_UnlockAudio();
}

// ---------------------------------------------------------------------------
// Prepare / commit (gameplay resources)
// ---------------------------------------------------------------------------

// Implemented in seg000.c (refactored load_lev_spr preparation paths).
SaveResult prepare_level_graphics(int level, PreparedLevelGraphics *out, SaveError *error);

void commit_level_graphics(const PreparedLevelGraphics *prepared);

// Install captured gameplay globals from the snapshot into live state.
static void commit_game_state(const GameSnapshot *s) {
#define POP_BASE_FIELD(name, c_type, kind, decl) memcpy(&name, &(s->base.name), sizeof(name));
#include "game_state_fields.def"
#undef POP_BASE_FIELD

	exit_room_timer = s->native.exit_room_timer;
	text_time_total = s->native.text_time_total;
	text_time_remaining = s->native.text_time_remaining;
	is_show_time = s->native.is_show_time;
	next_room = s->native.next_room;
	seamless = s->native.seamless;
	roomleave_result = s->native.roomleave_result;
	resurrect_time = s->native.resurrect_time;
	start_level = s->native.start_level;
	dont_reset_time = s->native.dont_reset_time;
	is_restart_level = s->native.is_restart_level;
	seed_was_init = s->native.seed_was_init;
	next_sound = s->native.next_sound;
	current_sound = s->native.current_sound;
	jumped_through_mirror = s->native.jumped_through_mirror;
	is_blind_mode = s->native.is_blind_mode;
	sound_mode = s->native.sound_mode;
	is_sound_on = s->native.is_sound_on;
	enable_music = s->native.enable_music;
	different_room = s->native.different_room;
	need_full_redraw = s->native.need_full_redraw;
	need_redraw_because_flipped = s->native.need_redraw_because_flipped;

	for (int i = 0; i < NUM_TIMERS; ++i) wait_time[i] = s->timer.wait_time[i];
}

SaveResult game_snapshot_prepare(const GameSnapshot *src,
                                 PreparedSnapshot *out, SaveError *error) {
	memset(out, 0, sizeof(*out));
	out->snapshot = *src;

	SaveResult r = validate_snapshot(src, error);
	if (r != SAVE_OK) return r;

	if (src->base.current_level >= 0 && src->base.current_level < 16) {
		out->level_graphics = calloc(1, sizeof(PreparedLevelGraphics));
		if (out->level_graphics == NULL) { save_error_set(error, -1, "out of memory"); return SAVE_IO_ERROR; }
		r = prepare_level_graphics(src->base.current_level, out->level_graphics, error);
		if (r != SAVE_OK) { free(out->level_graphics); out->level_graphics = NULL; return r; }
	}

	r = audio_state_prepare(&src->audio, &out->audio, error);
	if (r != SAVE_OK) {
		if (out->level_graphics) { free(out->level_graphics); out->level_graphics = NULL; }
		return r;
	}
	return SAVE_OK;
}

// Restore-only presentation state.
static byte g_restore_frame[POP_FRAME_RGB_BYTES];
bool snapshot_first_redraw = false;

void game_snapshot_commit(PreparedSnapshot *prepared) {
	if (prepared->level_graphics != NULL) {
		commit_level_graphics(prepared->level_graphics);
	}
	commit_game_state(&prepared->snapshot);
	audio_state_commit(&prepared->audio);
	memcpy(g_restore_frame, prepared->snapshot.frame, POP_FRAME_RGB_BYTES);
}

// Restore-only presentation: rebind room/door/tile pointers from the restored
// state, request the first ordinary frame to rebuild drawing caches, and
// blit the saved RGB24 frame + present once. No timers/gameplay/mirror/death
// side effects run here.
void redraw_loaded_snapshot(void) {
	doorlink1_ad = &level.doorlinks1[0];
	doorlink2_ad = &level.doorlinks2[0];
	load_room_links();
	get_room_address(drawn_room);
	// First ordinary frame after load rebuilds drawing caches; redraw_screen()
	// suppresses its exit_room_timer reset during that restore-triggered redraw.
	snapshot_first_redraw = true;
	need_full_redraw = 1;
	different_room = 0;

	SDL_Surface *surface = get_final_surface();
	if (surface != NULL) {
		byte *pixels = (byte *)surface->pixels;
		int pitch = surface->pitch;
		for (int row = 0; row < POP_FRAME_H; ++row) {
			memcpy(pixels + (size_t)row * (size_t)pitch,
			       g_restore_frame + (size_t)row * POP_FRAME_W * 3,
			       POP_FRAME_W * 3);
		}
	}
	update_screen();
}

// Disposes candidate-owned objects without installing any global state.
void game_snapshot_dispose(PreparedSnapshot *prepared) {
	if (prepared->level_graphics != NULL) {
		free(prepared->level_graphics);
		prepared->level_graphics = NULL;
	}
	midi_snapshot_dispose();
}

// ---------------------------------------------------------------------------
// Atomic file write / load
// ---------------------------------------------------------------------------

static SaveResult read_file_bytes(const char *path, uint8_t **out_data, size_t *out_len, SaveError *error) {
	FILE *f = fopen(path, "rb");
	if (f == NULL) { save_error_set(error, -1, "cannot open file for reading"); return SAVE_IO_ERROR; }
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); save_error_set(error, -1, "cannot seek"); return SAVE_IO_ERROR; }
	long sz = ftell(f);
	if (sz < 0 || (unsigned long)sz > POP_SAVE_MAX_DOCUMENT) { fclose(f); save_error_set(error, -1, "file too large"); return SAVE_BAD_FORMAT; }
	rewind(f);
	uint8_t *buf = malloc(sz ? (size_t)sz : 1);
	if (buf == NULL) { fclose(f); save_error_set(error, -1, "out of memory"); return SAVE_IO_ERROR; }
	size_t got = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	if (got != (size_t)sz) { free(buf); save_error_set(error, -1, "short read"); return SAVE_IO_ERROR; }
	*out_data = buf;
	*out_len = (size_t)sz;
	return SAVE_OK;
}

// Sibling temporary + fsync + atomic rename. A failure before the rename
// leaves any existing destination byte-for-byte intact.
static SaveResult atomic_write_file(const char *path, const uint8_t *data, size_t len, SaveError *error) {
	char tmp[PATH_MAX + 16];
	snprintf(tmp, sizeof(tmp), "%s.tmpXXXXXX", path);
	int fd = mkstemp(tmp);
	if (fd < 0) { save_error_set(error, -1, "cannot create temporary file"); return SAVE_IO_ERROR; }
	size_t off = 0;
	bool ok = true;
	while (off < len) {
		ssize_t n = write(fd, data + off, len - off);
		if (n < 0) {
			if (errno == EINTR) continue;
			ok = false; break;
		}
		off += (size_t)n;
	}
	if (ok && fsync(fd) != 0) ok = false;
	if (close(fd) != 0) ok = false;
	if (!ok) { unlink(tmp); save_error_set(error, -1, "write failed"); return SAVE_IO_ERROR; }
	if (rename(tmp, path) != 0) { unlink(tmp); save_error_set(error, -1, "rename failed"); return SAVE_IO_ERROR; }
	return SAVE_OK;
}

SaveResult game_save_file(const char *utf8_path, SaveError *error) {
	GameSnapshot snap;
	game_snapshot_capture(&snap);

	uint8_t buf[POP_SAVE_MAX_DOCUMENT];
	size_t written = 0;
	SaveResult r = game_snapshot_encode(&snap, buf, sizeof(buf), &written, error);
	if (r != SAVE_OK) return r;
	return atomic_write_file(utf8_path, buf, written, error);
}

SaveResult game_load_file_prepare(const char *utf8_path,
                                  PreparedSnapshot *out, SaveError *error) {
	uint8_t *data = NULL;
	size_t len = 0;
	SaveResult r = read_file_bytes(utf8_path, &data, &len, error);
	if (r != SAVE_OK) return r;
	GameSnapshot snap;
	r = game_snapshot_decode(data, len, &snap, error);
	free(data);
	if (r != SAVE_OK) return r;
	return game_snapshot_prepare(&snap, out, error);
}
