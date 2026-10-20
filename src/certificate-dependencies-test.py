#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


class CertificateDependenciesTest(unittest.TestCase):
    def test_tests_option(self):
        source = Path(sys.argv[1])
        text = (source / "src/meson.build").read_text()
        start = text.index("if get_option('tests')\n")
        end = text.index("\ninstall_data (", start)
        rules = text[start:end]
        with tempfile.TemporaryDirectory(prefix="certificate-test-options-") as directory:
            root = Path(directory)
            shutil.copyfile(source / "meson_options.txt", root / "meson_options.txt")
            for helper in (source / "src").glob("certificate-*.py"):
                shutil.copyfile(helper, root / helper.name)
            native_file = root / "no-python.ini"
            native_file.write_text(
                "[binaries]\npython3 = '/certificate-test-missing-python'\n")
            cross_file = root / "cross.ini"
            cross_file.write_text(
                "[host_machine]\nsystem = 'linux'\ncpu_family = 'aarch64'\n"
                "cpu = 'aarch64'\nendian = 'little'\n"
                "[properties]\nneeds_exe_wrapper = true\n")
            for tests, certificate, cross in (
                    (None, False, False), (False, False, False),
                    (None, True, False), (False, True, False),
                    (True, False, False), (True, True, True)):
                with self.subTest(tests=tests, certificate=certificate, cross=cross):
                    (root / "meson.build").write_text(
                        "project('certificate-test-options')\n" +
                        f"certificate_enabled = {str(certificate).lower()}\n" + rules)
                    build = root / f"build-{tests}-{certificate}-{cross}"
                    command = ["meson", "setup", str(build), str(root)]
                    if tests is not None:
                        command.append(f"-Dtests={str(tests).lower()}")
                    if not tests:
                        command.extend(["--native-file", str(native_file)])
                    if cross:
                        command.extend(["--cross-file", str(cross_file)])
                    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT)
                    self.assertEqual(result.returncode, 0, result.stdout)
                    registered = json.loads(subprocess.check_output(
                        ["meson", "introspect", "--tests", str(build)], text=True))
                    self.assertEqual([test["name"] for test in registered],
                                     ["certificate-dependencies"] if tests else [])

    def test_executable_wrappers(self):
        source = Path(sys.argv[1]) / "src"
        ui = runpy.run_path(str(source / "certificate-ui-test.py"))
        settings = runpy.run_path(str(source / "certificate-settings-test.py"))
        binary = "/target path/portal"
        for wrapper, prefix in (
                (None, []), ("", []),
                ("'/wrapper path/emulator' --root '/sysroot path'",
                 ["/wrapper path/emulator", "--root", "/sysroot path"])):
            with self.subTest(wrapper=wrapper):
                env = dict(os.environ)
                env.pop("MESON_EXE_WRAPPER", None)
                if wrapper is not None:
                    env["MESON_EXE_WRAPPER"] = wrapper
                with mock.patch.dict(os.environ, env, clear=True):
                    with mock.patch("subprocess.run", return_value=mock.Mock(returncode=77)) as launch:
                        self.assertEqual(ui["run_ui"](binary), 77)
                        launch.assert_called_once_with([*prefix, binary])
                    with tempfile.TemporaryDirectory(prefix="certificate-wrapper-") as directory:
                        with mock.patch("subprocess.Popen", side_effect=RuntimeError("launch")) as launch:
                            with self.assertRaisesRegex(RuntimeError, "launch"):
                                settings["check_settings_only"](binary, directory)
                            self.assertEqual(launch.call_args.args[0], [*prefix, binary])

    def test_feature_dependencies(self):
        meson = Path(sys.argv[1]) / "meson.build"
        text = meson.read_text()
        start = text.index("certificate_dep = dependency(")
        end = text.index("\nsubdir('data')", start)
        # Run the production dependency checks with isolated pkg-config inputs.
        checks = text[start:end]
        with tempfile.TemporaryDirectory(prefix="certificate-dependencies-") as directory:
            root = Path(directory)
            (root / "meson_options.txt").write_text(
                "option('certificate', type: 'feature', value: 'disabled')\n")
            (root / "meson.build").write_text(
                "project('certificate-dependencies')\n" + checks +
                "\nconfiguration = configuration_data()\n" +
                "configuration.set10('CERTIFICATE', certificate_enabled)\n" +
                "configure_file(output: 'feature.h', configuration: configuration)\n")
            packages = root / "pkgconfig"
            packages.mkdir()
            env = dict(os.environ, PKG_CONFIG_LIBDIR=str(packages), PKG_CONFIG_PATH="")
            for sdk in (False, True):
                for gcr in (False, True):
                    for name, available in (("xdp-certificate-core-0", sdk), ("gcr-4", gcr)):
                        package = packages / (name + ".pc")
                        if available:
                            package.write_text(f"Name: {name}\nDescription: Test dependency\nVersion: 1.0\n")
                        elif package.exists():
                            package.unlink()
                    for feature in ("auto", "enabled", "disabled"):
                        with self.subTest(sdk=sdk, gcr=gcr, feature=feature):
                            build = root / f"build-{sdk}-{gcr}-{feature}"
                            result = subprocess.run(
                                ["meson", "setup", str(build), str(root), "--wrap-mode=nofallback",
                                 f"-Dcertificate={feature}"], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                            success = feature != "enabled" or (sdk and gcr)
                            self.assertEqual(result.returncode == 0, success, result.stdout)
                            if success:
                                enabled = feature != "disabled" and sdk and gcr
                                self.assertIn(f"#define CERTIFICATE {int(enabled)}", (build / "feature.h").read_text())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
