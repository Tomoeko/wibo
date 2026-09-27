#!/usr/bin/env python3
"""Bounded native reference receipts for synthetic source-message transport."""
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


def blob(value):
    return number(len(value)) + value


def decode(value):
    assert len(value) <= 65536
    return b'' if value == '-' else bytes.fromhex(value)


def read_arguments(data):
    assert len(data) >= 4
    count, = struct.unpack_from('<I', data)
    assert count <= 99
    cursor = 4
    result = []
    for _ in range(count):
        assert cursor + 8 <= len(data)
        kind, value = struct.unpack_from('<II', data, cursor)
        cursor += 8
        assert kind <= 2
        if kind:
            assert cursor + value <= len(data)
            value = data[cursor:cursor + value]
            cursor += len(value)
        result.append((kind, value))
    assert cursor == len(data)
    return result


def wrapper():
    executable, fixture = (str(Path(value).resolve()) for value in sys.argv[1:3])
    mode = sys.argv[3]
    with tempfile.TemporaryDirectory(prefix='wibo-message-string-') as directory:
        state = Path(directory) / 'requests'
        environment = os.environ.copy()
        environment.update(WIBO_SYSTEM_PROVIDER=str(Path(__file__).resolve()),
                           WIBO_SYSTEM_PROVIDER_PERSISTENT='0',
                           WIBO_FIXTURE_MESSAGE_STRING_STATE=str(state),
                           WIBO_FIXTURE_MESSAGE_STRING_RESPONSE='success' if mode == 'all' else mode)
        if mode == 'unconfigured':
            environment.pop('WIBO_SYSTEM_PROVIDER', None)
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
            raise AssertionError('message fixture timed out; owned group terminated') from None
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        sys.stdout.buffer.write(output)
        sys.stderr.buffer.write(errors)
        assert process.returncode == 0, f'message fixture exited {process.returncode}'
        count = int(state.read_text()) if state.exists() else 0
        expected = 35 if mode == 'all' else 0 if mode in ('unconfigured', 'local-limits') else 2 if mode == 'retry' else 1
        assert count == expected, f'provider requests {count}, expected {expected}'


modes = {'all', 'failed', 'truncated', 'trailing', 'bad-magic', 'bad-version',
         'status-trailing', 'wrong-size', 'excess-count', 'missing-zero', 'negative-count',
         'allocated-size', 'allocated-zero', 'retry', 'unavailable', 'unconfigured', 'local-limits'}
if len(sys.argv) == 4 and sys.argv[3] in modes and Path(sys.argv[1]).is_file():
    wrapper()
    raise SystemExit(0)

# Other runtime facilities can consult the same optional provider. They are unavailable
# in this focused mock and must not affect the source-message request sequence.
if sys.argv[1:2] != ['format-message-string']:
    sys.stdout.buffer.write(header(0x80041001))
    raise SystemExit(0)
if len(sys.argv) != 9:
    sys.stdout.buffer.write(header(13))
    raise SystemExit(0)

operation, api, flags, capacity, encoded_source, encoded_arguments, encoded_seed, incoming = sys.argv[1:]
assert operation == 'format-message-string' and api in ('a', 'w')
state_name = os.environ.get('WIBO_FIXTURE_MESSAGE_STRING_STATE')
if state_name:
    state = Path(state_name)
    count = int(state.read_text()) + 1 if state.exists() else 1
    assert count <= 64
    state.write_text(str(count))
assert sum(map(len, (encoded_source, encoded_arguments, encoded_seed))) <= 65536 - 256
flags, capacity, incoming = int(flags), int(capacity), int(incoming)
assert flags & 0x400 and not flags & ~0x37ff
assert incoming == 0x4321
unit = 2 if api == 'w' else 1
encoding = 'utf-16-le' if unit == 2 else 'latin-1'
source_bytes = decode(encoded_source)
source = source_bytes.decode(encoding, errors='surrogatepass')
arguments = read_arguments(decode(encoded_arguments))
seed = decode(encoded_seed)
allocated = bool(flags & 0x100)
assert (not seed) if allocated else len(seed) == capacity * unit
assert allocated or seed == b'\xa5' * len(seed)


def text(value):
    return value.encode(encoding, errors='surrogatepass')


def string_argument(value):
    return (1 if unit == 2 else 2, text(value))


# Exact receipts independently measured with the public native source-message cases.
no_arguments = {
    'a%%b%.%!% c%tD%rE%nF%0': 'a%b.! c\tD\rE\r\nF',
    'a%qz': 'aqz',
    '%1 %2!08X! %% %n end': '%1 %2!08X! %% \r\n end',
    'one two three four%nlongword': 'one two\r\nthree\r\nfour\r\nlongword\r\n',
    '4294967298': '4294967298',
    '\x80\xe9': '\x80\xe9',
}
if source == 'one\ntwo\rthree\r\nfour%nend':
    output = 'one two three four\r\nend' if flags & 255 == 255 else 'one\r\ntwo\r\nthree\r\nfour\r\nend'
    expected_arguments = []
elif source in no_arguments:
    output = no_arguments[source]
    expected_arguments = []
elif source == '%1!s!/%2!u!/%3!s!/%4!s!':
    expected_arguments = [string_argument('first'), (0, 37), string_argument('third'), string_argument('first')]
    output = 'first/37/third/first'
elif source == '%1!d!/%2!05u!/%3!08X!':
    expected_arguments = [(0, 0xfffffff9), (0, 42), (0, 0x12af)]
    output = '-7/00042/000012AF'
elif source == '%1!*.*s! %4!s! %5!*s!':
    expected_arguments = [(0, 4), (0, 2), string_argument('Bill'), string_argument('Bob'), (0, 6), string_argument('Bill')]
    output = '  Bi Bob   Bill'
elif source == '%1!u! %2!*s!':
    expected_arguments = [(0, 37), (0, 4), string_argument('Next')]
    output = '37 Next'
elif source == '%1!*s! %3!*s!':
    expected_arguments = [(0, 4), string_argument('Bill'), (0, 4), string_argument('Next')]
    output = 'Bill Next'
elif source == '%1!*.*s! %4!*s!':
    expected_arguments = [(0, 4), (0, 2), string_argument('Bill'), (0, 4), string_argument('Next')]
    output = '  Bi Next'
elif source == '%1!u! %2!*.*s!':
    expected_arguments = [(0, 37), (0, 4), (0, 2), string_argument('Next')]
    output = '37   Ne'
elif source == '%1!s!':
    expected_arguments = [string_argument('slot')]
    output = 'slot'
elif source == '%1!s!/%2!d!/%3!s!':
    expected_arguments = [string_argument('first'), (0, 0xfffffff9), string_argument('first')]
    output = 'first/-7/first'
else:
    raise AssertionError('Unexpected synthetic source')
assert arguments == expected_arguments
result, native_error = len(output), incoming
output_bytes = text(output) + b'\0' * unit
if not allocated:
    assert result < capacity
    output_bytes += seed[len(output_bytes):]
response = header() + number(result) + number(native_error) + blob(output_bytes)
mode = os.environ.get('WIBO_FIXTURE_MESSAGE_STRING_RESPONSE', 'success')
if mode == 'retry':
    assert state_name and count <= 2
    mode = 'truncated' if count == 1 else 'success'
if mode == 'unavailable':
    response = header(0x80041001)
elif mode == 'failed':
    response = header(5)
elif mode == 'truncated':
    response = response[:-1]
elif mode == 'trailing':
    response += b'\0'
elif mode == 'bad-magic':
    response = number(0) + response[4:]
elif mode == 'bad-version':
    response = response[:4] + number(2) + response[8:]
elif mode == 'status-trailing':
    response = header(5) + number(0)
elif mode == 'wrong-size':
    response = header() + number(result) + number(native_error) + blob(output_bytes[:-unit])
elif mode == 'misaligned':
    response = header() + number(result) + number(native_error) + blob(output_bytes + b'\x01')
elif mode == 'excess-count':
    response = header() + number(capacity) + number(native_error) + blob(output_bytes)
elif mode == 'missing-zero':
    broken = bytearray(output_bytes)
    broken[result * unit] = 1
    response = header() + number(result) + number(native_error) + blob(broken)
elif mode == 'negative-count':
    response = header() + number(0xffffffff) + number(native_error) + blob(output_bytes)
elif mode == 'allocated-size':
    assert allocated
    response = header() + number(result) + number(native_error) + blob(output_bytes + b'\0' * unit)
elif mode == 'allocated-zero':
    assert allocated
    response = header() + number(0) + number(235) + blob(b'\0' * unit)
else:
    assert mode == 'success'
sys.stdout.buffer.write(response)
