/* libsmb2 6.2+ (master 57fea04) build configuration for snes9xPS5 (replaces the CMake-generated config.h).
 * The same file serves the PS5 build (-DPS4_PLATFORM, FreeBSD-style libc) and the Linux host build. */
#define HAVE_ARPA_INET_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_POLL_H 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_LINGER 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_TIME_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_ERRNO_H 1
/* the NTLM client challenge, the client GUID and salt and the SMB3 encryption nonces come from arc4random_buf (lib/init.c
 * smb2_random_bytes): glibc 2.36+ on the host, the SDK's libc.a on the console (FreeBSD's, which reads the
 * kernel's kern.arandom sysctl and falls back to /dev/urandom) */
#define HAVE_ARC4RANDOM_BUF 1
#define STDC_HEADERS 1
#define PACKAGE "libsmb2"
#define PACKAGE_NAME "libsmb2"
#define PACKAGE_VERSION "6.2.0-57fea04"
#define PACKAGE_STRING "libsmb2 6.2.0-57fea04"
#define VERSION "6.2.0-57fea04"
