# Prince of Persia: native Apple Silicon macOS app with Metal and exact-state saves

## Context
Create a native Apple Silicon macOS app for DOS Prince of Persia 1, using the game represented by https://archive.org/details/PERSIA_VGA as the asset source and visual reference. Reuse SDLPoP's reverse-engineered C engine, present through SDL2's actual Metal backend, and add persistent named exact-state Save/Load plus quicksave/quickload. First launch must obtain explicit consent before downloading the original assets; the resulting app must launch independently of DOSBox, Rosetta, Homebrew runtime libraries, and its working directory.

The workspace was empty during planning. This is a source port, not translation or execution of `PRINCE.EXE`: SDLPoP derives from DOS PoP 1.0 disassembly. The archive executable is 126304 bytes and is not recognized by upstream's executable-customization size table; use the archive's DAT resources with the stock engine rather than claiming binary-identical behavior for that unidentified EXE.

## Approach

Execute steps in order. Steps 5–6 depend on the state/audio contract in step 4; do not wire a destructive legacy loader into the UI as an interim implementation.

### 1. Establish the native, self-contained app build

- Import SDLPoP source from **commit `3c5add5fb7f83d4ceb542823ab66d00146c4271b`** into this project's `src/`, retaining source attribution and `COPYING`. Preserve the relevant `doc/replays-testcases/` fixtures for regression checks. Do not import upstream `data/`, mods, prebuilt executables, or platform-specific distribution artifacts. Source evidence: https://github.com/NagyD/SDLPoP/tree/3c5add5fb7f83d4ceb542823ab66d00146c4271b . The engine and app modifications are GPL-3.0-or-later; original game assets are separately copyrighted.
- Make a new root `CMakeLists.txt` the native build entry point, with CMake >=3.24, C99 and Objective-C, out-of-source output, `CMAKE_OSX_ARCHITECTURES=arm64`, and deployment target `11.0`. Adapt `src/CMakeLists.txt` to supply the existing explicit game source list to this build; remove its old `CREATE_BUNDLE` recipe, source-directory binary output, `copy_directory(data)` behavior, and bare SDL library linkage rather than retaining two competing Mac bundle recipes.
- Build SDL2 **2.32.10** and SDL2_image **2.8.8** statically using CMake FetchContent, with these fixed source archives and hashes:

  | Dependency | URL | SHA-256 |
  |---|---|---|
  | SDL2 | https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz | `5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165` |
  | SDL2_image | https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.8/SDL2_image-2.8.8.tar.gz | `2213b56fdaff2220d0e38c8e420cbe1a83c87374190cba8c70af2156097ce30a` |

- SDL settings: `SDL_SHARED=OFF`, `SDL_STATIC=ON`, `SDL_TEST=OFF`, `SDL_TESTS=OFF`, `SDL2_DISABLE_SDL2MAIN=ON`, Cocoa/Metal rendering enabled; retain CoreAudio and controller support. SDL_image settings: static build, samples/tests/install/vendored dependencies disabled, `SDL2IMAGE_BACKEND_STB=ON`, `SDL2IMAGE_BACKEND_IMAGEIO=OFF`, PNG load/save enabled, other image format options disabled. STB supports the upstream indexed PNG/screenshot paths without external image-codec dylibs. Link `SDL2::SDL2-static`, `SDL2_image::SDL2_image-static`, and system AppKit/Foundation/required propagated frameworks. No SDL_mixer or external MIDI synthesizer is needed.
- Change the native include convention in `src/types.h` to `<SDL.h>` and `<SDL_image.h>`, using target-provided include directories. The in-tree SDL_image target does not provide upstream's assumed `<SDL2/SDL_image.h>` layout.
- Produce `build/Prince of Persia.app`, executable `Contents/MacOS/PrinceOfPersia`, bundle ID **`local.princeofpersia.metal`**, numeric app version **`1.0.0`**, `NSHighResolutionCapable=YES`, and a new `macos/Info.plist.in`. Bundle only app-owned resources: default configuration and license notices/help; use the default macOS app icon rather than importing game artwork. Use `.popsave` filename filters in native panels; do not advertise Finder document opening, DOS executables, or replay files as app documents.
- Use an ordinary main-thread C `main`; no Swift runtime or custom Metal shader compiler is required. Apply ARC only to new Objective-C files. SDL's Metal library is already embedded in SDL's source. Add a final app-target POST_BUILD command `/usr/bin/codesign --force --sign - <app-bundle>` after resources are packaged. Developer ID signing/notarization is not a prerequisite for local execution.

### 2. Gate asset acquisition before game startup

Create `src/macos_app.h` and `src/macos_app.m`; no existing upstream component provides consent/download/progress UI. Add **`bool macos_prepare_application(void)`** to the main-thread startup path in `src/main.c`, after storing arguments and before `pop_main()`.

- Initialize SDL video first and let it create `SDLApplication`, its delegate, and Cocoa event pumping. Do not create a competing NSApplication, replace SDL's application delegate, or move the game to a worker thread. Native bootstrap runs under an autorelease pool. `set_gr_mode()` later initializes remaining timer/controller/haptic subsystems without reinitializing video.
- Resolve Application Support using Foundation, not `$HOME/.SDLPoP` or the current directory. Establish these roots beneath `~/Library/Application Support/local.princeofpersia.metal/`:
  - `GameData/`: validated original DAT files and an install identity manifest.
  - `Settings/`: `SDLPoP.ini` and `SDLPoP.cfg`.
  - `Saves/`: default location for named files and fixed `Quicksave.popsave`.
  - `Replays/`, `Screenshots/`, and `Scores/`: existing engine outputs if used.
- Before **any** asset-source network request, present native disclosure with the Archive.org item URL and approximate 345 KiB download size. Explain that original game assets remain copyrighted, the project does not grant a game-content license, and the user must have permission to download/use them. Require an unchecked acknowledgement and explicit **Download Game Data** action; provide **Quit**. Do not invent a rights-holder EULA or label the assets GPL/freeware. Cancellation quits without network activity or game startup.
- Fetch exactly `https://archive.org/download/PERSIA_VGA/PERSIA.zip` using NSURLSession, with visible progress and Cancel. Allow ordinary HTTPS redirects used by Archive.org; reject a downgrade to HTTP. Keep the Cocoa run loop responsive; never wait on the main thread for a completion scheduled to that same thread.
- Require HTTP success, **353211 bytes**, and SHA-256 **`99973555d3a2db46d0a7be1dcf17f4834e85d0b4d0bca2adb6bad48ac333db40`** before extracting. The planning session downloaded this payload into memory, verified its metadata SHA-1 `8705853e34426ed216fe2769ddcacb61f16513a3`, and checked all ZIP CRCs; it contains 29 flat members totaling 529159 uncompressed bytes.
- Extract the authenticated, pinned ZIP into an app-owned staging directory with `NSTask` invoking **`/usr/bin/ditto -x -k <verified-zip> <staging>`**, passing separate arguments, never a shell command string. This avoids a new ZIP parser/library dependency; the active SDK did not expose libarchive headers during planning. Never invoke ditto on bytes that have not passed the exact size/hash check. Copy only these **21 regular uppercase DAT files** from staging into the install candidate:
  - `PRINCE.DAT`, `LEVELS.DAT`, `KID.DAT`, `TITLE.DAT`, `PV.DAT`.
  - `VDUNGEON.DAT`, `VPALACE.DAT`.
  - `GUARD.DAT`, `GUARD1.DAT`, `GUARD2.DAT`, `FAT.DAT`, `SKEL.DAT`, `VIZIER.DAT`, `SHADOW.DAT`.
  - `IBM_SND1.DAT`, `IBM_SND2.DAT`, `DIGISND1.DAT`, `DIGISND2.DAT`, `DIGISND3.DAT`, `MIDISND1.DAT`, `MIDISND2.DAT`.
- Validate the DAT table structure before publishing: six-byte little-endian header, table bounds, entry count/entry bounds, and each resource's checksum-byte-plus-payload bounds. Reuse the actual layouts in `src/types.h:dat_header_type/dat_table_type`, not arbitrary minimum file sizes: GUARD1/2 are legitimately just 117 bytes. Record file lengths and SHA-256 hashes in the install manifest, bound to the archive hash, and verify them on later startup. Treat newer-version `PRINCE.DAT` resource 20 as optional, not an import failure.
- Publish the complete candidate directory/manifest by same-volume rename only after validation. A cancellation, network/checksum/extraction/write failure must leave any prior valid install untouched. Show the specific error with explicit Retry/Quit; no silent retries or substituted assets. Persist a completed install independently of consent, so an interrupted download is not considered installed. Once installed, launch offline without a network check. On missing/corrupt installed files, offer consent-gated repair; never silently redownload.
- Discard DOS installers/executables and unused CGA/EGA files with the staging directory; do not execute or retain `PRINCE.EXE` as a runtime requirement. This feature downloads on the user's request and does not authorize public redistribution of assets.

### 3. Isolate resources and require actual Metal rendering

- Use a narrow checked path API in `macos_app.h`:
  **`bool macos_copy_path(MacosPathRoot root, const char *relative, char *out, size_t capacity)`**, where roots are `MACOS_GAME_DATA`, `MACOS_RESOURCES`, `MACOS_SETTINGS`, `MACOS_SAVES`, `MACOS_REPLAYS`, `MACOS_SCREENSHOTS`, and `MACOS_SCORES`. Cache root strings at startup; return an error rather than truncate. Raise native `POP_MAX_PATH` to macOS `PATH_MAX`; panel-selected document paths remain owned UTF-8 strings, not borrowed alloca buffers.
- In `src/seg009.c:open_dat_from_root_or_data_dir/open_dat/load_from_opendats_metadata`, route original DATs exclusively to `MACOS_GAME_DATA` and disable their CWD/home/share/executable/PNG-stem-directory fallback. Keep optional built-in font fallback from `load_font()`; there is no need to bundle `data/font`. Missing optional resources remain optional; a missing required original DAT is a native startup error.
- Route `locate_file_` configuration reads and `locate_save_file_` configuration writes to the same Settings root. Update `src/menu.c:load_ingame_settings/save_ingame_settings`, `src/seg000.c:get_writable_file_path` for HOF, `src/screenshot.c:init_screenshot`, and `replays_folder` initialization to their explicit roots. Preserve the existing INI-versus-CFG mtime precedence. Canonicalize `g_argv[0]` from the bundle executable URL for the existing settings executable-CRC logic. Do not use `chdir` as resource resolution or let `SDLPOP_SAVE_PATH` redirect native app storage.
- `src/options.c:load_global_options()` must not scan the CWD for DOS executable modifications. Keep the original levelset active; do not expose upstream mods as an alternate asset source for this app. Initialize a default Settings INI once from app Resources; existing user configuration is not overwritten on launch.
- In `src/seg009.c:set_gr_mode`, enumerate render drivers, select the index whose name is **`metal`**, create the renderer with `SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE`, and verify `SDL_GetRendererInfo().name` is `metal`. Check window/renderer/texture creation before use. Missing/failed Metal produces a native error and exits, not OpenGL or software fallback. A render-driver hint alone is insufficient: SDL tries other drivers when a preferred driver fails.
- Keep **VSync off**, matching upstream's explicit timing requirement. Preserve the existing fixed game-timer/base/fight cadence; do not tie simulation to display refresh or change gameplay speed for 60/120 Hz displays. Default to corrected **4:3**, sharp nearest-neighbor filtering, and a **640×480** starting window. Preserve Retina-aware drawable sizing, resize/letterboxing, fullscreen, and available sharp/fuzzy/blurry choices. Do not promise integer scaling for small 4:3 drawables: upstream's corrected logical size is 1600×1200.
- Retain CPU sprite blitting into the existing 320×200 surface and persistent SDL textures; `update_screen()` uploads using the real pitch and presents through Metal. Do not add per-frame heap allocation, replace the sprite engine, or write a second Metal renderer. SDL's supported-format conversion can handle upstream RGB24. Remove the hardware/software renderer setting from this app's in-game settings so persisted configuration cannot override the Metal requirement. Skip the upstream `IMG_Load(data/icon.png)` window-icon path on macOS; use the app-owned bundle icon.
- Reuse digital DAT audio and `midi.c`/Nuked OPL3 for original music. No OGG assets, soundfonts, or DOS/CPU emulator are required. Retain original keyboard controls: arrows and Shift, with native command equivalents added in step 6.

### 4. Define a genuinely exact gameplay snapshot and continuation

Add `src/game_snapshot.h`, `src/game_snapshot.c`, and `src/game_state_fields.def`. No existing serializer covers all required state or stages malformed loads safely.

- Capture only at the **complete-frame boundary at the top of `src/seg003.c:play_level_2()`**, before its `timers()/play_frame()` work. The saved continuation is always `GAME_PHASE_FRAME_BOUNDARY`; narrative/title/ending sequences are not saveable. Death continuation and pending level/room transitions reachable at this boundary are saveable. Loading must resume that same phase, not insert an extra initialization/drawing/gameplay tick.
- Extract the current base field list from `src/seg000.c:quick_process()` into the `.def` manifest, maintaining its exact order/conditional tails for trusted legacy replay fixtures. Keep **`int quick_process(process_func_type process_func)`** and `quick_version` as the actually used replay contract; do not append native-save fields to the replay format. Remove the held-Shift/file-seeking special case from this shared visitor. Native documents must not be interpreted as legacy `.SAV` or `.p1r` data.
- `GameSnapshot` contains the full base list: world/door links/guard placement, mobs/trobs, Kid/Guard/Char/Opp, HP, sword/AI/collision history, effects, time/RNG, and gameplay control history. Add these exact native extension fields from `src/data.h`/their defining modules:
  **`exit_room_timer`, `text_time_total`, `text_time_remaining`, `is_show_time`, `next_room`, `seamless`, `roomleave_result`, `resurrect_time`, `start_level`, `dont_reset_time`, `is_restart_level`, `seed_was_init`, `next_sound`, `current_sound`, `jumped_through_mirror`, `is_blind_mode`, `wait_time[timer_1]`**; include sound/music mode/enabled state and the audio continuation below. Persist gameplay render-transition flags needed to perform the next ordinary frame (`different_room`, `need_full_redraw`, `need_redraw_because_flipped`) rather than forcing a new room on load.
- Include the last presented **320×200 RGB24 frame**, packed row-by-row without surface pitch padding, so HUD/death text and the first loaded frame do not borrow stale pixels from another session. Rebuild engine drawing caches separately; do not serialize SDL surfaces/textures, pointers, callback handles, absolute performance-counter origins, or replay recording buffers.
- The fixed normal-play rules profile is **`DOS_SAVE_V1`**, numeric ID **1**: original levelset/custom defaults, copy-protection challenge disabled, lighting disabled, no quickload time penalty, all gameplay enhancement flags disabled **except the existing `fix_quicksave_during_feather`**. Set `fixes_saved = {0}`, that member to one, and call `turn_fixes_and_enhancements_on_off(1)` after configuration initialization. Remove settings controls that change this fixed gameplay profile; retain input/display/audio preferences. That single save-support fix makes feather fall a persistent logical countdown instead of depending on an unresumable sound flag. Store its timer basis as well as duration. Do not enable the rest of SDLPoP's bugfix/enhancement collection.
- Disable user Save/Load commands during replay recording/playback; these documents represent gameplay, not an unrelated recorder session. Preserve legacy replay rules while running trusted fixtures. Normal snapshots neither restore foreign `curr_tick` into a recorder nor import a replay's rules into app play.

**Audio continuation is required, not optional polish:** `check_sound_playing()` gates death and level completion. Stopping/restarting sound on load would change the saved gameplay state.

- Add `AudioSnapshot` capture/prepare/commit hooks in `src/seg009.c` and `src/midi.c`, with one outer SDL audio lock for coherent capture/installation and an explicit panel-suspension flag. Avoid nested audio locks. Maintain the 44100 Hz mixer domain used by `digi_samplerate`; device output conversion is not serialized.
- Speaker: store active resource ID, note index, emitted samples within the note, square-wave phase/sample counter, and playing flag. Digital: store resource ID, byte offset from the converted buffer, remaining length, and playing flag; reconstruct buffers from validated DAT resources before commit, never persist `digi_remaining_pos`.
- MIDI: store active sound ID; per-track current event/next-pause tick; MIDI whole/fractional position, next pause, tempo/ticks-per-beat/semitone/tempo modifier; all channel/voice instrument assignments and last-used voice; and full OPL synthesizer continuation. Reparse the original track data during preparation and verify every stored cursor against it.
- For OPL, serialize every scalar/array field of `opl3_chip`, its 18 channels, 36 slots, and write-buffer entries from `src/opl3.h`. Encode pointer links as bounded symbolic references: channel/slot indices, chip-self, zero modulation, slot `out`/`fbmod`, and tremolo/zero sources. Rebind these to the prepared chip during installation. Never raw-dump `opl3_chip`: its internal pointers would be invalid across processes. Reuse the existing OPL implementation rather than replacing its synthesis algorithm.
- OGG overrides are not part of this fixed original-DAT profile; disable their native asset lookup, so there is no decoder pointer or external track identity to snapshot. Empty/inactive audio states remain valid, including sessions with unavailable audio hardware. Preparing a valid save must not start playback or change live audio.

### 5. Implement bounded documents, atomic writes, and non-mutating restore

Expose this app-owned API in `game_snapshot.h`:

```c
typedef enum {
    SAVE_OK, SAVE_IO_ERROR, SAVE_BAD_FORMAT, SAVE_UNSUPPORTED_VERSION,
    SAVE_INCOMPATIBLE_PROFILE, SAVE_INVALID_STATE, SAVE_PREPARE_FAILED
} SaveResult;

void game_snapshot_capture(GameSnapshot *out);
SaveResult game_snapshot_encode(const GameSnapshot *src, uint8_t *dst,
                                size_t capacity, size_t *written, SaveError *error);
SaveResult game_snapshot_decode(const uint8_t *src, size_t length,
                                GameSnapshot *out, SaveError *error);
SaveResult game_snapshot_prepare(const GameSnapshot *src,
                                 PreparedSnapshot *out, SaveError *error);
void game_snapshot_commit(PreparedSnapshot *prepared);
SaveResult game_save_file(const char *utf8_path, SaveError *error);
SaveResult game_load_file_prepare(const char *utf8_path,
                                 PreparedSnapshot *out, SaveError *error);
void audio_state_capture(AudioSnapshot *out);
SaveResult audio_state_prepare(const AudioSnapshot *src,
                               PreparedAudioState *out, SaveError *error);
void audio_state_commit(PreparedAudioState *prepared);
void audio_set_panel_suspended(bool suspended);
void redraw_loaded_snapshot(void);
```

- Use **`.popsave`** for both named saves and the fixed quick slot. Define format version **1** with an **84-byte header**: eight bytes `POPMACS\0`, LE uint32 format version, LE uint32 profile ID, 32 raw bytes of the archive SHA-256, LE uint32 payload length, and 32 raw bytes of SHA-256 over the payload. Payload has five ordered, required LE uint32-length-prefixed sections: base state, native extension, timer state, audio state, RGB frame. Encode members/arrays explicitly with fixed-width little-endian integers and IEEE-754 bit representations where the existing MIDI state uses floats; no compiler padding/native structs. This version is pinned to the imported core/state layout; future incompatible layouts require a new version and explicit rejection, not speculative migration.
- Reject documents larger than **1 MiB** before allocation, bad/missing headers, wrong version/profile/archive identity, wrong lengths/checksum, missing/extra sections, partial fields, and trailing bytes. Decode into a candidate, not live globals or the caller's previous candidate on failure.
- Validate all consumer-indexed fields before preparation: level-table domain, `level.used_rooms <= 24`, room links 0–24, active mob count <=14 and trob count <=30, active tiles/room/AI/sequence/frame/audio references, timer/effect/control values, and coherent transition state. Use actual enums/consumer bounds from the pinned source. Preserve legal sentinels (`dir_56_none`, actor room zero during falls/death, signed offscreen coordinates, held/ignored control values); do not clamp or normalize malformed values into a different game. Check arithmetic using remaining-length subtraction. Validate MIDI/OPL symbolic references against the prepared resource/topology domains.
- `PreparedSnapshot` owns fully prepared sprite/palette/audio resources. Validation and preparation cannot restart the game, stop live sound, redraw, modify RNG/options, or install globals. Refactor `load_lev_spr()` preparation so its current writes to `current_level/next_level/curr_guard_color` occur only at commit. If preparation fails, retain the previous game and dispose only candidate-owned objects. Commit at a safe point is infallible after successful preparation.
- Separate **restore-only presentation** from normal gameplay drawing. Add `redraw_loaded_snapshot()` and factor the exact side effects in `draw_game_frame()`, `redraw_screen()`, `draw_people()/check_mirror()`, and `show_level()` so rebuilding caches does not run mirror/shadow mechanics, consume queued sounds, decrement/reset death text, change HP, clear `seamless`, force camera to Kid.room, remove offscreen guards, reset `exit_room_timer`, or advance RNG. Preserve the ordinary-frame call order for normal play. Rebind room/door/tile pointers from restored state, rebuild palettes/peels/object/dirty-rect caches, restore stored framebuffer pixels, and present once. Do not call upstream `restore_room_after_quick_load()` for native documents; retain it only for the actually used legacy replay path.
- Write files through a sibling temporary file, checking all writes, flush/fsync, and close; then atomically rename/replace the destination on the same volume. A pre-replacement failure preserves the existing file byte-for-byte and cleans up only this operation's temporary file. NSSavePanel supplies overwrite consent; F6 intentionally replaces only `Quicksave.popsave`.
- Report missing/invalid/incompatible/unwritable files with a nonfatal native alert identifying the operation and underlying reason. No load-time penalty, five-tick black-screen delay, fake fallback, or gameplay-text replacement as success feedback: death-text timers are saved state.

### 6. Add native Save/Load commands at safe points

Add `src/game_commands.h` and `src/game_commands.c`; native panels/menu targets remain in `macos_app.m`. No existing dispatcher handles title-safe loading or native panel suspension.

- Define `GameCommandKind` values **`GAME_SAVE_NAMED`, `GAME_LOAD_NAMED`, `GAME_QUICKSAVE`, `GAME_QUICKLOAD`** and **`bool game_request_command(GameCommandKind kind)`**. Maintain one pending command and an explicit busy/panel state; idle frames allocate/copy nothing. Menu and SDL callbacks enqueue only, suppress shortcut autorepeat, and never install state or open a panel in the middle of `process_events()`/`control_kid()`.
- Define `GameSafePoint` values `GAME_SAFE_TITLE` and `GAME_SAFE_FRAME`, and **`void game_service_commands(GameSafePoint point)`**. Both safe points use the same dispatcher. Define **`void game_complete_panel(uint64_t generation, const char *utf8_path, bool cancelled)`**; completion copies any selected path into operation-owned storage and otherwise only records its result. Call `game_service_commands(GAME_SAFE_FRAME)` at the existing complete-frame consumption boundary and `game_service_commands(GAME_SAFE_TITLE)` before title key-start handling.
- Install a native **Game** menu: **Save Game… ⌘S**, **Load Game… ⌘O**, **Quicksave F6**, **Quickload F9**. Use NSSavePanel/NSOpenPanel with `.popsave` filtering and default Saves directory; permit user-selected named file locations and Unicode paths. Each named Save opens a panel; there is no implicit last-document overwrite policy. Quit uses SDL's quit path/SDL_QUIT, preserving engine teardown.
- Replace all native quick/checkpoint request paths: `seg000.c:process_key` title F9, ordinary F6/F9 and Ctrl+G/L; `seg006.c:control_kid` demo Ctrl+L; `menu.c:pause_menu_clicked` and `key_test_paused_menu`; and the two `seg003.c:check_quick_op` consumption sites. In-game **Save/Load** entries become named-panel actions; F6/F9 remain the quick slot. Close nested pause menus before servicing the pending request at the next complete boundary; remove their current enqueue-time `stop_sounds()` side effect. Shift+F9 does not retain a different current level in this app.
- Remove native-unused checkpoint `save_game/load_game/get_save_path`, fixed `QUICKSAVE.SAV` I/O, old quick request flags/dispatcher, penalty setting, and their prototypes once all callers have migrated. Preserve `quick_process`/legacy replay restore only because real replay callers remain. Before exported-symbol edits, use LSP references after source import; migrate every result, including declarations, without compatibility aliases.
- Save enabled for ordinary gameplay boundary states, including hanging/falling, death, and pending gameplay transitions; disabled on title, narrative/ending sequences, and replay sessions. Load enabled on title and ordinary gameplay, disabled during narrative/ending sequences, replay sessions, or another operation. Match native and in-game menu availability.
- Add a title safe point before the generic any-key-starts-game branch. A failed/cancelled title load must remain on title and must not start level 1. For a valid document, own the prepared snapshot in dispatcher storage that survives `start_game()`'s longjmp; bypass normal title/cutscene/fresh-level presentation, install prepared resources/state, and enter the saved frame-boundary continuation. For cross-level loads during play, synchronize the enclosing `play_level()` local level selection so it cannot overwrite the loaded level on the next transition.
- Suspend simulation at the safe point for a native panel; use a separate `native_panel_active` state, not `do_paused()`, which mutates controls/text. Freeze audio cursors too. While modal UI is open, pump window/quit events and repaint without running timers, gameplay, presentation side effects, or replay advancement. Panel completion only records path/cancel status; commit occurs when the dispatcher regains control. Ignore stale completions using a monotonically increasing operation generation.
- On cancel/failure, preserve gameplay/audio and prior pause disposition. On resume, reset host timer origins while preserving logical timer lengths, release/drain physical keyboard/controller states and queued text/menu clicks, and suppress the triggering held shortcut until release. Keep serialized gameplay control-history fields intact; text typed into a save dialog must not become game input. Consume Command-modified SDL key events so a menu shortcut cannot also start/move the prince.

## Critical files & anchors

All original anchors refer to SDLPoP commit `3c5add5fb7f83d4ceb542823ab66d00146c4271b`; reread before editing.

| File/region | Why it matters |
|---|---|
| `src/seg006.c:control_kid`, approximately 1407–1437 | Contains a separate demo Ctrl+L path, and pause/input occurs halfway through a gameplay frame. https://github.com/NagyD/SDLPoP/blob/3c5add5fb7f83d4ceb542823ab66d00146c4271b/src/seg006.c#L1407-L1437 |
| `src/seg008.c:draw_people`, approximately 1622; `show_level`, approximately 1828 | Drawing invokes mirror mechanics; level display changes text/seamless state. These cannot be reused blindly during restoration. https://github.com/NagyD/SDLPoP/blob/3c5add5fb7f83d4ceb542823ab66d00146c4271b/src/seg008.c#L1622-L1626 |
| `src/seg003.c:redraw_screen`, approximately 240–307 | Ordinary redraw changes room-exit timing even if the explicit quickload timer reset is removed. https://github.com/NagyD/SDLPoP/blob/3c5add5fb7f83d4ceb542823ab66d00146c4271b/src/seg003.c#L240-L307 |
| `src/midi.c`, private state approximately 47–68 and callback approximately 511–586 | Track cursors, voice assignment and OPL state must survive process restart, not just sound IDs. https://github.com/NagyD/SDLPoP/blob/3c5add5fb7f83d4ceb542823ab66d00146c4271b/src/midi.c#L47-L68 |
| `src/opl3.h:opl3_slot/opl3_channel/opl3_chip`, approximately 55–145 | Synthesizer state contains internal pointers; raw struct persistence is invalid. https://github.com/NagyD/SDLPoP/blob/3c5add5fb7f83d4ceb542823ab66d00146c4271b/src/opl3.h#L55-L145 |

## Verification

### Build and native runtime proof

Prerequisites: Apple Silicon Mac with a graphical login session, macOS SDK/Command Line Tools, CMake >=3.24, network access for build-source fetching and the consented first asset download. Planning confirmed clang/SDK/CMake/codesign/ditto are available; full Xcode is not active and Ninja is absent. Use Unix Makefiles; SDL ships compiled Metal shaders, so no `xcrun metal` prerequisite.

Run from the project root after implementation:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
file "build/Prince of Persia.app/Contents/MacOS/PrinceOfPersia"
otool -L "build/Prince of Persia.app/Contents/MacOS/PrinceOfPersia"
codesign --verify --strict --verbose=2 "build/Prince of Persia.app"
open "build/Prince of Persia.app"
```

Expected: Mach-O arm64 executable; only system dylibs/frameworks, no `/opt/homebrew`, source-tree or user-installed runtime linkage; valid local signature. Observe the actual game window and runtime renderer info **`metal`**; do not accept compilation, a Metal preference hint, or screenshots alone as that evidence. Exercise resize/Retina/fullscreen, arrows/Shift, MIDI intro and digital gameplay sound, then quit through the native menu. Also launch the executable from an unrelated CWD and a copied app path containing spaces/non-ASCII; resources work and neither location receives saves/settings.

### First-launch acquisition checks

Use a new throwaway macOS account or an isolated support directory created by the test harness; never delete the user's real support directory to simulate first launch.

1. Decline acknowledgement/download: no Archive.org requests, no game startup, no installed manifest.
2. Accept: observe responsive download/cancel UI, exact digest validation, 21 DATs published, title and level 1 playable with original sound. Inspect bundle contents to ensure no original DAT/PNG/level fallback tree is present.
3. Relaunch offline: game starts from the verified installation with no network access or consent repetition.
4. Inject network failure, cancellation, wrong/truncated bytes, extraction failure, missing DAT, or modified installed DAT: visible failure/repair choice; no fallback launch and no damage to a prior valid installation.

### Behavioral regression tests to add

Upstream has no unit-test runner in its inspected build; add small C/Objective-C CTest targets, not a new general testing framework. Tests must exercise consumer-visible behavior, not source strings, copied UI labels, or wrapper forwarding.

- **`tests/save_rejection_tests.c`**: malformed/truncated/version/profile/checksum/semantic-invalid documents leave live world, RNG, controls, timers and audio unchanged, including after a previously successful load. Re-sign invalid counts/indices to test semantic checks independently of checksum rejection. Reject missing quick slots at title without starting a game.
- **`tests/save_persistence_tests.m`**: real named/quick files survive restart; write/flush/close/replace failures preserve the previous readable save; Unicode/long selected paths work without truncation; Cancel leaves the session intact. Use filesystem fixtures named `test-save.popsave`, `test-directory`, etc., not realistic player names.
- **`tests/save_continuation_tests.c`**: feed fixed controls and a deterministic audio-callback schedule into the actual core. At the same frame boundary as the UI, capture S, advance a known sequence, record authoritative state/frame/audio digests, restore S, and repeat those inputs/callbacks. Compare immediately after commit/presentation and at every later boundary. Include a fresh-process load to catch retained pointers. Assets for these integration cases must come from the consented installation; supply `POP_TEST_ASSETS="$HOME/Library/Application Support/local.princeofpersia.metal/GameData"` when running them. Register the asset-dependent cases only when that explicit test input is provided; run them before calling the port complete.
- Cover active feather fall while changing base/fight speed; room-exit timer 1/2; hanging/falling; active loose floors/gates/chompers; mirror/shadow; guard HP; death music/death-text expiry; timer rollover; level 6 fall; level 12→13 seamless transition; and Jaffar's victory. Repeated loads must not lose effects, advance mirror mechanics twice, alter death timing, or deduct game time. Check actual PCM/audio continuation under the same callback schedule, not just the stored sound ID.
- RNG check: with `seed_was_init=1`, `random_seed=1234`, restored `prandom(255)` outputs must be **228, 213, 217, 54**, matching the source recurrence. This proves resumed RNG behavior, not merely serialized-byte equality.
- Preserve trusted upstream replay fixture behavior under each fixture's recorded rules; do not reinterpret their snapshots using the native codec. Upstream `validate` can exit zero on mismatches: observe its final level/room/alive/time and fixture-specific outcomes, not exit status alone. Hardening unrelated public replay ingestion is not part of this save feature.

Run the asset-dependent tests after first-launch provisioning:

```sh
POP_TEST_ASSETS="$HOME/Library/Application Support/local.princeofpersia.metal/GameData" cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
POP_TEST_ASSETS="$HOME/Library/Application Support/local.princeofpersia.metal/GameData" ctest --test-dir build --output-on-failure
```

At configure time, read the explicitly supplied environment variable `POP_TEST_ASSETS`; register asset-dependent CTest cases with that path as their argument, or omit those cases with a visible configuration status when it is absent. Do not auto-discover or download assets in tests. Run the new codec/continuation targets under sanitizers with these exact commands:

```sh
POP_TEST_ASSETS="$HOME/Library/Application Support/local.princeofpersia.metal/GameData" cmake -S . -B build-sanitize -G "Unix Makefiles" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_OBJC_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-sanitize --parallel
POP_TEST_ASSETS="$HOME/Library/Application Support/local.princeofpersia.metal/GameData" ctest --test-dir build-sanitize --output-on-failure
```

### End-to-end Save/Load acceptance

In the actual app, progress away from the initial position, change health/world state, and record the displayed room/position/health/time. Save **`test-save.popsave`** with ⌘S, continue/change those values, quit, reopen, and use **Load Game…** on the title. The saved frame and gameplay state return; the next moves behave identically and time is not penalized. Repeat with F6/F9, a mid-fall/ledge save, active feather effect, death continuation, and a cross-level load. Save/load panels held open must freeze game time/effects/audio; Cancel and invalid files must leave the current session or title intact. Keys typed in panels cannot move the prince afterward. Verify both native menus and the in-game Save/Load entries use these same semantics.

Planning executed only read-only discovery and in-memory archive/dependency/RNG verification; no app was built or run because no implementation exists yet. Implementation verification must include the actual app interactions above.

## Assumptions & contingencies

- Deliver a locally runnable, unofficial GPL macOS app, minimum macOS 11, arm64 only; neither Mac App Store submission nor public release signing credentials were requested. If distributing binaries later, provide corresponding source and required third-party notices; download consent does not settle original-asset redistribution rights.
- Preserve stock DOS-engine gameplay with the one feather-save support fix and corrected 4:3 presentation. The selected architecture is SDL2 Metal, not AppKit/MTKView with a bespoke renderer. Native named saves restore complete gameplay-boundary state, not narrative cinematics or replay recording sessions.
- If Archive.org is unavailable or serves different bytes, fail visibly and keep the prior valid installation; do not change pins, choose a mirror with unverified content, or package upstream fallback art. Finish the app/error paths and report the missing verified asset prerequisite for provisioning/live gameplay proof.
- If pinned dependencies are already cached, use the hash-verified cache. If fetching/build tools are unavailable, report that concrete prerequisite; do not substitute Homebrew-linked binaries or an x86/Rosetta build. Full Xcode is needed only if later verification requests GPU capture tooling not available through the installed CLT; normal Metal rendering uses SDL's embedded shaders.
