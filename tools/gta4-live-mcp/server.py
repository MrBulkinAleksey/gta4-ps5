#!/usr/bin/env python3
"""MCP server (stdio) for looking into the running GTA IV recomp.

It talks to the game's debug server (gta4-recomp/src/devtools/gta4_debug_server.cpp),
which listens on 127.0.0.1 inside the game when gta4_debug_server_port is set.
The game usually runs on another machine: forward its port here, for example
from the Windows PC

    ssh -R 5050:127.0.0.1:5050 user@this-machine

Environment: GTA4_LIVE_HOST (default 127.0.0.1), GTA4_LIVE_PORT (default 5050),
GTA4_LIVE_TARGET: "recomp" (default) or "pc" for the PC game with FusionFix's
debug server (source/debugserver.ixx in FusionFix: [DEBUG] DebugServerPort).

The game sends raw bytes in memory order (big-endian guest memory in the recomp,
as on the Xbox 360; little-endian on the PC); the decoding of values, pointer
chains and dumps happens here, so tools can grow without rebuilding the game.
No dependencies beyond the standard library.
"""

import base64
import json
import os
import subprocess
import tempfile
import socket
import struct
import sys
import threading
from pathlib import Path

PROTOCOL_VERSION = "2025-06-18"
HOST = os.environ.get("GTA4_LIVE_HOST", "127.0.0.1")
PORT = int(os.environ.get("GTA4_LIVE_PORT", "5050"))
TIMEOUT = float(os.environ.get("GTA4_LIVE_TIMEOUT", "60"))
TARGET = os.environ.get("GTA4_LIVE_TARGET", "recomp")
if TARGET not in ("recomp", "pc"):
    sys.exit("GTA4_LIVE_TARGET is recomp or pc")
ENDIAN = "<" if TARGET == "pc" else ">"
GAME_NAME = "the PC game (FusionFix)" if TARGET == "pc" else "the recomp"
ENABLE_HINT = ("[DEBUG] DebugServerPort = {port} in GTAIV.EFLC.FusionFix.ini" if TARGET == "pc"
               else "gta4_debug_server_port = {port} in native.toml")

# Value formats: struct code (in the target's byte order) and size.
FORMATS = {name: (ENDIAN + code, size) for name, (code, size) in {
    "u8": ("B", 1), "s8": ("b", 1),
    "u16": ("H", 2), "s16": ("h", 2),
    "u32": ("I", 4), "s32": ("i", 4),
    "u64": ("Q", 8), "s64": ("q", 8),
    "f32": ("f", 4), "f64": ("d", 8),
}.items()}


class GameError(Exception):
    pass


class Game:
    """One connection to the debug server, reopened when it drops."""

    def __init__(self):
        self.lock = threading.Lock()
        self.sock = None
        self.file = None
        self.next_id = 0

    def _connect(self):
        try:
            self.sock = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
        except OSError as error:
            raise GameError(
                f"cannot reach {GAME_NAME} at {HOST}:{PORT} ({error}). Is it running with "
                f"{ENABLE_HINT.format(port=PORT)}, and the SSH tunnel up?")
        self.file = self.sock.makefile("rb")

    def _close(self):
        for thing in (self.file, self.sock):
            try:
                if thing:
                    thing.close()
            except OSError:
                pass
        self.sock = self.file = None

    def call(self, cmd, **args):
        with self.lock:
            for attempt in (0, 1):
                if not self.sock:
                    self._connect()
                self.next_id += 1
                request = dict(args, cmd=cmd, id=self.next_id)
                try:
                    self.sock.sendall((json.dumps(request) + "\n").encode())
                    line = self.file.readline()
                    if not line:
                        raise OSError("connection closed")
                except OSError as error:
                    self._close()
                    if attempt:
                        raise GameError(f"lost the game connection: {error}")
                    continue
                reply = json.loads(line)
                if not reply.get("ok"):
                    raise GameError(reply.get("error", "unknown error"))
                return reply


GAME = Game()


def parse_int(value):
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def read_bytes(addr, size):
    out = bytearray()
    while size > 0:
        chunk = min(size, 1 << 20)
        out += bytes.fromhex(GAME.call("read", addr=f"0x{addr:X}", size=chunk)["hex"])
        addr += chunk
        size -= chunk
    return bytes(out)


def hexdump(addr, data, width=16):
    lines = []
    for offset in range(0, len(data), width):
        row = data[offset:offset + width]
        words = " ".join(row[i:i + 4].hex() for i in range(0, len(row), 4))
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        lines.append(f"{addr + offset:08X}  {words:<{width * 2 + width // 4}} {text}")
    return "\n".join(lines)


# ---- tools ---------------------------------------------------------------

def tool_game_info(_):
    return json.dumps({k: v for k, v in GAME.call("info").items() if k not in ("ok", "id")}, indent=1)


def tool_read_memory(args):
    addr = parse_int(args["addr"])
    fmt = args.get("format", "dump")
    if fmt == "dump":
        size = parse_int(args.get("size", 256))
        return hexdump(addr, read_bytes(addr, size))
    if fmt in ("ascii", "utf16be"):
        size = parse_int(args.get("size", 256))
        data = read_bytes(addr, size)
        if fmt == "ascii":
            return data.split(b"\0", 1)[0].decode("latin-1")
        text = data.decode("utf-16-be", errors="replace")
        return text.split("\0", 1)[0]
    if fmt not in FORMATS:
        raise GameError(f"format is dump, ascii, utf16be or one of {', '.join(FORMATS)}")
    code, width = FORMATS[fmt]
    count = parse_int(args.get("count", 1))
    data = read_bytes(addr, width * count)
    values = [struct.unpack_from(code, data, i * width)[0] for i in range(count)]
    shown = [f"0x{v:X}" if fmt.startswith("u") and width >= 4 else repr(v) for v in values]
    return "\n".join(f"{addr + i * width:08X}  {s}" for i, s in enumerate(shown))


def tool_follow_pointers(args):
    addr = parse_int(args["base"])
    steps = [f"start {addr:08X}"]
    for offset in [parse_int(o) for o in args.get("offsets", [])]:
        pointer = struct.unpack(ENDIAN + "I", read_bytes(addr, 4))[0]
        steps.append(f"[{addr:08X}] = {pointer:08X}, + 0x{offset:X} = {pointer + offset:08X}")
        if not pointer:
            steps.append("null pointer: stopped")
            return "\n".join(steps)
        addr = (pointer + offset) & 0xFFFFFFFF
    size = parse_int(args.get("size", 64))
    steps.append(hexdump(addr, read_bytes(addr, size)))
    return "\n".join(steps)


def pattern_bytes(args):
    if "hex" in args:
        return bytes.fromhex(str(args["hex"]).replace(" ", ""))
    if "text" in args:
        return str(args["text"]).encode("latin-1")
    for fmt in ("u32", "s32", "f32", "u16"):
        if fmt in args:
            value = float(args[fmt]) if fmt == "f32" else parse_int(args[fmt])
            return struct.pack(FORMATS[fmt][0], value)
    raise GameError("give hex, text, u32, s32, f32 or u16")


def tool_find_bytes(args):
    request = {"hex": pattern_bytes(args).hex(), "max_results": parse_int(args.get("max_results", 50))}
    for key in ("start", "end"):
        if key in args:
            request[key] = f"0x{parse_int(args[key]):X}"
    reply = GAME.call("find", **request)
    matches = reply["matches"]
    head = f"{len(matches)} match(es){' (truncated)' if reply.get('truncated') else ''}"
    return "\n".join([head] + matches)


def tool_write_memory(args):
    addr = parse_int(args["addr"])
    if "hex" in args:
        data = bytes.fromhex(str(args["hex"]).replace(" ", ""))
    else:
        fmt = args.get("format", "u32")
        if fmt not in FORMATS:
            raise GameError(f"format is one of {', '.join(FORMATS)}")
        values = args["values"] if isinstance(args.get("values"), list) else [args["value"]]
        code = FORMATS[fmt][0]
        data = b"".join(struct.pack(code, float(v) if fmt.startswith("f") else parse_int(v))
                        for v in values)
    before = read_bytes(addr, len(data))
    extra = {"force": True} if args.get("force") else {}
    GAME.call("write", addr=f"0x{addr:X}", hex=data.hex(), **extra)
    return f"wrote {len(data)} bytes at {addr:08X}\nbefore: {before.hex()}\nafter:  {data.hex()}"


def tool_cvar_list(args):
    reply = GAME.call("cvar_list", filter=args.get("filter", ""))
    return "\n".join(
        f"{c['name']} = {c['value']}" + (f"  [{c.get('category', '')}] {c.get('description', '')}"
                                         if args.get("verbose") else "")
        for c in reply["cvars"]) or "no cvars match"


def tool_cvar_get(args):
    reply = GAME.call("cvar_get", name=args["name"])
    return json.dumps({k: v for k, v in reply.items() if k not in ("ok", "id")}, indent=1)


def tool_cvar_set(args):
    reply = GAME.call("cvar_set", name=args["name"], value=str(args["value"]))
    note = " (takes effect after a restart)" if reply.get("requires_restart") else ""
    return f"{reply['name']} = {reply['value']}{note}"


def tool_log_tail(args):
    lines = parse_int(args.get("lines", 200))
    grep = args.get("grep")
    # Fetch more when filtering, so the filter still has lines to choose from.
    extra = {"feature": args["feature"]} if args.get("feature") else {}
    reply = GAME.call("log_tail", lines=5000 if grep else lines, **extra)
    found = reply["lines"]
    if grep:
        needle = grep.lower()
        found = [l for l in found if needle in l.lower()][-lines:]
    return f"{reply['file']}\n" + "\n".join(found)


def tool_screenshot(args):
    reply = GAME.call("screenshot", max_width=parse_int(args.get("max_width", 1600)))
    note = f"{reply['width']}x{reply['height']}, also saved in the game's folder as {reply['file']}"
    return [{"type": "image", "data": reply["data"], "mimeType": reply.get("mime", "image/png")},
            {"type": "text", "text": note}]


# ---- live shader overrides (recomp): compiled here, applied by the game at its next start

REPO = Path(__file__).resolve().parents[2]
SHADER_TOOLS = Path(os.environ.get("GTA4_SHADER_TOOLS", Path.home() / ".cache/gta4-shader-tools"))
OVERRIDES = REPO / "LibertyRecompLib/shader_overrides"
COMPILER = REPO / "glue/rexglue-sdk-main/tools/gta4_shader_override_compiler.py"


def tool_shader_manifest(args):
    needle = str(args.get("filter", "")).lower().removeprefix("0x")
    entries = json.loads((OVERRIDES / "manifest.json").read_text())["overrides"]
    rows = [f"{e['stage']:<6} {e['hash']}  {e['source']}" for e in entries
            if needle in e["hash"].lower() or needle in e["source"].lower()]
    return f"{len(rows)} of {len(entries)} entries\n" + "\n".join(rows[:200])


def tool_shader_override_push(args):
    wanted = args["only"] if isinstance(args["only"], list) else [args["only"]]
    with tempfile.TemporaryDirectory(prefix="gta4-live-ovr-") as out:
        command = [sys.executable, "-I", str(COMPILER),
                   "--manifest", str(OVERRIDES / "manifest.json"), "--source-root", str(OVERRIDES),
                   "--binary-dir", out,
                   "--dxc", str(SHADER_TOOLS / "dxc/bin/dxc"), "--dxc-library-path", str(SHADER_TOOLS / "dxc/lib"),
                   "--glslang-validator", str(SHADER_TOOLS / "glslangValidator"),
                   "--spirv-val", str(SHADER_TOOLS / "spirv-val")]
        for item in wanted:
            command += ["--only", str(item)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        if compiled.returncode:
            raise GameError("compile failed:\n" + (compiled.stderr or compiled.stdout)[-4000:])
        sent = []
        for file in sorted(Path(out).glob("*.ovr")):
            GAME.call("shader_override_put", name=file.name, data=base64.b64encode(file.read_bytes()).decode())
            sent.append(f"{file.name} ({file.stat().st_size} bytes)")
    return ("sent:\n" + "\n".join(sent) +
            "\napplied at the game's next start (restart_game)")


def tool_shader_override_list(_):
    reply = GAME.call("shader_override_list")
    rows = [f"{o['name']}  {o['bytes']} bytes" for o in reply["overrides"]]
    return f"{reply['dir']}\n" + ("\n".join(rows) or "none")


def tool_shader_override_remove(args):
    reply = GAME.call("shader_override_remove", name=args.get("name", "all"))
    return f"removed {reply['removed']} (applied at the next start)"


def tool_restart_game(_):
    reply = GAME.call("restart")
    GAME._close()
    return (f"restarting: the next instance is pid {reply['next_pid']}; the debug server is back "
            "once it has loaded (call game_info until it answers)")


WEATHERS = ["extra sunny", "sunny", "sunny windy", "cloudy", "rain", "drizzle", "foggy", "lightning",
            "extra sunny 2", "sunny windy 2"]


def tool_scene(_):
    reply = GAME.call("scene")
    weather = int(reply["weather"])
    name = WEATHERS[weather] if 0 <= weather < len(WEATHERS) else "?"
    return (f"player {reply['player']} heading {reply['heading']}{' (in a car)' if reply['in_car'] else ''}\n"
            f"camera {reply['camera']} rotation {reply['camera_rotation']}\n"
            f"time {reply['time']}, weather {weather} ({name})")


def tool_teleport(args):
    extra = {"heading": str(args["heading"])} if "heading" in args else {}
    reply = GAME.call("teleport", x=str(args["x"]), y=str(args["y"]), z=str(args["z"]), **extra)
    return f"player at {reply['player']}"


def tool_set_time(args):
    return "time " + GAME.call("set_time", hour=parse_int(args["hour"]), minute=parse_int(args.get("minute", 0)))["time"]


def tool_set_weather(args):
    weather = str(args["weather"])
    if weather.lower() in WEATHERS:
        weather = str(WEATHERS.index(weather.lower()))
    return "weather " + GAME.call("set_weather", weather=weather)["weather"]


def tool_modules(args):
    modules = GAME.call("modules")["modules"]
    needle = args.get("filter", "").lower()
    return "\n".join(f"{m['base']}  {int(m['size']):>10}  {m['name']}"
                     for m in modules if needle in m["name"].lower()) or "no module matches"


def tool_find_signature(args):
    request = {"pattern": args["pattern"], "max_results": parse_int(args.get("max_results", 20))}
    if args.get("module"):
        request["module"] = args["module"]
    reply = GAME.call("pattern", **request)
    return "\n".join([f"{reply['total']} match(es)"] + reply["matches"])


def tool_ini_get(args):
    reply = GAME.call("ini_get", section=args["section"], key=args["key"])
    return f"[{reply['section']}] {reply['key']} = {reply['value']}"


def tool_ini_set(args):
    reply = GAME.call("ini_set", section=args["section"], key=args["key"], value=str(args["value"]))
    return f"[{reply['section']}] {reply['key']} = {reply['value']} ({reply['note']})"


ADDR = {"type": ["integer", "string"], "description": (
    "Address in the game process, e.g. \"0x00C64CB0\"" if TARGET == "pc"
    else "Guest address, e.g. \"0x82000000\"")}
ORDER = "little-endian" if TARGET == "pc" else "big-endian"
MEMORY = "memory" if TARGET == "pc" else "guest memory"
WRITE_SWITCH = ("[DEBUG] DebugServerAllowWrite = 1 in the ini" if TARGET == "pc"
                else "gta4_debug_server_allow_write = true in native.toml")
TOOLS = [
    ("game_info", tool_game_info, f"Whether {GAME_NAME} is reachable, and where its memory is.", {}, []),
    ("read_memory", tool_read_memory,
     f"Read {MEMORY}. format: dump (hexdump, default), ascii, utf16be, or a {ORDER} value type "
     "u8/s8/u16/s16/u32/s32/u64/s64/f32/f64 with count.",
     {"addr": ADDR, "size": {"type": "integer", "description": "Bytes for dump/ascii/utf16be (default 256)"},
      "format": {"type": "string"}, "count": {"type": "integer", "description": "Values to read (default 1)"}},
     ["addr"]),
    ("follow_pointers", tool_follow_pointers,
     f"Follow a chain of 32-bit {ORDER} pointers: read the pointer at base, add the first offset, "
     "read there, and so on; dump size bytes at the end.",
     {"base": ADDR, "offsets": {"type": "array", "items": {"type": ["integer", "string"]}},
      "size": {"type": "integer"}}, ["base"]),
    ("find_bytes", tool_find_bytes,
     f"Search readable {MEMORY} for a pattern given as hex, text, or a {ORDER} u32/s32/f32/u16 value.",
     {"hex": {"type": "string"}, "text": {"type": "string"}, "u32": {"type": ["integer", "string"]},
      "s32": {"type": "integer"}, "f32": {"type": "number"}, "u16": {"type": ["integer", "string"]},
      "start": ADDR, "end": ADDR, "max_results": {"type": "integer"}}, []),
    ("write_memory", tool_write_memory,
     f"Write {MEMORY} (needs {WRITE_SWITCH}): hex bytes, or value(s) in a {ORDER} format. Shows the bytes "
     "before and after." + (" force: true also writes read-only pages (code)." if TARGET == "pc" else ""),
     {"addr": ADDR, "hex": {"type": "string"}, "format": {"type": "string"},
      "value": {"type": ["number", "string"]}, "values": {"type": "array"},
      **({"force": {"type": "boolean"}} if TARGET == "pc" else {})}, ["addr"]),
    ("screenshot", tool_screenshot,
     "The frame the game is showing, as an image (scaled down to at most max_width, default 1600).",
     {"max_width": {"type": "integer"}}, []),
    ("log_tail", tool_log_tail,
     "The last lines of the newest log, optionally only those containing grep."
     + (" feature picks GTAIV.EFLC.FusionFix.<feature>.log." if TARGET == "pc" else ""),
     {"lines": {"type": "integer"}, "grep": {"type": "string"},
      **({"feature": {"type": "string"}} if TARGET == "pc" else {})}, []),
]
if TARGET == "recomp":
    TOOLS += [
        ("shader_manifest", tool_shader_manifest,
         "Entries of the shader override manifest (LibertyRecompLib/shader_overrides) whose hash or source "
         "contains filter.", {"filter": {"type": "string"}}, []),
        ("shader_override_push", tool_shader_override_push,
         "Compile manifest entries (by hash or source file, as in the repository now) and send them to the "
         "game as live overrides, used from its next start (restart_game).",
         {"only": {"type": ["string", "array"], "items": {"type": "string"}}}, ["only"]),
        ("shader_override_list", tool_shader_override_list, "The live overrides the game holds.", {}, []),
        ("shader_override_remove", tool_shader_override_remove,
         "Remove one live override by file name, or all (the default).", {"name": {"type": "string"}}, []),
        ("restart_game", tool_restart_game,
         "Restart the game (same command line); live overrides and restart-only settings take effect.", {}, []),
        ("cvar_list", tool_cvar_list, "List the game's cvars (settings) whose name contains filter.",
         {"filter": {"type": "string"}, "verbose": {"type": "boolean"}}, []),
        ("cvar_get", tool_cvar_get, "One cvar: value, default, category, description.",
         {"name": {"type": "string"}}, ["name"]),
        ("cvar_set", tool_cvar_set, "Set a cvar in the running game (not saved to native.toml).",
         {"name": {"type": "string"}, "value": {"type": ["string", "number", "boolean"]}}, ["name", "value"]),
    ]
else:
    TOOLS += [
        ("modules", tool_modules, "The modules loaded in the game process (exe, plugins, DLLs): base and size.",
         {"filter": {"type": "string"}}, []),
        ("find_signature", tool_find_signature,
         "Find a byte signature with wildcards, as FusionFix's hooks write them (\"8B 0D ? ? ? ? 85 C9\"), "
         "in a module (the game's exe unless module is given).",
         {"pattern": {"type": "string"}, "module": {"type": "string"}, "max_results": {"type": "integer"}},
         ["pattern"]),
        ("scene", tool_scene,
         "Where the player and the game camera are, the time and the weather (the game must be running, "
         "not paused or in a menu).", {}, []),
        ("teleport", tool_teleport,
         "Move the player (and their car) to x, y, z (game units), optionally turned to heading in degrees; "
         "the scene and its collision are loaded first.",
         {"x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"},
          "heading": {"type": "number"}}, ["x", "y", "z"]),
        ("set_time", tool_set_time, "Set the time of day.",
         {"hour": {"type": "integer"}, "minute": {"type": "integer"}}, ["hour"]),
        ("set_weather", tool_set_weather,
         f"Force the weather now: an id or a name ({', '.join(WEATHERS)}), or release for the game's own.",
         {"weather": {"type": ["string", "integer"]}}, ["weather"]),
        ("ini_get", tool_ini_get, "A value from GTAIV.EFLC.FusionFix.ini.",
         {"section": {"type": "string"}, "key": {"type": "string"}}, ["section", "key"]),
        ("ini_set", tool_ini_set,
         "Write a value to GTAIV.EFLC.FusionFix.ini and have FusionFix read the ini again at the next frame "
         "(as Ctrl+Shift+F10); what is set up at startup needs a restart.",
         {"section": {"type": "string"}, "key": {"type": "string"}, "value": {"type": ["string", "number"]}},
         ["section", "key", "value"]),
    ]
HANDLERS = {name: handler for name, handler, *_ in TOOLS}


# ---- MCP over stdio (JSON-RPC 2.0, one message per line) -------------------

def send(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def handle(message):
    method = message.get("method")
    if "id" not in message:  # notifications (initialized, cancelled) need no answer
        return None
    if method == "initialize":
        version = message.get("params", {}).get("protocolVersion", PROTOCOL_VERSION)
        return {"protocolVersion": version,
                "capabilities": {"tools": {}},
                "serverInfo": {"name": f"gta4-live-{TARGET}", "version": "1.1.0"}}
    if method == "ping":
        return {}
    if method == "tools/list":
        return {"tools": [{"name": name, "description": description,
                           "inputSchema": {"type": "object", "properties": props, "required": required}}
                          for name, _, description, props, required in TOOLS]}
    if method == "tools/call":
        params = message.get("params", {})
        handler = HANDLERS.get(params.get("name"))
        if not handler:
            raise KeyError(f"unknown tool {params.get('name')}")
        try:
            result, is_error = handler(params.get("arguments") or {}), False
        except (GameError, KeyError, ValueError, struct.error) as error:
            result, is_error = f"error: {error}", True
        # (a tool gives text, or a list of content items such as an image)
        content = result if isinstance(result, list) else [{"type": "text", "text": result}]
        return {"content": content, "isError": is_error}
    raise NotImplementedError(method)


def main():
    for line in sys.stdin:
        if not line.strip():
            continue
        message = json.loads(line)
        try:
            result = handle(message)
            if result is not None:
                send({"jsonrpc": "2.0", "id": message["id"], "result": result})
        except NotImplementedError as error:
            send({"jsonrpc": "2.0", "id": message.get("id"),
                  "error": {"code": -32601, "message": f"method not found: {error}"}})
        except Exception as error:  # a bad request must not end the server
            send({"jsonrpc": "2.0", "id": message.get("id"),
                  "error": {"code": -32603, "message": str(error)}})


if __name__ == "__main__":
    main()
