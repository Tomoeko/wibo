#!/usr/bin/env python3
"""Synthetic version-size responses for guest ABI and transport checks."""
import os
import struct
import sys


def number(value):
    return struct.pack('<I', value)


def header(status=0):
    return number(0x50535957) + number(1) + number(status)


def native_response(size, error, written, handle=0):
    return header() + b''.join(number(value) for value in (size, error, written, handle))


def make_response(arguments):
    mode = os.environ.get('WIBO_FIXTURE_VERSION_SIZE_RESPONSE', 'success')
    filename = 'Z:\\Fixture\\version-size.exe'.encode('utf-16-le').hex()
    if mode == 'null-filename':
        filename = '-'
    elif mode == 'empty-filename':
        filename = ''
    optional_handle = mode not in ('no-handle', 'unexpected-handle')
    expected = ['file-version-info-size-ex-w', '1', filename, str(int(optional_handle)), '17185']
    if arguments != expected:
        return header(87)

    value = native_response(732, 0, int(optional_handle))
    if mode == 'success-preserved':
        value = native_response(732, 17185, 1)
    elif mode == 'zero-result':
        value = native_response(0, 1812, 1)
    elif mode == 'zero-error':
        value = native_response(0, 0, 1)
    elif mode == 'unwritten-handle':
        value = native_response(732, 0, 0)
    elif mode == 'null-filename':
        value = native_response(0, 87, 1)
    elif mode == 'empty-filename':
        value = native_response(0, 161, 1)
    elif mode == 'failed':
        value = header(5)
    elif mode == 'truncated':
        value = value[:-1]
    elif mode == 'trailing':
        value += b'\0'
    elif mode == 'invalid-presence':
        value = native_response(732, 0, 2)
    elif mode == 'nonzero-handle':
        value = native_response(732, 0, 1, 1)
    elif mode == 'absent-value':
        value = native_response(732, 0, 0, 1)
    elif mode == 'unexpected-handle':
        value = native_response(732, 0, 1)
    return value


sys.stdout.buffer.write(make_response(sys.argv[1:]))
