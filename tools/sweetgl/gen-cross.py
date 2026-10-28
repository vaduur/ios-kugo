#!/usr/bin/env python3
"""Generate ios-cross.txt for Meson using current Xcode / SDK paths."""
import os
import subprocess
import sys
from pathlib import Path


def get_cmd_output(cmd):
    return subprocess.check_output(cmd, shell=True, text=True).strip()


def main():
    repo_root = Path(__file__).resolve().parent.parent.parent
    sdk = get_cmd_output("xcrun --sdk iphoneos --show-sdk-path")
    clang = get_cmd_output("xcrun -find clang")
    clangxx = get_cmd_output("xcrun -find clang++")
    ar = get_cmd_output("xcrun -find ar")
    strip = get_cmd_output("xcrun -find strip")
    inc_dir = repo_root / "tools" / "sweetgl" / "include"
    out_file = repo_root / "tools" / "sweetgl" / "ios-cross.txt"

    content = f"""[binaries]
c = ['{clang}', '-arch', 'arm64', '-isysroot', '{sdk}', '-miphoneos-version-min=16.0', '-mcpu=cortex-a53']
cpp = ['{clangxx}', '-arch', 'arm64', '-isysroot', '{sdk}', '-miphoneos-version-min=16.0', '-mcpu=cortex-a53']
objc = ['{clang}', '-arch', 'arm64', '-isysroot', '{sdk}', '-miphoneos-version-min=16.0', '-mcpu=cortex-a53']
ar = '{ar}'
strip = '{strip}'
pkg-config = 'false'

[built-in options]
c_args = ['-I{inc_dir}']
cpp_args = ['-I{inc_dir}']

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
"""
    out_file.write_text(content)
    print(f"Generated {out_file}:\n{content}")


if __name__ == "__main__":
    main()

