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
| `screenshot` | The frame the game shows, as an image (both games) |
| `scene` / `teleport` / `set_time` / `set_weather` (PC) | Player, camera, time and weather through script natives (not while paused or in a menu) |
| `cpu_scopes` (PC) | FusionFix's own CPU work in ms a frame by scope and thread, frame time percentiles |
| `bench_spot` (PC) | Save, list or delete named places (player, camera, time, weather) to bench at |
| `bench` (PC) | Freeze, then measure GPU passes, draws, frame times, thread load and CPU scopes, optionally A/B over an ini key in alternating rounds; verdict of what holds the frame back; saved for baselines |
| `freeze` (PC) | A still scene for comparing frames: script camera at the game camera (or a given place), player frozen, clock stopped, weather held, cloud drift stopped, world slowed by `time_scale` (default 0.01); `on: false` releases |
| `files` (PC) | FusionFix's own files next to the plugin (logs, capture pictures, .folded); with `name`, copied to this machine (`GTA4_LIVE_FILES`, default ~/gta4-live-files) |
| `cpu_profile` (PC) | Where the threads spend their CPU time over some seconds (FusionFix's sampling profiler, on for the request): CPU per thread, busiest functions self/total, most common stacks, longest frames |
| `profiler` (PC) | GPU time per pass over the next 120 frames (FusionFix's PostFxProfiler, on for the request): FusionFix's passes, then the game's by render target |
| `watch` (PC) | Samples values (addresses or pointer chains) each frame into a buffer in the game: how they change over time; `changes_only`, `every`, `on: game` |
| `trace` (PC, allow-write) | Records each hit of an instruction (`mode: hook`, a mid-hook, or `execute`, a debug register) or of memory (`write` / `access`, data breakpoints): registers, callers on the stack, reads through registers, a `when` filter |
| `trace_read` / `trace_stop` / `trace_list` (PC) | A watch's summary and table, a trace's hits, callers by count and records; ending them; what runs |
| `shader_manifest` (recomp) | Find shader override entries by hash or source |
| `shader_override_push` (recomp) | Compile entries here and send them as live overrides |
| `shader_override_list` / `shader_override_remove` (recomp) | The live overrides the game holds |
| `restart_game` (recomp) | Restart the game; live overrides and restart-only settings apply |

Traces patch code (hooks) or set the debug registers of every thread, so they
need `DebugServerAllowWrite`. A hook's address must start an instruction: the
game checks it by decoding from the padding before the function (or from
`func`, the function's start). Up to 8 hooks and 4 debug registers at once.
Hooks and data breakpoints don't stop the game; each hit costs a few
microseconds, so `when` and `count` keep hot places cheap.

Recomp addresses are guest addresses, as in the generated code and the hooks
(`0x82000000` and up is the executable image); PC addresses are the game
process's own.

## Live shader overrides (recomp)

`shader_override_push` compiles manifest entries with
`glue/rexglue-sdk-main/tools/gta4_shader_override_compiler.py --binary-dir`,
using DXC 1.9, `spirv-val` and `glslangValidator` from
`~/.cache/gta4-shader-tools` (or `GTA4_SHADER_TOOLS`): `dxc/bin/dxc`,
`dxc/lib/libdxcompiler.so`, `spirv-val`, `glslangValidator`. The game keeps
them in `shader_overrides_live/` next to the exe and uses them from its next
start, in place of the built-in entries of the same stage and hash: edit the
HLSL, push, `restart_game`, `screenshot`. A rebuild is needed only to make a
change permanent for everyone.
