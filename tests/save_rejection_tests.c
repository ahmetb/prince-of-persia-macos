/*
Save codec rejection tests: malformed / truncated / wrong-version / profile /
checksum / semantic-invalid documents must reject without touching live
globals. The semantic checks are exercised independently of the checksum by
re-signing (re-encoding) the tampered payload first.
*/

#include "common.h"
#include "game_snapshot.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, what) do { \
	if (!(cond)) { printf("FAIL: %s\n", what); failures++; } \
	else { printf("ok: %s\n", what); } \
} while (0)

static void fill_valid_snapshot(GameSnapshot *s) {
	memset(s, 0, sizeof(*s));
	s->base.current_level = 3;
	s->base.level.used_rooms = 7;
	s->base.level.roomlinks[0].left = 1;
	s->base.level.roomlinks[0].right = 24;
	s->base.mobs_count = 1;
	s->base.trobs_count = 2;
	s->base.random_seed = 0x12345678;
	s->base.rem_min = 59;
	s->base.rem_tick = 700;
	s->base.Kid.room = 6;
	s->base.Kid.x = 100;
	s->base.Kid.curr_seq = 9;
	s->native.exit_room_timer = 2;
	s->native.next_sound = -1;
	s->native.current_sound = 12;
	s->native.different_room = 0;
	s->audio.speaker.resource_id = -1;
	s->audio.digi.resource_id = -1;
	s->audio.midi.resource_id = -1;
	s->audio.midi.playing = 0;
}

int main(void) {
	GameSnapshot s, d;
	SaveError err;
	SaveResult r;
	uint8_t buf[POP_SAVE_MAX_DOCUMENT];
	size_t written = 0;

	fill_valid_snapshot(&s);
	r = game_snapshot_encode(&s, buf, sizeof(buf), &written, &err);
	CHECK(r == SAVE_OK, "encode valid snapshot");
	CHECK(written >= 84 && written < POP_SAVE_MAX_DOCUMENT, "document within size bounds");
	CHECK(memcmp(buf, "POPMACS\0", 8) == 0, "magic bytes present");

	memset(&d, 0xFF, sizeof(d));
	r = game_snapshot_decode(buf, written, &d, &err);
	CHECK(r == SAVE_OK, "decode valid document");
	CHECK(memcmp(&s, &d, sizeof(s)) == 0, "round-trip byte equality");

	// Truncation.
	r = game_snapshot_decode(buf, 83, &d, &err);
	CHECK(r == SAVE_BAD_FORMAT, "short document rejected");
	r = game_snapshot_decode(buf, 90, &d, &err);
	CHECK(r == SAVE_BAD_FORMAT, "truncated document rejected");

	// Bad magic.
	uint8_t bad_magic[POP_SAVE_MAX_DOCUMENT];
	memcpy(bad_magic, buf, written);
	bad_magic[0] = 'X';
	r = game_snapshot_decode(bad_magic, written, &d, &err);
	CHECK(r == SAVE_BAD_FORMAT, "bad magic rejected");

	// Wrong version.
	memcpy(bad_magic, buf, written);
	bad_magic[8] = 2;
	r = game_snapshot_decode(bad_magic, written, &d, &err);
	CHECK(r == SAVE_UNSUPPORTED_VERSION, "wrong version rejected");

	// Wrong profile.
	memcpy(bad_magic, buf, written);
	bad_magic[12] = 9;
	r = game_snapshot_decode(bad_magic, written, &d, &err);
	CHECK(r == SAVE_INCOMPATIBLE_PROFILE, "wrong profile rejected");

	// Payload checksum mismatch.
	memcpy(bad_magic, buf, written);
	bad_magic[120] ^= 0x01;
	r = game_snapshot_decode(bad_magic, written, &d, &err);
	CHECK(r == SAVE_BAD_FORMAT, "payload checksum mismatch rejected");

	// Trailing bytes.
	memcpy(bad_magic, buf, written);
	bad_magic[written] = 0x00;
	r = game_snapshot_decode(bad_magic, written + 1, &d, &err);
	CHECK(r == SAVE_BAD_FORMAT, "trailing bytes rejected");

	// Semantic-invalid counts/indices, re-signed so checksum is valid:
	// used_rooms out of range.
	fill_valid_snapshot(&s);
	s.base.level.used_rooms = 200;
	r = game_snapshot_encode(&s, buf, sizeof(buf), &written, &err);
	CHECK(r == SAVE_OK, "encode semantic-invalid snapshot (re-signed)");
	r = game_snapshot_decode(buf, written, &d, &err);
	CHECK(r == SAVE_OK, "decode re-signed semantic-invalid (structurally ok)");
	PreparedSnapshot prep;
	r = game_snapshot_prepare(&d, &prep, &err);
	CHECK(r == SAVE_INVALID_STATE, "used_rooms>24 rejected at prepare");

	// Room link out of range.
	fill_valid_snapshot(&s);
	s.base.level.roomlinks[3].down = 200;
	r = game_snapshot_encode(&s, buf, sizeof(buf), &written, &err);
	r = game_snapshot_decode(buf, written, &d, &err);
	r = game_snapshot_prepare(&d, &prep, &err);
	CHECK(r == SAVE_INVALID_STATE, "room link >24 rejected at prepare");

	// mobs_count out of range.
	fill_valid_snapshot(&s);
	s.base.mobs_count = 99;
	r = game_snapshot_encode(&s, buf, sizeof(buf), &written, &err);
	r = game_snapshot_decode(buf, written, &d, &err);
	r = game_snapshot_prepare(&d, &prep, &err);
	CHECK(r == SAVE_INVALID_STATE, "mobs_count>14 rejected at prepare");

	// Audio resource id out of range (speaker).
	fill_valid_snapshot(&s);
	s.audio.speaker.resource_id = 300;
	r = game_snapshot_encode(&s, buf, sizeof(buf), &written, &err);
	r = game_snapshot_decode(buf, written, &d, &err);
	r = game_snapshot_prepare(&d, &prep, &err);
	CHECK(r == SAVE_INVALID_STATE, "speaker resource>57 rejected at prepare");

	printf("%s: %d failures\n", failures ? "FAILED" : "PASSED", failures);
	return failures != 0;
}
