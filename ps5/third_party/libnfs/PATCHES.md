# libnfs in snes9xPS5

libnfs from upstream master after 8.0.0 (`2d368ade3e30c4782d54a63efc4d029c6fc75dff`, 2026-10-14,
https://github.com/sahlberg/libnfs), LGPL-2.1-or-later,
by Ronnie Sahlberg and contributors. Licence texts: `COPYING`, `LICENCE-LGPL-2.1.txt`, `LICENCE-BSD.txt`
(the `.x` protocol files and what is generated from them), `LICENCE-GPL-3.txt` (as shipped upstream, for
the utilities we do not include).

Why master and not the 8.0.0 tag: 8.0.0 has the ZDR/NFSv4 bounds fixes that 6.0.2 lacked (91de6ec, 7b3c72a,
55c18ea, c0ad6bc, all in 7.0.0 and later), but the fix that makes the ZDR decoders honour their maxsize
(7613236, "ZDR: enforce the maxsize bound") only landed on master after 8.0.0. The few master commits on top
of 8.0.0 besides it are NFSv4.0 lease renewal (e0d26a0, f83f4a4), a ZDR assert (57adf64), an nfs3 errno fix
(27d0cd0), nfs-stat (a utility we leave out) and CMake Kerberos options.

Only the client library is vendored: `lib mount nfs nfs4 nlm nsm portmap rquota` and `include`. Left out:
examples, utils, tests, documentation, the Windows/AROS/PS2/PS3 ports, TLS and Kerberos support.

## What we changed

`config.h` is ours (upstream generates it). It leaves multithreading, TLS and Kerberos off: netfs holds one
lock per connection, so libnfs only ever runs on one thread at a time.

Every source change is under `#ifdef __PROSPERO__` and does not change the host build:

| File | Change | Why |
|---|---|---|
| `lib/socket.c` `set_tcp_sockopt` | Use `IPPROTO_TCP` instead of `getprotobyname("tcp")`. | The console has no protocol database. |
| `lib/socket.c` reserved-port loop | Skip the `getservbyport` check. | No services database. |
| `lib/socket.c` `set_keepalive` | Set `SO_KEEPALIVE` only, and never fail the connection over it. | Refused tuning options (`TCP_KEEPIDLE` and friends) must not make a mount fail; the netfs watchdog notices dead servers. |
| `lib/socket.c` after `set_nonblocking` | `setsockopt(SO_NOSIGPIPE)` on each socket. | Replaces `signal(SIGPIPE, SIG_IGN)`. |
| `lib/libnfs.c` `nfs_init_context` | Do not call `signal(SIGPIPE, SIG_IGN)`. | A native title has no `signal()`. |
| `config.h` | `CLOCK_MONOTONIC_COARSE` is `CLOCK_MONOTONIC`; no Linux-only headers. | Not on the console. |

All of these come from console quirks noted while porting (the contributor's design notes, not in this repository).
**Unverified on hardware.** Two more things libnfs needs come from the shared PS5 shims in netfs, not from
patches here: a working `getaddrinfo` (`lib/socket.c` calls it to resolve the server) and `fcntl`
on sockets (`set_nonblocking` and `is_nonblocking`).

## Updating

Replace the folders from a new upstream tag or commit (only `*.c` and `*.h`, without `include/win32`), then reapply the table above (`grep -rn __PROSPERO__ .`
lists every change) and run `make -C ps5 test-nfs`.
