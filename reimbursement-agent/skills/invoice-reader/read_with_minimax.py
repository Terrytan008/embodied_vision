#!/usr/bin/env python3
"""
使用 MiniMax 多模态模型读取发票内容
"""
import os, sys, subprocess, re, json, tempfile
from pathlib import Path

MINIMAX_API_KEY = os.environ.get("MINIMAX_API_KEY", "")
MINIMAX_API_HOST = os.environ.get("MINIMAX_API_HOST", "https://api.minimaxi.com")

PROMPT = """你是一张中国发票的信息提取专家。请从这张发票图片中提取以下信息，并以JSON格式返回：

{
    "invoice_type": "发票类型，如：电子发票（普通发票）",
    "invoice_number": "发票号码",
    "date": "开票日期，格式：YYYY-MM-DD",
    "buyer": "购买方名称",
    "seller": "销售方名称",
    "service_type": "货物或应税劳务、服务名称",
    "amount": "金额（小写/含税金额），数字格式",
    "currency": "货币代码：CNY/USD/HKD等",
    "tax_rate": "税率，如：6%",
    "tax_amount": "税额",
    "total_chinese": "价税合计大写，如：伍佰伍拾圆整",
    "category": "建议报销类别：交通出行/餐饮美食/办公用品/酒店住宿/其他"
}

请只返回JSON，不要有其他文字。"""

def pdf_to_image(pdf_path, output_path):
    """将 PDF 转换为图片"""
    try:
        subprocess.run([
            "pdftoppm", "-png", "-r", "200", "-singlepage",
            pdf_path, output_path.replace(".png", "")
        ], capture_output=True, timeout=30)
        return os.path.exists(output_path)
    except Exception as e:
        print(f"  PDF转图片失败: {e}")
        return False

def extract_amount_and_info(text):
    """从文本提取金额和基本信息"""
    info = {}
    
    # 金额
    m = re.search(r"[（(]?\s*小\s*写[）\)]?\s*[￥¥$]?\s*([0-9,]+\.?\d*)", text, re.DOTALL)
    if m:
        info["amount"] = float(m.group(1).replace(",", ""))
    
    # 货币
    if re.search(r"US\$|USD|Dollar", text, re.I):
        info["currency"] = "USD"
    elif re.search(r"HK\$|HKD|港币", text, re.I):
        info["currency"] = "HKD"
    else:
        info["currency"] = "CNY"
    
    # 日期
    m = re.search(r"(\d{4})年(\d{1,2})月(\d{1,2})日", text)
    if m:
        info["date"] = f"{m.group(1)}-{m.group(2).zfill(2)}-{m.group(3).zfill(2)}"
    
    # 发票号码
    for p in [r"发票号码[：:]\s*([0-9A-Z]{8,20})", r"发票号[码]?[：:]\s*([0-9A-Z]{8,20})"]:
        m = re.search(p, text, re.I)
        if m:
            info["invoice_number"] = m.group(1).strip()
            break
    
    # 购买方
    m = re.search(r"称[：:]\s*([^\n]{5,30}公司)", text)
    if m:
        info["buyer"] = m.group(1).strip()
    
    # 销售方
    m = re.search(r"销售方[^\n]*称[：:]\s*([^\n]{2,30})", text)
    if m:
        info["seller"] = m.group(1).strip()
    
    return info

def classify(text):
    """分类"""
    t = text.lower()
    cats = {
        "交通出行": ["交通", "出行", "打车", "滴滴", "出租车", "火车票", "机票", "航空", "地铁", "公交", "高速", "ETC", "汽油", "充电", "uber", "didi", "高德", "行程", "停车", "过路费"],
        "餐饮美食": ["餐饮", "餐费", "饭店", "餐厅", "午餐", "晚餐", "美食", "外卖", "奶茶", "咖啡", "酒楼"],
        "办公用品": ["办公", "文具", "打印", "耗材", "电脑", "设备", "半导体", "立创", "电子元件"],
        "酒店住宿": ["酒店", "住宿", "宾馆", "旅馆", "民宿", "房费", "hotel"],
    }
    for cat, kws in cats.items():
        if any(kw in t for kw in kws):
            return cat
    return "其他"

def call_minimax_vision(image_path):
    """调用 MiniMax 多模态 API 分析发票图片"""
    if not MINIMAX_API_KEY:
        return None
    
    import urllib.request
    import urllib.parse
    
    with open(image_path, "rb") as f:
        import base64
        img_data = base64.b64encode(f.read()).decode()
    
    payload = {
        "model": "MiniMax-VL-01",
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "image_url", "data": f"data:image/png;base64,{img_data}"},
                    {"type": "text", "text": PROMPT}
                ]
            }
        ],
        "temperature": 0.1
    }
    
    req = urllib.request.Request(
        f"{MINIMAX_API_HOST}/v1/chat/completions",
        data=json.dumps(payload).encode(),
        headers={
            "Authorization": f"Bearer {MINIMAX_API_KEY}",
            "Content-Type": "application/json"
        },
        method="POST"
    )
    
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            result = json.loads(resp.read().decode())
            content = result["choices"][0]["message"]["content"]
            # 尝试解析 JSON
            if "```json" in content:
                content = content.split("```json")[1].split("```")[0]
            elif "```" in content:
                content = content.split("```")[1].split("```")[0]
            return json.loads(content.strip())
    except Exception as e:
        print(f"  MiniMax API 调用失败: {e}")
        return None

def read_invoice(pdf_path):
    """读取发票信息 - 优先使用 MiniMax"""
    print(f"  处理: {os.path.basename(pdf_path)}")
    
    # 先尝试 MiniMax
    if MINIMAX_API_KEY:
        with tempfile.TemporaryDirectory() as tmpdir:
            img_path = os.path.join(tmpdir, "invoice")
            if pdf_to_image(pdf_path, img_path + ".png"):
                result = call_minimax_vision(img_path + ".png")
                if result:
                    print(f"  ✓ MiniMax识别: {result.get('amount', 'N/A')} {result.get('currency', 'CNY')}")
                    return {
                        "amount": float(result.get("amount", 0)),
                        "currency": result.get("currency", "CNY"),
                        "date": result.get("date", ""),
                        "invoice_number": result.get("invoice_number", ""),
                        "category": classify(result.get("service_type", "") or result.get("seller", "")),
                        "buyer": result.get("buyer", ""),
                        "seller": result.get("seller", ""),
                        "source": "minimax"
                    }
    
    # 降级到 pdftotext
    try:
        r = subprocess.run(["pdftotext", "-enc", "UTF-8", pdf_path, "-"], 
                          capture_output=True, text=True, timeout=30)
        text = r.stdout if r.returncode == 0 else ""
    except:
        text = ""
    
    info = extract_amount_and_info(text)
    info["category"] = classify(text)
    info["source"] = "pdftotext"
    
    print(f"  ↩️ 降级pdftotext: {info.get('amount', 0)} {info.get('currency', 'CNY')}")
    return info

if __name__ == "__main__":
    # Test
    if len(sys.argv) > 1:
        result = read_invoice(sys.argv[1])
        print(json.dumps(result, ensure_ascii=False, indent=2))
