#!/usr/bin/env python3
"""Synthetic search tuples test framing and guest writes, not collation accuracy."""
import os
import struct
import sys


def number(value):
    return struct.pack('<I', value & 0xffffffff)


def header(status=0):
    return number(0x50535957) + number(1) + number(status)


def native_response(index=1, error=17185, present=1, length=2):
    return header() + number(index) + number(error) + number(present) + number(length)


def make_response(arguments):
    mode = os.environ.get('WIBO_FIXTURE_NLS_FIND_RESPONSE', 'match')
    source_count, source = '6', 'xAbxAb'.encode('utf-16-le').hex()
    value_count, value = '2', 'Ab'.encode('utf-16-le').hex()
    flags, requested = '4194304', '1'
    if mode in ('no-match', 'no-match-zero', 'failure-found'):
        source_count, source = '3', 'xyz'.encode('utf-16-le').hex()
    elif mode == 'zero-index':
        source_count, source = '2', value
    elif mode == 'terminated':
        flags = '8388608'
        source_count, source = '-1', 'Ab--Ab\0'.encode('utf-16-le').hex()
        value_count, value = '-1', 'Ab\0'.encode('utf-16-le').hex()
    if mode in ('optional', 'unexpected-found'):
        requested = '0'
    expected = ['find-nls-string-ex', flags, 'en-US'.encode('utf-16-le').hex(),
                source_count, source, value_count, value, requested, '17185']
    if arguments != expected:
        return header(87)

    response = native_response()
    if mode == 'zero-index':
        response = native_response(index=0)
    elif mode == 'terminated':
        response = native_response(index=4)
    elif mode == 'optional':
        response = native_response(present=0, length=0)
    elif mode == 'no-match':
        response = native_response(index=-1, present=0, length=0)
    elif mode == 'no-match-zero':
        response = native_response(index=-1, error=0, present=0, length=0)
    elif mode == 'cleared-error':
        response = native_response(error=0)
    elif mode == 'failed':
        response = header(5)
    elif mode == 'unavailable':
        response = header(0x80041001)
    elif mode == 'failed-with-data':
        response = header(5) + number(0)
    elif mode == 'truncated':
        response = response[:-1]
    elif mode == 'trailing':
        response += b'\0'
    elif mode == 'bad-magic':
        response = number(0) + response[4:]
    elif mode == 'bad-version':
        response = response[:4] + number(2) + response[8:]
    elif mode == 'negative-index':
        response = native_response(index=-2)
    elif mode == 'large-index':
        response = native_response(index=7)
    elif mode == 'large-length':
        response = native_response(length=6)
    elif mode == 'negative-length':
        response = native_response(length=-1)
    elif mode == 'bad-presence':
        response = native_response(present=2)
    elif mode == 'missing-found':
        response = native_response(present=0, length=0)
    elif mode == 'absent-with-value':
        response = native_response(present=0, length=2)
    elif mode == 'failure-found':
        response = native_response(index=-1, error=0)
    # unexpected-found deliberately returns a present scalar for a NULL request.
    return response


sys.stdout.buffer.write(make_response(sys.argv[1:]))
