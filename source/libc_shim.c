/* libc_shim.c -- bionic-compatible libc wrappers for libchrono.so + libc++_shared.so
 *
 * Converting wrappers for the cases where bionic and newlib differ in struct
 * layout, flag values or available functions; matching calls pass through from
 * imports.c. Networking is stubbed (no network on the Switch port).
 *
 * Modified and distributed under the terms of the MIT license; see LICENSE.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "libc_shim.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memcpy(dst, src, n); }
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memmove(dst, src, n); }
void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) { (void)dstlen; return memset(dst, c, n); }
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcat(dst, src); }
char *__strchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strchr(s, c); }
char *__strrchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strrchr(s, c); }
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcpy(dst, src); }
size_t __strlen_chk_fake(const char *s, size_t slen) { (void)slen; return strlen(s); }
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncat(dst, src, n); }
char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncpy(dst, src, n); }
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) { (void)dstlen; (void)srclen; return strncpy(dst, src, n); }

int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va);
}
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsprintf(s, fmt, va);
}
int __snprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen; va_list va; va_start(va, fmt); int r = vsnprintf(s, maxlen, fmt, va); va_end(va); return r;
}
int __sprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen; va_list va; va_start(va, fmt); int r = vsprintf(s, fmt, va); va_end(va); return r;
}
ssize_t __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) { (void)buflen; return read_fake(fd, buf, count); }
int __open_2_fake(const char *path, int flags) { return open_fake(path, flags); }
void __FD_SET_chk_fake(int fd, void *set) { FD_SET(fd, (fd_set *)set); }
void __FD_CLR_chk_fake(int fd, void *set) { FD_CLR(fd, (fd_set *)set); }
int __FD_ISSET_chk_fake(int fd, const void *set) { return FD_ISSET(fd, (fd_set *)set); }

// ---------------------------------------------------------------------------
// android logging
// ---------------------------------------------------------------------------

int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  if (fmt && strstr(fmt, "nnGLCheckError")) {
    return 0;
  }
#if DEBUG_LOG
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  if (strstr(string, "nnGLCheckError")) {
    return 0;
  }
  debugPrintf("%s: %s\n", tag ? tag : "", string);
#else
  (void)tag; (void)fmt;
#endif
  return 0;
}

int __android_log_write_fake(int prio, const char *tag, const char *text) {
  (void)prio;
  if (text && strstr(text, "nnGLCheckError")) {
    return 0;
  }
#if DEBUG_LOG
  debugPrintf("%s: %s\n", tag ? tag : "", text ? text : "");
#else
  (void)tag; (void)text;
#endif
  return 0;
}

void __android_log_assert_fake(const char *cond, const char *tag, const char *fmt, ...) {
  (void)tag;
#if DEBUG_LOG
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("assert(%s): %s\n", cond ? cond : "", string);
#else
  (void)cond; (void)fmt;
#endif
  abort();
}

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

int __system_property_get_fake(const char *name, char *value) {
  // Answer locale/language props from config so the engine doesn't fall back to
  // its Japanese default (some code reads these instead of JNI getCurrentLanguage).
  value[0] = '\0';
  if (name) {
    debugPrintf("sysprop: get(%s)\n", name);
    if (strstr(name, "locale") || strstr(name, "language") || strstr(name, "country")) {
      const char *lang = config.language[0] ? config.language : LANG_DEFAULT;
      if (strstr(name, "language"))      snprintf(value, 92, "%s", lang);
      else if (strstr(name, "country"))  snprintf(value, 92, "US");
      else                                snprintf(value, 92, "%s-US", lang); // full locale
      return (int)strlen(value);
    }
  }
  return 0;
}

unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

int gettid_fake(void) {
  u64 thread_id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE)) && thread_id)
    return (int)(thread_id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178
long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID: return gettid_fake();
  }
  debugPrintf("libc: syscall(%ld) -> ENOSYS\n", number);
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
void sincos_fake(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("abort message: %s\n", msg ? msg : "(null)");
}

size_t __ctype_get_mb_cur_max_fake(void) { return 1; }
int __register_atfork_fake(void) { return 0; }

int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) {
  (void)fn; (void)arg; (void)dso; // threads leak rather than run dtors
  return 0;
}

int __cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso) {
  (void)fn; (void)arg; (void)dso; // never cleanly exit; registering is a no-op
  return 0;
}
int __cxa_finalize_fake(void *dso) { (void)dso; return 0; }

// bionic sysconf constants
#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE:
      return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN:
      return 3; // games may size thread pools off this; Switch app cores = 3
    case BIONIC_SC_PHYS_PAGES:
      return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default:
      debugPrintf("libc: sysconf(%d) -> -1\n", name);
      return -1;
  }
}

long pathconf_fake(const char *path, int name) { (void)path; (void)name; return -1; }

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT    0100
#define LINUX_O_EXCL     0200
#define LINUX_O_TRUNC    01000
#define LINUX_O_APPEND   02000
#define LINUX_O_NONBLOCK 04000

static int convert_open_flags(int flags) {
  int out = flags & 3; // O_RDONLY/O_WRONLY/O_RDWR match
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

#define FAKE_FD_URANDOM 9999

int open_fake(const char *path, int flags, ...) {
  if (path && (strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0)) {
    return FAKE_FD_URANDOM;
  }
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va);
  }
  int fd = open(path, convert_open_flags(flags), mode);
  debugPrintf("open: %s (flags: %d) -> %d\n", path ? path : "(null)", flags, fd);
  return fd;
}

int openat_fake(int dirfd, const char *path, int flags, ...) {
  if (path && (strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0)) {
    return FAKE_FD_URANDOM;
  }
  (void)dirfd; // assume AT_FDCWD or absolute paths
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va);
  }
  int fd = open(path, convert_open_flags(flags), mode);
  debugPrintf("openat: %s (flags: %d) -> %d\n", path ? path : "(null)", flags, fd);
  return fd;
}

int unlinkat_fake(int dirfd, const char *path, int flags) { (void)dirfd; (void)flags; return unlink(path); }

ssize_t read_fake(int fd, void *buf, size_t count) {
  if (fd == FAKE_FD_URANDOM) {
    csrngGetRandomBytes(buf, count);
    return count;
  }
  return read(fd, buf, count);
}

int close_fake(int fd) {
  if (fd == FAKE_FD_URANDOM) {
    return 0;
  }
  return close(fd);
}

// Large stream buffer on archive reads to cut per-read fsdev round trips.
FILE *fopen_fake(const char *path, const char *mode) {
  FILE *f = fopen(path, mode);
  debugPrintf("fopen: %s (%s) -> %s\n", path ? path : "(null)", mode ? mode : "(null)", f ? "OK" : "FAIL");
  if (!f) {
    return NULL;
  }
  if (strchr(mode, 'r')) {
    const char *ext = strrchr(path, '.');
    if (ext && (strcasecmp(ext, ".dat") == 0 || strcasecmp(ext, ".bin") == 0))
      setvbuf(f, NULL, _IOFBF, 256 * 1024);
  }
  return f;
}

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };

struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t  st_size;
  int32_t  st_blksize;
  int32_t  __pad2;
  int64_t  st_blocks;
  struct bionic_timespec st_atim;
  struct bionic_timespec st_mtim;
  struct bionic_timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev;
  out->st_ino = in->st_ino;
  out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid;
  out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev;
  out->st_size = in->st_size;
  out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  struct stat real;
  const int ret = stat(path, &real);
  debugPrintf("stat: %s -> %d\n", path ? path : "(null)", ret);
  if (ret == 0) convert_stat(&real, st);
  return ret;
}
int fstat_fake(int fd, struct bionic_stat *st) {
  struct stat real;
  const int ret = fstat(fd, &real);
  if (ret == 0) convert_stat(&real, st);
  return ret;
}
int lstat_fake(const char *path, struct bionic_stat *st) { return stat_fake(path, st); }
int fstatat_fake(int dirfd, const char *path, struct bionic_stat *st, int flags) {
  (void)dirfd; (void)flags; return stat_fake(path, st);
}

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino;
  int64_t  d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[256];
};

void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // NOTE: not thread-safe (matches bionic readdir)
  struct dirent *e = readdir((DIR *)dirp);
  if (!e) return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

// No symlinks in the game tree, so just copy; bionic guarantees `resolved` is
// at least PATH_MAX (4096).
#define BIONIC_PATH_MAX 4096
char *realpath_fake(const char *path, char *resolved) {
  if (!resolved) resolved = malloc(BIONIC_PATH_MAX);
  if (!resolved) return NULL;
  size_t n = strnlen(path, BIONIC_PATH_MAX - 1);
  memcpy(resolved, path, n);
  resolved[n] = 0;
  return resolved;
}

int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

// bionic statvfs (aarch64). Report a large filesystem so the engine's free-space
// checks pass; an all-zero statvfs reads as a full disk and blocks saves.
struct bionic_statvfs {
  unsigned long f_bsize, f_frsize;
  uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail;
  unsigned long f_fsid, f_flag, f_namemax;
  uint32_t __reserved[6];
};
int statvfs_fake(const char *path, void *buf) {
  (void)path;
  struct bionic_statvfs *s = buf;
  memset(s, 0, sizeof(*s));
  s->f_bsize = 0x1000;
  s->f_frsize = 0x1000;
  s->f_blocks = 256ull * 1024 * 1024;  // ~1 TiB of 4 KiB blocks
  s->f_bfree  = 256ull * 1024 * 1024;
  s->f_bavail = 256ull * 1024 * 1024;
  s->f_files  = 1ull << 20;
  s->f_ffree  = 1ull << 20;
  s->f_favail = 1ull << 20;
  s->f_namemax = 255;
  return 0;
}
int fchmod_fake(int fd, unsigned mode) { (void)fd; (void)mode; return 0; }
int fchmodat_fake(int dirfd, const char *path, unsigned mode, int flags) { (void)dirfd; (void)path; (void)mode; (void)flags; return 0; }
int utimensat_fake(int dirfd, const char *path, const void *times, int flags) { (void)dirfd; (void)path; (void)times; (void)flags; return 0; }

long sendfile_fake(int out_fd, int in_fd, long *offset, size_t count) {
  // not provided by newlib; emulate with a copy loop
  char buf[8192];
  if (offset) lseek(in_fd, *offset, SEEK_SET);
  size_t done = 0;
  while (done < count) {
    size_t want = count - done > sizeof(buf) ? sizeof(buf) : count - done;
    ssize_t r = read(in_fd, buf, want);
    if (r <= 0) break;
    ssize_t w = write(out_fd, buf, (size_t)r);
    if (w != r) break;
    done += (size_t)r;
  }
  if (offset) *offset = lseek(in_fd, 0, SEEK_CUR);
  return (long)done;
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C locale versions
// ---------------------------------------------------------------------------

void *newlocale_fake(int mask, const char *locale, void *base) { (void)mask; (void)locale; (void)base; return (void *)1; }
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }
char *setlocale_fake(int category, const char *locale) { (void)category; (void)locale; return (char *)"C"; }

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha) WRAP_ISW_L(iswblank) WRAP_ISW_L(iswcntrl) WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower) WRAP_ISW_L(iswprint) WRAP_ISW_L(iswpunct) WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper) WRAP_ISW_L(iswxdigit) WRAP_ISW_L(towlower) WRAP_ISW_L(towupper)
int isalpha_l_fake(int c, void *loc) { (void)loc; return isalpha(c); }
int isdigit_l_fake(int c, void *loc) { (void)loc; return isdigit(c); }

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) { (void)loc; return strftime(s, max, fmt, (const struct tm *)tm); }
double strtod_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtod(s, end); }
float strtof_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtof(s, end); }
long double strtold_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
long strtol_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtol(s, end, base); }
unsigned long strtoul_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoul(s, end, base); }
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  size_t i = 0;
  const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) { if (dst) dst[i] = (unsigned char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0;
  const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) { if (dst) dst[i] = (char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr)
// libc++_shared binds std::cout/cerr to &__sF[1]/&__sF[2]; these wrappers
// absorb accesses to the fake FILEs and forward everything else to newlib.
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c too

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    const size_t total = size * n < sizeof(buf) - 1 ? size * n : sizeof(buf) - 1;
    memcpy(buf, ptr, total); buf[total] = '\0';
    debugPrintf("stdio: %s", buf);
#endif
    return n;
  }
  return fwrite(ptr, size, n, f);
}
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) { if (is_fake_file(f)) return 0; return fread(ptr, size, n, f); }
int fputc_fake(int c, FILE *f) { if (is_fake_file(f)) return c; return fputc(c, f); }
int fputs_fake(const char *s, FILE *f) { if (is_fake_file(f)) { debugPrintf("stdio: %s", s); return 0; } return fputs(s, f); }
int fflush_fake(FILE *f) { if (is_fake_file(f) || f == NULL) return 0; return fflush(f); }
int fclose_fake(FILE *f) { if (is_fake_file(f)) return 0; return fclose(f); }
int ferror_fake(FILE *f) { if (is_fake_file(f)) return 0; return ferror(f); }
int fileno_fake(FILE *f) { if (is_fake_file(f)) return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100; return fileno(f); }

int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list va; va_start(va, fmt); int ret;
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
#else
    ret = 0;
#endif
  } else ret = vfprintf(f, fmt, va);
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    int ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
    return ret;
#else
    return 0;
#endif
  }
  return vfprintf(f, fmt, va);
}
int fseek_fake(FILE *f, long off, int whence) { if (is_fake_file(f)) return -1; return fseek(f, off, whence); }
int getc_fake(FILE *f) { if (is_fake_file(f)) return -1; return getc(f); }
int ungetc_fake(int c, FILE *f) { if (is_fake_file(f)) return -1; return ungetc(c, f); }
void setbuf_fake(FILE *f, char *buf) { if (is_fake_file(f)) return; setbuf(f, buf); }

// ---------------------------------------------------------------------------
// pthread rwlocks/semaphores: the game allocates the bionic struct; we stash a
// pointer to the real object in its first bytes (like the mutex fakes in imports.c).
// ---------------------------------------------------------------------------

typedef struct { RwLock lock; } FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) {
    FakeRwLock *l = calloc(1, sizeof(*l));
    rwlockInit(&l->lock);
    *storage = l;
  }
  return *storage;
}

int pthread_rwlock_init_fake(void **rw, const void *attr) { (void)attr; *rw = NULL; get_rwlock(rw); return 0; }
int pthread_rwlock_destroy_fake(void **rw) { if (rw && *rw && (uintptr_t)*rw > 0x8000) { free(*rw); *rw = NULL; } return 0; }
int pthread_rwlock_rdlock_fake(void **rw) { rwlockReadLock(&get_rwlock(rw)->lock); return 0; }
int pthread_rwlock_wrlock_fake(void **rw) { rwlockWriteLock(&get_rwlock(rw)->lock); return 0; }
int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock)) rwlockWriteUnlock(&l->lock);
  else rwlockReadUnlock(&l->lock);
  return 0;
}

typedef struct { Semaphore sem; } FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *fs = calloc(1, sizeof(*fs));
  semaphoreInit(&fs->sem, value);
  *s = fs;
  return 0;
}
int sem_destroy_fake(void **s) { if (s && *s) { free(*s); *s = NULL; } return 0; }
int sem_post_fake(void **s) { if (s && *s) semaphoreSignal(&((FakeSem *)*s)->sem); return 0; }
int sem_wait_fake(void **s) { if (s && *s) semaphoreWait(&((FakeSem *)*s)->sem); return 0; }
int sem_trywait_fake(void **s) { if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem)) return 0; errno = EAGAIN; return -1; }
int sem_getvalue_fake(void **s, int *val) { if (s && *s) *val = (int)((FakeSem *)*s)->sem.count; else *val = 0; return 0; }

int pthread_attr_getstacksize_fake(const void *attr, size_t *size) { (void)attr; *size = 1024 * 1024; return 0; }
int pthread_attr_setstacksize_fake(void *attr, size_t size) { (void)attr; (void)size; return 0; }
int pthread_attr_getschedparam_fake(const void *attr, void *param) { (void)attr; memset(param, 0, 8); return 0; }
int pthread_setname_np_fake(void *thread, const char *name) { (void)thread; (void)name; return 0; }
int pthread_setschedparam_fake(void *thread, int policy, const void *param) { (void)thread; (void)policy; (void)param; return 0; }
int pthread_condattr_setclock_fake(void *attr, int clk) { (void)attr; (void)clk; return 0; }

// ---------------------------------------------------------------------------
// networking (Cocos2dxDownloader): stubbed -- every call fails cleanly so
// downloads never complete.
// ---------------------------------------------------------------------------

int socket_fake(int domain, int type, int protocol) { (void)domain; (void)type; (void)protocol; errno = EAFNOSUPPORT; return -1; }
int connect_fake(int fd, const void *addr, unsigned len) { (void)fd; (void)addr; (void)len; errno = ENETUNREACH; return -1; }
int bind_fake(int fd, const void *addr, unsigned len) { (void)fd; (void)addr; (void)len; return -1; }
int listen_fake(int fd, int backlog) { (void)fd; (void)backlog; return -1; }
int accept_fake(int fd, void *addr, unsigned *len) { (void)fd; (void)addr; (void)len; return -1; }
long sendto_fake(int fd, const void *buf, size_t n, int flags, const void *addr, unsigned alen) { (void)fd; (void)buf; (void)n; (void)flags; (void)addr; (void)alen; return -1; }
long recvfrom_fake(int fd, void *buf, size_t n, int flags, void *addr, unsigned *alen) { (void)fd; (void)buf; (void)n; (void)flags; (void)addr; (void)alen; return -1; }
int select_fake(int nfds, void *r, void *w, void *e, void *timeout) { (void)nfds; (void)r; (void)w; (void)e; (void)timeout; return 0; }
int setsockopt_fake(int fd, int level, int opt, const void *val, unsigned len) { (void)fd; (void)level; (void)opt; (void)val; (void)len; return -1; }
int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res) { (void)node; (void)service; (void)hints; if (res) *res = NULL; return -2; /* EAI_NONAME */ }
void freeaddrinfo_fake(void *res) { (void)res; }
const char *gai_strerror_fake(int err) { (void)err; return "name resolution disabled"; }
const char *inet_ntop_fake(int af, const void *src, char *dst, unsigned size) { (void)af; (void)src; if (dst && size) dst[0] = 0; return dst; }
int inet_pton_fake(int af, const char *src, void *dst) { (void)af; (void)src; (void)dst; return 0; }
int ioctl_fake(int fd, unsigned long req, ...) { (void)fd; (void)req; return -1; }
int system_fake(const char *cmd) { (void)cmd; return -1; }

// extra shims for Sonic 4 Ep 2
int __android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list ap) {
  (void)prio;
  if (fmt && strstr(fmt, "nnGLCheckError")) {
    return 0;
  }
#if DEBUG_LOG
  static char string[0x1000];
  vsnprintf(string, sizeof(string), fmt, ap);
  if (strstr(string, "nnGLCheckError")) {
    return 0;
  }
  debugPrintf("%s: %s\n", tag ? tag : "", string);
#else
  (void)tag; (void)fmt; (void)ap;
#endif
  return 0;
}

int __libc_current_sigrtmin_fake(void) {
  return 32;
}

void *gethostbyname_fake(const char *name) {
  (void)name;
  return NULL;
}

int getnameinfo_fake(const void *sa, unsigned salen, char *host, unsigned hostlen, char *serv, unsigned servlen, int flags) {
  (void)sa; (void)salen; (void)host; (void)hostlen; (void)serv; (void)servlen; (void)flags;
  return -1;
}

int getpeername_fake(int fd, void *addr, unsigned *len) {
  (void)fd; (void)addr; (void)len;
  errno = ENOTCONN;
  return -1;
}

int getsockname_fake(int fd, void *addr, unsigned *len) {
  (void)fd; (void)addr; (void)len;
  errno = ENOTCONN;
  return -1;
}

int getsockopt_fake(int fd, int level, int optname, void *optval, unsigned *optlen) {
  (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
  errno = ENOPROTOOPT;
  return -1;
}

int recv_fake(int sockfd, void *buf, size_t len, int flags) {
  (void)sockfd; (void)buf; (void)len; (void)flags;
  errno = ENOTCONN;
  return -1;
}

int send_fake(int sockfd, const void *buf, size_t len, int flags) {
  (void)sockfd; (void)buf; (void)len; (void)flags;
  errno = ENOTCONN;
  return -1;
}

int shutdown_fake(int sockfd, int how) {
  (void)sockfd; (void)how;
  errno = ENOTCONN;
  return -1;
}

void *getpwuid_fake(unsigned uid) {
  (void)uid;
  return NULL;
}

unsigned getuid_fake(void) {
  return 0;
}

int setgid_fake(unsigned gid) {
  (void)gid;
  return 0;
}

int setuid_fake(unsigned uid) {
  (void)uid;
  return 0;
}

int initgroups_fake(const char *user, unsigned group) {
  (void)user; (void)group;
  return 0;
}

int getpagesize_fake(void) {
  return 4096;
}

int getrlimit_fake(int resource, struct bionic_rlimit *rlim) {
  (void)resource;
  if (rlim) {
    rlim->rlim_cur = 1024;
    rlim->rlim_max = 1024;
  }
  return 0;
}

int kill_fake(int pid, int sig) {
  (void)pid; (void)sig;
  errno = EPERM;
  return -1;
}

int pipe_fake(int pipefd[2]) {
  (void)pipefd;
  errno = EMFILE;
  return -1;
}

int poll_fake(void *fds, unsigned long nfds, int timeout) {
  (void)fds; (void)nfds; (void)timeout;
  errno = ENOSYS;
  return -1;
}

int sigaction_fake(int signum, const void *act, void *oldact) {
  (void)signum; (void)act; (void)oldact;
  return 0;
}

int sigaddset_fake(void *set, int signum) {
  (void)set; (void)signum;
  return 0;
}

int sigdelset_fake(void *set, int signum) {
  (void)set; (void)signum;
  return 0;
}

int sigemptyset_fake(void *set) {
  (void)set;
  return 0;
}

int sigfillset_fake(void *set) {
  (void)set;
  return 0;
}

int sigprocmask_fake(int how, const void *set, void *oldset) {
  (void)how; (void)set; (void)oldset;
  return 0;
}

void (*signal_fake(int signum, void (*handler)(int)))(int) {
  (void)signum; (void)handler;
  return NULL;
}

int sigsetjmp_fake(jmp_buf env, int savemask) {
  (void)savemask;
  return setjmp(env);
}

void siglongjmp_fake(jmp_buf env, int val) {
  longjmp(env, val);
}

int madvise_fake(void *addr, size_t length, int advice) {
  (void)addr; (void)length; (void)advice;
  return 0;
}

int mlock_fake(const void *addr, size_t len) {
  (void)addr; (void)len;
  return 0;
}

int mprotect_fake(void *addr, size_t len, int prot) {
  (void)addr; (void)len; (void)prot;
  return 0;
}

int pthread_attr_setschedpolicy_fake(void *attr, int policy) {
  (void)attr; (void)policy;
  return 0;
}

int pthread_attr_setstack_fake(void *attr, void *stackaddr, size_t stacksize) {
  (void)attr; (void)stackaddr; (void)stacksize;
  return 0;
}

int pthread_getschedparam_fake(pthread_t thread, int *policy, struct sched_param *param) {
  (void)thread;
  if (policy) *policy = 0;
  if (param) memset(param, 0, 8);
  return 0;
}

int sem_timedwait_fake(void **s, const struct timespec *abs_timeout) {
  (void)abs_timeout;
  return sem_wait_fake(s);
}

int tcgetattr_fake(int fd, void *termios_p) {
  (void)fd; (void)termios_p;
  errno = ENOTTY;
  return -1;
}

int tcsetattr_fake(int fd, int optional_actions, const void *termios_p) {
  (void)fd; (void)optional_actions; (void)termios_p;
  errno = ENOTTY;
  return -1;
}

int timer_create_fake(int clockid, void *sevp, int *timerid) {
  (void)clockid; (void)sevp;
  if (timerid) *timerid = 1;
  return 0;
}

int timer_delete_fake(int timerid) {
  (void)timerid;
  return 0;
}

int timer_settime_fake(int timerid, int flags, const void *new_value, void *old_value) {
  (void)timerid; (void)flags; (void)new_value; (void)old_value;
  return 0;
}

int uname_fake(struct bionic_utsname *buf) {
  if (buf) {
    snprintf(buf->sysname, sizeof(buf->sysname), "Linux");
    snprintf(buf->nodename, sizeof(buf->nodename), "localhost");
    snprintf(buf->release, sizeof(buf->release), "4.9.0-nintendo-switch");
    snprintf(buf->version, sizeof(buf->version), "#1 SMP PREEMPT");
    snprintf(buf->machine, sizeof(buf->machine), "aarch64");
    snprintf(buf->domainname, sizeof(buf->domainname), "localdomain");
  }
  return 0;
}

