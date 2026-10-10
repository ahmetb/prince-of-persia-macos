/*
SDLPoP, a port/conversion of the DOS game Prince of Persia.
Copyright (C) 2013-2025  Dávid Nagy

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

The authors of this program may be contacted at https://forum.princed.org

DESCRIPTION:
    MIDI playback routines for SDLPoP.

CREDITS:
    The OPL integration code is based in part on Chocolate Doom's MIDI interface, by Simon Howard (GPLv2-licensed).
    Uses the Nuked OPL3 emulator by Alexey Khokholov (GPLv2-licensed).
    MIDI playback code also published as standalone playback program 'popmidi' by Falcury (GPLv3)
*/

#include "common.h"
#include "opl3.h"
#include "midi_snapshot.h"
#include "math.h"

#define MAX_MIDI_CHANNELS 16
#define MAX_OPL_VOICES 18

// OPL3 supports more channels (voices) than OPL2: you could increase the number of used voices (try it!)
// However, released notes can linger longer, so for some tracks it will sound too 'washed out'.
// For example, listen to these sounds with 18 voices enabled:
// * The potion music in Potion1.mff and Potion2.mff
// * The hourglass summoning sound in Jaffar.mff
#define NUM_OPL_VOICES 9

extern short midi_playing; // seg009.c
extern SDL_AudioSpec* digi_audiospec; // seg009.c
extern int digi_unavailable; // seg009.c
extern int playing_midi_id; // seg009.c

static opl3_chip opl_chip;
static void* instruments_data;
static instrument_type* instruments;
static int num_instruments;
static byte voice_note[MAX_OPL_VOICES];
static int voice_instrument[MAX_OPL_VOICES];
static int voice_channel[MAX_OPL_VOICES];
static int channel_instrument[MAX_MIDI_CHANNELS];
static int last_used_voice;
static int num_midi_tracks;
static parsed_midi_type parsed_midi;
static midi_track_type* midi_tracks;
static int64_t midi_current_pos; // in MIDI ticks
static float midi_current_pos_fract_part; // partial ticks after the decimal point
static int ticks_to_next_pause; // in MIDI ticks
static dword us_per_beat;
static dword ticks_per_beat;
static int mixing_freq;
static sbyte midi_semitones_higher;
static float current_midi_tempo_modifier;

// Tempo adjustments for specific songs:
// * PV scene, with 'Story 3 Jaffar enters':
//   Speed must be exactly right, otherwise it will not line up with the flashing animation in the cutscene.
//   The 'Jaffar enters' song has tempo 705882 (maybe this tempo was carefully fine-tuned)?
// * Intro music: playback speed must match the title appearances/transitions.
const float midi_tempo_modifiers[58] = {
		[sound_53_story_3_Jaffar_comes] = -0.03f, // 3% speedup
		[sound_54_intro_music] = 0.03f, // 3% slowdown
};

// The hardcoded instrument is used as a fallback, if instrument data is not available for some reason.
static instrument_type hardcoded_instrument = {
		0x13, 0x09, 0x04, {{0x02, 0x8D, 0xD7, 0x37, 0x00}, {0x03, 0x03, 0xF5, 0x18, 0x00}}, 0x00, {0x00, 0x00},
};

// Read a variable length integer (max 4 bytes).
static dword midi_read_variable_length(byte** buffer_position) {
	dword result = 0;
	byte* pos = *buffer_position;
	int i;
	for (i = 0; i < 4; ++i) {
		result = (result << 7) | (pos[i] & 0x7F);
		if ((pos[i] & 0x80) == 0) break; // The most significant bit being 0 means that this is the last u8.
	}
	*buffer_position += i+1; // Advance the pointer, so we know where the next field starts.
	return result;
}

void free_parsed_midi(parsed_midi_type* parsed_midi) {
	for (int i = 0; i < parsed_midi->num_tracks; ++i) {
		free(parsed_midi->tracks[i].events);
	}
	free(parsed_midi->tracks);
	memset(&parsed_midi, 0, sizeof(parsed_midi));
}

bool parse_midi(midi_raw_chunk_type* midi, parsed_midi_type* parsed_midi) {
	parsed_midi->ticks_per_beat = 24;
	if (memcmp(midi->chunk_type, "MThd", 4) != 0) {
		printf("Warning: Tried to play a midi sound without the 'MThd' chunk header.\n");
		return 0;
	}
	if (SDL_SwapBE32(midi->chunk_length) != 6) {
		printf("Warning: Midi file with an invalid header length (expected 6, is %d)\n",
		       SDL_SwapBE32(midi->chunk_length));
		return 0;
	}
	word midi_format = SDL_SwapBE16(midi->header.format);
	if (midi_format >= 2) {
		printf("Warning: Unsupported midi format %d (only type 0 or 1 files are supported)\n", midi_format);
		return 0;
	}
	word num_tracks = SDL_SwapBE16(midi->header.num_tracks);
	if (num_tracks < 1) {
		printf("Warning: Midi sound does not have any tracks.\n");
		return 0;
	}
	int division = SDL_SwapBE16(midi->header.time_division);
	if (division < 0) {
		division = (-(division / 256)) * (division & 0xFF); // Translate time delta from the alternative SMTPE format.
	}
	parsed_midi->ticks_per_beat = division;

	parsed_midi->tracks = calloc(1, num_tracks * sizeof(midi_track_type));
	parsed_midi->num_tracks = num_tracks;
	midi_raw_chunk_type* next_track_chunk = (midi_raw_chunk_type*) midi->header.tracks; // The first track chunk starts after the header chunk.
	byte last_event_type = 0;
	for (int track_index = 0; track_index < num_tracks; ++track_index) {
		midi_raw_chunk_type* track_chunk = next_track_chunk;
		if (memcmp(track_chunk->chunk_type, "MTrk", 4) != 0) {
			printf("Warning: midi track without 'MTrk' chunk header.\n");
			free(parsed_midi->tracks);
			memset(&parsed_midi, 0, sizeof(parsed_midi));
			return 0;
		}
		next_track_chunk = (midi_raw_chunk_type*) (track_chunk->data + (dword) SDL_SwapBE32(track_chunk->chunk_length));
		midi_track_type* track = &parsed_midi->tracks[track_index];
		byte* buffer_position = track_chunk->data;
		for (;;) {
			++track->num_events;
			void* new_track_events = realloc(track->events, track->num_events * sizeof(midi_event_type));
			if (new_track_events == NULL) {
				printf("parse_midi: realloc failed!");
				quit(1);
			}
			track->events = new_track_events;

			midi_event_type* event = &track->events[track->num_events - 1];
			event->delta_time = midi_read_variable_length(&buffer_position);
			event->event_type = *buffer_position;
			if (event->event_type & 0x80) {
				if (event->event_type < 0xF8) last_event_type = event->event_type;
				++buffer_position;
			} else {
				event->event_type = last_event_type; // Implicit use of the previous event type.
			}
			// Determine the event type and parse the event.
			int num_channel_event_params = 1;
			switch (event->event_type & 0xF0) {
				case 0x80: // note off
				case 0x90: // note on
				case 0xA0: // aftertouch
				case 0xB0: // controller
				case 0xE0: // pitch bend
					num_channel_event_params = 2; //fallthrough
				case 0xC0: // program change
				case 0xD0: { // channel aftertouch
					// Read the channel event.
					event->channel.channel = event->event_type & 0x0F;
					event->event_type &= 0xF0;
					event->channel.param1 = *buffer_position++;
					if (num_channel_event_params == 2) {
						event->channel.param2 = *buffer_position++;
					}
				}
					break;
				default:
					// Not a channel event.
					switch (event->event_type) {
						case 0xF0: // SysEx
						case 0xF7: // SysEx split
							// Read SysEx event
							event->sysex.length = midi_read_variable_length(&buffer_position);
							event->sysex.data = buffer_position;
							buffer_position += event->sysex.length;
							break;
						case 0xFF: // Meta event
							event->meta.type = *buffer_position++;
							event->meta.length = midi_read_variable_length(&buffer_position);
							event->meta.data = buffer_position;
							buffer_position += event->meta.length;
							break;
						default:
							printf("Warning: unknown midi event type 0x%02x (track %d, event %d)\n",
							       event->event_type, track_index, track->num_events - 1);
							free_parsed_midi(parsed_midi);
							return 0;
					}
			}
			if (event->event_type == 0xFF /* meta event */ && event->meta.type == 0x2F /* end of track */) {
				break;
			}
			if (buffer_position >= (byte*) next_track_chunk) {
				printf("Error parsing MIDI events (track %d)\n", track_index);
				free_parsed_midi(parsed_midi);
				return 0;
			}

		}

	}

//	printf("Midi file looks good...\n");
	return 1;
}

#if 0
void print_midi_event(int track_index, int event_index, midi_event_type* event) {
	printf("Track %d Event %3d (dt=%4d): ", track_index, event_index, event->delta_time);
	switch (event->event_type) {
		default:
			printf("unknown type (%x)", event->event_type);
			break;
		case 0x80: // note off
			printf("noteoff: ch %d, par %02x|%02x", event->channel.channel, event->channel.param1, event->channel.param2);
			break;
		case 0x90: // note on
			printf("noteon: ch %d, par %02x|%02x", event->channel.channel, event->channel.param1, event->channel.param2);
			{
				float octaves_from_A4 = ((int)event->channel.param1 - 69) / 12.0f;
				float frequency = powf(2.0f,  octaves_from_A4) * 440.0f;
				float f_number_float = frequency * (float)(1 << 20) / 49716.0f;
				int b = (int)(log2f(f_number_float) - 9) & 7;
				int f = ((int)f_number_float >> b) & 1023;
				printf(", freq = %.1f Hz, F=%d, b=%d", frequency, f, b);
			}
			break;
		case 0xA0: // aftertouch
			printf("aftertouch: ch %d, par %x|%x", event->channel.channel, event->channel.param1, event->channel.param2);
			break;
		case 0xB0: // controller
			printf("controller: ch %d, par %x|%x", event->channel.channel, event->channel.param1, event->channel.param2);
			break;
		case 0xE0: // pitch bend
			printf("pitch bend: ch %d, par %x|%x", event->channel.channel, event->channel.param1, event->channel.param2);
			break;
		case 0xC0: // program change
			printf("program change: ch %d, par %x", event->channel.channel, event->channel.param1);
			break;
		case 0xD0:
			printf("channel aftertouch: ch %d, par %x", event->channel.channel, event->channel.param1);
			break;
		case 0xF0: // SysEx
			printf("sysex event (length=%d): ", event->sysex.length);
			for (int i = 0; i<event->sysex.length; ++i) {
				printf("%02x ", event->sysex.data[i]);
			}
			break;
		case 0xF7: // SysEx split
			printf("sysex split event");
			// Read SysEx event
			break;
		case 0xFF: // Meta event
			printf("meta: %02x (length=%d): ", event->meta.type, event->meta.length);
			switch(event->meta.type) {
				default:
					printf("unknown type");
					break;
				case 0:
					printf("sequence number");
					break;
				case 1:
					printf("text event");
					break;
				case 2:
					printf("copyright notice");
					break;
				case 3:
					printf("sequence/track name: ");
					{
						char* text = malloc(event->meta.length+1);
						memcpy(text, event->meta.data, event->meta.length);
						text[event->meta.length] = '\0';
						printf("%s", text);
						free(text);
					}
					break;
				case 4:
					printf("instrument name");
					break;
				case 0x51:
					printf("set tempo: ");
					{
						byte* data = event->meta.data;
						int new_tempo = (data[0]<<16) | (data[1]<<8) | (data[2]);
						printf("set tempo: %d", new_tempo);
					}
					break;
				case 0x54:
					printf("SMTPE offset: ");
					for (int i = 0; i<event->meta.length; ++i) {
						printf("%02x ", event->meta.data[i]);
					}
					break;
				case 0x58:
					printf("time signature: ");
					for (int i = 0; i<event->meta.length; ++i) {
						printf("%02x ", event->meta.data[i]);
					}
					break;
				case 0x2F:
					printf("end of track");
					break;
			}
			break;

	}
	putchar('\n');
}
#endif

static byte opl_cached_regs[512];

static void opl_reset(int freq) {
	OPL3_Reset(&opl_chip, freq);
	memset(opl_cached_regs, 0, sizeof(opl_cached_regs));
}

static void opl_write_reg(word reg, byte value) {
	OPL3_WriteReg(&opl_chip, reg, value);
	opl_cached_regs[reg] = value;
}

static void opl_write_reg_masked(word reg, byte value, byte mask) {
	byte cached = opl_cached_regs[reg] & ~mask;
	value = cached | (value & mask);
	opl_write_reg(reg, value);
}


// Reference: https://www.fit.vutbr.cz/~arnost/opl/opl3.html#appendixA
//static u8 adlib_op[] = {0, 1, 2, 8, 9, 10, 16, 17, 18};
static byte sbpro_op[] = { 0,  1,  2,   6,  7,  8,  12, 13, 14, 18, 19, 20,  24, 25, 26,  30, 31, 32};

static word reg_pair_offsets[] = {0x000,0x001,0x002,0x003,0x004,0x005,
                                 0x008,0x009,0x00A,0x00B,0x00C,0x00D,
                                 0x010,0x011,0x012,0x013,0x014,0x015,
                                 0x100,0x101,0x102,0x103,0x104,0x105,
                                 0x108,0x109,0x10A,0x10B,0x10C,0x10D,
                                 0x110,0x111,0x112,0x113,0x114,0x115};

static word reg_single_offsets[] = {0,1,2,3,4,5,6,7,8,0x100,0x101,0x102,0x103,0x104,0x105,0x106,0x107,0x108};

static word opl_reg_pair_offset(byte voice, byte op) {
	word reg_offset = reg_pair_offsets[sbpro_op[voice]];
	if (op == 1) reg_offset += 3;
	return reg_offset;
}

static void opl_write_instrument(instrument_type* instrument, byte voice) {
	opl_write_reg(0xC0 + reg_single_offsets[voice], instrument->FB_conn | 0x30 /* OPL3: L+R speaker enable */);
	for (byte operator_index = 0; operator_index < 2; ++operator_index) {
		operator_type* operator = &instrument->operators[operator_index];
		word op_reg = opl_reg_pair_offset(voice, operator_index);
		opl_write_reg(0x20 + op_reg, operator->mul);
		opl_write_reg(0x40 + op_reg, operator->ksl_tl);
		opl_write_reg(0x60 + op_reg, operator->a_d);
		opl_write_reg(0x80 + op_reg, operator->s_r);
		opl_write_reg(0xE0 + op_reg, operator->waveform);
	}
}

static void midi_note_off(midi_event_type* event) {
	byte note = event->channel.param1;
	byte channel = event->channel.channel;
	for (int voice = 0; voice < NUM_OPL_VOICES; ++voice) {
		if (voice_channel[voice] == channel && voice_note[voice] == note) {
			opl_write_reg_masked(0xB0 + reg_single_offsets[voice], 0, 0x20); // release key
			voice_note[voice] = 0; // This voice is now free to be re-used.
			break;
		}
	}
}

static instrument_type* get_instrument(int id) {
	if (id >= 0 && id < num_instruments) {
		return &instruments[id];
	} else {
		return &instruments[0];
	}
}

static void midi_note_on(midi_event_type* event) {
	byte note = event->channel.param1;
	byte velocity = event->channel.param2;
	byte channel = event->channel.channel;
	int instrument_id = channel_instrument[channel];
	instrument_type* instrument = get_instrument(instrument_id);

	if (velocity == 0) {
		midi_note_off(event);
	} else {
		// Find a free OPL voice.
		int voice = -1;
		int test_voice = last_used_voice;
		for (int i = 0; i < NUM_OPL_VOICES; ++i) {
			// Don't use the same voice immediately again: that note is probably still be in the release phase.
			++test_voice;
			test_voice %= NUM_OPL_VOICES;
			if (voice_note[test_voice] == 0) {
				voice = test_voice;
				break;
			}
		}
		last_used_voice = voice;
		if (voice >= 0) {
//			printf("voice %d\n", voice);

			// Set the correct instrument for this voice.
			if (voice_instrument[voice] != instrument_id) {
				opl_write_instrument(instrument, voice);
				voice_instrument[voice] = instrument_id;
			}
			voice_note[voice] = note;
			voice_channel[voice] = channel;

			// Calculate frequency for a MIDI note: note number 69 = A4 = 440 Hz.
			// However, Prince of Persia treats notes as one octave (12 semitones) lower than that, by default.
			// A special MIDI SysEx event is used to change the frequency of all notes.
			float octaves_from_A4 = ((int)event->channel.param1 - 69 - 12 + midi_semitones_higher) / 12.0f;
			float frequency = powf(2.0f,  octaves_from_A4) * 440.0f;
			float f_number_float = frequency * (float)(1 << 20) / 49716.0f;
			int block = (int)(log2f(f_number_float) - 9) & 7;
			int f = ((int)f_number_float >> block) & 1023;
			word reg_offset = reg_single_offsets[voice];
//			opl_write_reg_masked(0xB0 + reg_offset, 0, 0x20); // Turn note off first (should not be necessary)
			opl_write_reg(0xA0 + reg_offset, f & 0xFF);
			opl_write_reg(0xB0 + reg_offset, 0x20 | (block << 2) | (f >> 8));

			// The modulator always uses its own base volume level.
			opl_write_reg_masked(0x40 + opl_reg_pair_offset(voice, 0), instrument->operators[0].ksl_tl, 0x3F);

			// The carrier volume level is calculated as a combination of its base volume and the MIDI note velocity.
			//PRINCE.EXE disassembly: seg009:6C3C
			int instr_volume = instrument->operators[1].ksl_tl & 0x3F;
			int carrier_volume = ((instr_volume + 64) * 225) / (velocity + 161);
			if (carrier_volume < 64) carrier_volume = 64;
			if (carrier_volume > 127) carrier_volume = 127;
			carrier_volume -= 64;
			opl_write_reg_masked(0x40 + opl_reg_pair_offset(voice, 1), carrier_volume, 0x3F);
		} else {
			printf("skipping note, not enough OPL voices\n");
		}

	}

}

static void process_midi_event(midi_event_type* event) {
	switch (event->event_type) {
		case 0x80: // note off
			midi_note_off(event);
			break;
		case 0x90: // note on
			midi_note_on(event);
			break;
		case 0xC0: // program change
			channel_instrument[event->channel.channel] = event->channel.param1;
			break;
		case 0xF0: // SysEx event:
			if (event->sysex.length == 7) {
				byte* data = event->sysex.data;
				if (data[2] == 0x34 && (data[3] == 0 || data[3] == 1) && data[4] == 0) {
					midi_semitones_higher = data[5]; // Make all notes higher by this amount.
				}
			}
			break;
		case 0xFF: // Meta event
			switch(event->meta.type) {
				default: break;
				case 0x51: // set tempo
				{
					byte* data = event->meta.data;
					int new_tempo = (data[0]<<16) | (data[1]<<8) | (data[2]);
					new_tempo *= (1.0f + current_midi_tempo_modifier); // tempo adjustment for specific songs
					us_per_beat = new_tempo;
				}
					break;
				case 0x54: // SMTPE offset
					break;
				case 0x58: // time signature
					break;
				case 0x2F: // end of track
					break;
			}
			break;
		default: break;

	}

}

#define ONE_SECOND_IN_US 1000000LL

void midi_callback(void *userdata, Uint8 *stream, int len) {
	if (!midi_playing || len <= 0) return;
	int frames_needed = len / 4;
	while (frames_needed > 0) {
		if (ticks_to_next_pause > 0) {
			// Fill the audio buffer (we have already processed the MIDI events up till this point)
			int64_t us_to_next_pause = ticks_to_next_pause * us_per_beat / ticks_per_beat;
			int64_t us_needed = frames_needed * ONE_SECOND_IN_US / mixing_freq;
			int64_t advance_us = MIN(us_to_next_pause, us_needed);
			int available_frames = (int)(((advance_us * mixing_freq) + ONE_SECOND_IN_US - 1) / ONE_SECOND_IN_US); // round up.
			int advance_frames = MIN(available_frames, frames_needed);
			advance_us = advance_frames * ONE_SECOND_IN_US / mixing_freq; // recalculate, in case the rounding up increased this.
			short* temp_buffer = malloc(advance_frames * 4);
			OPL3_GenerateStream(&opl_chip, temp_buffer, advance_frames);
			if (is_sound_on && enable_music) {
				for (int sample = 0; sample < advance_frames * 2; ++sample) {
					((short*)stream)[sample] += temp_buffer[sample];
				}
			}
			free(temp_buffer);

			frames_needed -= advance_frames;
			stream += advance_frames * 4;
			// Advance the current MIDI tick position.
			// Keep track of the partial ticks that have elapsed so that we do not fall behind.
			float ticks_elapsed_float = (float)advance_us * ticks_per_beat / us_per_beat;
			int64_t ticks_elapsed = (int64_t) ticks_elapsed_float;
			midi_current_pos_fract_part += (ticks_elapsed_float - ticks_elapsed);
			if (midi_current_pos_fract_part > 1.0f) {
				midi_current_pos_fract_part -= 1.0f;
				ticks_elapsed += 1;
			}
			midi_current_pos += ticks_elapsed;
			ticks_to_next_pause -= ticks_elapsed;
		} else {
			// Need to process MIDI events on one or more tracks.
			int num_finished_tracks = 0;
			for (int track_index = 0; track_index < num_midi_tracks; ++track_index) {
				midi_track_type* track = &midi_tracks[track_index];

				while (midi_current_pos >= track->next_pause_tick) {
					int events_left = track->num_events - track->event_index;
					if (events_left > 0) {
						midi_event_type* event = &track->events[track->event_index];
						track->event_index++;
//						print_midi_event(track_index, track->event_index-1, event);
						process_midi_event(event);

						// Need to look ahead: must delay processing of the next event, if there is a pause.
						if (events_left > 1) {
							midi_event_type* next_event = &track->events[track->event_index];
							if (next_event->delta_time != 0) {
								track->next_pause_tick += next_event->delta_time;
							}
						}
					} else {
						// reached the last event in this track.
						++num_finished_tracks;
						break;
					}
				}
			}
			if (num_finished_tracks >= num_midi_tracks) {
				// All tracks have finished. Fill the remaining samples with silence and stop playback.
				SDL_memset(stream, 0, frames_needed * 4);
//				printf("midi_callback(): sound ended\n");
				SDL_LockAudio();
				midi_playing = 0;
				playing_midi_id = -1;
				free_parsed_midi(&parsed_midi);
				SDL_UnlockAudio();
				return;
			} else {
				// Need to delay (let the OPL chip do its work) until one of the tracks needs to process a MIDI event again.
				int64_t first_next_pause_tick = INT64_MAX;
				for (int i = 0; i < num_midi_tracks; ++i) {
					midi_track_type* track = &midi_tracks[i];
					if (track->event_index >= track->num_events || midi_current_pos >= track->next_pause_tick) continue;
					first_next_pause_tick = MIN(first_next_pause_tick, track->next_pause_tick);
				}
				if (first_next_pause_tick == INT64_MAX) {
					printf("MIDI: Couldn't figure out how long to delay (this is a bug)\n");
					quit(1);
				}
				ticks_to_next_pause = (int)(first_next_pause_tick - midi_current_pos);
				if (ticks_to_next_pause < 0) {
					printf("Tried to delay a negative amount of time (this is a bug)\n"); // This should never happen?
					quit(1);
				}
//				printf("                             delaying %d ticks = %.3f s\n",
//				       ticks_to_next_pause, (((us_per_beat / ticks_per_beat) * (dword)ticks_to_next_pause) / 1e6f));
			}
		}
	}
}


void stop_midi() {
	if (!midi_playing) return;
//	SDL_PauseAudio(1);
	SDL_LockAudio();
	midi_playing = 0;
	playing_midi_id = -1;
	free_parsed_midi(&parsed_midi);
	SDL_UnlockAudio();
}

void free_midi_resources(void) {
	free(instruments_data);
}

void init_midi() {
	static bool initialized = false;
	if (initialized) return;
	initialized = true;

	instruments = &hardcoded_instrument; // unused if instruments can be loaded normally.
	int size;
	dat_type* dathandle = open_dat("PRINCE.DAT", 0);
	instruments_data = load_from_opendats_alloc(1, "bin", NULL, &size);
	if (!instruments_data) {
		printf("Missing MIDI instruments data (resource 1)\n");
	} else {
		num_instruments = *(byte*)instruments_data;
		if (size == 1 + num_instruments*(int)sizeof(instrument_type)) {
			instruments = (instrument_type*) ((byte*)instruments_data+1);
		} else {
			printf("MIDI instruments data (resource 1) is not the expected size\n");
			num_instruments = 1;
		}
	}
	if (dathandle != NULL) close_dat(dathandle);
}

void play_midi_sound(sound_buffer_type* buffer) {
	stop_midi();
	if (buffer == NULL) return;
	init_digi();
	if (digi_unavailable) return;
	init_midi();

	if (!parse_midi((midi_raw_chunk_type*) &buffer->midi, &parsed_midi)) {
		printf("Error reading MIDI music\n");
		return;
	}

	// Initialize the OPL chip.
	opl_reset(digi_audiospec->freq);
	opl_write_reg(0x105, 0x01); // OPL3 enable (note: the PoP1 Adlib sounds don't actually use OPL3 extensions)
	for (int voice = 0; voice < NUM_OPL_VOICES; ++voice) {
		opl_write_instrument(&instruments[0], voice);
		voice_instrument[voice] = 0;
		voice_note[voice] = 0;
	}
	for (int channel = 0; channel < MAX_MIDI_CHANNELS; channel++) {
		channel_instrument[channel] = channel;
	}

	midi_current_pos = 0;
	midi_current_pos_fract_part = 0;
	ticks_to_next_pause = 0;
	midi_tracks = parsed_midi.tracks;
	num_midi_tracks = parsed_midi.num_tracks;
	midi_semitones_higher = 0;
	us_per_beat = 500000; // default tempo (500000 us/beat == 120 bpm)
	current_midi_tempo_modifier = midi_tempo_modifiers[current_sound];
	ticks_per_beat = parsed_midi.ticks_per_beat;
	mixing_freq = digi_audiospec->freq;
	midi_playing = 1;
	SDL_PauseAudio(0);
}

// ---------------------------------------------------------------------------
// Exact-state snapshot hooks (native .popsave audio continuation)
// ---------------------------------------------------------------------------

// Prepared continuation owned by this module; installed at commit.
static parsed_midi_type prepared_parsed_midi;
static bool prepared_midi_valid = false;
bool midi_prepared_is_valid(void) { return prepared_midi_valid; }

extern int playing_midi_id;

static int slot_index_of(const opl3_chip *chip, const opl3_slot *slot) {
	if (slot == NULL) return -1;
	ptrdiff_t d = slot - chip->slot;
	if (d < 0 || d >= 36) return -1;
	return (int)d;
}
static int channel_index_of(const opl3_chip *chip, const opl3_channel *ch) {
	if (ch == NULL) return -1;
	ptrdiff_t d = ch - chip->channel;
	if (d < 0 || d >= 18) return -1;
	return (int)d;
}

static void opl_capture(const opl3_chip *chip, SerialOplChip *out) {
	memset(out, 0, sizeof(*out));
	out->timer = chip->timer;
	out->newm = chip->newm; out->nts = chip->nts;
	out->rhy = chip->rhy; out->vibpos = chip->vibpos; out->vibshift = chip->vibshift;
	out->tremolo = chip->tremolo; out->tremolopos = chip->tremolopos; out->tremoloshift = chip->tremoloshift;
	out->noise = chip->noise;
	out->zeromod = chip->zeromod;
	out->mixbuff[0] = chip->mixbuff[0]; out->mixbuff[1] = chip->mixbuff[1];
	out->rateratio = chip->rateratio; out->samplecnt = chip->samplecnt;
	out->oldsamples[0] = chip->oldsamples[0]; out->oldsamples[1] = chip->oldsamples[1];
	out->samples[0] = chip->samples[0]; out->samples[1] = chip->samples[1];
	out->writebuf_samplecnt = chip->writebuf_samplecnt;
	out->writebuf_cur = chip->writebuf_cur; out->writebuf_last = chip->writebuf_last;
	out->writebuf_lasttime = chip->writebuf_lasttime;
	for (int i = 0; i < OPL_WRITEBUF_SIZE; ++i) {
		out->writebuf[i].time = chip->writebuf[i].time;
		out->writebuf[i].reg = chip->writebuf[i].reg;
		out->writebuf[i].data = chip->writebuf[i].data;
	}
	for (int i = 0; i < 36; ++i) {
		const opl3_slot *s = &chip->slot[i];
		SerialOplSlot *o = &out->slot[i];
		o->channel_index = (uint8_t)channel_index_of(chip, s->channel);
		if (s->mod == (Bit16s*)&chip->zeromod) {
			o->mod_kind = OPL_REF_ZEROMOD; o->mod_index = 0;
		} else {
			int found = -1, kind = -1;
			for (int k = 0; k < 36; ++k) {
				if (s->mod == &chip->slot[k].fbmod) { found = k; kind = OPL_REF_SLOT_FBMOD; break; }
				if (s->mod == &chip->slot[k].out) { found = k; kind = OPL_REF_SLOT_OUT; break; }
			}
			o->mod_kind = (kind < 0) ? OPL_REF_ZEROMOD : (uint8_t)kind;
			o->mod_index = (found < 0) ? 0 : (uint8_t)found;
		}
		o->trem_kind = (s->trem == &chip->tremolo) ? 0 : 1;
		o->out = s->out;
		o->fbmod = s->fbmod;
		o->prout = s->prout;
		o->eg_rout = s->eg_rout;
		o->eg_out = s->eg_out;
		o->eg_inc = s->eg_inc;
		o->eg_gen = s->eg_gen;
		o->eg_rate = s->eg_rate;
		o->eg_ksl = s->eg_ksl;
		o->reg_vib = s->reg_vib;
		o->reg_type = s->reg_type;
		o->reg_ksr = s->reg_ksr;
		o->reg_mult = s->reg_mult;
		o->reg_ksl = s->reg_ksl;
		o->reg_tl = s->reg_tl;
		o->reg_ar = s->reg_ar;
		o->reg_dr = s->reg_dr;
		o->reg_sl = s->reg_sl;
		o->reg_rr = s->reg_rr;
		o->reg_wf = s->reg_wf;
		o->key = s->key;
		o->pg_phase = s->pg_phase;
		o->timer = s->timer;
	}
	for (int i = 0; i < 18; ++i) {
		const opl3_channel *ch = &chip->channel[i];
		SerialOplChannel *o = &out->channel[i];
		o->slot0 = (uint16_t)slot_index_of(chip, ch->slots[0]);
		o->slot1 = (uint16_t)slot_index_of(chip, ch->slots[1]);
		o->pair = (int16_t)channel_index_of(chip, ch->pair);
		for (int k = 0; k < 4; ++k) {
			void *p = (void*)ch->out[k];
			if (p == NULL || p == (void*)&chip->zeromod) {
				o->out_kind[k] = OPL_REF_ZEROMOD; o->out_index[k] = 0;
			} else {
				int found = -1;
				for (int j = 0; j < 36; ++j) {
					if ((void*)(&chip->slot[j].out) == p) { found = j; break; }
				}
				if (found >= 0) { o->out_kind[k] = OPL_REF_SLOT_OUT; o->out_index[k] = (uint8_t)found; }
				else { o->out_kind[k] = OPL_REF_ZEROMOD; o->out_index[k] = 0; }
			}
		}
		o->chtype = ch->chtype;
		o->f_num = ch->f_num;
		o->block = ch->block; o->fb = ch->fb; o->con = ch->con;
		o->alg = ch->alg; o->ksv = ch->ksv;
		o->cha = ch->cha; o->chb = ch->chb;
	}
}

static void opl_install(const SerialOplChip *src, opl3_chip *chip, int samplerate) {
	OPL3_Reset(chip, (Bit32u)samplerate);
	chip->timer = src->timer;
	chip->newm = src->newm; chip->nts = src->nts;
	chip->rhy = src->rhy; chip->vibpos = src->vibpos; chip->vibshift = src->vibshift;
	chip->tremolo = src->tremolo; chip->tremolopos = src->tremolopos; chip->tremoloshift = src->tremoloshift;
	chip->noise = src->noise;
	chip->zeromod = src->zeromod;
	chip->mixbuff[0] = src->mixbuff[0]; chip->mixbuff[1] = src->mixbuff[1];
	chip->rateratio = src->rateratio; chip->samplecnt = src->samplecnt;
	chip->oldsamples[0] = src->oldsamples[0]; chip->oldsamples[1] = src->oldsamples[1];
	chip->samples[0] = src->samples[0]; chip->samples[1] = src->samples[1];
	chip->writebuf_samplecnt = src->writebuf_samplecnt;
	chip->writebuf_cur = src->writebuf_cur; chip->writebuf_last = src->writebuf_last;
	chip->writebuf_lasttime = src->writebuf_lasttime;
	for (int i = 0; i < OPL_WRITEBUF_SIZE; ++i) {
		chip->writebuf[i].time = src->writebuf[i].time;
		chip->writebuf[i].reg = src->writebuf[i].reg;
		chip->writebuf[i].data = src->writebuf[i].data;
	}
	for (int i = 0; i < 36; ++i) {
		opl3_slot *s = &chip->slot[i];
		const SerialOplSlot *o = &src->slot[i];
		s->channel = &chip->channel[o->channel_index];
		s->chip = chip;
		switch (o->mod_kind) {
			case OPL_REF_SLOT_FBMOD: s->mod = &chip->slot[o->mod_index].fbmod; break;
			case OPL_REF_SLOT_OUT: s->mod = &chip->slot[o->mod_index].out; break;
			default: s->mod = &chip->zeromod; break;
		}
		s->trem = (o->trem_kind == 0) ? &chip->tremolo : (Bit8u*)&chip->zeromod;
		s->out = o->out; s->fbmod = o->fbmod; s->prout = o->prout;
		s->eg_rout = o->eg_rout; s->eg_out = o->eg_out;
		s->eg_inc = o->eg_inc; s->eg_gen = o->eg_gen; s->eg_rate = o->eg_rate; s->eg_ksl = o->eg_ksl;
		s->reg_vib = o->reg_vib; s->reg_type = o->reg_type; s->reg_ksr = o->reg_ksr; s->reg_mult = o->reg_mult;
		s->reg_ksl = o->reg_ksl; s->reg_tl = o->reg_tl; s->reg_ar = o->reg_ar; s->reg_dr = o->reg_dr;
		s->reg_sl = o->reg_sl; s->reg_rr = o->reg_rr; s->reg_wf = o->reg_wf; s->key = o->key;
		s->pg_phase = o->pg_phase; s->timer = o->timer;
	}
	for (int i = 0; i < 18; ++i) {
		opl3_channel *ch = &chip->channel[i];
		const SerialOplChannel *o = &src->channel[i];
		ch->slots[0] = &chip->slot[o->slot0];
		ch->slots[1] = &chip->slot[o->slot1];
		ch->chip = chip;
		ch->pair = (o->pair >= 0) ? &chip->channel[o->pair] : NULL;
		for (int k = 0; k < 4; ++k) {
			ch->out[k] = (o->out_kind[k] == OPL_REF_SLOT_OUT) ? &chip->slot[o->out_index[k]].out : &chip->zeromod;
		}
		ch->chtype = o->chtype;
		ch->f_num = o->f_num;
		ch->block = o->block; ch->fb = o->fb; ch->con = o->con;
		ch->alg = o->alg; ch->ksv = o->ksv;
		ch->cha = o->cha; ch->chb = o->chb;
	}
}

void midi_snapshot_capture(MidiAudioState *out) {
	memset(out, 0, sizeof(*out));
	out->resource_id = (int16_t)playing_midi_id;
	out->playing = (uint8_t)(midi_playing != 0);
	out->num_tracks = (int16_t)num_midi_tracks;
	for (int i = 0; i < num_midi_tracks && i < POP_MIDI_MAX_TRACKS; ++i) {
		out->tracks[i].event_index = midi_tracks[i].event_index;
		out->tracks[i].next_pause_tick = midi_tracks[i].next_pause_tick;
	}
	out->current_pos = midi_current_pos;
	out->current_pos_fract = midi_current_pos_fract_part;
	out->ticks_to_next_pause = ticks_to_next_pause;
	out->us_per_beat = us_per_beat;
	out->ticks_per_beat = ticks_per_beat;
	out->mixing_freq = mixing_freq;
	out->semitones_higher = midi_semitones_higher;
	out->tempo_modifier = current_midi_tempo_modifier;
	for (int i = 0; i < MAX_MIDI_CHANNELS; ++i) out->channel_instrument[i] = (uint8_t)channel_instrument[i];
	for (int i = 0; i < MAX_OPL_VOICES; ++i) {
		out->voice_note[i] = voice_note[i];
		out->voice_instrument[i] = (uint8_t)voice_instrument[i];
		out->voice_channel[i] = (uint8_t)voice_channel[i];
	}
	out->last_used_voice = (int16_t)last_used_voice;
	opl_capture(&opl_chip, &out->opl);
}

SaveResult midi_snapshot_prepare(const MidiAudioState *src, SaveError *error) {
	prepared_midi_valid = false;
	if (prepared_parsed_midi.tracks != NULL) {
		free_parsed_midi(&prepared_parsed_midi);
	}
	if (!src->playing) {
		// Empty/inactive MIDI state is always valid.
		return SAVE_OK;
	}
	if (src->resource_id < 0 || src->resource_id >= 58 || sound_pointers[src->resource_id] == NULL) {
		save_error_set(error, 3, "midi resource missing");
		return SAVE_PREPARE_FAILED;
	}
	sound_buffer_type *buf = sound_pointers[src->resource_id];
	if (buf->type != sound_midi) {
		save_error_set(error, 3, "midi resource is not MIDI");
		return SAVE_PREPARE_FAILED;
	}
	if (!parse_midi((midi_raw_chunk_type*)&buf->midi, &prepared_parsed_midi)) {
		save_error_set(error, 3, "midi reparse failed");
		return SAVE_PREPARE_FAILED;
	}
	if (prepared_parsed_midi.num_tracks != src->num_tracks) {
		free_parsed_midi(&prepared_parsed_midi);
		save_error_set(error, 3, "midi track count mismatch");
		return SAVE_PREPARE_FAILED;
	}
	if (prepared_parsed_midi.ticks_per_beat != src->ticks_per_beat) {
		free_parsed_midi(&prepared_parsed_midi);
		save_error_set(error, 3, "midi ticks-per-beat mismatch");
		return SAVE_PREPARE_FAILED;
	}
	// Verify every stored cursor against the reparsed track data.
	for (int i = 0; i < src->num_tracks; ++i) {
		midi_track_type *track = &prepared_parsed_midi.tracks[i];
		int ei = src->tracks[i].event_index;
		if (ei < 0 || ei > track->num_events) {
			free_parsed_midi(&prepared_parsed_midi);
			save_error_set(error, 3, "midi event cursor out of range");
			return SAVE_PREPARE_FAILED;
		}
		if (src->tracks[i].next_pause_tick < 0) {
			free_parsed_midi(&prepared_parsed_midi);
			save_error_set(error, 3, "midi next-pause tick invalid");
			return SAVE_PREPARE_FAILED;
		}
	}
	prepared_midi_valid = true;
	return SAVE_OK;
}

void midi_snapshot_dispose(void) {
	if (prepared_midi_valid && prepared_parsed_midi.tracks != NULL) {
		free_parsed_midi(&prepared_parsed_midi);
	}
	prepared_midi_valid = false;
}

void midi_snapshot_commit(const MidiAudioState *prepared) {
	if (!prepared->playing) {
		midi_playing = 0;
		free_parsed_midi(&parsed_midi);
		playing_midi_id = -1;
		return;
	}
	// Install the prepared, cursor-verified continuation.
	free_parsed_midi(&parsed_midi);
	parsed_midi = prepared_parsed_midi;
	memset(&prepared_parsed_midi, 0, sizeof(prepared_parsed_midi));
	prepared_midi_valid = false;

	midi_tracks = parsed_midi.tracks;
	num_midi_tracks = prepared->num_tracks;
	for (int i = 0; i < num_midi_tracks; ++i) {
		midi_tracks[i].event_index = prepared->tracks[i].event_index;
		midi_tracks[i].next_pause_tick = prepared->tracks[i].next_pause_tick;
	}
	midi_current_pos = prepared->current_pos;
	midi_current_pos_fract_part = prepared->current_pos_fract;
	ticks_to_next_pause = prepared->ticks_to_next_pause;
	us_per_beat = prepared->us_per_beat;
	ticks_per_beat = prepared->ticks_per_beat;
	mixing_freq = prepared->mixing_freq;
	midi_semitones_higher = prepared->semitones_higher;
	current_midi_tempo_modifier = prepared->tempo_modifier;
	for (int i = 0; i < MAX_MIDI_CHANNELS; ++i) channel_instrument[i] = prepared->channel_instrument[i];
	for (int i = 0; i < MAX_OPL_VOICES; ++i) {
		voice_note[i] = prepared->voice_note[i];
		voice_instrument[i] = prepared->voice_instrument[i];
		voice_channel[i] = prepared->voice_channel[i];
	}
	last_used_voice = prepared->last_used_voice;
	opl_install(&prepared->opl, &opl_chip, mixing_freq);
	midi_playing = 1;
	playing_midi_id = prepared->resource_id;
}
