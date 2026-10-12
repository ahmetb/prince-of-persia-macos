#!/usr/bin/env python3
"""
savetool.py — inspect and edit Prince of Persia native save files (.popsave).

Self-contained (Python stdlib only). Understands the .popsave v1 binary format
and lets you dump a save's key fields, back it up, and add minutes to the
remaining time.

Subcommands:
  dump   <file...>            print level / remaining time / room / HP
  backup <file>               copy <file> into a sibling backups/ dir (timestamped)
  extend <file> [minutes=10]  bump remaining minutes (rem_min) by `minutes`

Format (v1):
  [0..7]    "POPMACS\\0"
  [8..11]   uint32 LE format version (=1)
  [12..15]  uint32 LE profile id (=1)
  [16..47]  32 bytes archive SHA-256 (game-asset identity)
  [48..51]  uint32 LE payload length
  [52..83]  32 bytes SHA-256 of the payload
  payload   five sections, each uint32 LE length then data:
             0 base state, 1 native extension, 2 timer state, 3 audio state,
             4 RGB frame (320*200*3 = 192000 bytes)

`rem_min` (remaining minutes; counts down) and `rem_tick` (remaining ticks,
720/minute) live in section 0 (base state) as 2-byte little-endian values.
Editing recomputes the payload SHA-256 so the file remains valid for the game.
"""

import os
import sys
import struct
import hashlib
import datetime
import shutil

MAGIC = b"POPMACS\x00"
HDR = 84

# Base-section (section 0) field layout, exact encode order. (name, size_bytes).
BASE_FIELDS = [
    ("level", 2305),
    ("checkpoint", 2),
    ("upside_down", 2),
    ("drawn_room", 2),
    ("current_level", 2),
    ("next_level", 2),
    ("mobs_count", 2),
    ("mobs", 84),
    ("trobs_count", 2),
    ("trobs", 90),
    ("leveldoor_open", 2),
    ("Kid", 16),          # char_type
    ("hitp_curr", 2),
    ("hitp_max", 2),
    ("hitp_beg_lev", 2),
    ("grab_timer", 2),
    ("holding_sword", 2),
    ("united_with_shadow", 2),
    ("have_sword", 2),
    ("kid_sword_strike", 2),
    ("pickup_obj_type", 2),
    ("offguard", 2),
    ("Guard", 16),        # char_type
    ("Char", 16),         # char_type
    ("Opp", 16),          # char_type
    ("guardhp_curr", 2),
    ("guardhp_max", 2),
    ("demo_index", 2),
    ("demo_time", 2),
    ("curr_guard_color", 2),
    ("guard_notice_timer", 2),
    ("guard_skill", 2),
    ("shadow_initialized", 2),
    ("guard_refrac", 2),
    ("justblocked", 2),
    ("droppedout", 2),
    ("curr_row_coll_room", 10),
    ("curr_row_coll_flags", 10),
    ("below_row_coll_room", 10),
    ("below_row_coll_flags", 10),
    ("above_row_coll_room", 10),
    ("above_row_coll_flags", 10),
    ("prev_collision_row", 1),
    ("flash_color", 2),
    ("flash_time", 2),
    ("need_level1_music", 2),
    ("is_screaming", 2),
    ("is_feather_fall", 2),
    ("last_loose_sound", 2),
    ("random_seed", 4),
    ("rem_min", 2),
    ("rem_tick", 2),
]
# Within char_type, `room` is the 10th byte (index 9).
KID_ROOM_INDEX = 9

_BASE_OFFSETS = {}
_running = 0
for _name, _size in BASE_FIELDS:
    _BASE_OFFSETS[_name] = _running
    _running += _size
_BASE_TOTAL = _running


def _fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def _read(path):
    with open(path, "rb") as f:
        return f.read()


def _sections(path):
    """Returns (payload_bytes, [sec0, sec1, sec2, sec3, sec4])."""
    data = _read(path)
    if len(data) < HDR:
        _fail(f"{path}: too short ({len(data)} bytes)")
    if data[:8] != MAGIC:
        _fail(f"{path}: bad magic (not a .popsave file)")
    version = struct.unpack("<I", data[8:12])[0]
    if version != 1:
        _fail(f"{path}: unsupported format version {version}")
    profile = struct.unpack("<I", data[12:16])[0]
    if profile != 1:
        _fail(f"{path}: unsupported profile {profile}")
    payload_len = struct.unpack("<I", data[48:52])[0]
    if HDR + payload_len != len(data):
        _fail(f"{path}: payload length mismatch / trailing bytes")
    payload = data[HDR:]
    stored = data[52:84]
    if hashlib.sha256(payload).digest() != stored:
        _fail(f"{path}: payload checksum mismatch (corrupt)")

    sections = []
    pos = 0
    for _ in range(5):
        if pos + 4 > len(payload):
            _fail(f"{path}: truncated section header")
        n = struct.unpack("<I", payload[pos:pos + 4])[0]
        pos += 4
        sections.append(payload[pos:pos + n])
        pos += n
    if pos != len(payload):
        _fail(f"{path}: sections do not cover the payload")
    return payload, sections


def snapshot_fields(path):
    _, sections = _sections(path)
    base = sections[0]
    kid_off = _BASE_OFFSETS["Kid"]
    return {
        "level": struct.unpack("<H", base[_BASE_OFFSETS["current_level"]:
                                        _BASE_OFFSETS["current_level"] + 2])[0],
        "rem_min": struct.unpack("<H", base[_BASE_OFFSETS["rem_min"]:
                                          _BASE_OFFSETS["rem_min"] + 2])[0],
        "rem_tick": struct.unpack("<H", base[_BASE_OFFSETS["rem_tick"]:
                                           _BASE_OFFSETS["rem_tick"] + 2])[0],
        "room": base[kid_off + KID_ROOM_INDEX],
        "hitp": struct.unpack("<H", base[_BASE_OFFSETS["hitp_curr"]:
                                       _BASE_OFFSETS["hitp_curr"] + 2])[0],
    }


def cmd_dump(paths):
    for path in paths:
        if not os.path.exists(path):
            print(f"{path}: not found")
            continue
        try:
            f = snapshot_fields(path)
        except SystemExit:
            print(f"{path}: not a valid save (skipped)")
            continue
        mins = max(0, f["rem_min"])
        shown = max(0, mins - 1)
        secs = f["rem_tick"] // 12
        print(f"{path}: level={f['level']} rem_min={f['rem_min']} "
              f"shown={shown:02d}:{secs:02d} room={f['room']} hitp={f['hitp']}")


def cmd_backup(path):
    if not os.path.exists(path):
        _fail(f"{path}: not found")
    d = os.path.join(os.path.dirname(os.path.abspath(path)), "backups")
    os.makedirs(d, exist_ok=True)
    base = os.path.basename(path)
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    dest = os.path.join(d, f"{base}.backup-{ts}")
    shutil.copy2(path, dest)
    print(f"backed up -> {dest}")


def cmd_extend(path, minutes=10):
    if not os.path.exists(path):
        _fail(f"{path}: not found")
    data = bytearray(_read(path))
    payload = bytearray(data[HDR:])

    # Find the base section's data start within the payload.
    base_start = None
    pos = 0
    for i in range(5):
        n = struct.unpack("<I", payload[pos:pos + 4])[0]
        pos += 4
        if i == 0:
            base_start = pos
        pos += n
    if base_start is None or pos != len(payload):
        _fail(f"{path}: bad section framing")

    off_min = base_start + _BASE_OFFSETS["rem_min"]
    old_min = struct.unpack("<H", payload[off_min:off_min + 2])[0]
    new_min = (old_min + minutes) & 0xFFFF
    struct.pack_into("<H", payload, off_min, new_min)

    # Recompute the payload SHA-256 (header bytes 52..84).
    new_hash = hashlib.sha256(bytes(payload)).digest()
    out = bytearray(data)
    out[HDR:] = payload
    out[52:84] = new_hash

    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    print(f"{path}: rem_min {old_min} -> {new_min} (+{minutes} min; "
          f"{os.path.getsize(path)} bytes)")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    args = sys.argv[2:]
    if cmd == "dump" and args:
        cmd_dump(args)
    elif cmd == "backup" and len(args) == 1:
        cmd_backup(args[0])
    elif cmd == "extend" and len(args) in (1, 2):
        minutes = int(args[1]) if len(args) == 2 else 10
        cmd_extend(args[0], minutes)
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
