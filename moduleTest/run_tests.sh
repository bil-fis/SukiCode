#!/bin/bash
# SukiCode 集成测试脚本
# 所有构建产物输出到 moduleTest/build/

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$SCRIPT_DIR/build"
SUKIC="$PROJECT_DIR/build/bin/sukic"

mkdir -p "$BUILD_DIR"

PASS=0
FAIL=0

run_test() {
    local name="$1"
    local src="$2"
    local expected_output="$3"

    local exe="$BUILD_DIR/$name.exe"

    echo -n "  $name ... "

    # 编译
    if ! "$SUKIC" -o "$exe" "$src" 2>/dev/null; then
        echo "FAIL (compile)"
        FAIL=$((FAIL + 1))
        return
    fi

    # 运行并检查输出
    local output
    output=$("$exe" 2>&1) || true

    if [ -n "$expected_output" ] && [ "$output" != "$expected_output" ]; then
        echo "FAIL (expected '$expected_output', got '$output')"
        FAIL=$((FAIL + 1))
        return
    fi

    echo "PASS"
    PASS=$((PASS + 1))
}

echo "=== SukiCode Integration Tests ==="
echo ""

# 基础测试
echo "--- Basic ---"
run_test "hello_world" "$SCRIPT_DIR/integration/hello_world.suki" "Hello, SukiCode!"
run_test "fibonacci" "$SCRIPT_DIR/integration/fibonacci.suki" "55"
run_test "generic" "$SCRIPT_DIR/integration/generic.suki" "42"
run_test "control_flow" "$SCRIPT_DIR/integration/control_flow.suki" "$(printf '42\n55')"

# 编译测试（只检查编译是否成功，不检查输出）
echo ""
echo "--- Compile Only ---"
for f in "$SCRIPT_DIR"/lexer/*.suki "$SCRIPT_DIR"/parser/*.suki; do
    name=$(basename "$f" .suki)
    exe="$BUILD_DIR/$name.exe"
    echo -n "  $name ... "
    if "$SUKIC" -o "$exe" "$f" 2>/dev/null; then
        echo "PASS (compile)"
        PASS=$((PASS + 1))
    else
        echo "FAIL (compile)"
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
