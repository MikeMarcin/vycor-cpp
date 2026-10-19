#!/usr/bin/env python3
# Copyright (c) 2026 The vycor-cpp Authors
# SPDX-License-Identifier: Apache-2.0
"""Exercise console cancellation and forced parent death through nested real jobs."""
import argparse
import re
import subprocess
import tempfile
import time
from pathlib import Path
from windows_process import alive, send_console


def check(helper, base, mode):
    scratch = base / mode
    scratch.mkdir()
    proc = subprocess.Popen([str(helper), '--controller', str(scratch)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            creationflags=subprocess.CREATE_NEW_CONSOLE)
    pids = []
    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            pids = []
            for log in ('worker.log', 'descendant.log'):
                path = scratch / log
                if path.exists():
                    pids += [int(x) for x in re.findall(r'WORKER-PID (\d+)', path.read_text())]
            if len(pids) == 2 and all(alive(pid) for pid in pids):
                break
            if proc.poll() is not None:
                raise AssertionError(proc.stderr.read().decode(errors='replace'))
            time.sleep(.05)
        assert len(pids) == 2, f'{mode}: nested workers did not start'
        if mode == 'parent-termination':
            proc.kill()
        else:
            send_console(proc.pid, 0 if mode == 'ctrl-c' else 1)
        _, err = proc.communicate(timeout=15)
        if mode != 'parent-termination':
            assert proc.returncode == 130, (proc.returncode, err)
            assert not scratch.exists(), f'{mode}: scratch survived cancellation'
        deadline = time.monotonic() + 10
        while any(alive(pid) for pid in pids) and time.monotonic() < deadline:
            time.sleep(.05)
        assert not any(alive(pid) for pid in pids), f'{mode}: descendant survived: {pids}'
        print(f'{mode}: two generations terminated; exit={proc.returncode}; scratch_exists={scratch.exists()}')
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--helper', type=Path, required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='vycor nested jobs ') as temp:
        for mode in ('ctrl-c', 'ctrl-break', 'parent-termination'):
            check(args.helper.resolve(), Path(temp), mode)
