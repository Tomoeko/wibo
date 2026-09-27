#!/usr/bin/env python3
"""Synthetic MUI failure receipts for transport validation."""
import os
import struct
import sys


def number(value):
    return struct.pack('<I', value)


def header(status=0):
    return number(0x50535957) + number(1) + number(status)


def blob(value):
    return number(len(value)) + value


def response(result, error, language_count, path_count, enumerator, language, path):
    return (header() + number(result) + number(error) + number(language_count)
            + number(path_count) + blob(enumerator) + blob(language) + blob(path))


def make_response(arguments):
    mode = os.environ.get('WIBO_FIXTURE_FILE_MUI_RESPONSE', 'unchanged')
    filename = 'C:\\Fixture\\resource.dll'.encode('utf-16-le').hex()
    if mode == 'long-filename':
        filename = ('a' * 128).encode('utf-16-le').hex()
    language = 'en-US\0'.encode('utf-16-le') + b'\xa5' * 4
    path = b'\xa5' * 32
    language_count, path_count = 8, 16
    language_seed, path_seed = language.hex(), path.hex()
    if mode == 'query':
        language_count = path_count = 0
        language = path = b''
        language_seed = path_seed = '-'
    expected = ['file-mui-path', '584', filename, str(language_count), language_seed,
                str(path_count), path_seed, '0000000000000000', '17185']
    if arguments != expected:
        return header(87)

    result, error = 0, 120
    enumerator = b'\0' * 8
    if mode == 'changed-size':
        language_count += 1
        path_count += 1
    elif mode == 'changed-language':
        language = b'X\0' + language[2:]
    elif mode == 'changed-path':
        path = b'X\0' + path[2:]
    elif mode == 'changed-enumerator':
        enumerator = enumerator[:-1] + b'\1'
    elif mode == 'true':
        result, error = 1, 0
    elif mode == 'other-error':
        error = 2
    elif mode == 'bad-bool':
        result = 2
    elif mode == 'enum-width':
        enumerator = enumerator[:-1]
    elif mode == 'blob-width':
        language = language[:-1]

    value = response(result, error, language_count, path_count, enumerator, language, path)
    if mode == 'truncated':
        value = value[:-1]
    elif mode == 'trailing':
        value += b'\0'
    elif mode == 'failed':
        value = header(5)
    elif mode == 'failed-with-data':
        value = header(5) + number(0)
    return value


sys.stdout.buffer.write(make_response(sys.argv[1:]))
