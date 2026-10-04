#!/usr/bin/env bash
#
# 一键执行全部 SukiCode 测试 / One-shot runner for the whole SukiCode test suite.
#
# 覆盖三层：
#   1. 宿主单元测试  — lexer / parser / sema / codegen / runtime
#   2. .suki 正例    — 编译并运行，退出码即预期值（moduleTest/codegen、examples）
#   3. 负例诊断      — 必须被拒绝并报出诊断（moduleTest/diagnostics）
#
# 用法：
#   ./run_all_tests.sh             构建并运行全部测试
#   ./run_all_tests.sh --no-build  跳过构建，复用已有产物
#   ./run_all_tests.sh --quick     只跑单元测试（最快反馈）
#
# 待实现特性：moduleTest/pending/ 下的用例覆盖尚未落地的语言特性。它们必须
# 至少能被解析（因此不会出现在本脚本的"解析健全性"之外），但允许编译或运行
# 失败 —— 失败记为 SKIP，一旦某个用例意外通过，说明对应特性已实现，脚本报
# FAIL 提醒把它移入 moduleTest/codegen 或 moduleTest/diagnostics。

set -uo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
SUKIC="$BUILD_DIR/bin/sukic"

BUILD=1
QUICK=0
for arg in "$@"; do
    case "$arg" in
        --no-build) BUILD=0 ;;
        --quick)    QUICK=1 ;;
        -h|--help)  sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

# ── 呈现 ────────────────────────────────────────────────────────────────────
if [ -t 1 ]; then
    RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'
    BOLD=$'\033[1m';  DIM=$'\033[2m';    RESET=$'\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BOLD=''; DIM=''; RESET=''
fi

PASS=0
FAIL=0
SKIP=0
FAILED_NAMES=()

# Per-case wall-clock limit in seconds; override with CASE_TIMEOUT=... ./run_all_tests.sh
CASE_TIMEOUT="${CASE_TIMEOUT:-60}"

# Sources that exercise language features belonging to a not-yet-implemented
# plan task. They are expected to fail today; listing them keeps a green run
# meaningful and turns a surprise pass into a signal to move the code.
# Format: <path-under-project>|<owning plan task>[|<kind>]
#   kind = positive (default): the case must compile, run and exit 0.
#   kind = negative:          the case must be *rejected* by `sukic check`.
KNOWN_PENDING=(
    "examples/memory.suki|owned-pool: Owned / MemoryPool / Collection / pool protocol family"
    "examples/concurrency.suki|concurrency: Channel / Task / TaskGroup / withTaskGroup"

    "moduleTest/pending/generic_type.suki|generics: 泛型类型的实例化与约束求解"
)

# pending_reason <relative-path> -> prints the owning task, or nothing if known.
pending_reason() {
    local rel="$1" entry rest
    for entry in "${KNOWN_PENDING[@]}"; do
        [ "${entry%%|*}" = "$rel" ] || continue
        rest="${entry#*|}"
        printf '%s' "${rest%%|*}"
        return 0
    done
    return 1
}

# pending_kind <relative-path> -> "positive" or "negative".
pending_kind() {
    local rel="$1" entry rest
    for entry in "${KNOWN_PENDING[@]}"; do
        [ "${entry%%|*}" = "$rel" ] || continue
        rest="${entry#*|}"
        # A single field means the kind was omitted: default to positive.
        [ "$rest" = "${rest%%|*}" ] && { printf 'positive'; return 0; }
        printf '%s' "${rest#*|}"
        return 0
    done
    printf 'positive'
}

section() { printf '\n%s== %s ==%s\n' "$BOLD" "$1" "$RESET"; }
ok()   { PASS=$((PASS + 1)); printf '  %sPASS%s %s\n' "$GREEN" "$RESET" "$1"; }
bad()  { FAIL=$((FAIL + 1)); FAILED_NAMES+=("$1")
         printf '  %sFAIL%s %s\n' "$RED" "$RESET" "$1"
         if [ -n "${2:-}" ]; then printf '       %s%s%s\n' "$DIM" "$2" "$RESET"; fi; }
# A case that needs a feature no plan task has delivered yet. It is reported
# separately from failures so a green run stays meaningful.
skip() { SKIP=$((SKIP + 1))
         printf '  %sSKIP%s %s' "$YELLOW" "$RESET" "$1"
         if [ -n "${2:-}" ]; then printf ' %s(%s)%s' "$DIM" "$2" "$RESET"; fi
         printf '\n'; }

# ── 构建 ────────────────────────────────────────────────────────────────────
if [ "$BUILD" = "1" ]; then
    section "构建 / Build"
    if cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
             -DSUKICODE_BUILD_TESTS=ON > /tmp/suki_cfg.log 2>&1 &&
       cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)" \
             > /tmp/suki_build.log 2>&1; then
        ok "构建成功"
    else
        bad "构建失败" "见 /tmp/suki_cfg.log 与 /tmp/suki_build.log"
        exit 1
    fi
fi

if [ ! -x "$SUKIC" ]; then
    printf '%s未找到编译器 %s，请先构建。%s\n' "$RED" "$SUKIC" "$RESET"
    exit 1
fi

# ── 1. 宿主单元测试 ─────────────────────────────────────────────────────────
section "宿主单元测试 / Host unit tests"
run_unit() {
    local name="$1" exe="$BUILD_DIR/bin/$1" out
    if [ ! -x "$exe" ]; then bad "$name" "未构建（$exe 不存在）"; return; fi
    out="$("$exe" 2>&1)"; local rc=$?
    if [ "$rc" = "0" ]; then
        local tally
        tally="$(printf '%s' "$out" | grep -oE '[0-9]+/[0-9]+ checks' | tail -1)"
        ok "$name ${DIM}${tally}${RESET}"
    else
        bad "$name" "$(printf '%s' "$out" | grep -E 'FAIL' | head -3 | tr '\n' ';')"
    fi
}
for u in test_lexer test_parser test_sema test_codegen test_runtime; do
    run_unit "$u"
done

if [ "$QUICK" = "1" ]; then
    section "结果 / Summary"
    printf '  通过 %d，失败 %d\n' "$PASS" "$FAIL"
    [ "$FAIL" = "0" ] || exit 1
    exit 0
fi

# ── 2. .suki 正例：编译 + 运行，退出码即预期值 ──────────────────────────────
section "语言用例：编译并运行 / .suki positive cases"

# run_suki <file> <expected-exit> <label>
run_suki() {
    local src="$1" expected="$2" label="$3" out rc
    out="$(timeout "$CASE_TIMEOUT" "$SUKIC" run "$src" 2>&1)"; rc=$?
    # 124 is the timeout(1) status: the program did not terminate in time.
    if [ "$rc" = "124" ]; then
        bad "$label" "超时（>${CASE_TIMEOUT}s）——程序可能死循环"
        return
    fi
    if [ "$rc" = "$expected" ]; then
        ok "$label"
    else
        bad "$label" "退出码 $rc（期望 $expected）；$(printf '%s' "$out" | head -2 | tr '\n' ' ')"
    fi
}

# 用例以 `return N` 表示第 N 项断言失败，因此期望退出码为 0。
for f in "$PROJECT_DIR"/moduleTest/codegen/*.suki; do
    [ -e "$f" ] || continue
    run_suki "$f" 0 "codegen/$(basename "$f" .suki)"
done

# 示例程序分两类判定：
#   1) 编译必须成功（emit-ir 通过 IR 验证）—— 编译失败一律 FAIL，
#      绝不能因为 `sukic check` 通过而被判 PASS（那会掩盖代码生成的错误）；
#   2) 运行阶段：退出码 0 通过；1..125 视为示例自身以返回值表达的结果；
#      126 及以上（信号、命令错误）为失败；没有 @main 的示例只做编译验证。
for f in "$PROJECT_DIR"/examples/*.suki; do
    [ -e "$f" ] || continue
    n="$(basename "$f" .suki)"
    compile_err="$(timeout "$CASE_TIMEOUT" "$SUKIC" emit-ir "$f" 2>&1 >/dev/null)"
    if [ -n "$compile_err" ]; then
        if reason="$(pending_reason "examples/$n.suki")"; then
            skip "examples/$n" "待实现：$reason"
        else
            bad "examples/$n" "编译失败：$(printf '%s' "$compile_err" | head -2 | tr '\n' ' ')"
        fi
        continue
    fi
    run_out="$(timeout "$CASE_TIMEOUT" "$SUKIC" run "$f" 2>&1)"; rc=$?
    if [ "$rc" = "0" ]; then
        ok "examples/$n"
    elif [ "$rc" = "124" ]; then
        bad "examples/$n" "运行超时（>${CASE_TIMEOUT}s）"
    elif printf '%s' "$run_out" | grep -q "no @main entry"; then
        skip "examples/$n" "无 @main 入口，仅做编译验证"
    elif [ "$rc" -ge 126 ]; then
        bad "examples/$n" "运行崩溃或链接失败（退出码 $rc）：$(printf '%s' "$run_out" | head -2 | tr '\n' ' ')"
    else
        ok "examples/$n ${DIM}(运行返回 $rc，编译与链接均通过)${RESET}"
    fi
done

# ── 3. 负例：必须被拒绝并给出诊断 ───────────────────────────────────────────
section "负例诊断 / Negative diagnostics"
# run_negative <file> <expected-substring> <label>
run_negative() {
    local src="$1" needle="$2" label="$3" out
    out="$(timeout "$CASE_TIMEOUT" "$SUKIC" check "$src" 2>&1)"
    if printf '%s' "$out" | grep -qi "$needle"; then
        ok "$label"
    else
        bad "$label" "未报出包含 '$needle' 的诊断"
    fi
}

D="$PROJECT_DIR/moduleTest/diagnostics"
# 带明确预期子串的重点负例（保持严格校验）。
run_negative "$D/type_mismatch.suki"      "cannot convert" "type_mismatch"
run_negative "$D/unknown_name.suki"       "cannot find"   "unknown_name"
run_negative "$D/missing_return.suki"     "missing return" "missing_return"
run_negative "$D/tuple_index_range.suki"  "out of range"  "tuple_index_range"
run_negative "$D/enum_arity.suki"          "expects"       "enum_arity"
run_negative "$D/switch_exhaustive.suki"   "exhaustive"   "switch_exhaustive"
run_negative "$D/access_override.suki"      "final"         "access_override"
# 覆盖该目录下全部其余负例：每个文件经 `sukic check` 必须被拒绝（退出码非 0）。
# 与 CTest 的 Rejects_* 用例一致，但在此以脚本方式统一执行，新增负例后无需
# 再到此处登记即可被覆盖。
_STRICT=" type_mismatch unknown_name missing_return tuple_index_range enum_arity switch_exhaustive access_override "
for f in "$D"/*.suki; do
    [ -e "$f" ] || continue
    n="$(basename "$f" .suki)"
    case "$_STRICT" in
        *" $n "*) continue ;;   # 已在上面按子串严格校验
    esac
    if timeout "$CASE_TIMEOUT" "$SUKIC" check "$f" > /dev/null 2>&1; then
        bad "diagnostics/$n" "负例竟被接受（sukic check 退出码 0）"
    else
        ok "diagnostics/$n"
    fi
done

# ── 4. 待实现特性用例：登记过的缺口允许失败，意外通过则报警 ──────────────────
section "待实现特性 / Pending feature cases"

# A pending positive case succeeds only when it compiles *and* runs to exit 0.
run_pending_positive() {
    timeout "$CASE_TIMEOUT" "$SUKIC" run "$1" > /dev/null 2>&1
}

PENDING_DIR="$PROJECT_DIR/moduleTest/pending"
if [ -d "$PENDING_DIR" ]; then
    for f in "$PENDING_DIR"/*.suki; do
        [ -e "$f" ] || continue
        n="$(basename "$f")"
        rel="moduleTest/pending/$n"
        if ! reason="$(pending_reason "$rel")"; then
            bad "$rel" "未登记到 KNOWN_PENDING（请在脚本中登记其所属任务）"
            continue
        fi
        if [ "$(pending_kind "$rel")" = "negative" ]; then
            # 负例：期望被语义检查拒绝。若已被拒绝，说明校验已实现 ——
            # 该用例应移入 moduleTest/diagnostics 并从 KNOWN_PENDING 移除。
            if timeout "$CASE_TIMEOUT" "$SUKIC" check "$f" > /dev/null 2>&1; then
                skip "$rel" "待实现：$reason"
            else
                bad "$rel" "已能报出诊断：请把 $n 移入 moduleTest/diagnostics 并撤销登记"
            fi
        else
            # 正例：期望编译并运行成功（退出码 0）。
            # 未实现的特性可能让编译器本身崩溃，调用处的 stderr 也一并丢弃，
            # 免得 shell 的 "Segmentation fault" 报告混进测试结果里。
            if run_pending_positive "$f" 2>/dev/null; then
                bad "$rel" "已能编译运行通过：请把 $n 移入 moduleTest/codegen 并撤销登记"
            else
                skip "$rel" "待实现：$reason"
            fi
        fi
    done
fi

# ── 5. 静态检查：全部 .suki 至少能被解析 ────────────────────────────────────
section "解析健全性 / Parse sanity over all .suki"
PARSE_FAIL=0
PARSE_TOTAL=0
while IFS= read -r f; do
    PARSE_TOTAL=$((PARSE_TOTAL + 1))
    "$SUKIC" parse "$f" > /dev/null 2>&1 || { PARSE_FAIL=$((PARSE_FAIL + 1)); bad "parse $(basename "$f")"; }
done < <(find "$PROJECT_DIR" -name '*.suki' -not -path "*/build/*" | sort)
if [ "$PARSE_FAIL" = "0" ]; then
    ok "全部 $PARSE_TOTAL 个 .suki 文件解析通过"
fi

# ── 汇总 ────────────────────────────────────────────────────────────────────
section "结果 / Summary"
printf '  通过 %s%d%s，失败 %s%d%s，跳过 %s%d%s\n' \
       "$GREEN" "$PASS" "$RESET" \
       "$([ "$FAIL" -gt 0 ] && echo "$RED" || echo "$GREEN")" "$FAIL" "$RESET" \
       "$YELLOW" "$SKIP" "$RESET"
if [ "$FAIL" -gt 0 ]; then
    printf '\n  失败项：\n'
    for n in "${FAILED_NAMES[@]}"; do printf '    - %s\n' "$n"; done
    exit 1
fi
printf '\n  全部测试通过 ✅\n'
exit 0
