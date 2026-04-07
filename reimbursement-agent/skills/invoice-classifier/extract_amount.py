def extract_amount(text):
    """从 PDF 文字提取含税金额，返回 (金额, 货币类型)"""
    import re
    currency = detect_currency(text)
    
    # 货币符号映射
    currency_symbol = {
        'CNY': '￥¥',
        'USD': '$',  # USD uses $ not US$
        'HKD': 'HK$',
        'JPY': '￥',
        'GBP': '£',
        'EUR': '€'
    }
    sym = currency_symbol.get(currency, '￥¥')
    
    # 1. 优先找"小写"（含税金额）- 使用 re.DOTALL 处理换行
    patterns_small = [
        rf'小写[）\)]?\s*[{sym}]\s*([0-9,]+\.?\d*)',
        rf'小写[：:]\s*[{sym}]?\s*([0-9,]+\.?\d*)',
    ]
    
    for pattern in patterns_small:
        match = re.search(pattern, text, re.DOTALL)
        if match:
            amount_str = match.group(1).replace(',', '')
            try:
                amount = float(amount_str)
                if 0 < amount < 1000000:
                    return amount, currency
            except:
                pass
    
    # 2. 其次找"价税合计"（含税金额）
    patterns_tax = [
        rf'价税合计[^\n]*?\s*[{sym}]\s*([0-9,]+\.?\d*)',
        rf'含税金额[：:]?\s*[{sym}]?\s*([0-9,]+\.?\d*)',
    ]
    
    for pattern in patterns_tax:
        match = re.search(pattern, text, re.DOTALL)
        if match:
            amount_str = match.group(1).replace(',', '')
            try:
                amount = float(amount_str)
                if 0 < amount < 1000000:
                    return amount, currency
            except:
                pass
    
    # 3. 最后找"总计"或"合计"（对于非增值税发票如Uber）
    # 支持多种格式：总计: $11.95 或 总计\nUS$11.95
    if currency == 'USD':
        # 对于美元，找 总计 后面跟着 $ 符号
        patterns_total = [
            rf'总计[^\$]*\$([0-9,]+\.?\d*)',  # 总计...$数字
            rf'\$([0-9,]+\.?\d*)',  # 直接找 $数字
        ]
    else:
        patterns_total = [
            rf'总计[：:]*\s*[{sym}]([0-9,]+\.?\d*)',
            rf'合计[：:]*\s*[{sym}]([0-9,]+\.?\d*)',
            rf'[{sym}]([0-9,]+\.?\d*)',  # 兜底：货币符号后的数字
        ]
    
    for pattern in patterns_total:
        match = re.search(pattern, text, re.DOTALL)
        if match:
            amount_str = match.group(1).replace(',', '')
            try:
                amount = float(amount_str)
                if 0 < amount < 100000:
                    return amount, currency
            except:
                pass
    
    return 0.0, currency
