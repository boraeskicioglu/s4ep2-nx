/* imports.c -- .so import resolution for libchrono.so / libc++_shared.so
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * The C++ ABI (std::/__cxa_/RTTI) is deliberately absent here -- it resolves
 * against libc++_shared.so. This table provides the libc subset (shimmed where
 * bionic/newlib differ), GLES2 (mesa), eglGetProcAddress, OpenSL ES and the
 * AAsset NDK API.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <wctype.h>
#include <fcntl.h>
#include <math.h>
#include <malloc.h>
#include <locale.h>
#include <setjmp.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>
#include <dirent.h>
#include <sys/time.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "opensles.h"
#include "asset.h"
#include "imports.h"

// ---------------------------------------------------------------------------
// stack protector: libchrono imports __stack_chk_guard as a data symbol AND the
// NDK compiler also reads a canary from TLS (tpidr_el0 + 0x28), so we satisfy
// both (the symbol here, the TLS slot via tls_setup_guard()).
// ---------------------------------------------------------------------------

uint64_t __stack_chk_guard_fake = 0x0123456789ABCDEFull;

static void __stack_chk_fail_fake(void) {
  debugPrintf("*** stack smashing detected ***\n");
  abort();
}

FILE *stderr_fake = (FILE *)0x1337;

// ---------------------------------------------------------------------------
// pthread wrappers: bionic allocates the opaque types inline and zero-inits
// them, so we lazily back them with heap-allocated newlib objects stashed
// through the caller's pointer slot.
// ---------------------------------------------------------------------------

static int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (mutexattr && *mutexattr == 1);
  int ret;
  if (recursive) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

static int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

// bionic statically initialises PTHREAD_MUTEX_INITIALIZER to all-zero, and
// PTHREAD_RECURSIVE_MUTEX_INITIALIZER to a small magic constant; lazily create
// the real object on first use.
static int ensure_mutex(pthread_mutex_t **uid) {
  if (!*uid) return pthread_mutex_init_fake(uid, NULL);
  if ((uintptr_t)*uid < 0x8000) { int attr = 1; return pthread_mutex_init_fake(uid, &attr); }
  return 0;
}

static int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_lock(*uid);
}
static int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_trylock(*uid);
}
static int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_unlock(*uid);
}

static int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}
static int ensure_cond(pthread_cond_t **cnd) { return *cnd ? 0 : pthread_cond_init_fake(cnd, NULL); }
static int pthread_cond_broadcast_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1; return pthread_cond_broadcast(*cnd); }
static int pthread_cond_signal_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1; return pthread_cond_signal(*cnd); }
static int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) { pthread_cond_destroy(*cnd); free(*cnd); *cnd = NULL; }
  return 0;
}
static int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (ensure_cond(cnd) < 0) return -1;
  if (ensure_mutex(mtx) < 0) return -1;
  return pthread_cond_wait(*cnd, *mtx);
}
static int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (ensure_cond(cnd) < 0) return -1;
  if (ensure_mutex(mtx) < 0) return -1;
  return pthread_cond_timedwait(*cnd, *mtx, t);
}

// bionic pthread_once_t is a zero-initialised int. A correct pthread_once must
// run the init exactly once AND block every other caller until it has finished,
// or singletons race (a second caller could use a half-constructed object).
// State machine: 0 = not started, 1 = in progress, 2 = done.
static int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == 2)
    return 0;
  if (__sync_bool_compare_and_swap(once_control, 0, 1)) {
    (*init_routine)();
    __atomic_store_n(once_control, 2, __ATOMIC_RELEASE);
  } else {
    while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) != 2)
      svcSleepThread(1000); // 1us; brief wait for the initialising thread
  }
  return 0;
}

static int pthread_mutexattr_init_fake(int *attr) { if (attr) *attr = 0; return 0; }
static int pthread_mutexattr_settype_fake(int *attr, int type) { if (attr) *attr = type; return 0; }

// each engine thread gets tpidr_el0 pointed at a stack-guard block before it
// runs any guarded engine code (see tls_setup_guard)
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();
  return ts.entry(ts.arg);
}

static int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
  (void)unused; // ignore the (incompatible) bionic attr
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  return pthread_create(thread, NULL, thread_trampoline, ts);
}

// devkitA64's pthread_detach returns ENOSYS; std::thread::detach turns that into
// an uncaught std::system_error -> terminate (the sead sound engine detaches a
// worker on battle teardown). Detach best-effort and always report success.
static int pthread_detach_fake(pthread_t thread) {
  pthread_detach(thread);
  return 0;
}

static int pthread_attr_setschedparam_fake(void *a, const void *p) { (void)a; (void)p; return 0; }

// ---------------------------------------------------------------------------
// small misc shims
// ---------------------------------------------------------------------------

static int getpid_fake(void) { return 1; }
static int sched_yield_fake(void) { svcSleepThread(0); return 0; }

// bionic and newlib number the POSIX clocks differently (bionic CLOCK_REALTIME=0,
// newlib=1), so libchrono's clock_gettime(CLOCK_REALTIME) hits an unsupported
// newlib id -> EINVAL -> std::chrono throws. Remap the realtime ids and fall back
// so a failure is never returned to the engine.
static int clock_gettime_fake(int clk, struct timespec *tp) {
  if (!tp) return 0;
  clockid_t want = (clk == 0 || clk == 5) ? CLOCK_REALTIME : (clockid_t)clk;
  if (clock_gettime(want, tp) == 0) return 0;
  if (clock_gettime(CLOCK_REALTIME, tp) == 0) return 0;
  if (clock_gettime(CLOCK_MONOTONIC, tp) == 0) return 0;
  const u64 f = armGetSystemTickFreq() ? armGetSystemTickFreq() : 19200000ull;
  const u64 t = armGetSystemTick();
  tp->tv_sec = (time_t)(t / f);
  tp->tv_nsec = (long)(((t % f) * 1000000000ull) / f);
  return 0;
}

// dlsym: cocos probes GL/EGL extensions through it. Resolve via eglGetProcAddress
// first, then fall back to our own import table.
DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name);
static void *dlsym_fake(void *handle, const char *name) {
  (void)handle;
  void *p = (void *)eglGetProcAddress(name);
  if (p) return p;
  DynLibFunction *f = so_find_import(dynlib_functions, (int)dynlib_numfunctions, name);
  return f ? (void *)f->func : NULL;
}
static void *dlopen_fake(const char *name, int flag) { (void)name; (void)flag; return (void *)1; }
static int dlclose_fake(void *h) { (void)h; return 0; }
static const char *dlerror_fake(void) { return NULL; }

// mmap/munmap are not implemented by newlib/libnx; report failure so callers
// fall back to read()/fread() (cocos FileUtils does).
static void *mmap_fake(void *addr, size_t len, int prot, int flags, int fd, long off) {
  (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
  return (void *)-1; // MAP_FAILED
}
static int munmap_fake(void *addr, size_t len) { (void)addr; (void)len; return 0; }

// fdopendir is absent from devkitA64 newlib; libc++ only needs it for
// std::filesystem directory iteration, which the game does not use.
static void *fdopendir_fake(int fd) { (void)fd; return NULL; }

// GL_OES_mapbuffer is an extension: resolve lazily through eglGetProcAddress so
// the table entry is valid even though mesa exports it only as a proc address.
static void *gl_MapBufferOES(GLenum target, GLenum access) {
  static PFNGLMAPBUFFEROESPROC fn = NULL;
  if (!fn) fn = (PFNGLMAPBUFFEROESPROC)eglGetProcAddress("glMapBufferOES");
  return fn ? fn(target, access) : NULL;
}
static GLboolean gl_UnmapBufferOES(GLenum target) {
  static PFNGLUNMAPBUFFEROESPROC fn = NULL;
  if (!fn) fn = (PFNGLUNMAPBUFFEROESPROC)eglGetProcAddress("glUnmapBufferOES");
  return fn ? fn(target) : GL_FALSE;
}

// cocos RenderTexture (FieldMap::RewriteBg) can request a 0-sized FBO texture
// before map data is ready; the mesa/nouveau driver then NULL-derefs in
// st_update_renderbuffer_surface. Clamp degenerate allocations to 1x1.
static void glTexImage2D_guard(GLenum target, GLint level, GLint internalformat,
    GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
    const void *pixels) {
  if (width <= 0 || height <= 0) {
    width = width <= 0 ? 1 : width;
    height = height <= 0 ? 1 : height;
    pixels = NULL; // the supplied buffer no longer matches the clamped size
  }
  glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

// GLES2 hooks bypassed for performance. Real entry points mapped directly in table.

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  // --- bionic runtime bits we must own (NOT the C++ ABI: that is in
  //     libc++_shared.so and resolves there) ---
  { "__sF", (uintptr_t)&fake_sF },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit_fake },
  { "__cxa_finalize", (uintptr_t)&__cxa_finalize_fake },
  { "__errno", (uintptr_t)&__errno },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_fake },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake },
  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },
  { "abort", (uintptr_t)&abort },
  { "exit", (uintptr_t)&exit },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },

  // android logging
  { "__android_log_print", (uintptr_t)&__android_log_print_fake },
  { "__android_log_write", (uintptr_t)&__android_log_write_fake },
  { "__android_log_assert", (uintptr_t)&__android_log_assert_fake },

  // fortify (_chk) wrappers
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__snprintf_chk", (uintptr_t)&__snprintf_chk_fake },
  { "__sprintf_chk", (uintptr_t)&__sprintf_chk_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },
  { "__open_2", (uintptr_t)&__open_2_fake },
  { "__FD_SET_chk", (uintptr_t)&__FD_SET_chk_fake },
  { "__FD_CLR_chk", (uintptr_t)&__FD_CLR_chk_fake },
  { "__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk_fake },

  // AAsset NDK API (served from the loose assets, see asset.c)
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek", (uintptr_t)&AAsset_seek_fake },
  { "AAsset_seek64", (uintptr_t)&AAsset_seek64_fake },
  { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength_fake },
  { "AAsset_getRemainingLength64", (uintptr_t)&AAsset_getRemainingLength64_fake },
  { "AAsset_getBuffer", (uintptr_t)&AAsset_getBuffer_fake },
  { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor_fake },
  { "AAsset_isAllocated", (uintptr_t)&AAsset_isAllocated_fake },
  { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName_fake },
  { "AAssetDir_rewind", (uintptr_t)&AAssetDir_rewind_fake },
  { "AAssetDir_close", (uintptr_t)&AAssetDir_close_fake },

  // memory
  { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "free", (uintptr_t)&free },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "memalign", (uintptr_t)&memalign },
  { "malloc_usable_size", (uintptr_t)&malloc_usable_size },

  // mem/str
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "memrchr", (uintptr_t)&memrchr },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strcpy", (uintptr_t)&strcpy },
  { "strdup", (uintptr_t)&strdup },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strncat", (uintptr_t)&strncat },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strspn", (uintptr_t)&strspn },
  { "strcspn", (uintptr_t)&strcspn },
  { "strtok", (uintptr_t)&strtok },
  { "strtok_r", (uintptr_t)&strtok_r },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strcoll", (uintptr_t)&strcoll },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "stpcpy", (uintptr_t)&stpcpy },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "atoi", (uintptr_t)&atoi },
  { "atol", (uintptr_t)&atol },
  { "atoll", (uintptr_t)&atoll },
  { "atof", (uintptr_t)&atof },
  { "toupper", (uintptr_t)&toupper },
  { "tolower", (uintptr_t)&tolower },
  { "bsearch", (uintptr_t)&bsearch },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "srand", (uintptr_t)&srand },
  { "getenv", (uintptr_t)&getenv },
  { "setenv", (uintptr_t)&setenv },
  { "system", (uintptr_t)&system_fake },

  // wide char + locale
  { "wcslen", (uintptr_t)&wcslen },
  { "wcscmp", (uintptr_t)&wcscmp },
  { "wcsncmp", (uintptr_t)&wcsncmp },
  { "wcscpy", (uintptr_t)&wcscpy },
  { "wcsncpy", (uintptr_t)&wcsncpy },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
  { "fputwc", (uintptr_t)&fputwc },
  { "getwc", (uintptr_t)&getwc },
  { "ungetwc", (uintptr_t)&ungetwc },
  { "fgetwc", (uintptr_t)&fgetwc },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wctob", (uintptr_t)&wctob },
  { "btowc", (uintptr_t)&btowc },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "setlocale", (uintptr_t)&setlocale_fake },
  { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake },
  { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake },
  { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake },
  { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake },
  { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake },
  { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake },
  { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "strftime_l", (uintptr_t)&strftime_l_fake },
  { "strtod_l", (uintptr_t)&strtod_l_fake },
  { "strtof_l", (uintptr_t)&strtof_l_fake },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtol_l", (uintptr_t)&strtol_l_fake },
  { "strtoul_l", (uintptr_t)&strtoul_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },

  // printf family
  { "printf", (uintptr_t)&debugPrintf },
  { "putchar", (uintptr_t)&putchar },
  { "puts", (uintptr_t)&puts },
  { "perror", (uintptr_t)&ret0 },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "swprintf", (uintptr_t)&swprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "asprintf", (uintptr_t)&asprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "vsscanf", (uintptr_t)&vsscanf },

  // math
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "asin", (uintptr_t)&asin },
  { "asinf", (uintptr_t)&asinf },
  { "atan", (uintptr_t)&atan },
  { "atanf", (uintptr_t)&atanf },
  { "atan2", (uintptr_t)&atan2 },
  { "atan2f", (uintptr_t)&atan2f },
  { "ceil", (uintptr_t)&ceil },
  { "ceilf", (uintptr_t)&ceilf },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "cosh", (uintptr_t)&cosh },
  { "exp", (uintptr_t)&exp },
  { "exp2", (uintptr_t)&exp2 },
  { "exp2f", (uintptr_t)&exp2f },
  { "expf", (uintptr_t)&expf },
  { "floor", (uintptr_t)&floor },
  { "floorf", (uintptr_t)&floorf },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "frexp", (uintptr_t)&frexp },
  { "frexpf", (uintptr_t)&frexpf },
  { "ldexp", (uintptr_t)&ldexp },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "modf", (uintptr_t)&modf },
  { "modff", (uintptr_t)&modff },
  { "pow", (uintptr_t)&pow },
  { "powf", (uintptr_t)&powf },
  { "round", (uintptr_t)&round },
  { "roundf", (uintptr_t)&roundf },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "sinh", (uintptr_t)&sinh },
  { "sincos", (uintptr_t)&sincos_fake },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "sqrt", (uintptr_t)&sqrt },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "tan", (uintptr_t)&tan },
  { "tanf", (uintptr_t)&tanf },
  { "tanh", (uintptr_t)&tanh },
  { "trunc", (uintptr_t)&trunc },

  // time
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "clock", (uintptr_t)&clock },
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "localtime", (uintptr_t)&localtime },
  { "localtime_r", (uintptr_t)&localtime_r },
  { "mktime", (uintptr_t)&mktime },
  { "strftime", (uintptr_t)&strftime },
  { "time", (uintptr_t)&time },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "usleep", (uintptr_t)&usleep },
  { "sleep", (uintptr_t)&sleep },

  // stdio (over the fake bionic __sF and buffered fopen)
  { "fopen", (uintptr_t)&fopen_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "freopen", (uintptr_t)&freopen },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fgets", (uintptr_t)&fgets },
  { "fgetc", (uintptr_t)&fgetc },
  { "getc", (uintptr_t)&getc_fake },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko },
  { "ftell", (uintptr_t)&ftell },
  { "ftello", (uintptr_t)&ftello },
  { "rewind", (uintptr_t)&rewind },
  { "fflush", (uintptr_t)&fflush_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "feof", (uintptr_t)&feof },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "putc", (uintptr_t)&fputc_fake },
  { "setbuf", (uintptr_t)&setbuf_fake },
  { "setvbuf", (uintptr_t)&setvbuf },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename },
  { "tmpfile", (uintptr_t)&tmpfile },

  // posix file
  { "open", (uintptr_t)&open_fake },
  { "openat", (uintptr_t)&openat_fake },
  { "__openat", (uintptr_t)&openat_fake },
  { "close", (uintptr_t)&close_fake },
  { "read", (uintptr_t)&read_fake },
  { "write", (uintptr_t)&write },
  { "lseek", (uintptr_t)&lseek },
  { "lseek64", (uintptr_t)&lseek },
  { "ftruncate", (uintptr_t)&ftruncate },
  { "truncate", (uintptr_t)&truncate },
  { "unlink", (uintptr_t)&unlink },
  { "unlinkat", (uintptr_t)&unlinkat_fake },
  { "access", (uintptr_t)&access },
  { "mkdir", (uintptr_t)&mkdir },
  { "rmdir", (uintptr_t)&rmdir },
  { "chdir", (uintptr_t)&chdir },
  { "getcwd", (uintptr_t)&getcwd },
  { "isatty", (uintptr_t)&isatty },
  { "link", (uintptr_t)&link },
  { "symlink", (uintptr_t)&symlink },
  { "readlink", (uintptr_t)&readlink },
  { "realpath", (uintptr_t)&realpath_fake },
  { "opendir", (uintptr_t)&opendir },
  { "fdopendir", (uintptr_t)&fdopendir_fake },
  { "closedir", (uintptr_t)&closedir },
  { "readdir", (uintptr_t)&readdir_fake },
  { "rewinddir", (uintptr_t)&rewinddir },
  { "stat", (uintptr_t)&stat_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "lstat", (uintptr_t)&lstat_fake },
  { "fstatat", (uintptr_t)&fstatat_fake },
  { "statvfs", (uintptr_t)&statvfs_fake },
  { "fchmod", (uintptr_t)&fchmod_fake },
  { "fchmodat", (uintptr_t)&fchmodat_fake },
  { "chmod", (uintptr_t)&ret0 },
  { "utimensat", (uintptr_t)&utimensat_fake },
  { "sendfile", (uintptr_t)&sendfile_fake },
  { "pathconf", (uintptr_t)&pathconf_fake },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },

  // pthread
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_detach", (uintptr_t)&pthread_detach_fake },
  { "pthread_self", (uintptr_t)&pthread_self },
  { "pthread_exit", (uintptr_t)&pthread_exit },
  { "pthread_equal", (uintptr_t)&pthread_equal },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_condattr_init", (uintptr_t)&ret0 },
  { "pthread_condattr_destroy", (uintptr_t)&ret0 },
  { "pthread_condattr_setclock", (uintptr_t)&pthread_condattr_setclock_fake },
  { "pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake },
  { "pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },
  { "pthread_attr_init", (uintptr_t)&ret0 },
  { "pthread_attr_destroy", (uintptr_t)&ret0 },
  { "pthread_attr_setdetachstate", (uintptr_t)&ret0 },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_fake },
  { "pthread_attr_getstacksize", (uintptr_t)&pthread_attr_getstacksize_fake },
  { "pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam_fake },
  { "pthread_attr_getschedparam", (uintptr_t)&pthread_attr_getschedparam_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_setname_np", (uintptr_t)&pthread_setname_np_fake },

  // semaphores
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },

  // misc extras
  { "getpid", (uintptr_t)&getpid_fake },
  { "gettid", (uintptr_t)&gettid_fake },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "longjmp", (uintptr_t)&longjmp },
  { "setjmp", (uintptr_t)&setjmp },

  // networking (Cocos2dxDownloader) -- stubbed
  { "socket", (uintptr_t)&socket_fake },
  { "connect", (uintptr_t)&connect_fake },
  { "bind", (uintptr_t)&bind_fake },
  { "listen", (uintptr_t)&listen_fake },
  { "accept", (uintptr_t)&accept_fake },
  { "sendto", (uintptr_t)&sendto_fake },
  { "recvfrom", (uintptr_t)&recvfrom_fake },
  { "select", (uintptr_t)&select_fake },
  { "setsockopt", (uintptr_t)&setsockopt_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "gai_strerror", (uintptr_t)&gai_strerror_fake },
  { "inet_ntop", (uintptr_t)&inet_ntop_fake },
  { "inet_pton", (uintptr_t)&inet_pton_fake },
  { "ioctl", (uintptr_t)&ioctl_fake },

  // EGL (only eglGetProcAddress is used by libchrono; resolve to real EGL)
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },

  // GLES2 fixed entry points (mesa libGLESv2)
  { "glActiveTexture", (uintptr_t)&glActiveTexture },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
  { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendColor", (uintptr_t)&glBlendColor },
  { "glBlendEquation", (uintptr_t)&glBlendEquation },
  { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glBufferSubData", (uintptr_t)&glBufferSubData },
  { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glClearStencil", (uintptr_t)&glClearStencil },
  { "glColorMask", (uintptr_t)&glColorMask },
  { "glCompileShader", (uintptr_t)&glCompileShader },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCompressedTexSubImage2D", (uintptr_t)&glCompressedTexSubImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram },
  { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDepthRangef", (uintptr_t)&glDepthRangef },
  { "glDetachShader", (uintptr_t)&glDetachShader },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFinish", (uintptr_t)&glFinish },
  { "glFlush", (uintptr_t)&glFlush },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glFrontFace", (uintptr_t)&glFrontFace },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGenerateMipmap", (uintptr_t)&glGenerateMipmap },
  { "glGetActiveAttrib", (uintptr_t)&glGetActiveAttrib },
  { "glGetActiveUniform", (uintptr_t)&glGetActiveUniform },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetFloatv", (uintptr_t)&glGetFloatv },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderSource", (uintptr_t)&glGetShaderSource },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glHint", (uintptr_t)&glHint },
  { "glIsBuffer", (uintptr_t)&glIsBuffer },
  { "glIsEnabled", (uintptr_t)&glIsEnabled },
  { "glIsRenderbuffer", (uintptr_t)&glIsRenderbuffer },
  { "glLineWidth", (uintptr_t)&glLineWidth },
  { "glLinkProgram", (uintptr_t)&glLinkProgram },
  { "glMapBufferOES", (uintptr_t)&gl_MapBufferOES },
  { "glUnmapBufferOES", (uintptr_t)&gl_UnmapBufferOES },
  { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glPolygonOffset", (uintptr_t)&glPolygonOffset },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
  { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&glShaderSource },
  { "glStencilFunc", (uintptr_t)&glStencilFunc },
  { "glStencilFuncSeparate", (uintptr_t)&glStencilFuncSeparate },
  { "glStencilMask", (uintptr_t)&glStencilMask },
  { "glStencilOp", (uintptr_t)&glStencilOp },
  { "glStencilOpSeparate", (uintptr_t)&glStencilOpSeparate },
  { "glTexImage2D", (uintptr_t)&glTexImage2D_guard },
  { "glTexParameterf", (uintptr_t)&glTexParameterf },
  { "glTexParameterfv", (uintptr_t)&glTexParameterfv },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
  { "glUniform1f", (uintptr_t)&glUniform1f },
  { "glUniform1fv", (uintptr_t)&glUniform1fv },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform1iv", (uintptr_t)&glUniform1iv },
  { "glUniform2f", (uintptr_t)&glUniform2f },
  { "glUniform2fv", (uintptr_t)&glUniform2fv },
  { "glUniform2i", (uintptr_t)&glUniform2i },
  { "glUniform2iv", (uintptr_t)&glUniform2iv },
  { "glUniform3f", (uintptr_t)&glUniform3f },
  { "glUniform3fv", (uintptr_t)&glUniform3fv },
  { "glUniform3i", (uintptr_t)&glUniform3i },
  { "glUniform3iv", (uintptr_t)&glUniform3iv },
  { "glUniform4f", (uintptr_t)&glUniform4f },
  { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniform4i", (uintptr_t)&glUniform4i },
  { "glUniform4iv", (uintptr_t)&glUniform4iv },
  { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv },
  { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
  { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glValidateProgram", (uintptr_t)&glValidateProgram },
  { "glVertexAttrib1f", (uintptr_t)&glVertexAttrib1f },
  { "glVertexAttrib2fv", (uintptr_t)&glVertexAttrib2fv },
  { "glVertexAttrib3fv", (uintptr_t)&glVertexAttrib3fv },
  { "glVertexAttrib4fv", (uintptr_t)&glVertexAttrib4fv },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
  { "glViewport", (uintptr_t)&glViewport },
  // VAO functions (glGenVertexArraysOES etc.) are fetched by the engine through
  // eglGetProcAddress, so they are not listed here.

  // OpenSL ES (SDL2-backed shim, see opensles.c)
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
  { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
  { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
  { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
  { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
  { "SL_IID_OUTPUTMIX", (uintptr_t)&SL_IID_OUTPUTMIX },
  { "SL_IID_OBJECT", (uintptr_t)&SL_IID_OBJECT },
  { "SL_IID_NULL", (uintptr_t)&SL_IID_NULL },

  // extra shims for Sonic 4 Ep 2
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint_fake },
  { "__libc_current_sigrtmin", (uintptr_t)&__libc_current_sigrtmin_fake },
  { "_exit", (uintptr_t)&_exit },
  { "fcntl", (uintptr_t)&fcntl },
  { "funopen", (uintptr_t)&funopen },
  { "gethostbyname", (uintptr_t)&gethostbyname_fake },
  { "getnameinfo", (uintptr_t)&getnameinfo_fake },
  { "getpagesize", (uintptr_t)&getpagesize_fake },
  { "getpeername", (uintptr_t)&getpeername_fake },
  { "getpwuid", (uintptr_t)&getpwuid_fake },
  { "getrlimit", (uintptr_t)&getrlimit_fake },
  { "getsockname", (uintptr_t)&getsockname_fake },
  { "getsockopt", (uintptr_t)&getsockopt_fake },
  { "getuid", (uintptr_t)&getuid_fake },
  { "glGetVertexAttribiv", (uintptr_t)&glGetVertexAttribiv },
  { "initgroups", (uintptr_t)&initgroups_fake },
  { "isalnum", (uintptr_t)&isalnum },
  { "isalpha", (uintptr_t)&isalpha },
  { "islower", (uintptr_t)&islower },
  { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper },
  { "iswalpha", (uintptr_t)&iswalpha },
  { "iswblank", (uintptr_t)&iswblank },
  { "iswcntrl", (uintptr_t)&iswcntrl },
  { "iswdigit", (uintptr_t)&iswdigit },
  { "iswlower", (uintptr_t)&iswlower },
  { "iswprint", (uintptr_t)&iswprint },
  { "iswpunct", (uintptr_t)&iswpunct },
  { "iswspace", (uintptr_t)&iswspace },
  { "iswupper", (uintptr_t)&iswupper },
  { "iswxdigit", (uintptr_t)&iswxdigit },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "kill", (uintptr_t)&kill_fake },
  { "ldiv", (uintptr_t)&ldiv },
  { "madvise", (uintptr_t)&madvise_fake },
  { "mlock", (uintptr_t)&mlock_fake },
  { "mprotect", (uintptr_t)&mprotect_fake },
  { "pipe", (uintptr_t)&pipe_fake },
  { "poll", (uintptr_t)&poll_fake },
  { "pthread_attr_setschedpolicy", (uintptr_t)&pthread_attr_setschedpolicy_fake },
  { "pthread_attr_setstack", (uintptr_t)&pthread_attr_setstack_fake },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_fake },
  { "recv", (uintptr_t)&recv_fake },
  { "sem_timedwait", (uintptr_t)&sem_timedwait_fake },
  { "send", (uintptr_t)&send_fake },
  { "setgid", (uintptr_t)&setgid_fake },
  { "setuid", (uintptr_t)&setuid_fake },
  { "shutdown", (uintptr_t)&shutdown_fake },
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "sigaddset", (uintptr_t)&sigaddset_fake },
  { "sigdelset", (uintptr_t)&sigdelset_fake },
  { "sigemptyset", (uintptr_t)&sigemptyset_fake },
  { "sigfillset", (uintptr_t)&sigfillset_fake },
  { "siglongjmp", (uintptr_t)&siglongjmp_fake },
  { "signal", (uintptr_t)&signal_fake },
  { "sigprocmask", (uintptr_t)&sigprocmask_fake },
  { "sigsetjmp", (uintptr_t)&sigsetjmp_fake },
  { "tcgetattr", (uintptr_t)&tcgetattr_fake },
  { "tcsetattr", (uintptr_t)&tcsetattr_fake },
  { "timer_create", (uintptr_t)&timer_create_fake },
  { "timer_delete", (uintptr_t)&timer_delete_fake },
  { "timer_settime", (uintptr_t)&timer_settime_fake },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "uname", (uintptr_t)&uname_fake },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void fox_resolve_imports(so_module *mod) {
  so_relocate(mod);
  so_resolve(mod, dynlib_functions, (int)dynlib_numfunctions, 1);
}

