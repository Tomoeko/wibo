#!/usr/bin/env python3
"""Synthetic language-list responses for guest ABI and transport checks."""
import os
import struct
import sys


def number(value):
    return struct.pack('<I', value)


def response(status, result, presence, count, units, data=None):
    value = b''.join(number(item) for item in (0x50535957, 1, status, result, presence, count, units))
    if data is not None:
        value += number(len(data)) + data
    return value


def make_response(arguments):
    if len(arguments) != 4 or arguments[0] != 'user-preferred-ui-languages':
        return response(87, 0, 0, 0, 0)
    try:
        flags, capacity, destination = map(int, arguments[1:])
    except ValueError:
        return response(87, 0, 0, 0, 0)
    if capacity < 0 or capacity > 0xffffffff or destination not in (0, 1):
        return response(87, 0, 0, 0, 0)
    if flags not in (0, 4, 8):
        return response(87, 0, 0, 0, capacity)

    names = ('0409', '040c') if flags == 4 else ('en-US', 'fr-FR')
    data = ('\0'.join(names) + '\0\0').encode('utf-16-le')
    units = len(data) // 2
    value = response(122, 0, 0, 0, units) if destination and capacity < units else response(
        0, 1, 1, 2, units, data if destination else b'')
    if not destination and capacity:
        value = response(87, 0, 0, 0, capacity)
    mode = os.environ.get('WIBO_FIXTURE_UI_LANGUAGES_RESPONSE', 'success')
    if mode == 'short-size':
        value = response(122, 0, 0, 0, units)
    elif mode == 'short-count':
        value = response(122, 0, 1, 2, units)
    elif mode == 'failed':
        value = response(87, 0, 0, 0, capacity)
    elif mode == 'failed-zero':
        value = response(0, 0, 0, 0, capacity)
    elif mode == 'truncated':
        value = value[:-1]
    elif mode == 'trailing':
        value += b'\0'
    elif mode == 'invalid-result':
        value = response(0, 2, 1, 2, units, data)
    elif mode == 'invalid-presence':
        value = response(0, 1, 2, 2, units, data)
    elif mode == 'absent-count':
        value = response(0, 1, 0, 0, units, data)
    elif mode == 'count-mismatch':
        value = response(0, 1, 1, 3, units, data)
    elif mode == 'unterminated':
        value = response(0, 1, 1, 2, units, data[:-2] + b'x\0')
    elif mode == 'wrong-length':
        value = response(0, 1, 1, 2, units, data[:-1])
    elif mode == 'oversized':
        value = response(0, 1, 1, 2, capacity + 1, data)
    elif mode == 'invalid-identifier':
        value = response(0, 1, 1, 2, units, b'G\0' + data[2:])
    elif mode in ('present-zero-success', 'null-positive-success'):
        value = response(0, 1, 1, 2, units, b'')
    elif mode == 'zero-required-size':
        value = response(122, 0, 0, 0, 0)
    elif mode == 'huge-required-size':
        value = response(122, 0, 0, 0, 0xffffffff)
    elif mode == 'non-growing-short':
        value = response(122, 0, 0, 0, capacity)
    elif mode == 'null-positive-short':
        value = response(122, 0, 0, 0, units)
    elif mode == 'query-huge-size':
        value = response(0, 1, 1, 1, 0xffffffff, b'')
    return value


sys.stdout.buffer.write(make_response(sys.argv[1:]))
