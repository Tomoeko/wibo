#!/usr/bin/env python3
"""Synthetic named locales exercise bounded snapshots before guest callbacks."""
import os
import struct
import sys


def number(value):
    return struct.pack('<I', value & 0xffffffff)


def header(status=0):
    return number(0x50535957) + number(1) + number(status)


def row(name, flags):
    data = name.encode('utf-16-le', errors='surrogatepass')
    return number(len(data)) + data + number(flags)


def make_response(arguments):
    mode = os.environ.get('WIBO_FIXTURE_LOCALE_ENUMERATION_EX_RESPONSE', 'good')
    if len(arguments) != 3 or arguments[0] != 'enum-system-locales-ex':
        return header(87)
    try:
        flags, error = map(int, arguments[1:])
    except ValueError:
        return header(87)
    if min(flags, error) < 0 or max(flags, error) > 0xffffffff:
        return header(87)
    if mode == 'unavailable':
        return header(0x80041001)
    if mode == 'native-failure':
        return header() + number(0) + number(1004) + number(error) + number(0)
    rows = [('', 0x21), ('en', 0x11), ('en-US', 0x21), ('de-DE', 0x21), ('de-DE_phoneb', 4)]
    rows = [(name, properties) for name, properties in rows if flags == 0 or properties & flags]
    if mode == 'late-invalid':
        rows = [('en-US', 0x21), ('bad\0data', 0x21)]
    elif mode == 'embedded-null':
        rows = [('en\0US', 0x21)]
    elif mode == 'too-long':
        rows = [('x' * 85, 0x21)]
    elif mode == 'unpaired':
        rows = [('\ud800', 0x21)]
    elif mode == 'wrong-filter':
        rows = [('en-US', 0x21), ('de-DE_phoneb', 4)]
    response = header() + number(1) + number(error) + number(error) + number(len(rows))
    response += b''.join(row(name, properties) for name, properties in rows)
    if mode == 'truncated':
        response = response[:-1]
    elif mode == 'trailing':
        response += b'\0'
    elif mode == 'bad-result':
        response = response[:12] + number(2) + response[16:]
    elif mode == 'huge-count':
        response = response[:24] + number(0xffffffff) + response[28:]
    elif mode == 'partial-failure':
        response = response[:12] + number(0) + response[16:]
    elif mode == 'failed-with-data':
        response = header(5) + number(0)
    elif mode == 'bad-magic':
        response = number(0) + response[4:]
    elif mode == 'bad-version':
        response = response[:4] + number(2) + response[8:]
    return response


sys.stdout.buffer.write(make_response(sys.argv[1:]))
