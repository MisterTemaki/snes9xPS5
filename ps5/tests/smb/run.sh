#!/bin/bash
# Snes9x PS5: the SMB backend (netfs/backend_smb.cpp + libsmb2) against a real Samba server, in userspace on the
# loopback. Skipped when smbd is not installed. Run from `make -C ps5 test`, or alone after `make smb-test-bin`.
set -u
cd "$(dirname "$0")/../.."
BIN=$PWD/build/host/smb-backend-test
SMBD=$(command -v smbd || ls /usr/sbin/smbd 2>/dev/null || true)
SMBPASSWD=$(command -v smbpasswd || ls /usr/bin/smbpasswd 2>/dev/null || true)
if [ -z "$SMBD" ] || [ -z "$SMBPASSWD" ]; then
	echo "SMB backend tests: skipped (install Samba: smbd and smbpasswd)"
	exit 0
fi
[ -x "$BIN" ] || { echo "SMB backend tests: $BIN is not built"; exit 1; }

T=$(mktemp -d)
SMBD_PID=
cleanup() { stop_smbd; rm -rf "$T"; }
trap cleanup EXIT
USER_NAME=$(id -un)
PASS='p@ss&word=1#;%'
PORT=$((21000 + RANDOM % 8000))

mkdir -p "$T"/{share/games/snes/sub/deep,share/private,priv,lock,state,cache,run,ncalrpc}
python3 - "$T/share" <<'PY'
import random, sys, os
root = sys.argv[1]
r = random.Random(1)  # deterministic
snes = root + "/games/snes"
open(snes + "/big.sfc", "wb").write(r.randbytes(6 * 1024 * 1024))
open(snes + "/empty.bin", "wb").close()
open(snes + "/one.bin", "wb").write(b"x")
open(snes + "/Pokémon Test.sfc", "wb").write(r.randbytes(5000))
open(snes + "/sub/deep/file.bin", "wb").write(r.randbytes(100))
for i in range(300):
    open("%s/rom%03d.smc" % (snes, i), "wb").write(r.randbytes(10))
open(root + "/private/secret.txt", "w").write("shh")
PY
chmod 755 "$T" # the guest account (nobody) has to get to the files
chmod -R a+rX "$T/share"

stop_smbd() {
	[ -n "$SMBD_PID" ] || return 0
	kill -TERM "$SMBD_PID" 2>/dev/null
	local i
	for ((i = 0; i < 50; i++)); do
		kill -0 "$SMBD_PID" 2>/dev/null || break
		sleep 0.1
	done
	kill -KILL "$SMBD_PID" 2>/dev/null
	pkill -KILL -f -- "-s $T/smb.conf" 2>/dev/null # its helper processes
	wait "$SMBD_PID" 2>/dev/null
	SMBD_PID=
}

start_smbd() { # extra [global] lines
	stop_smbd
	cat >"$T/smb.conf" <<CONF
[global]
	workgroup = TEST
	server role = standalone server
	interfaces = 127.0.0.1
	bind interfaces only = yes
	smb ports = $PORT
	log level = 0
	map to guest = Bad User
	private dir = $T/priv
	lock directory = $T/lock
	state directory = $T/state
	cache directory = $T/cache
	pid directory = $T/run
	ncalrpc dir = $T/ncalrpc
	passdb backend = tdbsam:$T/priv/passdb.tdb
	disable spoolss = yes
	load printers = no
	printing = bsd
	printcap name = /dev/null
	server min protocol = SMB2_02
	$1
[games]
	path = $T/share/games
	guest ok = yes
	read only = yes
[private]
	path = $T/share/private
	valid users = $USER_NAME
	guest ok = no
	read only = yes
CONF
	printf '%s\n%s\n' "$PASS" "$PASS" | "$SMBPASSWD" -c "$T/smb.conf" -a -s "$USER_NAME" >/dev/null 2>&1
	"$SMBD" --foreground -s "$T/smb.conf" >"$T/smbd.log" 2>&1 &
	SMBD_PID=$!
	for _ in $(seq 100); do
		(echo >"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && return 0
		sleep 0.1
	done
	echo "smbd did not start:"; cat "$T/smbd.log"; return 1
}

# A Samba that cannot start here (no permission, a port taken) is an environment problem, not a test failure.
skip() { echo "SMB backend tests: skipped ($1)"; exit 0; }

FAILED=0
run() { # mode
	SMB_MODE=$1 SMB_PORT=$PORT SMB_ROOT=$T/share/games SMB_USER=$USER_NAME SMB_PASS=$PASS \
		ASAN_OPTIONS=detect_leaks=1 timeout 120 "$BIN" || FAILED=1
}

ONLY=${1:-all} # main, sign, seal or smb1, to run one
want() { [ "$ONLY" = all ] || [ "$ONLY" = "$1" ]; }
if want main; then
	echo "-- Samba, SMB 2/3, guest and user"
	start_smbd "" || skip "smbd would not start"
	run main || FAILED=1
fi
if want sign; then
	echo "-- Samba, signing mandatory"
	start_smbd "server signing = mandatory" && run sign || FAILED=1
fi
if want seal; then
	echo "-- Samba, encryption required"
	start_smbd "server smb encrypt = required" && run seal || FAILED=1
fi
if want smb1; then
	echo "-- Samba, SMB 1 only"
	if start_smbd "server min protocol = NT1
	server max protocol = NT1
	ntlm auth = yes"; then run smb1 || FAILED=1; else echo "(this Samba cannot serve SMB 1: that case is skipped)"; fi
fi
exit $FAILED
