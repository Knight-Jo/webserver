#!/bin/bash
# kama-webserver 性能基准测试脚本
# 使用 Apache Bench (ab) 测试 HTTP 服务器性能
#
# 用法: ./benchmark.sh [branch_name] [server_binary_path]
# 示例: ./benchmark.sh feat/libco-integration ./bin/main
#
# 结果保存在 bench_results/ 目录下

set -euo pipefail

BRANCH_NAME="${1:-unknown}"
SERVER_BIN="${2:-./bin/main}"
RESULT_DIR="bench_results/${BRANCH_NAME}"
SERVER_PORT=8080
SERVER_URL="http://localhost:${SERVER_PORT}"
WARMUP_REQUESTS=1000
WARMUP_CONCURRENCY=10

# 测试场景：并发数 × 总请求数
declare -a SCENARIOS=(
    "1|10000"        # 低并发，大量请求 — 纯延迟测试
    "10|50000"       # 中等并发
    "50|100000"      # 中高并发
    "100|200000"     # 高并发
    "200|200000"     # 超高并发
)

mkdir -p "${RESULT_DIR}"

cleanup() {
    echo ""
    echo ">>> 清理: 停止服务器..."
    if [ -n "${SERVER_PID:-}" ]; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    echo ">>> 服务器已停止"
}
trap cleanup EXIT INT TERM

# 启动服务器
start_server() {
    echo ">>> 启动服务器: ${SERVER_BIN}"
    if [ ! -x "${SERVER_BIN}" ]; then
        echo "错误: 服务器二进制文件不存在或不可执行: ${SERVER_BIN}"
        echo "请先编译项目: cd build && cmake .. && make -j\$(nproc)"
        exit 1
    fi
    ${SERVER_BIN} &
    SERVER_PID=$!

    # 等待服务器就绪
    for i in $(seq 1 30); do
        if curl -s "${SERVER_URL}/" >/dev/null 2>&1; then
            echo ">>> 服务器已就绪 (PID=${SERVER_PID})"
            return 0
        fi
        sleep 0.5
    done
    echo "错误: 服务器启动失败"
    exit 1
}

# 运行单个测试
run_benchmark() {
    local concurrency="$1"
    local requests="$2"
    local route="$3"
    local label="$4"
    local method="${5:-GET}"
    local post_data="${6:-}"

    # 安全的文件名
    local safe_route="${route//\//_}"
    [ "${safe_route}" = "_" ] && safe_route="root"
    local filename="${RESULT_DIR}/${label}_c${concurrency}_n${requests}_${safe_route}.txt"

    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo "  测试: ${label} | 并发=${concurrency} | 请求数=${requests} | ${method} ${route}"
    echo "═══════════════════════════════════════════════════════════════"

    local ab_cmd=("ab" "-k" "-c" "${concurrency}" "-n" "${requests}" "-g" "${filename}.tsv")

    if [ "${method}" = "POST" ]; then
        local postfile="${RESULT_DIR}/post_data_$$.tmp"
        printf "%s" "${post_data}" > "${postfile}"
        ab -k -c "${concurrency}" -n "${requests}" -p "${postfile}" -T "text/plain" \
            "${SERVER_URL}${route}" > "${filename}" 2>&1
        rm -f "${postfile}"
    else
        ab -k -c "${concurrency}" -n "${requests}" \
            "${SERVER_URL}${route}" > "${filename}" 2>&1
    fi

    echo "结果已保存: ${filename}"
    # 提取关键指标
    grep -E "(Requests per second|Time per request|Transfer rate|Failed requests|Complete requests)" "${filename}" || true
}

# 预热（避免首次请求的冷启动影响）
warmup() {
    echo ""
    echo ">>> 服务器预热: ${WARMUP_CONCURRENCY} 并发 × ${WARMUP_REQUESTS} 请求"
    ab -k -c "${WARMUP_CONCURRENCY}" -n "${WARMUP_REQUESTS}" "${SERVER_URL}/" > /dev/null 2>&1 || true
    sleep 1
    echo ">>> 预热完成"
}

# 收集系统信息
collect_sysinfo() {
    local sysinfo_file="${RESULT_DIR}/sysinfo.txt"
    {
        echo "========== 系统信息 =========="
        echo "日期: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "分支: ${BRANCH_NAME}"
        echo "服务器: ${SERVER_BIN}"
        echo "CPU: $(nproc) 核"
        echo ""
        echo "--- /proc/cpuinfo ---"
        grep -E "model name|cpu cores|siblings" /proc/cpuinfo | sort -u
        echo ""
        echo "--- 内存 ---"
        free -h
        echo ""
        echo "--- 内核 ---"
        uname -a
        echo ""
        echo "--- git 最近 3 个提交 ---"
        git log --oneline -3 2>/dev/null || echo "N/A"
    } > "${sysinfo_file}"
    echo ">>> 系统信息已保存: ${sysinfo_file}"
}

# ====== 主流程 ======
echo ""
echo "================================================================="
echo "  kama-webserver 性能基准测试"
echo "  分支: ${BRANCH_NAME}"
echo "  服务器: ${SERVER_BIN}"
echo "  结果目录: ${RESULT_DIR}/"
echo "================================================================="
echo ""

start_server
collect_sysinfo
warmup

# 测试 GET /
echo ""
echo "─────────────────────────────────────────────────────────────────"
echo "  测试 GET /"
echo "─────────────────────────────────────────────────────────────────"
for scenario in "${SCENARIOS[@]}"; do
    IFS='|' read -r concurrency requests <<< "${scenario}"
    run_benchmark "${concurrency}" "${requests}" "/" "GET" "GET"
done

# 测试 POST /echo
echo ""
echo "─────────────────────────────────────────────────────────────────"
echo "  测试 POST /echo"
echo "─────────────────────────────────────────────────────────────────"
for scenario in "${SCENARIOS[@]}"; do
    IFS='|' read -r concurrency requests <<< "${scenario}"
    run_benchmark "${concurrency}" "${requests}" "/echo" "POST" "POST" "Hello, this is a benchmark test payload for kama-webserver"
done

cleanup

echo ""
echo "================================================================="
echo "  测试完成！"
echo "  结果目录: ${RESULT_DIR}/"
echo "================================================================="

# 生成摘要
SUMMARY_FILE="${RESULT_DIR}/summary.txt"
{
    echo "============================================"
    echo "  性能摘要 - ${BRANCH_NAME}"
    echo "============================================"
    echo ""
    for f in "${RESULT_DIR}"/*.txt; do
        [ "$(basename "${f}")" = "summary.txt" ] && continue
        [ "$(basename "${f}")" = "sysinfo.txt" ] && continue
        base="$(basename "${f}" .txt)"
        rps="$(grep "Requests per second" "${f}" 2>/dev/null | awk '{print $4}')"
        tpr="$(grep "Time per request.*mean" "${f}" 2>/dev/null | head -1 | awk '{print $4}')"
        failed="$(grep "Failed requests" "${f}" 2>/dev/null | awk '{print $3}')"
        echo "${base}: RPS=${rps:-N/A}, TPR=${tpr:-N/A}ms, Failed=${failed:-N/A}"
    done
} > "${SUMMARY_FILE}"
echo "摘要: ${SUMMARY_FILE}"
cat "${SUMMARY_FILE}"

exit 0
