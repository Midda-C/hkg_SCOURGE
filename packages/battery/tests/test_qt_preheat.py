#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Qt Quick regression test for the Battery preheat toggle.

Uses host Qt6, not the car's Qt5/Wayland surface manager. No OEM code is run.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="battery-qt-", dir="/tmp/opencode") as directory:
    temporary = Path(directory)
    qml = ROOT / "artifacts/qml/AppHomeScreen/qml_eco3.0"
    (temporary / "HomeScreen").mkdir()
    (temporary / "component/common").mkdir(parents=True)
    shutil.copyfile(qml / "HomeScreen/BatteryPage.qml",
                    temporary / "HomeScreen/BatteryPage.qml")
    shutil.copyfile(qml / "component/BatterySwitch.qml",
                    temporary / "component/BatterySwitch.qml")
    (temporary / "component/common/QText.qml").write_text(
        "import QtQuick 2.0\nText { property bool textBold: false }\n")
    (temporary / "component/common/Q_ThemeImage.qml").write_text(
        "import QtQuick 2.0\nItem { property string themeSource: \"\" }\n")
    for name in ("setup.cpp", "tst_preheat.qml"):
        shutil.copyfile(ROOT / "tests/qt" / name, temporary / name)
    (temporary / "test.pro").write_text(
        "QT += qmltest quick qml testlib\n"
        "CONFIG += testcase c++17\n"
        "SOURCES += setup.cpp\n"
        "TARGET = preheat-test\n")
    for command in (["qmake6", "test.pro"], ["make", "-s", "-j2"]):
        result = subprocess.run(command, cwd=temporary,
                                capture_output=True, text=True)
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen",
               QSG_RHI_BACKEND="software",
               QML2_IMPORT_PATH=str(temporary / "imports"))
    subprocess.run([str(temporary / "preheat-test"), "-input", str(temporary)],
                   cwd=temporary, env=env, check=True, timeout=60)
