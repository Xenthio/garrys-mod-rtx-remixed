"""Exercise the real startup proxy against a fake renderer, never a GPU/game."""
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


def fixture(root, name, gma_path):
    dds = b'DDS ' + b'Q' * 124
    digest = lambda data: hashlib.sha256(data).hexdigest()
    prefix = f'data_static/astra/{name}/rtx/'
    sha = digest(dds)
    target = f'textures/{sha}.dds'
    layer = ('#usda 1.0\ndef Scope "Looks" {\n def Material "fixture" {\n'
             f' asset inputs:diffuse_texture = @./{target}@\n}}\n}}\n').encode()
    desc = {'path': prefix + 'mod.usda.dat', 'target': 'mod.usda', 'sha256': digest(layer), 'bytes': len(layer)}
    file = {'path': prefix + sha + '.dds.dat', 'target': target, 'sha256': sha, 'bytes': len(dds)}
    manifest = {'version': 1, 'map': name, 'generation': digest(name.encode()), 'files': [file], 'layer': desc}
    entries = {prefix + 'startup.json': json.dumps(manifest).encode(), desc['path']: layer, file['path']: dds}
    header = b'GMAD\x03' + struct.pack('<QQ', 0, 0) + b'\0fixture\0{}\0astra\0' + struct.pack('<I', 1)
    table = b''.join(struct.pack('<I', i) + name.encode() + b'\0' + struct.pack('<QI', len(data), zlib.crc32(data))
                     for i, (name, data) in enumerate(entries.items(), 1)) + b'\0' * 4
    data = header + table + b''.join(entries.values())
    gma_path.write_bytes(data + struct.pack('<I', zlib.crc32(data)))
    return manifest


def child(root, mode, contract):
    module = ctypes.WinDLL(str(root / 'bin/win64/d3d9.dll'), winmode=0x8)
    status = module.AstraStartupStatusJson
    status.restype = ctypes.c_char_p
    assert json.loads(status())['phase'] == 'pending'
    create = module.Direct3DCreate9
    create.argtypes = [ctypes.c_uint]
    create.restype = ctypes.c_void_p
    assert create(123) == 0x1000007b
    first = json.loads(status())
    renderer = ctypes.WinDLL(str(root / 'bin/win64/d3d9_astra_renderer.dll'), winmode=0x8)
    count = renderer.TestCallCount
    count.restype = ctypes.c_uint
    assert count() == 1
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    address = kernel.GetProcAddress
    address.argtypes = [wintypes.HMODULE, ctypes.c_void_p]
    address.restype = ctypes.c_void_p
    for item in json.loads(contract.read_text(encoding='utf-8'))['exports']:
        name = ctypes.cast(ctypes.c_char_p(item['name'].encode()), ctypes.c_void_p)
        via_name = address(module._handle, name)
        via_ordinal = address(module._handle, ctypes.c_void_p(item['ordinal']))
        assert via_name and via_name == via_ordinal, item
        if item['name'] not in ('Direct3DCreate9', 'Direct3DCreate9Ex'):
            assert via_name == address(renderer._handle, name), item
    create_ex = module.Direct3DCreate9Ex
    create_ex.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)]
    create_ex.restype = ctypes.c_long
    output = ctypes.c_void_p()
    assert create_ex(456, ctypes.byref(output)) == 0 and output.value == 0x200001c8
    assert count() == 2 and json.loads(status()) == first
    if mode == 'default':
        assert first['maps']['gm_fixture']['ready'], first
        assert first['source_selection'] == 'default_discovery', first
        generation = first['maps']['gm_fixture']['generation']
        disable = module.AstraDisableStartupMap
        disable.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        disable.restype = ctypes.c_bool
        owned = root / 'rtx-remix/mods/!astra_startup_gm_fixture/mod.usda'
        before = owned.read_bytes()
        assert not disable(b'gm_fixture', b'0' * 64) and owned.read_bytes() == before
        assert not disable(b'../gm_fixture', generation.encode())
        assert disable(b'gm_fixture', generation.encode())
        assert b'subLayers = []' in owned.read_bytes()
        assert disable(b'gm_fixture', generation.encode())
        assert not json.loads(status())['maps']['gm_fixture']['ready']
    elif mode == 'workshop':
        assert first['source_selection'] == 'default_discovery', first
        assert first['maps']['gm_workshop']['ready'] and first['maps']['gm_fixture']['ready'], first
    elif mode == 'noworkshop':
        assert first['maps']['gm_fixture']['ready'] and 'gm_workshop' not in first['maps'], first
    elif mode == 'noaddons':
        assert first['source_selection'] == 'noaddons' and first['maps'] == {}, first
    elif mode == 'explicit':
        assert first['source_selection'] == 'explicit_environment', first
        assert first['maps']['gm_other']['ready'] and 'gm_fixture' not in first['maps'], first
    elif mode == 'malformed':
        assert first['phase'] == 'failed' and first['maps'] == {}, first
    assert (root / 'rtx-remix/mods/!advanced_material_editor/mod.usda').read_bytes() == b'unchanged editor'
    print(json.dumps({'mode': mode, 'passed': True, 'forwarded_exports': 144,
                      'status': json.loads(status())}))


if '--child' in sys.argv:
    child(Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4]))
    sys.exit()

PROXY, FAKE, CONTRACT = map(lambda p: Path(p).resolve(), sys.argv[1:4])
del sys.argv[1:4]


class ProxyTests(unittest.TestCase):
    def run_child(self, mode):
        with tempfile.TemporaryDirectory(prefix='astra_proxy_') as directory:
            root = Path(directory)
            binary = root / 'bin/win64'
            binary.mkdir(parents=True)
            addons = root / 'garrysmod/addons'
            addons.mkdir(parents=True)
            editor = root / 'rtx-remix/mods/!advanced_material_editor/mod.usda'
            editor.parent.mkdir(parents=True)
            editor.write_bytes(b'unchanged editor')
            shutil.copy2(PROXY, binary/'d3d9.dll')
            shutil.copy2(FAKE, binary/'d3d9_astra_renderer.dll')
            fixture(root, 'gm_fixture', addons/'fixture.gma')
            fixture(root, 'gm_other', root/'external.gma')
            env = dict(os.environ)
            env.pop('ASTRA_RTX_STARTUP_SOURCES', None)
            env['ASTRA_RTX_STEAM_ROOT'] = str(root / 'steam_unavailable')
            env['ASTRA_RTX_STEAM_USER'] = '123'
            args = [sys.executable, __file__, '--child', str(root), mode, str(CONTRACT)]
            if mode in ('workshop', 'noworkshop'):
                steam = root / 'Steam'
                item = steam / 'steamapps/workshop/content/4000/3800000001/3800000001.gma'
                item.parent.mkdir(parents=True)
                fixture(root, 'gm_workshop', item)
                (steam / 'steamapps/appmanifest_4000.acf').write_text(
                    '"AppState" { "appid" "4000" "installdir" "GarrysMod RTX" }', encoding='utf-8')
                metadata = steam / 'steamapps/workshop/appworkshop_4000.acf'
                metadata.write_text('"AppWorkshop" { "appid" "4000" '
                    '"WorkshopItemsInstalled" { "3800000001" { "size" "1024" "manifest" "987" } } '
                    '"WorkshopItemDetails" { "3800000001" { "manifest" "987" "latest_manifest" "987" } } }',
                    encoding='utf-8')
                subscriptions = steam / 'userdata/123/ugc/4000_subscriptions.vdf'
                subscriptions.parent.mkdir(parents=True)
                subscriptions.write_text('"subscribedfiles" { "appid" "4000" "0" { '
                    '"publishedfileid" "3800000001" "disabled_locally" "0" } }', encoding='utf-8')
                env['ASTRA_RTX_STEAM_ROOT'] = str(steam)
                if mode == 'noworkshop':
                    args.append('-noworkshop')
            if mode in ('explicit', 'noaddons'):
                args.append('-noaddons')
            if mode == 'explicit':
                env['ASTRA_RTX_STARTUP_SOURCES'] = json.dumps([str(root/'external.gma')])
            elif mode == 'malformed':
                env['ASTRA_RTX_STARTUP_SOURCES'] = '{"not":"an array"}'
            completed = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertTrue(json.loads(completed.stdout)['passed'])

    def test_prepares_once_preserves_all_names_ordinals_and_scoped_disable(self):
        self.run_child('default')

    def test_noaddons_selects_no_archive_packages(self):
        self.run_child('noaddons')

    def test_explicit_child_sources_override_noaddons(self):
        self.run_child('explicit')

    def test_workshop_subscription_is_prepared_before_renderer_creation(self):
        self.run_child('workshop')

    def test_noworkshop_skips_subscription_but_preserves_local_addons(self):
        self.run_child('noworkshop')

    def test_malformed_environment_falls_back_without_blocking_renderer(self):
        self.run_child('malformed')


if __name__ == '__main__':
    unittest.main()
