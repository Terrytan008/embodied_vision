#!/usr/bin/env python3
"""
报销列表脚本
查看所有报销记录
"""

import json
import argparse
from pathlib import Path

DATA_FILE = Path(__file__).parent.parent.parent / "memory" / "reimbursements.json"

STATUS_CN = {
    'pending': '等待审批',
    'approved': '已通过',
    'rejected': '已退回',
    'paid': '已打款',
    'closed': '已完成'
}

def load_data():
    if DATA_FILE.exists():
        with open(DATA_FILE, 'r', encoding='utf-8') as f:
            return json.load(f)
    return {"reimbursements": []}

def list_reimbursements(status_filter=None, verbose=False):
    data = load_data()
    records = data.get('reimbursements', [])
    
    if status_filter:
        records = [r for r in records if r['status'] == status_filter]
    
    if not records:
        print("暂无报销记录")
        return
    
    print(f"{'单号':<20} {'日期':<12} {'类型':<8} {'金额':<12} {'状态':<10}")
    print("-" * 65)
    
    for r in records:
        status_cn = STATUS_CN.get(r['status'], r['status'])
        print(f"{r['id']:<20} {r['submit_date']:<12} {r['type']:<8} ¥{r['amount']:<11.2f} {status_cn:<10}")
    
    print(f"\n共 {len(records)} 条记录")

def main():
    parser = argparse.ArgumentParser(description='查看报销列表')
    parser.add_argument('--status', '-s', choices=['pending', 'approved', 'rejected', 'paid', 'closed'], help='按状态筛选')
    parser.add_argument('--verbose', '-v', action='store_true', help='显示详细信息')
    
    args = parser.parse_args()
    list_reimbursements(args.status, args.verbose)

if __name__ == '__main__':
    main()
