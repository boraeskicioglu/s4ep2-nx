/* opensles.h -- minimal OpenSL ES shim for the SQEX "Sd" sound driver,
 * backed by a single software-mixed SDL2 audio device.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __OPENSLES_H__
#define __OPENSLES_H__

#include <stdint.h>

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                        uint32_t numInterfaces, const void *pInterfaceIds,
                        const void *pInterfaceRequired);

void opensles_shutdown(void);

// FMV audio ring (movie_player.c), mixed into the SDL output beside the engine.
int opensles_movie_begin(int requested_rate);              // -> device rate, 0 on fail
int opensles_movie_queue(const int16_t *pcm, int frames);  // interleaved stereo
void opensles_movie_set_paused(int paused);
uint64_t opensles_movie_samples_played(void);
void opensles_movie_end(void);

// interface-id tokens referenced by libchrono.so relocations: each a unique
// non-NULL self-addressed sentinel compared by pointer in GetInterface.
extern void *SL_IID_3DCOMMIT, *SL_IID_3DDOPPLER, *SL_IID_3DGROUPING, *SL_IID_3DLOCATION;
extern void *SL_IID_3DMACROSCOPIC, *SL_IID_3DSOURCE, *SL_IID_ANDROIDCONFIGURATION;
extern void *SL_IID_ANDROIDEFFECT, *SL_IID_ANDROIDEFFECTCAPABILITIES, *SL_IID_ANDROIDEFFECTSEND;
extern void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE, *SL_IID_AUDIODECODERCAPABILITIES, *SL_IID_AUDIOENCODER;
extern void *SL_IID_AUDIOENCODERCAPABILITIES, *SL_IID_AUDIOIODEVICECAPABILITIES, *SL_IID_BASSBOOST;
extern void *SL_IID_BUFFERQUEUE, *SL_IID_DEVICEVOLUME, *SL_IID_DYNAMICINTERFACEMANAGEMENT;
extern void *SL_IID_DYNAMICSOURCE, *SL_IID_EFFECTSEND, *SL_IID_ENGINE, *SL_IID_ENGINECAPABILITIES;
extern void *SL_IID_ENVIRONMENTALREVERB, *SL_IID_EQUALIZER, *SL_IID_LED, *SL_IID_METADATAEXTRACTION;
extern void *SL_IID_METADATATRAVERSAL, *SL_IID_MIDIMESSAGE, *SL_IID_MIDIMUTESOLO, *SL_IID_MIDITEMPO;
extern void *SL_IID_MIDITIME, *SL_IID_MUTESOLO, *SL_IID_NULL, *SL_IID_OBJECT, *SL_IID_OUTPUTMIX;
extern void *SL_IID_PITCH, *SL_IID_PLAY, *SL_IID_PLAYBACKRATE, *SL_IID_PREFETCHSTATUS;
extern void *SL_IID_PRESETREVERB, *SL_IID_RATEPITCH, *SL_IID_RECORD, *SL_IID_SEEK, *SL_IID_THREADSYNC;
extern void *SL_IID_VIBRA, *SL_IID_VIRTUALIZER, *SL_IID_VISUALIZATION, *SL_IID_VOLUME;

#endif
