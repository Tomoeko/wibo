#!/usr/bin/env python3
"""Synthetic version-buffer responses for guest ABI and transport checks."""
import os
import struct
import sys


def number(value):
    return struct.pack('<I', value)


def header(status=0):
    return number(0x50535957) + number(1) + number(status)


def native_response(result, error, output):
    return header() + number(result) + number(error) + number(len(output)) + output


def make_response(arguments):
    mode = os.environ.get('WIBO_FIXTURE_VERSION_INFO_RESPONSE', 'success')
    null_data = mode in ('null-data', 'null-data-success', 'null-data-with-blob')
    seed = bytes([0xa5]) * 96
    expected = [
        'file-version-info-ex-w', '3',
        'Z:\\Fixture\\version-info.exe'.encode('utf-16-le').hex(),
        '305419896', '96', '-' if null_data else seed.hex(), '17185',
    ]
    if arguments != expected:
        return header(87)

    changed = bytes([0, 0x11, 0x22, 0x33, 0x80, 0xff, 0x42, 0]) + seed[8:]
    value = native_response(1, 0, changed)
    if mode == 'success-preserved':
        value = native_response(1, 17185, changed)
    elif mode == 'false-zero':
        value = native_response(0, 0, seed)
    elif mode == 'false-mutated':
        value = native_response(0, 1812, changed)
    elif mode == 'null-data':
        value = native_response(0, 13, b'')
    elif mode == 'null-data-success':
        value = native_response(1, 0, b'')
    elif mode == 'null-data-with-blob':
        value = native_response(0, 13, seed)
    elif mode == 'failed':
        value = header(5)
    elif mode == 'failed-with-data':
        value = header(5) + number(0)
    elif mode == 'truncated':
        value = value[:-1]
    elif mode == 'trailing':
        value += b'\0'
    elif mode == 'invalid-result':
        value = native_response(2, 0, changed)
    elif mode == 'short-blob':
        value = native_response(1, 0, changed[:-1])
    elif mode == 'long-blob':
        value = native_response(1, 0, changed + b'\0')
    return value


sys.stdout.buffer.write(make_response(sys.argv[1:]))
