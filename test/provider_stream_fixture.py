#!/usr/bin/env python3
"""Changing responses and interrupted frames for persistent transport checks."""
import os
import pathlib
import struct
import sys
import subprocess
import time


def read(size):
    data = bytearray()
    while len(data) < size:
        part = sys.stdin.buffer.read(size - len(data))
        if not part:
            return None
        data.extend(part)
    return bytes(data)


assert sys.argv[1:] == ['--serve']
mode = os.environ.get('WIBO_FIXTURE_STREAM_MODE', 'fresh')
marker = pathlib.Path(os.environ['WIBO_FIXTURE_STREAM_MARKER'])
fault_marker = marker.with_suffix('.fault')
first = not fault_marker.exists()
marker.write_text(str(os.getpid()))
count = 0
while True:
    length = read(4)
    if length is None:
        break
    size = struct.unpack('<I', length)[0]
    assert 4 <= size <= 65536
    request = read(size)
    assert request is not None
    argc = struct.unpack_from('<I', request)[0]
    arguments, offset = [], 4
    for index in range(argc):
        length = struct.unpack_from('<I', request, offset)[0]
        offset += 4
        arguments.append(request[offset:offset + length].decode('utf-8'))
        offset += length
    assert offset == len(request)
    if arguments != ['time-zone-information']:
        result = subprocess.run([sys.executable, str(pathlib.Path(__file__).with_name('provider_fixture.py')),
                                 *arguments], stdout=subprocess.PIPE, check=True)
        sys.stdout.buffer.write(struct.pack('<I', len(result.stdout)) + result.stdout)
        sys.stdout.buffer.flush()
        continue
    if first and mode in ('exit', 'truncated', 'oversized', 'timeout'):
        fault_marker.touch()
        if mode == 'oversized':
            sys.stdout.buffer.write(struct.pack('<I', 8 * 1024 * 1024 + 1))
        elif mode == 'truncated':
            sys.stdout.buffer.write(struct.pack('<I', 192) + bytes(3))
        elif mode == 'timeout':
            time.sleep(12)
        sys.stdout.buffer.flush()
        sys.exit(0)
    count += 1
    zone = struct.pack('<i', 100 + count) + bytes(168)
    response = struct.pack('<IIIII', 0x50535957, 1, 0, 0, len(zone)) + zone
    frame = struct.pack('<I', len(response)) + response
    sys.stdout.buffer.write(frame[:3])
    sys.stdout.buffer.flush()
    sys.stdout.buffer.write(frame[3:])
    sys.stdout.buffer.flush()
