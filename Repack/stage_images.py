"""Stage visually approved textures for the Avadon 3 Steam Windows loader."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
from tempfile import TemporaryDirectory

from PIL import Image


PROJECT = Path(__file__).resolve().parents[1]
GAME = PROJECT / 'Game'
EXE_HASH = 'c9b1e33cbd14fa21ea876f50355ad7369a9fd74186e08e669210acf0cab7c503'
ASSET = re.compile(r'Avadon 3 Files/(Graphics Core|Graphics A|Graphics B)/G(\d+)\.(bmp|png|jpg)')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def image_origin(source, game):
    relative = source['path']
    match = ASSET.fullmatch(relative)
    if not match:
        raise ValueError(f'Unexpected texture path {relative!r}')
    family, number, suffix = match.groups()
    number = int(number)
    expected_family = 'Graphics Core' if number < 1000 else 'Graphics A' if number < 2500 else 'Graphics B'
    if family != expected_family:
        raise ValueError(f'Texture ID does not resolve to this directory: {relative}')
    original = game / relative
    if sha(original) != source['file_sha256']:
        raise ValueError(f'Game texture changed: {relative}')
    size = (source['width'], source['height'])
    with Image.open(original) as image:
        if image.size != size:
            raise ValueError(f'Game texture dimensions changed: {relative}')
    priorities = ('bmp', 'jpg', 'png')
    for prior in priorities[:priorities.index(suffix)]:
        if original.with_suffix('.' + prior).exists():
            raise ValueError(f'Game loader selects a different texture ahead of {relative}')
    return Path(relative), size


def stage(verified, destination, game=GAME, project=PROJECT):
    verified, destination, game = verified.resolve(), destination.resolve(), game.resolve()
    if (destination.exists() or destination.is_relative_to(game)
            or destination.is_relative_to(verified) or verified.is_relative_to(destination)):
        raise ValueError('Use a new staging directory outside the game and verified textures')
    if sha(game / 'Avadon 3.exe') != EXE_HASH:
        raise ValueError('Unsupported Avadon 3 executable')
    manifest_file = verified / 'IMAGE_PATCH_MANIFEST.json'
    manifest = read_json(manifest_file)
    if (manifest.get('status') != 'ready_for_engine_adapter'
            or manifest.get('stage') != 'final' or manifest.get('mode') != 'texture'
            or manifest.get('visual_review', {}).get('status') != 'static_pass'):
        raise ValueError('Expected collected, visually approved textures')
    if (manifest['translation_csv_sha256'] != sha(project / 'avadon-3_images_text_translated.csv')):
        raise ValueError('Approved textures refer to a different formal translation')
    mapping = read_json(project / 'avadon-3_images_text.mapping.json')
    if mapping['csv_sha256'] != sha(project / 'avadon-3_images_text.csv'):
        raise ValueError('Confirmed image mapping refers to a different source')
    mapped_sources = {}
    for region in mapping['regions']:
        image_id = region['image_id']
        if mapped_sources.setdefault(image_id, region['sources']) != region['sources']:
            raise ValueError(f'Conflicting original texture provenance: {image_id}')
    approved_sources = {}
    for entry in manifest['entries']:
        image_id = entry['image_id']
        if not entry['sources'] or entry['sources'] != mapped_sources.get(image_id):
            raise ValueError(f'Approved texture provenance differs from the confirmed mapping: {image_id}')
        approved_sources[image_id] = entry['sources']
    plans = {}
    for file in manifest['files']:
        texture = (verified / file['path']).resolve()
        if not texture.is_relative_to(verified) or sha(texture) != file['sha256']:
            raise ValueError('Approved texture is outside the manifest or has changed')
        sources = approved_sources.pop(file['image_id'], None)
        if not sources or file.get('sources', sources) != sources:
            raise ValueError(f'Missing or duplicate confirmed provenance for {file["image_id"]}')
        with Image.open(texture) as rendered:
            if rendered.format != 'PNG':
                raise ValueError(f'Expected PNG in verified texture collection: {texture}')
            for source in sources:
                target, size = image_origin(source, game)
                if rendered.size != size:
                    raise ValueError(f'Rendered texture dimensions changed: {source["path"]}')
                if target.suffix in ('.bmp', '.jpg') and rendered.convert('RGBA').getchannel('A').getextrema() != (255, 255):
                    raise ValueError(f'Non-alpha texture needs explicit handling of transparency: {source["path"]}')
                key = target.as_posix().casefold()
                if key in plans:
                    raise ValueError(f'Multiple approved textures resolve to {target}')
                plans[key] = (texture, target, size, source)
    if not plans or approved_sources:
        raise ValueError('Every approved image requires a staged texture')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix='.avadon3-textures-', dir=destination.parent) as temp:
        staging = Path(temp) / 'payload'
        rows = []
        for texture, relative, size, origin in plans.values():
            target = staging / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            expected_format = {'.bmp': 'BMP', '.jpg': 'JPEG', '.png': 'PNG'}[target.suffix]
            if expected_format == 'PNG':
                shutil.copyfile(texture, target)
            else:
                options = {'quality': 97, 'subsampling': 0} if expected_format == 'JPEG' else {}
                with Image.open(texture) as source:
                    source.convert('RGB').save(target, expected_format, **options)
            with Image.open(target) as output:
                if output.size != size or output.format != expected_format:
                    raise ValueError(f'Staged texture failed format/size verification: {relative}')
            rows.append({'path': relative.as_posix(), 'sha256': sha(target),
                         'source': origin['path'], 'original_sha256': origin['file_sha256']})
        report = {'status': 'staged', 'installed': False, 'exe_sha256': EXE_HASH,
                  'verified_manifest_sha256': sha(manifest_file), 'files': rows}
        (staging / 'IMAGE_PAYLOAD.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8'
        )
        staging.rename(destination)
    return len(rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verified', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps({'staged_images': stage(args.verified, args.out)}))
