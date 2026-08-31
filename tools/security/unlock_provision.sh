#!/usr/bin/env bash
# ============================================================
#  ESP32-S3 生产解锁：一键生成密钥 + 嵌入公钥 + 签发令牌
#
#  流程: 1) 检测解锁密钥是否存在 (存在则退出, 防覆盖)
#        2) 生成 ECDSA P-256 密钥对 (generate-key)
#        3) 导出公钥为固件头文件 (export-public → main/unlock_public_key.h)
#        4) 签发通用解锁令牌 (issue → eyecare.unlock)
#        5) 用公钥复核令牌 (verify)
#
#  依赖: python3 + cryptography 库 (unlock_token.py 需要)
#  用法:
#    ./unlock_provision.sh                      # 默认路径一键执行
#    ./unlock_provision.sh --force              # 密钥已存在也强制重新生成
#    ./unlock_provision.sh --skip-keygen        # 密钥已存在, 只嵌入+签发
#    ./unlock_provision.sh --python /path/idf/bin/python   # 指定 Python (ESP-IDF 虚拟环境)
#    ./unlock_provision.sh --output /path/eyecare.unlock   # 自定义令牌输出路径
#    ./unlock_provision.sh --help               # 帮助
# ============================================================

set -euo pipefail

# ---- 路径解析: 以脚本所在目录为基准定位项目根 ----
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}" 2>/dev/null || echo "${BASH_SOURCE[0]}")")" && pwd)"
# tools/security → 项目根
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOL="$SCRIPT_DIR/unlock_token.py"

# ---- 默认参数 (可用环境变量覆盖) ----
KEY_DIR="${EYECARE_KEY_DIR:-$PROJECT_ROOT/info/HTML/key}"
PRIVATE_KEY="${EYECARE_PRIVATE_KEY:-$KEY_DIR/eyecare_unlock_ecdsa_p256}"
PUBLIC_KEY="${PRIVATE_KEY}.pub"
PUBLIC_HEADER="${EYECARE_PUBLIC_HEADER:-$PROJECT_ROOT/main/unlock_public_key.h}"
TOKEN_OUTPUT="${EYECARE_TOKEN_OUTPUT:-$PWD/eyecare.unlock}"
PYTHON="${EYECARE_PYTHON:-}"

FORCE=0
SKIP_KEYGEN=0

# ---- 小工具 ----
info()  { printf '\033[1;34m[INFO]\033[0m %s\n' "$*"; }
ok()    { printf '\033[1;32m[ OK ]\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m[WARN]\033[0m %s\n' "$*"; }
die()   { printf '\033[1;31m[ERR ]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

# ---- 参数解析 ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --force)        FORCE=1; shift ;;
        --skip-keygen)  SKIP_KEYGEN=1; shift ;;
        --python)       [[ $# -ge 2 ]] || die "--python 需要参数"; PYTHON="$2"; shift 2 ;;
        --output)       [[ $# -ge 2 ]] || die "--output 需要参数"; TOKEN_OUTPUT="$2"; shift 2 ;;
        --help|-h)      usage ;;
        *)              die "未知参数: $1 (用 --help 查看用法)" ;;
    esac
done

# ---- 1. 检测 Python 与 cryptography ----
detect_python() {
    if [[ -n "$PYTHON" ]]; then
        command -v "$PYTHON" >/dev/null 2>&1 \
            || die "指定的 Python 不存在: $PYTHON"
        return 0
    fi
    for cand in python3 python; do
        if command -v "$cand" >/dev/null 2>&1; then
            PYTHON="$cand"
            return 0
        fi
    done
    die "未找到 python3/python, 请安装或使用 --python 指定 (如 ESP-IDF 虚拟环境的 python)"
}

check_cryptography() {
    "$PYTHON" -c 'import cryptography' 2>/dev/null \
        || die "Python 缺少 cryptography 库; 请安装: pip install cryptography, 或用 --python 指向含该库的解释器 (ESP-IDF 虚拟环境已含)"
}

# ---- 2. 检测密钥 ----
check_existing_key() {
    if [[ -f "$PRIVATE_KEY" || -f "$PUBLIC_KEY" ]]; then
        if [[ $SKIP_KEYGEN -eq 1 ]]; then
            [[ -f "$PRIVATE_KEY" ]] || die "密钥不存在: $PRIVATE_KEY (--skip-keygen 需要已有密钥)"
            ok "检测到已有密钥, 跳过生成: $PRIVATE_KEY"
            return 0
        fi
        if [[ $FORCE -eq 1 ]]; then
            warn "检测到已有密钥, --force 将覆盖: $PRIVATE_KEY"
            return 0
        fi
        die "检测到解锁密钥已存在: $PRIVATE_KEY
  为避免覆盖已有密钥, 脚本已退出。
  若确认要重新生成: 使用 --force
  若只想用现有密钥嵌入+签发: 使用 --skip-keygen"
    fi
}

# ---- 3. 生成密钥对 ----
generate_keys() {
    info "生成 ECDSA P-256 密钥对 → $KEY_DIR"
    "$PYTHON" "$TOOL" generate-key --private-key "$PRIVATE_KEY"
}

# ---- 4. 导出公钥为固件头文件 ----
export_public_key() {
    info "导出公钥 → $PUBLIC_HEADER"
    "$PYTHON" "$TOOL" export-public --private-key "$PRIVATE_KEY" --output "$PUBLIC_HEADER"
}

# ---- 5. 签发通用解锁令牌 ----
issue_token() {
    info "签发通用解锁令牌 → $TOKEN_OUTPUT"
    "$PYTHON" "$TOOL" issue --private-key "$PRIVATE_KEY" --output "$TOKEN_OUTPUT"
}

# ---- 6. 复核令牌 ----
verify_token() {
    info "用公钥复核令牌"
    "$PYTHON" "$TOOL" verify --public-key "$PUBLIC_KEY" --token "$TOKEN_OUTPUT"
}

# ============================================================
# 主流程
# ============================================================
detect_python
check_cryptography
check_existing_key
[[ $SKIP_KEYGEN -eq 1 ]] || generate_keys
export_public_key
issue_token
verify_token

ok "全部完成!"
printf '\n  密钥(私): %s\n  公钥:     %s\n  固件头:   %s\n  令牌:     %s\n' \
    "$PRIVATE_KEY" "$PUBLIC_KEY" "$PUBLIC_HEADER" "$TOKEN_OUTPUT"
printf '\n  下一步: 把 %s 复制到 FAT/FAT32 TF 卡根目录, 插入任意设备即可解锁。\n' "$TOKEN_OUTPUT"
printf '  私钥 %s 绝不能提交、不能放入固件或 TF 卡。\n' "$PRIVATE_KEY"
