# -*- coding: utf-8 -*-
# OTA 拼包: fromelf(axf -> 临时bin) -> LEN:数字\r\n + bin -> bin\Industrial_temperature_humidity_ota.bin
# 不产出裸bin: 设备端要求带LEN行 (After-Build 只跑这一个脚本)
import os, subprocess, sys, tempfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
axf = os.path.join(root, 'Output', 'Industrial_temperature_humidity.axf')

candidates = [
    r'D:\keil_v5\ARM\ARM_Compiler_5.06u7\bin\fromelf.exe',
    r'D:\keil_v5\ARM\ARM_Compiler_5.06u7\bin64\fromelf.exe',
    r'D:\keil_v5\ARM\ARMCLANG\bin\fromelf.exe',
    r'D:\keil\ARM\ARMCC\Bin\fromelf.exe',
    r'C:\Keil_v5\ARM\ARMCC\Bin\fromelf.exe',
    r'C:\Keil\ARM\ARMCC\Bin\fromelf.exe',
]
fromelf = next((p for p in candidates if os.path.exists(p)), None)
if fromelf is None:
    print('找不到 fromelf.exe, 请修改 Tools/make_ota_pkg.py 顶部候选路径')
    sys.exit(1)

tmp = os.path.join(tempfile.gettempdir(), 'ota_tmp_app.bin')
subprocess.run([fromelf, '--bin', '-o', tmp, axf], check=True)
d = open(tmp, 'rb').read()
os.unlink(tmp)
bin_dir = os.path.join(root, 'bin')
os.makedirs(bin_dir, exist_ok=True)
open(os.path.join(bin_dir, 'Industrial_temperature_humidity_ota.bin'), 'wb').write(
    b'LEN:%d\r\n' % len(d) + d)
print('ota包已生成: ..\\bin\\Industrial_temperature_humidity_ota.bin (%d 字节)' % len(d))
