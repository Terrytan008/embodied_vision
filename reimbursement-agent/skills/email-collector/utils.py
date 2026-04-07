#!/usr/bin/env python3
"""邮箱收集工具函数：RFC2047解码、附件判断、URL提取、邮件正文提取"""
import re
import email
import email.header
from pathlib import Path

# 发件人关键词（识别发票邮件）
INVOICE_SENDERS = [
    'didifapiao', '滴滴', 'didi',
    'itinerary', '高德', 'amap',
    'trip', '携程', 'ctrip',
    'overseas_rsv', 'etc', 'ETC',
    '发票', '报销', 'invoice', 'fapiao'
]

# URL跳过模式（logo、banner、tracking pixel 等无关图片）
URL_SKIP_PATTERNS = [
    'logo', 'banner', 'edm', 'open', 'seal', 'qr', 'code',
    'phone', 'app', 'wechat', 'gif', '600x', '600000',
    'tps-', '!!', 'yuandian', 'round', 'openEdm',
    'ctrip', 'trip.com', 'inv', 'invoicelogo'
]


def decode_str(s):
    """解码邮件头字符串（支持多行 RFC 2047 编码）"""
    if not s:
        return ""
    result = ""
    s = s.replace('\r\n', '\n').replace('\r', '\n')
    decoded = email.header.decode_header(s)
    for part, charset in decoded:
        if isinstance(part, bytes):
            charset = charset or 'utf-8'
            for enc in [charset, 'utf-8', 'latin-1', 'ascii']:
                try:
                    result += part.decode(enc, errors='ignore')
                    break
                except Exception:
                    continue
        elif isinstance(part, str):
            result += part
    return result


def is_invoice_attachment(filename, content_type=''):
    """判断附件是否是发票相关"""
    if not filename:
        return False

    # RFC 2047 解码
    try:
        decoded_parts = email.header.decode_header(filename)
        for part in reversed(decoded_parts):
            if isinstance(part[0], bytes):
                filename = part[0].decode(part[1] or 'utf-8', errors='replace')
            else:
                filename = part[0]
    except Exception:
        pass

    fn_lower = filename.lower()

    # 按扩展名判断
    if any(fn_lower.endswith(ext) for ext in ['.pdf', '.jpg', '.jpeg', '.png', '.gif', '.ofd', '.xml', '.zip']):
        return True

    # 按内容类型判断
    if 'pdf' in content_type.lower():
        return True

    # 按关键词判断
    invoice_keywords = [
        '发票', 'fapiao', 'invoice', 'receipt',
        '行程', 'itinerary', '水单', '入住',
        'check', '酒店', 'hotel', 'etc'
    ]
    if any(kw in fn_lower for kw in invoice_keywords):
        return True

    return False


def get_email_body(msg):
    """提取邮件正文内容（纯文本）"""
    body = ""
    if msg.is_multipart():
        for part in msg.walk():
            if part.get_content_type() in ('text/plain', 'text/html'):
                try:
                    payload = part.get_payload(decode=True)
                    charset = part.get_content_charset() or 'utf-8'
                    body += payload.decode(charset, errors='ignore')
                except Exception:
                    pass
    else:
        try:
            payload = msg.get_payload(decode=True)
            charset = msg.get_content_charset() or 'utf-8'
            body = payload.decode(charset, errors='ignore')
        except Exception:
            pass
    return body


def extract_urls_from_body(msg):
    """从邮件正文中提取 PDF/图片下载链接"""
    body = get_email_body(msg)
    url_pattern = re.compile(r'https?://[^\s<>"\'"]+', re.IGNORECASE)
    urls = []
    for url in url_pattern.findall(body):
        url_lower = url.lower()
        if any(ext in url_lower for ext in ['.pdf', '.jpg', '.jpeg', '.png', '.gif', '.ofd']):
            urls.append(url.rstrip('.,;:'))
    return urls


def is_relevant_url(url):
    """判断 URL 是否是发票相关（过滤 logo/banner 等无关图片）"""
    url_lower = url.lower()
    # PDF 直接通过
    if '.pdf' in url_lower:
        return True
    # 非 PDF 文件跳过 logo 等无关图片
    if any(p in url_lower for p in URL_SKIP_PATTERNS):
        return False
    return True
