#!/usr/bin/env python3
"""build-binary.py - the client as one file, for people without Python:
`wasabi` (or wasabi.exe) with the desktop app's pages, the phone server
and the MCP server inside. Run on the system you build for (PyInstaller
does not cross-build); GitHub's Linux, macOS and Windows machines do it
for every release (.github/workflows/binaries.yml).

    python3 -m pip install pyinstaller
    python3 tools/build-binary.py            # -> dist/wasabi[.exe]
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "binary")
SEP = ";" if sys.platform == "win32" else ":"


def main():
    os.makedirs(BUILD, exist_ok=True)
    # PyInstaller wants a .py to analyse; the script is the same file.
    entry = os.path.join(BUILD, "wasabi_cli.py")
    shutil.copyfile(os.path.join(ROOT, "wasabi"), entry)
    app = os.path.join(ROOT, "app")
    hidden = [f[:-3] for f in os.listdir(app) if f.endswith(".py")]
    hidden.append("wasabi_mcp")
    cmd = [sys.executable, "-m", "PyInstaller", "--onefile", "--noconfirm",
           "--name", "wasabi", "--distpath", os.path.join(ROOT, "dist"),
           "--workpath", os.path.join(BUILD, "work"),
           "--specpath", BUILD,
           "--paths", app, "--paths", ROOT,
           "--add-data", os.path.join(ROOT, "view", "dist") + SEP +
           os.path.join("view", "dist"),
           # the app window is optional (it falls back to the browser),
           # and its engines are large and system-specific
           "--exclude-module", "webview"]
    for h in hidden:
        cmd += ["--hidden-import", h]
    cmd.append(entry)
    subprocess.run(cmd, check=True)
    exe = os.path.join(ROOT, "dist", "wasabi" +
                       (".exe" if sys.platform == "win32" else ""))
    print("built", exe, os.path.getsize(exe) // 1024, "KB")


if __name__ == "__main__":
    main()
