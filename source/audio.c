/* audio.c -- SDL2-based audio engine for libfox.so JNI AudioHelper mapping
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <SDL2/SDL.h>

#include "audio.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

// Prototype for stb_vorbis function defined in stb_vorbis.c
int stb_vorbis_decode_filename(const char *filename, int *channels, int *sample_rate, short **output);

#define TARGET_RATE 44100
#define TARGET_CHANNELS 2

static SDL_AudioDeviceID audio_device = 0;

// Path resolution helper
static void get_full_path(const char *rel, char *dest, size_t maxlen) {
  if (rel[0] == '/' || (rel[0] && rel[1] == ':')) {
    snprintf(dest, maxlen, "%s", rel);
  } else {
    snprintf(dest, maxlen, "sdmc:/switch/s4ep2/%s", rel);
  }
}

#include "audio_mapping.h"

static int find_mapping(const char *logical, char *out_physical, size_t maxlen) {
  char lower[256];
  int i = 0;
  for (; logical[i] && i < 255; i++) {
    char c = logical[i];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    lower[i] = c;
  }
  lower[i] = '\0';

  int left = 0;
  int right = sizeof(audio_mappings) / sizeof(audio_mappings[0]) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    int cmp = strcmp(lower, audio_mappings[mid].logical);
    if (cmp == 0) {
      strncpy(out_physical, audio_mappings[mid].physical, maxlen - 1);
      out_physical[maxlen - 1] = '\0';
      return 1;
    }
    if (cmp < 0) {
      right = mid - 1;
    } else {
      left = mid + 1;
    }
  }
  return 0;
}

static int file_exists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f) {
    fclose(f);
    return 1;
  }
  return 0;
}

static int resolve_audio_path(const char *logical, char *out_path, size_t maxlen) {
  char clean_logical[256];
  strncpy(clean_logical, logical, sizeof(clean_logical) - 1);
  clean_logical[sizeof(clean_logical) - 1] = '\0';
  
  // Strip extension if present
  size_t len = strlen(clean_logical);
  if (len > 4) {
    const char *ext = clean_logical + len - 4;
    if (strcasecmp(ext, ".ogg") == 0 || strcasecmp(ext, ".mp3") == 0) {
      clean_logical[len - 4] = '\0';
    }
  }

  char physical[256];
  if (!find_mapping(clean_logical, physical, sizeof(physical)) || physical[0] == '\0') {
    strncpy(physical, clean_logical, sizeof(physical) - 1);
    physical[sizeof(physical) - 1] = '\0';
  }

  char base_paths[3][512];
  snprintf(base_paths[0], sizeof(base_paths[0]), "assets/%s", physical);
  
  snprintf(base_paths[1], sizeof(base_paths[1]), "assets/%s", physical);
  for (int i = 7; base_paths[1][i]; i++) {
    if (base_paths[1][i] >= 'a' && base_paths[1][i] <= 'z') {
      base_paths[1][i] = base_paths[1][i] - 'a' + 'A';
    }
  }
  
  snprintf(base_paths[2], sizeof(base_paths[2]), "assets/%s", physical);
  for (int i = 7; base_paths[2][i]; i++) {
    if (base_paths[2][i] >= 'A' && base_paths[2][i] <= 'Z') {
      base_paths[2][i] = base_paths[2][i] - 'A' + 'a';
    }
  }

  const char *exts[] = { ".MP3", ".mp3", ".OGG", ".ogg" };
  char test_path[1024];
  char full_path[1024];

  for (int b = 0; b < 3; b++) {
    for (int e = 0; e < 4; e++) {
      snprintf(test_path, sizeof(test_path), "%s%s", base_paths[b], exts[e]);
      get_full_path(test_path, full_path, sizeof(full_path));
      if (file_exists(full_path)) {
        strncpy(out_path, test_path, maxlen - 1);
        out_path[maxlen - 1] = '\0';
        return 1;
      }
    }
  }

  for (int e = 0; e < 4; e++) {
    snprintf(test_path, sizeof(test_path), "assets/%s%s", logical, exts[e]);
    get_full_path(test_path, full_path, sizeof(full_path));
    if (file_exists(full_path)) {
      strncpy(out_path, test_path, maxlen - 1);
      out_path[maxlen - 1] = '\0';
      return 1;
    }
  }

  return 0;
}

// Resampler helper
static int16_t *resample_and_mix_to_stereo(int16_t *src, int src_rate, int src_channels, int src_len_frames, int *out_len_frames) {
  double rate_ratio = (double)TARGET_RATE / src_rate;
  int target_len_frames = (int)(src_len_frames * rate_ratio);
  if (target_len_frames <= 0) target_len_frames = 1;
  
  int16_t *dst = malloc(target_len_frames * TARGET_CHANNELS * sizeof(int16_t));
  if (!dst) return NULL;
  
  for (int i = 0; i < target_len_frames; i++) {
    double src_idx = i / rate_ratio;
    int idx1 = (int)src_idx;
    int idx2 = idx1 + 1;
    double t = src_idx - idx1;
    if (idx1 >= src_len_frames) idx1 = src_len_frames - 1;
    if (idx2 >= src_len_frames) idx2 = src_len_frames - 1;
    
    if (src_channels == 1) {
      int16_t s1 = src[idx1];
      int16_t s2 = src[idx2];
      int16_t sample = (int16_t)(s1 + t * (s2 - s1));
      dst[i * 2] = sample;
      dst[i * 2 + 1] = sample;
    } else {
      int16_t l1 = src[idx1 * 2];
      int16_t l2 = src[idx2 * 2];
      int16_t r1 = src[idx1 * 2 + 1];
      int16_t r2 = src[idx2 * 2 + 1];
      dst[i * 2] = (int16_t)(l1 + t * (l2 - l1));
      dst[i * 2 + 1] = (int16_t)(r1 + t * (r2 - r1));
    }
  }
  
  *out_len_frames = target_len_frames;
  return dst;
}

// Generic file decoder
static int16_t *load_audio_file(const char *path, int *out_len_frames) {
  int channels = 0;
  int sample_rate = 0;
  int16_t *raw_data = NULL;
  int raw_len_frames = 0;
  
  if (strstr(path, ".ogg") || strstr(path, ".OGG")) {
    short *decoded = NULL;
    int samples = stb_vorbis_decode_filename(path, &channels, &sample_rate, &decoded);
    if (samples > 0 && decoded) {
      raw_data = (int16_t *)decoded;
      raw_len_frames = samples;
    }
  } else {
    drmp3_uint64 total_frames = 0;
    drmp3_config config;
    int16_t *decoded = drmp3_open_file_and_read_pcm_frames_s16(path, &config, &total_frames, NULL);
    if (decoded && total_frames > 0) {
      channels = config.channels;
      sample_rate = config.sampleRate;
      raw_data = decoded;
      raw_len_frames = (int)total_frames;
    }
  }
  
  if (!raw_data) return NULL;
  
  int target_len_frames = 0;
  int16_t *resampled = resample_and_mix_to_stereo(raw_data, sample_rate, channels, raw_len_frames, &target_len_frames);
  free(raw_data);
  
  *out_len_frames = target_len_frames;
  return resampled;
}

// Sound effects state
#define MAX_SOUND_CHANNELS 32
typedef struct {
  int active;
  int loop;
  float volume;
  int play_pos;
  int data_len;
  int16_t *data;
  int stream_id;
  int slot_id;
  char name[64];
} SoundChannel;

static SoundChannel sound_channels[MAX_SOUND_CHANNELS];
static int next_stream_id = 1000;

// Sound cache state
#define MAX_CACHE_ENTRIES 512
typedef struct {
  char path[256];
  int16_t *data;
  int data_len;
} CacheEntry;

static CacheEntry sound_cache[MAX_CACHE_ENTRIES];
static int cache_count = 0;

// Music state
#define MAX_MUSIC_SLOTS 8
typedef struct {
  char path[512];
  int active;
  int loop;
  float volume;
  int play_pos;
  int data_len;
  int16_t *data;
  int state; // 0=stopped, 1=playing, 2=paused
  int can_start; // Android MediaPlayer state machine replica
} MusicSlot;

static MusicSlot music_slots[MAX_MUSIC_SLOTS];
static int g_game_paused = 0;

static int is_jingle(const char *path) {
  if (!path) return 0;
  char lower[512];
  int i = 0;
  for (; path[i] && i < 511; i++) {
    char c = path[i];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    lower[i] = c;
  }
  lower[i] = '\0';
  
  if (strstr(lower, "jin") != NULL) return 1;
  if (strstr(lower, "1up") != NULL) return 1;
  if (strstr(lower, "clear") != NULL) return 1;
  if (strstr(lower, "emerald") != NULL) return 1;
  if (strstr(lower, "timer") != NULL) return 1; // drowning countdown jingle
  return 0;
}

// SDL2 Mixer callback
static void audio_callback(void *userdata, uint8_t *stream, int len) {
  (void)userdata;
  int frames_to_write = len / 4;
  int16_t *out = (int16_t *)stream;
  memset(out, 0, len);
  
  for (int f = 0; f < frames_to_write; f++) {
    int32_t mix_l = 0;
    int32_t mix_r = 0;
    
    // Mix sound effects
    for (int i = 0; i < MAX_SOUND_CHANNELS; i++) {
      if (!sound_channels[i].active) continue;
      
      int pos = sound_channels[i].play_pos;
      if (pos >= sound_channels[i].data_len) {
        if (sound_channels[i].loop) {
          sound_channels[i].play_pos = 0;
          pos = 0;
        } else {
          sound_channels[i].active = 0;
          continue;
        }
      }
      
      int16_t sample_l = 0;
      int16_t sample_r = 0;
      if (sound_channels[i].data) {
        sample_l = sound_channels[i].data[pos * 2];
        sample_r = sound_channels[i].data[pos * 2 + 1];
      }
      
      mix_l += (int32_t)(sample_l * sound_channels[i].volume);
      mix_r += (int32_t)(sample_r * sound_channels[i].volume);
      
      sound_channels[i].play_pos++;
    }
    
    // Check if any jingle is currently playing on other slots (slots 1 to 7)
    int jingle_playing = 0;
    for (int i = 1; i < MAX_MUSIC_SLOTS; i++) {
      if (music_slots[i].active && music_slots[i].state == 1 && is_jingle(music_slots[i].path)) {
        jingle_playing = 1;
        break;
      }
    }
    
    // Mix music slots
    for (int i = 0; i < MAX_MUSIC_SLOTS; i++) {
      if (!music_slots[i].active || music_slots[i].state != 1) continue;
      
      // If a jingle is playing, temporarily pause the level BGM (slot 0) by skipping it
      if (i == 0 && jingle_playing) continue;
      
      int pos = music_slots[i].play_pos;
      if (pos >= music_slots[i].data_len) {
        if (music_slots[i].loop) {
          music_slots[i].play_pos = 0;
          pos = 0;
        } else {
          music_slots[i].state = 0; // stopped
          continue;
        }
      }
      
      int16_t sample_l = 0;
      int16_t sample_r = 0;
      if (music_slots[i].data) {
        sample_l = music_slots[i].data[pos * 2];
        sample_r = music_slots[i].data[pos * 2 + 1];
      }
      
      mix_l += (int32_t)(sample_l * music_slots[i].volume);
      mix_r += (int32_t)(sample_r * music_slots[i].volume);
      
      music_slots[i].play_pos++;
    }
    
    // Clamp to 16-bit range
    if (mix_l > 32767) mix_l = 32767;
    else if (mix_l < -32768) mix_l = -32768;
    
    if (mix_r > 32767) mix_r = 32767;
    else if (mix_r < -32768) mix_r = -32768;
    
    out[f * 2] = (int16_t)mix_l;
    out[f * 2 + 1] = (int16_t)mix_r;
  }
}

// API implementation
void audio_init(void) {
  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof(want));
  want.freq = TARGET_RATE;
  want.format = AUDIO_S16SYS;
  want.channels = TARGET_CHANNELS;
  want.samples = 1024;
  want.callback = audio_callback;
  
  audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (audio_device) {
    SDL_PauseAudioDevice(audio_device, 0);
  }
}

void audio_shutdown(void) {
  if (audio_device) {
    SDL_CloseAudioDevice(audio_device);
    audio_device = 0;
  }
  
  // Free cache
  for (int i = 0; i < cache_count; i++) {
    free(sound_cache[i].data);
  }
  cache_count = 0;
  
  // Free music
  for (int i = 0; i < MAX_MUSIC_SLOTS; i++) {
    if (music_slots[i].data) {
      free(music_slots[i].data);
      music_slots[i].data = NULL;
    }
  }
}

void audio_music_set_source(int id, const char *path) {
  if (id < 0 || id >= MAX_MUSIC_SLOTS) return;
  
  SDL_LockAudioDevice(audio_device);
  
  if (music_slots[id].data) {
    free(music_slots[id].data);
    music_slots[id].data = NULL;
  }
  
  music_slots[id].active = 0;
  music_slots[id].state = 0;
  music_slots[id].can_start = 0;
  strncpy(music_slots[id].path, path, 511);
  
  char resolved_path[512];
  char full_path[1024];
  if (resolve_audio_path(path, resolved_path, sizeof(resolved_path))) {
    get_full_path(resolved_path, full_path, sizeof(full_path));
  } else {
    get_full_path(path, full_path, sizeof(full_path));
  }
  
  printf("audio_music_set_source(id=%d, path='%s', full_path='%s')\n", id, path, full_path);
  
  SDL_UnlockAudioDevice(audio_device);
  int data_len = 0;
  int16_t *data = load_audio_file(full_path, &data_len);
  SDL_LockAudioDevice(audio_device);
  
  if (data) {
    printf("audio_music_set_source: Successfully loaded '%s' (%d frames)\n", full_path, data_len);
    music_slots[id].data = data;
    music_slots[id].data_len = data_len;
    music_slots[id].active = 1;
    music_slots[id].volume = 1.0f;
    music_slots[id].play_pos = 0;
    music_slots[id].state = 0;
    music_slots[id].can_start = 1; // mark as ready to start
    music_slots[id].loop = (id == 0) ? 1 : 0; // default loop to true ONLY for slot 0 (BGM)
  } else {
    printf("audio_music_set_source: Failed to load audio file '%s'. Using silence stub.\n", full_path);
    music_slots[id].data = NULL;
    music_slots[id].data_len = TARGET_RATE; // 1 second of silence
    music_slots[id].active = 1;
    music_slots[id].volume = 0.0f;
    music_slots[id].play_pos = 0;
    music_slots[id].state = 0;
    music_slots[id].can_start = 1;
    music_slots[id].loop = (id == 0) ? 1 : 0;
  }
  
  SDL_UnlockAudioDevice(audio_device);
}

void audio_music_start(int id) {
  if (id < 0 || id >= MAX_MUSIC_SLOTS) return;
  SDL_LockAudioDevice(audio_device);
  if (music_slots[id].active) {
    if (music_slots[id].state == 2) { // paused -> resume playing
      music_slots[id].state = 1;
    } else if (music_slots[id].state == 0) { // stopped -> start from beginning only if allowed
      if (music_slots[id].can_start) {
        music_slots[id].state = 1;
        music_slots[id].play_pos = 0;
        music_slots[id].can_start = 0; // consume starting token
      }
    }
  }
  
  // If the game starts/resumes slot 2, 3, or 4, it means the game is resuming!
  if (id >= 2 && id <= 4) {
    if (g_game_paused) {
      g_game_paused = 0;
      if (music_slots[0].active && music_slots[0].state == 2) {
        music_slots[0].state = 1; // resume BGM
      }
    }
  }
  if (id == 0) {
    g_game_paused = 0;
  }
  SDL_UnlockAudioDevice(audio_device);
}

void audio_music_stop(int id) {
  if (id < 0 || id >= MAX_MUSIC_SLOTS) return;
  SDL_LockAudioDevice(audio_device);
  if (music_slots[id].active) {
    music_slots[id].state = 0;
    music_slots[id].play_pos = 0;
    music_slots[id].can_start = 0; // stopped -> block from auto-starting until set_source is called again
  }
  SDL_UnlockAudioDevice(audio_device);
}

void audio_music_pause(int id) {
  if (id < 0 || id >= MAX_MUSIC_SLOTS) return;
  SDL_LockAudioDevice(audio_device);
  if (music_slots[id].active && music_slots[id].state == 1) {
    music_slots[id].state = 2;
  }
  
  // If the game pauses slot 2, 3, or 4, it means the game is pausing!
  if (id >= 2 && id <= 4) {
    g_game_paused = 1;
    if (music_slots[0].active && music_slots[0].state == 1) {
      music_slots[0].state = 2; // pause BGM
    }
  }
  SDL_UnlockAudioDevice(audio_device);
}

void audio_music_volume(int id, float vol) {
  if (id < 0 || id >= MAX_MUSIC_SLOTS) return;
  SDL_LockAudioDevice(audio_device);
  if (music_slots[id].active) {
    music_slots[id].volume = vol;
  }
  SDL_UnlockAudioDevice(audio_device);
}

void audio_music_set_loop(int id, int loop) {
  if (id < 0 || id >= MAX_MUSIC_SLOTS) return;
  SDL_LockAudioDevice(audio_device);
  if (music_slots[id].active) {
    music_slots[id].loop = loop;
  }
  SDL_UnlockAudioDevice(audio_device);
}

void audio_music_release(int id) {
  if (id < 0 || id >= MAX_MUSIC_SLOTS) return;
  SDL_LockAudioDevice(audio_device);
  if (music_slots[id].data) {
    free(music_slots[id].data);
    music_slots[id].data = NULL;
  }
  music_slots[id].active = 0;
  music_slots[id].state = 0;
  SDL_UnlockAudioDevice(audio_device);
}

int audio_music_get_state(int id) {
  if (id < 0 || id >= MAX_MUSIC_SLOTS) return 0;
  int state = 0;
  SDL_LockAudioDevice(audio_device);
  if (music_slots[id].active) {
    state = music_slots[id].state;
  }
  SDL_UnlockAudioDevice(audio_device);
  return state;
}

static int is_looping_sfx(const char *logical) {
  char lower[256];
  int i = 0;
  for (; logical[i] && i < 255; i++) {
    char c = logical[i];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    lower[i] = c;
  }
  lower[i] = '\0';

  if (strncmp(lower, "env", 3) == 0) return 1;

  const char *loop_names[] = {
    "abry01", "avalanche01", "avalanche02", "bigrock2", "bigrock3", "burner01",
    "catapult1", "e2_boss1_05", "e2_boss1_17", "e2_boss2_01", "e2_boss2_02",
    "e2_boss2_04", "e2_boss2_10", "e2_boss3_16", "e2_boss4_07", "e2_bossf_03",
    "e2_bossf_05", "e2_bossf_06", "e2_bossf_08", "e2_bossf_20", "e2_bossm_01",
    "e2_bossm_02", "e2_bossm_07", "e2_bossm_08", "e2_bossm_10", "lightring02",
    "ms_burner1", "oilroad01", "oilslider01", "propeller01", "rail01",
    "rotary_sw01", "rotary_sw02", "sand02", "sandbranch01", "sandbranch04",
    "sandstorm01", "sandstorm02", "sandtrank02", "scara02", "shutter01",
    "snowball01", "snowwall01", "tlsscrew", "tomado01", "tomado05"
  };
  int n_names = sizeof(loop_names) / sizeof(loop_names[0]);
  for (int j = 0; j < n_names; j++) {
    if (strcmp(lower, loop_names[j]) == 0) return 1;
  }
  return 0;
}

int audio_play_sound(const char *name, float vol, int slot_id) {
  if (!name || !name[0]) return -1;
  
  int loop = 0;
  if (is_looping_sfx(name)) {
    loop = 1;
  }
  
  SDL_LockAudioDevice(audio_device);
  
  // If slot_id is tracked, stop any sound playing on this slot first
  if (slot_id >= 0) {
    for (int i = 0; i < MAX_SOUND_CHANNELS; i++) {
      if (sound_channels[i].active && sound_channels[i].slot_id == slot_id) {
        sound_channels[i].active = 0;
      }
    }
  }
  
  int16_t *data = NULL;
  int data_len = 0;
  for (int i = 0; i < cache_count; i++) {
    if (strcasecmp(sound_cache[i].path, name) == 0) {
      data = sound_cache[i].data;
      data_len = sound_cache[i].data_len;
      break;
    }
  }
  
  if (!data) {
    char resolved_path[512];
    char full_path[1024];
    if (resolve_audio_path(name, resolved_path, sizeof(resolved_path))) {
      get_full_path(resolved_path, full_path, sizeof(full_path));
    } else {
      get_full_path(name, full_path, sizeof(full_path));
    }
    
    SDL_UnlockAudioDevice(audio_device);
    data = load_audio_file(full_path, &data_len);
    SDL_LockAudioDevice(audio_device);
    
    if (data) {
      if (cache_count < MAX_CACHE_ENTRIES) {
        strncpy(sound_cache[cache_count].path, name, 255);
        sound_cache[cache_count].data = data;
        sound_cache[cache_count].data_len = data_len;
        cache_count++;
      }
    } else {
      printf("audio_play_sound: Failed to load sound effect '%s' from '%s'\n", name, full_path);
    }
  }
  
  if (!data) {
    SDL_UnlockAudioDevice(audio_device);
    return -1;
  }
  
  int ch_idx = -1;
  for (int i = 0; i < MAX_SOUND_CHANNELS; i++) {
    if (!sound_channels[i].active) {
      ch_idx = i;
      break;
    }
  }
  
  if (ch_idx == -1) {
    ch_idx = 0; // overwrite channel 0 if all full
  }
  
  int stream_id = slot_id >= 0 ? slot_id : next_stream_id++;
  sound_channels[ch_idx].active = 1;
  sound_channels[ch_idx].loop = loop;
  sound_channels[ch_idx].volume = vol;
  sound_channels[ch_idx].play_pos = 0;
  sound_channels[ch_idx].data_len = data_len;
  sound_channels[ch_idx].data = data;
  sound_channels[ch_idx].stream_id = stream_id;
  sound_channels[ch_idx].slot_id = slot_id;
  strncpy(sound_channels[ch_idx].name, name, sizeof(sound_channels[ch_idx].name) - 1);
  sound_channels[ch_idx].name[sizeof(sound_channels[ch_idx].name) - 1] = '\0';
  
  SDL_UnlockAudioDevice(audio_device);
  return stream_id;
}

void audio_stop_sound(int streamId) {
  if (streamId < 0) return;
  SDL_LockAudioDevice(audio_device);
  for (int i = 0; i < MAX_SOUND_CHANNELS; i++) {
    if (sound_channels[i].active && (sound_channels[i].slot_id == streamId || sound_channels[i].stream_id == streamId)) {
      sound_channels[i].active = 0;
    }
  }
  SDL_UnlockAudioDevice(audio_device);
}

void audio_pause_sound(int streamId) {
  if (streamId < 0) return;
  SDL_LockAudioDevice(audio_device);
  for (int i = 0; i < MAX_SOUND_CHANNELS; i++) {
    if (sound_channels[i].active == 1 && (sound_channels[i].slot_id == streamId || sound_channels[i].stream_id == streamId)) {
      sound_channels[i].active = 2; // 2 = paused
    }
  }
  SDL_UnlockAudioDevice(audio_device);
}

void audio_resume_sound(int streamId) {
  if (streamId < 0) return;
  SDL_LockAudioDevice(audio_device);
  for (int i = 0; i < MAX_SOUND_CHANNELS; i++) {
    if (sound_channels[i].active == 2 && (sound_channels[i].slot_id == streamId || sound_channels[i].stream_id == streamId)) {
      sound_channels[i].active = 1;
    }
  }
  SDL_UnlockAudioDevice(audio_device);
}

void audio_set_sound_volume(int streamId, float vol) {
  if (streamId < 0) return;
  SDL_LockAudioDevice(audio_device);
  for (int i = 0; i < MAX_SOUND_CHANNELS; i++) {
    if (sound_channels[i].active && (sound_channels[i].slot_id == streamId || sound_channels[i].stream_id == streamId)) {
      sound_channels[i].volume = vol;
    }
  }
  SDL_UnlockAudioDevice(audio_device);
}
