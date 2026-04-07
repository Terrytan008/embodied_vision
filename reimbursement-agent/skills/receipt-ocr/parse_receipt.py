#!/usr/bin/env python3
"""
发票 OCR 识别脚本
从发票图片中提取关键信息
"""

import sys
import json
import re
import argparse
from pathlib import Path

def parse_receipt(image_path):
    """
    解析发票图片，返回结构化数据
    目前为模拟实现，返回示例数据
    实际使用时需要接入 OCR 服务（如百度 OCR、腾讯 OCR）
    """
    path = Path(image_path)
    
    # 模拟返回（实际需要接入 OCR API）
    result = {
        "success": True,
        "file": str(path.name),
        "invoice_type": detect_invoice_type(image_path),
        "invoice_number": "1234567890",
        "invoice_code": "1234567890",
        "date": "2026-04-06",
        "amount": 0.0,
        "tax_amount": 0.0,
        "price_ex_tax": 0.0,
        "buyer_name": "",
        "buyer_tax_id": "",
        "seller_name": "",
        "seller_tax_id": "",
        "items": [],
        "raw_text": "",  # 原始 OCR 文本，供人工核对
        "confidence": 0.95
    }
    
    return result

def detect_invoice_type(filename):
    """根据文件名判断发票类型"""
    fname = filename.lower()
    if '增值税' in fname or '专用' in fname:
        return '增值税专用发票'
    elif '电子' in fname:
        return '电子发票'
    elif '出租车' in fname or 'taxi' in fname:
        return '出租车票'
    elif '火车' in fname:
        return '火车票'
    elif '机票' in fname or 'flight' in fname:
        return '机票行程单'
    else:
        return '增值税普通发票'

def extract_amount(text):
    """从文本中提取金额"""
    patterns = [
        r'价税合计[￥¥]?\s*([0-9,]+\.?\d*)',
        r'合计[￥¥]?\s*([0-9,]+\.?\d*)',
        r'金额[￥¥]?\s*([0-9,]+\.?\d*)',
    ]
    for pattern in patterns:
        match = re.search(pattern, text)
        if match:
            return float(match.group(1).replace(',', ''))
    return 0.0

def main():
    parser = argparse.ArgumentParser(description='发票 OCR 识别')
    parser.add_argument('image_path', help='发票图片路径')
    parser.add_argument('--output', '-o', help='输出 JSON 文件路径')
    parser.add_argument('--verbose', '-v', action='store_true', help='显示详细信息')
    
    args = parser.parse_args()
    
    if not Path(args.image_path).exists():
        print(f"错误: 文件不存在 {args.image_path}", file=sys.stderr)
        sys.exit(1)
    
    result = parse_receipt(args.image_path)
    
    if args.verbose:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print(json.dumps(result, ensure_ascii=False))
    
    if args.output:
        with open(args.output, 'w', encoding='utf-8') as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        print(f"结果已保存到: {args.output}")

if __name__ == '__main__':
    main()
