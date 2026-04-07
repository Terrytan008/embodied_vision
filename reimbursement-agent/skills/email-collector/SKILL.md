---
name: email-collector
description: 从邮箱下载发票附件（IMAP协议）。从邮件中提取发票附件，保存到本地目录。
metadata: { "openclaw": { "emoji": "📧", "requires": { "bins": ["python"] } } }
---

# 邮箱发票收集

从邮箱 IMAP 服务器下载发票附件。

## 模块结构

```
email-collector/
├── collect.py       # Orchestrator（IMAP连接、邮件处理、URL下载）
├── utils.py         # 工具函数（RFC2047解码、附件判断、URL过滤）
└── zip_handler.py  # ZIP解压（ETC通行费、12306火车票自动分类）
```

## 执行命令

```bash
python3 /Users/terrytan/openclaw/workspace/reimbursement-agent/skills/email-collector/collect.py \
  --email terrytan007@qq.com \
  --imap imap.qq.com \
  --password <授权码> \
  --month 2026-03 \
  --output /Users/terrytan/报销发票/downloaded
```

## 支持的附件类型

- `.pdf` — 电子发票
- `.jpg` / `.jpeg` — 发票照片
- `.png` — 截图
- `.zip — ETC/12306 发票包（自动解压）

## 识别逻辑

**发件人关键词：**
`didifapiao`, `滴滴`, `didi`, `itinerary`, `高德`, `amap`, `trip`, `携程`, `ctrip`, `overseas_rsv`, `etc`, `ETC`, `发票`, `报销`, `invoice`, `fapiao`

**附件关键词：**
`发票`, `fapiao`, `invoice`, `receipt`, `行程`, `itinerary`, `水单`, `入住`, `check`, `酒店`, `hotel`, `etc`

## 特殊处理

### ETC 通行费 ZIP
- 只下载 `[1].zip` 文件（避免重复）
- 自动从 PDF 内容提取开票月份
- 匹配中文车牌号（如粤BGF4860）

### 12306 火车票 ZIP
- 自动识别纯数字ID文件名
- 从 PDF 提取开票月份

## 输出

下载文件保存到：`{output}/{月份}_邮箱发票/`

文件命名格式：`{原文件名}_{邮件序号}.{扩展名}`
