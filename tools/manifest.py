#!/usr/bin/env python3
"""Generate a hash-bound load description directly from ELF load segments."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
p=argparse.ArgumentParser()
p.add_argument('elf',type=Path)
p.add_argument('--build-id',required=True)
p.add_argument('--vid',type=lambda s:int(s,0),default=0x1908)
p.add_argument('--pid',type=lambda s:int(s,0),default=0x3319)
p.add_argument('--opt',default='-Os')
p.add_argument('--lto',type=int,choices=(0,1),default=0)
p.add_argument('--heap',type=int,default=16384)
p.add_argument('--cpu-mhz',type=int,choices=(96,144),default=144)
p.add_argument('--full-speed',type=int,choices=(0,1),default=0)
p.add_argument('--prefix',type=Path,default=Path(__file__).resolve().parents[1]/'toolchain/install')
args=p.parse_args()
sdk=Path(__file__).resolve().parents[1]
prefix=args.prefix.resolve()
lock=sdk/'toolchain/sources.lock.json'
provenance={'sources':json.loads(lock.read_text()) if lock.exists() else {},
            'compiler':subprocess.check_output([prefix/'bin/or1k-appotech-elf-gcc','--version'],text=True).splitlines()[0],
            'binutils':subprocess.check_output([prefix/'bin/or1k-appotech-elf-ld','--version'],text=True).splitlines()[0],
            'flags':f'{args.opt} -mappotech -fno-delayed-branch -fno-pic{" -flto" if args.lto else ""}',
            'lto':bool(args.lto),'heap_bytes':args.heap,'force_full_speed':bool(args.full_speed)}
libgcc=Path(subprocess.check_output([prefix/'bin/or1k-appotech-elf-gcc','-print-libgcc-file-name'],text=True).strip())
provenance['installed_sha256']={name:hashlib.sha256(path.read_bytes()).hexdigest()
    for name,path in {'gcc':prefix/'bin/or1k-appotech-elf-gcc','libgcc':libgcc,
                      'libc':prefix/'or1k-appotech-elf/lib/libc.a'}.items()}
with args.elf.open('rb') as stream:
    elf=ELFFile(stream)
    if not elf.little_endian or elf.elfclass!=32 or elf.header.e_machine!='EM_OPENRISC':
        raise RuntimeError('expected native little-endian 32-bit OpenRISC ELF')
    segments=[]
    reservations=[[0x02180000,0x20000],[0x02200000,0x4000]]
    dma_section=elf.get_section_by_name('.display_dma')
    if dma_section and dma_section['sh_size']:
        reservations.append([0x02210000,0x10000])
    jpeg_section=elf.get_section_by_name('.jpeg_buffer')
    jpeg_code=elf.get_section_by_name('.jpeg_code')
    audio_code=elf.get_section_by_name('.audio_code')
    camera_code=elf.get_section_by_name('.camera_code')
    peripheral_code=elf.get_section_by_name('.peripheral_code')
    fatfs_section=elf.get_section_by_name('.fatfs_tables')
    large_section=elf.get_section_by_name('.large_buffer')
    lvgl_code=elf.get_section_by_name('.lvgl_code')
    lvgl_tables=elf.get_section_by_name('.lvgl_tables')
    doom_image=elf.get_section_by_name('.doom_image')
    doom_bss=elf.get_section_by_name('.doom_bss')
    if any(section and section['sh_size'] for section in (fatfs_section,large_section,jpeg_code,audio_code,camera_code,peripheral_code,lvgl_code,lvgl_tables,doom_bss)):
        reservations.append([0x02220000,0xc0000])
    if (jpeg_section and jpeg_section['sh_size']) or (doom_image and doom_image['sh_size']) or \
       (lvgl_code and lvgl_code['sh_size']) or (lvgl_tables and lvgl_tables['sh_size']):
        reservations.append([0x022e0000,0x120000])
    if fatfs_section and fatfs_section['sh_size']:
        provenance['fatfs']={'version':'R0.16','sha256':{name:hashlib.sha256((sdk/'firmware/sdk/fatfs'/name).read_bytes()).hexdigest() for name in ('ff.c','ff.h','ffconf.h','ffunicode.c','diskio.h')}}
    for index,segment in enumerate(elf.iter_segments()):
        if segment['p_type']!='PT_LOAD':continue
        address,size=segment['p_vaddr'],segment['p_memsz']
        if not any(base<=address and address+size<=base+length for base,length in reservations):
            raise RuntimeError('ELF segment is outside SDK reservations')
        data=segment.data()
        if data:
            filename=args.elf.with_suffix(f'.segment{index}.bin')
            filename.write_bytes(data)
            segments.append({'address':address,'file':filename.name,'bytes':len(data),
                             'memory_bytes':size,'flags':segment['p_flags'],'sha256':hashlib.sha256(data).hexdigest()})
    symbols=elf.get_section_by_name('.symtab')
    spi_entry=symbols.get_symbol_by_name('apx_spi_entry')
    manifest={'spi_entry':spi_entry[0]['st_value'] if spi_entry else None,'schema_version':1,'entry':elf.header.e_entry,'build_id':args.build_id,
              'elf_sha256':hashlib.sha256(args.elf.read_bytes()).hexdigest(),
              'reservations':reservations,
              'segments':segments,'clock_policy':'sdk-pll1-144' if args.cpu_mhz==144 else 'inherit-loader-96',
              'clock':{'cpu_hz_estimate':args.cpu_mhz*1000000,'incoming':'loader-96',
                       'bootrom_return':'restore-loader-96','critical_block_bytes':32,
                       'settling_iterations':500,'frequency_measured_independently':False},
              'protocol':1,'vid':args.vid,'pid':args.pid,'product':'JT5295 SDK Debug',
              'toolchain':provenance}
    if fatfs_section and fatfs_section['sh_size']:
        manifest['fatfs_tables']={'address':fatfs_section['sh_addr'],'bytes':fatfs_section['sh_size'],'reservation_bytes':0xc0000}
    if large_section and large_section['sh_size']:
        manifest['application_buffer']={'address':large_section['sh_addr'],'bytes':large_section['sh_size'],
            'reservation_bytes':0xc0000,'initialization':'zero enforced by launch preflight'}
    if dma_section and dma_section['sh_size']:
        manifest['display_dma_workspace']={'address':dma_section['sh_addr'],
            'bytes':dma_section['sh_size'],'reservation_bytes':65536,
            'initialization':'zero enforced by launch preflight; conversion fills each active frame'}
    if jpeg_section and jpeg_section['sh_size']:
        manifest['jpeg_dma_workspace']={'address':jpeg_section['sh_addr'],
            'bytes':jpeg_section['sh_size'],'reservation_bytes':0x120000,
            'initialization':'zero enforced by launch preflight; occupied RAM requires explicit backed-up reclamation'}
    if elf.get_section_by_name('.symtab').get_symbol_by_name('apx_image_decode_start'):
        provenance['stb_image']={'version':'2.30','source_sha256':hashlib.sha256((sdk/'third_party/stb/stb_image.h').read_bytes()).hexdigest(),
            'hook_generator_sha256':hashlib.sha256((sdk/'tools/prepare_stb.py').read_bytes()).hexdigest(),
            'configuration':'integer, no SIMD/TLS/stdio/HDR; bounded workspace, cooperative JPEG scans, lossless SD coefficient paging'}
    if jpeg_code and jpeg_code['sh_size']:
        jpeg_tables=elf.get_section_by_name('.jpeg_tables')
        manifest['jpeg_code']={'address':jpeg_code['sh_addr'],'bytes':jpeg_code['sh_size'],
            'table_bytes':jpeg_tables['sh_size'] if jpeg_tables else 0}
    if audio_code and audio_code['sh_size']:
        audio_tables=elf.get_section_by_name('.audio_tables')
        manifest['audio_code']={'address':audio_code['sh_addr'],'bytes':audio_code['sh_size'],
            'table_bytes':audio_tables['sh_size'] if audio_tables else 0}
    if camera_code and camera_code['sh_size']:
        camera_tables=elf.get_section_by_name('.camera_tables')
        manifest['camera_code']={'address':camera_code['sh_addr'],'bytes':camera_code['sh_size'],
            'table_bytes':camera_tables['sh_size'] if camera_tables else 0,
            'sensor_init_sha256':hashlib.sha256((sdk/'firmware/sdk/src/camera_sensor.inc').read_bytes()).hexdigest()}
    if lvgl_code and lvgl_code['sh_size']:
        manifest['lvgl']={'address':lvgl_code['sh_addr'],'bytes':lvgl_code['sh_size'],
            'table_address':lvgl_tables['sh_addr'] if lvgl_tables else 0,
            'table_bytes':lvgl_tables['sh_size'] if lvgl_tables else 0,
            'version':'10.0.0','reservation_address':0x022e0000,'reservation_bytes':0x120000}
args.elf.with_suffix('.manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
