#!/usr/bin/env python3
"""
高级发票 OCR 读取 - 专为发票优化
支持 pdftotext + tesseract 多重验证
"""
import os, re, subprocess, hashlib
from pathlib import Path
from datetime import datetime

def enhance_image(img_path, out_path=None):
    """使用 ImageMagick 增强图片质量"""
    if out_path is None:
        out_path = img_path
    try:
        # 增强对比度、锐化、灰度化
        subprocess.run([
            "convert", img_path,
            "-colorspace", "Gray",
            "-contrast", "-unsharp", "1.5x1.2+0.5+0",
            "-deskew", "40%",
            "-resize", "200%",
            "-quality", "100",
            out_path
        ], capture_output=True, timeout=30)
        return os.path.exists(out_path)
    except:
        return False

def pdf_to_images(pdf_path, output_dir):
    """将 PDF 转换为增强图片"""
    try:
        subprocess.run([
            "pdftoppm", "-png", "-r", "400", "-singlepage",
            pdf_path, os.path.join(output_dir, "page")
        ], capture_output=True, timeout=60)
        return True
    except:
        return False

def extract_amount_from_text(text):
    """从文本提取金额 - 多种模式优先取含税金额"""
    text = text.replace('＿', '_').replace('—', '-').replace('﹣', '-')
    text = text.replace('￥', '¥').replace('圆', '元')
    
    currency = 'CNY'
    if re.search(r'US\$|USD|Dollar', text, re.I): currency = 'USD'
    elif re.search(r'HK\$|HKD|港币', text, re.I): currency = 'HKD'
    elif re.search(r'JPY|JP¥|日元', text, re.I): currency = 'JPY'
    
    # 优先模式1: 小写（含税）- 最准确
    patterns1 = [
        r'[（(]\s*小\s*写\s*[）\)]\s*[¥￥$]?\s*([0-9,]+\.?\d*)',
        r'小\s*写[：:]\s*[¥￥$]?\s*([0-9,]+\.?\d*)',
        r'小\s*写\s*[¥￥$]?\s*([0-9,]+\.?\d*)',
    ]
    for p in patterns1:
        m = re.search(p, text, re.DOTALL)
        if m:
            try:
                v = float(m.group(1).replace(',', ''))
                if 0 < v < 1000000:
                    return v, currency
            except: pass
    
    # 模式2: 价税合计
    patterns2 = [
        r'价税合计[^\d]*?\s*[¥￥$]?\s*([0-9,]+\.?\d*)',
        r'含税金额[：:]?\s*[¥￥$]?\s*([0-9,]+\.?\d*)',
    ]
    for p in patterns2:
        m = re.search(p, text, re.DOTALL)
        if m:
            try:
                v = float(m.group(1).replace(',', ''))
                if 0 < v < 1000000:
                    return v, currency
            except: pass
    
    # 模式3: 合计/总计 (包括无空格情况)
    patterns3 = [
        r'合\s*计[^\d]*?\s*[¥￥$]?\s*([0-9,]+\.?\d*)',
        r'总\s*计[^\d]*?\s*[¥￥$]?\s*([0-9,]+\.?\d*)',
    ]
    for p in patterns3:
        m = re.search(p, text, re.DOTALL)
        if m:
            try:
                v = float(m.group(1).replace(',', ''))
                if 0 < v < 100000:
                    return v, currency
            except: pass
    
    # 模式4: ¥数字 元 或 数字元
    patterns4 = [
        r'[¥￥$]\s*([0-9,]+\.?\d*)\s*元',
        r'([0-9,]+\.?\d*)\s*元',
    ]
    for p in patterns4:
        m = re.search(p, text)
        if m:
            try:
                v = float(m.group(1).replace(',', ''))
                if 0 < v < 100000:
                    return v, currency
            except: pass
    
    # 模式5: 票价（12306火车票）如 "票价:￥778.50" 或 "票价:¥778.50"
    patterns5 = [
        r'票价[：:]*[¥￥$]?\s*([0-9,]+\.?\d*)',
    ]
    for p in patterns5:
        m = re.search(p, text)
        if m:
            try:
                v = float(m.group(1).replace(',', ''))
                if 0 < v < 100000:
                    return v, currency
            except: pass
    
    return 0.0, currency

def extract_date(text):
    """提取日期"""
    # 格式1: 2026年04月04日
    m = re.search(r'(\d{4})\s*年\s*(\d{1,2})\s*月\s*(\d{1,2})\s*日', text)
    if m:
        return f"{m.group(1)}-{m.group(2).zfill(2)}-{m.group(3).zfill(2)}"
    # 格式2: 开票申请日期: 2026-04-04
    m = re.search(r'开票申请日期[:：]\s*(\d{4})-(\d{1,2})-(\d{1,2})', text)
    if m:
        return f"{m.group(1)}-{m.group(2).zfill(2)}-{m.group(3).zfill(2)}"
    return ""

def extract_invoice_number(text):
    """提取发票号码"""
    for p in [r'发票号码[：:\s]*([0-9A-Z]{8,20})', r'发票号[码]?[：:\s]*([0-9A-Z]{8,20})']:
        m = re.search(p, text, re.I)
        if m:
            return m.group(1).strip()
    return ""

def classify(text, fn):
    """分类"""
    t = text.lower()
    fn_lower = fn.lower()
    
    # 交通出行细分（按优先级排序）
    transport_subcats = {
        "机票": ["机票", "flight", "航空", "airline", "ia_rsv"],
        "停车费": ["停车费", "parking"],
        "ETC": ["etc", "高速", "过路费", "通行费", "收费公路"],
        "网约车": ["滴滴", "didi", "uber", "高德", "打车", "网约车", "行程", "itinerary"],
        "汽油": ["汽油", "加油", "燃油", "石化"],
        "充电": ["充电", "电动车", "充电桩"],
        "代驾": ["代驾"],
        "租车": ["租车", "car rental"],
    }
    
    for subcat, kws in transport_subcats.items():
        for kw in kws:
            if kw.lower() in t or kw.lower() in fn_lower:
                return "交通出行", subcat
    
    # 主分类
    cats = {
        "餐饮招待": ["餐饮", "餐费", "餐服务", "饭店", "餐厅", "午餐", "晚餐", "美食", "外卖", "奶茶", "咖啡", "酒楼"],
        "办公用品": ["办公", "文具", "打印", "耗材", "电脑", "设备", "半导体", "立创", "电子元件"],
        "酒店住宿": ["酒店", "住宿", "宾馆", "旅馆", "民宿", "hotel"],
    }
    for cat, kws in cats.items():
        if any(kw.lower() in t for kw in kws):
            return cat, ""
    
    return "其他", ""

def read_invoice_with_best_ocr(pdf_path, temp_dir="/tmp"):
    """使用最佳OCR策略读取发票"""
    basename = os.path.basename(pdf_path)
    
    # 方法1: pdftotext (最快，最准确直接)
    try:
        r = subprocess.run(['pdftotext', '-enc', 'UTF-8', '-layout', pdf_path, '-'],
                         capture_output=True, text=True, timeout=30)
        if r.returncode == 0 and r.stdout.strip():
            text1 = r.stdout
            amount1, currency1 = extract_amount_from_text(text1)
            if amount1 > 0:
                return {
                    "amount": amount1, "currency": currency1,
                    "date": extract_date(text1),
                    "invoice_number": extract_invoice_number(text1),
                    "text": text1[:500],
                    "method": "pdftotext",
                    "category": classify(text1, basename)
                }
    except: pass
    
    # 方法2: tesseract OCR (处理图片型PDF)
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        img_base = os.path.join(tmpdir, "invoice")
        try:
            subprocess.run(['pdftoppm', '-png', '-r', '400', '-singlepage', pdf_path, img_base],
                         capture_output=True, timeout=60)
            
            img_path = img_base + "-1.png"
            if not os.path.exists(img_path):
                img_path = img_base + ".png"
            
            if os.path.exists(img_path):
                # 增强图片
                enhanced_path = os.path.join(tmpdir, "enhanced.png")
                enhance_image(img_path, enhanced_path)
                
                # OCR 多种语言和PSM模式
                best_result = None
                best_amount = 0
                
                configs = [
                    ('-l', 'chi_sim+eng', '--psm', '6'),  # 简体中文+英文，假设单栏
                    ('-l', 'chi_sim+eng', '--psm', '4'),  # 假设单栏
                    ('-l', 'eng', '--psm', '6'),  # 纯英文
                ]
                
                for cfg in configs:
                    r = subprocess.run(['tesseract', enhanced_path, 'stdout'] + list(cfg),
                                      capture_output=True, text=True, timeout=60)
                    if r.returncode == 0 and r.stdout.strip():
                        text = r.stdout
                        amount, currency = extract_amount_from_text(text)
                        if amount > best_amount:
                            best_amount = amount
                            best_result = {"text": text, "amount": amount, "currency": currency}
                
                if best_result:
                    text2 = best_result["text"]
                    return {
                        "amount": best_result["amount"],
                        "currency": best_result["currency"],
                        "date": extract_date(text2),
                        "invoice_number": extract_invoice_number(text2),
                        "text": text2[:500],
                        "method": "tesseract",
                        "category": classify(text2, basename)
                    }
        except: pass
    
    # 方法3: 降级尝试
    try:
        r = subprocess.run(['pdftotext', pdf_path, '-'],
                         capture_output=True, text=True, timeout=30)
        if r.returncode == 0 and r.stdout.strip():
            text3 = r.stdout
            amount3, currency3 = extract_amount_from_text(text3)
            return {
                "amount": amount3, "currency": currency3,
                "date": extract_date(text3),
                "invoice_number": extract_invoice_number(text3),
                "text": text3[:500],
                "method": "pdftotext-fallback",
                "category": classify(text3, basename)
            }
    except: pass
    
    return {
        "amount": 0, "currency": "CNY",
        "date": datetime.now().strftime("%Y-%m-%d"),
        "invoice_number": "", "text": "",
        "method": "none", "category": ("其他", "")
    }

if __name__ == "__main__":
    import sys, json
    if len(sys.argv) > 1:
        result = read_invoice_with_best_ocr(sys.argv[1])
        print(json.dumps(result, ensure_ascii=False, indent=2))
