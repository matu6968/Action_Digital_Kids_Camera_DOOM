#!/usr/bin/env python3
"""Check the SDK's fixed PLL pair against the accepted loader instructions."""
import argparse,hashlib,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('elf',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
# Accepted loader image SHA 9b02950c...; both critical blocks are identical.
critical=bytes.fromhex('5c2820c2583020c2f401809cffff849c000024bcfeffff1300000015')
report={'status':'passed','elf_sha256':hashlib.sha256(a.elf.read_bytes()).hexdigest(),'blocks':{}}
with a.elf.open('rb') as f:
 elf=ELFFile(f);symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
 for direction,coefficient,control in [('up',0x112a8800,0x1123),('down',0x0b71b000,0x1623)]:
  name='apx_clock_'+direction+'_critical';symbol=symbols[name];address=symbol['st_value'];section=elf.get_section(symbol['st_shndx'])
  offset=address-section['sh_addr'];assert address%32==0
  assert section.data()[offset:offset+28]==critical,(name,'changed critical instructions')
  # Alignment padding can separate the three immediate loads from the pair.
  prefix=section.data()[:offset];loads=struct.pack('<3I',0x18a00000|(coefficient>>16),0xa8a50000|(coefficient&65535),0x9cc00000|control)
  start=prefix.rfind(loads);assert start>=0 and offset-start<=44,(name,'changed constants')
  assert all(word>>26==5 for word in struct.unpack('<'+'I'*((offset-start-12)//4),prefix[start+12:])),name
  report['blocks'][direction]={'address':address,'bytes':28,'block_alignment':32,'coefficient':coefficient,'control':control,'sha256':hashlib.sha256(critical).hexdigest()}
a.output.write_text(json.dumps(report,indent=2)+'\n');print('Clock ELF: accepted aligned 28-byte transitions and fixed 96/144 MHz constants verified')
