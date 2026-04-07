#!/usr/bin/env python3
"""
邮箱发票收集脚本 v3
邮箱连接、邮件处理、附件下载 orchestrator
依赖: utils.py, zip_handler.py
"""
import sys
import os
import argparse
import ssl
import hashlib
import urllib.request
from pathlib import Path

import imaplib
import email
import email.header

from utils import (
    INVOICE_SENDERS, decode_str, is_invoice_attachment,
    extract_urls_from_body, get_email_body, is_relevant_url
)
from zip_handler import extract_from_zip


# ── IMAP 连接 ────────────────────────────────────────────────

def connect_imap(email_address, password, imap_server):
    """连接 IMAP 服务器"""
    try:
        mail = imaplib.IMAP4_SSL(imap_server)
        mail.login(email_address, password)
        return mail
    except Exception as e:
        print(f"连接失败: {e}")
        return None


def search_invoice_emails(mail):
    """搜索发票相关邮件"""
    mail.select('INBOX')
    result, messages = mail.search(None, 'ALL')
    if result != 'OK':
        print("搜索邮件失败")
        return []

    email_ids = messages[0].split()
    print(f"邮箱共 {len(email_ids)} 封邮件，开始筛选...")

    invoice_emails = []
    for eid in email_ids:
        try:
            _, msg_data = mail.fetch(eid, '(BODY[HEADER.FIELDS (FROM SUBJECT)])')
            if result != 'OK':
                continue
            raw_header = msg_data[0][1].decode('utf-8', errors='ignore')

            # 检查主题
            m = re.search(r'Subject:\s*(.+)', raw_header, re.IGNORECASE)
            if m:
                decoded = decode_str(m.group(1).strip()).lower()
                if any(kw in decoded for kw in INVOICE_SENDERS):
                    invoice_emails.append(eid)
                    continue

            # 检查发件人
            m = re.search(r'From:\s*(.+)', raw_header, re.IGNORECASE)
            if m:
                decoded = decode_str(m.group(1).strip()).lower()
                if any(kw in decoded for kw in INVOICE_SENDERS):
                    invoice_emails.append(eid)
        except Exception:
            continue

    print(f"找到 {len(invoice_emails)} 封发票相关邮件")
    return invoice_emails


# ── 附件保存 ────────────────────────────────────────────────

def save_attachment(part, filename, output_dir, email_index, seen_hashes):
    """保存单个附件，返回 (saved_filename or None, is_zip)"""
    try:
        payload = part.get_payload(decode=True)
        if not payload:
            return None, False

        # MD5 去重
        file_hash = hashlib.md5(payload).hexdigest()
        if file_hash in seen_hashes:
            print(f"    ⊝ 内容完全相同（已存在）")
            return None, False
        seen_hashes.add(file_hash)

        # 解码并清理文件名
        filename = decode_str(filename)
        safe = ' '.join(filename.split())
        safe = safe.replace('/', '_').replace('\\', '_').replace('<', '_').replace('>', '_').strip()

        ext = Path(safe).suffix.lower() or ('.pdf' if 'pdf' in part.get_content_type() else '')
        stem = Path(safe).stem[:200]
        saved_name = f"{stem}_{email_index:02d}{ext}"
        save_path = Path(output_dir) / saved_name

        with open(save_path, 'wb') as f:
            f.write(payload)

        is_zip = ext == '.zip'
        return saved_name, is_zip

    except Exception as e:
        print(f"    保存失败: {e}")
        return None, False


def process_email(mail, email_id, output_dir, seen_hashes, email_index):
    """处理单封邮件，返回下载的文件名列表"""
    try:
        _, msg_data = mail.fetch(email_id, '(RFC822)')
        if _ != 'OK':
            return []

        raw_email = msg_data[0][1]
        msg = email.message_from_bytes(raw_email)

        subject = decode_str(msg['Subject'] or '')
        sender = decode_str(msg['From'] or '')
        date_str = msg['Date'] or ''

        print(f"\n[{email_index}] 处理邮件: {subject[:50]}...")
        print(f"    发件人: {sender}")
        print(f"    日期: {date_str}")

        # 保存邮件正文
        body = get_email_body(msg)
        if body:
            body_file = Path(output_dir) / f"_email_body_{email_index:02d}.txt"
            with open(body_file, 'w', encoding='utf-8') as f:
                f.write(f"Subject: {subject}\nFrom: {sender}\nDate: {date_str}\n---\n{body}")

        downloaded = []
        sender_prefix = re.sub(r'[^a-zA-Z0-9_.-]', '_', sender)[:20]

        # ── 遍历所有 parts ──
        for part in msg.walk():
            filename = part.get_filename()
            if not filename:
                name = part.get_param('name', header='Content-Type')
                if name:
                    filename = name
            if not filename and part.get_content_type() == 'application/pdf':
                filename = f'invoice_{email_index}.pdf'
            if not filename:
                continue

            # ETC 只下载 [1].zip
            if 'etczs' in sender_prefix.lower() and filename.lower().endswith('.zip'):
                decoded_fn = decode_str(filename)
                if '[1]' not in decoded_fn:
                    print(f"    ⊝ 跳过非[1] ZIP: {decoded_fn}")
                    continue

            if not is_invoice_attachment(filename, part.get_content_type()):
                continue

            saved_name, is_zip = save_attachment(
                part, filename, output_dir, email_index, seen_hashes
            )
            if saved_name:
                print(f"    ✓ 下载: {saved_name}")
                downloaded.append(saved_name)

                # 自动解压 ZIP
                if is_zip:
                    zip_path = Path(output_dir) / saved_name
                    extracted = extract_from_zip(zip_path, output_dir, email_index)
                    downloaded.extend(extracted)

        # ── 无传统附件时：从正文 URL 下载 ──
        if not downloaded:
            urls = extract_urls_from_body(msg)
            relevant = [u for u in urls if is_relevant_url(u)]
            if relevant:
                print(f"    发现 {len(relevant)} 个相关下载链接")
                for counter, url in enumerate(relevant, 1):
                    saved = download_url(url, output_dir, email_index, counter, seen_hashes)
                    if saved:
                        downloaded.append(saved)

        if not downloaded:
            print(f"    (无附件)")

        return downloaded

    except Exception as e:
        print(f"    处理失败: {e}")
        return []


# ── URL 下载 ────────────────────────────────────────────────

def download_url(url, output_dir, email_index, counter, seen_hashes):
    """下载 URL 对应的文件"""
    try:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

        req = urllib.request.Request(
            url,
            headers={
                'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36',
                'Accept': '*/*'
            }
        )
        with urllib.request.urlopen(req, context=ctx, timeout=30) as resp:
            content = resp.read()
            ct = resp.headers.get('Content-Type', '').lower()

            # 扩展名
            url_lower = url.lower()
            if '.pdf' in url_lower:
                ext = '.pdf'
            elif '.jpg' in url_lower or 'jpeg' in url_lower:
                ext = '.jpg'
            elif '.png' in url_lower:
                ext = '.png'
            elif '.gif' in url_lower:
                ext = '.gif'
            elif '.ofd' in url_lower:
                ext = '.ofd'
            elif 'image' in ct:
                ext = '.jpg'
            else:
                ext = '.pdf'

            # 文件名
            url_fn = url.split('/')[-1].split('?')[0] or f"download_{counter}"
            if len(url_fn) < 3:
                url_fn = f"download_{counter}{ext}"

            safe_stem = url_fn[:150]
            filename = f"{safe_stem}_e{email_index:02d}{ext}"
            save_path = Path(output_dir) / filename

            with open(save_path, 'wb') as f:
                f.write(content)

            print(f"    ✓ 下载链接: {filename}")
            return filename

    except Exception as e:
        print(f"    ✗ 下载失败 [{url[:50]}...]: {e}")
        return None


# ── 主流程 ────────────────────────────────────────────────

def collect_from_email(email_address, password, imap_server, month, output_dir):
    print(f"正在连接邮箱: {email_address}")
    mail = connect_imap(email_address, password, imap_server)
    if not mail:
        return []
    print(f"已连接 IMAP 服务器: {imap_server}")

    # 搜索
    email_ids = search_invoice_emails(mail)
    if not email_ids:
        print("未找到发票相关邮件")
        mail.logout()
        return []

    # 输出目录
    output_path = Path(output_dir) / f"{month}_邮箱发票"
    output_path.mkdir(parents=True, exist_ok=True)

    # 跨运行 MD5 去重
    seen_hashes = set()
    for f in output_path.glob('*'):
        if f.is_file():
            try:
                with open(f, 'rb') as fp:
                    seen_hashes.add(hashlib.md5(fp.read()).hexdigest())
            except Exception:
                pass
    print(f"已扫描 {len(seen_hashes)} 个已下载文件（将跳过重复下载）")

    # 处理每封邮件
    all_downloaded = []
    for i, eid in enumerate(email_ids, 1):
        downloaded = process_email(mail, eid, str(output_path), seen_hashes, i)
        all_downloaded.extend(downloaded)

    mail.logout()
    print(f"\n✅ 完成！共下载 {len(all_downloaded)} 个附件")
    return all_downloaded


# ── CLI ──────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description='从邮箱下载发票附件 v3')
    parser.add_argument('--email', required=True)
    parser.add_argument('--imap', required=True)
    parser.add_argument('--password', required=True)
    parser.add_argument('--month', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    collect_from_email(args.email, args.password, args.imap, args.month, args.output)


if __name__ == '__main__':
    main()
