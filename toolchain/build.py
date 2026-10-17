#!/usr/bin/env python3
"""Build the existing compiler trees out of source; never changes branches."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

SDK = Path(__file__).resolve().parents[1]
WORKSPACE = SDK.parent
# Where the pinned gcc / binutils-gdb checkouts live (docs/TOOLCHAIN.md). Override with APPOTECH_COMPILERS=/path/to/dir.
COMPILERS = Path(os.environ.get('APPOTECH_COMPILERS', str(WORKSPACE / 'compilers')))
PREFIX = SDK / 'toolchain/install'
BUILD = SDK / 'toolchain/build'
TARGET = 'or1k-appotech-elf'
FLAGS = '-Os -mappotech -fno-delayed-branch -fno-pic -ffunction-sections -fdata-sections'

def prepare(source,name):
    lock=json.loads((SDK/'toolchain/sources.lock.json').read_text())['sources'][name]
    patch=SDK/'toolchain'/lock['patch']
    if hashlib.sha256(patch.read_bytes()).hexdigest()!=lock['patch_sha256']:
        raise RuntimeError(f'{name}: patch hash mismatch')
    head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip()
    if head!=lock['baseline']:raise RuntimeError(f'{name}: expected {lock["baseline"]}, found {head}')
    applied=subprocess.run(['git','apply','--reverse','--check',str(patch)],cwd=source,capture_output=True)
    if applied.returncode:
        subprocess.run(['git','apply','--check',str(patch)],cwd=source,check=True)
        subprocess.run(['git','apply',str(patch)],cwd=source,check=True)

def run(args, cwd, env):
    print('Running:', ' '.join(map(str,args)), flush=True)
    subprocess.run(list(map(str,args)), cwd=cwd, env=env, check=True)

def main():
    p=argparse.ArgumentParser()
    p.add_argument('stage', choices=['binutils','gcc','libgcc','picolibc'])
    p.add_argument('--jobs',type=int,default=3)
    args=p.parse_args()
    env=os.environ.copy()
    env['PATH']=str(PREFIX/'bin')+os.pathsep+env['PATH']
    env['CFLAGS_FOR_TARGET']=FLAGS
    BUILD.mkdir(parents=True,exist_ok=True)
    (PREFIX/TARGET/'usr/include').mkdir(parents=True,exist_ok=True)
    source=(COMPILERS/('binutils-gdb' if args.stage=='binutils' else 'gcc')).resolve()
    if args.stage in ('binutils','gcc'):
        expected='binutils-openrisc-appotech' if args.stage=='binutils' else 'gcc16-openrisc-appotech'
        branch=subprocess.check_output(['git','-C',str(source),'branch','--show-current'],text=True).strip()
        if branch!=expected: raise RuntimeError(f'expected {expected}, found {branch}')
        prepare(source,args.stage)
        out=BUILD/args.stage;out.mkdir(exist_ok=True)
        if not (out/'Makefile').exists():
            common=[source/'configure',f'--target={TARGET}',f'--prefix={PREFIX}',
                    '--disable-nls','--disable-werror','--disable-multilib']
            if args.stage=='binutils':
                common+=['--disable-gdb','--disable-gdbserver','--disable-sim',
                         '--disable-gprofng','--disable-shared','--disable-maintainer-mode']
            else:
                common+=['--enable-languages=c,c++','--without-headers','--disable-bootstrap',
                         '--disable-shared','--disable-threads','--disable-libssp',
                         '--disable-libquadmath','--disable-libgomp','--disable-libatomic',
                         '--disable-libsanitizer','--with-picolibc',f'--with-sysroot={PREFIX/TARGET}']
            run(common,out,env)
        targets=['all','install'] if args.stage=='binutils' else ['all-gcc','install-gcc']
        for target in targets:run(['make',f'-j{args.jobs}',target],out,env)
    elif args.stage=='libgcc':
        for target in ['all-target-libgcc','install-target-libgcc']:
            run(['make',f'-j{args.jobs}',f'CFLAGS_FOR_TARGET={FLAGS} --sysroot={PREFIX/TARGET}',target],BUILD/'gcc',env)
    else:
        source=SDK/'third_party/picolibc'
        if not source.exists():
            source.parent.mkdir(exist_ok=True)
            run(['git','clone','--depth=1','--branch','1.8.12',
                 'https://github.com/picolibc/picolibc.git',source],SDK,env)
        prepare(source,'picolibc')
        out=BUILD/'picolibc'
        if not (out/'build.ninja').exists():
            run(['meson','setup',out,source,'--cross-file',SDK/'toolchain/picolibc-cross.ini',
                 f'--prefix={PREFIX/TARGET}','-Dsysroot-install=true','-Dsystem-libc=true',
                 '-Dincludedir=usr/include','-Dlibdir=lib',
                 '-Dmultilib=false','-Dpicocrt=false','-Dsemihost=false',
                 '-Dsingle-thread=true','-Dthread-local-storage=false','-Dformat-default=integer',
                 '-Dio-long-long=true',
                 '-Dposix-console=false','-Datomic-ungetc=false',
                 '-Dprintf-small-ultoa=true','-Dinitfini-array=false','--buildtype=minsize'],SDK,env)
        else:
            run(['meson','configure',out,'-Dio-long-long=true'],SDK,env)
        run(['ninja','-C',out,f'-j{args.jobs}'],SDK,env)
        run(['ninja','-C',out,'install'],SDK,env)
    print('Completed',args.stage,flush=True)
if __name__=='__main__':main()
