#!/usr/bin/env python3
"""
报销提交脚本
提交新的报销记录
"""

import sys
import json
import argparse
from datetime import datetime, timedelta
from pathlib import Path

DATA_FILE = Path(__file__).parent.parent.parent / "memory" / "reimbursements.json"

def load_data():
    """加载报销数据"""
    if DATA_FILE.exists():
        with open(DATA_FILE, 'r', encoding='utf-8') as f:
            return json.load(f)
    return {"reimbursements": []}

def save_data(data):
    """保存报销数据"""
    DATA_FILE.parent.mkdir(parents=True, exist_ok=True)
    with open(DATA_FILE, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

def generate_id(data):
    """生成报销单号"""
    today = datetime.now().strftime('%Y%m%d')
    count = sum(1 for r in data['reimbursements'] if r['id'].startswith(f'REIMB-{today}'))
    return f'REIMB-{today}-{str(count + 1).zfill(3)}'

def submit(amount, expense_type, description='', expected_days=7):
    """提交新报销"""
    data = load_data()
    
    record = {
        'id': generate_id(data),
        'submit_date': datetime.now().strftime('%Y-%m-%d'),
        'amount': amount,
        'type': expense_type,
        'description': description,
        'status': 'pending',
        'expected_days': expected_days,
        'history': [
            {
                'date': datetime.now().strftime('%Y-%m-%d %H:%M'),
                'action': 'submit',
                'status': 'pending'
            }
        ]
    }
    
    data['reimbursements'].append(record)
    save_data(data)
    
    print(f"报销已提交: {record['id']}")
    print(f"金额: ¥{amount:.2f}")
    print(f"类型: {expense_type}")
    print(f"状态: {record['status']}")
    print(f"预计审批: {expected_days} 天内")
    
    return record['id']

def main():
    parser = argparse.ArgumentParser(description='提交报销')
    parser.add_argument('--amount', '-a', type=float, required=True, help='报销金额')
    parser.add_argument('--type', '-t', required=True, help='报销类型（差旅费/交通费/招待费/其他）')
    parser.add_argument('--description', '-d', default='', help='报销说明')
    parser.add_argument('--expected-days', '-e', type=int, default=7, help='预期审批天数')
    
    args = parser.parse_args()
    submit(args.amount, args.type, args.description, args.expected_days)

if __name__ == '__main__':
    main()
