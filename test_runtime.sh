#!/bin/bash
set -e

HOST_PORT=17071
RUNTIME_PORT=17070
HOST_ROOT=.test-host-store
RUNTIME_ROOT=.test-runtime-store
OUTSIDE=.test-outside
HOST_LOG=.test-host-store.log
STORE_LOG=.test-runtime-store.log
QEMU_LOG=.test-qemu.log
QEMU_FIFO=.test-qemu.in
TEST_ISO=gmker-test.iso
HOST_PID=
STORE_PID=
QEMU_PID=
LOG_POS=0

fail() {
  echo "FAIL: $1"
  if [ -f "$QEMU_LOG" ]; then
    echo "--- qemu tail ---"
    tail -n 80 "$QEMU_LOG" || true
  fi
  exit 1
}

cleanup() {
  if [ -n "$QEMU_PID" ]; then kill "$QEMU_PID" 2>/dev/null || true; fi
  if [ -n "$STORE_PID" ]; then kill "$STORE_PID" 2>/dev/null || true; fi
  if [ -n "$HOST_PID" ]; then kill "$HOST_PID" 2>/dev/null || true; fi
  exec 3>&- 2>/dev/null || true
  rm -f "$QEMU_FIFO" "$TEST_ISO" "$HOST_LOG" "$STORE_LOG" "$QEMU_LOG"
  rm -f .test-limine.conf .test-good.c .test-df.c .test-ud.c .test-guard.c .test-timeout.c
  rm -rf .test-iso-root "$HOST_ROOT" "$RUNTIME_ROOT" "$OUTSIDE"
}
trap cleanup EXIT INT TERM

wait_listen() {
  local port="$1"
  local i

  for ((i=0;i<100;i++)); do
    if ss -ltn | grep -q ":$port "; then return 0; fi
    sleep 0.05
  done
  return 1
}

wait_new() {
  local pattern="$1"
  local tenths="$2"
  local i

  for ((i=0;i<tenths*10;i++)); do
    if tail -c +$((LOG_POS+1)) "$QEMU_LOG" 2>/dev/null | tr -d '\r' | grep -Fq -- "$pattern"; then return 0; fi
    if [ -n "$QEMU_PID" ] && ! kill -0 "$QEMU_PID" 2>/dev/null; then return 1; fi
    sleep 0.01
  done
  return 1
}

send_wait() {
  local command="$1"
  local expected="$2"
  local tenths="$3"

  LOG_POS=$(wc -c < "$QEMU_LOG")
  printf '%s\n' "$command" >&3
  wait_new "$expected" "$tenths" || fail "$command -> $expected"
}

new_has() {
  local pattern="$1"
  tail -c +$((LOG_POS+1)) "$QEMU_LOG" | tr -d '\r' | grep -Fq -- "$pattern"
}

start_store() {
  ./gmstored --root "$RUNTIME_ROOT" --host 0.0.0.0 --port "$RUNTIME_PORT" > "$STORE_LOG" 2>&1 &
  STORE_PID=$!
  wait_listen "$RUNTIME_PORT" || fail "runtime gmstored listen"
}

stop_store() {
  if [ -n "$STORE_PID" ]; then
    kill "$STORE_PID" 2>/dev/null || true
    wait "$STORE_PID" 2>/dev/null || true
    STORE_PID=
  fi
}

echo '--- host GMSTORE isolation/multiclient ---'
rm -rf "$HOST_ROOT" "$OUTSIDE"
mkdir -p "$HOST_ROOT" "$OUTSIDE"
printf '%s' secret > "$OUTSIDE/secret.txt"
ln -s "../$OUTSIDE" "$HOST_ROOT/escape"
./gmstored --root "$HOST_ROOT" --host 127.0.0.1 --port "$HOST_PORT" > "$HOST_LOG" 2>&1 &
HOST_PID=$!
wait_listen "$HOST_PORT" || fail "host gmstored listen"

exec 4<>/dev/tcp/127.0.0.1/$HOST_PORT
IFS= read -r line <&4
[ "$line" = 'GMSTORE 2.0' ] || fail "host greeting client A"
exec 5<>/dev/tcp/127.0.0.1/$HOST_PORT
IFS= read -r line <&5
[ "$line" = 'GMSTORE 2.0' ] || fail "host greeting client B"
printf 'PING\n' >&4
IFS= read -r line <&4
[ "$line" = 'OK PONG' ] || fail "host ping client A"
printf 'WRITE /multi.txt 5\nhello' >&4
IFS= read -r line <&4
[ "$line" = 'OK' ] || fail "host write"
printf 'STAT /multi.txt\n' >&5
IFS= read -r line <&5
[ "$line" = 'OK 5' ] || fail "host multiclient stat"
printf 'STAT /escape/secret.txt\n' >&5
IFS= read -r line <&5
[[ "$line" == ERR* ]] || fail "host symlink confinement"
printf 'LIST\n' >&4
seen_escape=0
seen_multi=0
for ((i=0;i<20;i++)); do
  IFS= read -r line <&4 || fail "host list read"
  [ "$line" = 'ITEM /multi.txt' ] && seen_multi=1
  [[ "$line" == *escape* ]] && seen_escape=1
  [ "$line" = 'OK END' ] && break
done
[ "$seen_multi" -eq 1 ] || fail "host list missing file"
[ "$seen_escape" -eq 0 ] || fail "host list exposed symlink"
exec 4>&-
exec 5>&-
kill "$HOST_PID"
wait "$HOST_PID" 2>/dev/null || true
HOST_PID=
./gmstored --root "$HOST_ROOT" --host 127.0.0.1 --port "$HOST_PORT" > "$HOST_LOG" 2>&1 &
HOST_PID=$!
wait_listen "$HOST_PORT" || fail "host gmstored restart"
exec 4<>/dev/tcp/127.0.0.1/$HOST_PORT
IFS= read -r line <&4
printf 'READ /multi.txt 0 5\n' >&4
IFS= read -r line <&4
[ "$line" = 'OK 5' ] || fail "host persistence header"
IFS= read -r -n 5 data <&4
[ "$data" = 'hello' ] || fail "host persistence data"
exec 4>&-
kill "$HOST_PID"
wait "$HOST_PID" 2>/dev/null || true
HOST_PID=
echo 'host GMSTORE: OK'

echo '--- gmpack negative validation ---'
if ./gmpack /bin/true .test-invalid.gm >/dev/null 2>&1; then fail "gmpack accepted ET_DYN"; fi
rm -f .test-invalid.gm
echo 'gmpack validation: OK'

echo '--- build ring3 regression images ---'
rm -rf "$RUNTIME_ROOT"
mkdir -p "$RUNTIME_ROOT/programs/x86_64"
cat > .test-good.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  (void)args;
  (void)arg_len;
  gm_write("RUNTIME_GOOD\n");
  return 7;
}
SRC
cat > .test-df.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  (void)args;
  (void)arg_len;
  __asm__ volatile("std":::"cc");
  gm_write("DF_OK\n");
  __asm__ volatile("cld":::"cc");
  return 0;
}
SRC
cat > .test-ud.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  (void)args;
  (void)arg_len;
  __asm__ volatile("ud2");
  return 1;
}
SRC
cat > .test-guard.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  volatile uint8_t *guard;
  (void)args;
  (void)arg_len;
  guard=(volatile uint8_t *)(GM_PROGRAM_BASE+GM_PROGRAM_IMAGE_MAX+4096ULL);
  *guard=1;
  return 1;
}
SRC
cat > .test-timeout.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  (void)args;
  (void)arg_len;
  for (;;) {}
  return 1;
}
SRC
make -s gm SRC=.test-good.c OUT="$RUNTIME_ROOT/programs/x86_64/good.gm"
make -s gm SRC=.test-df.c OUT="$RUNTIME_ROOT/programs/x86_64/df.gm"
make -s gm SRC=.test-ud.c OUT="$RUNTIME_ROOT/programs/x86_64/ud.gm"
make -s gm SRC=.test-guard.c OUT="$RUNTIME_ROOT/programs/x86_64/guard.gm"
make -s gm SRC=.test-timeout.c OUT="$RUNTIME_ROOT/programs/x86_64/timeout.gm"
cp "$RUNTIME_ROOT/programs/x86_64/good.gm" "$RUNTIME_ROOT/programs/x86_64/badimage.gm"
printf '\001' | dd of="$RUNTIME_ROOT/programs/x86_64/badimage.gm" bs=1 seek=24 conv=notrunc status=none

echo '--- build isolated runtime ISO ---'
cat > .test-limine.conf <<EOF_CONF
timeout: 0

/gmker
protocol: limine
path: boot():/gmker.elf
cmdline: gmker.store=10.0.2.2:$RUNTIME_PORT
EOF_CONF
rm -rf .test-iso-root
mkdir -p .test-iso-root/boot/limine .test-iso-root/EFI/BOOT
cp gmker.elf .test-iso-root/gmker.elf
cp .test-limine.conf .test-iso-root/boot/limine/limine.conf
cp limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin .test-iso-root/boot/limine/
cp BOOTX64.EFI BOOTIA32.EFI .test-iso-root/EFI/BOOT/
xorriso -as mkisofs -quiet -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label .test-iso-root -o "$TEST_ISO"
./limine bios-install "$TEST_ISO" >/dev/null

echo '--- QEMU end-to-end and recovery ---'
rm -f "$QEMU_FIFO" "$QEMU_LOG"
mkfifo "$QEMU_FIFO"
qemu-system-x86_64 -m 128M -cdrom "$TEST_ISO" -display none -serial mon:stdio -no-reboot \
  -device isa-debug-exit,iobase=0x501,iosize=0x04 \
  -netdev user,id=net0 -device virtio-net-pci,disable-legacy=on,disable-modern=off,netdev=net0 \
  < "$QEMU_FIFO" > "$QEMU_LOG" 2>&1 &
QEMU_PID=$!
exec 3>"$QEMU_FIFO"
LOG_POS=0
wait_new 'type help' 50 || fail "boot prompt"

send_wait 'net' 'net state=up' 20
send_wait 'ping 10.0.2.2' 'ok' 40
send_wait 'store connect' 'error' 50
send_wait 'version' 'gmker 2.0' 20

start_store
send_wait 'store connect' 'ok' 50
send_wait 'store ping' 'ok' 30
send_wait 'store write /persist.txt persist-ok' 'ok' 30
send_wait 'store cat /persist.txt' 'persist-ok' 30

send_wait 'run good' 'returned 7' 40
new_has 'RUNTIME_GOOD' || fail "good program output"
send_wait 'run df' 'returned 0' 40
new_has 'DF_OK' || fail "direction flag service"
send_wait 'run ud' 'returned -106' 40
new_has 'program fault vector=6' || fail "invalid opcode fault"
send_wait 'run guard' 'returned -114' 40
new_has 'program fault vector=14' || fail "guard page fault"
send_wait 'run badimage' 'program load error' 40
send_wait 'version' 'gmker 2.0' 20
send_wait 'run timeout' 'returned -200' 130
new_has 'program timeout' || fail "program timeout marker"

stop_store
send_wait 'store ping' 'error' 50
send_wait 'version' 'gmker 2.0' 20
start_store
send_wait 'store connect' 'ok' 50
send_wait 'store cat /persist.txt' 'persist-ok' 30

send_wait 'shutdown' 'shutdown' 20
exec 3>&-
set +e
wait "$QEMU_PID"
qrc=$?
set -e
QEMU_PID=
[ "$qrc" -eq 33 ] || fail "qemu exit code $qrc"
stop_store

echo 'QEMU runtime/recovery: OK'
echo '===== OK GMKER 2.0 CONSOLIDATION TEST ====='
