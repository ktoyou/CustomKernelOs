"""Generate kernel identity; SOURCE_DATE_EPOCH supports reproducible builds."""
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

GIT_TIMEOUT = 3

def git(*args):
    try:
        return subprocess.check_output(
            ['git', '--no-optional-locks', *args], stderr=subprocess.DEVNULL,
            text=True, timeout=GIT_TIMEOUT).strip()
    except subprocess.TimeoutExpired:
        print('build_info: git ' + args[0] + ' timed out; continuing without complete Git metadata', file=sys.stderr)
        return None
    except (OSError, subprocess.CalledProcessError):
        return None

commit = os.environ.get('MYOS_BUILD_COMMIT') or git('rev-parse', 'HEAD') or 'unknown'
if commit != 'unknown':
    state = git('status', '--porcelain', '--untracked-files=normal')
    if state is None:
        commit += '-state-unknown'
    elif state:
        commit += '-dirty'
epoch = os.environ.get('SOURCE_DATE_EPOCH')
now = datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc) if epoch else datetime.datetime.now(datetime.timezone.utc)
version = Path('VERSION').read_text().strip()
build_time = now.strftime('%Y-%m-%d %H:%M:%S UTC')
out = Path(sys.argv[1])

def defined(text, key):
    prefix = '#define %s ' % key
    for line in text.splitlines():
        if line.startswith(prefix):
            return json.loads(line[len(prefix):])
    return None

# Время сборки — часть ядра. Пока версия и коммит те же, оставляем прежнее,
# иначе каждый make run на новой секунде пересобирает sysinfo.c и ядро.
if out.exists():
    previous = out.read_text()
    if defined(previous, 'MYOS_VERSION') == version and defined(previous, 'MYOS_COMMIT') == commit:
        old_time = defined(previous, 'MYOS_BUILD_TIME')
        if old_time:
            build_time = old_time
values = {'MYOS_VERSION': version, 'MYOS_COMMIT': commit, 'MYOS_BUILD_TIME': build_time}
for key, limit in [('MYOS_VERSION', 31), ('MYOS_COMMIT', 63), ('MYOS_BUILD_TIME', 31)]:
    if len(values[key].encode('utf-8')) > limit:
        raise SystemExit(key + ' exceeds kernel field size')
data = ''.join('#define %s %s\n' % (k, json.dumps(v)) for k, v in values.items())
if not out.exists() or out.read_text() != data:
    out.write_text(data)
