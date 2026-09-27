#!/usr/bin/env python3
"""Synthetic locale identifiers exercise enumeration framing and callbacks."""
import os
import struct
import sys


def number(value):
    return struct.pack('<I', value & 0xffffffff)


def header(status=0):
    return number(0x50535957) + number(1) + number(status)


def text(value):
    encoded = value.encode('utf-16-le')
    return number(len(encoded)) + encoded


def make_response(arguments):
    mode = os.environ.get('WIBO_FIXTURE_LOCALE_ENUMERATION_RESPONSE', 'good')
    if len(arguments) != 4 or arguments[0] != 'enum-system-locales' or arguments[1] not in ('wide', 'narrow'):
        return header(87)
    try:
        flags, error = map(int, arguments[2:])
    except ValueError:
        return header(87)
    if min(flags, error) < 0 or max(flags, error) > 0xffffffff:
        return header(87)
    if mode == 'unavailable':
        return header(0x80041001)
    if mode == 'native-failure':
        return header() + number(0) + number(1004) + number(error) + number(0)
    identifiers = []
    if flags == 0 or flags & 3:
        identifiers += ['00000409', '00000407', '0000040c']
    if flags & 4:
        identifiers += ['00010407']
    if mode == 'late-invalid':
        identifiers = ['00000409', 'bad-data']
    if mode == 'embedded-null':
        identifiers = ['0000\0' + '409']
    if mode == 'short-record':
        identifiers = ['409']
    response = header() + number(1) + number(error) + number(error) + number(len(identifiers))
    response += b''.join(map(text, identifiers))
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
