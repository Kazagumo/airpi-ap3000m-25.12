#!/usr/bin/env bash
set -euo pipefail

echo "============================================================"
echo " Apply Airpi AP3000M WiFi MAC fix"
echo " 修复：phy0/phy1 分配不同 WiFi MAC，避免 ra0/rax0 BSSID 相同"
echo "============================================================"

ROOT="${GITHUB_WORKSPACE:-$(pwd)}"

FIX_FILE="$(find "$ROOT" -path '*/target/linux/mediatek/filogic/base-files/etc/hotplug.d/ieee80211/11_fix_wifi_mac' -type f 2>/dev/null | head -n 1 || true)"

if [ -z "$FIX_FILE" ]; then
  echo "ERROR: 找不到 11_fix_wifi_mac"
  echo "ROOT=$ROOT"
  exit 1
fi

echo "Target file: $FIX_FILE"

if grep -q 'airpi,ap3000m)' "$FIX_FILE"; then
  echo "airpi,ap3000m 分支已存在，不重复插入。"
  grep -nA6 -B2 'airpi,ap3000m)' "$FIX_FILE" || true
  exit 0
fi

python3 - "$FIX_FILE" <<'PY'
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
lines = text.splitlines(True)

insert_block = [
'\tairpi,ap3000m)\n',
'\t\tbase_mac=$(macaddr_generate_from_mmc_cid mmcblk0)\n',
'\t\t[ "$PHYNBR" = "0" ] && macaddr_add $base_mac 2 > /sys${DEVPATH}/macaddress\n',
'\t\t[ "$PHYNBR" = "1" ] && macaddr_add $base_mac 3 > /sys${DEVPATH}/macaddress\n',
'\t\t;;\n',
]

# 优先插在 asus,rt-ax59u 前面；这个位置和公开 fork 中 Airpi 分支位置相同风格。
insert_at = None
for i, line in enumerate(lines):
    if re.match(r'^\s*asus,rt-ax59u\)', line):
        insert_at = i
        break

if insert_at is None:
    # 兜底：插在 bananapi,bpi-r3 前面，仍然在 case "$board" 内。
    for i, line in enumerate(lines):
        if re.match(r'^\s*bananapi,bpi-r3', line):
            insert_at = i
            break

if insert_at is None:
    raise SystemExit("ERROR: 未找到合适插入点，未修改文件。")

new_lines = lines[:insert_at] + insert_block + lines[insert_at:]
path.write_text(''.join(new_lines))
PY

echo
echo "=== 插入后的 Airpi 分支 ==="
grep -nA6 -B2 'airpi,ap3000m)' "$FIX_FILE"

echo
echo "Airpi WiFi MAC fix applied."
