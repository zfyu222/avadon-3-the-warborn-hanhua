"""Build, verify, and package the independent Steam Windows enhancement."""

import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, ZipFile

from build_runtime import EXE, EXE_HASH, PROJECT, REPACK, compile_adapter, shared_tools
from stage_text import read_json, sha as sha_bytes


VERSION = '1.2'
ARCHIVE = f'阿瓦登 3：战裔_体验优化{VERSION}.zip'
SETTINGS = 'Enhancement-Settings.exe'
DEFAULTS = '''[Font]
ScalePercent=150
FitLabels=1
[Settings]
Language=0
[Inventory]
ToggleHotkey=1
[Camera]
FollowCharacter=1
[Movement]
SpeedPercent=200
[Interaction]
HighlightOnShift=2
'''
GUIDE = '''阿瓦登 3：战裔 · 体验优化补丁 1.2

可选，推荐使用。适用于 Steam Windows 构建 4248987，需要先安装含字幕显示修复的子非鱼汉化。

安装
退出游戏，将压缩包解压到 Avadon 3.exe 所在的文件夹。双击 Install-Enhancement.cmd。
双击 Enhancement-Settings.exe 调整选项。保存后下次启动游戏生效，可直接从 Steam 启动。

功能
中文字号和探索移速支持 100%–200%，可开启短标签适配。镜头跟随一号角色；一号角色不在队伍中时跟随二号角色，战斗中受当前行动回合限制。探索加速沿用游戏碰撞规则。可互动物体标记可关闭、按住 Shift 显示或始终显示。三代原有的背包快捷键切换可在设置里关闭，Esc 保留原有功能。
首次安装的默认值为字号 150%、探索移速 200%、始终显示交互物标记，并开启短标签适配、背包快捷键关闭和镜头跟随。设置界面可跟随系统语言，也可手动切换简体中文或 English。

升级与卸载
重复安装保留已保存的体验优化设置。退出游戏后双击 Uninstall-Enhancement.cmd，可恢复基础汉化组件，并保留增强设置供下次安装使用。

v1.0：首次发布
v1.1：匹配修复后的汉化显示组件，并沿用现有增强设置。
v1.2：补齐伤害类型、地面效果、战斗按钮和典籍通知的中文显示。
'''


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha(path):
    return sha_bytes(path.read_bytes())


def check_layout(executable, shared):
    data = executable.read_bytes()
    layout = read_json(PROJECT / 'Analysis/enhancement-layout.json')
    if sha_bytes(data) != EXE_HASH or layout['exe_sha256'] != EXE_HASH:
        raise ValueError('Unexpected game executable')
    for hook in layout['hooks']:
        entry = int(hook['entry'], 16)
        offset = shared.pe_file_offset(data, entry)
        signature = bytes.fromhex(hook['signature'])
        if data[offset:offset + len(signature)] != signature:
            raise ValueError(f'Unexpected entry signature: {hook["feature"]}')
        for site in map(lambda address: int(address, 16), hook['calls']):
            start = shared.pe_file_offset(data, site)
            if data[start] != 0xe8 or site + 5 + struct.unpack_from('<i', data, start + 1)[0] != entry:
                raise ValueError(f'Unexpected call target at {site:#x}')
    return {'entries': len(layout['hooks']), 'call_sites': sum(len(row['calls']) for row in layout['hooks'])}


def run_installer(script, game, action='Install', *, success=True):
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                             '-File', str(script), '-Action', action, '-GameRoot', str(game)],
                            capture_output=True, timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
    if (result.returncode == 0) != success:
        raise RuntimeError((result.stdout + result.stderr).decode(errors='replace'))


def verify_installer(root, base, manifest):
    fixture = root.parent / '隔离验证 文件夹'
    (fixture / 'Avadon3Chinese').mkdir(parents=True)
    for name in (EXE, 'winmm.dll', 'Avadon3Chinese/resources.bin'):
        shutil.copy2(base / name, fixture / name)
    script = root / 'Enhancement/install.ps1'
    settings = fixture / 'Avadon3Chinese/enhancement.ini'
    installed = fixture / 'winmm.dll'
    run_installer(script, fixture)
    assert sha(installed) == manifest['enhanced_dll_sha256'] and sha(settings) == manifest['defaults_sha256']
    custom = b'[Font]\r\nScalePercent=125\r\nFitLabels=0\r\n[Movement]\r\nSpeedPercent=140\r\n'
    settings.write_bytes(custom)
    run_installer(script, fixture)
    assert settings.read_bytes() == custom
    run_installer(script, fixture, 'Uninstall')
    run_installer(script, fixture, 'Uninstall')
    assert sha(installed) == manifest['baseline_dll_sha256'] and settings.read_bytes() == custom
    registry = fixture / 'Avadon3Chinese/resources.bin'
    with registry.open('ab') as stream:
        stream.write(b'\x00')
    run_installer(script, fixture, success=False)
    assert sha(installed) == manifest['baseline_dll_sha256']
    return ['fresh_install', 'repeat_install_preserves_config', 'uninstall_restores_base_and_preserves_config',
            'uninstall_idempotent', 'unexpected_registry_rejected']


def build(base, output):
    base, output = base.resolve(strict=True), output.resolve()
    if output.is_relative_to((PROJECT / 'Game').resolve()) or output.exists():
        raise ValueError('Choose a new archive path outside the game directory')
    source = read_json(base / 'TEST_MANIFEST.json')
    if source['status'] != 'assembled_for_user_testing' or source['exe_sha256'] != EXE_HASH:
        raise ValueError('Expected the reviewed main localization build')
    files = source['runtime']['files']
    for name in ('winmm.dll', 'Avadon3Chinese/resources.bin'):
        if sha(base / name) != files[name]:
            raise ValueError(f'Base localization file changed: {name}')
    shared = shared_tools()
    verified = check_layout(base / EXE, shared)
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix='av3-enhancement-', dir=output.parent) as temporary:
        work = Path(temporary)
        build_dir, payload = work / 'build', work / 'payload'
        build_dir.mkdir()
        (payload / 'Avadon3Chinese').mkdir(parents=True)
        shutil.copy2(base / 'Avadon3Chinese/resources.bin', payload / 'Avadon3Chinese/resources.bin')
        compile_adapter(build_dir, payload, shared, enhancement=True)
        package = work / 'package'
        support = package / 'Enhancement'
        support.mkdir(parents=True)
        shutil.copy2(payload / 'winmm.dll', support / 'winmm.dll')
        shutil.copy2(payload / 'Avadon3Chinese/MinHook-LICENSE.txt', support / 'MinHook-LICENSE.txt')
        shutil.copy2(REPACK / 'enhancement.ps1', support / 'install.ps1')
        (support / 'defaults.ini').write_text(DEFAULTS, encoding='ascii', newline='\r\n')
        compiler = Path(os.environ['WINDIR']) / 'Microsoft.NET/Framework/v4.0.30319/csc.exe'
        settings = package / SETTINGS
        subprocess.run([str(compiler), '/nologo', '/target:winexe', '/optimize+', '/codepage:65001',
                        f'/out:{settings}', '/reference:System.Drawing.dll', '/reference:System.Windows.Forms.dll',
                        str(REPACK / 'EnhancementSettings.cs')],
                       check=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
        manifest = dict(version=VERSION, steam_build='4248987', game_sha256=EXE_HASH,
                        registry_sha256=files['Avadon3Chinese/resources.bin'], baseline_dll_sha256=files['winmm.dll'],
                        enhanced_dll_sha256=sha(support / 'winmm.dll'), defaults_sha256=sha(support / 'defaults.ini'),
                        settings_sha256=sha(settings))
        write_json(support / 'manifest.json', manifest)
        for action in ('Install', 'Uninstall'):
            (package / f'{action}-Enhancement.cmd').write_text(
                '@echo off\n'
                f'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Enhancement\\install.ps1" -Action {action}\n'
                'set "code=%errorlevel%"\npause\nexit /b %code%\n', encoding='ascii', newline='\r\n')
        (package / '体验优化说明.txt').write_text(GUIDE, encoding='utf-8-sig', newline='\r\n')
        cases = verify_installer(package, base, manifest)
        payload_files = {file.relative_to(package).as_posix(): sha(file)
                         for file in sorted(package.rglob('*')) if file.is_file()}
        (package / '体验优化_SHA256SUMS.txt').write_text(
            ''.join(f'{digest}  {name}\n' for name, digest in payload_files.items()), encoding='utf-8-sig')
        staged = work / ARCHIVE
        with ZipFile(staged, 'w', ZIP_DEFLATED) as archive:
            for file in sorted(package.rglob('*')):
                if file.is_file():
                    archive.write(file, file.relative_to(package).as_posix())
        with ZipFile(staged) as archive:
            if archive.testzip() or any(sha_bytes(archive.read(name)) != digest for name, digest in payload_files.items()):
                raise ValueError('Archive verification failed')
        staged.rename(output)
    return dict(status='verified', archive=output.name, archive_sha256=sha(output), layout=verified,
                installer_cases=cases, runtime_and_settings_built=True, game_launched=False, manifest=manifest)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, default=PROJECT / 'ImagePatch/display-fix-r2')
    parser.add_argument('--out', type=Path, default=PROJECT / 'Dispatch' / ARCHIVE)
    args = parser.parse_args()
    print(json.dumps(build(args.base, args.out), ensure_ascii=False))
