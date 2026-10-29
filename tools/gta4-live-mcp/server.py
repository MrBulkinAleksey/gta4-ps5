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


def tool_clock_rate(args):
    request = {}
    if args.get("reset"):
        request["reset"] = "true"
    elif "seconds_per_minute" in args:
        request["ms_per_minute"] = max(1, round(float(args["seconds_per_minute"]) * 1000))
    elif "ms_per_minute" in args:
        request["ms_per_minute"] = parse_int(args["ms_per_minute"])
    reply = GAME.call("clock_rate", **request)
    ms = int(reply["ms_per_minute"])
    day = ms * 24 * 60 / 1000
    stopped = " (stopped by freeze)" if reply["clock_stopped"] == "true" else ""
    return (f"clock {ms} ms a game minute (the game's 2000), a game day in {day:.0f} s; "
            f"time {reply['time']}{stopped}")


def tool_freeze(args):
    if args.get("on", True) is False:
        GAME.call("freeze", on="false")
        return "unfrozen: the game camera, the player's control, the clock, the weather, the clouds and the time scale are back"
    extra = {}
    if "x" in args:
        extra.update(x=str(args["x"]), y=str(args["y"]), z=str(args["z"]),
                     rx=str(args.get("rx", 0)), ry=str(args.get("ry", 0)), rz=str(args.get("rz", 0)))
    if "fov" in args:
        extra["fov"] = str(args["fov"])
    if "time_scale" in args:
        extra["time_scale"] = str(args["time_scale"])
    reply = GAME.call("freeze", on="true", **extra)
    scale = f" time scale {reply['time_scale']}" if "time_scale" in reply else ""
    return f"frozen: camera {reply['camera']} rotation {reply['camera_rotation']} fov {reply['fov']}{scale}"


# ---- bench: a still scene measured, with an ini key or two compared ------------------------------------------

BENCH_DIR = Path(os.environ.get("GTA4_LIVE_FILES", str(Path.home() / "gta4-live-files"))) / "bench"
SPOTS_FILE = Path(__file__).with_name("bench_spots.json")


def load_spots():
    try:
        return json.loads(SPOTS_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def numbers(text):
    return [float(v) for v in str(text).strip("[] ").replace(",", " ").split()]


def tool_bench_spot(args):
    spots = load_spots()
    if "name" not in args:
        return "\n".join(f"{name}: player {s['player']}, camera {s['camera']} rot {s['rotation']} fov {s.get('fov', '-')}, "
                         f"{s['time']}, weather {s['weather']}" for name, s in spots.items()) or "no spots saved"
    if args.get("delete"):
        spots.pop(args["name"], None)
    else:
        reply = GAME.call("scene")
        spots[args["name"]] = {"player": numbers(reply["player"]), "camera": numbers(reply["camera"]),
                               "rotation": numbers(reply["camera_rotation"]), "time": reply["time"],
                               "weather": int(reply["weather"]), "fov": args.get("fov")}
    SPOTS_FILE.write_text(json.dumps(spots, indent=1), encoding="utf-8")
    return tool_bench_spot({})


def thread_cpu(report, main_tid, render_tid):
    """Busy shares (% of a core) of the main, render and DXVK command threads from a cpu_profile report"""
    import re
    busy = {}
    for line in report.splitlines():
        m = re.match(r"\s+([\d.]+)%\s+tid\s+(\d+)\s+(.*)", line)
        if not m:
            if busy and not line.startswith(" "):
                break
            continue
        share, tid, name = float(m.group(1)), m.group(2), m.group(3)
        if tid == main_tid:
            busy["main"] = share
        elif tid == render_tid:
            busy["render"] = share
        elif name.startswith("dxvk-cs"):
            busy["dxvk-cs"] = max(busy.get("dxvk-cs", 0.0), share)
    return busy


def verdict(frame_ms, gpu_ms, busy):
    """What holds the frame back. A thread busy nearly all the time comes first: the GPU's timestamps span its waits
    for the CPU too, so its time reads high whenever a thread holds it up. gpu_ms is the GPU profiler's frame less the
    time no pass accounts for (its waits between passes); waits inside a pass stay in it."""
    name, share = max(busy.items(), key=lambda kv: kv[1]) if busy else ("?", 0.0)
    if share >= 85.0:
        return f"{name} thread-bound ({share:.0f}% busy; GPU passes {gpu_ms:.1f} of {frame_ms:.1f} ms)"
    if frame_ms > 0 and gpu_ms >= 0.85 * frame_ms:
        return f"GPU-bound (GPU passes {gpu_ms:.1f} of {frame_ms:.1f} ms; busiest thread {name} {share:.0f}%)"
    return f"nothing saturated (busiest {name} {share:.0f}%, GPU passes {gpu_ms:.1f} of {frame_ms:.1f} ms): a limiter, vsync or waits"


def measure_once(frames, cpu_seconds):
    gpu = GAME.call("profiler", wait=45, timeout="30")
    scopes = cpu_scopes(frames)
    profile = GAME.call("cpu_profile", wait=cpu_seconds + 45, seconds=str(cpu_seconds), rate="100", threads="3",
                        functions="5", stacks="0", spikes="0")
    rows = {}
    path = []
    for row in gpu["rows"]:
        depth = int(row["depth"])
        path = path[:depth + 1] + [row["name"]] if depth >= 0 else []
        key = " / ".join(path) if depth >= 0 else "frame"
        rows[key] = {"ms": float(row["ms"]), "draws": float(row.get("draws", 0)), "depth": depth}
    busy = thread_cpu(profile["report"], scopes["main_thread"], scopes["render_thread"])
    frame_ms = float(scopes["frame_ms"])
    idle = rows.get("the game's other passes / other", {}).get("ms", 0.0)
    gpu_passes = rows.get("frame", {}).get("ms", 0.0) - idle
    return {"gpu": rows, "scopes": {f"{r['thread']} {r['name']}": float(r["ms"]) for r in scopes["rows"]},
            "frames": {k: float(scopes[k]) for k in ("frame_ms", "p50_ms", "p95_ms", "p99_ms", "max_ms", "main_frame_ms")},
            "busy": busy, "verdict": verdict(frame_ms, gpu_passes, busy)}


def mean_spread(values):
    values = [v for v in values if v is not None]
    if not values:
        return None, None
    return sum(values) / len(values), (max(values) - min(values)) / 2 if len(values) > 1 else 0.0


def tool_bench(args):
    import time
    label = args.get("label") or time.strftime("%Y%m%d-%H%M%S")
    section, key = args.get("section"), args.get("key")
    values = [str(v) for v in args.get("values", [])] if key else ["(as is)"]
    rounds = max(1, int(args.get("rounds", 2 if key else 1)))
    settle = float(args.get("settle", 3))
    frames, cpu_seconds = int(args.get("frames", 240)), int(args.get("cpu_seconds", 4))
    notes = []

    frozen_before = GAME.call("clock_rate").get("clock_stopped") == "true"
    spot = None
    if args.get("spot"):
        spot = load_spots().get(args["spot"])
        if not spot:
            raise ValueError(f"no spot {args['spot']} (bench_spot saves one)")
        if frozen_before:
            GAME.call("freeze", on="false")
        p = spot["player"]
        GAME.call("teleport", wait=45, x=str(p[0]), y=str(p[1]), z=str(p[2]))
        hour, minute = spot["time"].split(":")
        GAME.call("set_weather", weather=str(spot["weather"]))
        c, r = spot["camera"], spot["rotation"]
        extra = {"fov": str(spot["fov"])} if spot.get("fov") else {}
        GAME.call("freeze", on="true", x=str(c[0]), y=str(c[1]), z=str(c[2]), rx=str(r[0]), ry=str(r[1]), rz=str(r[2]),
                  time_scale=str(args.get("time_scale", 0.01)), **extra)
        GAME.call("set_time", hour=int(hour), minute=int(minute))
        time.sleep(float(args.get("stream_seconds", 8)))
        notes.append(f"spot {args['spot']}")
    elif not frozen_before:
        GAME.call("freeze", on="true", time_scale=str(args.get("time_scale", 0.01)))
        notes.append("frozen where the camera was")
    else:
        notes.append("already frozen")

    original = None
    if key:
        original = GAME.call("ini_get", section=section, key=key).get("value", "")
    results = {v: [] for v in values}
    try:
        for _ in range(rounds):
            for v in values:
                if key:
                    GAME.call("ini_set", section=section, key=key, value=v)
                time.sleep(settle)
                results[v].append(measure_once(frames, cpu_seconds))
    finally:
        if key and original != "":
            GAME.call("ini_set", section=section, key=key, value=original)
        elif key:
            # (an empty value could read as 0, not as the key's default)
            GAME.call("ini_set", section=section, key=key, value=values[0])
            notes.append(f"{key} was not in the ini: left at {values[0]}, the first value")
        if not frozen_before and not args.get("keep_frozen"):
            GAME.call("freeze", on="false")

    baseline = None
    if args.get("baseline"):
        try:
            saved = json.loads((BENCH_DIR / f"{args['baseline']}.json").read_text(encoding="utf-8"))
            baseline = next(iter(saved["results"].values()))
        except (OSError, ValueError, StopIteration):
            notes.append(f"no baseline {args['baseline']}")
    BENCH_DIR.mkdir(parents=True, exist_ok=True)
    (BENCH_DIR / f"{label}.json").write_text(json.dumps({"label": label, "section": section, "key": key, "notes": notes,
                                                         "results": results}, indent=1), encoding="utf-8")

    columns = ([("baseline " + args["baseline"], [baseline])] if baseline else []) + [(v, results[v]) for v in values]
    head = f"bench {label}" + (f": [{section}] {key} = " + " / ".join(values) if key else "") + \
           f", {rounds} round(s), {'; '.join(notes)}; mean ± half the spread between rounds"
    out = [head, ""]

    def table(title, getter, keys, fmt="{:7.2f}"):
        out.append(title)
        out.append(f"  {'':<52}" + "".join(f"{name[:16]:>18}" for name, _ in columns))
        for k in keys:
            cells = []
            for _, runs in columns:
                m, sp = mean_spread([getter(run, k) for run in runs])
                cells.append(f"{'-':>18}" if m is None else f"{fmt.format(m)} ±{sp:5.2f}".rjust(18))
            out.append(f"  {k[:52]:<52}" + "".join(cells))
        out.append("")

    def gpu_ms(run, k):
        row = run["gpu"].get(k)
        return row["ms"] if row else None

    def gpu_draws(run, k):
        row = run["gpu"].get(k)
        return row["draws"] if row else None

    first = results[values[0]][0]
    max_depth = int(args.get("depth", 1))
    gpu_keys = [k for k, row in first["gpu"].items() if row["depth"] <= max_depth and (row["ms"] >= 0.05 or k == "frame")]
    table("GPU ms a frame", gpu_ms, gpu_keys)
    table("device draws a frame", gpu_draws, [k for k in gpu_keys if (first["gpu"][k]["draws"] or 0) >= 1], "{:7.0f}")
    table("frames (ms)", lambda run, k: run["frames"].get(k), list(first["frames"].keys()))
    table("threads busy (% of a core)", lambda run, k: run["busy"].get(k), ["main", "render", "dxvk-cs"], "{:7.1f}")
    scope_keys = sorted({k for runs in results.values() for run in runs for k in run["scopes"]},
                        key=lambda k: -max(run["scopes"].get(k, 0.0) for runs in results.values() for run in runs))
    scope_keys = [k for k in scope_keys if max(run["scopes"].get(k, 0.0) for runs in results.values() for run in runs) >= 0.02]
    table("FusionFix CPU ms a frame (thread, scope)", lambda run, k: run["scopes"].get(k, 0.0), scope_keys[:int(args.get("scopes", 25))], "{:7.3f}")
    out.append("verdict:")
    for name, runs in columns:
        out.append(f"  {name}: " + "; ".join(sorted({run['verdict'] for run in runs})))
    out.append(f"\nsaved {BENCH_DIR / (label + '.json')}")
    return "\n".join(out)


def tool_profiler(args):
    extra = {"timeout": str(int(args["timeout"]))} if "timeout" in args else {}
    reply = GAME.call("profiler", wait=int(args.get("timeout", 30)) + 15, **extra)
    lines = [f"GPU milliseconds per frame, averaged over {reply['frames']} frames; device draws and primitives a frame"]
    for row in reply["rows"]:
        depth = int(row["depth"])
        counts = (f" {float(row['draws']):7.0f} {float(row['primitives']):9.0f}" if "draws" in row else "")
        lines.append(f"{'  ' * (depth + 1)}{row['name']:<{36 - 2 * (depth + 1)}} {float(row['ms']):6.2f}{counts}")
    return "\n".join(lines)


def tool_cpu_profile(args):
    keys = ("seconds", "rate", "threads", "spike_ms", "functions", "stacks", "spikes")
    extra = {k: str(int(args[k])) for k in keys if k in args}
    if args.get("waiting"):
        extra["waiting"] = "1"
    seconds = int(args.get("seconds", 10))
    reply = GAME.call("cpu_profile", wait=seconds + 45, **extra)
    return f"{name_game_functions(reply['report'])}\n(every stack of the run, for flame graphs: {reply['folded']})"


# Game functions by Complete Edition address (gtaiv_ce_names.tsv next to the server: address, name, source), put
# after "GTAIV 0x..." in the sampling profiler's reports where the address is a function's start.
GAME_NAMES = {}
try:
    for line in (Path(__file__).with_name("gtaiv_ce_names.tsv")).read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            address, name, *_ = line.split("\t")
            GAME_NAMES[int(address, 16)] = name
except OSError:
    pass


def name_game_functions(text):
    import re
    def named(match):
        name = GAME_NAMES.get(int(match.group(1), 16)) if not match.group(2) else None
        return f"{match.group(0)} [{name}]" if name else match.group(0)
    return re.sub(r"GTAIV 0x([0-9a-f]+)(\+0x[0-9a-f]+)?", named, text)


def cpu_scopes(frames=240):
    return GAME.call("cpu_scopes", wait=20 + frames // 10, frames=str(int(frames)))


def tool_cpu_scopes(args):
    reply = cpu_scopes(int(args.get("frames", 240)))
    lines = [f"{reply['frames']} frames: {float(reply['frame_ms']):.2f} ms a frame (p50 {float(reply['p50_ms']):.2f}, "
             f"p95 {float(reply['p95_ms']):.2f}, p99 {float(reply['p99_ms']):.2f}, worst {float(reply['max_ms']):.2f}); "
             f"main thread {float(reply['main_frame_ms']):.2f} ms a frame (worst {float(reply['main_max_ms']):.2f})",
             "FusionFix's own CPU work, ms a frame (mean, worst frame), calls a frame, by thread:"]
    for row in reply["rows"][:int(args.get("max", 60))]:
        lines.append(f"  {float(row['ms']):7.3f} {float(row['max_ms']):7.3f} {float(row['calls']):7.1f}  "
                     f"{row['thread']:<6} {row['name']}")
    return "\n".join(lines)


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
            "stretch_pictures", "probes", "raw", "raw_ps", "timeout")
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


# ---- shadow_probe (PC): which lamp's shadow map darkens a point of the screen, and with what -------------------

CAPTURE_LOG = "GTAIV.EFLC.FusionFix.FrameCapture.log"
SLOT_RECORDS = 0x119F100  # the night shadow slots' records, 0x110 apart (CE)


def capture_section(**extra):
    """Run a frame capture (no pictures fetched) and give its number and its part of the capture log."""
    reply = GAME.call("frame_capture", wait=90, **{k: str(v) for k, v in extra.items()})
    number = int(reply["capture"])
    text = base64.b64decode(GAME.call("file", wait=120, name=CAPTURE_LOG)["data"]).decode("utf-8", "replace")
    start = text.rfind(f"capture {number} (files")
    if start < 0:
        raise GameError(f"capture {number} is not in {CAPTURE_LOG}")
    end = text.find(f"capture {number + 1} (files", start)
    return number, text[start:end if end > 0 else len(text)]


def probe_values(line):
    """The probes of a picture line: [(r, g, b, a), ...] in the order asked."""
    out = []
    for part in line.split("| at ")[1:]:
        fields = part.split()
        out.append(tuple(float(v) for v in fields[2:6]))
    return out


def constants_of(line):
    """'c12 (a b c d) c13 (...)' as {12: (a, b, c, d)}."""
    out = {}
    for chunk in line.split(")"):
        if "(" not in chunk:
            continue
        name, values = chunk.split("(", 1)
        name = name.strip().split()[-1] if name.strip() else ""
        if name.startswith("c") and name[1:].isdigit():
            out[int(name[1:])] = tuple(float(v) for v in values.split())
    return out


def parse_draws(section):
    """Draws of a capture log section: number, kind, model, place, ps, ps constants, probes after it."""
    draws, current = {}, None
    for line in section.splitlines():
        body = line.split("] ", 2)[-1] if "] [FrameCapture" in line else line
        stripped = body.strip()
        head = stripped.split(" ", 1)
        if head[0].isdigit() and " prim " in stripped:
            number = int(head[0])
            words = stripped.split()
            current = {"draw": number, "kind": words[1], "line": stripped}
            if len(words) > 6 and words[3] == "at":
                current["model"] = words[2]
                current["at"] = tuple(float(v) for v in words[4:7])
            if " ps " in stripped:
                current["ps"] = stripped.split(" ps ", 1)[1].split()[0]
            draws[number] = current
        elif stripped.startswith("ps: ") and current is not None:
            current["c"] = constants_of(stripped[4:])
        elif stripped.startswith("rt0-after-draw-"):
            number = int(stripped[len("rt0-after-draw-"):].split(":", 1)[0])
            if number in draws:
                draws[number]["probes"] = probe_values(stripped)
    return draws


def parse_stretch_pictures(section):
    """Stretch pictures of a capture log section: [(stretch, name, target, width, height, format, probes)]."""
    out, stretch, name = [], None, ""
    for line in section.splitlines():
        stripped = line.split("] ", 2)[-1].strip() if "] [FrameCapture" in line else line.strip()
        if stripped.startswith("-- stretch "):
            words = stripped.split()
            stretch, name = int(words[2]), stripped.split("'")[1]
        elif stretch is not None and stripped[:4] in ("rt0:", "rt1:", "rt2:", "rt3:") and "| at " in stripped:
            words = stripped.split()
            width, height = (int(v) for v in words[1].split("x"))
            out.append((stretch, name, words[0][:-1], width, height, int(words[3]), probe_values(stripped)))
    return out


def slot_records():
    """The 8 slot records: position, radius, cache index, key, mode."""
    data = read_bytes(SLOT_RECORDS, 8 * 0x110)
    slots = []
    for i in range(8):
        r = data[i * 0x110:(i + 1) * 0x110]
        slots.append({"slot": i, "pos": struct.unpack_from("<3f", r, 0xC0), "radius": struct.unpack_from("<f", r, 0xE0)[0],
                      "cache": struct.unpack_from("<i", r, 0xF0)[0], "key": struct.unpack_from("<I", r, 0xF8)[0],
                      "mode": struct.unpack_from("<I", r, 0xFC)[0], "active": r[0xED]})
    return slots


def tool_shadow_probe(args):
    """See the tool's description in TOOLS."""
    x, y = float(args["x"]), float(args["y"])
    ps_list = [p.strip() for p in str(args.get("ps", "13,14,16")).split(",") if p.strip()]
    out = [f"shadow_probe at ({x:.4f}, {y:.4f}) of the screen"]

    # 1. The pixel's view depth: the first full-screen R32F stretch after the G-buffer holds it in metres
    _, section = capture_section(kinds="none", target="_DEFERRED_GBUFFER_2_", lines=200, pictures=40,
                                 draw_pictures=0, constants=0, probes=f"{x},{y}", stretch_pictures=1)
    depth = None
    for stretch, name, target, width, height, fmt, probes in parse_stretch_pictures(section):
        if fmt == 114 and width >= 1280 and probes and probes[0][0] > 0.01:
            depth = probes[0][0]
            out.append(f"view depth {depth:.3f} m (stretch {stretch} '{name}' {target} {width}x{height} R32F)")
            break
    if depth is None:
        raise GameError("no full-screen R32F depth stretch found after the G-buffer")

    # 2. The lights drawn with a shadow map, their constants and what they gave the pixel
    lamps = []
    for ps in ps_list:
        _, section = capture_section(kinds="all", ps=ps, lines=600, pictures=0, draw_pictures=40, constants=40,
                                     probes=f"{x},{y}", stretch_pictures=0)
        lamps += [d for d in parse_draws(section).values() if d.get("ps", "").upper() == ps.upper() and "c" in d]
    if not lamps:
        return "\n".join(out + [f"no draws with ps {', '.join(ps_list)} (no lamp with a shadow map lit the frame)"])
    lamps.sort(key=lambda d: d["draw"])
    c = lamps[0]["c"]
    right, up, back, cam = c[12][:3], c[13][:3], c[14][:3], c[15][:3]
    tan_x, tan_y = c[85][0], c[85][1]
    nx, ny = 2 * x - 1, 1 - 2 * y
    ray = [-back[i] + right[i] * nx * tan_x + up[i] * ny * tan_y for i in range(3)]
    world = [cam[i] + ray[i] * depth for i in range(3)]
    out.append(f"world point ({world[0]:.2f}, {world[1]:.2f}, {world[2]:.2f}); camera ({cam[0]:.2f}, {cam[1]:.2f}, {cam[2]:.2f})")

    slots = slot_records()
    previous = 0.0
    found = []
    for d in lamps:
        k = d["c"]
        light = k[78][:3]
        radius = (1.0 / k[80][0]) ** 0.5 if k.get(80, (0,))[0] > 0 else 0.0
        rows = [k.get(i, (0, 0, 0, 0)) for i in (72, 73, 74, 75)]
        v = [world[0] * rows[0][j] + world[1] * rows[1][j] + world[2] * rows[2][j] + rows[3][j] for j in range(3)]
        z = v[2] + k[76][2]
        length = (v[0] ** 2 + v[1] ** 2 + z ** 2) ** 0.5
        denom = abs(z) + length
        u = v[0] / denom * 0.5 + 0.5
        w = -v[1] / denom * 0.5 + 0.5
        channel = 0 if z <= 0 else 1
        rect = k[77]
        atlas = (u * rect[3] + rect[0], w * rect[3] + rect[1])
        seam_deg = math_degrees_asin(abs(z) / length) if length else 0.0
        gave = (d["probes"][0][0] - previous) if d.get("probes") else None
        if d.get("probes"):
            previous = d["probes"][0][0]
        slot = next((s for s in slots if all(abs(s["pos"][i] - light[i]) < 0.05 for i in range(3))), None)
        found.append({"draw": d, "light": light, "radius": radius, "dist": length, "channel": channel, "uv": (u, w),
                      "atlas": atlas, "seam": seam_deg, "gave": gave, "slot": slot, "inside": length < radius})

    # 3. What the atlas holds there, for the lamps that reach the point
    reach = [f for f in found if f["inside"]]
    if reach:
        probes = "/".join(f"{f['atlas'][0]:.6f},{f['atlas'][1]:.6f}" for f in reach)
        _, section = capture_section(kinds="none", target="SHADOW_MAP_COLOUR_CACHE", lines=100, pictures=64,
                                     draw_pictures=0, constants=0, probes=probes, stretch_pictures=1)
        pictures = [p for p in parse_stretch_pictures(section) if p[2] == "rt0"]
        if pictures:
            final = pictures[-1][6]
            for f, value in zip(reach, final):
                f["stored"] = value[f["channel"]]

    for f in found:
        d, s = f["draw"], f["slot"]
        where = (f"slot {s['slot']} (mode {s['mode']}, cache {s['cache']}, key {s['key']:08X})" if s else "no slot record at its position")
        line = (f"\nlighting draw {d['draw']} ps {d['ps']}: lamp ({f['light'][0]:.2f}, {f['light'][1]:.2f}, {f['light'][2]:.2f}) "
                f"radius {f['radius']:.1f}, {where}, atlas tile rect {tuple(round(v, 4) for v in d['c'][77])}")
        out.append(line)
        if not f["inside"]:
            out.append(f"  the point is {f['dist']:.1f} m away, outside its radius")
            continue
        gave = "" if f["gave"] is None else f", gave the pixel {f['gave']:+.3f} (with any draws since the last picture)"
        out.append(f"  {f['dist']:.2f} m from it{gave}; half {'R (behind its axis)' if f['channel'] == 0 else 'G (along its axis)'}, "
                   f"{f['seam']:.1f} deg from the halves' seam; map uv ({f['uv'][0]:.4f}, {f['uv'][1]:.4f})")
        if "stored" in f:
            stored = f["stored"]
            own = f["dist"] / f["radius"]
            # The shader moves the point 5 cm out along its normal first (c77.z * 17.07), which this leaves out:
            # within that of the surface the map holds the point's own surface, not a shadow
            shadowed = stored < own - 0.07 / f["radius"]
            out.append(f"  map holds {stored:.4f} (= {stored * f['radius']:.2f} m), the point is at {own:.4f}: "
                       + ("SHADOWED" if shadowed else "lit"))
            if shadowed:
                dx = [(world[i] - f["light"][i]) / f["dist"] for i in range(3)]
                hit = [f["light"][i] + dx[i] * stored * f["radius"] for i in range(3)]
                out.append(f"  the map's occluder lies at ({hit[0]:.2f}, {hit[1]:.2f}, {hit[2]:.2f}), "
                           f"{(f['dist'] - stored * f['radius']):.2f} m before the point on the way to the lamp")
                try:
                    near = GAME.call("nearby", x=str(hit[0]), y=str(hit[1]), z=str(hit[2]), radius="1.0", max=6,
                                     group="false", shaders="false")
                    if near["entities"]:
                        for e in near["entities"]:
                            out.append(f"    {e.get('kind', '?')} {e['hash']} {model_name(e['hash'])} at "
                                       f"({', '.join(f'{v:.1f}' for v in e['pos'])}), {e.get('distance', 0):.2f} m from it")
                    else:
                        out.append("    nothing within 1 m of it: not a real occluder (a fault of the map, as at the halves' seam)")
                except GameError as error:
                    out.append(f"    nearby failed: {error}")
    return "\n".join(out)


def math_degrees_asin(value):
    import math
    return math.degrees(math.asin(max(-1.0, min(1.0, value))))



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


LAST_VMMAP = None


def tool_vmmap(args):
    global LAST_VMMAP
    mb = 1024 * 1024
    request = {"min_size": int(float(args.get("min_mb", 1)) * mb), "top": parse_int(args.get("top", 60))}
    reply = GAME.call("vmmap", **request)
    kinds = {k["kind"]: k for k in reply["kinds"]}
    listed = {a["base"]: a for a in reply["listed"]}
    before = LAST_VMMAP if args.get("compare", True) else None

    def megs(value):
        return f"{int(value) / mb:8.1f}"

    def delta(now, then):
        return f" ({(int(now) - int(then)) / mb:+.1f})" if then is not None else ""

    old = before["reply"] if before else {}
    lines = [f"address space up to {reply['max_address']}: {reply['allocations']} allocations; free"
             f" {megs(reply['free_total']).strip()} MB{delta(reply['free_total'], old.get('free_total'))} in"
             f" {reply['free_blocks']} blocks, {megs(reply['free_usable']).strip()} MB in"
             f" {reply['free_usable_blocks']} blocks of 64 KB or more"]
    lines.append("largest free blocks (MB): " + ", ".join(
        f"{int(b['size']) / mb:.1f} at {b['base']}" for b in reply["largest_free"]))
    lines.append("")
    lines.append("kind       count   reserved MB  committed MB   (of them under min_mb: count, MB)")
    for name, k in sorted(kinds.items(), key=lambda kv: -int(kv[1]["reserved"])):
        then = before["kinds"].get(name) if before else None
        lines.append(f"{name:<8} {k['count']:>7}  {megs(k['reserved'])}{delta(k['reserved'], then and then['reserved'])}"
                     f"  {megs(k['committed'])}{delta(k['committed'], then and then['committed'])}"
                     f"   ({k['small_count']}, {int(k['small_reserved']) / mb:.1f})")
    lines.append("")
    lines.append("base          size MB  committed MB  kind     protect  regions  owner")
    for a in reply["listed"]:
        mark = ""
        if before:
            prev = before["listed"].get(a["base"])
            mark = "  NEW" if prev is None else (
                f"  {(int(a['size']) - int(prev['size'])) / mb:+.1f} MB" if prev["size"] != a["size"] else "")
        lines.append(f"{a['base']}  {megs(a['size'])}  {megs(a['committed'])}     {a['kind']:<8} {a['protect']:<8}"
                     f" {a['regions']:>7}  {a['owner']}{mark}")
    if before:
        gone = [a for base, a in before["listed"].items() if base not in listed]
        if gone:
            lines.append("")
            lines.append("listed last time, now gone or under min_mb: " + ", ".join(
                f"{a['base']} {int(a['size']) / mb:.1f} MB {a['kind']} {a['owner']}".rstrip() for a in gone))
    LAST_VMMAP = {"reply": reply, "kinds": kinds, "listed": listed}
    return "\n".join(lines)


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
        ("vmmap", tool_vmmap,
         "The game's address space (a 32-bit process runs out of addresses before memory): free in all and the "
         "largest free blocks, reserved and committed by kind (image, mapped, heap, stack, private), and the "
         "allocations of min_mb (default 1) or more, largest first (top, default 60), with the module or mapped "
         "file that owns them. Each call is compared with the one before (compare: false to skip): changes in MB, "
         "NEW allocations, ones gone.",
         {"min_mb": {"type": "number"}, "top": {"type": "integer"}, "compare": {"type": "boolean"}}, []),
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
        ("clock_rate", tool_clock_rate,
         "How fast the game clock runs: real ms a game minute (the game's 2000, a day in 48 min) or "
         "seconds_per_minute; reset gives 2000 back; with neither only reads it. 10 ms runs a day in ~15 s, "
         "to watch dawn or dusk pass. Saves keep the rate: reset before saving. freeze stops the clock whatever the rate.",
         {"ms_per_minute": {"type": "integer"}, "seconds_per_minute": {"type": "number"},
          "reset": {"type": "boolean"}}, []),
        ("freeze", tool_freeze,
         "A still scene for comparing frames: a script camera where the game camera is (or at x, y, z turned "
         "rx, ry, rz in degrees, with fov), the player frozen without control, the clock stopped (set_time keeps "
         "it stopped), the weather held (unless set_weather forced one), the volumetric clouds' drift stopped, and the "
         "world slowed by time_scale (default 0.01: people, cars, water next to still; 1 leaves it running). on: false "
         "gives all back.",
         {"on": {"type": "boolean"}, "x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"},
          "rx": {"type": "number"}, "ry": {"type": "number"}, "rz": {"type": "number"}, "fov": {"type": "number"},
          "time_scale": {"type": "number"}},
         []),
        ("profiler", tool_profiler,
         "GPU time of a frame by pass (FusionFix's PostFxProfiler): turned on for the request whatever the ini says, "
         "a block of 120 frames started afresh and returned as a tree of milliseconds per frame (FusionFix's own "
         "passes, then the game's by the render target it drew into), then as the ini had it. Hold the scene still "
         "(freeze) to compare two settings; timeout in seconds (default 30).",
         {"timeout": {"type": "integer"}}, []),
        ("files", tool_files,
         "FusionFix's own files next to the plugin (GTAIV.EFLC.FusionFix.*: logs, capture pictures, Profiler.folded), "
         "newest first, names containing filter; with name, that file (up to 64 MB) copied to this machine (to, or "
         "GTA4_LIVE_FILES, default ~/gta4-live-files) and its path given, so pictures can be looked at with Read.",
         {"filter": {"type": "string"}, "name": {"type": "string"}, "to": {"type": "string"}}, []),
        ("shadow_probe", tool_shadow_probe,
         "Which lamp's shadow map darkens a point of the screen (x, y as shares, 0-1), and with what: four frame "
         "captures of the still scene (freeze first). Gives the pixel's view depth and world point; for each lamp "
         "drawn with a shadow map (lighting pixel shaders ps, default 13,14,16): its lighting draw, position, radius, "
         "slot record (mode, cache index, key) and atlas tile, the light it gave the pixel, the paraboloid half and the "
         "angle from the halves' seam, the map's depth there against the point's (shadowed or lit), and for a shadowed "
         "point where the map's occluder lies and what stands within 1 m of it (nothing: a fault of the map).",
         {"x": {"type": "number"}, "y": {"type": "number"}, "ps": {"type": "string"}}, ["x", "y"]),
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
          "probes": {"type": "string"}, "raw": {"type": "string"}, "raw_ps": {"type": "string"}, "timeout": {"type": "integer"}, "fetch": {"type": "boolean"},
          "to": {"type": "string"}}, []),
        ("cpu_profile", tool_cpu_profile,
         "Where the game's threads spend their CPU time (FusionFix's sampling profiler, run for the request even "
         "with the ini's Profiler off): for seconds (default 10, up to 300) it stops the busiest threads rate times "
         "a second (default 1000, up to 4000) for an instant each and reads their stacks. The report: CPU time of "
         "every thread, for the sampled ones (threads, default 8; the main thread always) the busiest functions "
         "in (self) and on the stack (total), functions of each (default 40), the stacks most seen (stacks, default "
         "40) and the longest frames over spike_ms (default 50) with what the main thread and the two busiest others "
         "did in them (spikes, default 10). Game functions are named by their Complete Edition address. Sampling "
         "slows the game a little: don't read FPS from that time. waiting: the threads that wait too (up to "
         "threads, 64 at most), not only the busiest: where each thread of a hung game stands.",
         {"seconds": {"type": "integer"}, "rate": {"type": "integer"}, "threads": {"type": "integer"},
          "spike_ms": {"type": "integer"}, "functions": {"type": "integer"}, "stacks": {"type": "integer"},
          "spikes": {"type": "integer"}, "waiting": {"type": "boolean"}}, []),
        ("cpu_scopes", tool_cpu_scopes,
         "FusionFix's own CPU work in exact milliseconds a frame (CpuScopes: timed scopes, two counter reads each, "
         "only while measured): each scope by thread (main, render, other) with its mean, worst frame and calls a "
         "frame, and the frames' times between presents (mean, p50, p95, p99, worst) and the main thread's. Scopes: "
         "the GPU profiler's own sections (PostFx.*), box/prop shapes per light, coronas, LOD lights, instancing, GPU "
         "grass, procedural cone, occlusion, clouds. frames (default 240), max rows (default 60).",
         {"frames": {"type": "integer"}, "max": {"type": "integer"}}, []),
        ("bench_spot", tool_bench_spot,
         "Places to bench at: with name, saves where the player and the camera are now, the time and the weather "
         "under that name (fov optional; delete: true removes it); without, lists them (bench_spots.json next to the "
         "server).",
         {"name": {"type": "string"}, "fov": {"type": "number"}, "delete": {"type": "boolean"}}, []),
        ("bench", tool_bench,
         "Measure a still scene, and compare settings: freezes the scene where the camera is (or goes to a saved "
         "spot first: teleport, weather, camera, time, stream_seconds default 8), then for each round and each of "
         "values sets [section] key (live ini keys only), waits settle s (default 3, for accumulations), and takes the "
         "GPU profiler's block (ms and draws by pass), cpu_scopes (frames default 240) and a light CPU sample "
         "(cpu_seconds default 4) for the threads' load; the key and the freeze are given back. Report: each row's mean "
         "± half the spread between rounds (rounds default 2 with a key, else 1) per value, GPU passes to depth "
         "(default 1), frames' times, main/render/DXVK threads busy, FusionFix's CPU scopes, and a verdict of what "
         "holds the frame back. Saved under label (default the time) in GTA4_LIVE_FILES/bench; baseline: an earlier "
         "label as an extra column. time_scale for the freeze (default 0.01); keep_frozen leaves it frozen.",
         {"section": {"type": "string"}, "key": {"type": "string"}, "values": {"type": "array", "items": {"type": "string"}},
          "rounds": {"type": "integer"}, "settle": {"type": "number"}, "frames": {"type": "integer"},
          "cpu_seconds": {"type": "integer"}, "spot": {"type": "string"}, "stream_seconds": {"type": "number"},
          "label": {"type": "string"}, "baseline": {"type": "string"}, "depth": {"type": "integer"},
          "scopes": {"type": "integer"}, "time_scale": {"type": "number"}, "keep_frozen": {"type": "boolean"}}, []),
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
