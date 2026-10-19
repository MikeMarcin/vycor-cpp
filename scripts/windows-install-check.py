#!/usr/bin/env python3
# Copyright (c) 2026 The vycor-cpp Authors
# SPDX-License-Identifier: Apache-2.0
"""Validate a relocated Windows install and argument-based PCH compilation."""
import argparse
import json
import os
import subprocess
import shutil
import tempfile
from pathlib import Path


def run(argv, env):
    result = subprocess.run([str(a) for a in argv], env=env, capture_output=True,
                            encoding='utf-8', timeout=120)
    assert result.returncode == 0, (argv, result.returncode, result.stdout, result.stderr)
    return result


def complete(binary, index, env):
    info = json.loads(run([binary, 'megascope', 'info', '--files', '--index', index], env).stdout)
    assert info['coverage'] == dict(complete=True, requested=1, indexed=1, partial=0, failed=0), info
    assert all(f['status'] == 'indexed' for f in info['files']), info


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--prefix', type=Path, required=True)
    ap.add_argument('--sdk', type=Path, required=True)
    args = ap.parse_args()
    binary = args.prefix.resolve() / 'bin/vycor-cpp.exe'
    sdk = args.sdk.resolve()
    env = dict(os.environ)
    env['PATH'] = os.pathsep.join(p for p in env['PATH'].split(os.pathsep)
                                if str(sdk).lower() not in p.lower())
    run([binary, '--version'], env)
    with tempfile.TemporaryDirectory(prefix="vycor space ü ' & ") as temp:
        root = Path(temp)
        header = root / "pch header ü ' &.hpp"
        header.write_text('#include <stddef.h>\n#include <intrin.h>\ninline int value() {return sizeof(size_t);}\n', encoding='utf-8')
        include = root / 'include env ü'
        include.mkdir()
        (include/'only_env.hpp').write_text('#define ENV_VALUE 42\n', encoding='utf-8')
        env['VYCOR_EXTRA_ARGS'] = subprocess.list2cmdline(['-I', str(include)])
        source = root / 'source ü.cpp'
        source.write_text('#include "only_env.hpp"\nstatic_assert(ENV_VALUE == 42);\nint main() {return value();}\n', encoding='utf-8')
        commands = [dict(directory=str(root), file=str(source), arguments=[
            str(sdk/'bin/clang++.exe'), '-std=c++17', '-include', header.name, '-c', str(source)])]
        (root/'compile_commands.json').write_text(json.dumps(commands), encoding='utf-8')
        index = root/'relocated.vycs'
        result = run([binary, 'megascope', 'index', '--build-path', root, '--index', index,
                      '--threads', '2', '--extra-arg=-v'], env)
        normalized = result.stderr.replace('\\', '/').lower()
        # In-process verbose output is retained, unlike isolated-worker logs.
        if 'lib/clang/22/include' not in normalized:
            result = run([binary, 'megascope', 'index', '--build-path', root, '--index', index,
                          '--threads', '1', '--force', '--extra-arg=-v'], env)
            normalized = result.stderr.replace('\\', '/').lower()
        assert str(args.prefix.resolve()).replace('\\', '/').lower() in normalized, result.stderr
        complete(binary, index, env)
        env['VYCOR_INDEX'] = str(index)
        assert json.loads(run([binary, 'megascope', 'info'], env).stdout)['coverage']['complete']
        # Spaces, Unicode, apostrophes and shell metacharacters are argv data.
        compiler = root / "compiler ü ' &" / 'clang++.exe'
        compiler.parent.mkdir()
        shutil.copy2(sdk/'bin/clang++.exe', compiler)
        pch = run([binary, 'megascope', 'index', '--build-path', root, '--index', root/'pch.vycs',
                   '--threads', '1', '--extra-arg=-v', '--pch-dir', root/'cache ü', '--clang', compiler], env)
        assert '1 of 1 PCH files compiled successfully' in pch.stderr, pch.stderr
        assert list((root/'cache ü').glob('*.pch'))
        assert '-include-pch' in pch.stderr, pch.stderr
        complete(binary, root/'pch.vycs', env)
    print('relocated install: startup, resource headers, isolated index, reload and quoted PCH passed')


if __name__ == '__main__':
    main()
