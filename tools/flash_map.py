#!/usr/bin/env python3
"""Print the flash map of an SPI-boot image or of a chip dump (research/spi-flash-map.md explains the layout).

Works on any 2 MiB file: a packed image from tools/spi_image.py, or `apxtool debug flash read` output. Needs nothing
but the BootROM header (load sector count at +0x28) and, if present, the overlay table at 0x1fc000.
"""
import argparse,struct,sys,zlib
from pathlib import Path
CAPACITY=0x200000;IMAGE_START=0x2200;TABLE=0x1fc000;MAGIC=0x4f585041
def analyse(data):
    if len(data)!=CAPACITY:raise ValueError('expected exactly 2 MiB')
    if data[4:8]!=b'BLDR':raise ValueError('no BLDR header: not a BootROM-bootable image')
    sectors,=struct.unpack_from('<I',data,0x28)
    load=(IMAGE_START,IMAGE_START+sectors*512,'BootROM load block (main image)')
    overlays=[]
    magic,version,count,crc=struct.unpack_from('<IIII',data,TABLE)
    if magic==MAGIC and version==1 and 0<count<=15:
        body=data[TABLE+16:TABLE+16+16*count]
        if zlib.crc32(body)==crc:
            for i in range(count):
                offset,address,length,c=struct.unpack_from('<IIII',body,16*i)
                overlays.append((offset,offset+length,f'overlay -> RAM {address:#010x} ({length/1024:.0f} KiB, crc32 {c:#010x})'))
    end=max([load[1]]+[o[1] for o in overlays]);end=(end+4095)&~4095
    return load,overlays,end
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('image',type=Path);a=p.parse_args()
    data=a.image.read_bytes()
    try:load,overlays,end=analyse(data)
    except ValueError as e:sys.exit(f'{a.image}: {e}')
    rows=[(0,IMAGE_START,'BootROM header + SRAM bootstrap (never rewrite; sector 0 is committed last on install)'),load,*overlays,
          (end,TABLE,'FREE for application data'),
          (TABLE,TABLE+0x1000,'overlay table (erased if the image has no overlay)'),
          (0x1fd000,0x1fe000,'unused'),
          (0x1fe000,0x1ff000,'trace log of USB-launched runs (APX_SPI_TRACE builds only)'),
          (0x1ff000,0x200000,'trace log of SPI boots (APX_SPI_TRACE builds only)')]
    for lo,hi,label in rows:print(f'{lo:#08x}-{hi:#08x} {(hi-lo)/1024:8.1f} KiB  {label}')
    free=data[end:TABLE]
    print(f'free area {end:#x}..{TABLE:#x}: {(TABLE-end)/1024:.0f} KiB, of which erased (0xff): {free.count(0xff)/1024:.0f} KiB; the rest is old vendor data - erase before use')
    gap=0;blk=data[IMAGE_START:load[1]];run=0
    for i in range(0,len(blk),4096):
        if not any(blk[i:i+4096]):run+=4096
        else:
            gap+=run if run>=8192 else 0;run=0
    gap+=run if run>=8192 else 0
    print(f'all-zero stretches (>= 8 KiB) inside the load block: {gap/1024:.0f} KiB'+('  <- wasted gap, see research/spi-flash-map.md' if gap>=65536 else ''))
if __name__=='__main__':main()
