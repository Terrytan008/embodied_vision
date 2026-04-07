#!/usr/bin/env python3
"""ZIP 发票包解压处理：ETC 通行费、12306 火车票"""
import re
import zipfile
import tempfile
import subprocess
from pathlib import Path


def extract_month_from_pdf(data):
    """从 PDF 内容提取开票月份"""
    month_str = '未知月份'
    with tempfile.NamedTemporaryFile(suffix='.pdf', delete=False) as tmp:
        tmp.write(data)
        tmp_path = tmp.name
    try:
        result = subprocess.run(
            ['pdftotext', '-layout', tmp_path, '-'],
            capture_output=True, text=True, timeout=10
        )
        if result.returncode == 0:
            # 格式1: 开票申请日期: 2026-04-04 (ETC)
            # 格式2: 开票日期:2026年04月07日 (12306)
            m = re.search(r'开票申请日期:\s*(\d{4})-(\d{2})-(\d{2})', result.stdout)
            if not m:
                m = re.search(r'开票日期[^0-9]*(\d{4})年(\d{2})月(\d{2})日', result.stdout)
            if m:
                month_str = f"{m.group(1)}{m.group(2)}"
    except Exception:
        pass
    finally:
        try:
            import os
            os.unlink(tmp_path)
        except Exception:
            pass
    return month_str


def classify_zip_type(zip_filename):
    """判断 ZIP 是 12306 火车票还是 ETC 通行费"""
    # 12306: 文件名是纯数字ID (如 26119110001000363051_16.zip)
    # ETC: 中文车牌开头 (如 粤BGF4860[1]_05.zip)
    clean_name = zip_filename.replace('[1]', '')
    if re.match(r'^\d+', clean_name):
        return '12306'
    return 'ETC'


def extract_from_zip(zip_path, output_dir, email_index):
    """解压 ZIP 文件，提取其中的 PDF"""
    extracted = []
    try:
        with zipfile.ZipFile(zip_path, 'r') as zf:
            zip_name = Path(zip_path).name
            zip_type = classify_zip_type(zip_name)

            # 从 ZIP 文件名提取车牌/票号（用于给内层文件命名）
            if zip_type == '12306':
                # 纯数字 ID，如 26119110001000363051_16.zip
                m = re.match(r'^(\d+)', zip_name.replace('[1]', ''))
                zip_plate = f"火车票_{m.group(1)[:10]}" if m else '火车票'
            else:
                # 中文车牌，如 粤BGF4860[1]_05.zip
                m = re.match(r'([\u4e00-\u9fa5A-Z]{1,2}[A-Z0-9]{5,6})', zip_name)
                zip_plate = m.group(1) if m else '未知车牌'

            for name in zf.namelist():
                name_lower = name.lower()
                if not name_lower.endswith(('.pdf', '.jpg', '.jpeg', '.png')):
                    continue

                # ETC: 跳过 apply.pdf，只保留 trans.pdf（按行程索引）
                # 12306: 所有 PDF 都保留
                if zip_type == 'ETC' and 'trans' not in name_lower and 'apply' in name_lower:
                    print(f"    跳过 apply.pdf: {name}")
                    continue

                data = zf.read(name)
                safe_name = ' '.join(name.split()).replace('/', '_').replace('\\', '_')

                # 优先从内层文件名提取票号/车牌，其次用外层 ZIP 文件名
                if zip_type == '12306':
                    m = re.match(r'^(\d+)', safe_name)
                    plate = f"火车票_{m.group(1)[:10]}" if m else zip_plate
                else:
                    m = re.match(r'([\u4e00-\u9fa5A-Z]{1,2}[A-Z0-9]{5,6})', safe_name)
                    plate = m.group(1) if m else zip_plate

                month_str = extract_month_from_pdf(data)
                out_name = f"{plate}_{month_str}_e{email_index:02d}.pdf"
                out_path = Path(output_dir) / out_name
                with open(out_path, 'wb') as f:
                    f.write(data)
                print(f"    解压: {out_name}")
                extracted.append(out_name)
    except Exception as e:
        print(f"    解压失败: {e}")
    return extracted
