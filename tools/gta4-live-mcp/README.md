# gta4-live-mcp

An MCP server (stdio, Python standard library only) for looking into a
running game from another machine. It is the client of two debug servers that
speak the same protocol:

- the recomp's (`glue/rexglue-sdk-main/gta4-recomp/src/devtools/gta4_debug_server.cpp`):
  guest memory (big-endian), cvars, the log;
- FusionFix's, in the PC game (`source/debugserver.ixx` in FusionFix):
  process memory (little-endian), modules, signatures, the ini, the logs.

`GTA4_LIVE_TARGET` picks one: `recomp` (default) or `pc`.

The games send raw bytes in memory order; decoding (values in the target's
byte order, pointer chains, dumps) happens here, so tools can be added without
rebuilding either game.

## In the recomp (Windows)

Add to `Documents\LibertyRecomp\native.toml`, before the first `[section]`:

```toml
gta4_debug_server_port = 5050
# gta4_debug_server_allow_write = true   # only if writing memory is wanted
```

The log then shows `gta4-debug-server: listening on 127.0.0.1:5050`. The server
listens on loopback only.

## In the PC game (FusionFix)

In `plugins\GTAIV.EFLC.FusionFix.ini`, `[DEBUG]`:

```ini
DebugServerPort = 5051
DebugServerAllowWrite = 0     ; 1 to write memory (code too, with force)
```

`GTAIV.EFLC.FusionFix.DebugServer.log` then shows `listening on 127.0.0.1:5051`.

## The tunnel

From each Windows PC, forward the game's port to the machine running Claude
Code (one command can carry both when they run on the same PC):

```
ssh -N -R 5050:127.0.0.1:5050 user@that-machine     # the recomp
ssh -N -R 5051:127.0.0.1:5051 user@that-machine     # the PC game
```

## Claude Code

```
claude mcp add gta4-live -e GTA4_LIVE_PORT=5050 -- python3 -I /path/to/server.py
claude mcp add gtaiv-pc-live -e GTA4_LIVE_PORT=5051 -e GTA4_LIVE_TARGET=pc -- python3 -I /path/to/server.py
```

## Tools

| Tool | Does |
|---|---|
| `game_info` | Reachability, where memory is (recomp: guest memory; PC: exe and plugin bases) |
| `read_memory` | Hexdump, ASCII / UTF-16BE string, or values in the target's byte order (`u8`…`f64`, `count`) |
| `follow_pointers` | Follows 32-bit pointers through a list of offsets, dumps the end |
| `find_bytes` | Searches mapped memory for hex, text or a u32/s32/f32/u16 value |
| `write_memory` | Writes hex or values (needs the allow-write switch), shows before/after; PC: `force` for code |
| `cvar_list` / `cvar_get` / `cvar_set` (recomp) | The game's settings, live (`cvar_set` is not saved to `native.toml`) |
| `log_tail` | The newest log's last lines, optionally filtered (PC: `feature` picks a FusionFix log) |
| `modules` (PC) | Loaded modules with base and size |
| `find_signature` (PC) | A signature with wildcards, as FusionFix's hooks write them |
| `ini_get` / `ini_set` (PC) | The FusionFix ini; `ini_set` has it read again at the next frame |

Recomp addresses are guest addresses, as in the generated code and the hooks
(`0x82000000` and up is the executable image); PC addresses are the game
process's own.
