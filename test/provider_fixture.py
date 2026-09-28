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


# Fixed native reference values for synthetic classification transport checks.
ASCII_CLASSIFICATIONS = {
    1: (
        0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0268, 0x0228, 0x0228, 0x0228, 0x0228, 0x0220, 0x0220,
        0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220, 0x0220,
        0x0248, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210,
        0x0284, 0x0284, 0x0284, 0x0284, 0x0284, 0x0284, 0x0284, 0x0284, 0x0284, 0x0284, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210,
        0x0210, 0x0381, 0x0381, 0x0381, 0x0381, 0x0381, 0x0381, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301,
        0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0210, 0x0210, 0x0210, 0x0210, 0x0210,
        0x0210, 0x0382, 0x0382, 0x0382, 0x0382, 0x0382, 0x0382, 0x0302, 0x0302, 0x0302, 0x0302, 0x0302, 0x0302, 0x0302, 0x0302, 0x0302,
        0x0302, 0x0302, 0x0302, 0x0302, 0x0302, 0x0302, 0x0302, 0x0302, 0x0302, 0x0302, 0x0302, 0x0210, 0x0210, 0x0210, 0x0210, 0x0220,
    ),
    2: (
        0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0009, 0x0008, 0x0009, 0x000a, 0x0008, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0008, 0x0008, 0x0008, 0x0009,
        0x000a, 0x000b, 0x000b, 0x0005, 0x0005, 0x0005, 0x000b, 0x000b, 0x000b, 0x000b, 0x000b, 0x0004, 0x0007, 0x0004, 0x0007, 0x0007,
        0x0003, 0x0003, 0x0003, 0x0003, 0x0003, 0x0003, 0x0003, 0x0003, 0x0003, 0x0003, 0x0007, 0x000b, 0x000b, 0x000b, 0x000b, 0x000b,
        0x000b, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
        0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x000b, 0x000b, 0x000b, 0x000b, 0x000b,
        0x000b, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
        0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x000b, 0x000b, 0x000b, 0x000b, 0x0000,
    ),
    4: (
        0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0048, 0x0048, 0x0448, 0x0048, 0x0448, 0x0048, 0x0048, 0x0440, 0x0048, 0x0048, 0x0048, 0x0048, 0x0048, 0x0440, 0x0048, 0x0448,
        0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0048, 0x0048, 0x0048, 0x0448, 0x0048, 0x0048,
        0x0448, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040,
        0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x0048, 0x0448, 0x0048, 0x044a, 0x0448,
        0x044a, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040,
        0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x0048, 0x0048, 0x0048, 0x0448, 0x0000,
    ),
}

EXTRA_CLASSIFICATIONS = {
    0x20ac: (0x0200, 0x0005, 0x0008),
    0x00e9: (0x0302, 0x0001, 0x8003),
    0x00df: (0x0302, 0x0001, 0x8000),
    0x00ff: (0x0302, 0x0001, 0x8003),
    0x3042: (0x0300, 0x0001, 0x8020),
}

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

elif operation == 'is-char-alpha-w-table':
    assert not arguments
    table = bytearray(8192)
    characters = list(range(ord('A'), ord('Z') + 1)) + list(range(ord('a'), ord('z') + 1))
    characters += [0x00aa, 0x00ba, 0x00c0, 0x00df, 0x00e9, 0x0100, 0x0130, 0x0178,
                   0x03a9, 0x03c0, 0x0416, 0x05d0, 0x0627, 0x0905, 0x0e01, 0x3042,
                   0x30a2, 0x4e2d, 0xac00, 0xff21, 0xff41]
    for character in characters:
        table[character // 8] |= 1 << (character % 8)
    response = header() + blob(table)
    fault = os.environ.get('WIBO_FIXTURE_ALPHA_RESPONSE')
    if fault == 'failed':
        response = header(5)
    elif fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'wrong-size':
        response = header() + blob(table[:-1])

elif operation == 'char-upper-buff-w':
    count, encoded, incoming_error = arguments
    count, incoming_error = int(count), int(incoming_error)
    data = bytes.fromhex(encoded)
    assert 0 < count <= 16000 and len(data) == count * 2
    units = list(struct.unpack('<' + 'H' * count, data))
    mappings = {0x00e9: 0x00c9, 0x00ff: 0x0178, 0x03c3: 0x03a3, 0x03ac: 0x0386,
                0x0436: 0x0416, 0x0561: 0x0531, 0xff41: 0xff21}
    pair_mappings = {(0xd801, 0xdc28): (0xd801, 0xdc00),
                     (0xd83a, 0xdd22): (0xd83a, 0xdd00),
                     (0xd801, 0xdd97): (0xd801, 0xdd70)}
    index = 0
    while index < count:
        pair = tuple(units[index:index + 2])
        if pair in pair_mappings:
            units[index:index + 2] = pair_mappings[pair]
            index += 2
            continue
        code = units[index]
        units[index] = code - 0x20 if ord('a') <= code <= ord('z') else mappings.get(code, code)
        index += 1
    output = struct.pack('<' + 'H' * count, *units)
    response = header() + number(count) + number(incoming_error) + blob(output)
    fault = os.environ.get('WIBO_FIXTURE_UPPER_RESPONSE')
    if fault == 'failed':
        response = header(5)
    elif fault == 'wrong-count':
        response = header() + number(count + 1) + number(incoming_error) + blob(output)
    elif fault == 'wrong-size':
        response = header() + number(count) + number(incoming_error) + blob(output[:-2])

elif operation == 'numa-highest-node-number':
    response = header() + number(3)
    fault = os.environ.get('WIBO_FIXTURE_NUMA_RESPONSE')
    if fault == 'failed':
        response = header(5)
    elif fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'

elif operation == 'lc-map-string-ex':
    flags, locale, count, source, capacity, destination = arguments
    # A fixed response checks transport; the adapter owns locale mapping.
    valid = (flags == '512' and locale == '' and count == '2'
             and source == '61004200' and capacity in ('0', '2')
             and destination == ('0' if capacity == '0' else '1'))
    response = header() + number(2) + blob(b'' if capacity == '0' else b'A\0B\0') if valid else header(87)
    fault = os.environ.get('WIBO_FIXTURE_NLS_RESPONSE')
    if fault == 'failed':
        response = header(122)
    elif fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'wrong-length':
        response = header() + number(2) + blob(b'A\0')
    elif fault == 'oversized-count':
        response = header() + number(0xffffffff) + blob(b'')
    elif fault == 'query-data':
        response = header() + number(2) + blob(b'A\0B\0')
    elif fault == 'large-query':
        response = header() + number(8 * 1024 * 1024 + 1) + blob(b'')

elif operation == 'compare-string-ex':
    flags, locale, left_count, left, right_count, right = arguments
    valid = (flags == '1' and locale == '' and left_count == '2' and left == '61004200'
             and right_count == '2' and right == '41004200')
    response = header() + number(2) if valid else header(87)
    fault = os.environ.get('WIBO_FIXTURE_COMPARE_RESPONSE')
    if fault == 'failed':
        response = header(87)
    elif fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'invalid-result-zero':
        response = header() + number(0)
    elif fault == 'invalid-result-high':
        response = header() + number(4)

elif operation == 'path-match-spec-w':
    source, pattern, incoming_error = arguments
    pair = (bytes.fromhex(source), bytes.fromhex(pattern))
    # Fixed native results, including unmatched per-WCHAR surrogate casing.
    results = {
        ('\u00e9.txt', '\u00c9.TXT'): 1,
        ('\u03c9.txt', '\u03a9.TXT'): 1,
        ('\u03c2.txt', '\u03a3.TXT'): 0,
        ('\u0131.txt', 'I.TXT'): 0,
        ('\uff41.txt', '\uff21.TXT'): 1,
        ('\u65e5.txt', '\u65e5.TXT'): 1,
        ('\ud801\udc28.txt', '\ud801\udc28.TXT'): 1,
        ('\ud801\udc28.txt', '\ud801\udc00.TXT'): 0,
        ('\ud801\udc28', '?'): 0,
        ('\ud801\udc28', '??'): 1,
        ('\ud801.txt', '\ud801.TXT'): 1,
    }
    results = {(file.encode('utf-16-le', 'surrogatepass'), spec.encode('utf-16-le', 'surrogatepass')): result
               for (file, spec), result in results.items()}
    response = header() + number(results[pair]) + number(int(incoming_error))
    fault = os.environ.get('WIBO_FIXTURE_PATH_MATCH_RESPONSE')
    if fault == 'failed':
        response = header(5)
    elif fault == 'invalid-result':
        response = header() + number(2) + number(int(incoming_error))
    elif fault == 'truncated':
        response = response[:8]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'wrong-size':
        response = header() + number(results[pair])

elif operation in ('date-format-w', 'time-format-w'):
    locale, flags, time_hex, picture_hex, capacity, initial, incoming_error = arguments
    locale, flags, capacity, incoming_error = map(int, (locale, flags, capacity, incoming_error))
    value = None if time_hex == '-' else struct.unpack('<8H', bytes.fromhex(time_hex))
    picture = None if picture_hex == '-' else bytes.fromhex(picture_hex).decode('utf-16-le')
    reference = (2024, 2, 6, 29, 13, 5, 9, 456)
    date = operation == 'date-format-w'
    # Snapshot text and errors come from synthetic native reference cases.
    texts = {
        (0x007f, 0, reference, 'yyyy-MM-dd'): '2024-02-29',
        (0x007f, 0, reference, 'HH:mm:ss'): '13:05:09',
        (0x007f, 0, reference, ''): '',
        (0x007f, 0, reference, "'fixed''value'"): "fixed'value",
        (0x007f, 0, None, "'fixed'"): 'fixed',
    }
    if date:
        texts.update({
            (0x007f, 0x80000000, reference, None): '02/29/2024',
            (0x0409, 0, reference, 'dddd, MMMM d, yyyy'): 'Thursday, February 29, 2024',
            (0x0411, 0, reference, "yyyy'\u5e74'M'\u6708'd'\u65e5'"): '2024\u5e742\u670829\u65e5',
            (0x0409, 0x80000001, reference, None): '2/29/2024',
            (0x0409, 0x80000002, reference, None): 'Thursday, February 29, 2024',
            (0x0409, 0x80000008, reference, None): 'February 2024',
            (0x007f, 0, (2024, 2, 6, 29, 65535, 65535, 65535, 65535), 'yyyy-MM-dd'): '2024-02-29',
            (0x0409, 0, (2024, 2, 65535, 29, 13, 5, 9, 456), 'dddd'): 'Thursday',
        })
    else:
        texts.update({
            (0x007f, 0x80000000, reference, None): '13:05:09',
            (0x0409, 0, reference, 'hh:mm:ss tt'): '01:05:09 PM',
            (0x0409, 2, reference, 'HH:mm:ss'): '13:05',
            (0x0409, 1, reference, 'HH:mm:ss'): '13',
            (0x0409, 4, reference, 'hh:mm:ss tt'): '01:05:09',
            (0x0409, 8, reference, 'hh:mm:ss tt'): '13:05:09 PM',
            (0x007f, 0, (65535, 65535, 65535, 65535, 13, 5, 9, 456), 'HH:mm:ss'): '13:05:09',
        })
    key = (locale, flags, value, picture)
    errors = {
        (0xffffffff, 0, reference, 'yyyy-MM-dd'): 87,
        (0xffffffff, 0, reference, 'HH:mm:ss'): 87,
        (0x007f, 0x80000000, reference, 'yyyy-MM-dd'): 1004,
        (0x007f, 0x80000000, reference, 'HH:mm:ss'): 1004,
        (0x0409, 3, reference, None): 1004,
        (0x0409, 1, reference, 'yyyy-MM-dd'): 1004,
        (0x007f, 0, (2024, 2, 6, 30, 13, 5, 9, 456), 'yyyy-MM-dd'): 87,
        (0x007f, 0, (2024, 2, 6, 29, 24, 5, 9, 456), 'HH:mm:ss'): 87,
        (0x007f, 0, (2024, 2, 6, 29, 13, 5, 9, 1000), 'HH:mm:ss'): 87,
    }
    output = bytearray() if not capacity else bytearray.fromhex(initial)
    assert len(output) == capacity * 2
    native_error, result = incoming_error, 0
    if key in errors:
        native_error = errors[key]
    else:
        text_value = texts[key]
        result = len(text_value) + 1
        if capacity:
            written = (text_value[:capacity - 1] + '\0').encode('utf-16-le')
            output[:len(written)] = written
            if result > capacity:
                result, native_error = 0, 122
    response = header() + number(result) + number(native_error) + blob(output)
    fault = os.environ.get('WIBO_FIXTURE_NLS_FORMAT_RESPONSE')
    if fault == 'failed':
        response = header(5)
    elif fault == 'invalid-result':
        response = header() + number(0xffffffff) + number(native_error) + blob(output)
    elif fault == 'wrong-size':
        response = header() + number(result) + number(native_error) + blob(output[:-2])
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'unterminated':
        assert result > 0 and capacity
        struct.pack_into('<H', output, (result - 1) * 2, 0xa5a5)
        response = header() + number(result) + number(native_error) + blob(output)

elif operation == 'is-valid-locale-name':
    encoded, incoming_error = arguments
    locale = bytes.fromhex(encoded).decode('utf-16-le').lower()
    result = locale in ('', 'en', 'en-us', 'ja-jp', 'de-de_phoneb')
    response = header() + number(result) + number(int(incoming_error))
    fault = os.environ.get('WIBO_FIXTURE_NLS_SCALAR_RESPONSE')
    if fault == 'failed':
        response = header(5)
    elif fault == 'invalid-result':
        response = header() + number(2) + number(int(incoming_error))
    elif fault == 'wrong-size':
        response += b'\0'

elif operation == 'string-type-ex-a':
    locale, kind, count, source, initial, incoming_error = arguments
    locale, kind, count, incoming_error = map(int, (locale, kind, count, incoming_error))
    source, output = bytes.fromhex(source), bytearray.fromhex(initial)
    assert count >= -1 and len(output) == len(source) * 2
    result, native_error = 1, incoming_error
    if not count:
        native_error = 87
    elif kind not in ASCII_CLASSIFICATIONS:
        result, native_error = 0, 87
    else:
        units = []
        index = 0
        while index < len(source):
            if locale == 0x0411 and source[index:index + 2] == b'\x82\xa0':
                units.append(0x3042)
                index += 2
            else:
                units.append(0x20ac if locale == 0x0409 and source[index] == 0x80 else source[index])
                index += 1
        for index, unit in enumerate(units):
            value = (ASCII_CLASSIFICATIONS[kind][unit] if unit < 128
                     else EXTRA_CLASSIFICATIONS[unit][(1, 2, 4).index(kind)])
            struct.pack_into('<H', output, index * 2, value)
    response = header() + number(result) + number(native_error) + blob(output)
    fault = os.environ.get('WIBO_FIXTURE_NLS_SCALAR_RESPONSE')
    if fault == 'failed':
        response = header(5)
    elif fault == 'invalid-result':
        response = header() + number(2) + number(native_error) + blob(output)
    elif fault == 'wrong-size':
        response = header() + number(result) + number(native_error) + blob(output[:-2])

elif operation == 'cp-info-ex-w':
    code_page, flags = map(int, arguments)
    value = bytearray(544)
    struct.pack_into('<I', value, 0, 4)
    value[4] = ord('?')
    struct.pack_into('<HI', value, 18, 0xfffd, 65001)
    name = 'Fixture Code Page\0'.encode('utf-16-le')
    value[24:24 + len(name)] = name
    response = header() + number(1) + blob(value) if code_page == 65001 and flags == 0 else header(87) + number(0)
    fault = os.environ.get('WIBO_FIXTURE_CP_INFO_RESPONSE')
    if fault == 'failed':
        response = header(87) + number(0)
    elif fault == 'failed-zero':
        response = header() + number(0)
    elif fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'wrong-size':
        response = header() + number(1) + blob(value[:-1])
    elif fault == 'unterminated-name':
        value[24:] = b'A\0' * 260
        response = header() + number(1) + blob(value)
    elif fault == 'mismatched-code-page':
        struct.pack_into('<I', value, 20, 1252)
        response = header() + number(1) + blob(value)
    elif fault == 'zero-char-size':
        struct.pack_into('<I', value, 0, 0)
        response = header() + number(1) + blob(value)
    elif fault == 'invalid-result':
        response = header() + number(2) + blob(value)
    elif fault == 'success-error':
        response = header(87) + number(1) + blob(value)
    scalar_mode = os.environ.get('WIBO_FIXTURE_NLS_SCALAR_RESPONSE')
    if code_page == 1 and flags == 0 and scalar_mode:
        value = bytearray(544)
        struct.pack_into('<I', value, 0, 1)
        value[4] = ord('?')
        struct.pack_into('<HI', value, 18, ord('?'), 437)
        name = 'Fixture OEM Code Page\0'.encode('utf-16-le')
        value[24:24 + len(name)] = name
        response = header() + number(1) + blob(value)
        if scalar_mode == 'failed':
            response = header(5) + number(0)
        elif scalar_mode == 'invalid-result':
            response = header() + number(2) + blob(value)
        elif scalar_mode == 'wrong-size':
            response = header() + number(1) + blob(value[:-1])
    lead_mode = os.environ.get('WIBO_FIXTURE_LEAD_RESPONSE')
    if code_page == 60000 and flags == 0 and lead_mode:
        value = bytearray(544)
        struct.pack_into('<I', value, 0, 2)
        value[4] = ord('?')
        value[6:8] = bytes([0x81, 0x9f])
        struct.pack_into('<HI', value, 18, 0xfffd, 60000)
        name = 'FixtureCodePage\0'.encode('utf-16-le')
        value[24:24 + len(name)] = name
        if lead_mode == 'unterminated':
            value[6:18] = bytes([0x81, 0x9f]) * 6
        elif lead_mode == 'reversed':
            value[6:8] = bytes([0x9f, 0x81])
        response = header(87) + number(0) if lead_mode == 'failed' else header() + number(1) + blob(value)

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
elif operation == 'device-info-set-a':
    identity, enumerator, flags_text = arguments
    class_guid = bytes.fromhex(identity) if identity != '-' else None
    name = bytes.fromhex(enumerator) if enumerator != '-' else None
    flags = int(flags_text)
    entries = [(bytes(range(16)), 9), (bytes(range(16, 32)), 10)]
    if class_guid is not None and not flags & 4:
        entries = [entry for entry in entries if entry[0] == class_guid]
    if name == b'WIBO_SYNTHETIC_ABSENT':
        entries = []
    response = header() + number(len(entries)) + b''.join(blob(guid) + number(instance) for guid, instance in entries)
    fault = os.environ.get('WIBO_FIXTURE_DEVICES_RESPONSE')
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'bad-guid':
        response = header() + number(1) + blob(bytes(15)) + number(9)
    elif fault == 'bad-count':
        response = header() + number(0xffffffff)
    elif fault == 'failed':
        response = header(5)
elif operation == 'device-interface-set-a':
    identity, enumerator, flags_text = arguments
    class_guid = bytes.fromhex(identity) if identity != '-' else None
    name = bytes.fromhex(enumerator) if enumerator != '-' else None
    flags = int(flags_text)
    synthetic_guid = bytes(range(16))
    device_class_guid = bytes(range(16, 32))
    synthetic_path = r'\\?\SYNTHETIC#DEVICE#0'.encode('utf-16-le')
    devices = []
    interfaces = []
    if class_guid == synthetic_guid and flags & 0x10 and name != b'WIBO_SYNTHETIC_ABSENT':
        devices.append((device_class_guid, 9))
        interfaces.append((class_guid, 1, 9, device_class_guid, synthetic_path))
    device_rows = b''.join(blob(guid) + number(instance) for guid, instance in devices)
    interface_rows = b''.join(blob(guid) + number(interface_flags) + number(instance) + blob(owner) + blob(path)
                              for guid, interface_flags, instance, owner, path in interfaces)
    prefix = header() + number(len(devices)) + device_rows
    response = prefix + number(len(interfaces)) + interface_rows
    fault = os.environ.get('WIBO_FIXTURE_DEVICE_INTERFACES_RESPONSE')
    if fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
    elif fault == 'bad-guid':
        response = (prefix + number(1) + blob(bytes(15)) + number(1) + number(9)
                    + blob(device_class_guid) + blob(synthetic_path))
    elif fault == 'bad-owner-guid':
        response = (prefix + number(1) + blob(synthetic_guid) + number(1) + number(9)
                    + blob(bytes(15)) + blob(synthetic_path))
    elif fault == 'bad-path':
        response = (prefix + number(1) + blob(synthetic_guid) + number(1) + number(9)
                    + blob(device_class_guid) + blob(b'\0'))
    elif fault == 'bad-device-guid':
        response = header() + number(1) + blob(bytes(15)) + number(9) + number(1) + interface_rows
    elif fault == 'missing-device':
        response = header() + number(0) + number(1) + interface_rows
    elif fault == 'bad-count':
        response = header() + number(0xffffffff)
    elif fault == 'failed':
        response = header(5)
elif operation == 'user-default-ui-language':
    assert not arguments
    response = header() + number(0x0411)
    fault = os.environ.get('WIBO_FIXTURE_UI_LANGUAGE_RESPONSE')
    if fault == 'failed':
        response = header(5)
    elif fault == 'invalid-zero':
        response = header() + number(0)
    elif fault == 'invalid-high':
        response = header() + number(0x10000)
    elif fault == 'truncated':
        response = response[:-1]
    elif fault == 'trailing':
        response += b'\0'
elif operation == 'memory-resource-state':
    state_file = os.environ.get('WIBO_FIXTURE_MEMORY_RESOURCE_STATE_FILE')
    state = '0,0'
    if state_file:
        with open(state_file, encoding='ascii') as source:
            state = source.read().strip()
    if state == 'failed':
        response = header(5)
    else:
        values = {'0,0': (0, 0), '1,0': (1, 0), '0,1': (0, 1),
                  'bad-state': (2, 0)}
        response = header() + b''.join(number(value) for value in values.get(state, (0, 0)))
        if state == 'slow':
            time.sleep(1)
        if state == 'truncated':
            response = response[:-1]
        elif state == 'trailing':
            response += b'\0'
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
