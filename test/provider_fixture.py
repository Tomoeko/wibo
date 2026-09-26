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
elif operation in ('set-file-security-a', 'set-file-security-w'):
    descriptor = bytes.fromhex(arguments[2])
    revision, reserved, control, owner, group, sacl, dacl = struct.unpack_from('<BBHIIII', descriptor)
    valid = revision == 1 and control == 0x8004 and dacl == 20 and len(descriptor) == 48 and arguments[1] == '4'
    response = header(0 if valid else 1338)
elif operation in ('file-security-a', 'file-security-w'):
    path = bytes.fromhex(arguments[0]).decode('ascii') if operation.endswith('-a') else arguments[0]
    sid = bytes([1, 1, 0, 0, 0, 0, 0, 5]) + number(18)
    ace = struct.pack('<BBH', 0, 0, 20) + number(0x80000000) + sid
    acl = struct.pack('<BBHHH', 2, 0, 28, 1, 0) + ace
    descriptor = struct.pack('<BBHIIII', 1, 0, 0x8004, 0, 0, 0, 20) + acl
    if path == 'bad-offset':
        descriptor = struct.pack('<BBHIIII', 1, 0, 0x8004, 0, 0, 0, 0xffffffff) + acl
    elif path == 'bad-ace':
        descriptor = descriptor[:-1]
    response = header() + blob(descriptor)
elif operation == 'user-name':
    response = header() + text('FixtureUser') + blob(b'FixtureUser')
elif operation in ('account-lookup-a', 'account-lookup-w'):
    name = bytes.fromhex(arguments[1]).decode('ascii') if operation.endswith('-a') else arguments[1]
    if name != 'FixtureUser':
        response = header(1332)
    else:
        sid = bytes([1, 5, 0, 0, 0, 0, 0, 5]) + struct.pack('<IIIII', 21, 11, 22, 33, 1001)
        domain = b'FixtureHost' if operation.endswith('-a') else 'FixtureHost'.encode('utf-16-le')
        response = header() + blob(sid) + blob(domain) + number(1)
elif operation.startswith('registry-'):
    path = arguments[0].lower()
    exists = path == 'hkey_current_user\\software\\wiboproviderfixture'
    response = header(0 if exists else 2)
    if operation in ('registry-query', 'registry-snapshot') and exists:
        values = {
            'text': (1, '\u4e2d\0\U0001f600\0'.encode('utf-16-le')),
            'number': (4, number(0xfedcba98)),
            'view': (4, number(int(arguments[-1]))),
            '': (3, b'\0\xff\x17'),
        }
        if operation == 'registry-snapshot':
            response = header() + number(len(values))
            for name, (kind, data) in values.items():
                response += text(name) + number(kind) + blob(data)
        else:
            value = values.get(arguments[1].lower())
            response = header() + number(value[0]) + blob(value[1]) if value else header(2)
sys.stdout.buffer.write(response)
