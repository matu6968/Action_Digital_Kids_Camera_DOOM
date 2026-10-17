import argparse
import json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('path',type=Path);p.add_argument('--opt');p.add_argument('--heap');p.add_argument('--lto',default='0');p.add_argument('--id')
a=p.parse_args();content=json.dumps({'opt':a.opt,'heap':a.heap,'lto':a.lto,'build_id':a.id},sort_keys=True)+'\n'
if not a.path.exists() or a.path.read_text()!=content:a.path.write_text(content)
