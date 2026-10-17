/* Hand-written libnfs configuration for the snes9xPS5 build (host test build and PS5).
 * Single-threaded library: netfs serialises every call with one mutex per connection,
 * so HAVE_MULTITHREADING, HAVE_TLS and HAVE_LIBKRB5 stay off. */

#define HAVE_ARPA_INET_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_NET_IF_H 1
#define HAVE_POLL_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME_H 1
#define HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC 1
#define HAVE_SYS_STATVFS_H 1

#ifdef __PROSPERO__
/* FreeBSD-based: sockaddr carries sa_len; no pwd.h or Linux-only headers. The console has no coarse
 * monotonic clock. */
#define HAVE_SOCKADDR_LEN 1
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif
#else
#define HAVE_SO_BINDTODEVICE 1
#define HAVE_SYS_VFS_H 1
#define HAVE_SYS_SYSMACROS_H 1
#endif

#ifndef _U_
#define _U_ __attribute__((unused))
#endif
