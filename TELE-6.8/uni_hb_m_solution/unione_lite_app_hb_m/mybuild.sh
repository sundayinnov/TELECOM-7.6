#!/bin/bash
set -e
cd "$(dirname "$0")"

# 确保 output 目录存在
mkdir -p output

# 编译 release
echo "=== Building release ==="
cp tools/scripts/config_release.bin tools/scripts/config.bin
cp tools/scripts/aik_release.json tools/scripts/aik.json
cd tools/scripts && python2 res_build_tool.py manual && cd -
cd build && find . -type f \( -name "*.o" -o -name "*.d" -o -name "*.adx" \) -delete && make && cd -
mv output/uni_app.bin output/uni_app_release.bin

# 编译 debug
echo "=== Building debug ==="
cp tools/scripts/config_debug.bin tools/scripts/config.bin
cp tools/scripts/aik_debug.json tools/scripts/aik.json
cd tools/scripts && python2 res_build_tool.py manual && cd -
cd build && find . -type f \( -name "*.o" -o -name "*.d" -o -name "*.adx" \) -delete && make CFLAGS="-g -DDEBUG -O0" && cd -
mv output/uni_app.bin output/uni_app_debug.bin

echo "=== Build done ==="
ls -lh output/uni_app_*.bin
