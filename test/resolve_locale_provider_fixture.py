#!/usr/bin/env python3
"""Synthetic locale resolution snapshots for transport validation."""
import os
import struct
import sys


def number(value):
    return struct.pack('<I', value)


def header(status=0):
    return number(0x50535957) + number(1) + number(status)


def response(result, error, output):
    return header() + number(result) + number(error) + number(len(output)) + output


def make_response(arguments):
    mode = os.environ.get('WIBO_FIXTURE_RESOLVE_RESPONSE', 'success')
    name = 'en-US'.encode('utf-16-le').hex()
    capacity, seed = '8', 'a5' * 16
    if mode == 'null-name':
        name = '-'
    elif mode == 'invariant':
        name = ''
    elif mode == 'short-mutated':
        capacity, seed = '5', 'a5' * 10
    elif mode in ('query-data', 'query-overflow'):
        capacity, seed = '0', '-'
    if mode == 'success' and arguments[2:4] in (['0', '-'], ['0', '']):
        capacity, seed = arguments[2:4]
    if arguments != ['resolve-locale-name', name, capacity, seed, '17185']:
        return header(87)

    text = 'en-US\0'.encode('utf-16-le')
    output = text + b'\xa5' * 4 if capacity == '8' else b''
    value = response(6, 17185, output)
    if mode == 'invariant':
        value = response(1, 17185, b'\0\0' + b'\xa5' * 14)
    elif mode == 'success-cleared':
        value = response(6, 0, output)
    elif mode == 'short-mutated':
        value = response(0, 122, 'en-U\0'.encode('utf-16-le'))
    elif mode == 'false-zero':
        value = response(0, 0, b'\xa5' * 16)
    elif mode == 'truncated':
        value = value[:-1]
    elif mode == 'trailing':
        value += b'\0'
    elif mode == 'wrong-size':
        value = response(6, 17185, output[:-1])
    elif mode == 'overflow-result':
        value = response(9, 17185, output)
    elif mode == 'unterminated':
        value = response(6, 17185, 'en-US!'.encode('utf-16-le') + b'\xa5' * 4)
    elif mode == 'early-null':
        value = response(6, 17185, 'en\0US\0'.encode('utf-16-le') + b'\xa5' * 4)
    elif mode == 'query-data':
        value = response(6, 17185, text)
    elif mode == 'query-overflow':
        value = response(86, 17185, b'')
    elif mode == 'failed':
        value = header(5)
    elif mode == 'failed-with-data':
        value = header(5) + number(0)
    return value


sys.stdout.buffer.write(make_response(sys.argv[1:]))
