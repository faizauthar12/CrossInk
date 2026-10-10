#!/usr/bin/env python3
"""Build one CrossInk X4 Pro debug environment and record its source identity."""
import argparse
import fcntl
import json
import math
import os
import sys
from pathlib import Path
import signal
import subprocess
import time

from core import Terminated, command, ignore_termination_signals, install_signal_handlers, sha, write_json


def identity(source):
    paths = ["src", "lib", "include", "platformio.ini", "partitions.csv", "sdkconfig"]
    untracked = command(["git", "ls-files", "--others", "--exclude-standard", "-z", "--", *paths], cwd=source).stdout
    sdk_untracked = command(["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=source / "freeink-sdk").stdout
    return dict(path=str(source),
        commit=command(["git", "rev-parse", "HEAD"], cwd=source).stdout.decode().strip(),
        firmware_diff=command(["git", "diff", "HEAD", "--", *paths], cwd=source).stdout.decode(),
        untracked={os.fsdecode(name): sha(source / os.fsdecode(name)) for name in untracked.split(b"\0") if name},
        sdk=command(["git", "rev-parse", "HEAD"], cwd=source / "freeink-sdk").stdout.decode().strip(),
        sdk_status=command(["git", "status", "--short"], cwd=source / "freeink-sdk").stdout.decode(),
        sdk_diff=command(["git", "diff", "HEAD"], cwd=source / "freeink-sdk").stdout.decode(),
        sdk_untracked={os.fsdecode(name): sha(source / "freeink-sdk" / os.fsdecode(name))
                       for name in sdk_untracked.split(b"\0") if name})


def main():
    install_signal_handlers()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--pio", default="/Users/home/.local/bin/codex-pio-s3")
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=1200)
    args = parser.parse_args()
    source = args.source.resolve()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive")
    before = identity(source)
    argv = [args.pio, "run", "-e", "x4-pro-debug", "-j1"]
    args.log.parent.mkdir(parents=True, exist_ok=True)
    # Cooperating callers serialize use of the S3-only PlatformIO core. This
    # does not replace coordinating other direct pio builds on this Mac.
    with open("/tmp/crossink-x4pro-s3-build.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        started = time.time()
        with args.log.open("wb") as log:
            process = subprocess.Popen(argv, cwd=source, stdout=log, stderr=log, start_new_session=True)
            try:
                code = process.wait(timeout=args.timeout)
            finally:
                ignore_termination_signals()
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
        if code:
            raise RuntimeError(f"Build failed ({code}); see {args.log}")
        if identity(source) != before:
            raise RuntimeError("Firmware source changed during build; no provenance record published")
        build = source / ".pio/build/x4-pro-debug"
        record = dict(source=before, command=argv, environment="x4-pro-debug", started=started,
            finished=time.time(), build_log=str(args.log.resolve()),
            artifacts={name: sha(build / name) for name in ("firmware.bin", "bootloader.bin", "partitions.bin")})
        write_json(build / "crossink-build.json", record)
        print(build)


if __name__ == "__main__":
    try:
        main()
    except Terminated as error:
        sys.exit(128 + error.signum)
