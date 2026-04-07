---
name: approval-tracker
description: 报销审批跟踪。记录报销提交历史，跟踪审批状态，逾期提醒。
metadata: { "openclaw": { "emoji": "📋", "requires": { "bins": ["python"] } } }
---

# 报销审批跟踪

记录每一笔报销的审批进度，逾期时主动提醒。

## 核心功能

### 1. 报销记录

记录每次报销提交：

```json
{
  "id": "REIMB-20260406-001",
  "submit_date": "2026-04-06",
  "amount": 1580.00,
  "type": "差旅费",
  "status": "pending",
  "expected_days": 7,
  "history": [
    {"date": "2026-04-06", "action": "submit", "status": "pending"}
  ]
}
```

### 2. 状态跟踪

| 状态 | 说明 |
|------|------|
| pending | 已提交，等待审批 |
| approved | 已审批通过 |
| rejected | 被退回 |
| paid | 已打款 |
| closed | 已完成 |

### 3. 逾期提醒

检查所有 pending 状态的报销，超过预期天数未审批的，主动提醒。

## 状态更新

当用户告知审批状态变化时，更新记录：

```
状态: pending → approved
时间: 2026-04-08
操作人: XXX
```

## 使用方式

```bash
# 提交新报销
python3 skills/approval-tracker/submit.py --amount 1580 --type 差旅费

# 查看所有报销状态
python3 skills/approval-tracker/list.py

# 更新状态
python3 skills/approval-tracker/update.py --id REIMB-20260406-001 --status approved

# 检查逾期
python3 skills/approval-tracker/check_overdue.py
```

## 数据存储

- 报销记录存储在 `memory/reimbursements.json`
- 每个报销有唯一 ID：`REIMB-YYYYMMDD-NNN`
- 历史变更记录在 `history` 字段中
