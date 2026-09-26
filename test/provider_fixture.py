#!/usr/bin/env python3
"""Deterministic typed service responses for guest ABI and transport checks."""
import struct
import sys
import time


def number(value):
    return struct.pack('<I', value & 0xffffffff)


def blob(data):
    return number(len(data)) + data


def text(value):
    return blob(value.encode('utf-16-le'))


def header(status=0):
    return number(0x50535957) + number(1) + number(status)


def property_value(name, cim_type, variant_type, data, status=0):
    return text(name) + number(cim_type) + number(0x20) + number(variant_type) + number(status) + blob(data)


operation, *arguments = sys.argv[1:]
response = header(0x80041008)
if operation == 'management-connect':
    response = header(0x8004100e if arguments[0] == 'root\\missing' else 0) + number(0)
elif operation == 'management-query':
    query = arguments[1]
    properties = [
        property_value('Caption', 8, 8, '\u4e2d\0\U0001f600'.encode('utf-16-le')),
        property_value('Version', 8, 8, '1.0'.encode('utf-16-le')),
        property_value('Flags', 19, 19, number(0xfedcba98)),
        property_value('Signed', 20, 20, struct.pack('<q', -0x100000001)),
        property_value('Optional', 8, 1, b''),
        property_value('Unsupported', 0x2008, 0x2008, b'', 0x8004100c),
    ]
    response = header() + number(1) + number(len(properties)) + b''.join(properties)
    if query == 'truncated':
        response = response[:-1]
    elif query == 'trailing':
        response += b'\0'
    elif query == 'bad-type':
        response = header() + number(1) + number(1) + property_value('Caption', 8, 19, b'\0')
    elif query == 'failed':
        response = header(0x80041010) + number(0)
    elif query == 'timeout':
        time.sleep(1)
    elif query == 'exit-failed':
        sys.stdout.buffer.write(response)
        sys.exit(1)
elif operation.startswith('registry-'):
    path = arguments[0].lower()
    exists = path == 'hkey_current_user\\software\\wiboproviderfixture'
    response = header(0 if exists else 2)
    if operation == 'registry-query' and exists:
        values = {
            'text': (1, '\u4e2d\0\U0001f600\0'.encode('utf-16-le')),
            'number': (4, number(0xfedcba98)),
            'view': (4, number(int(arguments[2]))),
            '': (3, b'\0\xff\x17'),
        }
        value = values.get(arguments[1].lower())
        response = header() + number(value[0]) + blob(value[1]) if value else header(2)
sys.stdout.buffer.write(response)
