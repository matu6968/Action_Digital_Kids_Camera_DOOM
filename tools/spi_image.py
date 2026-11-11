#!/usr/bin/env python3
"""Build the camera diagnostic BLDR SPI image from a verified original backup."""
import argparse,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'python'))
from apxtool.spi_boot import package
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--original',type=Path,required=True)
p.add_argument('--manifest',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--board',choices=('action','silvergear'),default='action')
a=p.parse_args()
print(json.dumps(package(a.original,a.manifest,a.output,a.board),indent=2))
