#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
import os
import shlex
import subprocess
import sys


def run_ui(binary):
    wrapper = shlex.split(os.environ.get("MESON_EXE_WRAPPER", ""))
    return subprocess.run([*wrapper, binary]).returncode


if __name__ == "__main__":
    sys.exit(run_ui(sys.argv[1]))
