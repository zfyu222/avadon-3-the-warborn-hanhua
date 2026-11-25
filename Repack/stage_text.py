"""Bind an approved Chinese corpus to the exact Avadon 3 resource occurrences."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory


PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT.parents[1] / 'Tools/localization'))
from workflow_resource_occurrences import ResourceResolver


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def stage(project, output, original_root=None):
    source = project / 'avadon-3_text.csv'
    translated = project / 'avadon-3_text_translated.csv'
    mapping_path = source.with_suffix('.mapping.json')
    report = read_json(project / 'TranslateFiles/reports/final_validation.json')
    source_hash = sha(source.read_bytes())
    translation_hash = sha(translated.read_bytes())
    if (report.get('status') != 'passed' or report.get('semantic_review') != 'full'
            or report.get('source_sha256') != source_hash
            or report.get('translation_sha256') != translation_hash):
        raise ValueError('Expected the current formally validated translation')
    extraction = read_json(project / 'Extraction/report.json')
    mapping_bytes = mapping_path.read_bytes()
    if sha(mapping_bytes) != extraction['mapping_sha256']:
        raise ValueError('The extracted resource mapping changed')
    mapping = json.loads(mapping_bytes)['records']
    resolver = ResourceResolver(source, translated, mapping_path=mapping_path)
    if len(mapping) != len(resolver.targets) or len(mapping) != report['records']:
        raise ValueError('Translation and resource occurrence count mismatch')

    source_manifest = read_json(project / 'Extraction/source_manifest.json')
    originals = {}
    game = (project / 'Game').resolve()
    original_root = original_root.resolve() if original_root else game
    for name, identity in source_manifest.items():
        path = (original_root / name).resolve()
        if not path.is_relative_to(original_root):
            raise ValueError(f'Original resource is outside the source directory: {name}')
        content = path.read_bytes()
        if len(content) != identity['size'] or sha(content) != identity['sha256']:
            raise ValueError(f'Original resource changed: {name}')
        originals[name] = content

    by_source = defaultdict(list)
    records = []
    for entry in mapping:
        ordinal = entry['ordinal']
        name = entry['file']
        offset, length = entry['offset'], entry['length']
        raw = originals[name][offset:offset + length]
        original = raw.decode(entry['encoding'])
        if original != resolver.sources[ordinal - 1]:
            raise ValueError(f'Original bytes differ at {name} offset {offset:#x}')
        target = resolver.resolve(entry['resource'], original)
        record = {**entry, 'source': original, 'translation': target}
        records.append(record)
        by_source[original].append(record)
    resolver.finish()
    ambiguous = [{'source': original, 'ordinals': [row['ordinal'] for row in rows]}
                 for original, rows in by_source.items()
                 if len({row['translation'] for row in rows}) > 1]
    if output.resolve().is_relative_to(game):
        raise ValueError('Use a staging path outside the game directory')
    if output.exists():
        raise FileExistsError(output)
    staging = dict(status='occurrences_verified', source_sha256=source_hash,
                   translation_sha256=translation_hash, by_kind=dict(Counter(row['kind'] for row in records)),
                   requires_context=ambiguous, records=records)
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix='.avadon3-text-', dir=output.parent) as temporary:
        ready = Path(temporary) / output.name
        with ready.open('w', encoding='utf-8') as handle:
            json.dump(staging, handle, ensure_ascii=False, indent=2)
            handle.write('\n')
        ready.rename(output)
    return dict(records=len(records), requires_context=len(ambiguous))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    print(json.dumps(stage(PROJECT, parser.parse_args().out)))
