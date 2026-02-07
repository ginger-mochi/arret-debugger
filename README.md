# Arrêt Debugger: AI Agent Guide

A libretro frontend designed for AI agent control of emulated games. Has both a Qt frontend, and a stripped-down SDL frontend.

## Quick Start

```bash
# Build
cd arret-debugger && make

# Run headless (AI mode)
./arret-qt --headless ../libretro-sameboy/libretro/sameboy_libretro.so ../roms/base.gb

# Run headed (human verification)
./arret-qt ../libretro-sameboy/libretro/sameboy_libretro.so ../roms/base.gb
```

## Command Line Options

```
arret-qt [options] <core.so> <rom>
arret-qt --cmd "command" [--port N]

Options:
  --headless          No display, TCP-only control
  --mute              Start with audio disabled
  --system-dir DIR    System/BIOS directory (default: .)
  --scale N           Window scale factor (default: 3)
  --port N            TCP command port (default: 2783)
  --cmd "command"     Send command to running instance and exit
```

## TCP Socket Interface

Arrêt Debugger always listens on a TCP port (default 2783) for commands. This allows
external processes to send commands one at a time. This is useful for a gdb-like interface,
or to allow agents to automate debugging and reversing tasks.

```bash
# Terminal 1: start arret-debugger
./arret-qt --headless ../libretro-sameboy/libretro/sameboy_libretro.so ../roms/base.gb

# Terminal 2: send commands via --cmd
./arret-qt --cmd "info"
./arret-qt --cmd "run 60"
./arret-qt --cmd "screen"
./arret-qt --cmd "quit"
```

The `--cmd` mode connects to localhost on the configured port, sends the command,
prints the JSON response to stdout, and exits. Exit code 0 on success, 1 on
connection failure.

See [CMD.md](CMD.md) for the full command reference.
