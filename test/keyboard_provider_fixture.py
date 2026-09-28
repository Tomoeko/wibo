#!/usr/bin/env python3
import os
import struct
import sys


MAGIC = 0x50535957
UNAVAILABLE = 0x80041001
MODE = os.environ.get("WIBO_FIXTURE_KEYBOARD_RESPONSE", "success")


def number(value):
    return struct.pack("<I", value & 0xFFFFFFFF)


def frame(status, payload=b""):
    return number(MAGIC) + number(1) + number(status) + payload


def blob(value):
    return number(len(value)) + value


def parse(text, maximum=0xFFFFFFFF):
    if not text.isascii() or not text.isdecimal():
        raise ValueError("invalid scalar")
    value = int(text)
    if value > maximum:
        raise ValueError("scalar exceeds range")
    return value


def serve(arguments):
    if not arguments or not arguments[0].startswith("keyboard-"):
        return frame(UNAVAILABLE)
    if MODE == "unavailable":
        return frame(UNAVAILABLE)
    if arguments[0] == "keyboard-layout" and len(arguments) == 1:
        if MODE == "truncated-layout":
            return frame(0, number(0x04090409))
        return frame(0, number(0x04090409) + number(0) + blob("00000409\0".encode("utf-16le")))
    if MODE != "success":
        return frame(UNAVAILABLE)
    if arguments[0] == "keyboard-vk-scan" and len(arguments) == 5:
        character, low, high, incoming = map(parse, arguments[1:])
        if character > 0xFFFF or (low, high) != (0x04090409, 0):
            return frame(50)
        results = {ord("a"): 0x0041, ord("A"): 0x0141}
        return frame(0, number(results.get(character, 0xFFFF)) + number(incoming))
    if arguments[0] == "keyboard-map" and len(arguments) == 7:
        variant = arguments[1]
        code, kind, low, high, incoming = map(parse, arguments[2:])
        if variant not in ("a", "w", "ex-a") or (low, high) != (0x04090409, 0):
            return frame(50)
        result = 0x1E if code == ord("A") and kind == 0 else 0
        return frame(0, number(result) + number(incoming))
    if arguments[0] == "keyboard-to-unicode" and len(arguments) == 10:
        key, scan, flags, count, low, high = map(parse, arguments[1:7])
        incoming = parse(arguments[9])
        state = bytes.fromhex(arguments[7])
        output = bytearray.fromhex(arguments[8])
        if (key, scan, flags, count, low, high) != (ord("A"), 0x1E, 4, 8, 0x04090409, 0):
            return frame(50)
        if len(state) != 256 or len(output) != count * 2:
            return frame(87)
        output[0:4] = b"a\x00\x00\x00"
        return frame(0, number(1) + number(incoming) + blob(output))
    return frame(UNAVAILABLE)


try:
    result = serve(sys.argv[1:])
except (ValueError, UnicodeError):
    result = frame(13)
sys.stdout.buffer.write(result)
