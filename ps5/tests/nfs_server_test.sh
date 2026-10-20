#!/bin/bash
# Runs the NFS backend against a real NFSv3 server (unfs3) on this machine: bash nfs_server_test.sh <nfs-server-test>
# Needs root (rpcbind listens on port 111), rpcbind, rpcinfo and unfsd (UNFSD=/path/to/unfsd if it is not in PATH).
# Stock unfs3 splits every reply above a few KB into RPC fragments, which libnfs 6.0.2 could not join (not rechecked with the current libnfs); build it with
#   transp = svc_vc_create(sock, 1 << 22, 1 << 22);   (daemon.c, instead of 0, 0)
# so the replies of the backend's 128 KiB reads stay in one piece. Skips (and passes) when something is missing.
set -u
BIN=${1:?usage: nfs_server_test.sh <nfs-server-test>}
UNFSD=${UNFSD:-$(command -v unfsd || true)}
for tool in rpcbind rpcinfo; do
	command -v $tool >/dev/null || { echo "skipped: $tool not installed"; exit 0; }
done
[ -n "$UNFSD" ] && [ -x "$UNFSD" ] || { echo "skipped: no unfsd (set UNFSD=...)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "skipped: needs root for rpcbind and unfsd"; exit 0; }

T=$(mktemp -d /tmp/nfs-server-test.XXXXXX)
RPCBIND_PID=
UNFSD_PID=
cleanup() {
	[ -n "$UNFSD_PID" ] && kill "$UNFSD_PID" 2>/dev/null
	[ -n "$RPCBIND_PID" ] && kill "$RPCBIND_PID" 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT

mkdir -p "$T/snes/hacks" "$T/snes/dir with space" "$T/secret"
python3 - "$T/snes/Game.sfc" <<'PY'
import random, sys
r = random.Random(1)
open(sys.argv[1], "wb").write(bytes(r.getrandbits(8) for _ in range(3500000)))
PY
echo hack >"$T/snes/hacks/Hack.sfc"
echo x >"$T/snes/dir with space/a.sfc"
ln -s Game.sfc "$T/snes/link.sfc"
ln -s nowhere "$T/snes/dead.sfc"
echo s >"$T/secret/s.txt"
cat >"$T/exports" <<EXP
$T/snes 127.0.0.1(ro,no_root_squash)
$T/secret 10.9.9.9(ro)
EXP

if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
	rpcbind -w -f >"$T/rpcbind.log" 2>&1 &
	RPCBIND_PID=$!
	for _ in $(seq 50); do
		rpcinfo -p 127.0.0.1 >/dev/null 2>&1 && break
		sleep 0.1
	done
fi
"$UNFSD" -d -e "$T/exports" >"$T/unfsd.log" 2>&1 &
UNFSD_PID=$!
for _ in $(seq 50); do
	rpcinfo -p 127.0.0.1 2>/dev/null | grep -q mountd && break
	sleep 0.1
done
rpcinfo -p 127.0.0.1 2>/dev/null | grep -q mountd || { echo "FAIL: unfsd did not register"; cat "$T/unfsd.log"; exit 1; }

NFS_TEST_HOST=127.0.0.1 NFS_TEST_EXPORT=${T#/}/snes NFS_TEST_DIR=$T/snes NFS_TEST_DENIED=${T#/}/secret \
	NFS_TEST_SERVER_PID=$UNFSD_PID ASAN_OPTIONS=detect_leaks=0 "$BIN"
