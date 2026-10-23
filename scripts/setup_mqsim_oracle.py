#!/usr/bin/env python3
"""Fetch pinned official MQSim, apply the opt-in DSA patch, and build cleanly."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

REVISION = '51f0f2d3fed92d88ef4a0fa61a38024b07bf9d16'
ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'bmpclient/bench/mqsim/oracle.patch'


def setup(source, jobs):
    source = source.resolve()
    if jobs < 1:
        raise ValueError('jobs must be positive')
    if not source.exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'clone', 'https://github.com/CMU-SAFARI/MQSim.git', str(source)], check=True)
        subprocess.run(['git', 'checkout', '--detach', REVISION], cwd=source, check=True)
    rev = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
    if rev != REVISION:
        raise ValueError(f'Expected MQSim {REVISION}; got {rev}; use a separate pinned checkout')
    # Accept only pristine upstream or exactly our patch. Never reset user edits.
    diff = subprocess.check_output(['git', 'diff', '--binary'], cwd=source)
    if diff:
        if subprocess.run(['git', 'apply', '--reverse', '--check', str(PATCH)], cwd=source,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            raise ValueError('MQSim has unrelated edits; use a clean checkout')
        # Ensure no edits beyond the patch, using a temporary index rather than mutating source.
        import tempfile, os
        with tempfile.TemporaryDirectory() as temp:
            env = dict(os.environ, GIT_INDEX_FILE=str(Path(temp)/'index'))
            subprocess.run(['git', 'read-tree', 'HEAD'], cwd=source, env=env, check=True)
            subprocess.run(['git', 'apply', '--cached', str(PATCH)], cwd=source, env=env, check=True)
            if subprocess.run(['git', 'diff', '--exit-code'], cwd=source, env=env,
                              stdout=subprocess.DEVNULL).returncode:
                raise ValueError('MQSim has edits beyond oracle.patch')
    else:
        subprocess.run(['git', 'apply', '--check', str(PATCH)], cwd=source, check=True)
        subprocess.run(['git', 'apply', str(PATCH)], cwd=source, check=True)
    # Upstream Makefile does not track header dependencies: incremental build is unsafe.
    subprocess.run(['make', 'clean'], cwd=source, check=True)
    subprocess.run(['make', f'-j{jobs}'], cwd=source, check=True)
    marker = dict(upstream_revision=rev, patch_sha256=hashlib.sha256(PATCH.read_bytes()).hexdigest(),
                  binary_sha256=hashlib.sha256((source/'MQSim').read_bytes()).hexdigest())
    (source/'dsa-oracle-build.json').write_text(json.dumps(marker, indent=2)+'\n')
    print(f'Ready: {source / "MQSim"}', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT/'.deps/MQSim')
    p.add_argument('--jobs', type=int, default=4)
    a=p.parse_args(); setup(a.source, a.jobs)
