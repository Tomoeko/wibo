#!/usr/bin/env python3
"""Verify helper lifetime after normal and interrupted guest requests."""
import os
import pathlib
import subprocess
import sys
import tempfile
import time

runner, image, helper, mode = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='wibo-provider-') as directory:
    marker = pathlib.Path(directory) / 'marker'
    env = dict(os.environ, WIBO_SYSTEM_PROVIDER=helper, WIBO_SYSTEM_PROVIDER_PERSISTENT='1',
               WIBO_FIXTURE_STREAM_MARKER=str(marker), WIBO_FIXTURE_STREAM_MODE=mode)
    env.pop('WIBO_SYSTEM_PROVIDER_MAX_TIMEOUT_MS', None)
    timeout = 25
    if mode == 'timeout':
        env['WIBO_SYSTEM_PROVIDER_MAX_TIMEOUT_MS'] = '1000'
        timeout = 5
    subprocess.run([runner, image], env=env, check=True, timeout=timeout)
    assert marker.exists()
    pid = int(marker.read_text())
    for attempt in range(100):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.02)
    else:
        raise AssertionError('helper survived guest exit')
