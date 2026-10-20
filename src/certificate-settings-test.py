#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile


def check_settings_only(binary, directory):
    env = dict(os.environ, DISPLAY="", WAYLAND_DISPLAY="", GDK_BACKEND="x11",
               XDG_CONFIG_HOME=directory, GSETTINGS_BACKEND="memory")
    config = Path(directory) / "xdg-desktop-portal-gnome"
    config.mkdir()
    (config / "certificate.conf").write_text("[Certificate]\nPinPrompt=invalid\n")
    with tempfile.TemporaryFile(mode="w+") as log:
        wrapper = shlex.split(env.get("MESON_EXE_WRAPPER", ""))
        process = subprocess.Popen([*wrapper, binary], env=env, stdout=log, stderr=log)
        try:
            name = "org.freedesktop.impl.portal.desktop.gnome"
            subprocess.run(["gdbus", "wait", "--session", "--timeout", "15", name],
                           env=env, check=True, timeout=20)
            result = subprocess.run(
                ["gdbus", "introspect", "--session", "--dest", name,
                 "--object-path", "/org/freedesktop/portal/desktop"],
                env=env, check=True, text=True, capture_output=True, timeout=10)
            assert "org.freedesktop.impl.portal.Settings" in result.stdout, result.stdout
            assert "org.freedesktop.impl.portal.experimental.Certificate1" not in result.stdout, result.stdout
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
            log.seek(0)
            output = log.read()
            print(output)
        assert process.returncode == 0, process.returncode
        assert "exposing settings only" in output, output
        assert "Certificate portal initialization failed" not in output, output


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="certificate-settings-") as directory:
        check_settings_only(sys.argv[1], directory)
