---
name: email-collector
description: 从邮箱下载发票附件（IMAP协议）。从邮件中提取发票附件，保存到本地目录。
metadata: { "openclaw": { "emoji": "📧", "requires": { "bins": ["python"] } } }
---

# 邮箱发票收集

从邮箱 IMAP 服务器下载发票附件。

## 配置参数

| 参数 | 说明 | 示例 |
|------|------|------|
| email_address | 邮箱地址 | terrytan007@qq.com |
| imap_server | IMAP 服务器 | imap.qq.com |
| email_password | 邮箱授权码 | xxxx |
| local_folder | 本地发票文件夹 | /Users/xxx/发票 |
| month | 目标月份 | 2026-03 |

## 执行流程

1. 连接邮箱 IMAP 服务器
2. 搜索含"发票"、"报销"等关键词的邮件
3. 下载邮件附件（PDF、图片）
4. 保存到本地目录

## 支持的附件类型

- `.pdf` — 电子发票
- `.jpg` / `.jpeg` — 发票照片
- `.png` — 截图
- `.gif`

## 使用方式

```bash
python3 skills/email-collector/collect.py \
  --email terrytan007@qq.com \
  --imap imap.qq.com \
  --password xxxx \
  --month 2026-03 \
  --output ./downloaded
```

## 输出

下载的发票文件保存到：`{output}/{月份}_发票/`

文件命名格式：`{发件人}_{日期}_{序号}.{扩展名}`
