---
name: receipt-ocr
description: 发票OCR识别。解析发票图片/PDF，提取关键信息：发票号、日期、金额、购买方、销售方、商品明细。支持增值税发票、普通发票、电子发票。
metadata: { "openclaw": { "emoji": "📸", "requires": { "bins": ["python", "tesseract"] } } }
---

# 发票 OCR 识别

从发票图片、截图或 PDF 中提取结构化数据。

## 支持的发票类型

| 类型 | 示例 |
|------|------|
| 增值税专用发票 | 票面有"专用发票"字样 |
| 增值税普通发票 | 票面有"普通发票"字样 |
| 电子发票 | PDF 或截图格式 |
| 出租车票 | 行程+金额 |
| 火车票 | 出发/到达+金额 |
| 机票行程单 | 航班+金额 |

## 核心字段

从每张发票中提取：

```
- invoice_type: 发票类型
- invoice_number: 发票号码
- invoice_code: 发票代码
- date: 开票日期
- amount: 价税合计金额
- tax_amount: 税额
- price_ex_tax: 不含税金额
- buyer_name: 购买方名称
- buyer_tax_id: 购买方纳税人识别号
- seller_name: 销售方名称
- seller_tax_id: 销售方纳税人识别号
- items: 商品明细列表
```

## 使用方式

### Python 脚本

```bash
python3 skills/receipt-ocr/parse_receipt.py <发票图片路径>
```

### 调用示例

```bash
python3 skills/receipt-ocr/parse_receipt.py ./receipts/invoice_01.jpg
```

## 输出格式

JSON 格式输出，例如：

```json
{
  "success": true,
  "invoice_type": "增值税普通发票",
  "invoice_number": "1234567890",
  "date": "2026-04-06",
  "amount": 1580.00,
  "buyer_name": "XXX有限公司",
  "seller_name": "XXX供应商",
  "items": [
    {"name": "服务费", "quantity": 1, "unit_price": 1580.00}
  ]
}
```

## 依赖

- `pytesseract` — OCR 引擎
- `Pillow` — 图片处理
- `tesseract` — 系统 OCR 程序（需单独安装）

### 安装依赖

```bash
brew install tesseract
pip3 install pytesseract Pillow
```
