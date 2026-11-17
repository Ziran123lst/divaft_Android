#!/bin/bash
# Debian trixie arm64 sysroot（glibc 运行时 + qemu 验证用），含 lavapipe。
set -euo pipefail
BA=${DIVA_BB_ANDROID:-/root/divaft-work/bb-android}
cd "$BA"
sed -i "s|MIRROR = 'https://deb.debian.org/debian'|MIRROR = '${DIVA_DEBIAN_MIRROR:-https://mirrors.aliyun.com/debian}'|" tools/android/fetch_arm64_sysroot.py
PKGS=$(python3 -c "
import re
s=open('tools/android/fetch_arm64_sysroot.py').read()
m=re.search(r'WANTED = \[(.*?)\]',s,re.S)
print(' '.join(re.findall(r\"'([^']+)'\",m.group(1))))")
python3 tools/android/fetch_arm64_sysroot.py --packages $PKGS mesa-vulkan-drivers
echo SYSROOT_OK
