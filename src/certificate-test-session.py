#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="certificate-session-") as directory:
    config = Path(directory) / "bus.conf"
    # No service directories or systemd activation on the test bus.
    config.write_text('''<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow own="*"/>
    <allow send_destination="*"/>
    <allow receive_sender="*"/>
  </policy>
</busconfig>
''')
    result = subprocess.run(["dbus-run-session", f"--config-file={config}", "--", *sys.argv[1:]])
    sys.exit(result.returncode)
