#!/usr/bin/env python3
"""Export the working branches without modifying their index or commits."""
import hashlib
import json
from pathlib import Path
import subprocess

SDK=Path(__file__).resolve().parents[1]
import os
COMPILERS=Path(os.environ.get('APPOTECH_COMPILERS',str(SDK.parent/'compilers')))
SOURCES={
    'gcc':(COMPILERS/'gcc','71b50793e1e2b12b36baff23c1211e43b8c86ecd'),
    'binutils':(COMPILERS/'binutils-gdb','6ce87bbc521cf46eaee9a1f7ef61cee2cdfb3e32'),
    'picolibc':(SDK/'third_party/picolibc','2ae376c6cdf4fef90ca2388ecf7a07457fa63cff'),
}
def main():
    directory=SDK/'toolchain/patches';directory.mkdir(exist_ok=True)
    lock={'target':'or1k-appotech-elf','abi':'little endian; no delay; link PC+4; software multiply/divide/float',
          'target_cflags':'-Os -mappotech -fno-delayed-branch -fno-pic -ffunction-sections -fdata-sections',
          'picolibc_options':{'multilib':False,'picocrt':False,'semihost':False,'single-thread':True,
                             'thread-local-storage':False,'format-default':'integer','io-long-long':True,
                             'posix-console':False,'atomic-ungetc':False,'printf-small-ultoa':True,
                             'initfini-array':False,'sysroot-install':True,'system-libc':True},
          'sources':{}}
    for name,(source,baseline) in SOURCES.items():
        head=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
        if head!=baseline:raise RuntimeError(f'{name}: expected baseline {baseline}, found {head}')
        patch=subprocess.check_output(['git','diff','--binary','HEAD'],cwd=source)
        if name=='gcc':
            extra='libgcc/config/or1k/appotech-lib1funcs.S'
            tracked=subprocess.run(['git','ls-files','--error-unmatch',extra],cwd=source,capture_output=True)
            if tracked.returncode:
                result=subprocess.run(['git','diff','--no-index','--','/dev/null',extra],cwd=source,capture_output=True)
                if result.returncode!=1:raise RuntimeError('cannot export libgcc assembly')
                patch+=result.stdout
        filename=f'{name}-jt5295.patch';(directory/filename).write_bytes(patch)
        lock['sources'][name]={'baseline':baseline,'patch':f'patches/{filename}',
                              'patch_sha256':hashlib.sha256(patch).hexdigest()}
    lock['sources']['gcc'].update(version='16.2.1',branch='gcc16-openrisc-appotech')
    lock['sources']['binutils'].update(version='2.47',branch='binutils-openrisc-appotech')
    lock['sources']['picolibc'].update(version='1.8.12',tag='1.8.12',url='https://github.com/picolibc/picolibc.git')
    (SDK/'toolchain/sources.lock.json').write_text(json.dumps(lock,indent=2)+'\n')
if __name__=='__main__':main()
