#!/usr/bin/env python3
"""
报销状态更新脚本
更新报销的审批状态
"""

import json
import argparse
from datetime import datetime
from pathlib import Path

DATA_FILE = Path(__file__).parent.parent.parent / "memory" / "reimbursements.json"

STATUS_OPTIONS = ['pending', 'approved', 'rejected', 'paid', 'closed']

def load_data():
    if DATA_FILE.exists():
        with open(DATA_FILE, 'r', encoding='utf-8') as f:
            return json.load(f)
    return {"reimbursements": []}

def save_data(data):
    with open(DATA_FILE, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

def update_status(reimb_id, new_status, note=''):
    data = load_data()
    
    for r in data['reimbursements']:
        if r['id'] == reimb_id:
            old_status = r['status']
            r['status'] = new_status
            r['history'].append({
                'date': datetime.now().strftime('%Y-%m-%d %H:%M'),
                'action': 'status_change',
                'from': old_status,
                'to': new_status,
                'note': note
            })
            
            if new_status == 'paid':
                r['paid_date'] = datetime.now().strftime('%Y-%m-%d')
            
            save_data(data)
            
            print(f"✓ {reimb_id}: {old_status} → {new_status}")
            if note:
                print(f"  备注: {note}")
            return True
    
    print(f"错误: 未找到报销单 {reimb_id}")
    return False

def main():
    parser = argparse.ArgumentParser(description='更新报销状态')
    parser.add_argument('--id', '-i', required=True, help='报销单号 (如 REIMB-20260406-001)')
    parser.add_argument('--status', '-s', required=True, choices=STATUS_OPTIONS, help='新状态')
    parser.add_argument('--note', '-n', default='', help='备注说明')
    
    args = parser.parse_args()
    update_status(args.id, args.status, args.note)

if __name__ == '__main__':
    main()
