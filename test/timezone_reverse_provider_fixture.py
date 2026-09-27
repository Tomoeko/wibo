#!/usr/bin/env python3
import os
import struct
import sys


def number(value):
    return struct.pack('<I', value)


def header(status=0):
    return number(0x50535957) + number(1) + number(status)


def blob(value):
    return number(len(value)) + value


def zone_record(bias=0, standard_bias=0, daylight_bias=0,
                standard=(0,) * 8, daylight=(0,) * 8, names=False):
    value = bytearray(172)
    struct.pack_into('<l', value, 0, bias)
    struct.pack_into('<8H', value, 68, *standard)
    struct.pack_into('<l', value, 84, standard_bias)
    struct.pack_into('<8H', value, 152, *daylight)
    struct.pack_into('<l', value, 168, daylight_bias)
    if names:
        value[4:68] = value[88:152] = struct.pack('<32H', *([0xd801] * 32))
    return bytes(value)


STANDARD = (0, 11, 0, 1, 2, 0, 0, 0)
DAYLIGHT = (0, 3, 0, 2, 2, 0, 0, 0)
NORTH = zone_record(300, 0, -60, STANDARD, DAYLIGHT)
ZONES = (
    zone_record(), zone_record(90), zone_record(-330), zone_record(-330, 27, -91),
    zone_record(120), zone_record(-120), zone_record(-60), NORTH,
    zone_record(300, 30, -60, STANDARD, DAYLIGHT),
    zone_record(300, 0, -30, STANDARD, DAYLIGHT),
    zone_record(300, 0, -60, (2024, 11, 0, 3, 2, 0, 0, 0), (2024, 3, 0, 10, 2, 0, 0, 0)),
    zone_record(-600, 0, -60, (0, 4, 0, 1, 2, 0, 0, 0), (0, 10, 0, 1, 2, 0, 0, 0)),
    zone_record(300, 0, -60, STANDARD, (0, 3, 0, 5, 2, 0, 0, 0)),
    zone_record(300, 0, -60, (0, 11, 0, 0, 2, 0, 0, 0), DAYLIGHT),
    zone_record(300, 0, -60, (0, 0, 0, 1, 2, 0, 0, 0), DAYLIGHT),
    zone_record(300, 0, -60, STANDARD, DAYLIGHT, names=True),
)

# Captured results for independent, synthetic native conversion cases.
ROWS = (
    (0, (2024, 2, 0, 29, 12, 34, 56, 789), (2024, 2, 4, 29, 12, 34, 56, 789)),
    (1, (2024, 2, 0, 29, 12, 34, 56, 789), (2024, 2, 4, 29, 14, 4, 56, 789)),
    (2, (2024, 2, 0, 29, 12, 34, 56, 789), (2024, 2, 4, 29, 7, 4, 56, 789)),
    (3, (2024, 2, 0, 29, 12, 34, 56, 789), (2024, 2, 4, 29, 7, 4, 56, 789)),
    (4, (2024, 12, 0, 31, 23, 34, 56, 789), (2025, 1, 3, 1, 1, 34, 56, 789)),
    (5, (2024, 1, 0, 1, 0, 15, 0, 0), (2023, 12, 0, 31, 22, 15, 0, 0)),
    (6, (1601, 1, 0, 1, 0, 0, 0, 0), None),
    (7, (2024, 1, 0, 15, 12, 34, 56, 789), (2024, 1, 1, 15, 17, 34, 56, 789)),
    (7, (2024, 7, 0, 15, 12, 34, 56, 789), (2024, 7, 1, 15, 16, 34, 56, 789)),
    (7, (2024, 3, 0, 10, 1, 59, 59, 999), (2024, 3, 0, 10, 6, 59, 59, 999)),
    (7, (2024, 3, 0, 10, 2, 0, 0, 0), (2024, 3, 0, 10, 6, 0, 0, 0)),
    (7, (2024, 3, 0, 10, 2, 30, 0, 0), (2024, 3, 0, 10, 6, 30, 0, 0)),
    (7, (2024, 3, 0, 10, 3, 0, 0, 0), (2024, 3, 0, 10, 7, 0, 0, 0)),
    (7, (2024, 11, 0, 3, 0, 59, 59, 999), (2024, 11, 0, 3, 4, 59, 59, 999)),
    (7, (2024, 11, 0, 3, 1, 30, 0, 0), (2024, 11, 0, 3, 5, 30, 0, 0)),
    (7, (2024, 11, 0, 3, 2, 0, 0, 0), (2024, 11, 0, 3, 7, 0, 0, 0)),
    (8, (2024, 11, 0, 3, 2, 0, 0, 0), (2024, 11, 0, 3, 7, 30, 0, 0)),
    (9, (2024, 3, 0, 10, 2, 15, 0, 0), (2024, 3, 0, 10, 6, 45, 0, 0)),
    (10, (2024, 7, 0, 15, 12, 0, 0, 0), (2024, 7, 1, 15, 16, 0, 0, 0)),
    (11, (2024, 1, 0, 15, 12, 0, 0, 0), (2024, 1, 1, 15, 1, 0, 0, 0)),
    (11, (2024, 7, 0, 15, 12, 0, 0, 0), (2024, 7, 1, 15, 2, 0, 0, 0)),
    (12, (2024, 3, 0, 31, 2, 30, 0, 0), (2024, 3, 0, 31, 6, 30, 0, 0)),
    (13, (2024, 3, 0, 31, 2, 30, 0, 0), None),
    (14, (2024, 3, 0, 31, 2, 30, 0, 0), None),
    (7, (2024, 2, 0, 30, 12, 0, 0, 0), None),
    (7, (2024, 2, 0, 29, 24, 0, 0, 0), None),
    (7, (2024, 2, 0, 29, 12, 0, 0, 1000), None),
    (15, (2024, 2, 65535, 29, 12, 34, 56, 789), (2024, 2, 4, 29, 17, 34, 56, 789)),
    (7, (2024, 2, 65535, 29, 12, 34, 56, 789), (2024, 2, 4, 29, 17, 34, 56, 789)),
)
RESULTS = {(ZONES[zone], local): output for zone, local, output in ROWS}

operation, *arguments = sys.argv[1:]
if operation == 'time-zone-information':
    assert not arguments
    response = header() + number(1) + blob(NORTH)
else:
    assert operation == 'tz-local-to-system'
    zone_hex, local_hex, initial_hex, incoming_error = arguments
    zone = NORTH if zone_hex == '-' else bytes.fromhex(zone_hex)
    local = struct.unpack('<8H', bytes.fromhex(local_hex))
    initial = bytes.fromhex(initial_hex)
    assert len(zone) == 172 and len(initial) == 16
    expected = RESULTS[(zone, local)]
    output = initial if expected is None else struct.pack('<8H', *expected)
    result = int(expected is not None)
    native_error = 87 if expected is None else int(incoming_error)
    response = header() + number(result) + number(native_error) + blob(output)
    mode = os.environ.get('WIBO_FIXTURE_TIMEZONE_REVERSE_RESPONSE')
    if mode == 'failed':
        response = header(5)
    elif mode == 'invalid-result':
        response = header() + number(2) + number(native_error) + blob(output)
    elif mode == 'wrong-size':
        response = header() + number(result) + number(native_error) + blob(output[:-1])
    elif mode == 'trailing':
        response += b'\0'
    elif mode == 'invalid-time':
        response = header() + number(1) + number(native_error) + blob(bytes(16))

sys.stdout.buffer.write(response)
