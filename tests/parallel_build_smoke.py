"""Build real objects concurrently and install the pinned Nano port inside MyOS."""
from pathlib import Path
import hashlib
import subprocess
import tempfile
from hardware_inventory_smoke import VM, login
import hardware_smoke

B=Path('build')
archive=B/'nano-port/nano-9.3.tar.gz'
assert hashlib.sha256(archive.read_bytes()).hexdigest()=='3208f3c459c64e4a4e0ef242d2653572a56ca9a6cfbdc4de5af4714594ae8836', 'Prepare the pinned Nano archive with make nano-build'


def run(*args):subprocess.run([str(x) for x in args],check=True,stdout=subprocess.DEVNULL)


with tempfile.TemporaryDirectory(prefix='myos-parallel-build-') as temporary:
    temporary=Path(temporary);disk=temporary/'root.img'
    with disk.open('wb') as stream:stream.truncate(128*1048576)
    def fs(*args):run(B/'myfs',disk,*args)
    for directory in ['/bin','/lib','/include','/etc/ext','/root','/home','/tmp','/ext/instructions','/ext/installed','/ext/bin']:
        fs('mkdir',directory)
    for name in ['sh','make','ext','cc','tar','cp','ar','echo']:
        fs('put',B/'user'/name,'/bin/'+name)
    fs('put',B/'make-job-probe','/bin/cc-probe')
    fs('put',B/'ld.so','/lib/ld.so')
    for name in ['libc.so','libgfx.so']:fs('put',B/'user'/name,'/lib/'+name)
    for name in ['libc.a','libm.a','libgfx.a']:fs('put',B/'mlib'/name,'/lib/'+name)
    for source in sorted((B/'ccroot/include').rglob('*')):
        target='/include/'+source.relative_to(B/'ccroot/include').as_posix()
        if source.is_dir():fs('mkdir',target)
        elif source.is_file():fs('put',source,target)
    def text(target,content):
        local=temporary/'input';local.write_text(content);fs('put',local,target)
    text('/etc/ext/build-jobs','auto\n')
    for package in ['parallel','serial','configured']:
        base='/ext/instructions/'+package;fs('mkdir',base)
        instruction=f'name {package}\ndescr Parallel build regression\nworkdir /tmp/ext/{package}\n'
        for name in ['a','b','c','d']:
            source='#include "shared.h"\n'+''.join(f'int {name}{i}(int v){{return v+{i};}}\n' for i in range(1000))
            source+=f'int {name}(void){{return VALUE;}}\n'
            text(base+'/'+name+'.c',source);instruction+=f'copy {name}.c {name}.c\n'
        text(base+'/main.c','#include <stdio.h>\nint a(void);int b(void);int c(void);int d(void);\nint main(void){if(a()+b()+c()+d()!=28)return 1;puts("LINKED BUILD RESULT PASS");return 0;}\n')
        text(base+'/template.h','#define VALUE 7\n')
        makefile='all: prog\nprog: a.o b.o c.o d.o main.c\n\tcc a.o b.o c.o d.o main.c -o prog\n'
        for name in ['a','b','c','d']:
            makefile+=f'{name}.o: {name}.c shared.h\n\t/bin/cc-probe -c {name}.c -o {name}.o\n\t/bin/cc-probe end {name}.o\n'
        makefile+='shared.h: template.h\n\tcp template.h shared.h\n'
        text(base+'/Makefile',makefile)
        instruction+='copy main.c main.c\ncopy template.h template.h\ncopy Makefile Makefile\nmake\n'
        text(base+'/setup.inst',instruction)
    fs('mkdir','/ext/instructions/nano')
    for source in Path('ext/instructions/nano').iterdir():
        if source.is_file():fs('put',source,'/ext/instructions/nano/'+source.name)
    # Exercise the official recipe, omitting only its network download.
    recipe='\n'.join('# preloaded pinned archive' if line.startswith('fetch ') else line
                     for line in Path('ext/instructions/nano/setup.inst').read_text().splitlines())+'\n'
    text('/ext/instructions/nano/setup.inst',recipe)
    fs('mkdir','/tmp/ext/nano');fs('put',archive,'/tmp/ext/nano/nano-9.3.tar.gz')
    lua_source=B/'lua-5.4.8'
    assert (lua_source/'lua.h').is_file(), 'Prepare Lua 5.4.8 with make luatest'
    fs('mkdir','/ext/instructions/lua');fs('mkdir','/tmp/ext/lua-src')
    for source in Path('ext/instructions/lua').iterdir():
        if source.is_file():fs('put',source,'/ext/instructions/lua/'+source.name)
    for source in lua_source.iterdir():
        if source.is_file():fs('put',source,'/tmp/ext/lua-src/'+source.name)
    lines=Path('ext/instructions/lua/setup.inst').read_text().splitlines()
    text('/ext/instructions/lua/setup.inst','\n'.join('# preloaded pinned source' if line.startswith('git ') else line for line in lines)+'\n')
    vm=VM('parallel-build-native',B/'os.img',[
        '-drive',f'format=raw,file={disk},index=1,snapshot=on'],4,memory='512M')
    try:
        login(vm)
        vm.send('ext install parallel','installed parallel')
        vm.send('/bin/cc-probe check 4','PARALLEL BUILD PASS jobs=4')
        vm.send('/tmp/ext/parallel/prog','LINKED BUILD RESULT PASS')
        vm.send('ext install -j1 serial','installed serial')
        vm.send('/bin/cc-probe check 1','PARALLEL BUILD PASS jobs=1')
        vm.send('/tmp/ext/serial/prog','LINKED BUILD RESULT PASS')
        vm.send('echo 2 > /etc/ext/build-jobs','root:/')
        vm.send('ext install configured','installed configured')
        vm.send('/bin/cc-probe check 2','PARALLEL BUILD PASS jobs=2')
        vm.send('echo auto > /etc/ext/build-jobs','root:/')
        vm.boot_timeout=300
        vm.send('ext install nano','installed nano')
        vm.send('nano --version','GNU nano, version 9.3')
        vm.send('ext install lua','installed lua')
        vm.send('lua -v','Lua 5.4.8')
        log=hardware_smoke.clean(vm.log)
        assert 'make -j4' in log and 'make -j1' in log and 'make -j2' in log,log[-8000:]
        assert 'JOB PROBE FAIL' not in log and 'PANIC' not in log,log[-8000:]
        print('Native cc: four concurrent jobs on multiple CPUs, explicit -j1, generated headers and linked programs: PASS',flush=True)
        print('EXT installs real Nano and Lua recipes with automatic parallel jobs; version checks: PASS',flush=True)
    finally:vm.stop()
