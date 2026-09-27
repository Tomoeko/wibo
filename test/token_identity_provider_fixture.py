#!/usr/bin/env python3
"""Exercise token snapshot framing and successful immutable capture."""

import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile


def number(value):
    return struct.pack('<I', value)


def blob(value):
    return number(len(value)) + value


def sid(authority, *subauthorities):
    return bytes((1, len(subauthorities))) + authority.to_bytes(6, 'big') + b''.join(map(number, subauthorities))


def response(mode):
    header = number(0x50535957) + number(1)
    if mode == 'failed':
        return header + number(5)
    if mode == 'unavailable':
        return header + number(0x80041001)
    user = sid(5, 12345678)
    groups = [(4, sid(1, 0)), (20, sid(5, 32, 544)), (0, sid(5, 32, 545))]
    restricted = [(4, sid(1, 0))] if mode == 'restricted' else []
    native = mode.startswith('native-evaluated') or mode.startswith('bad-native-')
    if mode == 'bad-native-restricted':
        restricted = [(4, sid(1, 0))]
    statistics = struct.pack('<14I', 17, 0, 18, 0, 0xffffffff, 0x7fffffff,
                             1, 0, 0, 0, len(groups), 0, 19, 0)
    if mode == 'bad-stats-size':
        statistics = statistics[:-1]
    if mode == 'bad-stats-type':
        statistics = statistics[:24] + number(2) + statistics[28:]
    if mode == 'bad-stats-count':
        statistics = statistics[:40] + number(4) + statistics[44:]
    if mode == 'bad-sid-revision':
        user = bytes((2,)) + user[1:]
    if mode == 'bad-sid-count':
        user = user[:1] + bytes((16,)) + user[2:]
    if mode == 'bad-sid-size':
        user = user[:-1]
    frame = header + number(0) + number(2 if mode == 'bad-source' else 1) + blob(statistics)
    frame += number(1 if mode == 'user-attributes' else 0) + blob(user)
    frame += number(0xffffffff if mode == 'oversized-count' else len(groups))
    for attributes, identifier in groups:
        frame += number(attributes) + blob(identifier)
    frame += number(len(restricted))
    for attributes, identifier in restricted:
        frame += number(attributes) + blob(identifier)
    frame += blob(sid(1, 0)) + number(2 if mode == 'bad-elevation' else 1)
    disposition = 2 if native else 1
    restriction_error = 87 if mode == 'native-evaluated-87' else 50 if mode == 'native-evaluated-50' else 1 if native else 0
    decisions = [0, 1, 0, 0] if native else []
    if mode == 'bad-native-mode':
        disposition = 3
    if mode == 'bad-native-error':
        restriction_error = 5
    if mode == 'bad-native-count':
        decisions = decisions[:-1]
    if mode == 'bad-native-value':
        decisions[0] = 2
    if mode == 'bad-native-deny-only':
        decisions[2] = 1
    if mode == 'bad-native-disabled':
        decisions[3] = 1
    if mode == 'bad-list-decisions':
        decisions = [0, 1, 0, 0]
    if mode == 'bad-list-error':
        restriction_error = 1
    frame += number(disposition) + number(restriction_error) + number(len(decisions))
    frame += b''.join(map(number, decisions))
    if mode == 'truncated':
        frame = frame[:-1]
    if mode == 'trailing':
        frame += b'x'
    return frame


def provider():
    mode = os.environ.get('WIBO_FIXTURE_TOKEN_RESPONSE', '')
    state = Path(os.environ['WIBO_FIXTURE_TOKEN_STATE'])
    count = int(state.read_text()) + 1 if state.exists() else 1
    state.write_text(str(count))
    if mode == 'retry':
        mode = 'truncated' if count == 1 else 'success' if count == 2 else 'failed'
    elif (mode in ('success', 'restricted') or mode.startswith('native-evaluated')) and count > 1:
        mode = 'failed'
    sys.stdout.buffer.write(response(mode))


def main():
    if sys.argv[1:] == ['token-identity']:
        provider()
        return
    if len(sys.argv) != 4:
        raise SystemExit('usage: token_identity_provider_fixture.py wibo fixture mode')
    executable, fixture = (str(Path(path).resolve()) for path in sys.argv[1:3])
    mode = sys.argv[3]
    with tempfile.TemporaryDirectory(prefix='wibo-token-identity-') as directory:
        environment = os.environ.copy()
        environment.update(WIBO_SYSTEM_PROVIDER=str(Path(__file__).resolve()),
                           WIBO_SYSTEM_PROVIDER_PERSISTENT='0',
                           WIBO_FIXTURE_TOKEN_STATE=str(Path(directory) / 'requests'),
                           WIBO_FIXTURE_TOKEN_RESPONSE=mode)
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
            raise AssertionError('token fixture timed out; owned process group terminated') from None
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        sys.stdout.buffer.write(output)
        sys.stderr.buffer.write(errors)
        if process.returncode:
            raise AssertionError(f'token fixture exited {process.returncode}')
        count = int((Path(directory) / 'requests').read_text())
        captured = mode in ('success', 'restricted') or mode.startswith('native-evaluated')
        expected = 2 if mode == 'retry' else 1 if captured else 2
        if count != expected:
            raise AssertionError(f'provider requests {count}, expected {expected}')


if __name__ == '__main__':
    main()
