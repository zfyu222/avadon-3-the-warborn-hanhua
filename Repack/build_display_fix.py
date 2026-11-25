"""Rebuild only the two display adapters against the verified localization."""

import json
from pathlib import Path
import shutil
from tempfile import TemporaryDirectory

from build_runtime import EXE, EXE_HASH, PROJECT, compile_adapter, shared_tools
from stage_text import read_json, sha


def build(reference, output):
    manifest = read_json(reference / 'TEST_MANIFEST.json')
    if manifest['exe_sha256'] != EXE_HASH or sha((reference / EXE).read_bytes()) != EXE_HASH:
        raise ValueError('Unsupported executable')
    name = 'Avadon3Chinese/resources.bin'
    if sha((reference / name).read_bytes()) != manifest['runtime']['files'][name]:
        raise ValueError('The published translation registry has changed')
    if output.exists() or output.resolve().is_relative_to((PROJECT / 'Game').resolve()):
        raise ValueError('Expected a new staging directory outside the Steam game')
    with TemporaryDirectory(prefix='av3-display-', dir=output.parent) as temporary:
        work = Path(temporary)
        base = work / 'base'
        (base / name).parent.mkdir(parents=True)
        shutil.copy2(reference / EXE, base / EXE)
        shutil.copy2(reference / name, base / name)
        compilation = work / 'compile'
        compilation.mkdir()
        compile_adapter(compilation, base, shared_tools())
        manifest['runtime']['files']['winmm.dll'] = sha((base / 'winmm.dll').read_bytes())
        (base / 'TEST_MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        base.rename(output)
    return manifest['runtime']['files']['winmm.dll']


if __name__ == '__main__':
    print(build(PROJECT / 'ImagePatch/game-final-local', PROJECT / 'ImagePatch/display-fix-r2'))
