# gmker 2.0

gmker is a small x86_64 operating environment designed to remain understandable as a whole.

Its target is not Unix compatibility. Its target is a compact, deterministic machine with one execution flow, explicit resource bounds, native networking, remote persistent data and small gmker-native programs.

A mechanism belongs in gmker only when a concrete gmker use case requires it.

## Design target

gmker chooses:

- one execution flow;
- explicit event pumping instead of a general scheduler;
- bounded static state;
- monotonic physical page allocation;
- a narrow and explicit hardware target;
- client-native networking;
- GMSTORE as persistent named data and program source;
- one loadable gm program at a time;
- hardware isolation for loadable programs;
- a flat source tree containing one current implementation.

These are architectural choices, not reduced versions of Linux facilities.

## Execution model

After initialization gmker runs one loop:

```text
network RX
TCP timers
serial shell
HLT
```

The PIT runs at 100 Hz. Timer interrupts advance the monotonic tick counter and wake the CPU from `hlt`.

There are no kernel threads, process table or general scheduler. Operations that wait for network progress explicitly pump the same event loop.

At every point there is one active control path.

## Reliability model

Reliability is a primary design criterion.

External failure is recoverable:

```text
missing NIC
malformed packet
unreachable host
GMSTORE unavailable
TCP timeout
invalid gm image
program fault
program timeout
        |
        v
reject / offline / return to shell
```

A panic is reserved for internal corruption where continuing could damage kernel state.

The network is a capability, not a boot requirement. If the supported NIC is unavailable, gmker starts with networking offline and keeps the serial control plane alive.

Loadable gm programs execute in ring 3. They do not share kernel privilege. Recoverable CPU exceptions caused by program code, including divide, debug/breakpoint, invalid opcode, segment/stack faults, general protection, page fault, x87, alignment and SIMD faults, terminate that program and return control to gmker. Every interrupt/service entry clears the x86 Direction Flag before calling C code, so user register flags cannot alter kernel string/memory semantics.

A dedicated 8 KiB IST stack handles x86 double faults. A double fault in kernel context remains fatal, but it has an independent stack on which gmker can diagnose and panic instead of immediately losing control through a stack-related triple fault.

Each program has a 1000-tick user-execution deadline. At the current 100 Hz timer rate this is 10 seconds of ring-3 execution. The deadline is checked from timer interrupts and on service entry. A bounded service already executing in kernel context is allowed to finish, so this is not advertised as a hard 10-second wall-clock kill boundary.

## Memory model

Limine supplies the physical memory map and HHDM mapping. Boot stops with a diagnostic panic if the requested Limine base revision is not supported.

gmker selects the largest usable physical region above 16 MiB and manages it with a monotonic 4 KiB page allocator:

```text
next page -> next page -> next page -> end
```

Allocated pages are zeroed and remain allocated for the lifetime of the kernel.

There is no general-purpose kernel heap in the normal execution path.

The loadable-program arena is allocated once at boot and reused for every program:

```text
64 KiB  image
 4 KiB  arguments / return stub
 4 KiB  unmapped guard page
16 KiB  stack
```

The guard page converts downward stack overflow into a recoverable program page fault.

The arena occupies one fixed user virtual range beginning at:

```text
0x0000000040000000
```

Only that range is mapped user-accessible by gmker.

## Privilege model

gmker itself executes in ring 0.

A loadable gm program executes in ring 3 using one TSS kernel stack and one `int 0x80` service gate. This isolation exists only to protect gmker from faulty program code; it is not a Unix process model.

There is still:

- one program at a time;
- no PID;
- no fork;
- no multitasking between programs;
- no users or permissions model;
- no syscall compatibility layer;
- no per-process scheduler.

When a program returns, faults or times out, gmker restores the shell execution path directly.


## Architecture model

The gmker design is architecture-neutral above a small machine-specific boundary. The current implemented kernel backend is x86_64, but the GM01 application format already reserves native identifiers for x86_64, AArch64 and RISC-V64.

Programs are source-portable but execute as native machine code. The same C source can therefore be compiled into distinct images:

```text
/programs/x86_64/example.gm
/programs/aarch64/example.gm
/programs/riscv64/example.gm
```

GMSTORE can hold all architectures at the same time. Each gmker node lists and loads only its native namespace, and the GM01 architecture field provides a second validation before execution.

The common program virtual base is `0x40000000`. The service ABI numbers and semantic contract are shared; only the low-level service-gate implementation changes with the CPU architecture. Today `gmprog.h` implements the x86_64 `int 0x80` gate. AArch64 and RISC-V64 backends are reserved by the format but their kernel exception, MMU, timer and service-gate implementations are not yet present.

## Hardware model

gmker currently boots on x86_64 through Limine.

The current x86_64 machine model is explicit:

- legacy COM1 serial at `0x3f8`;
- PIT timer;
- 8259 PIC;
- PCI configuration through `0xcf8/0xcfc`;
- modern VirtIO-net PCI device (`1af4:1041`).

The network device is a capability rather than a boot requirement. If the modern VirtIO-net device cannot be discovered or initialized, gmker reports `net offline` and the shell remains available.

The device/transport boundary is intentionally narrow. `virtio.c` currently contains the modern VirtIO PCI transport used by x86_64. A future AArch64 or RISC-V64 machine can provide VirtIO-MMIO without duplicating Ethernet, IP, TCP or GMSTORE. No legacy VirtIO transport is kept beside the modern path.

Hardware support grows only when another concrete target requires it.

## Network model

The current path is:

```text
modern VirtIO-net PCI -> Ethernet -> ARP / IPv4 -> ICMP / TCP -> GMSTORE
```

VirtIO-net is deliberately minimal:

- VirtIO 1.x modern PCI capabilities only;
- `VIRTIO_F_VERSION_1` and `VIRTIO_NET_F_MAC` are the only negotiated features;
- one RX split virtqueue and one TX split virtqueue;
- 8 descriptors per queue;
- polling only, with virtqueue interrupts suppressed;
- no checksum, segmentation or mergeable-buffer offload;
- no multiqueue;
- asynchronous bounded TX: a frame is queued when a descriptor is free and completion is reaped by polling;
- 2 KiB DMA slots, two slots per physical page.

The two virtqueue metadata areas consume one page each. RX and TX buffers consume four pages each, for a fixed 40 KiB VirtIO queue/buffer footprint. Ethernet minimum-frame padding is handled above the device backend, so it remains correct for future transports. RX data is copied out before its DMA descriptor is returned to the device; malformed used-ring entries or a VirtIO reset/failure indication take networking offline rather than allowing corrupted queue state to propagate.

ARP uses a fixed 8-entry table. IPv4 validates the header checksum and accepts only complete, non-fragmented datagrams because gmker deliberately has no fragment reassembly. ICMP provides echo request/reply. TCP is an internal client transport, not a socket subsystem.

The default QEMU user-network configuration is:

```text
IP       10.0.2.15
netmask  255.255.255.0
gateway  10.0.2.2
```

A static address can be selected at boot:

```text
gmker.net=10.10.0.11/24,10.10.0.1
```

## TCP model

gmker is a network client, not a general-purpose network host.

It maintains one active TCP client connection and implements the transport behavior required by gmker services:

- active open;
- SYN retransmission;
- in-order receive;
- cumulative ACK;
- one outstanding transmitted payload;
- payload retransmission;
- FIN close;
- RST handling;
- a fixed 4096-byte circular RX buffer with no data compaction on reads;
- an in-order payload is acknowledged only after the complete payload fits in the RX ring; if it does not fit, the ACK remains at the previous sequence so the peer can retransmit;
- a transmit/ACK timeout invalidates the connection instead of leaving a stale connection reusable.

There is no BSD socket API and no TCP listener inside gmker.

The transport bounds are coherent:

```text
TCP RX ring         4096 bytes
reliable TCP TX     1280 bytes
GMSTORE data block  1024 bytes
```

GMSTORE requires the exact `GMSTORE 2.0` greeting after TCP establishment. A missing, different or invalid greeting closes that connection, so a later operation can establish a clean session rather than inheriting a half-valid transport.

## GMSTORE

GMSTORE is gmker's persistence service and the only source of loadable programs.

The gmker kernel contains the GMSTORE client. `gmstored.c` is the external server.

```text
gmker                     host
------                     ----
GMSTORE client -- TCP -->  gmstored
                              |
                              v
                            store/
```

The default server is:

```text
10.0.2.2:7070
```

The client destination is configurable at boot:

```text
gmker.store=10.10.0.1:7070
```

The server bind address and port are independent:

```sh
./gmstored --root store --host 0.0.0.0 --port 7070
```

`gmstored` is single-process and event-driven. One `poll()` loop serves up to 16 simultaneous clients with fixed per-client buffers. It uses neither threads nor `fork()`.

The server opens the configured root once as a directory fd. File operations walk path components with dirfd-relative `openat`/`fstatat`/`unlinkat` operations and `O_NOFOLLOW`; symlink components are never followed, including intermediate directories. This keeps every GMSTORE object confined below the configured root.

Persistence means data is backed by the host filesystem and survives gmker restarts. GMSTORE 2.0 does not promise transactional replacement or power-loss atomicity; those semantics would require an explicit stronger storage contract rather than hidden complexity.

The protocol is intentionally small:

```text
PING
LIST
STAT path
READ path offset count
WRITE path count\n<data>
APPEND path count\n<data>
DEL path
```

A data operation carries at most 1024 bytes.

GMSTORE names are persistent named byte objects, not mounted files inside gmker. A useful namespace convention is:

```text
/programs/...   executable gm images
/config/...     configuration
/results/...    program results
/logs/...       append-only style output
```

The convention does not create a VFS or local filesystem.

GMSTORE availability is not required for gmker to remain alive. If GMSTORE is unavailable, the shell and local diagnostics remain available; loadable programs and persistent data operations simply cannot proceed.

## Loadable gm programs

All application programs come from GMSTORE.

There is no second built-in application model.

A typical execution is:

```text
> run random

/programs/x86_64/random.gm
        |
        v
STAT + READ from GMSTORE
        |
        v
validate GM01
        |
        v
copy to program arena
        |
        v
ring 3 execution
        |
        +--> gm services
        |
        +--> return / fault / timeout
        |
        v
shell
```

`run random` resolves to `/programs/x86_64/random.gm`.

An explicit path may also be used, but it must remain under `/programs/`:

```text
run /programs/x86_64/random.gm 10
```

`programs` lists GMSTORE objects below the native architecture namespace. On the current backend this is `/programs/x86_64/`. Future backends use `/programs/aarch64/` and `/programs/riscv64/`.

## GM01 format

A `.gm` file is not an ELF image.

ELF is only an intermediate host build format. `gmpack` converts the linked ELF into the compact format consumed by gmker.

The GM01 header is 24 bytes, little-endian:

```text
4 bytes   magic       "GM01"
4 bytes   API version
4 bytes   architecture
4 bytes   image size
4 bytes   entry offset
4 bytes   FNV-1a checksum of the image
```

Architecture identifiers are:

```text
1  x86_64
2  aarch64
3  riscv64
```

The header is followed by one flat native image of at most 64 KiB. The entry field is an offset from the common program base `0x40000000` (1 GiB), chosen to fit the supported 64-bit address models including RISC-V Sv39. A kernel rejects an image whose architecture field does not match its native backend.

The loader validates:

- magic;
- API version;
- native architecture;
- total object size;
- image size;
- entry offset;
- maximum arena size;
- checksum.

`gmpack` separately validates that its input is an ELF64 executable for a supported architecture and that the entry address lies inside an executable load segment before producing GM01.

The whole program arena is cleared before loading a new image.

## gm service ABI

Programs do not call kernel functions directly. They use the small service ABI exposed by `gmprog.h` through `int 0x80`.

API version 1 contains:

```text
0  exit
1  write serial string
2  print u64
3  ticks
4  gateway address
5  ping
6  GMSTORE read
7  GMSTORE write
8  GMSTORE append
```

Every pointer supplied by a program is range-checked before the kernel dereferences it.

GMSTORE read/write services accept at most one 1024-byte block per call. Programs can loop when they need larger objects.

A program can therefore save persistent output directly, for example:

```c
#include "gmprog.h"

int gm_main(const char *args,uint64_t arg_len) {
  static const char path[]="/results/example.txt";
  static const char text[]="result from gm\n";

  (void)args;
  (void)arg_len;
  if (!gm_store_write(path,text,sizeof(text)-1U)) return 1;
  return 0;
}
```

On the host this becomes an ordinary object below the configured GMSTORE root, for example:

```text
store/results/example.txt
```

## Building a gm program

A gm program is normal freestanding native C using `gmprog.h`. The source-level API is shared across architectures; the generated machine code is architecture-specific. Maintained application sources live below `gmapps/`, separate from the flat kernel source while remaining part of the gmker tree.

Its entry point is:

```c
int gm_main(const char *args,uint64_t arg_len);
```

Build a `.gm` image with:

```sh
make gm SRC=gmapps/example.c OUT=store/programs/x86_64/example.gm
```

The build path is:

```text
example.c
   |
clang freestanding (`-mcmodel=small` on x86_64)
   |
example ELF linked at fixed gm base with 16-byte text/data alignment
   |
gmpack
   |
example.gm
```

Deploy it by placing it below the GMSTORE root:

```sh
mkdir -p store/programs/x86_64
cp example.gm store/programs/x86_64/example.gm
```

Then from gmker:

```text
run example
```

The maintained application examples currently are:

```text
gmapps/primes100.c   compute and print the first 100 prime numbers
gmapps/diag.c        exercise the complete current application ABI
gmapps/storecat.c    print a text object from GMSTORE
gmapps/netping.c     parse an IPv4 argument and ping it
```

Build outputs are deployed below `store/programs/x86_64/`. Example invocations are:

```text
run primes100
run diag hello
run storecat /diag.txt
run netping 10.0.2.2
```

## Serial shell

The serial shell is the machine control plane.

```text
help
version
uptime
mem
net
arp
ping IP
tcp
programs
run NAME [ARGS]
store status
store connect
store ping
store ls
store stat PATH
store cat PATH
store write PATH TEXT
store append PATH TEXT
store rm PATH
shutdown
```

`shutdown` exits QEMU through `isa-debug-exit`.

## Boot configuration

A physical or LAN target can pass explicit configuration through Limine:

```text
cmdline: gmker.net=10.10.0.11/24,10.10.0.1 gmker.store=10.10.0.1:7070
```

Configuration remains small and boot-time static.

## Source model

The maintained kernel implementation is flat. Application sources are the only maintained source subtree and live in `gmapps/`:

- `kernel.c` - boot configuration, initialization and main event loop;
- `core.c` - runtime, serial, memory, page mapping, GDT/TSS, IDT/PIC/PIT;
- `virtio.c` - modern VirtIO-net PCI transport, split virtqueues and DMA buffers;
- `net.c` - Ethernet, ARP, IPv4 and ICMP;
- `tcpstore.c` - one TCP client and GMSTORE client;
- `programs.c` - GM01 loader, ring-3 runtime and service dispatch;
- `shell.c` - serial command interpreter;
- `gmker.h` - kernel internal interface;
- `gmabi.h` - shared GM01/architecture/service ABI constants;
- `gmprog.h` - loadable-program service ABI;
- `gmpack.c` - host GM01 packer;
- `gmprog.ld` - fixed-address program linker layout;
- `start.asm`, `isr.asm` - kernel entry and interrupt/service transitions;
- `gmstored.c` - external GMSTORE server;
- `linker.ld`, `limine.conf`, `Makefile` - kernel build and boot chain;
- `limine.h` - external Limine protocol definition;
- `gmapps/` - source files for loadable gm applications, never linked into the kernel.

The kernel source remains flat. `gmapps/` is the single application-source subtree and does not create a second built-in application model.

## Build

Required build tools are:

```text
clang
GNU ld
nasm
cc
```

Build and validate kernel, GMSTORE server and GM01 packer:

```sh
make clean
make check
```

The kernel uses freestanding x86_64 C with `-std=gnu89 -Wall -Wextra -Werror`.

The host tools are also built with `-std=gnu89 -Wall -Wextra -Werror`.

Fetch the pinned Limine boot artifacts when needed:

```sh
make bootfiles
```

Create an ISO:

```sh
make iso
```

Run it in QEMU:

```sh
make run
```

The QEMU target exposes only the modern VirtIO-net PCI interface to gmker:

```text
-device virtio-net-pci,disable-legacy=on,disable-modern=off,netdev=net0
```

Start GMSTORE with:

```sh
make store
```

## Project rule

Every change must preserve a simple answer to four questions:

1. What concrete gmker function requires this mechanism?
2. What is its explicit resource bound?
3. What happens when it fails?
4. Can an older mechanism be removed instead of kept beside it?

A feature is complete when the current source contains one clear implementation, its failure behavior is defined and no superseded path remains.

The target is not the smallest possible kernel. The target is the smallest coherent kernel that performs its intended work reliably.

## Consolidation test

The x86_64/QEMU baseline has a repeatable end-to-end regression suite:

```sh
make test
```

The test is bounded and self-cleaning. It validates:

- host GMSTORE multi-client operation;
- GMSTORE root confinement against intermediate symlink escape;
- persistence across GMSTORE restart;
- rejection of an unsupported ET_DYN input by `gmpack`;
- QEMU boot with modern VirtIO-net;
- network-up state, ARP and ICMP gateway reachability;
- GMSTORE unavailable while gmker remains responsive;
- later GMSTORE connect and ping;
- persistent GMSTORE write/read;
- valid GM01 load and ring-3 execution;
- service entry after a user program sets the x86 Direction Flag;
- recoverable invalid-opcode fault;
- recoverable guard-page page fault;
- rejection of a corrupted GM01 image;
- ring-3 execution timeout and recovery;
- GMSTORE loss while running, shell survival, reconnect and persistent-data recovery;
- clean QEMU shutdown through `isa-debug-exit`.

A successful run ends with:

```text
===== OK GMKER 2.0 CONSOLIDATION TEST =====
```

This is the consolidation baseline for gmker 2.0 on the current x86_64 QEMU target. Additional architectures, physical NIC drivers, protocol features or execution models are future evolution rather than requirements for this baseline.
