# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project context

This is an academic Operating Systems project (FMATCOM/UH) built on top of the MINIX 3 source tree (which is itself derived from NetBSD). The work consists of targeted modifications to MINIX rather than building a full OS from scratch. Three milestones were implemented:
1. Customized `/etc/motd` welcome message.
2. Bug fix in `pthread_mutex_trylock` (recursive call → delegate to `mthread`) and a new `tree` command.
3. CPU-bound process penalty mechanism in the user-space scheduler.

## Build system

MINIX uses BSD `make` (the `make` binary inside the VM is `bmake`-compatible). The top-level `Makefile` follows the NetBSD release engineering model.

Build the entire system (run inside the MINIX VM):
```sh
make build DESTDIR=/
```

Build a single subdirectory (e.g., the scheduler server):
```sh
cd minix/servers/sched && make
```

Build the `tree` command:
```sh
cd bin/tree && make
```

Install a single component after building:
```sh
cd minix/servers/sched && make install
```

Rebuild and test the report (requires TeX Live):
```sh
cd docs/report && pdflatex report.tex
```

There is no automated test runner for the kernel/server code; validation is done manually inside the VM.

## Architecture overview

MINIX is a **microkernel OS**. Almost everything runs as a user-space server process:

| Layer | Location | Role |
|---|---|---|
| Kernel | `minix/kernel/` | Minimal kernel: process table, IPC, interrupts, clock, memory |
| Process Manager | `minix/servers/pm/` | `fork`/`exec`/`wait`, signals, nice/priority |
| Scheduler | `minix/servers/sched/` | User-space scheduling policy (this is where the CPU-bound penalty lives) |
| VFS | `minix/servers/vfs/` | Virtual filesystem multiplexer |
| RS | `minix/servers/rs/` | Reincarnation Server — starts, monitors and restarts system services |
| DS | `minix/servers/ds/` | Data Store — publish/subscribe for system-wide state |
| VM | `minix/servers/vm/` | Virtual memory management |
| Drivers | `minix/drivers/` | Each driver is a separate user-space process |
| Compat libs | `minix/lib/libmthread/` | `mthread_*` primitives; `pthread_compat.c` maps `pthread_*` → `mthread_*` |
| User commands | `bin/`, `minix/commands/` | Standard and MINIX-specific userland tools |

IPC between all components uses Minix message passing (`sys_send`, `sys_receive`, `_taskcall`).

## Key modified files

| File | What changed |
|---|---|
| `etc/motd` | Custom welcome message |
| `bin/tree/tree.c` | New command: recursive directory tree printer using `opendir`/`readdir`/`lstat` |
| `bin/tree/Makefile` | Build file for `tree` |
| `minix/lib/libmthread/pthread_compat.c` | Fixed `pthread_mutex_trylock` which was calling itself recursively instead of `mthread_mutex_trylock` |
| `minix/servers/sched/schedproc.h` | Added `base_priority`, `quantum_count`, `penalty_level` fields to `struct schedproc` |
| `minix/servers/sched/schedule.c` | CPU-bound penalty logic in `do_noquantum()`, `do_start_scheduling()`, and `balance_queues()` |

## CPU-bound penalty design (scheduler)

- `do_noquantum()` increments `quantum_count` each time a non-system process exhausts its quantum.
- `balance_queues()` runs every `BALANCE_TIMEOUT = 5` seconds:
  - If `quantum_count >= CPU_BOUND_QUANTUM_THRESHOLD (3)`: increment `penalty_level` (up to `MAX_PENALTY_LEVEL = 2`), lower priority by `penalty_level` relative to `base_priority`.
  - If `quantum_count == 0` and `penalty_level > 0`: decrement `penalty_level` (gradual recovery).
  - Reset `quantum_count` each window.
- Priority numbers in MINIX are inverted: lower number = higher priority. `MIN_USER_Q` is the lowest allowed user priority (highest queue number).

## Documentation

The technical report is a LaTeX document at `docs/report/report.tex`. Compile with `pdflatex`. The PDF and auxiliary files are tracked in git. Reference documents from the course are in `docs/orientation/`.
