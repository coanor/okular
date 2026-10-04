#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Android callback/lifecycle regression against the installed mobile APK.

Run on a rooted Android emulator with Chrome, network access to OIDC discovery,
English UI, cached-app freezing enabled, and boot completed at least 10 minutes
ago (Android exempts recently booted devices). No ChatGPT login is needed.

    adb root
    python3 autotests/mobilechatgptcallbacktest.py

ADB_SERIAL selects the emulator. The test starts a fresh login, lets Android
cache the app, probes the real listener, then declines authorization to check
cleanup. It never exchanges a code, prints an auth URL, or saves credentials.
"""

import re
import socket
import shlex
import subprocess
import time
import urllib.parse
import xml.etree.ElementTree as ET

PACKAGE = "org.kde.okular.kirigami"
ACTIVITY = PACKAGE + "/org.kde.something.OpenFileActivity"
SERVICE = PACKAGE + "/org.kde.something.ChatGptSignInService"
UI_DUMP = "/sdcard/okular-callback-test.xml"


def adb(*args):
    result = subprocess.run(["adb", *args], capture_output=True, text=True, timeout=30)
    if result.returncode:
        # Android can include intent data in command output. Do not echo it.
        raise RuntimeError("ADB command failed: " + args[0])
    return result.stdout


def ui():
    adb("shell", "uiautomator", "dump", UI_DUMP)
    try:
        return ET.fromstring(adb("shell", "cat", UI_DUMP))
    finally:
        adb("shell", "rm", "-f", UI_DUMP)


def tap(text):
    for _ in range(12):
        for node in ui().iter("node"):
            if node.get("text") == text or node.get("content-desc") == text:
                bounds = list(map(int, re.findall(r"\d+", node.get("bounds"))))
                adb("shell", "input", "tap", str((bounds[0] + bounds[2]) // 2),
                    str((bounds[1] + bounds[3]) // 2))
                return
        time.sleep(0.5)
    raise RuntimeError("UI control unavailable: " + text)


def callback(port, query):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
            connection.sendall(("GET /auth/callback?" + query
                                + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n").encode())
            return connection.recv(200).split(b"\r\n")[0].decode()
    except OSError as error:
        return type(error).__name__


def sign_in_service():
    services = adb("shell", "dumpsys", "activity", "services", PACKAGE)
    # dumpsys includes the last service ANR even when it belongs to another
    # package. Only active records, marked with '*', describe running services.
    for record in re.split(r"(?m)^\s*\* ServiceRecord", services)[1:]:
        if SERVICE in record.splitlines()[0]:
            return record.split("Last ANR service:", 1)[0]
    return ""


def main():
    assert adb("shell", "getprop", "ro.kernel.qemu").strip() == "1", "Use a test emulator"
    assert adb("shell", "id", "-u").strip() == "0", "Run adb root first"
    assert float(adb("shell", "cat", "/proc/uptime").split()[0]) >= 600, "Wait 10 minutes after boot"
    settings = adb("shell", "dumpsys", "activity", "settings")
    assert "use_freezer=true" in settings, "Enable Android cached-app freezing"
    uid = re.search(r"(?:userId|appId)=(\d+)", adb("shell", "dumpsys", "package", PACKAGE)).group(1)
    forwarded = None
    try:
        adb("shell", "am", "force-stop", PACKAGE)
        adb("shell", "am", "force-stop", "com.android.chrome")
        adb("shell", "am", "start", "-W", "-n", ACTIVITY)
        tap("AI Reading Assistant")
        tap("Models…")
        tap("Continue with ChatGPT…")
        tap("Continue with ChatGPT")
        for _ in range(30):
            activities = adb("shell", "dumpsys", "activity", "activities")
            resumed = re.search(r"topResumedActivity=.*", activities)
            if resumed and "com.android.chrome/" in resumed.group(0):
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("Sign-in browser did not open")
        ports = []
        for line in adb("shell", "cat", "/proc/net/tcp").splitlines()[1:]:
            fields = line.split()
            if fields[3] == "0A" and fields[7] == uid:
                ports.append(int(fields[1].split(":")[1], 16))
        assert len(ports) == 1, "Expected one loopback callback listener"
        # Match the authorization intent to this listener rather than an old tab.
        callback_url = "http://127.0.0.1:" + str(ports[0]) + "/auth/callback"
        authorization = next(line for line in activities.splitlines()
                             if callback_url in urllib.parse.unquote(line) and "state=" in line)
        state = re.search(r"[?&]state=([^&\s]+)", urllib.parse.unquote(authorization)).group(1)
        forwarded = int(adb("forward", "tcp:0", "tcp:" + str(ports[0])).strip())
        # Also exercise returning to the browser through the launcher. OAuth
        # can involve other activities (account pickers or password managers).
        adb("shell", "input", "keyevent", "KEYCODE_HOME")
        adb("shell", "am", "start", "-W", "-a", "android.settings.SETTINGS")
        adb("shell", "am", "start", "-W", "-n",
            "com.android.chrome/org.chromium.chrome.browser.ChromeTabbedActivity")
        pid = adb("shell", "pidof", PACKAGE).strip()
        freeze_file = "/sys/fs/cgroup/uid_" + uid + "/pid_" + pid + "/cgroup.freeze"
        services = sign_in_service()
        if "isForeground=true" in services:
            # A foreground service exempts the process from cached-app freezing.
            time.sleep(15)
            assert adb("shell", "cat", freeze_file).strip() == "0", "Sign-in process was frozen"
        else:
            # Wait for the original bug condition, rather than testing during
            # Android's temporary exemption immediately after an activity stops.
            deadline = time.monotonic() + 180
            while adb("shell", "cat", freeze_file).strip() != "1":
                assert time.monotonic() < deadline, "Emulator did not freeze the background app"
                time.sleep(1)
        background = callback(forwarded, "state=invalid-test")
        print("Browser foreground; callback:", background, flush=True)
        if background != "HTTP/1.1 400 Bad Request":
            adb("shell", "am", "start", "-W", "-n", ACTIVITY)
            print("Okular foreground; callback:", callback(forwarded, "state=invalid-test"), flush=True)
        assert background == "HTTP/1.1 400 Bad Request", "Callback timed out with browser foreground"
        # Exercise the browser's own HTTP path and real callback cleanup.
        declined = "http://127.0.0.1:" + str(ports[0]) + "/auth/callback?" + urllib.parse.urlencode(
            {"state": state, "error": "access_denied"})
        adb("shell", "am", "start", "-W", "-a", "android.intent.action.VIEW",
            "-d", shlex.quote(declined), "-p", "com.android.chrome")
        for _ in range(12):
            if any("Sign-in received." in node.get("text", "") for node in ui().iter("node")):
                break
        else:
            raise AssertionError("Chrome did not receive the callback response")
        for _ in range(20):
            services = sign_in_service()
            if not services:
                break
            time.sleep(0.1)
        assert not services, "Sign-in service was not stopped after callback"
        assert callback(forwarded, "state=invalid-test") != "HTTP/1.1 400 Bad Request", "Listener remained open"
        print("PASS: background callback, Chrome response, listener and service cleanup")
    finally:
        if forwarded is not None:
            adb("forward", "--remove", "tcp:" + str(forwarded))
        adb("shell", "am", "force-stop", PACKAGE)


if __name__ == "__main__":
    main()
