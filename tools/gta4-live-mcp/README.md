# gta4-live-mcp

An MCP server (stdio, Python standard library only) for looking into the
running game: guest memory, cvars and the log. It is the client of the game's
debug server (`glue/rexglue-sdk-main/gta4-recomp/src/devtools/gta4_debug_server.cpp`).

The game sends raw guest bytes; decoding (big-endian values, pointer chains,
dumps) happens here, so tools can be added without rebuilding the game.

## In the game (Windows)

Add to `Documents\LibertyRecomp\native.toml`, before the first `[section]`:

```toml
gta4_debug_server_port = 5050
# gta4_debug_server_allow_write = true   # only if writing memory is wanted
```

The log then shows `gta4-debug-server: listening on 127.0.0.1:5050`. The server
listens on loopback only.

## The tunnel

From the Windows PC, forward the game's port to the machine running Claude Code:

```
ssh -N -R 5050:127.0.0.1:5050 user@that-machine
```

## Claude Code

```
claude mcp add gta4-live -e GTA4_LIVE_PORT=5050 -- python3 -I /path/to/tools/gta4-live-mcp/server.py
```

## Tools

| Tool | Does |
|---|---|
| `game_info` | Reachability, where guest memory is mapped |
| `read_memory` | Hexdump, ASCII / UTF-16BE string, or big-endian values (`u8`…`f64`, `count`) |
| `follow_pointers` | Follows 32-bit guest pointers through a list of offsets, dumps the end |
| `find_bytes` | Searches mapped memory for hex, text or a u32/s32/f32/u16 value |
| `write_memory` | Writes hex or values (needs `gta4_debug_server_allow_write`), shows before/after |
| `cvar_list` / `cvar_get` / `cvar_set` | The game's settings, live (`cvar_set` is not saved to `native.toml`) |
| `log_tail` | The newest log's last lines, optionally filtered |

Addresses are guest addresses, as in the generated code and the hooks
(`0x82000000` and up is the executable image).
