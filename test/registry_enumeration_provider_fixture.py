#!/usr/bin/env python3
"""Bounded synthetic subkey snapshots with cache, overlay and failure checks."""
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile


def number(value):
    return struct.pack('<I', value & 0xffffffff)


def header(status=0):
    return number(0x50535957) + number(1) + number(status)


def text(value):
    data = value.encode('utf-16-le')
    return number(len(data)) + data


def response(mode):
    if mode == 'failed':
        return header(5)
    if mode == 'unavailable':
        return None
    entries = [('NativeOne', 'NativeClassA', 0x12345678, 0x01234567),
               ('NativeTwo', 'NativeClassB', 0x23456789, 0x02345678)]
    if mode.startswith('ansi-'):
        entries = [('Né', '€', 0x12345678, 0x01234567)]
    if mode == 'duplicate':
        entries[1] = ('nativeone', 'NativeClassB', 0x23456789, 0x02345678)
    elif mode == 'empty-name':
        entries[1] = ('', 'NativeClassB', 0x23456789, 0x02345678)
    elif mode == 'nested-name':
        entries[1] = ('Outer\\Inner', 'NativeClassB', 0x23456789, 0x02345678)
    elif mode == 'embedded-null':
        entries[1] = ('Native\0Two', 'NativeClassB', 0x23456789, 0x02345678)
    frame = header() + number(len(entries))
    for name, key_class, low, high in entries:
        frame += text(name) + text(key_class) + number(low) + number(high)
    if mode == 'truncated':
        frame = frame[:-1]
    elif mode == 'trailing':
        frame += b'x'
    elif mode == 'huge-count':
        frame = frame[:12] + number(0xffffffff) + frame[16:]
    elif mode == 'failed-with-data':
        frame = header(5) + frame[12:]
    return frame


def provider(arguments):
    if len(arguments) == 2 and arguments[0] == 'registry-ansi-string':
        state = Path(os.environ['WIBO_FIXTURE_REGISTRY_ENUM_STATE'] + '.ansi')
        count = int(state.read_text()) + 1 if state.exists() else 1
        state.write_text(str(count))
        mode = os.environ['WIBO_FIXTURE_REGISTRY_ENUM_RESPONSE']
        source = bytes.fromhex(arguments[1]).decode('utf-16-le')
        if mode == 'ansi-failed':
            frame = header(5)
        else:
            data = {'Né': b'N\xe9', '€': b'\x80'}[source]
            if mode == 'ansi-zero':
                data += b'\0'
            frame = header() + number(len(data)) + data
            if mode == 'ansi-truncated':
                frame = frame[:-1]
            elif mode == 'ansi-trailing':
                frame += b'x'
        sys.stdout.buffer.write(frame)
        return
    if len(arguments) != 3 or arguments[2] not in ('32', '64'):
        raise SystemExit(2)
    operation, path, _view = arguments
    if operation in ('registry-snapshot', 'registry-open'):
        base = path.lower().endswith('\\registryenumerationprovider')
        sys.stdout.buffer.write(header() + number(0) if base and operation == 'registry-snapshot'
                                else header(0 if base else 2))
        return
    if operation != 'registry-subkeys':
        raise SystemExit(2)
    state = Path(os.environ['WIBO_FIXTURE_REGISTRY_ENUM_STATE'])
    count = int(state.read_text()) + 1 if state.exists() else 1
    state.write_text(str(count))
    mode = os.environ['WIBO_FIXTURE_REGISTRY_ENUM_RESPONSE']
    if mode == 'retry':
        mode = 'truncated' if count == 1 else 'success' if count == 2 else 'failed'
    elif mode == 'success' and count > 1:
        mode = 'failed'
    frame = response(mode)
    if frame is None:
        raise SystemExit(2)
    sys.stdout.buffer.write(frame)


def main():
    if len(sys.argv) > 1 and sys.argv[1].startswith('registry-'):
        provider(sys.argv[1:])
        return
    if len(sys.argv) != 4:
        raise SystemExit('usage: registry_enumeration_provider_fixture.py wibo fixture mode')
    executable, fixture = (str(Path(path).resolve()) for path in sys.argv[1:3])
    mode = sys.argv[3]
    with tempfile.TemporaryDirectory(prefix='wibo-registry-enumeration-') as directory:
        state = Path(directory) / 'requests'
        environment = os.environ.copy()
        environment.update(WIBO_SYSTEM_PROVIDER=str(Path(__file__).resolve()),
                           WIBO_SYSTEM_PROVIDER_PERSISTENT='0',
                           WIBO_FIXTURE_REGISTRY_ENUM_STATE=str(state),
                           WIBO_FIXTURE_REGISTRY_ENUM_RESPONSE=mode)
        process = subprocess.Popen([executable, fixture, mode], env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try:
            output, errors = process.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            output, errors = process.communicate(timeout=5)
            sys.stderr.buffer.write(output + errors)
            raise AssertionError('registry fixture timed out; owned process group terminated') from None
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        sys.stdout.buffer.write(output)
        sys.stderr.buffer.write(errors)
        if process.returncode:
            raise AssertionError(f'registry fixture exited {process.returncode}')
        expected = 1 if mode == 'success' or mode.startswith('ansi-') else 2
        count = int(state.read_text()) if state.exists() else 0
        if count != expected:
            raise AssertionError(f'provider requests {count}, expected {expected}')
        if mode.startswith('ansi-'):
            conversion_count = int(Path(str(state) + '.ansi').read_text())
            if conversion_count != 2:
                raise AssertionError(f'conversion requests {conversion_count}, expected 2')



if __name__ == '__main__':
    main()
