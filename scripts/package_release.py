"""Package the frozen pair and matching Git source; no build or device I/O."""
from pathlib import Path
import hashlib
import json
import subprocess
import zipfile

root = Path(__file__).resolve().parents[1]
dist = root / 'dist'
dist.mkdir(exist_ok=True)
manifest = json.loads((root / 'release/manifest.json').read_text())
for name, info in manifest['binaries'].items():
    data = (root / 'release' / name).read_bytes()
    if len(data) != info['bytes'] or hashlib.sha256(data).hexdigest() != info['sha256']:
        raise SystemExit(f'Binary mismatch: {name}')
if subprocess.check_output(['git', 'status', '--porcelain'], cwd=root).strip():
    raise SystemExit('Commit changes before packaging matching sources')

base = 'GePatch-Adrenaline-2026.10.30'
source = dist / f'{base}-source.zip'
subprocess.run(['git', 'archive', '--format=zip', f'--prefix={base}/',
                f'--output={source}', 'HEAD'], cwd=root, check=True)
update = dist / f'{base}-update.zip'
with zipfile.ZipFile(update, 'w', zipfile.ZIP_DEFLATED) as z:
    for path in sorted((root / 'release').iterdir()):
        if path.is_file():
            z.write(path, path.name)
    for name in ('README.md', 'LICENSES.md', 'RELEASE_NOTES.md'):
        z.write(root / name, name)
    z.write(root / 'gepatch/LICENSE.TXT', 'licenses/GePatch-LICENSE.TXT')
    z.write(root / 'adrenaline/src/LICENSE', 'licenses/Adrenaline-LICENSE')
    for path in (root / 'adrenaline/src').rglob('*'):
        if path.is_file() and path.name.upper().startswith('LICENSE') and path.name != 'LICENSE':
            z.write(path, 'licenses/' + path.relative_to(root).as_posix())
    z.writestr('SOURCE.txt',
        'Matching source: GePatch-Adrenaline-2026.10.30-source.zip\n'
        'Repository: https://github.com/Fevzy/GePatch-Adrenaline\n'
        'Tag: v2026.10.30\n')
for path in (source, update):
    with zipfile.ZipFile(path) as z:
        assert z.testzip() is None, path.name
        for name, info in manifest['binaries'].items():
            entry = f'{base}/release/{name}' if path == source else name
            assert hashlib.sha256(z.read(entry)).hexdigest() == info['sha256']
sums = dist / 'SHA256SUMS'
entries = [(p.name, hashlib.sha256(p.read_bytes()).hexdigest()) for p in (update, source)]
entries += [(n, i['sha256']) for n, i in manifest['binaries'].items()]
sums.write_text(''.join(f'{digest}  {name}\n' for name, digest in entries))
print(sums.read_text())
