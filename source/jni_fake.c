/* jni_fake.c -- fake JNI environment for libfox.so (Sonic 4 Episode II)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "audio.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

#define eq(a, b) (strcmp(a, b) == 0)

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  singleton, never freed
};

typedef struct { uint32_t tag; char label[96]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char name[96]; char cls[96]; } FakeID;

typedef struct {
  uint32_t tag;
  char label[96];
  FILE *f;
  void *data_arr; // FakePriArray *
  int length;
  int position;
} FakeAPKFile;

volatile int jni_quit_requested = 0;
volatile int g_continue_flag = 2;

// ---------------------------------------------------------------------------
// local reference registry (native code must free the refs it creates)
// ---------------------------------------------------------------------------

#define MAX_LOCALS 32768
#define MAX_FRAMES 128
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS) locals[locals_top++] = ref;
    else debugPrintf("JNI: local ref table full, leaking %p\n", ref);
    mutexUnlock(&locals_lock);
  }
  return ref;
}

static void free_ref(void *ref) {
  if (!ref) return;
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: {
      FakeObject *obj = ref;
      if (strcmp(obj->label, "APKFile") == 0) {
        FakeAPKFile *file = ref;
        if (file->f) fclose(file->f);
      }
      free(ref);
      break;
    }
    default: break; // TAG_ID / TAG_CLASS are pooled, never freed
  }
}

static void delete_local(void *ref) {
  if (!ref) return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// constructors
// ---------------------------------------------------------------------------

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  if (label) snprintf(o->label, sizeof(o->label), "%s", label);
  return reg_local(o);
}

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

void *jni_new_byte_array(const void *data, int len) {
  void *buf = malloc(len > 0 ? len : 1);
  if (data && len > 0) memcpy(buf, data, len);
  else if (len > 0) memset(buf, 0, len);
  return make_pri_array_adopt(buf, len, 1);
}
void *jni_new_int_array(const int *data, int len) {
  int *buf = malloc((len > 0 ? len : 1) * sizeof(int));
  if (data && len > 0) memcpy(buf, data, (size_t)len * sizeof(int));
  return make_pri_array_adopt(buf, len, 4);
}
void *jni_new_float_array(const float *data, int len) {
  float *buf = malloc((len > 0 ? len : 1) * sizeof(float));
  if (data && len > 0) memcpy(buf, data, (size_t)len * sizeof(float));
  return make_pri_array_adopt(buf, len, 4);
}
void jni_delete_ref(void *ref) { delete_local(ref); }

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING) return s->utf;
  return "";
}

// ---------------------------------------------------------------------------
// class + classloader + method-id pools (never freed)
// ---------------------------------------------------------------------------

#define MAX_CLASSES 128
static FakeObject class_pool[MAX_CLASSES];
static int class_count = 0;

static void *get_class(const char *name) {
  if (!name) name = "";
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].label, name))
      return &class_pool[i];
  if (class_count >= MAX_CLASSES) return &class_pool[0];
  FakeObject *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  strncpy(c->label, name, sizeof(c->label) - 1);
  return c;
}

#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  (void)sig;
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].cls, cls ? cls : ""))
      return &id_pool[i];
  if (id_count >= MAX_IDS) { debugPrintf("JNI: id pool full (%s)\n", name); return &id_pool[0]; }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  snprintf(id->name, sizeof(id->name), "%s", name);
  snprintf(id->cls, sizeof(id->cls), "%s", cls ? cls : "");
  return id;
}

// ---------------------------------------------------------------------------
// Switch Vibration / Rumble helper
// ---------------------------------------------------------------------------

static HidVibrationDeviceHandle vib_device_handles[2][2]; // [controllers][left/right]
static int vib_initialized = 0;

static void vib_init(void) {
  hidInitializeVibrationDevices(vib_device_handles[0], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadHandheld);
  hidInitializeVibrationDevices(vib_device_handles[1], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
  vib_initialized = 1;
}

typedef struct { int ms; } VibThreadArg;
static void *vib_thread(void *arg) {
  VibThreadArg *v = arg;
  svcSleepThread((u64)v->ms * 1000000ull); // ms to ns
  HidVibrationValue zero = {0};
  hidSendVibrationValue(vib_device_handles[0][0], &zero);
  hidSendVibrationValue(vib_device_handles[0][1], &zero);
  hidSendVibrationValue(vib_device_handles[1][0], &zero);
  hidSendVibrationValue(vib_device_handles[1][1], &zero);
  free(v);
  return NULL;
}

static void vib_vibrate(int ms) {
  if (!vib_initialized) vib_init();
  HidVibrationValue vib_value = {
    .amp_low = 0.2f,
    .freq_low = 160.0f,
    .amp_high = 0.2f,
    .freq_high = 320.0f
  };
  hidSendVibrationValue(vib_device_handles[0][0], &vib_value);
  hidSendVibrationValue(vib_device_handles[0][1], &vib_value);
  hidSendVibrationValue(vib_device_handles[1][0], &vib_value);
  hidSendVibrationValue(vib_device_handles[1][1], &vib_value);
  
  VibThreadArg *arg = malloc(sizeof(*arg));
  arg->ms = ms;
  pthread_t t;
  if (pthread_create(&t, NULL, vib_thread, arg) == 0) {
    pthread_detach(t);
  } else {
    free(arg);
  }
}

// ---------------------------------------------------------------------------
// writable path / package identity
// ---------------------------------------------------------------------------

static char g_writable[512];

static const char *writable_dir(void) {
  if (!g_writable[0]) {
    strcpy(g_writable, "sdmc:/switch/s4ep2");
  }
  return g_writable;
}

// ---------------------------------------------------------------------------
// JNI Method Call Handlers
// ---------------------------------------------------------------------------

static void *j_NewByteArray(void *env, int len);

static int is_frequent_jni_call(const char *name) {
  if (!name) return 0;
  if (strcmp(name, "GetMusicState") == 0 ||
      strcmp(name, "PlaySound") == 0 ||
      strcmp(name, "StopSound") == 0 ||
      strcmp(name, "PauseSound") == 0 ||
      strcmp(name, "ResumeSound") == 0 ||
      strcmp(name, "SetVolume") == 0 ||
      strcmp(name, "isPadButtonA") == 0 ||
      strcmp(name, "isPadButtonB") == 0 ||
      strcmp(name, "isPadCancelButton") == 0 ||
      strcmp(name, "isPadConfirmButton") == 0 ||
      strcmp(name, "IsBluetoothEnabled") == 0 ||
      strcmp(name, "IsInternetEnabled") == 0 ||
      strcmp(name, "IsFujisModel") == 0 ||
      strcmp(name, "GetBuildTarget") == 0 ||
      strcmp(name, "GetMarketTarget") == 0 ||
      strcmp(name, "IsTegra3Version") == 0 ||
      strcmp(name, "IsShieldDevice") == 0 ||
      strcmp(name, "IsExitBlock") == 0 ||
      strcmp(name, "EP1HasInstalled") == 0 ||
      strcmp(name, "isInitParam") == 0 ||
      strcmp(name, "isRVForPaidUser") == 0 ||
      strcmp(name, "isUserConsentGDPR") == 0) {
    return 1;
  }
  return 0;
}

static void *call_object(const char *cls, const char *name, va_list va) {
  if (!is_frequent_jni_call(name)) {
    debugPrintf("JNI: call_object: %s::%s\n", cls ? cls : "NULL", name ? name : "NULL");
  }
  if (eq(cls, "com/mineloader/fox/AudioHelper") && eq(name, "getInstance")) {
    return get_class("AudioHelper");
  }
  if (eq(cls, "com/mineloader/fox/VibHelper") && eq(name, "getInstance")) {
    return get_class("VibHelper");
  }
  if (eq(cls, "com/mineloader/fox/APKFileHelper") && eq(name, "getInstance")) {
    return get_class("APKFileHelper");
  }
  if (eq(name, "openFileAndroid")) {
    char obb_path[1024];
    snprintf(obb_path, sizeof(obb_path), "%s/assets/data.obb", writable_dir());
    
    FILE *f = fopen(obb_path, "rb");
    if (!f) {
      snprintf(obb_path, sizeof(obb_path), "assets/data.obb");
      f = fopen(obb_path, "rb");
    }
    if (!f) {
      debugPrintf("JNI: openFileAndroid failed to open data.obb (path: %s)\n", obb_path);
      return NULL;
    }
    
    FakeAPKFile *file = calloc(1, sizeof(*file));
    file->tag = TAG_OBJECT;
    snprintf(file->label, sizeof(file->label), "APKFile");
    file->f = f;
    
    fseek(f, 0, SEEK_END);
    file->length = ftell(f);
    fseek(f, 0, SEEK_SET);
    file->position = 0;
    file->data_arr = j_NewByteArray(fake_env, 4096);
    
    debugPrintf("JNI: openFileAndroid opened data.obb, size = %d\n", file->length);
    return reg_local(file);
  }
  if (eq(name, "getFilesDir")) {
    return jni_make_string(writable_dir());
  }
  return NULL;
}

static void call_void(const char *cls, const char *name, va_list va) {
  if (!is_frequent_jni_call(name)) {
    debugPrintf("JNI: call_void: %s::%s\n", cls ? cls : "NULL", name ? name : "NULL");
  }
  if (eq(cls, "com/sega/f2fextension/Android_Utils") && eq(name, "playIntroVideo")) {
    extern volatile int should_resume_video;
    should_resume_video = 1;
    return;
  }
  if (eq(cls, "com/mineloader/fox/FoxActivity_Core")) {
    if (eq(name, "updateContinueFlag")) {
      int flag = va_arg(va, int);
      debugPrintf("JNI: FoxActivity_Core::updateContinueFlag(flag=%d)\n", flag);
      g_continue_flag = flag;
      return;
    }
  }
  if (eq(cls, "com/mineloader/fox/AudioHelper")) {
    if (eq(name, "OpenContinueMsg")) {
      debugPrintf("JNI: AudioHelper::OpenContinueMsg called. Auto-responding with SetContinueFlag(1)...\n");
      extern volatile int should_set_continue_flag;
      should_set_continue_flag = 1;
      return;
    }
    if (eq(name, "Initialise") || eq(name, "SysLockPaused") || eq(name, "SysLockResumed") || eq(name, "SetMaxVolume")) {
      return;
    }
    if (eq(name, "MusicSetDataSource")) {
      int id = va_arg(va, int);
      const char *path = obj_str(va_arg(va, void *));
      debugPrintf("JNI: MusicSetDataSource(id=%d, path='%s')\n", id, path);
      audio_music_set_source(id, path);
      return;
    }
    if (eq(name, "MusicStart")) {
      int id = va_arg(va, int);
      debugPrintf("JNI: MusicStart(id=%d)\n", id);
      audio_music_start(id);
      return;
    }
    if (eq(name, "MusicStop")) {
      int id = va_arg(va, int);
      debugPrintf("JNI: MusicStop(id=%d)\n", id);
      audio_music_stop(id);
      return;
    }
    if (eq(name, "MusicPause")) {
      int id = va_arg(va, int);
      debugPrintf("JNI: MusicPause(id=%d)\n", id);
      audio_music_pause(id);
      return;
    }
    if (eq(name, "MusicVolume")) {
      int id = va_arg(va, int);
      double vol = va_arg(va, double);
      debugPrintf("JNI: MusicVolume(id=%d, vol=%f)\n", id, vol);
      audio_music_volume(id, (float)vol);
      return;
    }
    if (eq(name, "MusicSetLoopFlag")) {
      int id = va_arg(va, int);
      int loop = va_arg(va, int);
      debugPrintf("JNI: MusicSetLoopFlag(id=%d, loop=%d)\n", id, loop);
      audio_music_set_loop(id, loop);
      return;
    }
    if (eq(name, "MusicRelease")) {
      int id = va_arg(va, int);
      debugPrintf("JNI: MusicRelease(id=%d)\n", id);
      audio_music_release(id);
      return;
    }
    if (eq(name, "StopSound")) {
      int streamId = va_arg(va, int);
      // debugPrintf("JNI: StopSound(streamId=%d)\n", streamId);
      audio_stop_sound(streamId);
      return;
    }
    if (eq(name, "PauseSound")) {
      int streamId = va_arg(va, int);
      // debugPrintf("JNI: PauseSound(streamId=%d)\n", streamId);
      audio_pause_sound(streamId);
      return;
    }
    if (eq(name, "ResumeSound")) {
      int streamId = va_arg(va, int);
      // debugPrintf("JNI: ResumeSound(streamId=%d)\n", streamId);
      audio_resume_sound(streamId);
      return;
    }
    if (eq(name, "SetVolume")) {
      int streamId = va_arg(va, int);
      double vol = va_arg(va, double);
      // debugPrintf("JNI: SetVolume(streamId=%d, vol=%f)\n", streamId, vol);
      audio_set_sound_volume(streamId, (float)vol);
      return;
    }
  }
  if (eq(cls, "com/mineloader/fox/VibHelper")) {
    if (eq(name, "Init") || eq(name, "setContext")) {
      return;
    }
    if (eq(name, "Vibrate")) {
      int ms = va_arg(va, int);
      vib_vibrate(ms);
      return;
    }
  }
  if (eq(cls, "com/mineloader/fox/APKFileHelper")) {
    if (eq(name, "closeFileAndroid")) {
      FakeAPKFile *file = va_arg(va, void *);
      if (file && file->tag == TAG_OBJECT && file->f) {
        fclose(file->f);
        file->f = NULL;
        debugPrintf("JNI: closed data.obb\n");
      }
      return;
    }
    if (eq(name, "readFileAndroid")) {
      FakeAPKFile *file = va_arg(va, void *);
      int bytes = va_arg(va, int);
      if (file && file->tag == TAG_OBJECT && file->f) {
        FakePriArray *arr = file->data_arr;
        if (arr && arr->tag == TAG_PRIARR) {
          int count = bytes;
          if (count > arr->len) count = arr->len;
          size_t n = fread(arr->data, 1, count, file->f);
          file->position += n;
        }
      }
      return;
    }
  }
}

static jint call_int(const char *cls, const char *name, va_list va) {
  if (!is_frequent_jni_call(name)) {
    debugPrintf("JNI: call_int: %s::%s\n", cls ? cls : "NULL", name ? name : "NULL");
  }
  if (eq(cls, "com/mineloader/fox/AudioHelper")) {
    if (eq(name, "PlaySound")) {
      const char *sound_name = obj_str(va_arg(va, void *));
      double vol = va_arg(va, double);
      int loop = va_arg(va, int);
      int res = audio_play_sound(sound_name, (float)vol, loop);
      // debugPrintf("JNI: PlaySound(name='%s', vol=%f, loop=%d) -> %d\n", sound_name, vol, loop, res);
      return res;
    }
    if (eq(name, "GetMusicState")) {
      int id = va_arg(va, int);
      int state = audio_music_get_state(id);
      // debugPrintf("JNI: GetMusicState(id=%d) -> %d\n", id, state);
      return state;
    }
    if (eq(name, "VideoSetDataSource")) {
      return 1;
    }
    if (eq(name, "VideoIsPlaying")) {
      return 0;
    }
    if (eq(name, "isDoneBuildSp") || eq(name, "isDoneBuildBgm")) {
      return 1;
    }
  }
  if (eq(cls, "com/mineloader/fox/APKFileHelper")) {
    if (eq(name, "seekFileAndroid")) {
      FakeAPKFile *file = va_arg(va, void *);
      int offset = va_arg(va, int);
      if (file && file->tag == TAG_OBJECT && file->f) {
        if (fseek(file->f, offset, SEEK_SET) == 0) {
          file->position = offset;
        }
        return file->position;
      }
      return 0;
    }
  }
  if (eq(cls, "com/mineloader/fox/foxJniLib")) {
    if (eq(name, "GetBuildTarget") || eq(name, "GetMarketTarget")) {
      return 1;
    }
  }
  return 0;
}

static jboolean call_bool(const char *cls, const char *name, va_list va) {
  if (!is_frequent_jni_call(name)) {
    debugPrintf("JNI: call_bool: %s::%s\n", cls ? cls : "NULL", name ? name : "NULL");
  }
  if (eq(cls, "com/mineloader/fox/foxJniLib")) {
    if (eq(name, "IsTegra3Version") || eq(name, "IsShieldDevice") || eq(name, "IsExitBlock")) {
      return 1;
    }
    if (eq(name, "isPadButtonA")) {
      int kc = va_arg(va, int);
      return (kc == 96 || kc == 8);
    }
    if (eq(name, "isPadButtonB")) {
      int kc = va_arg(va, int);
      return (kc == 97 || kc == 9);
    }
    if (eq(name, "isPadCancelButton")) {
      int kc = va_arg(va, int);
      return (kc == 4 || kc == 109 || kc == 111 || kc == 97 || kc == 9);
    }
    if (eq(name, "isPadConfirmButton")) {
      int kc = va_arg(va, int);
      return (kc == 66 || kc == 108 || kc == 96 || kc == 8);
    }
  }
  if (eq(cls, "com/mineloader/fox/AudioHelper")) {
    if (eq(name, "EP1HasInstalled")) {
      return 1;
    }
  }
  if (eq(cls, "com/sega/f2fextension/F2FAndroidJNI")) {
    if (eq(name, "isInitParam") || eq(name, "isRVForPaidUser") || eq(name, "isUserConsentGDPR")) {
      return 1;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// JNI Interface Functions
// ---------------------------------------------------------------------------

static void *j_GetVersion(void *env) { (void)env; return (void *)JNI_VERSION_1_6; }

static void *j_FindClass(void *env, const char *name) {
  (void)env;
  debugPrintf("JNI: FindClass: %s\n", name);
  return get_class(name);
}

static FakeID *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  FakeObject *c = cls;
  debugPrintf("JNI: GetMethodID: %s::%s (%s)\n", c ? c->label : "NULL", name, sig);
  return get_id(c ? c->label : NULL, name, sig);
}

static uint16_t *jni_u8_to_u16(const char *s, size_t *out_units) {
  const size_t n = strlen(s);
  uint16_t *out = malloc((n + 1) * sizeof(uint16_t)); // units <= bytes
  if (!out) { if (out_units) *out_units = 0; return NULL; }
  size_t u = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 0xF) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }
    if (c < 0x10000) out[u++] = (uint16_t)c;
    else { c -= 0x10000; out[u++] = (uint16_t)(0xD800 + (c >> 10)); out[u++] = (uint16_t)(0xDC00 + (c & 0x3FF)); }
  }
  out[u] = 0;
  if (out_units) *out_units = u;
  return out;
}

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }

static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *chars) {
  (void)env; (void)jstr; (void)chars;
}
static juint j_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  return strlen(obj_str(jstr));
}
static void *j_NewString(void *env, const uint16_t *chars, int len) {
  (void)env;
  char *utf8 = malloc((size_t)len * 4 + 4);
  int o = 0;
  for (int i = 0; i < len; i++) {
    uint32_t c = chars[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len) { uint32_t lo = chars[++i]; c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); }
    if (c < 0x80) utf8[o++] = (char)c;
    else if (c < 0x800) { utf8[o++] = 0xC0 | (c >> 6); utf8[o++] = 0x80 | (c & 0x3F); }
    else if (c < 0x10000) { utf8[o++] = 0xE0 | (c >> 12); utf8[o++] = 0x80 | ((c >> 6) & 0x3F); utf8[o++] = 0x80 | (c & 0x3F); }
    else { utf8[o++] = 0xF0 | (c >> 18); utf8[o++] = 0x80 | ((c >> 12) & 0x3F); utf8[o++] = 0x80 | ((c >> 6) & 0x3F); utf8[o++] = 0x80 | (c & 0x3F); }
  }
  utf8[o] = 0;
  void *r = jni_make_string(utf8);
  free(utf8);
  return r;
}
static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  const char *s = obj_str(jstr);
  int sl = (int)strlen(s);
  if (start < 0 || start > sl) return;
  if (start + len > sl) len = sl - start;
  memcpy(buf, s + start, len > 0 ? len : 0);
}
static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  size_t u = 0;
  uint16_t *t = jni_u8_to_u16(obj_str(jstr), &u);
  if (!t) return;
  if (start >= 0 && len > 0 && (size_t)(start + len) <= u)
    memcpy(buf, t + start, (size_t)len * sizeof(uint16_t));
  free(t);
}

static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  size_t u = 0;
  uint16_t *t = jni_u8_to_u16(obj_str(jstr), &u);
  if (t) free(t);
  return (juint)u;
}

static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 1;
  size_t u = 0;
  return jni_u8_to_u16(obj_str(jstr), &u);
}

static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr;
  free((void *)chars);
}

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR)) return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len > 0 ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len > 0 ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init;
  return reg_local(a);
}
static void *j_GetObjectArrayElement(void *env, void *arr, int idx) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len) return a->items[idx];
  return NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int idx, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len) a->items[idx] = val;
}

// --- objects & methods ------------------------------------------------------

static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  FakeObject *o = obj;
  if (o && (o->tag == TAG_OBJECT || o->tag == TAG_CLASS)) {
    return get_class(o->label);
  }
  return get_class("object");
}

static void *j_CallStaticObjectMethod(void *env, void *cls, FakeID *id, ...) {
  (void)env; (void)cls;
  va_list va;
  va_start(va, id);
  void *res = call_object(id->cls, id->name, va);
  va_end(va);
  return res;
}
static void *j_CallStaticObjectMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return call_object(id->cls, id->name, va);
}
static void *j_CallObjectMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env; (void)obj;
  va_list va;
  va_start(va, id);
  void *res = call_object(id->cls, id->name, va);
  va_end(va);
  return res;
}
static void *j_CallObjectMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return call_object(id->cls, id->name, va);
}

static void j_CallStaticVoidMethod(void *env, void *cls, FakeID *id, ...) {
  (void)env; (void)cls;
  va_list va;
  va_start(va, id);
  call_void(id->cls, id->name, va);
  va_end(va);
}
static void j_CallStaticVoidMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  call_void(id->cls, id->name, va);
}
static void j_CallVoidMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env; (void)obj;
  va_list va;
  va_start(va, id);
  call_void(id->cls, id->name, va);
  va_end(va);
}
static void j_CallVoidMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  call_void(id->cls, id->name, va);
}

static jint j_CallStaticIntMethod(void *env, void *cls, FakeID *id, ...) {
  (void)env; (void)cls;
  va_list va;
  va_start(va, id);
  jint res = call_int(id->cls, id->name, va);
  va_end(va);
  return res;
}
static jint j_CallStaticIntMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return call_int(id->cls, id->name, va);
}
static jint j_CallIntMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env; (void)obj;
  va_list va;
  va_start(va, id);
  jint res = call_int(id->cls, id->name, va);
  va_end(va);
  return res;
}
static jint j_CallIntMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return call_int(id->cls, id->name, va);
}

static jboolean j_CallStaticBooleanMethod(void *env, void *cls, FakeID *id, ...) {
  (void)env; (void)cls;
  va_list va;
  va_start(va, id);
  jboolean res = call_bool(id->cls, id->name, va);
  va_end(va);
  return res;
}
static jboolean j_CallStaticBooleanMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return call_bool(id->cls, id->name, va);
}
static jboolean j_CallBooleanMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env; (void)obj;
  va_list va;
  va_start(va, id);
  jboolean res = call_bool(id->cls, id->name, va);
  va_end(va);
  return res;
}
static jboolean j_CallBooleanMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return call_bool(id->cls, id->name, va);
}

static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  FakeObject *c = cls;
  return get_id(c ? c->label : NULL, name, sig);
}

static void *j_GetObjectField(void *env, void *obj, FakeID *id) {
  (void)env;
  FakeObject *o = obj;
  if (o && !strcmp(o->label, "APKFile")) {
    FakeAPKFile *file = obj;
    if (!strcmp(id->name, "data")) {
      return file->data_arr;
    }
  }
  return NULL;
}

static jint j_GetIntField(void *env, void *obj, FakeID *id) {
  (void)env;
  FakeObject *o = obj;
  if (o && !strcmp(o->label, "APKFile")) {
    FakeAPKFile *file = obj;
    if (!strcmp(id->name, "length")) {
      return file->length;
    }
    if (!strcmp(id->name, "position")) {
      return file->position;
    }
    if (!strcmp(id->name, "bufferSize")) {
      return 4096;
    }
  }
  return 0;
}

static void *j_GetStaticObjectField(void *env, void *cls, FakeID *id) {
  (void)env; (void)cls;
  if (eq(id->name, "instance") || eq(id->name, "sFoxJniLib")) {
    return get_class(id->cls);
  }
  return NULL;
}

// --- refs & frames ----------------------------------------------------------

static void *j_NewGlobalRef(void *env, void *ref) {
  (void)env;
  return ref;
}
static void j_DeleteGlobalRef(void *env, void *ref) { (void)env; (void)ref; }

static void j_DeleteLocalRef(void *env, void *ref) {
  (void)env;
  delete_local(ref);
}

static jint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES) frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return JNI_OK;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  if (frame_top > 0) {
    int prev_top = frames[--frame_top];
    for (int i = locals_top - 1; i >= prev_top; i--) {
      if (locals[i] != result) {
        free_ref(locals[i]);
      }
    }
    locals_top = prev_top;
    if (result) {
      locals[locals_top++] = result;
    }
  }
  mutexUnlock(&locals_lock);
  return result;
}
static void *j_NewLocalRef(void *env, void *ref) { (void)env; return reg_local(ref); }
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return JNI_OK; }
static jboolean j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }
static jboolean j_IsInstanceOf(void *env, void *obj, void *cls) { (void)env; (void)obj; (void)cls; return 1; }

// --- misc -------------------------------------------------------------------

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods; (void)n; return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void *j_NewObject(void *env, void *cls, FakeID *id, ...) { (void)env; (void)id; FakeObject *c = cls; return jni_make_object(c ? c->label : "obj"); }
static void j_void1(void *env) { (void)env; }
static juint j_unimplemented(void) {
  debugPrintf("JNI: unimplemented slot called\n");
  return 0;
}

// ---------------------------------------------------------------------------
// table assembly
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);

  for (int i = 0; i < 233; i++) env_table[i] = (void *)j_unimplemented;

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[13]  = (void *)j_void1; // Throw
  env_table[14]  = (void *)j_void1; // ThrowNew
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[18]  = (void *)j_void1; // FatalError
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[27]  = (void *)jni_make_object;  // AllocObject
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObject;      // NewObjectV
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[40]  = (void *)j_CallIntMethod;  // CallByteMethod
  env_table[41]  = (void *)j_CallIntMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[100] = (void *)j_GetIntField;
  
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[120] = (void *)j_CallStaticIntMethod;    // CallStaticByteMethod
  env_table[121] = (void *)j_CallStaticIntMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetFieldID;            // GetStaticFieldID
  env_table[145] = (void *)j_GetStaticObjectField;
  
  env_table[163] = (void *)j_NewString;
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion;
  env_table[224] = (void *)j_GetStringChars;
  env_table[225] = (void *)j_ReleaseStringChars;
  env_table[222] = (void *)j_GetPriArrayElements;
  env_table[223] = (void *)j_ReleasePriArrayElements;
  env_table[226] = (void *)j_NewGlobalRef;
  env_table[227] = (void *)j_DeleteGlobalRef;
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread;
}

void jni_set_writable_path(const char *p) {
  if (p && p[0]) { strncpy(g_writable, p, sizeof(g_writable) - 1); g_writable[sizeof(g_writable)-1] = 0; }
}
