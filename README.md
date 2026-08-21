# gmker 3.2

gmker is a small x86_64 operating environment designed to remain understandable as a whole.

Its target is not Unix compatibility. Its target is a compact, deterministic machine with one kernel control flow, explicit resource bounds, native networking, remote persistent data and a small bounded set of isolated gmker-native programs.

A mechanism belongs in gmker only when a concrete gmker use case requires it.

## Design target

gmker chooses:

- one kernel control flow with bounded preemptive application time-multiplexing;
- one fixed round-robin scheduler for at most four isolated applications, with explicit kernel event pumping between quanta;
- bounded static state;
- monotonic physical page allocation;
- a narrow and explicit hardware target;
- client-native networking;
- GMSTORE as persistent named data and program source;
- at most four isolated loadable gm programs at a time;
- hardware isolation for loadable programs;
- a flat source tree containing one current implementation.

These are architectural choices, not reduced versions of Linux facilities.

## Execution model

After initialization gmker runs one kernel loop:

```text
network RX
TCP timers
serial shell
next READY application
HLT when no application is runnable
```

The PIT runs at 100 Hz. Timer interrupts advance the monotonic tick counter and provide the preemption boundary for ring-3 applications. Kernel execution itself is not scheduled as an application and always regains control between application quanta.

There are no kernel threads or Unix-style process scheduler. Up to four statically bounded application slots are scheduled round-robin with no priorities or dynamic policy. A timer interrupt from ring 3 saves the complete user register/iret frame, marks the application READY and returns to the kernel loop. Every application service call is also a scheduling point: the service result is saved in that application's context before control returns to the kernel. This prevents syscall-heavy applications from avoiding preemption.

At every instant there remains one active CPU control path; concurrency is time multiplexing between isolated application contexts under kernel control.

## Periodic application launch

gmker 3.2 adds one bounded automatic-launch mechanism without adding cron, daemon processes, a wall clock or a second scheduler. The serial command is:

```text
every SECONDS COUNT NAME [ARGS]
```

For example:

```text
every 60 10 ntp 129.6.15.28
```

creates one fixed periodic entry that launches the existing gmapp through the normal GM01 loader after 60 seconds and then repeats until 10 actual launches have occurred. `SECONDS` is converted to the existing 100 Hz monotonic kernel tick counter; calendar time is not involved. The table contains at most four entries and every entry has a finite non-zero count.

A periodic entry never overlaps with itself. If its previous gmapp is still running when another interval expires, or if all four application slots are occupied, the next launch is deferred rather than duplicated. Missed intervals are not accumulated: after each actual launch the next deadline becomes `now + interval`. A load failure does not consume `COUNT`; that entry retries on its next interval. Once the final launched gmapp terminates, the entry is removed automatically.

`periodics` displays the fixed active table, including interval, remaining launches, seconds until the next deadline, currently associated application slot, program and arguments. `cancel ID` removes that periodic reservation immediately and prevents every future launch from it. Cancellation never terminates a gmapp that has already been launched: that application continues normally until return, fault or timeout. `cancel` therefore changes only future scheduling and is deliberately distinct from an application abort mechanism.

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

The x86_64 arena spans 88 KiB of virtual address space: 64 KiB image, 4 KiB arguments, 4 KiB unmapped guard and 16 KiB stack. Twenty-one pages (84 KiB) are physically backed per application slot.

gmker now reserves four bounded application slots, each with its own x86_64 CR3 and its own physical backing for the same GM01 virtual layout beginning at `0x40000000`. The upper half of each application address space reuses the kernel/HHDM/MMIO mappings with supervisor permission only; the lower half is constructed independently for that slot. A boot-time isolation check writes different values to the same user virtual address in two slots and verifies that the values remain distinct. Runtime regression also verifies that ring 3 cannot read the high-half kernel mapping.

The scheduler is now multi-app and preemptive. The four private address spaces are persistent slot-owned arenas, and READY applications are selected by fixed round-robin order. A pure CPU loop cannot prevent the kernel loop, serial shell or network maintenance from running.

Only the application arena is mapped user-accessible by gmker.

## Privilege model

gmker itself executes in ring 0.

A loadable gm program executes in ring 3 using one TSS kernel stack and one `int 0x80` service gate. This isolation exists only to protect gmker from faulty program code; it is not a Unix process model.

Application runtime state is held in four fixed application-slot structures. Each slot owns its CR3, arena, saved user context, state and execution accounting. The implemented states are FREE, READY, RUNNING and BLOCKED; BLOCKED is reserved for kernel-resource contention and becomes active with the resource manager.

There is still:

- no PID namespace;
- no fork;
- no shared writable memory between applications;
- no users or permissions model;
- no syscall compatibility layer;
- no priorities or dynamic scheduling policy.

When an application returns, faults or times out, only that slot terminates. The kernel and other READY applications continue.

## Exclusive resource ownership

gmker has a fixed kernel resource manager for application-facing resources that cannot be used concurrently. The implemented named resources are `GM_RESOURCE_TCP` and `GM_RESOURCE_UDP`. Resource ownership is distinct from kernel control: the kernel always retains transport maintenance and control-plane use, including timers, RX, retransmission, loader operations and serial-shell GMSTORE commands, even while an application owns an application-facing network resource. Ownership is therefore exclusive between applications, not a lock against the kernel itself.

Applications use two ABI services through `gm_resource_acquire()` and `gm_resource_release()`. An acquire on a free resource assigns ownership immediately. An acquire on a busy resource saves the caller context, moves that slot to BLOCKED and enqueues it in a fixed FIFO of at most four application slots; there is no busy waiting. Release transfers ownership directly to the oldest waiter and changes that slot to READY.

Application GMSTORE services (`STORE_READ`, `STORE_WRITE` and `STORE_APPEND`) require the caller to own `GM_RESOURCE_TCP`. The one-shot UDP service requires `GM_RESOURCE_UDP`. Calls without the required ownership fail without touching the transport. Ownership remains with the application across service calls until explicit release. The intended programming discipline is acquire immediately before related network work and release immediately afterward. Kernel GMSTORE operations do not consume or steal the application owner.

An application may own at most one exclusive resource at a time. A second acquire while it already owns or waits for a resource fails. This deliberately prevents circular application lock dependency instead of providing general mutex/semaphore primitives. Applications are expected to acquire as late as practical, use the resource for the shortest interval and release immediately.

All implemented application termination paths reclaim ownership automatically: normal return/exit, recoverable fault and execution timeout. If an administrative abort operation is later added, it must use the same reclaim path rather than stealing a resource independently.

Each exclusive resource maintains bounded accounting: current owner and acquisition tick, FIFO waiters, acquisition count, completed total holding ticks, longest completed hold and 60 rolling ten-second buckets covering the most recent ten minutes. The `resources` shell command combines completed accounting with a current live hold, so `held`, `total`, `max` and `recent10m` remain meaningful while the resource is still owned. No accounting structure grows dynamically.

The `apps` shell command shows all four slots with state, observed ring-3 CPU timer ticks, owned resource and awaited resource. The tick field is application CPU execution accounting, not wall-clock age. The `resources` command reports both TCP and UDP ownership, current hold, waiter count, acquisition count, total holding ticks since boot, longest hold and rolling ten-minute usage.

No administrative `abort` command is implemented. This is deliberate: an application cannot retain a resource indefinitely under the current model because every slot has a bounded execution deadline and timeout already follows the normal automatic resource-reclaim path. A separate abort mechanism would duplicate recovery behavior without a demonstrated need.


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

## UDP model

gmker 3.1 adds the smallest UDP mechanism required by a concrete gmapp: one bounded client exchange. It is not a socket subsystem. There is no bind API, listener, server endpoint table, raw IPv4 access or persistent application UDP endpoint.

`gm_udp_exchange()` sends one datagram to an explicit IPv4 address and destination port from a temporary source port in the dynamic range, then waits for one matching reply from that exact peer. The kernel validates the UDP length and checksum, bounds payloads to `GM_UDP_MAX` (1024 bytes), and times out the exchange. Only one UDP exchange can be active because application access is serialized by `GM_RESOURCE_UDP`.

The maintained UDP users are `gmapps/ntp.c` and `gmapps/dns.c`. Both protocols remain entirely application-level: the kernel knows only bounded UDP exchange. NTP is invoked with an explicit server address:

```text
run ntp 129.6.15.28
run dns 172.64.32.162 example.com
```

It sends a 48-byte NTP client request to UDP port 123, validates the basic server reply, converts the transmit timestamp to Unix time and prints UTC. No DNS server or NTP server is hardcoded in the kernel.

The DNS gmapp is deliberately non-recursive and explicit:

```text
run dns DNS_SERVER_IP name
```

It sends one standard UDP/53 `A IN` query with `RD=0`, so it never asks the selected server to recurse. The gmapp validates the transaction ID and DNS response flags, rejects truncated/error replies, understands DNS compression pointers and prints `A` and `CNAME` answer records with TTL. It has no cache, resolver configuration, EDNS, DNSSEC, TCP fallback, server role or iterative referral chasing. Querying an authoritative server therefore provides a direct non-recursive lookup; querying a recursive resolver with `RD=0` may return only information that resolver already has available.

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

A typical launch is:

```text
> run random
started slot 0 /programs/x86_64/random.gm
>

GMSTORE load -> validate GM01 -> private slot arena -> READY
                                              |
                              kernel round-robin scheduler
                                              |
                                      ring 3 execution
                                              |
                         service / timer -> kernel loop
                                              |
                              return / fault / timeout
```

`run` is asynchronous: after loading and preparing a free slot it returns immediately to the shell. Application output may therefore appear between shell output or output from other applications. Each individual kernel service is serialized, but separate service calls from different applications may interleave.

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

The current contract is GM API version 2:

```text
0   exit
1   write serial string
2   print u64
3   ticks
4   gateway address
5   ping
6   GMSTORE read
7   GMSTORE write
8   GMSTORE append
9   exclusive resource acquire
10  exclusive resource release
11  GMSTORE stat
12  one-shot UDP exchange
```

GM API version 2 is an intentional semantic break from version 1. Services 6-8 retain their numbers, but application GMSTORE access now requires explicit `GM_RESOURCE_TCP` ownership; executing an API-1 image with API-2 semantics would therefore be incorrect. The loader rejects API-1 GM01 images rather than carrying a compatibility path or two application execution models.

`STORE_STAT` was added because a real gmapp (`storecat`) needs to distinguish an empty object or EOF from read failure. It returns success separately and writes the 64-bit object size through a validated user pointer. `STORE_READ` keeps its compact byte-count-or-zero behavior and is not changed merely to make the interface more general.

Every pointer supplied by a program is range-checked before the kernel dereferences it. GMSTORE read/write services accept at most one 1024-byte block per call; programs can loop when they need larger objects. Application GMSTORE services require TCP ownership, while the UDP exchange requires UDP ownership. Kernel control-plane GMSTORE operations remain independent of application ownership. Service 12 is an additive API-2 extension, so existing API-2 gmapps remain valid.

The ABI deliberately has no general IPC, shared-memory synchronization, mutex or semaphore service. Application isolation plus kernel resource ownership remains the complete inter-application coordination model.

A program can therefore save persistent output directly, for example:

```c
#include "gmprog.h"

int gm_main(const char *args,uint64_t arg_len) {
  static const char path[]="/results/example.txt";
  static const char text[]="result from gm\n";

  (void)args;
  (void)arg_len;
  if (!gm_resource_acquire(GM_RESOURCE_TCP)) return 1;
  if (!gm_store_write(path,text,sizeof(text)-1U)) {
    (void)gm_resource_release(GM_RESOURCE_TCP);
    return 2;
  }
  if (!gm_resource_release(GM_RESOURCE_TCP)) return 3;
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
gmapps/diag.c        exercise core/store services including TCP ownership
gmapps/storecat.c    acquire TCP, print a GMSTORE text object, then release TCP
gmapps/netping.c     parse an IPv4 argument and ping it
gmapps/ntp.c         acquire UDP and read UTC from an explicit NTP server IPv4 address
gmapps/dns.c         issue one non-recursive A/IN DNS query to an explicit server
```

Build outputs are deployed below `store/programs/x86_64/`. Example invocations are:

```text
run primes100
run diag hello
run storecat /diag.txt
run netping 10.0.2.2
run ntp 129.6.15.28
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
apps
resources
run NAME [ARGS]
every SECONDS COUNT NAME [ARGS]
periodics
cancel ID
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
- `net.c` - Ethernet, ARP, IPv4, ICMP and bounded one-shot UDP;
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
- valid GM01 API 2 load and ring-3 execution;
- rejection of a deliberately downgraded GM01 API 1 image;
- private-address-space isolation at the same application virtual address;
- rejection of ring-3 access to the high-half kernel mapping;
- service entry after a user program sets the x86 Direction Flag;
- recoverable invalid-opcode fault;
- recoverable guard-page page fault;
- explicit TCP ownership for application GMSTORE services;
- rejection of UDP exchange without UDP ownership;
- bounded UDP one-shot request/reply through QEMU user-network NAT;
- independent UDP resource accounting as the second named exclusive resource;
- rejection of application GMSTORE access without TCP ownership;
- FIFO TCP ownership contention between two applications;
- automatic resource reclaim after application return, fault and timeout;
- live `apps` state and `resources` ownership/accounting during and after contention;
- `STORE_STAT` distinction for empty, missing and non-empty objects;
- kernel GMSTORE control-plane use while TCP is application-owned;
- rejection of a corrupted GM01 image;
- ring-3 execution timeout and recovery;
- simultaneous execution of two isolated applications under round-robin scheduling;
- simultaneous pure CPU-bound applications preempted only by the timer;
- bounded periodic application launch with exact finite launch count;
- deferred periodic launch without overlapping a still-running prior instance;
- automatic removal after the final periodic execution completes;
- cancellation of future periodic launches while an already-running gmapp completes normally;
- serial shell and network responsiveness while applications are running;
- GMSTORE loss while running, shell survival, reconnect and persistent-data recovery;
- clean QEMU shutdown through `isa-debug-exit`.

A successful run ends with:

```text
===== OK GMKER 3.2 CONSOLIDATION TEST =====
```

The final clean-build x86_64 kernel image reports:

```text
text   41385
data     224
bss    40304
total  81913 bytes
```

This is the consolidation baseline for gmker 3.2 on the current x86_64 QEMU target. Additional architectures, physical NIC drivers, protocol features or execution models are future evolution rather than requirements for this baseline.
