# libnfs in snes9xPS5

libnfs 6.0.2 (`18c5c73ee88bb7dc8da0d55dc95164bb77e49dc6`, https://github.com/sahlberg/libnfs), LGPL-2.1-or-later,
by Ronnie Sahlberg and contributors. Licence texts: `COPYING`, `LICENCE-LGPL-2.1.txt`, `LICENCE-BSD.txt`
(the `.x` protocol files and what is generated from them), `LICENCE-GPL-3.txt` (as shipped upstream, for
the utilities we do not include).

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

All of these come from the console quirks listed in `.context/projects/snes9xps5/swanstation-nfs-reference.md`.
**Unverified on hardware.** Two more things libnfs needs come from the shared PS5 shims in netfs, not from
patches here: a working `getaddrinfo` (`lib/socket.c` calls it to resolve the server) and `fcntl`
on sockets (`set_nonblocking` and `is_nonblocking`).

## Updating

Replace the folders from a new upstream tag, then reapply the table above (`grep -rn __PROSPERO__ .`
lists every change) and run `make -C ps5 test-nfs`.
