---
name: form-filler
description: 报销单填写。根据发票OCR数据和用户输入，自动填充报销单模板（Excel/Word格式）。
metadata: { "openclaw": { "emoji": "📊", "requires": { "bins": ["python"] } } }
---

# 报销单填写

根据解析出的发票数据和用户信息，自动填充报销单。

## 工作流程

1. 收集发票数据（来自 receipt-ocr）
2. 读取报销单模板
3. 按模板格式填入数据
4. 生成可提交的报销单文件

## 报销单模板格式

支持两种格式：

### Excel 模板（.xlsx）
使用 openpyxl 库填写：

| 字段 | 位置 |
|------|------|
| 申请人 | A1 |
| 部门 | B1 |
| 报销日期 | C1 |
| 摘要 | D1 |
| ... | ... |

### Word 模板（.docx）
使用 python-docx 库填写占位符：
`{{申请人}}`, `{{金额}}`, `{{日期}}` 等

## 使用方式

```bash
python3 skills/form-filler/fill_form.py \
  --data ./invoice_data.json \
  --template ./template.xlsx \
  --output ./filled_form.xlsx
```

## 输入数据格式

```json
{
  "applicant": "张三",
  "department": "技术部",
  "reimbursement_date": "2026-04-06",
  "expenses": [
    {
      "date": "2026-04-01",
      "type": "差旅费",
      "description": "北京出差",
      "amount": 1500.00,
      "invoice_number": "1234567890"
    }
  ],
  "total_amount": 1500.00
}
```

## 输出

填充好的报销单文件（Excel 或 Word），可直接提交或打印。

## 依赖

```bash
pip3 install openpyxl python-docx
```
