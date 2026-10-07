#!/usr/bin/env bash
#
# 将 sukic 工具链打包为「独立、可重定位」的 Linux 压缩包（开箱即用）。
# 用法：package_linux.sh <artifact-name>
#
# 依赖（已在 CI 中用 apt 安装）：cmake / clang / llvm（提供 llvm-config）。
# 产物：dist/<artifact-name>.zip
#
set -euo pipefail

ART="${1:?usage: package_linux.sh <artifact-name>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PKG="$ROOT/dist/$ART"

rm -rf "$PKG"
mkdir -p "$PKG/bin" "$PKG/lib/suki/runtime" "$PKG/share/suki" "$PKG/lib/llvm"

# 1) 编译器二进制
cp "$ROOT/build/bin/sukic" "$PKG/bin/"

# 2) 预编译的 C 运行时对象（Linux 默认为 *.o）
RT="$(find "$ROOT/build" -name 'runtime.o' -print -quit || true)"
[ -n "$RT" ] && cp "$RT" "$PKG/lib/suki/runtime.o"

# 3) 运行时源码（交叉编译目标时由 clang 即时编译 runtime.c）
cp "$ROOT/src/runtime/runtime.c" "$ROOT/src/runtime/runtime.h" "$PKG/lib/suki/runtime/"

# 4) 标准库（保留目录结构）
cp -r "$ROOT/src/stdlib" "$PKG/share/suki/stdlib"

# 5) 捆绑 LLVM 共享库，提升跨机器可移植性（Linux 下 sukic 动态链接 libLLVM）。
#    优先从二进制自身的 ldd 依赖里提取 libLLVM*，避免依赖 llvm-config 是否挂在
#    PATH 上（各发行版命名不一：llvm-config / llvm-config-18 / …）。
BUNDLED=0
if command -v ldd >/dev/null 2>&1; then
  while read -r lib; do
    [ -f "$lib" ] || continue
    if cp -L "$lib" "$PKG/lib/llvm/" 2>/dev/null; then BUNDLED=1; fi
  done < <(ldd "$PKG/bin/sukic" 2>/dev/null | awk '/libLLVM/{print $3}' | sort -u)
fi

# 兜底：若 ldd 未命中（例如 LLVM 为静态链接），再尝试各类 llvm-config 的 libdir。
if [ "$BUNDLED" = "0" ]; then
  for cfg in llvm-config llvm-config-20 llvm-config-19 llvm-config-18 \
             llvm-config-17 llvm-config-16 llvm-config-15; do
    command -v "$cfg" >/dev/null 2>&1 || continue
    LLVM_LIBDIR="$("$cfg" --libdir 2>/dev/null || true)"
    if [ -n "$LLVM_LIBDIR" ] && [ -d "$LLVM_LIBDIR" ] &&
       cp -a "$LLVM_LIBDIR"/libLLVM*.so* "$PKG/lib/llvm/" 2>/dev/null; then
      BUNDLED=1
    fi
    break
  done
fi

if [ "$BUNDLED" = "0" ]; then
  rmdir "$PKG/lib/llvm" 2>/dev/null || true
  echo "注意：未检测到 libLLVM 动态依赖（可能已静态链接），跳过捆绑。"
fi

# 6) 启动包装脚本：把编译器指到本目录内的运行时/标准库（sukic 已支持环境变量重定位）
cat > "$PKG/bin/suki" <<'EOF'
#!/usr/bin/env bash
# SukiCode 启动包装：将编译器指向本目录内的运行时与标准库，开箱即用。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export SUKICODE_STDLIB_DIR="$SCRIPT_DIR/share/suki/stdlib"
export SUKICODE_RUNTIME_OBJECT="$SCRIPT_DIR/lib/suki/runtime.o"
export SUKICODE_RUNTIME_SOURCE="$SCRIPT_DIR/lib/suki/runtime/runtime.c"
export SUKICODE_RUNTIME_INCLUDE="$SCRIPT_DIR/lib/suki/runtime"
export SUKICODE_CLANG_DRIVER="${SUKICODE_CLANG_DRIVER:-clang}"
export LD_LIBRARY_PATH="$SCRIPT_DIR/lib/llvm${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$SCRIPT_DIR/bin/sukic" "$@"
EOF
chmod +x "$PKG/bin/suki"

# 7) 使用说明
cat > "$PKG/README.md" <<'EOF'
# SukiCode 独立工具链（Linux x86_64）

将本目录解压到任意位置，运行 `bin/suki`（或 `bin/sukic`）即可。

## 前置依赖
- **clang**：链接阶段使用，请确保 `clang` 在 PATH 中（如 `sudo apt install clang`）。
- LLVM 共享库已随包附带于 `lib/llvm`，由启动脚本自动加入 `LD_LIBRARY_PATH`，
  无需在系统中安装 LLVM。

## 快速开始
```
bin/suki run hello.suki                  # 编译并运行
bin/sukic build hello.suki -o hello      # 仅编译链接为可执行文件
```
EOF

# 8) 打包为 zip（使用 python 的 zipfile，避免依赖系统 zip 命令）。
#    必须显式写入 Unix 权限位：默认的 zipfile.write 不保留执行位，
#    会导致用户解压后 bin/suki 与 bin/sukic 无法直接执行。
cd "$ROOT/dist"
python3 - "$ART" <<'PY'
import os, stat, sys, time, zipfile
name = sys.argv[1]
with zipfile.ZipFile(name + '.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(name):
        for f in files:
            p = os.path.join(root, f)
            st = os.stat(p)
            zi = zipfile.ZipInfo(os.path.relpath(p, '.'),
                                 time.localtime(st.st_mtime)[:6])
            zi.external_attr = (stat.S_IMODE(st.st_mode) & 0xFFFF) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            with open(p, 'rb') as fp:
                z.writestr(zi, fp.read())
print('wrote', name + '.zip')
PY

echo "::group::打包产物"; ls -lh "$ART.zip"; echo "::endgroup::"
