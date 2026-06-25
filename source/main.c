/* main.c -- Sonic the Hedgehog 4: Episode II Switch wrapper entry point
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <EGL/egl.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "asset.h"
#include "audio.h"

pthread_t g_main_thread;

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module game_mod;  // libfox.so

void fox_resolve_imports(so_module *mod);

// provide a replacement heap init so the newlib heap is separate from the .so
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_reserve = (size_t)SO_HEAP_RESERVE_MB * 1024 * 1024;
  if (so_reserve > size / 2)
    so_reserve = size / 2;
  fake_heap_size = size - so_reserve;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)    fatal_error("Could not find\n%s.\nCheck your data files.", SO_NAME);
  // data.obb path check
  if (stat("assets/data.obb", &st) < 0) {
    if (stat("sdmc:/switch/s4ep2/assets/data.obb", &st) < 0) {
      fatal_error("Could not find assets/data.obb.\nCheck your data files.");
    }
  }
}


static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w;
    screen_height = h;
  }
}

// ---------------------------------------------------------------------------
// EGL / GLES2 context on the default NWindow
// ---------------------------------------------------------------------------

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) { debugPrintf("egl: no display\n"); return 0; }
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { debugPrintf("egl: bindAPI failed\n"); return 0; }

  const EGLint cfg_attr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLConfig config;
  EGLint num = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &config, 1, &num) || num < 1) {
    debugPrintf("egl: no config\n");
    return 0;
  }

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
  if (!s_surface) { debugPrintf("egl: no surface\n"); return 0; }

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) { debugPrintf("egl: no context\n"); return 0; }

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, 1);
  return 1;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}

// ---------------------------------------------------------------------------
// libfox.so entry points
// ---------------------------------------------------------------------------

static void (*e_JNI_OnLoad)(void *vm, void *reserved);
static void (*e_nativeSetContext)(void *env, void *clazz, void *context);
static void (*e_nativeSetApkPath)(void *env, void *clazz, void *path);
static void (*e_init)(void *env, void *clazz, int w, int h);
static void (*e_DrawEGLCreated)(void *env, void *clazz);
static void (*e_DrawFrame)(void *env, void *clazz, int arg);
static void (*e_GameProcess)(void *env, void *clazz);
static void (*e_SetGamePath)(void *env, void *clazz, int id, void *path, int arg3);
static void (*e_SetLanguageId)(void *env, void *clazz, int lang);
static void (*e_SetPadData)(void *env, void *clazz, int buttons);
static void (*e_SetTPData)(void *env, void *clazz, int id, int act, int x, int y);
static void (*e_pauseEvent)(void *env, void *clazz);
static void (*e_resumeEvent)(void *env, void *clazz);
void (*e_activeGame)(void *env, void *clazz, unsigned char active, int reason) = NULL;
static void (*e_FileProcess)(void *env, void *clazz);
void (*e_SetContinueFlag)(int flag) = NULL;

#define RX(sym) so_try_find_addr_rx(&game_mod, sym)

static void resolve_entry_points(void) {
  e_JNI_OnLoad        = (void *)RX("JNI_OnLoad");
  e_nativeSetContext  = (void *)RX("Java_com_sega_f2fextension_F2FAndroidJNI_nativeSetContext");
  e_nativeSetApkPath  = (void *)RX("Java_com_sega_f2fextension_F2FAndroidJNI_nativeSetApkPath");
  e_init              = (void *)RX("Java_com_mineloader_fox_foxJniLib_init");
  e_DrawEGLCreated    = (void *)RX("Java_com_mineloader_fox_foxJniLib_DrawEGLCreated");
  e_DrawFrame         = (void *)RX("Java_com_mineloader_fox_foxJniLib_DrawFrame");
  e_GameProcess       = (void *)RX("Java_com_mineloader_fox_foxJniLib_GameProcess");
  e_SetGamePath       = (void *)RX("Java_com_mineloader_fox_foxJniLib_SetGamePath");
  e_SetLanguageId     = (void *)RX("Java_com_mineloader_fox_foxJniLib_SetLanguageId");
  e_SetPadData        = (void *)RX("Java_com_mineloader_fox_foxJniLib_SetPadData");
  e_SetTPData         = (void *)RX("Java_com_mineloader_fox_foxJniLib_SetTPData");
  e_pauseEvent        = (void *)RX("Java_com_mineloader_fox_foxJniLib_pauseEvent");
  e_resumeEvent       = (void *)RX("Java_com_mineloader_fox_foxJniLib_resumeEvent");
  e_activeGame        = (void *)RX("Java_com_sega_f2fextension_F2FAndroidJNI_activeGame");
  e_FileProcess       = (void *)RX("Java_com_mineloader_fox_foxJniLib_FileProcess");
  e_SetContinueFlag   = (void *)RX("Java_com_mineloader_fox_foxJniLib_SetContinueFlag");
}

static void *thiz = NULL; // AppActivity fake instance

// ---------------------------------------------------------------------------
// input -- Switch HID -> SetPadData bitmask
// ---------------------------------------------------------------------------

static PadState pad;

static int update_keys(void) {
  padUpdate(&pad);
  u64 buttons = padGetButtons(&pad);
  
  int gmPadValue = 0;
  if (buttons & (HidNpadButton_Up | HidNpadButton_StickLUp))       gmPadValue |= (1 << 0);
  if (buttons & (HidNpadButton_Down | HidNpadButton_StickLDown))   gmPadValue |= (1 << 1);
  if (buttons & (HidNpadButton_Left | HidNpadButton_StickLLeft))   gmPadValue |= (1 << 2);
  if (buttons & (HidNpadButton_Right | HidNpadButton_StickLRight))  gmPadValue |= (1 << 3);
  
  if (buttons & HidNpadButton_Y)     gmPadValue |= (1 << 4);
  if (buttons & HidNpadButton_B)     gmPadValue |= (1 << 5); // Confirms / B maps to A in engine
  if (buttons & HidNpadButton_X)     gmPadValue |= (1 << 6);
  if (buttons & HidNpadButton_A)     gmPadValue |= (1 << 7); // Cancels / A maps to B in engine
  
  if (buttons & HidNpadButton_L)     gmPadValue |= (1 << 8);
  if (buttons & HidNpadButton_ZL)    gmPadValue |= (1 << 9);
  if (buttons & HidNpadButton_ZR)    gmPadValue |= (1 << 10);
  if (buttons & HidNpadButton_R)     gmPadValue |= (1 << 11);
  
  if (buttons & HidNpadButton_Minus) gmPadValue |= (1 << 14); // SELECT / BACK
  if (buttons & HidNpadButton_Plus)  gmPadValue |= (1 << 15); // START / ENTER
  
  return gmPadValue;
}

// ---------------------------------------------------------------------------
// touch -- single pointer mapped into the engine
// ---------------------------------------------------------------------------

static int touch_down = 0;
static float last_tx = 0, last_ty = 0;

static void update_touch(void) {
  HidTouchScreenState st = {0};
  int have = hidGetTouchScreenStates(&st, 1) && st.count > 0;

  if (have) {
    const float sx = (float)screen_width / 1280.0f;
    const float sy = (float)screen_height / 720.0f;
    float x = st.touches[0].x * sx;
    float y = st.touches[0].y * sy;
    if (!touch_down) {
      touch_down = 1;
      if (e_SetTPData) e_SetTPData(fake_env, thiz, 0, 0, (int)x, (int)y);
    } else {
      if (e_SetTPData) e_SetTPData(fake_env, thiz, 0, 2, (int)x, (int)y);
    }
    last_tx = x; last_ty = y;
  } else if (touch_down) {
    touch_down = 0;
    if (e_SetTPData) e_SetTPData(fake_env, thiz, 0, 1, (int)last_tx, (int)last_ty);
  }
}

volatile int should_resume_video = 0;
volatile int should_set_continue_flag = 0;

static void *game_update_loop(void *arg) {
  (void)arg;
  tls_setup_guard();
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 1ULL << 1);
  
  int process_count = 0;
  while (!jni_quit_requested) {
    if (should_resume_video && e_activeGame) {
      debugPrintf("JNI: Resuming engine from game thread with activeGame(true, 128)...\n");
      should_resume_video = 0;
      e_activeGame(fake_env, NULL, 1, 128); // 128 = REASON_PAUSE::PAUSE_INTRO_VIDEO
    }
    if (should_set_continue_flag && e_SetContinueFlag) {
      debugPrintf("JNI: Auto-responding with SetContinueFlag(1) from game thread...\n");
      should_set_continue_flag = 0;
      e_SetContinueFlag(1);
    }
    if (e_GameProcess) {
      e_GameProcess(fake_env, thiz);
      process_count++;
      if (process_count % 1000 == 0) {
        debugPrintf("LOOP: e_GameProcess called %d times\n", process_count);
      }
    }
    svcSleepThread(1000000ull); // 1ms
  }
  return NULL;
}

static void *file_process_loop(void *arg) {
  (void)arg;
  tls_setup_guard();
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1ULL << 2);
  if (e_FileProcess) {
    debugPrintf("LOOP: e_FileProcess thread started\n");
    e_FileProcess(fake_env, thiz);
    debugPrintf("LOOP: e_FileProcess thread exited\n");
  }
  return NULL;
}

// ---------------------------------------------------------------------------

static void OPENSSL_cpuid_setup_hook(void) {
  // Do nothing, no-op
}

int main(int argc, char *argv[]) {
  g_main_thread = pthread_self();
  debugPrintf("Main thread ID: %lu\n", (unsigned long)g_main_thread);

  // change directory to the NRO's folder to ensure relative paths resolve correctly
  if (argc > 0 && argv[0]) {
    char nro_path[512];
    strncpy(nro_path, argv[0], sizeof(nro_path) - 1);
    nro_path[sizeof(nro_path) - 1] = 0;
    char *slash = strrchr(nro_path, '/');
    if (slash) {
      *slash = 0;
      chdir(nro_path);
    }
  } else {
    chdir("sdmc:/switch/s4ep2");
  }

  cpu_boost(1);

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();
  set_screen_size(config.screen_width, config.screen_height);

  plInitialize(PlServiceType_User);
  
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s\n", SDL_GetError());

  audio_init();

  if (!egl_init())
    fatal_error("Failed to create an OpenGL ES 2.0 context.");

  // Load libfox.so
  if (so_load(&game_mod, SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  fox_resolve_imports(&game_mod);

  resolve_entry_points();
  if (!e_init || !e_DrawEGLCreated || !e_DrawFrame || !e_JNI_OnLoad)
    fatal_error("Could not resolve libfox.so engine entry points.");

  // Hook OPENSSL_cpuid_setup to prevent signal/siglongjmp crash during CPU capability probing
  uintptr_t cpuid_setup_addr = so_try_find_addr_rx(&game_mod, "OPENSSL_cpuid_setup");
  if (cpuid_setup_addr) {
    uintptr_t offset = cpuid_setup_addr - (uintptr_t)game_mod.load_virtbase;
    uintptr_t backing_addr = (uintptr_t)game_mod.load_base + offset;
    hook_arm64(backing_addr, (uintptr_t)&OPENSSL_cpuid_setup_hook);
  }

  // Set OPENSSL_armcap_P value to indicate basic NEON (which is safe/required on ARM64)
  uintptr_t armcap_addr = so_try_find_addr_rx(&game_mod, "OPENSSL_armcap_P");
  if (armcap_addr) {
    uintptr_t offset = armcap_addr - (uintptr_t)game_mod.load_virtbase;
    uint32_t *backing_armcap = (uint32_t *)((uintptr_t)game_mod.load_base + offset);
    *backing_armcap = 1; // Enable NEON (ARMV7_NEON bit)
  }

  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  tls_setup_guard();

  // Execute C++ static constructors
  so_execute_init_array(&game_mod);
  so_free_temp(&game_mod);

  // Initialize fake JNI environment
  jni_init();
  thiz = jni_make_object("FoxActivity_Core");

  // Get current working directory
  static char wdir[512];
  if (getcwd(wdir, sizeof(wdir)) && wdir[0]) {
    size_t n = strlen(wdir);
    if (n > 1 && wdir[n - 1] == '/') wdir[n - 1] = 0;
  } else {
    strcpy(wdir, "sdmc:/switch/s4ep2");
  }
  jni_set_writable_path(wdir);

  // Call JNI_OnLoad
  if (e_JNI_OnLoad)
    e_JNI_OnLoad(fake_vm, NULL);

  // Sega F2F Analytics JNI Bootstrap
  void *ctx = jni_make_object("Context");
  if (e_nativeSetContext) e_nativeSetContext(fake_env, thiz, ctx);
  
  void *apk_path = jni_make_string("data.obb");
  if (e_nativeSetApkPath) e_nativeSetApkPath(fake_env, thiz, apk_path);

  // Set game paths
  void *res_loc = jni_make_string(wdir);
  if (e_SetGamePath) {
    e_SetGamePath(fake_env, thiz, 0, res_loc, 0); // Resources folder
    
    // Save folder path
    char save_dir[600];
    snprintf(save_dir, sizeof(save_dir), "%s/save/", wdir);
    mkdir(save_dir, 0777);
    void *save_loc = jni_make_string(save_dir);
    e_SetGamePath(fake_env, thiz, 254, save_loc, 0);
    
    // OBB path
    char obb_path[1024];
    snprintf(obb_path, sizeof(obb_path), "%s/assets/data.obb", wdir);
    void *obb_loc = jni_make_string(obb_path);
    e_SetGamePath(fake_env, thiz, 255, obb_loc, 0);
  }

  // Set default language ID (1 = English)
  if (e_SetLanguageId) {
    e_SetLanguageId(fake_env, thiz, 1);
  }

  // Initialize engine graphics
  e_DrawEGLCreated(fake_env, thiz);
  e_init(fake_env, thiz, screen_width, screen_height);

  // Spawn game update loop on separate thread
  pthread_t game_thread;
  pthread_create(&game_thread, NULL, game_update_loop, NULL);

  // Spawn file processing loop on separate thread
  pthread_t file_thread;
  pthread_create(&file_thread, NULL, file_process_loop, NULL);

  // Initialize input
  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  int paused = 1;
  int boot_frames = 0;
  while (appletMainLoop() && !jni_quit_requested) {
    // Focus states
    const int focused = appletGetFocusState() == AppletFocusState_InFocus;
    if (!focused && !paused) {
      if (e_pauseEvent) e_pauseEvent(fake_env, thiz);
      paused = 1;
    } else if (focused && paused) {
      if (e_resumeEvent) e_resumeEvent(fake_env, thiz);
      paused = 0;
    }

    if (paused) {
      svcSleepThread(16000000ull); // ~16ms
      continue;
    }

    // Input
    int gmPadValue = update_keys();
    if (e_SetPadData) {
      e_SetPadData(fake_env, thiz, gmPadValue);
    }
    update_touch();

    // Render
    static int draw_count = 0;
    if (e_DrawFrame) {
      e_DrawFrame(fake_env, thiz, g_continue_flag);
      draw_count++;
      if (draw_count % 60 == 0) {
        debugPrintf("LOOP: e_DrawFrame called %d times\n", draw_count);
      }
    }
    if (!eglSwapBuffers(s_display, s_surface)) {
      EGLint err = eglGetError();
      debugPrintf("EGL: eglSwapBuffers failed: 0x%04X\n", err);
    }

    if (boot_frames < 10 && ++boot_frames == 10)
      cpu_boost(0);
  }

  // Clean up
  jni_quit_requested = 1;
  pthread_join(game_thread, NULL);
  pthread_join(file_thread, NULL);

  if (e_pauseEvent) e_pauseEvent(fake_env, thiz);
  audio_shutdown();
  egl_deinit();
  plExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
