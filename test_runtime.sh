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
  rm -f .test-limine.conf .test-good.c .test-df.c .test-ud.c .test-guard.c .test-kernel.c .test-dual.c .test-resowner.c .test-reswait.c .test-resfault.c .test-restimeout.c .test-store-noowner.c .test-store-owner.c .test-store-waiter.c .test-store-stat.c .test-timeout.c
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
cat > .test-kernel.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  volatile uint8_t *kernel;
  volatile uint8_t value;
  (void)args;
  (void)arg_len;
  kernel=(volatile uint8_t *)0xffffffff80000000ULL;
  value=*kernel;
  (void)value;
  return 1;
}
SRC
cat > .test-dual.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  uint64_t start;
  int a;

  a=arg_len==1ULL && args[0]=='A';
  gm_write(a?"DUAL_A_START\n":"DUAL_B_START\n");
  start=gm_ticks();
  while (gm_ticks()-start<300ULL) {}
  gm_write(a?"DUAL_A_END\n":"DUAL_B_END\n");
  return 0;
}
SRC
cat > .test-resowner.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  uint64_t start;
  (void)args;
  (void)arg_len;
  if (!gm_resource_acquire(GM_RESOURCE_TCP)) return 10;
  if (gm_resource_acquire(GM_RESOURCE_TCP)) return 11;
  gm_write("RES_OWNER_ACQUIRED\n");
  start=gm_ticks();
  for (;gm_ticks()-start<100ULL;) {}
  gm_write("RES_OWNER_RELEASE\n");
  if (!gm_resource_release(GM_RESOURCE_TCP)) return 12;
  return 0;
}
SRC
cat > .test-reswait.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  (void)args;
  (void)arg_len;
  gm_write("RES_WAITER_REQUEST\n");
  if (!gm_resource_acquire(GM_RESOURCE_TCP)) return 20;
  gm_write("RES_WAITER_ACQUIRED\n");
  if (!gm_resource_release(GM_RESOURCE_TCP)) return 21;
  return 0;
}
SRC
cat > .test-resfault.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  (void)args;
  (void)arg_len;
  if (!gm_resource_acquire(GM_RESOURCE_TCP)) return 30;
  gm_write("RES_FAULT_ACQUIRED\n");
  __asm__ volatile("ud2");
  return 31;
}
SRC
cat > .test-restimeout.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  (void)args;
  (void)arg_len;
  if (!gm_resource_acquire(GM_RESOURCE_TCP)) return 40;
  gm_write("RES_TIMEOUT_ACQUIRED\n");
  for (;;) {}
  return 41;
}
SRC
cat > .test-store-noowner.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  static const char data[]="bad";
  (void)args;
  (void)arg_len;
  if (gm_store_write("/noowner.txt",data,3U)) return 50;
  gm_write("STORE_NOOWNER_REJECTED\n");
  return 0;
}
SRC
cat > .test-store-owner.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  static const char data[]="owner";
  uint64_t start;
  (void)args;
  (void)arg_len;
  if (!gm_resource_acquire(GM_RESOURCE_TCP)) return 60;
  gm_write("TCP_OWNER_ACQUIRED\n");
  if (!gm_store_write("/tcp.txt",data,5U)) return 61;
  gm_write("TCP_OWNER_WROTE\n");
  start=gm_ticks();
  for (;gm_ticks()-start<300ULL;) {}
  if (!gm_resource_release(GM_RESOURCE_TCP)) return 62;
  gm_write("TCP_OWNER_RELEASED\n");
  return 0;
}
SRC
cat > .test-store-waiter.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  char data[6];
  uint16_t got;
  (void)args;
  (void)arg_len;
  gm_write("TCP_WAITER_REQUEST\n");
  if (!gm_resource_acquire(GM_RESOURCE_TCP)) return 70;
  gm_write("TCP_WAITER_ACQUIRED\n");
  got=gm_store_read("/tcp.txt",0ULL,data,5U);
  if (got!=5U || data[0]!='o' || data[1]!='w' || data[2]!='n' || data[3]!='e' || data[4]!='r') return 71;
  if (!gm_resource_release(GM_RESOURCE_TCP)) return 72;
  gm_write("TCP_WAITER_READ_OK\n");
  return 0;
}
SRC
cat > .test-store-stat.c <<'SRC'
// Gianluca Mazzini @2026- Version 1.0
#include "gmprog.h"
int gm_main(const char *args,uint64_t arg_len) {
  static const char one[]="x";
  uint64_t size;
  (void)args;
  (void)arg_len;
  if (!gm_resource_acquire(GM_RESOURCE_TCP)) return 80;
  if (!gm_store_write("/empty.txt",one,0U)) return 81;
  size=999ULL;
  if (!gm_store_stat("/empty.txt",&size) || size!=0ULL) return 82;
  gm_write("STAT_EMPTY_OK\n");
  size=999ULL;
  if (gm_store_stat("/missing-stat.txt",&size)) return 83;
  gm_write("STAT_MISSING_OK\n");
  if (!gm_store_write("/one.txt",one,1U)) return 84;
  size=0ULL;
  if (!gm_store_stat("/one.txt",&size) || size!=1ULL) return 85;
  gm_write("STAT_ONE_OK\n");
  if (!gm_resource_release(GM_RESOURCE_TCP)) return 86;
  return 0;
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
make -s gm SRC=.test-kernel.c OUT="$RUNTIME_ROOT/programs/x86_64/kernelmap.gm"
make -s gm SRC=.test-dual.c OUT="$RUNTIME_ROOT/programs/x86_64/dual.gm"
make -s gm SRC=.test-resowner.c OUT="$RUNTIME_ROOT/programs/x86_64/resowner.gm"
make -s gm SRC=.test-reswait.c OUT="$RUNTIME_ROOT/programs/x86_64/reswait.gm"
make -s gm SRC=.test-resfault.c OUT="$RUNTIME_ROOT/programs/x86_64/resfault.gm"
make -s gm SRC=.test-restimeout.c OUT="$RUNTIME_ROOT/programs/x86_64/restimeout.gm"
make -s gm SRC=.test-store-noowner.c OUT="$RUNTIME_ROOT/programs/x86_64/storenoowner.gm"
make -s gm SRC=.test-store-owner.c OUT="$RUNTIME_ROOT/programs/x86_64/storeowner.gm"
make -s gm SRC=.test-store-waiter.c OUT="$RUNTIME_ROOT/programs/x86_64/storewaiter.gm"
make -s gm SRC=.test-store-stat.c OUT="$RUNTIME_ROOT/programs/x86_64/storestat.gm"
make -s gm SRC=.test-timeout.c OUT="$RUNTIME_ROOT/programs/x86_64/timeout.gm"
make -s gm SRC=gmapps/diag.c OUT="$RUNTIME_ROOT/programs/x86_64/diag.gm"
make -s gm SRC=gmapps/storecat.c OUT="$RUNTIME_ROOT/programs/x86_64/storecat.gm"
cp "$RUNTIME_ROOT/programs/x86_64/good.gm" "$RUNTIME_ROOT/programs/x86_64/badimage.gm"
printf '\001' | dd of="$RUNTIME_ROOT/programs/x86_64/badimage.gm" bs=1 seek=24 conv=notrunc status=none
cp "$RUNTIME_ROOT/programs/x86_64/good.gm" "$RUNTIME_ROOT/programs/x86_64/api1.gm"
printf '\001\000\000\000' | dd of="$RUNTIME_ROOT/programs/x86_64/api1.gm" bs=1 seek=4 conv=notrunc status=none

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
send_wait 'version' 'gmker 3.0' 20

start_store
send_wait 'store connect' 'ok' 50
send_wait 'store ping' 'ok' 30
send_wait 'store write /persist.txt persist-ok' 'ok' 30
send_wait 'store cat /persist.txt' 'persist-ok' 30

send_wait 'apps' 'app 0 free ticks=0 owns=- waits=-' 20
new_has 'app 3 free ticks=0 owns=- waits=-' || fail "apps idle slots"
send_wait 'resources' 'tcp owner=free held=0 waiters=0 acquisitions=0 total=0 max=0 recent10m=0 ticks' 20

send_wait 'run good' 'returned 7' 40
new_has 'RUNTIME_GOOD' || fail "good program output"
send_wait 'run df' 'returned 0' 40
new_has 'DF_OK' || fail "direction flag service"
send_wait 'run ud' 'returned -106' 40
new_has 'program fault vector=6' || fail "invalid opcode fault"
send_wait 'run guard' 'returned -114' 40
new_has 'program fault vector=14' || fail "guard page fault"
send_wait 'run kernelmap' 'returned -114' 40
new_has 'program fault vector=14' || fail "kernel mapping user isolation"
send_wait 'run badimage' 'program load error' 40
send_wait 'run api1' 'program load error' 40
send_wait 'version' 'gmker 3.0' 20

LOG_POS=$(wc -c < "$QEMU_LOG")
printf 'run dual A\nrun dual B\n' >&3
wait_new 'started slot 0 /programs/x86_64/dual.gm' 40 || fail "dual slot 0 start"
wait_new 'started slot 1 /programs/x86_64/dual.gm' 40 || fail "dual slot 1 start"
send_wait 'version' 'gmker 3.0' 20
send_wait 'ping 10.0.2.2' 'ok' 40
wait_new 'DUAL_A_END' 80 || fail "round-robin app A"
wait_new 'DUAL_B_END' 80 || fail "round-robin app B"
grep -Fq 'app 0 returned 0' "$QEMU_LOG" || fail "round-robin app A return"
grep -Fq 'app 1 returned 0' "$QEMU_LOG" || fail "round-robin app B return"

LOG_POS=$(wc -c < "$QEMU_LOG")
printf 'run resowner
run reswait
' >&3
wait_new 'RES_OWNER_ACQUIRED' 40 || fail "resource owner acquire"
wait_new 'RES_WAITER_REQUEST' 40 || fail "resource waiter request"
wait_new 'RES_OWNER_RELEASE' 130 || fail "resource owner release"
wait_new 'RES_WAITER_ACQUIRED' 40 || fail "resource waiter wake"
wait_new 'app 0 returned 0' 40 || fail "resource owner result"
wait_new 'app 1 returned 0' 40 || fail "resource waiter result"

LOG_POS=$(wc -c < "$QEMU_LOG")
printf 'run resfault
run reswait
' >&3
wait_new 'RES_FAULT_ACQUIRED' 40 || fail "resource fault owner acquire"
wait_new 'program fault vector=6' 40 || fail "resource owner fault"
wait_new 'RES_WAITER_ACQUIRED' 40 || fail "resource release after fault"
wait_new 'app 1 returned 0' 40 || fail "resource waiter after fault result"

LOG_POS=$(wc -c < "$QEMU_LOG")
printf 'run restimeout
run reswait
' >&3
wait_new 'RES_TIMEOUT_ACQUIRED' 40 || fail "resource timeout owner acquire"
send_wait 'version' 'gmker 3.0' 20
wait_new 'program timeout' 140 || fail "resource owner timeout"
wait_new 'RES_WAITER_ACQUIRED' 40 || fail "resource release after timeout"
wait_new 'app 1 returned 0' 40 || fail "resource waiter after timeout result"

send_wait 'run diag hello' 'all tests passed' 80
new_has 'tcp acquire: ok' || fail "diag TCP acquire"
new_has 'tcp release: ok' || fail "diag TCP release"
wait_new 'app 0 returned 0' 40 || fail "diag result"

send_wait 'run storecat /diag.txt' 'diag-ok' 60
wait_new 'app 0 returned 0' 40 || fail "storecat result"

send_wait 'run storenoowner' 'STORE_NOOWNER_REJECTED' 40
new_has 'app 0 returned 0' || fail "store without TCP ownership result"

LOG_POS=$(wc -c < "$QEMU_LOG")
printf 'run storeowner\nrun storewaiter\n' >&3
wait_new 'TCP_OWNER_ACQUIRED' 40 || fail "TCP application owner acquire"
wait_new 'TCP_WAITER_REQUEST' 40 || fail "TCP application waiter request"
wait_new 'TCP_OWNER_WROTE' 60 || fail "TCP owner GMSTORE write"
send_wait 'apps' 'app 0 ready' 20
new_has 'owns=tcp waits=-' || fail "apps TCP owner"
new_has 'app 1 blocked' || fail "apps TCP waiter blocked"
new_has 'owns=- waits=tcp' || fail "apps TCP waiter resource"
send_wait 'resources' 'tcp owner=app0' 20
new_has 'waiters=1' || fail "resources TCP waiter count"
new_has 'acquisitions=' || fail "resources acquisition accounting"
new_has 'recent10m=' || fail "resources recent accounting"
send_wait 'store ping' 'ok' 40
wait_new 'TCP_OWNER_RELEASED' 330 || fail "TCP owner release"
wait_new 'TCP_WAITER_ACQUIRED' 40 || fail "TCP waiter wake"
wait_new 'TCP_WAITER_READ_OK' 60 || fail "TCP waiter GMSTORE read"
wait_new 'app 0 returned 0' 40 || fail "TCP owner result"
wait_new 'app 1 returned 0' 40 || fail "TCP waiter result"
send_wait 'resources' 'tcp owner=free held=0 waiters=0' 20
new_has 'total=' || fail "resources completed total accounting"
new_has 'max=' || fail "resources completed max accounting"
new_has 'recent10m=' || fail "resources completed recent accounting"
send_wait 'apps' 'app 0 free' 20
new_has 'app 1 free' || fail "apps slots free after TCP contention"

send_wait 'run storestat' 'STAT_EMPTY_OK' 60
wait_new 'STAT_MISSING_OK' 40 || fail "STORE_STAT missing distinction"
wait_new 'STAT_ONE_OK' 40 || fail "STORE_STAT non-zero size"
wait_new 'app 0 returned 0' 40 || fail "STORE_STAT result"

LOG_POS=$(wc -c < "$QEMU_LOG")
printf 'run timeout\nrun timeout\n' >&3
wait_new 'started slot 0 /programs/x86_64/timeout.gm' 40 || fail "CPU-bound slot 0 start"
wait_new 'started slot 1 /programs/x86_64/timeout.gm' 40 || fail "CPU-bound slot 1 start"
send_wait 'version' 'gmker 3.0' 20
send_wait 'ping 10.0.2.2' 'ok' 40
wait_new 'app 0 returned -200' 260 || fail "CPU-bound slot 0 timeout"
wait_new 'app 1 returned -200' 260 || fail "CPU-bound slot 1 timeout"

stop_store
send_wait 'store ping' 'error' 50
send_wait 'version' 'gmker 3.0' 20
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
echo '===== OK GMKER 3.0 CONSOLIDATION TEST ====='
