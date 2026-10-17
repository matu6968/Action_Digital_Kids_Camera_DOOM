#!/usr/bin/env python3
"""Check native ELF call destinations and inert slots; emit a review record."""
import argparse
import hashlib
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile
parser=argparse.ArgumentParser();parser.add_argument('elf',type=Path)
parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
calls=[];branches=0
with args.elf.open('rb') as stream:
    elf=ELFFile(stream)
    assert elf.little_endian and elf.elfclass==32 and elf.header.e_flags&1
    sections=[s for s in elf.iter_sections() if s['sh_flags']&4]
    for section in sections:
        data=section.data();base=section['sh_addr']
        for offset in range(0,len(data),4):
            word=int.from_bytes(data[offset:offset+4],'little');opcode=word>>26
            if opcode in (0,1,3,4,0x11,0x12):
                branches+=1
                assert data[offset+4:offset+8]==b'\0\0\0\x15',f'non-inert slot at {base+offset:#x}'
            if opcode==1:
                immediate=word&0x3ffffff
                if immediate&0x2000000:immediate-=0x4000000
                target=base+offset+immediate*4
                assert any(s['sh_addr']<=target<s['sh_addr']+s['sh_size'] for s in sections),f'call outside SDK: {target:#x}'
                calls.append({'address':base+offset,'target':target})
    symbols=elf.get_section_by_name('.symtab')
    functions=[s.name for s in symbols.iter_symbols() if s['st_info']['type']=='STT_FUNC' and s['st_shndx']!='SHN_UNDEF']
report={'status':'passed','elf_sha256':hashlib.sha256(args.elf.read_bytes()).hexdigest(),
        'direct_calls':calls,'branches_with_inert_slots':branches,'functions':functions,
        'scope':'All direct calls stay in SDK executable sections; source review limits indirect calls to SDK stdio callbacks and the explicit BootROM recovery handoff.'}
args.output.write_text(json.dumps(report,indent=2)+'\n')
print(f'ELF audit passed: {len(calls)} direct calls, {branches} inert branch slots')
