#!/usr/bin/env python3
"""
Generate TII transmitter lookup table from a CSV file and inject it into index.js.

Usage: python3 scripts/generate-tii-db.py [path/to/tii-database.csv]

Expected CSV format (semicolon-separated):
  id;country;block;ensemblelabel;eid;tii;location;...

The script injects the lookup table into src/welle-cli/index.js (var tii_db).
Do NOT commit index.js after running this script if the CSV data is under
copyright (e.g. FMLIST data requires prior written authorization for public use).
"""

import csv
import json
import re
import sys
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
INDEX_JS = os.path.join(PROJECT_ROOT, 'src', 'welle-cli', 'index.js')
DEFAULT_CSV = os.path.join(PROJECT_ROOT, 'dab-tx-list.csv')

csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV

if not os.path.exists(csv_path):
    print(f"Error: CSV file not found: {csv_path}")
    sys.exit(1)

db = {}
with open(csv_path, encoding='latin-1') as f:
    reader = csv.DictReader(f, delimiter=';')
    for row in reader:
        eid = row.get('eid', '').strip()
        tii = row.get('tii', '').strip()
        location = row.get('location', '').strip()
        if eid and tii and location:
            key = eid.upper() + '_' + tii.zfill(4)
            db[key] = location.encode('latin-1').decode('utf-8', errors='replace')

js_dict = json.dumps(db, ensure_ascii=False, separators=(',', ':'))

with open(INDEX_JS, 'r', encoding='utf-8') as f:
    content = f.read()

new_content = re.sub(
    r'var tii_db = \{.*?\};',
    f'var tii_db = {js_dict};',
    content,
    flags=re.DOTALL
)

if new_content == content:
    print("Error: could not find 'var tii_db' in index.js")
    sys.exit(1)

with open(INDEX_JS, 'w', encoding='utf-8') as f:
    f.write(new_content)

print(f"OK — {len(db)} entries injected into src/welle-cli/index.js")
print("Now recompile: cd build && make -j$(nproc) welle-cli && sudo make install")
print("WARNING: do NOT commit index.js if the CSV data is under copyright.")
