#!/usr/bin/env python3
"""Deterministic typed service responses for guest ABI and transport checks."""
import struct
import os
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
elif operation == 'format-message':
    width, flags, message_id, language = arguments
    response = header(317)
    if message_id == '5':
        value = 'Access is denied.\r\n'
        response = header() + blob(value.encode('utf-16-le' if width == 'w' else 'ascii'))
elif operation == 'network-connectivity':
    response = header() + number(0x420) + number(1) + number(1)
    fault = os.environ.get('WIBO_FIXTURE_NETWORK_RESPONSE')
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'bad-flags':
        response = header() + number(0x8000) + number(1) + number(1)
    elif fault == 'failed':
        response = header(0x80070005)
elif operation == 'ip-address-table':
    rows = [struct.pack('<IIIIIHH', address, index, 0x00ffffff, 1, 65535, 0, 1)
            for address, index in [(0x0100007f, 1), (0x0100000a, 2), (0x0200000a, 3)]]
    if arguments == ['1']:
        rows.sort(key=lambda row: row[:4])
    response = header() + number(len(rows)) + b''.join(rows)
    fault = os.environ.get('WIBO_FIXTURE_IP_TABLE_RESPONSE')
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'bad-count':
        response = header() + number(0xffffffff)
    elif fault == 'failed':
        response = header(5)
elif operation == 'memory-status':
    response = header() + blob(struct.pack('<II7Q', 64, 25, 16 << 30, 12 << 30, 20 << 30, 14 << 30, 1 << 47, (1 << 47) - 65536, 0))
    fault = os.environ.get('WIBO_FIXTURE_MEMORY_RESPONSE')
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'bad-load':
        response = header() + blob(struct.pack('<II7Q', 64, 101, 16 << 30, 12 << 30, 20 << 30, 14 << 30, 1 << 47, (1 << 47) - 65536, 0))
elif operation == 'system-metrics':
    index, last_error = map(int, arguments)
    values = {0: 1280, 1: 720, 11: 32, 12: 32, 13: 32, 14: 32, 76: -1280, 80: 2}
    response = header() + number(values.get(index, 0)) + number(last_error)
    fault = os.environ.get('WIBO_FIXTURE_METRICS_RESPONSE')
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'failed':
        response = header(5)
elif operation == 'image-load':
    image_type, image, kind, name = arguments
    if kind != 'id' or image:
        response = header(1814)
    else:
        width = 0xffffffff if name == '65534' else 1
        response = header() + number(1 if image_type == 'icon' else 0) + number(0) + number(0)
        response += b''.join(number(value) for value in (width, 2, 2, 1, 1)) + blob(b'\xff\xff\0\0') + number(0)
        if name == '65535':
            response = response[:-1]
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
