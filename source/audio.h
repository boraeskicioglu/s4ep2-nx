/* audio.h -- SDL2-based audio engine for libfox.so JNI AudioHelper mapping
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __AUDIO_H__
#define __AUDIO_H__

void audio_init(void);
void audio_shutdown(void);

// Music (BGM)
void audio_music_set_source(int id, const char *path);
void audio_music_start(int id);
void audio_music_stop(int id);
void audio_music_pause(int id);
void audio_music_volume(int id, float vol);
void audio_music_set_loop(int id, int loop);
void audio_music_release(int id);
int audio_music_get_state(int id); // 0=stopped, 1=playing, 2=paused

// Sound Effects (SE)
int audio_play_sound(const char *name, float vol, int loop);
void audio_stop_sound(int streamId);
void audio_pause_sound(int streamId);
void audio_resume_sound(int streamId);
void audio_set_sound_volume(int streamId, float vol);

#endif
