"""Check clean/dirty identity, archive fallback and reproducible timestamps."""
import os
import runpy
from unittest.mock import patch
from pathlib import Path
import subprocess
import sys
import tempfile

script=Path('tools/build_info.py').resolve()
with tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp)
    (root/'VERSION').write_text('0.19\n')
    env=dict(os.environ, SOURCE_DATE_EPOCH='0')
    env.pop('MYOS_BUILD_COMMIT',None)
    def generate():
        subprocess.run([sys.executable,str(script),'identity.h'],cwd=root,env=env,check=True)
        return (root/'identity.h').read_text()
    first=generate()
    assert '"unknown"' in first and '1970-01-01 00:00:00 UTC' in first
    assert generate()==first
    def git(*args):
        return subprocess.check_output(['git',*args],cwd=root,text=True,stderr=subprocess.DEVNULL).strip()
    git('init')
    (root/'.gitignore').write_text('identity.h\n')
    git('add','VERSION','.gitignore')
    git('-c','user.name=Test','-c','user.email=test@example.invalid','commit','-m','fixture')
    commit=git('rev-parse','HEAD')
    assert '"'+commit+'"' in generate()
    (root/'VERSION').write_text('0.20\n')
    assert commit+'-dirty' in generate()
    (root/'VERSION').write_text('0.19\n')
    assert '"'+commit+'"' in generate()
    frozen=(root/'identity.h').read_text()
    later=dict(env)
    later.pop('SOURCE_DATE_EPOCH', None)
    subprocess.run([sys.executable, str(script), 'identity.h'], cwd=root, env=later, check=True)
    assert (root/'identity.h').read_text()==frozen
    (root/'new-source.c').write_text('/* untracked */')
    assert commit+'-dirty' in generate()
    # A slow status must not block the build or falsely label it clean.
    def slow_status(command, **kwargs):
        assert kwargs['timeout'] == 3
        assert '--no-optional-locks' in command
        if 'rev-parse' in command:
            return commit + '\n'
        raise subprocess.TimeoutExpired(command, kwargs['timeout'])
    old_cwd=Path.cwd()
    try:
        os.chdir(root)
        with patch.dict(os.environ,env,clear=True), \
             patch.object(sys,'argv',[str(script),'identity.h']), \
             patch('subprocess.check_output',side_effect=slow_status):
            runpy.run_path(str(script),run_name='__main__')
        assert commit+'-state-unknown' in (root/'identity.h').read_text()
    finally:
        os.chdir(old_cwd)
print('Build metadata: PASS')
