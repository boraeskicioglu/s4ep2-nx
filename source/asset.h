/* asset.h -- AAssetManager NDK emulation over the loose Chrono Trigger assets
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * The Android build keeps the game data in the APK's assets/ folder
 * (resources.bin, 001.dat..008.dat, Shaders/, build_date.txt). cocos2d-x reads
 * them through the NDK AAsset API and through cocos FileUtilsAndroid, which both
 * go via an AAssetManager. There is no devkitPro AAsset implementation, so we
 * provide one here that serves files straight out of the ASSETS_DIR directory on
 * the SD card. No decryption is needed at this layer: the .dat / resources.bin
 * archives are parsed (and any per-file obfuscation undone) inside libchrono.so.
 */

#ifndef __ASSET_H__
#define __ASSET_H__

#include <stdint.h>
#include <stddef.h>

// opaque AAsset / AAssetDir handles (we hand the engine our own structs)
typedef struct CtAsset CtAsset;
typedef struct CtAssetDir CtAssetDir;

// returns 1 if the named asset (path relative to the assets root) exists.
int asset_exists(const char *name);

// open a real fd onto an asset for the engine's streaming/mmap paths.
// returns the fd (>=0) and fills *out_off (always 0) and *out_len; -1 on miss.
int asset_open_fd(const char *name, int64_t *out_off, int64_t *out_len);

// --- AAsset NDK surface (wired into the import table in imports.c) ----------

void *AAssetManager_fromJava_fake(void *env, void *mgr);
void *AAssetManager_open_fake(void *mgr, const char *path, int mode);
void  AAsset_close_fake(void *asset);
int   AAsset_read_fake(void *asset, void *buf, size_t count);
long  AAsset_seek_fake(void *asset, long off, int whence);
int64_t AAsset_seek64_fake(void *asset, int64_t off, int whence);
long  AAsset_getLength_fake(void *asset);
int64_t AAsset_getLength64_fake(void *asset);
long  AAsset_getRemainingLength_fake(void *asset);
int64_t AAsset_getRemainingLength64_fake(void *asset);
const void *AAsset_getBuffer_fake(void *asset);
int   AAsset_openFileDescriptor_fake(void *asset, off_t *outStart, off_t *outLen);
int   AAsset_isAllocated_fake(void *asset);

void *AAssetManager_openDir_fake(void *mgr, const char *dir);
const char *AAssetDir_getNextFileName_fake(void *dir);
void  AAssetDir_rewind_fake(void *dir);
void  AAssetDir_close_fake(void *dir);

#endif
