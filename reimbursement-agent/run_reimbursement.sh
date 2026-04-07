#!/bin/bash
# 报销发票收集与汇总 - 一键执行脚本
# 用法: ./run_reimbursement.sh <月份> [本地文件夹路径]

set -e

# 配置（请根据实际情况修改）
EMAIL="terrytan007@qq.com"
IMAP_SERVER="imap.qq.com"
# 邮箱授权码（请替换为你的）
EMAIL_PASSWORD="dnfkhltnkeoeceaf"
# 输出目录
OUTPUT_DIR="$HOME/报销发票"
# 本地发票文件夹
LOCAL_FOLDER="/Users/terrytan/发票"

# 月份参数
MONTH="${1:-}"

if [ -z "$MONTH" ]; then
    echo "用法: $0 <月份> [本地文件夹路径]"
    echo "示例: $0 2026-03"
    echo "      $0 2026-03 /Users/xxx/发票"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILLS_DIR="$SCRIPT_DIR/skills"

echo "======================================"
echo "  报销发票收集与汇总"
echo "  月份: $MONTH"
echo "======================================"

# Step 0: 清理上次的输出（避免旧文件残留）
echo ""
echo "[0/4] 清理上次输出..."
if [ -d "$OUTPUT_DIR/$MONTH\_报销发票" ]; then
    echo "  删除旧输出文件夹: $OUTPUT_DIR/$MONTH\_报销发票"
    rm -rf "$OUTPUT_DIR/$MONTH\_报销发票"
fi

# Step 1: 创建目录
# 只创建 downloaded 目录，输出的发票汇总表.xlsx 直接放在 $OUTPUT_DIR 下
echo ""
echo "[1/4] 创建目录..."
mkdir -p "$OUTPUT_DIR/downloaded"

# Step 2: 从邮箱下载发票
echo ""
echo "[2/4] 从邮箱下载发票..."
if [ -n "$EMAIL_PASSWORD" ] && [ "$EMAIL_PASSWORD" != "your授权码" ]; then
    python3 "$SKILLS_DIR/email-collector/collect.py" \
        --email "$EMAIL" \
        --imap "$IMAP_SERVER" \
        --password "$EMAIL_PASSWORD" \
        --month "$MONTH" \
        --output "$OUTPUT_DIR/downloaded"
else
    echo "跳过邮箱下载（未配置授权码）"
fi

# Step 3: 分类与汇总
echo ""
echo "[3/4] 分类发票文件..."
EMAIL_DIR="$OUTPUT_DIR/downloaded/${MONTH}_邮箱发票"
LOCAL_DIR="${LOCAL_FOLDER:-}"

python3 "$SKILLS_DIR/invoice-classifier/classify.py" \
    --source "${LOCAL_DIR:-$OUTPUT_DIR}" \
    --output "$OUTPUT_DIR" \
    --month "$MONTH" \
    --email-dir "$EMAIL_DIR"

# Step 4: 完成
echo ""
echo "[4/4] 完成！"
echo "======================================"
echo "  发票目录: $OUTPUT_DIR/$MONTH\_报销发票/"
echo "  汇总表: $OUTPUT_DIR/$MONTH\_报销发票/发票汇总表.xlsx"
echo "======================================"
