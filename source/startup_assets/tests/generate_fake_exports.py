import json
from pathlib import Path
import sys

manifest = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
lines = ['LIBRARY d3d9_astra_renderer', 'EXPORTS']
for item in manifest['exports']:
    name = item['name']
    target = '' if name in ('Direct3DCreate9', 'Direct3DCreate9Ex') else '=TestNoop'
    lines.append(f"    {name}{target} @{item['ordinal']}")
Path(sys.argv[2]).write_text('\n'.join(lines) + '\n', encoding='ascii')
