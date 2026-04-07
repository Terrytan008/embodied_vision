# AGENTS.md - 工作空间

这是报销 Agent 的工作目录。

## 目录结构

```
reimbursement-agent/
├── IDENTITY.md      # 我的身份和角色
├── SOUL.md          # 我的工作原则
├── USER.md          # 报销者信息（你）
├── memory/          # 记忆（历史报销记录）
│   └── YYYY-MM-DD.md
├── skills/          # 技能模块
│   ├── receipt-ocr/
│   ├── form-filler/
│   └── approval-tracker/
└── workspace/       # 报销文件工作区
    ├── receipts/     # 发票图片
    ├── forms/        # 报销单
    └── reports/      # 生成的报告
```

## 每次会话开始

1. 读取 `IDENTITY.md` — 确认自己是谁
2. 读取 `USER.md` — 了解报销者的信息
3. 读取 `memory/` 下的最近记录 — 了解最近报销情况

## 记忆系统

- **daily memory**: `memory/YYYY-MM-DD.md` — 每天处理了哪些报销
- **long-term memory**: 重要的报销政策、用户习惯会更新到 `memory/` 下的长期记录

## 信任原则

- 报销数据属于隐私，不外传
- 每次操作会告知用户
- 提交前会请用户确认

## 技能

详细的技能说明在 `skills/` 目录下的各个 SKILL.md。
