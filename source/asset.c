/* asset.c -- AAssetManager NDK emulation over the loose Chrono Trigger assets
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "config.h"
#include "asset.h"
#include "util.h"

// An absolute path: a libnx device path ("sdmc:/...") or leading '/'. cocos only
// treats '/'-prefixed paths as filesystem files, so our writable dir gets
// misrouted here -- open those directly instead of prepending ASSETS_DIR, or the
// engine can't read back the saves/common.bin it writes there.
static int is_abs_path(const char *p) {
  if (!p) return 0;
  if (p[0] == '/') return 1;
  return strstr(p, ":/") != NULL; // "sdmc:/", "romfs:/", ...
}

// The engine passes asset names relative to the APK assets root. We serve them
// from ASSETS_DIR. A leading "assets/" or "./" is tolerated and stripped.
// Absolute / device paths are passed through verbatim.
static void resolve(char *out, size_t out_size, const char *name) {
  if (!name) name = "";
  if (is_abs_path(name)) {
    snprintf(out, out_size, "%s", name);
    return;
  }
  while (name[0] == '/' || (name[0] == '.' && name[1] == '/'))
    name += (name[0] == '/') ? 1 : 2;
  if (!strncmp(name, "assets/", 7))
    name += 7;
  snprintf(out, out_size, "%s/%s", ASSETS_DIR, name);
}

int asset_exists(const char *name) {
  char path[1024];
  resolve(path, sizeof(path), name);
  struct stat st;
  return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

int asset_open_fd(const char *name, int64_t *out_off, int64_t *out_len) {
  char path[1024];
  resolve(path, sizeof(path), name);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }
  if (out_off) *out_off = 0;
  if (out_len) *out_len = (int64_t)st.st_size;
  return fd;
}

// ---------------------------------------------------------------------------
// AAsset: we always back assets with a buffered FILE* opened from ASSETS_DIR.
// The big archives (resources.bin, *.dat) issue many small reads, so a large
// stream buffer keeps the per-read fsdev round-trips down.
// ---------------------------------------------------------------------------

struct CtAsset {
  FILE *f;
  int64_t size;
  int64_t pos;
  uint8_t *whole;   // lazily materialised for AAsset_getBuffer
};

void *AAssetManager_fromJava_fake(void *env, void *mgr) {
  (void)env; (void)mgr;
  return (void *)1; // any non-NULL token; we ignore it
}

void *AAssetManager_open_fake(void *mgr, const char *path, int mode) {
  (void)mgr; (void)mode;
  char real[1024];
  resolve(real, sizeof(real), path);

  FILE *f = fopen(real, "rb");
  debugPrintf("AAssetManager_open: %s (real: %s) -> %s\n", path, real, f ? "OK" : "FAIL");
  if (!f)
    return NULL;

  CtAsset *a = calloc(1, sizeof(*a));
  if (!a) { fclose(f); return NULL; }

  // a generous buffer; archives are read in long sequential bursts
  setvbuf(f, NULL, _IOFBF, 256 * 1024);
  fseek(f, 0, SEEK_END);
  a->size = ftell(f);
  fseek(f, 0, SEEK_SET);
  a->f = f;
  return a;
}

void AAsset_close_fake(void *asset) {
  CtAsset *a = asset;
  if (!a)
    return;
  if (a->f)
    fclose(a->f);
  free(a->whole);
  free(a);
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  CtAsset *a = asset;
  if (!a || !a->f)
    return -1;
  size_t n = fread(buf, 1, count, a->f);
  a->pos += (int64_t)n;
  return (int)n;
}

long AAsset_seek_fake(void *asset, long off, int whence) {
  return (long)AAsset_seek64_fake(asset, (int64_t)off, whence);
}

int64_t AAsset_seek64_fake(void *asset, int64_t off, int whence) {
  CtAsset *a = asset;
  if (!a || !a->f)
    return -1;
  if (fseek(a->f, (long)off, whence) != 0)
    return -1;
  a->pos = (int64_t)ftell(a->f);
  return a->pos;
}

long AAsset_getLength_fake(void *asset) {
  CtAsset *a = asset;
  return a ? (long)a->size : 0;
}

int64_t AAsset_getLength64_fake(void *asset) {
  CtAsset *a = asset;
  return a ? a->size : 0;
}

long AAsset_getRemainingLength_fake(void *asset) {
  CtAsset *a = asset;
  return a ? (long)(a->size - a->pos) : 0;
}

int64_t AAsset_getRemainingLength64_fake(void *asset) {
  CtAsset *a = asset;
  return a ? (a->size - a->pos) : 0;
}

// some cocos paths memory-map small assets; materialise the whole file once.
const void *AAsset_getBuffer_fake(void *asset) {
  CtAsset *a = asset;
  if (!a || !a->f)
    return NULL;
  if (!a->whole) {
    a->whole = malloc(a->size ? (size_t)a->size : 1);
    if (!a->whole)
      return NULL;
    long save = ftell(a->f);
    fseek(a->f, 0, SEEK_SET);
    if (fread(a->whole, 1, (size_t)a->size, a->f) != (size_t)a->size) {
      free(a->whole);
      a->whole = NULL;
    }
    fseek(a->f, save, SEEK_SET);
  }
  return a->whole;
}

// AAsset_openFileDescriptor is unsupported over a buffered FILE*; report failure
// so callers fall back to AAsset_read (which we fully support).
int AAsset_openFileDescriptor_fake(void *asset, off_t *outStart, off_t *outLen) {
  (void)asset; (void)outStart; (void)outLen;
  return -1;
}

int AAsset_isAllocated_fake(void *asset) {
  (void)asset;
  return 0;
}

// ---------------------------------------------------------------------------
// AAssetDir: directory enumeration over ASSETS_DIR/<dir>
// ---------------------------------------------------------------------------

struct CtAssetDir {
  DIR *d;
  char base[1024];
  char name[512]; // storage returned by getNextFileName
};

void *AAssetManager_openDir_fake(void *mgr, const char *dir) {
  (void)mgr;
  CtAssetDir *ad = calloc(1, sizeof(*ad));
  if (!ad)
    return NULL;
  resolve(ad->base, sizeof(ad->base), dir ? dir : "");
  ad->d = opendir(ad->base);
  // Android returns a valid (possibly empty) AAssetDir even for missing dirs;
  // keep the handle either way so getNextFileName just yields nothing.
  return ad;
}

const char *AAssetDir_getNextFileName_fake(void *dir) {
  CtAssetDir *ad = dir;
  if (!ad || !ad->d)
    return NULL;
  struct dirent *e;
  while ((e = readdir(ad->d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    // AAssetDir lists regular files only
    char full[1600];
    snprintf(full, sizeof(full), "%s/%s", ad->base, e->d_name);
    struct stat st;
    if (stat(full, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
      snprintf(ad->name, sizeof(ad->name), "%s", e->d_name);
      return ad->name;
    }
  }
  return NULL;
}

void AAssetDir_rewind_fake(void *dir) {
  CtAssetDir *ad = dir;
  if (ad && ad->d)
    rewinddir(ad->d);
}

void AAssetDir_close_fake(void *dir) {
  CtAssetDir *ad = dir;
  if (!ad)
    return;
  if (ad->d)
    closedir(ad->d);
  free(ad);
}
