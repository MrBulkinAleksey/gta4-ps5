#!/usr/bin/env python3
"""MCP server (stdio) for looking into the running GTA IV recomp.

It talks to the game's debug server (gta4-recomp/src/devtools/gta4_debug_server.cpp),
which listens on 127.0.0.1 inside the game when gta4_debug_server_port is set.
The game usually runs on another machine: forward its port here, for example
from the Windows PC

    ssh -R 5050:127.0.0.1:5050 user@this-machine

Environment: GTA4_LIVE_HOST (default 127.0.0.1), GTA4_LIVE_PORT (default 5050).

The game sends raw bytes in guest order (big-endian, as on the Xbox 360); the
decoding of values, pointer chains and dumps happens here, so tools can grow
without rebuilding the game. No dependencies beyond the standard library.
"""

import json
import os
import socket
import struct
import sys
import threading

PROTOCOL_VERSION = "2025-06-18"
HOST = os.environ.get("GTA4_LIVE_HOST", "127.0.0.1")
PORT = int(os.environ.get("GTA4_LIVE_PORT", "5050"))
TIMEOUT = float(os.environ.get("GTA4_LIVE_TIMEOUT", "60"))

# Value formats: struct code (big-endian) and size.
FORMATS = {
    "u8": ("B", 1), "s8": ("b", 1),
    "u16": (">H", 2), "s16": (">h", 2),
    "u32": (">I", 4), "s32": (">i", 4),
    "u64": (">Q", 8), "s64": (">q", 8),
    "f32": (">f", 4), "f64": (">d", 8),
}


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
                f"cannot reach the game at {HOST}:{PORT} ({error}). Is the game running with "
                f"gta4_debug_server_port = {PORT} in native.toml, and the SSH tunnel up?")
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
        pointer = struct.unpack(">I", read_bytes(addr, 4))[0]
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
    GAME.call("write", addr=f"0x{addr:X}", hex=data.hex())
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
    reply = GAME.call("log_tail", lines=5000 if grep else lines)
    found = reply["lines"]
    if grep:
        needle = grep.lower()
        found = [l for l in found if needle in l.lower()][-lines:]
    return f"{reply['file']}\n" + "\n".join(found)


ADDR = {"type": ["integer", "string"], "description": "Guest address, e.g. \"0x82000000\""}
TOOLS = [
    ("game_info", tool_game_info, "Whether the game is reachable, and where guest memory is mapped in the process.", {}, []),
    ("read_memory", tool_read_memory,
     "Read guest memory. format: dump (hexdump, default), ascii, utf16be, or a big-endian value type "
     "u8/s8/u16/s16/u32/s32/u64/s64/f32/f64 with count.",
     {"addr": ADDR, "size": {"type": "integer", "description": "Bytes for dump/ascii/utf16be (default 256)"},
      "format": {"type": "string"}, "count": {"type": "integer", "description": "Values to read (default 1)"}},
     ["addr"]),
    ("follow_pointers", tool_follow_pointers,
     "Follow a chain of 32-bit big-endian guest pointers: read the pointer at base, add the first offset, "
     "read there, and so on; dump size bytes at the end.",
     {"base": ADDR, "offsets": {"type": "array", "items": {"type": ["integer", "string"]}},
      "size": {"type": "integer"}}, ["base"]),
    ("find_bytes", tool_find_bytes,
     "Search mapped guest memory for a pattern given as hex, text, or a big-endian u32/s32/f32/u16 value.",
     {"hex": {"type": "string"}, "text": {"type": "string"}, "u32": {"type": ["integer", "string"]},
      "s32": {"type": "integer"}, "f32": {"type": "number"}, "u16": {"type": ["integer", "string"]},
      "start": ADDR, "end": ADDR, "max_results": {"type": "integer"}}, []),
    ("write_memory", tool_write_memory,
     "Write guest memory (needs gta4_debug_server_allow_write = true in the game): hex bytes, or value(s) "
     "in a big-endian format. Shows the bytes before and after.",
     {"addr": ADDR, "hex": {"type": "string"}, "format": {"type": "string"},
      "value": {"type": ["number", "string"]}, "values": {"type": "array"}}, ["addr"]),
    ("cvar_list", tool_cvar_list, "List the game's cvars (settings) whose name contains filter.",
     {"filter": {"type": "string"}, "verbose": {"type": "boolean"}}, []),
    ("cvar_get", tool_cvar_get, "One cvar: value, default, category, description.", {"name": {"type": "string"}}, ["name"]),
    ("cvar_set", tool_cvar_set, "Set a cvar in the running game (not saved to native.toml).",
     {"name": {"type": "string"}, "value": {"type": ["string", "number", "boolean"]}}, ["name", "value"]),
    ("log_tail", tool_log_tail, "The last lines of the game's newest log, optionally only those containing grep.",
     {"lines": {"type": "integer"}, "grep": {"type": "string"}}, []),
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
                "serverInfo": {"name": "gta4-live", "version": "1.0.0"}}
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
            text, is_error = handler(params.get("arguments") or {}), False
        except (GameError, KeyError, ValueError, struct.error) as error:
            text, is_error = f"error: {error}", True
        return {"content": [{"type": "text", "text": text}], "isError": is_error}
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
