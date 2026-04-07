#!/usr/bin/env python3
"""
邮箱发票收集脚本 v2
增强附件识别和下载能力
"""

import sys
import os
import argparse
import email
import re
import urllib.request
import urllib.error
from datetime import datetime
from pathlib import Path
import imaplib
import email.header
import quopri
import base64
import ssl

# 发件人关键词（识别发票邮件）
INVOICE_SENDERS = [
    'didifapiao', '滴滴', 'didi', 
    'itinerary', '高德', 'amap',
    'trip', '携程', 'ctrip',
    'overseas_rsv', 'etc', 'ETC',
    '发票', '报销', 'invoice', 'fapiao'
]

def connect_imap(email_address, password, imap_server):
    """连接 IMAP 服务器"""
    try:
        mail = imaplib.IMAP4_SSL(imap_server)
        mail.login(email_address, password)
        return mail
    except Exception as e:
        print(f"连接失败: {e}")
        return None

def decode_str(s):
    """解码邮件头字符串（支持多行 RFC 2047 编码）"""
    if not s:
        return ""
    result = ""
    # 处理多行编码字符串
    s = s.replace('\r\n', '\n').replace('\r', '\n')
    # 使用 email.header 库解码
    decoded = email.header.decode_header(s)
    for part, charset in decoded:
        if isinstance(part, bytes):
            charset = charset or 'utf-8'
            try:
                result += part.decode(charset, errors='ignore')
            except:
                try:
                    result += part.decode('utf-8', errors='ignore')
                except:
                    try:
                        result += part.decode('latin-1', errors='ignore')
                    except:
                        result += part.decode('ascii', errors='ignore')
        elif isinstance(part, str):
            result += part
    return result

def is_invoice_attachment(filename, content_type=''):
    """判断附件是否是发票相关"""
    if not filename:
        return False
    
    # 先解码 RFC 2047 编码的文件名
    try:
        decoded_parts = email.header.decode_header(filename)
        # 取最后一个已解码的部分（通常是实际内容）
        for part in reversed(decoded_parts):
            if isinstance(part[0], bytes):
                filename = part[0].decode(part[1] or 'utf-8', errors='replace')
            else:
                filename = part[0]
    except: pass
    
    filename_lower = filename.lower()
    
    # 检查扩展名
    valid_exts = ['.pdf', '.jpg', '.jpeg', '.png', '.gif', '.ofd', '.xml', '.zip']
    if any(filename_lower.endswith(ext) for ext in valid_exts):
        return True
    
    # 检查内容类型
    if 'pdf' in content_type.lower():
        return True
    
    # 检查文件名是否含发票关键词
    invoice_keywords = ['发票', 'fapiao', 'invoice', 'receipt', '行程', 'itinerary', '水单', '入住', 'check', '酒店', 'hotel', 'etc']
    if any(kw in filename_lower for kw in invoice_keywords):
        return True
    
    return False

def extract_urls_from_body(msg):
    """从邮件正文中提取 PDF/图片下载链接"""
    urls = []
    
    # 获取邮件正文内容
    body = ""
    if msg.is_multipart():
        for part in msg.walk():
            content_type = part.get_content_type()
            if content_type in ('text/plain', 'text/html'):
                try:
                    payload = part.get_payload(decode=True)
                    charset = part.get_content_charset() or 'utf-8'
                    body += payload.decode(charset, errors='ignore')
                except:
                    pass
    else:
        try:
            payload = msg.get_payload(decode=True)
            charset = msg.get_content_charset() or 'utf-8'
            body = payload.decode(charset, errors='ignore')
        except:
            pass
    
    # 查找所有 URL（匹配 http/https 链接）
    # 匹配模式：各种格式的 URL
    url_pattern = re.compile(r'https?://[^\s<>"\'"]+', re.IGNORECASE)
    found_urls = url_pattern.findall(body)
    
    # 过滤出 PDF 和图片链接
    for url in found_urls:
        url_lower = url.lower()
        if any(ext in url_lower for ext in ['.pdf', '.jpg', '.jpeg', '.png', '.gif', '.ofd']):
            # 去掉 URL 末尾的可能标点符号
            url = url.rstrip('.,;:)')
            urls.append(url)
    
    return urls

def extract_email_body(msg):
    """提取邮件正文内容（纯文本，用于辅助分类）"""
    body = ""
    if msg.is_multipart():
        for part in msg.walk():
            content_type = part.get_content_type()
            if content_type in ('text/plain', 'text/html'):
                try:
                    payload = part.get_payload(decode=True)
                    charset = part.get_content_charset() or 'utf-8'
                    body += payload.decode(charset, errors='ignore')
                except:
                    pass
    else:
        try:
            payload = msg.get_payload(decode=True)
            charset = msg.get_content_charset() or 'utf-8'
            body = payload.decode(charset, errors='ignore')
        except:
            pass
    return body

def download_url(url, output_dir, sender_prefix, email_index, counter):
    """下载 URL 对应的文件"""
    try:
        # 创建 SSL context 允许自签名证书
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        
        # 设置请求头模拟浏览器
        req = urllib.request.Request(
            url,
            headers={
                'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36',
                'Accept': '*/*'
            }
        )
        
        with urllib.request.urlopen(req, context=ctx, timeout=30) as response:
            content = response.read()
            content_type = response.headers.get('Content-Type', '').lower()
            
            # 确定文件扩展名
            if '.pdf' in url.lower():
                ext = '.pdf'
            elif '.jpg' in url.lower() or 'jpeg' in url.lower():
                ext = '.jpg'
            elif '.png' in url.lower():
                ext = '.png'
            elif '.gif' in url.lower():
                ext = '.gif'
            elif '.ofd' in url.lower():
                ext = '.ofd'
            elif 'image' in content_type:
                ext = '.jpg'
            elif 'pdf' in content_type:
                ext = '.pdf'
            else:
                ext = '.pdf'
            
            # 从 URL 提取有意义的文件名部分
            url_filename = url.split('/')[-1]
            # 清理 URL 参数
            if '?' in url_filename:
                url_filename = url_filename.split('?')[0]
            if not url_filename or len(url_filename) < 3:
                url_filename = f"download_{counter}{ext}"
            
            # 生成唯一文件名
            safe_stem = url_filename[:150]
            new_filename = f"{safe_stem}_e{email_index:02d}{ext}"
            
            save_path = Path(output_dir) / new_filename
            with open(save_path, 'wb') as f:
                f.write(content)
            
            print(f"    ✓ 下载链接: {new_filename}")
            return new_filename
    except Exception as e:
        print(f"    ✗ 下载失败 [{url[:50]}...]: {e}")
        return None

def extract_attachments_from_msg(msg, output_dir, sender_prefix="unknown", seen_hashes=None, email_index=1):
    """从邮件中提取所有附件（带去重）"""
    if seen_hashes is None:
        seen_hashes = set()
    downloaded = []
    
    def save_attachment(part, filename, counter):
        """保存附件（基于内容哈希去重）"""
        try:
            payload = part.get_payload(decode=True)
            if not payload:
                return None
            
            # 计算文件内容哈希去重
            import hashlib
            file_hash = hashlib.md5(payload).hexdigest()
            if file_hash in seen_hashes:
                print(f"    ⊝ 内容完全相同（已存在）")
                return None
            seen_hashes.add(file_hash)
            
            # 解码文件名
            
            # 解码文件名
            filename = decode_str(filename)
            
            # 清理文件名
            # 先移除所有换行符和多余空白
            safe_filename = ' '.join(filename.split())
            safe_filename = safe_filename.replace('/', '_').replace('\\', '_')
            safe_filename = safe_filename.replace('<', '_').replace('>', '_')
            safe_filename = safe_filename.strip()
            
            # 保留原始扩展名
            ext = Path(safe_filename).suffix.lower()
            if not ext:
                ext = '.pdf' if 'pdf' in part.get_content_type() else ''
            
            # 保留原始文件名，只做安全清理（去掉路径分隔符等危险字符）
            # 文件名格式：原文件名_邮件序号，确保唯一性
            stem = Path(safe_filename).stem
            safe_stem = stem[:200]  # 限制长度避免问题
            new_filename = f"{safe_stem}_{email_index:02d}{ext}"
            
            save_path = Path(output_dir) / new_filename
            with open(save_path, 'wb') as f:
                f.write(payload)
            
            # 如果是 ZIP 文件，自动解压
            if ext == '.zip':
                import zipfile
                try:
                    with zipfile.ZipFile(save_path, 'r') as zf:
                        for name in zf.namelist():
                            if name.lower().endswith(('.pdf', '.jpg', '.jpeg', '.png')):
                                # ETC zip 只保留 trans.pdf（按行程索引），跳过 apply.pdf（按票据索引）
                                name_lower = name.lower()
                                if 'trans' not in name_lower and 'apply' in name_lower:
                                    print(f"    跳过 apply.pdf: {name}")
                                    continue
                                data = zf.read(name)
                                safe_name = ' '.join(name.split()).replace('/', '_').replace('\\', '_')
                                
                                # 从 ZIP 文件名提取车牌号（如：粤BGF4860[1]_05.zip -> 粤BGF4860）
                                import re
                                # 检查是否是12306火车票（文件名是纯数字ID，如 26119110001000363051_16.zip）
                                # 12306: 纯数字ID开头；ETC: 中文车牌开头
                                is_12306 = bool(re.match(r'^\d+', safe_filename.replace('[1]', '')))
                                
                                if is_12306:
                                    # 12306火车票：从文件名提取车票ID
                                    ticket_match = re.match(r'^(\d+)', safe_filename.replace('[1]', ''))
                                    ticket_id = ticket_match.group(1)[:10] if ticket_match else '未知'
                                    plate = f"火车票"
                                else:
                                    # ETC：匹配中文车牌号格式
                                    plate_match = re.match(r'([粤粤A-Z]{1,2}[A-Z0-9]{5,6})', safe_filename)
                                    plate = plate_match.group(1) if plate_match else '未知车牌'
                                
                                # 从 PDF 内容提取开票月份
                                import tempfile
                                with tempfile.NamedTemporaryFile(suffix='.pdf', delete=False) as tmp:
                                    tmp.write(data)
                                    tmp_path = tmp.name
                                
                                month_str = '未知月份'
                                try:
                                    import subprocess
                                    result = subprocess.run(['pdftotext', '-layout', tmp_path, '-'], 
                                                           capture_output=True, text=True, timeout=10)
                                    if result.returncode == 0:
                                        # 提取开票日期中的月份，支持多种格式
                                        # 格式1: 开票申请日期: 2026-04-04 (ETC)
                                        # 格式2: 开票日期:2026年04月07日 (12306)
                                        date_match = re.search(r'开票申请日期:\s*(\d{4})-(\d{2})-(\d{2})', result.stdout)
                                        if not date_match:
                                            date_match = re.search(r'开票日期[^0-9]*(\d{4})年(\d{2})月(\d{2})日', result.stdout)
                                        if date_match:
                                            year = date_match.group(1)
                                            month = date_match.group(2)
                                            month_str = f"{year}{month}"
                                except:
                                    pass
                                finally:
                                    import os
                                    try:
                                        os.unlink(tmp_path)
                                    except:
                                        pass
                                
                                out_name = f"{plate}_{month_str}_e{email_index:02d}.pdf"
                                out_path = Path(output_dir) / out_name
                                with open(out_path, 'wb') as out_f:
                                    out_f.write(data)
                                print(f"    解压: {out_name}")
                except Exception as e:
                    print(f"    解压失败: {e}")
            
            return new_filename
        except Exception as e:
            print(f"    保存失败: {e}")
            return None
    
    counter = 0
    # 清理 sender_prefix 中的特殊字符，只保留字母数字和下划线
    import re
    sender_for_file = re.sub(r'[^a-zA-Z0-9_.-]', '_', sender_prefix)
    sender_for_file = sender_for_file.replace('.', '_')[:20]
    
    # 方法1: 遍历所有 parts
    for part in msg.walk():
        content_type = part.get_content_type()
        content_disposition = str(part.get("Content-Disposition", ""))
        
        # 检查是否有附件
        filename = part.get_filename()
        
        # 如果没有 filename，尝试从 Content-Type 获取
        if not filename:
            name = part.get_param('name', header='Content-Type')
            if name:
                filename = name
        
        # 如果还是没有，尝试从 payload 推断
        if not filename and content_type == 'application/pdf':
            filename = f'invoice_{counter}.pdf'
        
        if not filename:
            continue
        
        # 解码文件名用于 ETC 过滤
        decoded_filename = filename
        try:
            decoded_parts = email.header.decode_header(filename)
            for p in reversed(decoded_parts):
                if isinstance(p[0], bytes):
                    decoded_filename = p[0].decode(p[1] or 'utf-8', errors='replace')
                else:
                    decoded_filename = p[0]
                break
        except: pass
        
        # ETC 邮件只下载 [1].zip 文件（避免重复）
        if 'etczs' in sender_prefix.lower() and decoded_filename.lower().endswith('.zip'):
            if '[1]' not in decoded_filename:
                print(f"    ⊝ 跳过非[1] ZIP: {decoded_filename}")
                continue
        
        # 检查是否应该下载
        if is_invoice_attachment(filename, content_type):
            counter += 1
            saved = save_attachment(part, filename, counter)
            # 不管是否重复，都要添加到 downloaded（用于判断是否有附件）
            downloaded.append(saved if saved else f"已存在_{counter}")
            if saved:
                print(f"    ✓ 下载: {saved}")
    
    return downloaded

def process_email(mail, email_id, output_dir, seen_hashes=None, email_index=1):
    """处理单封邮件"""
    try:
        result, msg_data = mail.fetch(email_id, '(RFC822)')
        if result != 'OK':
            return []
        
        raw_email = msg_data[0][1]
        msg = email.message_from_bytes(raw_email)
        
        # 解码主题和发件人
        subject = decode_str(msg['Subject'] or '')
        sender = decode_str(msg['From'] or '')
        date_str = msg['Date'] or ''
        
        print(f"\n处理邮件: {subject[:50]}...")
        print(f"  发件人: {sender}")
        print(f"  日期: {date_str}")
        
        # 保存邮件正文内容到文件（用于辅助分类）
        email_body_file = Path(output_dir) / f"_email_body_{email_index:02d}.txt"
        email_body = extract_email_body(msg)
        if email_body:
            with open(email_body_file, 'w', encoding='utf-8') as f:
                f.write(f"Subject: {subject}\n")
                f.write(f"From: {sender}\n")
                f.write(f"Date: {date_str}\n")
                f.write(f"---\n")
                f.write(email_body)
        
        # 提取附件
        sender_prefix = sender.replace('@', '_at_').replace('.', '_').replace('<', '_').replace('>', '_').replace(' ', '').replace('（', '').replace('）', '')[:20]
        downloaded = extract_attachments_from_msg(msg, output_dir, sender_prefix, seen_hashes, email_index)
        
        # 从邮件正文提取链接并下载（只有当没有传统附件时才下载链接）
        # 并且过滤掉 logo、banner、tracking pixel 等无关图片
        print(f"  附件数量: {len(downloaded)}")
        if not downloaded:
            urls = extract_urls_from_body(msg)
            if urls:
                print(f"  发现 {len(urls)} 个下载链接")
                counter = 0
                for url in urls:
                    # 过滤逻辑：只下载 PDF 或者包含发票关键词的链接
                    url_lower = url.lower()
                    # PDF 文件直接下载（即使是 logo 等 URL）
                    if '.pdf' in url_lower:
                        counter += 1
                        saved = download_url(url, output_dir, sender_prefix, email_index, counter)
                        if saved:
                            downloaded.append(saved)
                        continue
                    
                    # 非 PDF 文件：跳过 logo、banner 等无关图片
                    # 只要包含 skip_patterns 中的任何一个，就跳过
                    skip_patterns = ['logo', 'banner', 'edm', 'open', 'seal', 'qr', 'code', 'phone', 'app', 'wechat', 'gif', '600x', '600000', 'tps-', '!!', 'yuandian', 'round', 'openEdm', 'ctrip', 'trip.com', 'inv', 'invoicelogo']
                    if any(p in url_lower for p in skip_patterns):
                        print(f"    ⊝ 跳过无关图片: {url[:60]}...")
                        continue
                    
                    counter += 1
                    saved = download_url(url, output_dir, sender_prefix, email_index, counter)
                    if saved:
                        downloaded.append(saved)
        
        if not downloaded:
            print(f"  (无附件)")
        
        return downloaded
        
    except Exception as e:
        print(f"  处理失败: {e}")
        return []

def search_invoice_emails(mail, month):
    """搜索发票相关邮件"""
    mail.select('INBOX')
    
    # 获取所有邮件
    result, messages = mail.search(None, 'ALL')
    if result != 'OK':
        print("搜索邮件失败")
        return []
    
    email_ids = messages[0].split()
    print(f"邮箱共 {len(email_ids)} 封邮件，开始筛选...")
    
    # 筛选发票相关邮件
    invoice_emails = []
    for email_id in email_ids:
        try:
            result, msg_data = mail.fetch(email_id, '(ENVELOPE)')
            if result != 'OK':
                continue
            
            # 检查发件人和主题（正确解码 RFC 2047 编码）
            result, msg_data = mail.fetch(email_id, '(BODY[HEADER.FIELDS (FROM SUBJECT)])')
            if result == 'OK':
                raw_header = msg_data[0][1].decode('utf-8', errors='ignore')
                # 解码主题
                subject_match = re.search(r'Subject:\s*(.+)', raw_header, re.IGNORECASE)
                if subject_match:
                    subject_raw = subject_match.group(1).strip()
                    # 解码 RFC 2047 编码
                    decoded_parts = email.header.decode_header(subject_raw)
                    decoded_subject = ''
                    for part, charset in decoded_parts:
                        if isinstance(part, bytes):
                            charset = charset or 'utf-8'
                            try:
                                decoded_subject += part.decode(charset, errors='ignore')
                            except:
                                decoded_subject += part.decode('utf-8', errors='ignore')
                        elif isinstance(part, str):
                            decoded_subject += part
                    decoded_subject = decoded_subject.lower()
                    if any(keyword in decoded_subject for keyword in INVOICE_SENDERS):
                        invoice_emails.append(email_id)
                        continue
                # 检查发件人
                from_match = re.search(r'From:\s*(.+)', raw_header, re.IGNORECASE)
                if from_match:
                    from_raw = from_match.group(1).strip().lower()
                    if any(keyword in from_raw for keyword in INVOICE_SENDERS):
                        invoice_emails.append(email_id)
        except:
            continue
    
    print(f"找到 {len(invoice_emails)} 封发票相关邮件")
    return invoice_emails

def collect_from_email(email_address, password, imap_server, month, output_dir):
    """从邮箱收集发票"""
    print(f"正在连接邮箱: {email_address}")
    
    mail = connect_imap(email_address, password, imap_server)
    if not mail:
        return []
    
    print(f"已连接 IMAP 服务器: {imap_server}")
    
    # 搜索发票邮件
    email_ids = search_invoice_emails(mail, month)
    
    if not email_ids:
        print("未找到发票相关邮件")
        mail.logout()
        return []
    
    # 创建输出目录
    output_path = Path(output_dir) / f"{month}_邮箱发票"
    output_path.mkdir(parents=True, exist_ok=True)
    
    # 扫描已有文件，建立哈希集合（跨运行去重）
    import hashlib
    seen_hashes = set()  # 跨邮件去重
    for existing_file in output_path.glob('*'):
        if existing_file.is_file():
            try:
                with open(existing_file, 'rb') as f:
                    file_hash = hashlib.md5(f.read()).hexdigest()
                seen_hashes.add(file_hash)
            except:
                pass
    print(f"已扫描 {len(seen_hashes)} 个已下载文件（将跳过重复下载）")
    
    # 处理每封邮件（全局去重）
    all_downloaded = []
    for i, email_id in enumerate(email_ids, 1):
        print(f"\n[{i}/{len(email_ids)}]")
        downloaded = process_email(mail, email_id, str(output_path), seen_hashes, i)
        all_downloaded.extend(downloaded)
    
    mail.logout()
    
    print(f"\n✅ 完成！共下载 {len(all_downloaded)} 个附件")
    return all_downloaded

def main():
    parser = argparse.ArgumentParser(description='从邮箱下载发票附件 v2')
    parser.add_argument('--email', required=True, help='邮箱地址')
    parser.add_argument('--imap', required=True, help='IMAP 服务器')
    parser.add_argument('--password', required=True, help='邮箱授权码')
    parser.add_argument('--month', required=True, help='目标月份 (YYYY-MM)')
    parser.add_argument('--output', required=True, help='输出目录')
    
    args = parser.parse_args()
    
    collect_from_email(
        args.email,
        args.password,
        args.imap,
        args.month,
        args.output
    )

if __name__ == '__main__':
    main()
