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
import zlib
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
    """A connection to the debug server for each request, closed after its reply: the server serves one
    client at a time, so a connection kept open would shut every other session out."""

    def __init__(self):
        self.lock = threading.Lock()
        self.next_id = 0

    def call(self, cmd, wait=None, **args):
        """wait: seconds this request may take, if longer than TIMEOUT (a profile of a given length)"""
        timeout = max(TIMEOUT, wait or 0)
        with self.lock:
            try:
                sock = socket.create_connection((HOST, PORT), timeout=timeout)
            except OSError as error:
                raise GameError(
                    f"cannot reach {GAME_NAME} at {HOST}:{PORT} ({error}). Is it running with "
                    f"{ENABLE_HINT.format(port=PORT)}, and the SSH tunnel up?")
            self.next_id += 1
            request = dict(args, cmd=cmd, id=self.next_id)
            try:
                with sock, sock.makefile("rb") as file:
                    sock.sendall((json.dumps(request) + "\n").encode())
                    line = file.readline()
            except socket.timeout:
                raise GameError(
                    f"no reply from {GAME_NAME} in {timeout:g} s (another session's long request, "
                    "such as a teleport, is served first)")
            except OSError as error:
                raise GameError(f"lost the game connection: {error}")
            if not line:
                raise GameError("lost the game connection: connection closed")
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


def tool_freeze(args):
    if args.get("on", True) is False:
        GAME.call("freeze", on="false")
        return "unfrozen: the game camera, the player's control and the clock are back"
    extra = {}
    if "x" in args:
        extra.update(x=str(args["x"]), y=str(args["y"]), z=str(args["z"]),
                     rx=str(args.get("rx", 0)), ry=str(args.get("ry", 0)), rz=str(args.get("rz", 0)))
    if "fov" in args:
        extra["fov"] = str(args["fov"])
    reply = GAME.call("freeze", on="true", **extra)
    return f"frozen: camera {reply['camera']} rotation {reply['camera_rotation']} fov {reply['fov']}"


def tool_profiler(args):
    extra = {"timeout": str(int(args["timeout"]))} if "timeout" in args else {}
    reply = GAME.call("profiler", wait=int(args.get("timeout", 30)) + 15, **extra)
    lines = [f"GPU milliseconds per frame, averaged over {reply['frames']} frames"]
    for row in reply["rows"]:
        depth = int(row["depth"])
        lines.append(f"{'  ' * (depth + 1)}{row['name']:<{36 - 2 * (depth + 1)}} {float(row['ms']):6.2f}")
    return "\n".join(lines)


def tool_cpu_profile(args):
    keys = ("seconds", "rate", "threads", "spike_ms", "functions", "stacks", "spikes")
    extra = {k: str(int(args[k])) for k in keys if k in args}
    seconds = int(args.get("seconds", 10))
    reply = GAME.call("cpu_profile", wait=seconds + 45, **extra)
    return f"{reply['report']}\n(every stack of the run, for flame graphs: {reply['folded']})"


FILES_DIR = os.environ.get("GTA4_LIVE_FILES", str(Path.home() / "gta4-live-files"))


def tool_files(args):
    """FusionFix's files next to the plugin; with name, that file copied here (FILES_DIR or to)."""
    if "name" not in args:
        reply = GAME.call("files", filter=str(args.get("filter", "")))
        lines = [f"{f['name']}  {int(f['size']):,} bytes" for f in reply["files"]]
        return "\n".join(lines) if lines else "no files match"
    reply = GAME.call("file", wait=120, name=str(args["name"]))
    folder = Path(args.get("to", FILES_DIR))
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / Path(reply["name"]).name
    path.write_bytes(base64.b64decode(reply["data"]))
    return f"{reply['name']}: {int(reply['size']):,} bytes saved to {path}"


def bmp_to_png(data):
    """A 24-bit bottom-up BMP (FusionFix's capture pictures) as PNG bytes, so Read can show it."""
    width, height = struct.unpack_from("<ii", data, 18)
    offset = struct.unpack_from("<I", data, 10)[0]
    row = (width * 3 + 3) & ~3
    raw = bytearray()
    for y in range(abs(height)):
        line = data[offset + row * (abs(height) - 1 - y if height > 0 else y):][:width * 3]
        raw.append(0)
        raw += bytes(b for i in range(0, width * 3, 3) for b in (line[i + 2], line[i + 1], line[i]))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, abs(height), 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def tool_frame_capture(args):
    keys = ("kinds", "model", "ps", "vs", "target", "lines", "pictures", "draw_pictures", "constants",
            "stretch_pictures", "probes", "raw", "timeout")
    extra = {k: str(args[k]) for k in keys if k in args}
    if isinstance(args.get("stretch_pictures"), bool):
        extra["stretch_pictures"] = "1" if args["stretch_pictures"] else "0"
    reply = GAME.call("frame_capture", wait=int(args.get("timeout", 30)) + 30, **extra)
    lines = [f"capture {reply['capture']}, the whole log: {reply['log']} (files name=GTAIV.EFLC.FusionFix.FrameCapture.log)",
             reply["summary"].rstrip()]
    pictures = reply.get("pictures", [])
    if args.get("fetch", True) and pictures:
        folder = Path(args.get("to", FILES_DIR)) / f"capture-{reply['capture']}"
        folder.mkdir(parents=True, exist_ok=True)
        lines.append(f"pictures as PNG in {folder}:")
        for name in pictures:
            data = base64.b64decode(GAME.call("file", wait=120, name=name)["data"])
            if name.endswith(".bmp"):
                path = folder / (Path(name).stem + ".png")
                data = bmp_to_png(data)
            else:
                path = folder / Path(name).name
            path.write_bytes(data)
            lines.append(f"  {path}")
    else:
        lines += [f"  {name}" for name in pictures]
    return "\n".join(lines)


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


# ---- over time (PC): watches sample values each frame, traces record the hits of code or memory ----------

REGISTERS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


def item_size(item):
    fmt = item.get("type", "u32")
    if fmt == "hex":
        return parse_int(item.get("size", 4))
    if fmt not in FORMATS:
        raise GameError(f"type is hex or one of {', '.join(FORMATS)}")
    return FORMATS[fmt][1] * parse_int(item.get("count", 1))


def item_spec(item, registers):
    """A spec for the game: base[,offset...]:size (see source/debugserver.ixx)."""
    if "reg" in item:
        if not registers:
            raise GameError("a watch reads memory only: give addr, not reg")
        base = str(item["reg"]).lower()
        if base not in REGISTERS:
            raise GameError(f"reg is one of {', '.join(REGISTERS)}")
        offsets = [parse_int(o) for o in item.get("offsets", [])]
    elif "addr" in item:
        base = f"0x{parse_int(item['addr']):X}"
        # (an address alone means the bytes there, as offset 0)
        offsets = [parse_int(o) for o in item.get("offsets", [])] or [0]
    else:
        raise GameError("each item needs reg or addr")
    size = item_size(item)
    if not offsets and size > 4:
        raise GameError("a register's own value is at most 4 bytes: give offsets to read memory it points to")
    return ",".join([base] + [f"0x{o:X}" for o in offsets]) + f":{size}"


def item_label(item):
    if item.get("label"):
        return item["label"]
    base = item.get("reg") or f"0x{parse_int(item['addr']):X}"
    offsets = [parse_int(o) for o in item.get("offsets", [])]
    return base + "".join(f"+0x{o:X}" if i == 0 else f">+0x{o:X}" for i, o in enumerate(offsets))


def item_meta(item):
    return {"label": item_label(item), "type": item.get("type", "u32"), "count": parse_int(item.get("count", 1))}


def decode(hex_text, meta):
    if hex_text is None:
        return "?"
    data = bytes.fromhex(hex_text)
    fmt = meta.get("type", "u32")
    if fmt not in FORMATS:
        return data.hex()
    code, width = FORMATS[fmt]
    values = [struct.unpack_from(code, data, i * width)[0] for i in range(len(data) // width)]
    shown = [f"{v:.4g}" if fmt.startswith("f") else f"0x{v:X}" if fmt.startswith("u") and width >= 4 else str(v)
             for v in values]
    return shown[0] if len(shown) == 1 else "(" + " ".join(shown) + ")"


def tool_watch(args):
    items = args["items"]
    request = {"name": args["name"], "items": ";".join(item_spec(i, False) for i in items),
               "meta": json.dumps([item_meta(i) for i in items]),
               "every": parse_int(args.get("every", 1)), "count": parse_int(args.get("count", 600)),
               "keep": args.get("keep", "last"), "on": args.get("on", "present"),
               "changes": "true" if args.get("changes_only") else "false"}
    GAME.call("watch_start", **request)
    return (f"watching {args['name']}: {len(items)} item(s) every {request['every']} {request['on']} frame(s), "
            f"{request['count']} records kept ({request['keep']}); trace_read to see them, trace_stop to end")


def tool_trace(args):
    reads = args.get("reads", [])
    request = {"name": args["name"], "addr": f"0x{parse_int(args['addr']):X}", "mode": args.get("mode", "hook"),
               "reads": ";".join(item_spec(i, True) for i in reads),
               "count": parse_int(args.get("count", 1000)), "keep": args.get("keep", "first"),
               "stack": parse_int(args.get("stack", 4)), "xmm": "true" if args.get("xmm") else "false"}
    meta = {"reads": [item_meta(i) for i in reads]}
    if args.get("mode") in ("write", "access"):
        request["length"] = parse_int(args.get("length", 4))
        meta["length_type"] = {1: "u8", 2: "u16", 4: "u32"}.get(request["length"], "u32")
        if args.get("value_type"):
            meta["length_type"] = args["value_type"]
    if "func" in args:
        request["func"] = f"0x{parse_int(args['func']):X}"
    if args.get("force"):
        request["force"] = "true"
    when = args.get("when")
    if when:
        request["when"] = item_spec(when, True)
        fmt = when.get("type", "u32")
        if fmt not in FORMATS:
            raise GameError(f"when's type is one of {', '.join(FORMATS)}")
        negate = "not_equals" in when
        value = when["not_equals" if negate else "equals"]
        request["equals"] = struct.pack(FORMATS[fmt][0], float(value) if fmt.startswith("f") else parse_int(value)).hex()
        request["op"] = "ne" if negate else "eq"
        meta["when"] = f"{item_label(when)} {'!=' if negate else '=='} {value}"
    request["meta"] = json.dumps(meta)
    GAME.call("trace_start", **request)
    return (f"tracing {args['name']}: {request['mode']} at {request['addr']}, {len(reads)} read(s), "
            f"{request['count']} records kept ({request['keep']}); trace_read to see them, trace_stop to end")


def summarize(label, texts, meta):
    """min/max/changes of a watched item's first value."""
    fmt = meta.get("type", "u32")
    if fmt not in FORMATS:
        return f"  {label}: {len(set(texts))} distinct"
    code, _ = FORMATS[fmt]
    values = [struct.unpack_from(code, bytes.fromhex(t))[0] for t in texts if t is not None]
    if not values:
        return f"  {label}: never read"
    changes = sum(1 for a, b in zip(texts, texts[1:]) if a != b)
    one = {"type": fmt, "count": 1}
    pack = lambda v: struct.pack(code, v).hex()
    return (f"  {label}: first {decode(pack(values[0]), one)}, last {decode(pack(values[-1]), one)}, "
            f"min {decode(pack(min(values)), one)}, max {decode(pack(max(values)), one)}, {changes} change(s)")


def tool_trace_read(args):
    request = {"name": args["name"], "from": parse_int(args.get("from", 0)), "max": parse_int(args.get("max", 200))}
    reply = GAME.call("trace_read", **request)
    records = reply["records"]
    try:
        meta = json.loads(reply.get("meta") or "null")
    except ValueError:
        meta = None
    lines = []
    if reply["kind"] == "watch":
        items = meta if isinstance(meta, list) else [{"label": f"item{i}", "type": "hex"} for i in range(len(records[0]["values"]) if records else 0)]
        lines.append(f"watch {reply['name']}: {reply['samples']} samples, {reply['written']} recorded (keeps {reply['capacity']}, "
                     f"{reply['keep']}), over {reply['seconds']} s / {reply['frames']} frames")
        for i, item in enumerate(items):
            lines.append(summarize(item["label"], [r["values"][i] for r in records], item))
        lines.append("n  frame  ms  " + "  ".join(i["label"] for i in items))
        for r in records:
            lines.append(f"{r['n']}  {r['f']}  {int(r['t']) / 1000:.1f}  " +
                         "  ".join(decode(v, items[i]) for i, v in enumerate(r["values"])))
        return "\n".join(lines)

    meta = meta if isinstance(meta, dict) else {}
    reads = meta.get("reads", [])
    shown = args.get("regs", REGISTERS)
    lines.append(f"trace {reply['name']}: {reply['mode']} at {reply['addr']}, {reply['hits']} hits"
                 + (f" ({reply['matched']} with {meta['when']})" if meta.get("when") else "")
                 + f", {reply['written']} recorded (keeps {reply['capacity']}, {reply['keep']}), "
                   f"over {reply['seconds']} s / {reply['frames']} frames")
    if reply["callers"]:
        lines.append("callers (first code address on the stack, all hits): " +
                     ", ".join(f"{c['addr']} x{c['hits']}" for c in reply["callers"][:16]) +
                     (f", other {reply['callers_other']}" if int(reply["callers_other"]) else ""))
    if reply["mode"] in ("write", "access"):
        lines.append("eip is the instruction after the access")
    for r in records:
        regs = struct.unpack("<10I", bytes.fromhex(r["regs"]))
        parts = [f"#{r['n']} f{r['f']} {int(r['t']) / 1000:.1f}ms tid {r['tid']} eip {regs[9]:08X}"]
        parts.append(" ".join(f"{name}={regs[i]:08X}" for i, name in enumerate(REGISTERS) if name in shown))
        if "before" in r:
            t = {"type": meta.get("length_type", "u32"), "count": 1}
            parts.append(f"{decode(r['before'], t)} -> {decode(r['after'], t)}")
        if reads:
            parts.append(" ".join(f"{m['label']}={decode(v, m)}" for m, v in zip(reads, r["reads"])))
        if r["stack"]:
            parts.append("stack " + " ".join(a[2:] for a in r["stack"]))
        if "xmm" in r:
            data = bytes.fromhex(r["xmm"])
            parts.append("xmm " + " ".join(f"{i}:(" + " ".join(f"{v:.4g}" for v in struct.unpack_from('<4f', data, i * 16)) + ")"
                                           for i in range(8)))
        lines.append(" | ".join(p for p in parts if p))
    return "\n".join(lines)


def tool_trace_stop(args):
    return f"stopped {GAME.call('trace_stop', name=args.get('name', 'all'))['stopped']}"


def tool_trace_list(_):
    sessions = GAME.call("trace_list")["sessions"]
    return "\n".join(
        f"{s['name']}: trace {s['mode']} at {s['addr']}, {s['hits']} hits, {s['written']}/{s['capacity']} recorded"
        if s["kind"] == "trace" else f"{s['name']}: watch, {s['samples']} samples, {s['written']}/{s['capacity']} recorded"
        for s in sessions) or "none"


# ---- what is near (PC): buildings and objects within a radius, model hashes named from a list of names ----

NAMES_FILE = os.environ.get("GTA4_LIVE_NAMES", str(Path(__file__).with_name("model_names.txt")))
_names = None


def joaat(text):
    h = 0
    for c in text.lower().encode():
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


def model_name(hash_text):
    """The model's name from model_names.txt (one name a line, or .ide lines: the name is the first field)."""
    global _names
    if _names is None:
        _names = {}
        try:
            for line in Path(NAMES_FILE).read_text(errors="replace").splitlines():
                name = line.split(",")[0].strip()
                if name and not name.startswith("#") and " " not in name:
                    _names[joaat(name)] = name
        except OSError:
            pass
    return _names.get(int(hash_text, 16), "")


def tool_nearby(args):
    if "x" in args:
        x, y, z = float(args["x"]), float(args["y"]), float(args["z"])
    else:
        x, y, z = (float(v) for v in GAME.call("scene")["player"])
    radius = args.get("radius", 10)
    request = dict(x=str(x), y=str(y), z=str(z), radius=str(radius), max=parse_int(args.get("max", 60)))
    for key in ("kind", "shader", "texture"):
        if args.get(key):
            request[key] = str(args[key])
    if args.get("model") not in (None, ""):
        model = str(args["model"])
        request["model"] = model if model.lower().startswith("0x") or model.isdigit() else f"0x{joaat(model):08X}"
    for key in ("min_top", "max_top"):
        if args.get(key) is not None:
            request[key] = str(args[key])
    request["group"] = "true" if args.get("group") else "false"
    request["shaders"] = "false" if args.get("shaders") is False else "true"
    reply = GAME.call("nearby", **request)
    grouped = args.get("group")
    head = f"{reply['total']} within {radius} m of ({x:.1f}, {y:.1f}, {z:.1f})"
    lines = [head + (f", {reply.get('models', '?')} models" if grouped else "")]
    for e in reply["entities"]:
        size = " x ".join(f"{v:.1f}" for v in e["size"])
        name = model_name(e["hash"])
        where = f"({', '.join(f'{v:.1f}' for v in e['pos'])})"
        if grouped:
            line = (f"{e['distance']:>6.1f} m  {e['count']:>4} x {e['kind']:<8} model {e['model']:>5} {e['hash']}"
                    f"{' ' + name if name else ''}  size {size}, top {e.get('top', 0):.2f}  nearest at {where} entity {e['entity']}")
        else:
            line = (f"{e['distance']:>6.1f} m  {e['kind']:<8} model {e['model']:>5} {e['hash']}{' ' + name if name else ''}  "
                    f"size {size}, top {e.get('top', 0):.2f}  at {where} (origin {e.get('origin_distance', 0):.1f} m)  up {e['up']:.2f}  "
                    f"alpha {e.get('alpha', 0)}  +0x210 {e['flags210']}"
                    f"{'  lod parent ' + e['lod_parent'] if e.get('lod_parent', '0x00000000') != '0x00000000' else ''}  "
                    f"entity {e['entity']} info {e['info']}")
        lines.append(line)
        for shader in e.get("shaders", []):
            lines.append(f"            {shader}")
    return "\n".join(lines)


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
        ("freeze", tool_freeze,
         "A still scene for comparing frames: a script camera where the game camera is (or at x, y, z turned "
         "rx, ry, rz in degrees, with fov), the player frozen without control, the clock stopped (set_time keeps "
         "it stopped). on: false gives the game camera, control and clock back.",
         {"on": {"type": "boolean"}, "x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"},
          "rx": {"type": "number"}, "ry": {"type": "number"}, "rz": {"type": "number"}, "fov": {"type": "number"}},
         []),
        ("profiler", tool_profiler,
         "GPU time of a frame by pass (FusionFix's PostFxProfiler): turned on for the request whatever the ini says, "
         "a block of 120 frames started afresh and returned as a tree of milliseconds per frame (FusionFix's own "
         "passes, then the game's by the render target it drew into), then as the ini had it. Hold the scene still "
         "(freeze) to compare two settings; timeout in seconds (default 30).",
         {"timeout": {"type": "integer"}}, []),
        ("files", tool_files,
         "FusionFix's own files next to the plugin (GTAIV.EFLC.FusionFix.*: logs, capture pictures, Profiler.folded), "
         "newest first, names containing filter; with name, that file (up to 16 MB) copied to this machine (to, or "
         "GTA4_LIVE_FILES, default ~/gta4-live-files) and its path given, so pictures can be looked at with Read.",
         {"filter": {"type": "string"}, "name": {"type": "string"}, "to": {"type": "string"}}, []),
        ("frame_capture", tool_frame_capture,
         "Capture the next whole scene frame (FusionFix's Frame Capture): every stretch of draws into one set of "
         "colour targets (target name, rt0-rt3 and depth formats, draws by pixel shader) with pictures of its "
         "targets as the frame leaves them (G-buffer albedo/normals/material ID/depth, light, fog, post), and every "
         "draw matching the filters (entity kind, model hash, place, states, VS/PS FusionShaders numbers, textures "
         "s0-s15), rt0 pictured after the first draw_pictures of them, all non-zero shader constants (ps c0-c223, "
         "b, i; vs c0-c255, b) of the first constants of them. Filters: kinds (all, or a comma list of none, "
         "unmarked, buildings, props, vehicles, peds), model (hash, hex), ps / vs (numbers as the log gives them, "
         "hex), target (part of the target name, also picks the stretches pictured). lines (3000), pictures "
         "(128), draw_pictures (24), constants (4), stretch_pictures (true). probes: points x,y as shares of the "
         "target (0-1) apart by '/', whose raw values (all four channels) are read in every picture taken. raw: part "
         "of a target name whose stretches' targets are also saved whole in their own format (.raw: 'FFRAW1', "
         "uint32 width, height, D3DFORMAT, bytes per pixel, rows top down), for exact values at any pixel. "
         "Returns the stretches in short; the pictures are fetched as PNG (fetch false: names only) to read with "
         "Read; the full log via files.",
         {"kinds": {"type": "string"}, "model": {"type": "string"}, "ps": {"type": "string"},
          "vs": {"type": "string"}, "target": {"type": "string"}, "lines": {"type": "integer"},
          "pictures": {"type": "integer"}, "draw_pictures": {"type": "integer"},
          "constants": {"type": "integer"}, "stretch_pictures": {"type": "boolean"},
          "probes": {"type": "string"}, "raw": {"type": "string"}, "timeout": {"type": "integer"}, "fetch": {"type": "boolean"},
          "to": {"type": "string"}}, []),
        ("cpu_profile", tool_cpu_profile,
         "Where the game's threads spend their CPU time (FusionFix's sampling profiler, run for the request even "
         "with the ini's Profiler off): for seconds (default 10, up to 300) it stops the busiest threads rate times "
         "a second (default 1000, up to 4000) for an instant each and reads their stacks. The report: CPU time of "
         "every thread, for the sampled ones (threads, default 8; the main thread always) the busiest functions "
         "in (self) and on the stack (total), functions of each (default 40), the stacks most seen (stacks, default "
         "40) and the longest frames over spike_ms (default 50) with what the main thread and the two busiest others "
         "did in them (spikes, default 10). Game functions are named by their Complete Edition address. Sampling "
         "slows the game a little: don't read FPS from that time.",
         {"seconds": {"type": "integer"}, "rate": {"type": "integer"}, "threads": {"type": "integer"},
          "spike_ms": {"type": "integer"}, "functions": {"type": "integer"}, "stacks": {"type": "integer"},
          "spikes": {"type": "integer"}}, []),
        ("watch", tool_watch,
         "Sample values each frame into a buffer in the game, to see how they change over time. items: "
         "[{addr, offsets?, type (u8..f64 or hex with size), count?, label?}]; offsets as in follow_pointers but "
         "from the address itself: the bytes at addr + offsets[0], each further offset reading a pointer there and "
         "adding to it. every: each Nth frame; count: records kept (default 600), keep: last (default) or first; "
         "on: present (each frame shown, also in menus) or game (the game's own frames); changes_only: record only "
         "when a value changes. Read with trace_read, end with trace_stop.",
         {"name": {"type": "string"}, "items": {"type": "array", "items": {"type": "object"}},
          "every": {"type": "integer"}, "count": {"type": "integer"}, "keep": {"type": "string"},
          "on": {"type": "string"}, "changes_only": {"type": "boolean"}}, ["name", "items"]),
        ("trace", tool_trace,
         "Record each hit of an instruction or of a piece of memory (needs DebugServerAllowWrite): registers, the "
         "code addresses on the stack (callers; the first one is the return address at a function's start), and "
         "reads. mode: hook (a mid-hook at addr, which must start an instruction: checked by decoding from the "
         "padding before the function, or from func; force skips the check), execute (a debug register, no code "
         "patched), write or access (a data breakpoint on length 1/2/4 aligned bytes: records before -> after and "
         "eip after the accessing instruction; value_type decodes them); at most 8 hooks and 4 debug registers. "
         "reads: [{reg or addr, offsets?, type, count?, label?}] at the hit (reg alone = its value; reg with offsets "
         "= memory it points to, e.g. {reg: ecx, offsets: [0x24], type: f32, count: 4}; esp offset 4 = the first "
         "stack argument at a function's start). when: {reg or addr, offsets?, type, equals | not_equals} records "
         "only matching hits. count: records kept (default 1000), keep: first (default) or last; stack: code "
         "addresses to keep (0..16, default 4); xmm: also xmm0..7. Read with trace_read, end with trace_stop.",
         {"name": {"type": "string"}, "addr": ADDR, "mode": {"type": "string"}, "length": {"type": "integer"},
          "value_type": {"type": "string"}, "func": ADDR, "force": {"type": "boolean"},
          "reads": {"type": "array", "items": {"type": "object"}}, "when": {"type": "object"},
          "count": {"type": "integer"}, "keep": {"type": "string"}, "stack": {"type": "integer"},
          "xmm": {"type": "boolean"}}, ["name", "addr"]),
        ("trace_read", tool_trace_read,
         "The records of a watch (a summary per item, then a table) or a trace (hits, callers by count, then one "
         "line per hit), from record number from (default the oldest kept), at most max (default 200, up to 5000). "
         "regs: which registers to show in trace lines (default all).",
         {"name": {"type": "string"}, "from": {"type": "integer"}, "max": {"type": "integer"},
          "regs": {"type": "array", "items": {"type": "string"}}}, ["name"]),
        ("trace_stop", tool_trace_stop, "End a watch or trace by name (unhooks, frees debug registers), or all.",
         {"name": {"type": "string"}}, []),
        ("trace_list", tool_trace_list, "The watches and traces running in the game.", {}, []),
        ("nearby", tool_nearby,
         "The buildings (map models, procedural props) and objects within radius (default 10 m) of x, y, z or of the "
         "player, nearest first. Distance is to the model's bounding box in the world (0 = inside it), so big models "
         "whose origin is far off (a row of bushes, a map section) are found by their nearest part. Each: model index "
         "and hash (named from model_names.txt next to the server, one name or .ide line each), size and top above the "
         "origin from the model's bounds, position, origin distance, uprightness, fade alpha, LOD parent, entity and "
         "model info, and, when its drawable is loaded, its shaders as \"preset.sps: texture, texture\". Filters: kind "
         "(building/object), model (index, 0x hash or a name), shader and texture (parts of names, any case), min_top / "
         "max_top (m above the origin). group: one row per model with the count. shaders: false leaves them out.",
         {"x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"}, "radius": {"type": "number"},
          "max": {"type": "integer"}, "kind": {"type": "string"}, "model": {"type": ["string", "integer"]},
          "shader": {"type": "string"}, "texture": {"type": "string"}, "min_top": {"type": "number"},
          "max_top": {"type": "number"}, "group": {"type": "boolean"}, "shaders": {"type": "boolean"}}, []),
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
