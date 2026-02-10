# Command Protocol

Commands are sent via TCP socket (one command per connection) or `--cmd`.
All responses are single-line JSON. Errors return `{"ok":false,"error":"message"}`.

## Commands

| Command | Description | Response |
|---------|-------------|----------|
| `info` | Core name, resolution, debug capabilities | `{"ok":true,"core":"SameBoy","width":160,...}` |
| `content` | Content info (mapper, title, checksums, etc.) | `{"ok":true,"info":"Title: ...\\nMapper: ..."}` |
| `run [N]` | Run N frames (default 1, max 10000). Returns `"breakpoint":ID` if a breakpoint/watchpoint hit, plus `"blocked":true` if the core thread is blocked mid-frame (save/load unavailable). Auto-resumes from a previous blocked state. | `{"ok":true,"frames":N}` |
| `input <button> <0\|1>` | Press (1) or release (0) a button | `{"ok":true}` |
| `peek <addr> [len]` | Read bytes from memory (retrodebug) | `{"ok":true,"addr":"0x1234","data":[...]}` |
| `poke <addr> <byte>...` | Write bytes to memory | `{"ok":true,"written":N}` |
| `reg` | Dump all CPU registers | `{"ok":true,"registers":{"a":0,"f":0,...}}` |
| `reg <name>` | Read one register | `{"ok":true,"pc":256}` |
| `reg <name> <value>` | Set a register | `{"ok":true}` |
| `save <slot>` | Save state to slot 0-9 | `{"ok":true,"slot":N}` |
| `load <slot>` | Load state from slot 0-9 (doesn't update screen immediately.) | `{"ok":true,"slot":N}` |
| `statehash` | CRC32 hash of serialized save state (for determinism checks) | `{"ok":true,"hash":"ABCD1234","size":N}` |
| `screen [path]` | Save frame as PNG (default: `screenshot.png`) | `{"ok":true,"width":160,"height":144,"path":"screenshot.png"}` |
| `regions` | List all memory regions | `{"ok":true,"regions":[{"id":"...","description":"...","base_address":"0x0","size":65536,"has_mmap":true},...]}` |
| `dump <id> [start size [path]]` | Hex dump of memory region (to TCP or file) | Text hex dump, or `{"ok":true,"path":"..."}` if file |
| `dis [cpu] [region.]<start>-<end>` | Disassemble address range (hex, no `0x`) | Text disassembly listing |
| `search reset <region> [size] [align]` | Start new value search in memory region | `{"ok":true,"candidates":N}` |
| `search filter <op> <value\|p>` | Filter candidates (eq/ne/lt/gt/le/ge, `p` = vs previous) | `{"ok":true,"candidates":N}` |
| `search list [max]` | List search results (default max 100) | `{"ok":true,"candidates":N,"results":[...]}` |
| `search count` | Count remaining candidates | `{"ok":true,"candidates":N}` |
| `cpu` | List available CPUs | `{"ok":true,"cpus":[{"id":"lr35902","description":"...","primary":true}]}` |
| `bp add [cpu.]<addr> [flags] [cond]` | Add breakpoint (optional CPU prefix, flags: X/R/W combo + T for temporary, default X). T = auto-delete on first hit | `{"ok":true,"id":1}` |
| `bp delete <id>` | Delete breakpoint by ID | `{"ok":true}` |
| `bp enable <id>` | Enable breakpoint | `{"ok":true}` |
| `bp disable <id>` | Disable breakpoint | `{"ok":true}` |
| `bp list` | List all breakpoints (includes `cpu`, `temporary` fields) | `{"ok":true,"breakpoints":[...]}` |
| `bp clear` | Delete all breakpoints | `{"ok":true}` |
| `bp save [path]` | Save breakpoints to file (default: `<rom>.bp`) | `{"ok":true,"path":"..."}` |
| `bp load [path]` | Load breakpoints from file (default: `<rom>.bp`) | `{"ok":true,"path":"...","count":N}` |
| `s` | Single-step one instruction (step into). Auto-resumes from blocked state. | `{"ok":true,"frames":N}` |
| `so` | Step over (execute one instruction, stepping over JSR/calls) | `{"ok":true,"frames":N}` |
| `sout` | Step out (run until current subroutine returns) | `{"ok":true,"frames":N}` |
| `trace on [path]` | Start execution trace (optionally to file) | `{"ok":true,"tracing":true}` |
| `trace off` | Stop execution trace | `{"ok":true,"tracing":false,"lines":N}` |
| `trace status` | Query trace state | `{"ok":true,"tracing":...,"lines":N,...}` |
| `trace cpu <name> on\|off` | Enable/disable tracing a CPU | `{"ok":true,"cpu":"...","enabled":...}` |
| `trace registers on\|off` | Toggle register state in trace output | `{"ok":true,"registers":...}` |
| `trace indent on\|off` | Toggle SP-based indentation | `{"ok":true,"indent":...}` |
| `reset` | Reset emulated system | `{"ok":true}` |
| `manual on\|off` | Enable/disable keyboard input | `{"ok":true,"manual":true}` |
| `display on\|off` | Show/close SDL display window | `{"ok":true,"display":true}` |
| `sound on\|off` | Enable/disable audio output | `{"ok":true,"sound":true}` |
| `sym label get <addrspec>` | Get label at address (resolves through memory maps) | `{"ok":true,"label":"name"}` or `{"ok":true,"label":null}` |
| `sym label set <addrspec> <label>` | Set label (must match `[a-zA-Z_][a-zA-Z0-9_]*`) | `{"ok":true}` |
| `sym label delete <addrspec>` | Delete label | `{"ok":true}` |
| `sym comment get <addrspec>` | Get comment at address | `{"ok":true,"comment":"text"}` or `{"ok":true,"comment":null}` |
| `sym comment set <addrspec> <text>` | Set comment (free-form text) | `{"ok":true}` |
| `sym comment delete <addrspec>` | Delete comment | `{"ok":true}` |
| `sym list` | List all symbols | `{"ok":true,"symbols":[...]}` |
| `quit` | Clean shutdown | `{"ok":true}` |

## Button Names

`up`, `down`, `left`, `right`, `a`, `b`, `start`, `select`, `x`, `y`, `l`, `r`

## Address Spec (`<addrspec>`)

The `sym` commands use a unified address specifier:

| Format | Example | Meaning |
|--------|---------|---------|
| `<hex_addr>` | `0100` | Bare hex address, defaults to primary CPU memory region |
| `<bank>:<hex_addr>` | `e:4000` | Banked address, defaults to primary CPU memory region |
| `<region>.<hex_addr>` | `ram.0100` | Hex address in named region |
| `<region>.<bank>:<hex_addr>` | `ram.e:4000` | Banked address in named region |

Both bank and address are hexadecimal. Addresses are resolved through memory maps to
the deepest backing region before storage. Banked addresses use `get_bank_address` to
map a specific bank before resolution.

## Addresses

Addresses can be decimal or hex (`0x` prefix). Memory space is 0x0000-0xFFFF.

Key Game Boy addresses:
- `0xFF44` - LY (current scanline)
- `0xFF40` - LCDC (LCD control)
- `0xFF00` - Joypad register
- `0xC000-0xDFFF` - Work RAM

## Example Session

```bash
# Start server in background
./arret-qt --headless ../libretro-sameboy/libretro/sameboy_libretro.so ../roms/base.gb &

# Send commands
./arret-qt --cmd "info"       # {"ok":true,"core":"SameBoy","width":160,...}
./arret-qt --cmd "run 60"     # {"ok":true,"frames":60}
./arret-qt --cmd "reg"        # {"ok":true,"registers":{"a":0,"f":128,...}}
./arret-qt --cmd "screen"     # {"ok":true,"width":160,"height":144,"path":"screenshot.png"}
./arret-qt --cmd "quit"       # {"ok":true}
```
