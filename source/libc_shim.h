/* libc_shim.h -- bionic-compatible libc wrappers for libchrono.so / libc++_shared.so
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __LIBC_SHIM_H__
#define __LIBC_SHIM_H__

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>

// fortify (_chk) wrappers -- ignore the compiler-supplied object size
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen);
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen);
char *__strchr_chk_fake(const char *s, int c, size_t slen);
char *__strrchr_chk_fake(const char *s, int c, size_t slen);
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen);
size_t __strlen_chk_fake(const char *s, size_t slen);
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen);
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen);
char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen);
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen);
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va);
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va);
int __snprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...);
int __sprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, ...);
ssize_t __read_chk_fake(int fd, void *buf, size_t count, size_t buflen);
int __open_2_fake(const char *path, int flags);
void __FD_SET_chk_fake(int fd, void *set);
void __FD_CLR_chk_fake(int fd, void *set);
int __FD_ISSET_chk_fake(int fd, const void *set);

// misc bionic
int __system_property_get_fake(const char *name, char *value);
unsigned long getauxval_fake(unsigned long type);
int gettid_fake(void);
long syscall_fake(long number, ...);
void sincosf_fake(float x, float *s, float *c);
void sincos_fake(double x, double *s, double *c);
void android_set_abort_message_fake(const char *msg);
size_t __ctype_get_mb_cur_max_fake(void);
int __register_atfork_fake(void);
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso);
long sysconf_fake(int name);
long pathconf_fake(const char *path, int name);
int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...);
int __android_log_write_fake(int prio, const char *tag, const char *text);
void __android_log_assert_fake(const char *cond, const char *tag, const char *fmt, ...);
int __cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso);
int __cxa_finalize_fake(void *dso);

// fs
int open_fake(const char *path, int flags, ...);
int openat_fake(int dirfd, const char *path, int flags, ...);
int unlinkat_fake(int dirfd, const char *path, int flags);
ssize_t read_fake(int fd, void *buf, size_t count);
int close_fake(int fd);
FILE *fopen_fake(const char *path, const char *mode);
struct bionic_stat;
int stat_fake(const char *path, struct bionic_stat *st);
int fstat_fake(int fd, struct bionic_stat *st);
int lstat_fake(const char *path, struct bionic_stat *st);
int fstatat_fake(int dirfd, const char *path, struct bionic_stat *st, int flags);
void *readdir_fake(void *dirp);
char *realpath_fake(const char *path, char *resolved);
int strerror_r_fake(int err, char *buf, size_t len);
int statvfs_fake(const char *path, void *buf);
int fchmod_fake(int fd, unsigned mode);
int fchmodat_fake(int dirfd, const char *path, unsigned mode, int flags);
int utimensat_fake(int dirfd, const char *path, const void *times, int flags);
long sendfile_fake(int out_fd, int in_fd, long *offset, size_t count);

// locale
void *newlocale_fake(int mask, const char *locale, void *base);
void freelocale_fake(void *loc);
void *uselocale_fake(void *loc);
char *setlocale_fake(int category, const char *locale);
int iswalpha_l_fake(int wc, void *loc);
int iswblank_l_fake(int wc, void *loc);
int iswcntrl_l_fake(int wc, void *loc);
int iswdigit_l_fake(int wc, void *loc);
int iswlower_l_fake(int wc, void *loc);
int iswprint_l_fake(int wc, void *loc);
int iswpunct_l_fake(int wc, void *loc);
int iswspace_l_fake(int wc, void *loc);
int iswupper_l_fake(int wc, void *loc);
int iswxdigit_l_fake(int wc, void *loc);
int towlower_l_fake(int wc, void *loc);
int towupper_l_fake(int wc, void *loc);
int isalpha_l_fake(int c, void *loc);
int isdigit_l_fake(int c, void *loc);
int strcoll_l_fake(const char *a, const char *b, void *loc);
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc);
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc);
double strtod_l_fake(const char *s, char **end, void *loc);
float strtof_l_fake(const char *s, char **end, void *loc);
long double strtold_l_fake(const char *s, char **end, void *loc);
long strtol_l_fake(const char *s, char **end, int base, void *loc);
unsigned long strtoul_l_fake(const char *s, char **end, int base, void *loc);
long long strtoll_l_fake(const char *s, char **end, int base, void *loc);
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc);
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc);
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc);
size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps);
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps);

// memory
int posix_memalign_fake(void **out, size_t align, size_t size);

// stdio over fake __sF
extern uint8_t fake_sF[3][0x100];
size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f);
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f);
int fputc_fake(int c, FILE *f);
int fputs_fake(const char *s, FILE *f);
int fflush_fake(FILE *f);
int fclose_fake(FILE *f);
int ferror_fake(FILE *f);
int fileno_fake(FILE *f);
int fprintf_fake(FILE *f, const char *fmt, ...);
int vfprintf_fake(FILE *f, const char *fmt, va_list va);
int fseek_fake(FILE *f, long off, int whence);
int getc_fake(FILE *f);
int ungetc_fake(int c, FILE *f);
void setbuf_fake(FILE *f, char *buf);

// pthread extras (bionic opaque types -> heap-allocated libnx primitives)
int pthread_rwlock_rdlock_fake(void **rw);
int pthread_rwlock_wrlock_fake(void **rw);
int pthread_rwlock_unlock_fake(void **rw);
int pthread_rwlock_init_fake(void **rw, const void *attr);
int pthread_rwlock_destroy_fake(void **rw);
int sem_init_fake(void **s, int pshared, unsigned int value);
int sem_destroy_fake(void **s);
int sem_post_fake(void **s);
int sem_wait_fake(void **s);
int sem_trywait_fake(void **s);
int sem_getvalue_fake(void **s, int *val);
int pthread_attr_getstacksize_fake(const void *attr, size_t *size);
int pthread_attr_setstacksize_fake(void *attr, size_t size);
int pthread_attr_getschedparam_fake(const void *attr, void *param);
int pthread_setname_np_fake(void *thread, const char *name);
int pthread_setschedparam_fake(void *thread, int policy, const void *param);
int pthread_condattr_setclock_fake(void *attr, int clk);

// networking (Cocos2dxDownloader) -- stubbed; the port has no network access
int socket_fake(int domain, int type, int protocol);
int connect_fake(int fd, const void *addr, unsigned len);
int bind_fake(int fd, const void *addr, unsigned len);
int listen_fake(int fd, int backlog);
int accept_fake(int fd, void *addr, unsigned *len);
long sendto_fake(int fd, const void *buf, size_t n, int flags, const void *addr, unsigned alen);
long recvfrom_fake(int fd, void *buf, size_t n, int flags, void *addr, unsigned *alen);
int select_fake(int nfds, void *r, void *w, void *e, void *timeout);
int setsockopt_fake(int fd, int level, int opt, const void *val, unsigned len);
int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res);
void freeaddrinfo_fake(void *res);
const char *gai_strerror_fake(int err);
const char *inet_ntop_fake(int af, const void *src, char *dst, unsigned size);
int inet_pton_fake(int af, const char *src, void *dst);
int ioctl_fake(int fd, unsigned long req, ...);
int system_fake(const char *cmd);

// extra shims for Sonic 4 Ep 2
int __android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list ap);
int __libc_current_sigrtmin_fake(void);
void *gethostbyname_fake(const char *name);
int getnameinfo_fake(const void *sa, unsigned salen, char *host, unsigned hostlen, char *serv, unsigned servlen, int flags);
int getpeername_fake(int fd, void *addr, unsigned *len);
int getsockname_fake(int fd, void *addr, unsigned *len);
int getsockopt_fake(int fd, int level, int optname, void *optval, unsigned *optlen);
int recv_fake(int sockfd, void *buf, size_t len, int flags);
int send_fake(int sockfd, const void *buf, size_t len, int flags);
int shutdown_fake(int sockfd, int how);
void *getpwuid_fake(unsigned uid);
unsigned getuid_fake(void);
int setgid_fake(unsigned gid);
int setuid_fake(unsigned uid);
int initgroups_fake(const char *user, unsigned group);
int getpagesize_fake(void);
struct bionic_rlimit {
  unsigned long rlim_cur;
  unsigned long rlim_max;
};
int getrlimit_fake(int resource, struct bionic_rlimit *rlim);
int kill_fake(int pid, int sig);
int pipe_fake(int pipefd[2]);
int poll_fake(void *fds, unsigned long nfds, int timeout);
int sigaction_fake(int signum, const void *act, void *oldact);
int sigaddset_fake(void *set, int signum);
int sigdelset_fake(void *set, int signum);
int sigemptyset_fake(void *set);
int sigfillset_fake(void *set);
int sigprocmask_fake(int how, const void *set, void *oldset);
void (*signal_fake(int signum, void (*handler)(int)))(int);
#include <setjmp.h>
int sigsetjmp_fake(jmp_buf env, int savemask);
void siglongjmp_fake(jmp_buf env, int val);
int madvise_fake(void *addr, size_t length, int advice);
int mlock_fake(const void *addr, size_t len);
int mprotect_fake(void *addr, size_t len, int prot);
int pthread_attr_setschedpolicy_fake(void *attr, int policy);
int pthread_attr_setstack_fake(void *attr, void *stackaddr, size_t stacksize);
struct sched_param;
int pthread_getschedparam_fake(pthread_t thread, int *policy, struct sched_param *param);
int sem_timedwait_fake(void **s, const struct timespec *abs_timeout);
int tcgetattr_fake(int fd, void *termios_p);
int tcsetattr_fake(int fd, int optional_actions, const void *termios_p);
int timer_create_fake(int clockid, void *sevp, int *timerid);
int timer_delete_fake(int timerid);
int timer_settime_fake(int timerid, int flags, const void *new_value, void *old_value);
struct bionic_utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};
int uname_fake(struct bionic_utsname *buf);

#endif

