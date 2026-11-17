#!/bin/bash
# 一键恢复 DIVA AOT 工具链（环境重置后运行）
set -e
D=/root/userdata/workspace/divaft-persist
mkdir -p /root/divaft-work/bb-recomp/diva-aot/aot /root/divaft-work/bb-recomp/out/recomp-build
cp $D/bb-tools/bb-recomp /root/divaft-work/bb-recomp/out/recomp-build/bb-recomp
chmod +x /root/divaft-work/bb-recomp/out/recomp-build/bb-recomp
cp $D/diva-aot/libbbaot.so $D/diva-aot/libbbaot.json /root/divaft-work/bb-recomp/diva-aot/aot/
[ -f $D/diva-aot/boot-linked.bin ] && { mkdir -p /root/divaft-work/bb-recomp/diva-aot/.bbwork; cp $D/diva-aot/boot-linked.bin /root/divaft-work/bb-recomp/diva-aot/.bbwork/; }
if [ ! -d /root/android-ndk-r30 ]; then
  python3 -c "
with open('$D/ndk-r30-linux.zip','rb') as f,open('/root/ndk.zip','wb') as g:
    while True:
        b=f.read(1<<20)
        if not b: break
        g.write(b)"
  unzip -q /root/ndk.zip -d /root && rm /root/ndk.zip
fi
echo RESTORE_AOT_OK
