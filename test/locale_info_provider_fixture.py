#!/usr/bin/env python3
"""Synthetic raw locale responses for guest ABI and transport checks."""
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
    mode = os.environ.get('WIBO_FIXTURE_LOCALE_INFO_RESPONSE', 'success')
    locale = '-' if mode == 'null-locale' else 'en-US'.encode('utf-16-le').hex()
    capacity, seed = '2', 'a5' * 4
    if mode == 'query-data':
        capacity, seed = '0', '-'
    elif mode == 'negative-success':
        capacity, seed = '-1', ''
    if mode == 'success' and arguments[3:5] in (['0', '-'], ['0', '']):
        capacity, seed = arguments[3:5]
    expected = ['locale-info-ex', '536871025', locale, capacity, seed, '17185']
    if arguments != expected:
        return header(87)

    output = number(0x44332211) if capacity == '2' else b''
    value = native_response(2, 17185, output)
    if mode == 'success-cleared':
        value = native_response(2, 0, output)
    elif mode == 'false-zero':
        value = native_response(0, 0, bytes([0xa5]) * 4)
    elif mode == 'false-mutated':
        value = native_response(0, 122, number(0x78563412))
    elif mode == 'query-data':
        value = native_response(2, 17185, number(0x44332211))
    elif mode == 'write-overflow':
        value = native_response(3, 17185, output)
    elif mode == 'invalid-result':
        value = native_response(0xffffffff, 17185, output)
    elif mode == 'truncated':
        value = value[:-1]
    elif mode == 'trailing':
        value += b'\0'
    elif mode == 'short-blob':
        value = native_response(2, 17185, output[:-1])
    elif mode == 'failed':
        value = header(5)
    elif mode == 'failed-with-data':
        value = header(5) + number(0)
    return value


sys.stdout.buffer.write(make_response(sys.argv[1:]))
