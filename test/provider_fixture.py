#!/usr/bin/env python3
"""Deterministic typed service responses for guest ABI and transport checks."""
import struct
import ctypes
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
if operation == 'known-folder-path':
    identity, flags, user = arguments
    paths = {'825dab62c1fdc34da9dd070d1d495d97': 'C:\\ProgramData',
             '8f856c5e220e60479afeea3317b67173': 'C:\\Fixture\\profile-\u4e2d-\U0001f600'}
    path = paths.get(identity)
    response = header() + text(path) if path else header(0x80070057)
    fault = os.environ.get('WIBO_FIXTURE_FOLDER_RESPONSE')
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'embedded-zero':
        response = header() + text('C:\\Fixture\0suffix')

elif operation == 'api-set-host':
    response = header(126) if 'absent-synthetic' in arguments[0] else header() + text('kernel32.dll')
    fault = os.environ.get('WIBO_FIXTURE_API_SET_RESPONSE')
    if fault == 'failed':
        response = header(126)
    elif fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'path':
        response = header() + text('..\\kernel32.dll')
    elif fault == 'embedded-zero':
        response = header() + text('kernel32.dll\0suffix')
    elif fault == 'contract-host':
        response = header() + text(arguments[0])

elif operation == 'numa-highest-node-number':
    response = header() + number(3)
    fault = os.environ.get('WIBO_FIXTURE_NUMA_RESPONSE')
    if fault == 'failed':
        response = header(5)
    elif fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'

elif operation in ('time-zone-information', 'dynamic-time-zone-information'):
    transition = lambda month, week, hour: struct.pack('<8H', 0, month, 0, week, hour, 0, 0, 0)
    zone = number(300) + bytes(64) + transition(11, 1, 2) + number(0)
    zone += bytes(64) + transition(3, 2, 2) + number(-60)
    if operation == 'dynamic-time-zone-information':
        key = 'Synthetic Zone'.encode('utf-16-le')
        zone += key + bytes(256 - len(key)) + bytes(4)
    response = header() + number(2) + blob(zone)
    fault = os.environ.get('WIBO_FIXTURE_ZONE_RESPONSE')
    if fault == 'failed':
        response = header(5)
    elif fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'invalid-state':
        response = header() + number(3) + blob(zone)
elif operation == 'environment-defaults':
    values = [('APPDATA', 'C:\\Fixture\\Roaming'), ('LOCALAPPDATA', 'C:\\Fixture\\Local')]
    response = header() + number(len(values)) + b''.join(blob(name.encode()) + blob(value.encode())
                                                       for name, value in values)
    fault = os.environ.get('WIBO_FIXTURE_ENV_RESPONSE')
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
elif operation == 'management-connect':
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
elif operation == 'best-route':
    luid, index, options, source, destination = arguments
    destination = bytes.fromhex(destination)
    route = bytearray(104)
    struct.pack_into('<QI', route, 0, int(luid) if luid != 'none' else 0x0102030405060708, 1)
    route[12:40] = destination
    route[40] = 32
    struct.pack_into('<H', route, 44, 2)
    fault = os.environ.get('WIBO_FIXTURE_ROUTE_RESPONSE')
    response = header() + blob(route[:-1] if fault == 'bad-size' else route) + blob(destination)
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'failed':
        response = header(2)
elif operation == 'ip-adapter-addresses':
    family, flags, width = map(int, arguments)
    pointer = ctypes.c_uint64 if width == 8 else ctypes.c_uint32

    class SocketAddress(ctypes.Structure):
        _fields_ = [('pointer', pointer), ('length', ctypes.c_int32)]

    class Address(ctypes.Structure):
        _fields_ = [('header', ctypes.c_uint64), ('next', pointer), ('socket', SocketAddress)]

    class Prefix(ctypes.Structure):
        _fields_ = Address._fields_ + [('prefix_length', ctypes.c_uint32)]

    class Adapter(ctypes.Structure):
        _fields_ = [('header', ctypes.c_uint64)] + [
            (name, pointer) for name in ['next', 'name', 'unicast', 'anycast', 'multicast',
                                        'dns_servers', 'suffix', 'description', 'friendly']
        ] + [('physical', ctypes.c_uint8 * 8)] + [
            (name, ctypes.c_uint32) for name in ['physical_length', 'flags', 'mtu', 'type', 'status', 'ipv6_index']
        ] + [('zones', ctypes.c_uint32 * 16), ('prefix', pointer)]

    storage = bytearray()
    fixups = []

    def append(data, alignment=8):
        storage.extend(b'\0' * (-len(storage) % alignment))
        offset = len(storage)
        storage.extend(data)
        return offset

    def link(position, target):
        struct.pack_into('<Q' if width == 8 else '<I', storage, position, target)
        fixups.append(position)

    previous = None
    for index, af in enumerate([2, 23] if family == 0 else [family]):
        adapter = append(bytes(Adapter()))
        struct.pack_into('<II', storage, adapter, ctypes.sizeof(Adapter), index + 1)
        if previous is not None:
            link(previous + Adapter.next.offset, adapter)
        previous = adapter
        name = append(('interface-%d' % index).encode() + b'\0', 1)
        friendly = append('Interface \u4e2d\U0001f600'.encode('utf-16-le') + b'\0\0', 2)
        link(adapter + Adapter.name.offset, name)
        link(adapter + Adapter.friendly.offset, friendly)
        struct.pack_into('<I', storage, adapter + Adapter.mtu.offset, 1500)
        struct.pack_into('<I', storage, adapter + Adapter.type.offset, 24)
        struct.pack_into('<I', storage, adapter + Adapter.status.offset, 1)
        node = append(bytes(Address()))
        prefix = append(bytes(Prefix()))
        sockaddr = struct.pack('<HH', af, 0) + (b'\x7f\0\0\x01' + b'\0' * 8 if af == 2
                                              else b'\0' * 19 + b'\x01' + b'\0' * 4)
        address = append(sockaddr)
        link(adapter + Adapter.unicast.offset, node)
        link(adapter + Adapter.prefix.offset, prefix)
        struct.pack_into('<I', storage, node, ctypes.sizeof(Address))
        struct.pack_into('<I', storage, prefix, ctypes.sizeof(Prefix))
        for position in [node + Address.socket.offset, prefix + Prefix.socket.offset]:
            link(position + SocketAddress.pointer.offset, address)
            struct.pack_into('<i', storage, position + SocketAddress.length.offset, len(sockaddr))
        struct.pack_into('<I', storage, prefix + Prefix.prefix_length.offset, 8 if af == 2 else 128)
    fixups.sort()
    fault = os.environ.get('WIBO_FIXTURE_ADAPTER_RESPONSE')
    if fault == 'outside':
        struct.pack_into('<Q' if width == 8 else '<I', storage, fixups[0], len(storage))
    elif fault == 'overlap':
        fixups.insert(1, fixups[0])
    response = header() + number(width + 1 if fault == 'width' else width) + blob(storage)
    response += number(len(fixups)) + b''.join(number(offset) for offset in fixups)
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'failed':
        response = header(5)
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
elif operation == 'system-query':
    kind, capacity = map(int, arguments)
    status, payload = 0, b''
    if kind == 3:
        if capacity > 48:
            status = 0xc0000004
        else:
            current = int(time.time() * 10000000) + 116444736000000000
            payload = struct.pack('<qqqIIQQ', current - 36000000000, current, 0, 0, 0, 0, 0)[:capacity]
    elif kind == 8:
        if capacity < 48:
            status = 0xc0000004
        else:
            cpu = struct.pack('<qqqqqII', 0x100000001, 0x200000002, 0x300000003, 0, 0, 3, 0)
            payload = cpu * min(capacity // 48, 2)
    response = header() + number(status) + number(len(payload)) + blob(payload)
    fault = os.environ.get('WIBO_FIXTURE_SYSTEM_RESPONSE')
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'oversized':
        response = header() + number(0) + number(capacity + 1) + blob(payload)
    elif fault == 'failed':
        response = header(5)
elif operation == 'volume-query':
    path, kind, capacity = arguments
    kind, capacity = int(kind), int(capacity)
    payloads = {3: struct.pack('<qqII', 4096, 1024, 8, 512),
                4: struct.pack('<II', 7, 0),
                5: struct.pack('<III', 3, 255, 18) + 'FixtureFS'.encode('utf-16-le'),
                7: struct.pack('<qqqII', 4096, 1024, 2048, 8, 512)}
    payload = payloads.get(kind, b'')
    status = 0 if payload else 0xc0000002
    if capacity < len(payload):
        status, payload = 0xc0000023, b''
    response = header() + number(status) + number(len(payload)) + number(0) + blob(payload)
    fault = os.environ.get('WIBO_FIXTURE_VOLUME_RESPONSE')
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'oversized':
        response = header() + number(0) + number(0) + number(1) + blob(payload)
    elif fault == 'failed':
        response = header(5)
elif operation == 'status-error':
    status = int(arguments[0])
    values = {0: 0, 0xc0000022: 5, 0xc0000008: 6, 0xc000000d: 87, 0x80000005: 234,
              0x20000001: 0x20000001, 0xd0000022: 5, 0xc0070005: 5}
    response = header() + number(values.get(status, 317))
    if os.environ.get('WIBO_FIXTURE_STATUS_RESPONSE') == 'truncated':
        response = response[:-1]
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
