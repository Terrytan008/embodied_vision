---
name: invoice-classifier
description: 发票智能分类与汇总表生成。按规则自动分类发票文件，生成Excel汇总表。
metadata: { "openclaw": { "emoji": "📁", "requires": { "bins": ["python"] } } }
---

# 发票分类与汇总

按类别自动分类发票文件，生成 Excel 汇总表。

## 分类规则

| 类别 | 关键词 |
|------|--------|
| 交通出行 | 火车票、机票、滴滴、打车、地铁、公交、高速、ETC、汽油费、充电费 |
| 餐饮美食 | 餐饮、饭店、餐厅、午餐、晚餐、奶茶、咖啡 |
| 办公用品 | 办公、文具、打印、耗材、电脑 |
| 酒店住宿 | 酒店、住宿、宾馆 |
| 其他 | 未匹配上述类别的发票 |

## 执行命令（重要）

⚠️ **必须使用绝对路径**，禁止用 `cd + 相对路径` 或 `\` 换行，否则会被 exec preflight 拦截。

⚠️ **创建/修改 Python 脚本**：必须用 `write` 工具写文件，**禁止用 `cat << 'EOF' > file.py` 或任何 heredoc**，会被拦截。

**方式A（推荐）：直接调用 classify.py**
```
python3 /Users/terrytan/openclaw/workspace/reimbursement-agent/skills/invoice-classifier/classify.py --source /Users/terrytan/报销发票/downloaded --output /Users/terrytan/报销发票 --month 2026-03 --email-dir /Users/terrytan/报销发票/downloaded/2026-03_邮箱发票
```

**方式B：一键脚本（推荐用于整月汇总）**
```
bash /Users/terrytan/openclaw/workspace/reimbursement-agent/run_reimbursement.sh 2026-03
```

**禁止这样写（会被拦截）：**
```
cd ~/openclaw/workspace/reimbursement-agent
python3 skills/invoice-classifier/classify.py \
  --source ...
```

## 执行流程

1. 扫描源目录所有发票文件
2. 根据文件名/路径关键词分类
3. 复制文件到对应分类目录
4. 生成 Excel 汇总表

## 输出

```
{月份}_报销发票/
├── 交通出行/
├── 餐饮美食/
├── 办公用品/
├── 酒店住宿/
├── 其他/
└── 发票汇总表.xlsx
```

## Excel 汇总表格式

| 序号 | 文件名 | 类别 | 日期 | 金额(元) | 来源 |
|------|--------|------|------|----------|------|
| 1 | xxx.pdf | 交通出行 | 2026-03-05 | 158.00 | 邮箱 |
| 2 | xxx.jpg | 餐饮美食 | 2026-03-08 | 86.50 | 本地 |
