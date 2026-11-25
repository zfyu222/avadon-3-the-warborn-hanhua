"""Build a resource-indexed x86 Chinese renderer for the analyzed Steam build."""
import argparse
from collections import Counter, defaultdict
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
from tempfile import TemporaryDirectory

from stage_text import PROJECT, read_json, sha, stage
from workflow_resource_occurrences import column


REPACK = PROJECT / 'Repack'
WORKSPACE = PROJECT.parents[1]
SHARED = PROJECT.parent / 'avadon-the-black-fortress' / 'Repack'
EXE = 'Avadon 3.exe'
EXE_HASH = 'c9b1e33cbd14fa21ea876f50355ad7369a9fd74186e08e669210acf0cab7c503'
PROLOGUES = {
    0x488780: bytes.fromhex('558bec81ec80080000'),
    0x488600: bytes.fromhex('558bec81ec30040000'),
    0x4caaf5: bytes.fromhex('8bff558bec83ec20'),
}
FORMAT = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hlLzjt])?[diuoxXfFeEgGaAcsp])')
MARKER = re.compile(rb'~[0-9A-Za-z]{3}~')
DIGITS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'
SMOKE_LABELS = {
    'Medals You Have Earned': '已获得的勋章',
    '%s Inventory:': '%s 的物品栏：',
    'Welcome to Avadon 3!': '欢迎来到阿瓦登 3！',
    'Here are some tips which will make playing Avadon 3: The Warborn a much more pleasant experience.':
        '这里有一些实用提示。它们能帮你熟悉阿瓦登的战斗与探索，让你从容应对接下来的挑战。',
    'Avadon 3': '阿瓦登 3',
}
WIDE_FILTERS = {
    'Avadon 3 Save Files (*.SAV)': '*.sav',
    'Text Files (*.TXT)': '*.txt',
    'All Files (*.*)': '*.*',
}


def shared_tools():
    spec = importlib.util.spec_from_file_location('avadon_build_tools', SHARED / 'build_patch.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def marker_for(row):
    ordinal = row['ordinal']
    if not 0 < ordinal < 36 ** 3:
        raise ValueError(f'Ordinal outside the marker ID range: {ordinal}')
    value = '~' + DIGITS[ordinal // 1296] + DIGITS[ordinal // 36 % 36] + DIGITS[ordinal % 36] + '~'
    return value.encode('ascii')


def resource_span(row, original):
    start, length = row['offset'], row['length']
    replacement = marker_for(row)
    target = row['translation']
    if row['kind'] == 'scenario':
        if len(replacement) >= row['capacity']:
            raise ValueError(f"Marker exceeds DAT field capacity: {row['resource']}")
        if original[start + length] != 0:
            raise ValueError(f"Scenario DAT terminator differs: {row['resource']}")
        # Short map labels can retain old bytes after their NUL inside the fixed field.
        length = max(length + 1, len(replacement) + 1)
        replacement = replacement.ljust(length, b'\0')
    elif row['file'].endswith('/textdocs.txt'):
        # The help viewer uses $ and a two-space prefix as control flags.
        prefix = ('$' if row['source'].startswith('$') else
                  '  ' if row['source'].startswith('  ') else '')
        replacement = prefix.encode('ascii') + replacement
        target = target.removeprefix(prefix)
    return length, replacement, target


def native_entry(row, executable):
    encoding = row['encoding']
    source = row['source'].encode(encoding)
    target = row['translation']
    offset, rva = row['offset'], int(row['rva'], 16)
    wide_resource = row['context'] == 'win32_string_resource'
    if wide_resource:
        length = struct.pack('<H', len(source) // 2)
        if executable[offset - 2:offset + len(source)] != length + source:
            raise ValueError(f"Windows string table changed: {row['resource']}")
    elif executable[offset:offset + len(source) + len('\0'.encode(encoding))] != source + '\0'.encode(encoding):
        raise ValueError(f"Native string changed: {row['resource']}")
    if encoding == 'utf-16-le':
        remaining = len(source) - len(target.encode(encoding))
        if remaining < 0:
            raise ValueError(f"Windows string exceeds its original slot: {row['resource']}")
        file_filter = WIDE_FILTERS.get(row['source'])
        if file_filter:
            suffix = ('\0' + file_filter + '\0').encode(encoding)
            end = offset + len(source)
            if executable[end:end + len(suffix)] != suffix:
                raise ValueError(f"Windows file filter pattern changed: {row['resource']}")
        target += (' ' if wide_resource or file_filter else '\0') * (remaining // 2)
        return rva | (0xc0000000 if wide_resource else 0x40000000), source, target
    conversions = FORMAT.findall(row['source'])
    if conversions != FORMAT.findall(target):
        raise ValueError(f"Ordered printf placeholders changed: {row['resource']}")
    return rva | (0x80000000 if conversions else 0), source, target


def write_registry(game, payload, rows, executable):
    changed = defaultdict(list)
    entries = []
    counts = Counter()
    originals = read_json(PROJECT / 'Extraction/source_manifest.json')
    for row in rows:
        target = row['translation']
        if row['source'] == target:
            counts['preserved'] += 1
            continue
        if row['context'] == 'call:set_level':
            # This argument is consumed by the script VM as a level, rather than displayed.
            counts['script_argument_preserved'] += 1
            continue
        if not target or '\0' in target:
            raise ValueError(f"Empty or embedded-NUL translation: {row['resource']}")
        if row['kind'] == 'native':
            entries.append(native_entry(row, executable))
        else:
            changed[row['file']].append(row)
        counts[row['kind']] += 1

    for name, replacements in changed.items():
        path = (game / name).resolve()
        if not path.is_relative_to(game) or name not in originals:
            raise ValueError(f'Unexpected game resource: {name}')
        original = path.read_bytes()
        if sha(original) != originals[name]['sha256'] or MARKER.search(original):
            raise ValueError(f'Source changed or marker sequence already present: {name}')
        result = bytearray()
        end = 0
        for row in sorted(replacements, key=lambda item: item['offset']):
            start = row['offset']
            source = row['source'].encode('cp1252')
            if start < end or original[start:start + len(source)] != source:
                raise ValueError(f"Source occurrence changed: {row['resource']}")
            length, marker, translated = resource_span(row, original)
            result.extend(original[end:start] + marker)
            end = start + length
            entries.append((0, str(row['ordinal']).encode('ascii'), translated))
        result.extend(original[end:])
        if replacements[0]['kind'] == 'scenario' and len(result) != len(original):
            raise ValueError('Scenario DAT length changed')
        destination = payload / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(result)

    registry = payload / 'Avadon3Chinese' / 'resources.bin'
    registry.parent.mkdir()
    with registry.open('wb') as stream:
        stream.write(b'AV3LOC1\0' + struct.pack('<I', len(entries)))
        for address, source, target in entries:
            data = target.encode('utf-8')
            if not 0 < len(data) <= 16384 or not 0 < len(source) <= 16384:
                raise ValueError('Registry string length exceeds the runtime limit')
            stream.write(struct.pack('<III', address, len(source), len(data)))
            stream.write(source + data)
    return counts


def compile_adapter(build_dir, payload, shared, *, enhancement=False):
    compiler, dumpbin = shared.compiler()
    minhook = WORKSPACE / 'Tools/native/MinHook'
    actual = subprocess.check_output(['git', '-C', str(minhook), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != shared.MINHOOK_COMMIT:
        raise ValueError('MinHook differs from the reviewed dependency version')
    shared.BUILD = build_dir
    shared.forwarders(dumpbin)
    shutil.copy2(minhook / 'LICENSE.txt', payload / 'Avadon3Chinese/MinHook-LICENSE.txt')
    objects = build_dir / 'obj'
    objects.mkdir()

    def quote(path):
        return f'"{path}"'

    steps = [f'call {quote(compiler)} >nul']
    link = []
    for source in ('buffer', 'hook', 'trampoline', 'hde/hde32'):
        path = objects / (Path(source).name + '.obj')
        file = minhook / 'src' / (source + '.c')
        steps.append(f'cl /nologo /c /O2 /MT /TC /I{quote(minhook / "include")} {quote(file)} /Fo{quote(path)}')
        link.append(quote(path))
    includes = ' '.join('/I' + quote(path) for path in (minhook / 'include', build_dir, SHARED / 'native'))
    common = f'cl /nologo /std:c++17 /utf-8 /O2 /MT /EHsc {includes} ' + quote(REPACK / 'native/adapter.cpp')
    if enhancement:
        common += ' /DAV3_ENHANCEMENT'
    linked = ' '.join(link)
    steps.append(f'{common} /LD {linked} /Fo{quote(objects / "adapter.obj")} '
                 f'/link /DEF:{quote(build_dir / "winmm.def")} /OUT:{quote(payload / "winmm.dll")}')
    fixture = build_dir / 'offline_test.exe'
    steps.append(f'{common} /DAV3_OFFLINE_TEST {linked} /Fo{quote(objects / "fixture.obj")} '
                 f'/link /OUT:{quote(fixture)}')
    script = build_dir / 'compile.cmd'
    script.write_text('@echo off\n' + ''.join(step + '\nif errorlevel 1 exit /b 1\n' for step in steps), encoding='utf-8')
    result = subprocess.run(['cmd.exe', '/d', '/c', str(script)], cwd=build_dir, capture_output=True)
    if result.returncode:
        raise RuntimeError((result.stdout + result.stderr).decode('oem', errors='replace')[-6000:])
    subprocess.run([str(fixture), str(payload / 'Avadon3Chinese')], cwd=build_dir, check=True, timeout=30)


def build(output, smoke=False):
    output = output.resolve()
    game = (PROJECT / 'Game').resolve()
    if output.is_relative_to(game) or output.exists():
        raise ValueError('Expected a new output directory outside the Steam installation')
    executable = (game / EXE).read_bytes()
    if sha(executable) != EXE_HASH:
        raise ValueError('Unsupported executable')
    shared = shared_tools()
    for address, expected in PROLOGUES.items():
        offset = shared.pe_file_offset(executable, address)
        if executable[offset:offset + len(expected)] != expected:
            raise ValueError(f'Unexpected engine signature at {address:#x}')
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix='.avadon3-runtime-', dir=output.parent) as temporary:
        root = Path(temporary)
        if smoke:
            mapped = read_json(PROJECT / 'avadon-3_text.mapping.json')['records']
            source = column(PROJECT / 'avadon-3_text.csv')
            rows = [dict(row, source=value, translation=SMOKE_LABELS[value])
                    for row, value in zip(mapped, source, strict=True)
                    if value in SMOKE_LABELS and (row['kind'] == 'native' or row['file'].endswith('/textdocs.txt'))]
            if {row['source'] for row in rows} != SMOKE_LABELS.keys():
                raise ValueError('Expected smoke-test native labels are missing')
        else:
            staged = root / 'occurrences.json'
            stage(PROJECT, staged)
            rows = read_json(staged)['records']
        payload = root / 'payload'
        payload.mkdir()
        modes = write_registry(game, payload, rows, executable)
        compile_adapter(root, payload, shared)
        files = {path.relative_to(payload).as_posix(): sha(path.read_bytes())
                 for path in sorted(payload.rglob('*')) if path.is_file()}
        report = dict(status='smoke_test_only' if smoke else 'built', installed=False,
                      exe_sha256=EXE_HASH, translation_modes=modes, files=files)
        (payload / 'RUNTIME_MANIFEST.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        payload.rename(output)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--smoke', action='store_true', help='Build a small translation fixture for an isolated copy')
    args = parser.parse_args()
    print(json.dumps(build(args.out, args.smoke)))
