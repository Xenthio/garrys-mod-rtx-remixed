"""Native preparation contracts against synthetic GMAs; never launches a game."""
import hashlib
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

EXE = Path(sys.argv.pop(1)).resolve()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def gma(path, entries):
    header = b'GMAD\x03' + struct.pack('<QQ', 0, 0) + b'\0fixture\0{}\0astra\0' + struct.pack('<I', 1)
    table = b''.join(struct.pack('<I', index) + name.encode() + b'\0' +
                     struct.pack('<QI', len(data), zlib.crc32(data))
                     for index, (name, data) in enumerate(entries.items(), 1)) + b'\0' * 4
    data = header + table + b''.join(entries.values())
    path.write_bytes(data + struct.pack('<I', zlib.crc32(data)))


def package(name='gm_fixture', marker=b'A', hashes=None):
    # The scanner performs transport validation; native DDS parsing is tested
    # separately by the texture builder. This is a small opaque DDS fixture.
    dds = b'DDS ' + marker * 124
    sha = digest(dds)
    prefix = 'data_static/astra/' + name + '/rtx/'
    target = 'textures/' + sha + '.dds'
    layer = ('#usda 1.0\ndef Scope "Looks" {\n def Material "fixture" {\n'
             ' asset inputs:diffuse_texture = @./' + target + '@\n}\n}\n').encode()
    file = {'path': prefix + sha + '.dds.dat', 'target': target, 'sha256': sha, 'bytes': len(dds)}
    desc = {'path': prefix + 'mod.usda.dat', 'target': 'mod.usda', 'sha256': digest(layer), 'bytes': len(layer)}
    manifest = {'version': 1, 'map': name, 'generation': digest(marker + name.encode()), 'files': [file], 'layer': desc}
    if hashes is not None:
        manifest['hashes'] = hashes
    return manifest, {prefix + 'startup.json': json.dumps(manifest).encode(), desc['path']: layer, file['path']: dds}


def legacy_root():
    return (b'#usda 1.0\n(\n    customLayerData = {\n'
            b'        string lightspeed_game_name = "Garry\'s Mod (x64)"\n'
            b'        string lightspeed_layer_type = "replacement"\n'
            b'    }\n    metersPerUnit = 0.01\n    subLayers = [\n'
            b'        @./profiles/hash_ABCDEF0123456789_0123456789ABCDEF.usda@\n'
            b'    ]\n    timeCodesPerSecond = 24\n    upAxis = "Z"\n)\n')


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='astra_startup_native_')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.addons = self.root / 'garrysmod/addons'
        self.addons.mkdir(parents=True)
        self.mods = self.root / 'rtx-remix/mods'
        self.mods.mkdir(parents=True)
        self.editor = self.mods / '!advanced_material_editor/mod.usda'
        self.editor.parent.mkdir()
        self.editor.write_bytes(b'editor bytes remain unchanged')

    def run_prepare(self, *args):
        completed = subprocess.run([str(EXE), '--game-root', str(self.root), *map(str, args)],
                                   capture_output=True, text=True, timeout=20)
        self.assertIn(completed.returncode, (0, 1), completed.stdout + completed.stderr)
        report = json.loads(completed.stdout)
        self.assertEqual(self.editor.read_bytes(), b'editor bytes remain unchanged')
        self.assertEqual(json.loads((self.root/'garrysmod/data/astra/startup/status.json').read_text()), report)
        return report

    def output(self, name='gm_fixture'):
        return self.mods / ('!astra_startup_' + name)

    def directory_link(self, target, link):
        try:
            os.symlink(target, link, target_is_directory=True)
        except OSError:
            if os.name != 'nt':
                raise
            # Junction creation needs no symbolic-link privilege. Both paths
            # are explicit owned children of this test's temporary directory.
            self.assertTrue(target.resolve().is_relative_to(self.root.resolve()))
            self.assertTrue(link.resolve().is_relative_to(self.root.resolve()))
            result = subprocess.run(['cmd', '/c', 'mklink', '/J', str(link), str(target)],
                                    capture_output=True, text=True, timeout=10,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            if result.returncode:
                self.skipTest('Directory link capability unavailable: ' + result.stdout + result.stderr)

    def test_cold_then_warm_skips_archive_payload_and_preserves_layer_mtime(self):
        manifest, entries = package()
        entries = {'materials/unrelated.bin': b'x' * (16 * 1048576), **entries}
        gma(self.addons/'map.gma', entries)
        cold = self.run_prepare()
        self.assertTrue(cold['ready'], cold)
        self.assertLess(cold['bytes_read'], 65536)
        result = cold['maps']['gm_fixture']
        self.assertTrue(result['ready'])
        self.assertEqual(result['cache_hits'], 0)
        target = self.output()/manifest['files'][0]['target']
        self.assertEqual(digest(target.read_bytes()), manifest['files'][0]['sha256'])
        initial_mtime = (self.output()/'mod.usda').stat().st_mtime_ns
        warm = self.run_prepare()['maps']['gm_fixture']
        self.assertEqual(warm['cache_hits'], 2)
        self.assertEqual(warm['bytes_written'], 0)
        self.assertEqual((self.output()/'mod.usda').stat().st_mtime_ns, initial_mtime)

    def test_multivolume_union_and_missing_companion_deactivate_even_with_warm_cache(self):
        manifest, entries = package()
        file_path = manifest['files'][0]['path']
        gma(self.addons/'map_1.gma', {key: data for key, data in entries.items() if key != file_path})
        companion = self.addons/'map_2.gma'
        gma(companion, {file_path: entries[file_path]})
        self.assertTrue(self.run_prepare()['ready'])
        companion.unlink()
        result = self.run_prepare()['maps']['gm_fixture']
        self.assertFalse(result['ready'])
        self.assertTrue(result['deactivated'])
        self.assertIn('Missing package member', result['errors'][0])
        self.assertIn(b'subLayers = []', (self.output()/'mod.usda').read_bytes())

    def test_nested_multipart_cold_warm_and_missing_companion(self):
        manifest, entries = package()
        folder = self.addons/'astra_imported'
        folder.mkdir()
        asset = manifest['files'][0]['path']
        gma(folder/'gm_fixture.gma', {name: raw for name, raw in entries.items() if name != asset})
        companion = folder/'gm_fixture_content_001.gma'
        gma(companion, {asset: entries[asset]})
        cold = self.run_prepare()
        self.assertTrue(cold['ready'], cold)
        self.assertEqual(cold['maps']['gm_fixture']['cache_hits'], 0)
        self.assertEqual((self.output()/manifest['files'][0]['target']).read_bytes(), entries[asset])
        layer = self.output()/'mod.usda'
        initial_mtime = layer.stat().st_mtime_ns
        warm = self.run_prepare()['maps']['gm_fixture']
        self.assertTrue(warm['ready'], warm)
        self.assertEqual(warm['cache_hits'], 2)
        self.assertEqual(warm['bytes_written'], 0)
        self.assertEqual(layer.stat().st_mtime_ns, initial_mtime)
        companion.unlink()
        missing = self.run_prepare()['maps']['gm_fixture']
        self.assertFalse(missing['ready'])
        self.assertTrue(missing['deactivated'])
        self.assertIn('Missing package member', missing['errors'][0])
        self.assertIn(b'subLayers = []', layer.read_bytes())

    def test_explicit_folder_includes_loose_data_and_deduplicates_explicit_gma(self):
        folder = self.addons/'astra_imported'
        folder.mkdir()
        _, entries = package()
        archive = folder/'gm_fixture.gma'
        gma(archive, entries)
        for target_root, name in ((folder, 'gm_loose'), (self.root/'garrysmod', 'gm_game')):
            _, loose = package(name)
            for relative, raw in loose.items():
                target = target_root/relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(raw)
        baseline = self.run_prepare('--source', folder)
        self.assertTrue(baseline['ready'], baseline)
        self.assertEqual(set(baseline['maps']), {'gm_fixture', 'gm_loose', 'gm_game'})
        repeated = self.run_prepare('--source', folder, '--source', archive,
                                    '--source', folder/'..'/'astra_imported')
        self.assertTrue(repeated['ready'], repeated)
        self.assertEqual(repeated['table_bytes_read'], baseline['table_bytes_read'])
        self.assertEqual(set(repeated['maps']), set(baseline['maps']))
        no_addons = self.run_prepare('--no-addons')
        self.assertTrue(no_addons['maps']['gm_game']['ready'])
        self.assertTrue(no_addons['maps']['gm_fixture']['removed'])
        self.assertTrue(no_addons['maps']['gm_loose']['removed'])

    def test_nested_discovery_does_not_recurse_or_scan_game_root_archives(self):
        targets = [(self.addons/'astra_imported/deeper/test.gma', 'gm_deep'),
                   (self.root/'garrysmod/cache/workshop/test.gma', 'gm_cache'),
                   (self.root/'garrysmod/test.gma', 'gm_root')]
        for path, name in targets:
            path.parent.mkdir(parents=True, exist_ok=True)
            _, entries = package(name)
            gma(path, entries)
        result = self.run_prepare()
        self.assertTrue(result['ready'], result)
        self.assertEqual(result['maps'], {})
        # An explicit physical GMA retains its original meaning regardless of
        # where it lives; only automatic directory expansion is depth-bounded.
        explicit = self.run_prepare('--source', targets[0][0])
        self.assertTrue(explicit['maps']['gm_deep']['ready'])

    def test_nested_reparse_candidate_is_not_followed_and_missing_part_fails(self):
        folder = self.addons/'astra_imported'
        folder.mkdir()
        manifest, entries = package()
        asset = manifest['files'][0]['path']
        gma(folder/'gm_fixture.gma', {name: raw for name, raw in entries.items() if name != asset})
        companion = folder/'gm_fixture_content_001.gma'
        gma(companion, {asset: entries[asset]})
        self.assertTrue(self.run_prepare()['maps']['gm_fixture']['ready'])
        outside = self.root/'outside_addon'
        outside.mkdir()
        actual = outside/'real.gma'
        companion.replace(actual)
        try:
            os.symlink(actual, companion)
        except OSError:
            # A directory junction also carries the Windows reparse attribute;
            # checking it before file classification must produce the same refusal.
            self.directory_link(outside, companion)
        before = actual.read_bytes()
        result = self.run_prepare()
        self.assertEqual(actual.read_bytes(), before)
        self.assertTrue(any('Reparse points' in w['error'] for w in result['warnings']))
        self.assertFalse(result['maps']['gm_fixture']['ready'])
        self.assertTrue(result['maps']['gm_fixture']['deactivated'])
        # A linked explicit addon root is an error, not an invitation to scan.
        linked_root = self.root/'linked_explicit'
        self.directory_link(folder, linked_root)
        explicit = self.run_prepare('--source', linked_root)
        self.assertFalse(explicit['ready'])
        self.assertTrue(any('Reparse points' in e['error'] for e in explicit['errors']))

    def test_expanded_source_limit_aborts_before_publication(self):
        folder = self.addons/'astra_imported'
        folder.mkdir()
        # The addon directory itself plus 4096 distinct children exceeds the
        # shared 4096-source budget. Empty files prove scanning never starts.
        for index in range(4096):
            (folder/f'part_{index:04}.gma').touch()
        result = subprocess.run([str(EXE), '--game-root', str(self.root)],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('Too many addon sources', result.stderr)
        self.assertNotIn('Unsupported GMA', result.stderr)
        self.assertFalse((self.root/'garrysmod/data/astra/startup/status.json').exists())
        self.assertEqual(self.editor.read_bytes(), b'editor bytes remain unchanged')

    def test_changed_output_is_reverified_and_restored(self):
        manifest, entries = package()
        gma(self.addons/'map.gma', entries)
        self.assertTrue(self.run_prepare()['ready'])
        output = self.output()/manifest['files'][0]['target']
        output.write_bytes(b'Q' * output.stat().st_size)
        result = self.run_prepare()['maps']['gm_fixture']
        self.assertTrue(result['ready'])
        self.assertEqual(result['bytes_written'], len(entries[manifest['files'][0]['path']]))
        self.assertEqual(digest(output.read_bytes()), manifest['files'][0]['sha256'])

    def test_truncated_archive_after_success_deactivates_removed_root(self):
        _, entries = package()
        path = self.addons/'map.gma'
        gma(path, entries)
        self.assertTrue(self.run_prepare()['ready'])
        path.write_bytes(path.read_bytes()[:-100])
        result = self.run_prepare()
        self.assertFalse(result['ready'])
        self.assertTrue(result['maps']['gm_fixture']['removed'])
        self.assertTrue(result['maps']['gm_fixture']['deactivated'])

    def test_asset_hash_failure_does_not_publish_partial_layer(self):
        manifest, entries = package()
        entries[manifest['files'][0]['path']] = b'DDS ' + b'Q' * 124
        gma(self.addons/'map.gma', entries)
        result = self.run_prepare()['maps']['gm_fixture']
        self.assertFalse(result['ready'])
        self.assertIn('SHA256 mismatch', result['errors'][0])
        self.assertFalse((self.output()/manifest['files'][0]['target']).exists())
        self.assertEqual(list(self.output().rglob('*.astra_tmp_*')), [])

    def test_conflicting_virtual_paths_fail_instead_of_mount_order_guess(self):
        manifest, entries = package()
        gma(self.addons/'map.gma', entries)
        gma(self.addons/'conflict.gma', {manifest['files'][0]['path']: b'changed bytes'})
        result = self.run_prepare()['maps']['gm_fixture']
        self.assertFalse(result['ready'])
        self.assertIn('Conflicting package member', result['errors'][0])

    def test_identical_crc_companion_duplicates_are_accepted(self):
        manifest, entries = package()
        gma(self.addons/'map.gma', entries)
        path = manifest['files'][0]['path']
        gma(self.addons/'copy.gma', {path: entries[path]})
        self.assertTrue(self.run_prepare()['ready'])

    def test_cross_map_material_hash_collision_deactivates_both(self):
        for name in ('gm_first', 'gm_second'):
            _, entries = package(name, name.encode(), hashes=['ABCDEF0123456789'])
            gma(self.addons/(name+'.gma'), entries)
        result = self.run_prepare()
        self.assertFalse(result['ready'])
        for name in ('gm_first', 'gm_second'):
            self.assertFalse(result['maps'][name]['ready'])
            self.assertIn('Material hash overlaps', result['maps'][name]['errors'][0])

    def test_layer_cannot_reference_unlisted_external_file(self):
        manifest, entries = package()
        path = manifest['layer']['path']
        entries[path] = b'#usda 1.0\ndef Material "bad" { asset x = @../../outside.dds@ }\n'
        manifest['layer'].update(bytes=len(entries[path]), sha256=digest(entries[path]))
        entries[next(key for key in entries if key.endswith('startup.json'))] = json.dumps(manifest).encode()
        gma(self.addons/'map.gma', entries)
        result = self.run_prepare()['maps']['gm_fixture']
        self.assertFalse(result['ready'])
        self.assertIn('undeclared asset', result['errors'][0])

    def test_real_importer_layer_with_builtin_mdl_is_accepted(self):
        manifest, entries = package(hashes=['ABCDEF0123456789'])
        fixture = Path(__file__).parent / 'fixtures/importer_layer.usda'
        layer = fixture.read_bytes()
        provenance = json.loads(fixture.with_name('importer_layer_provenance.json').read_text())
        self.assertEqual(digest(layer), provenance['layer_sha256'])
        path = manifest['layer']['path']
        entries[path] = layer
        manifest['layer'].update(bytes=len(layer), sha256=digest(layer))
        entries[next(key for key in entries if key.endswith('startup.json'))] = json.dumps(manifest).encode()
        gma(self.addons/'map.gma', entries)
        result = self.run_prepare()['maps']['gm_fixture']
        self.assertTrue(result['ready'], result)

    def test_mdl_name_must_match_builtin_exactly(self):
        manifest, entries = package()
        path = manifest['layer']['path']
        entries[path] = b'#usda 1.0\ndef Material "bad" { asset x = @./AperturePBR_Opacity.mdl@ }\n'
        # A relative MDL path is an addon-provided file, not the exact built-in.
        manifest['layer'].update(bytes=len(entries[path]), sha256=digest(entries[path]))
        entries[next(key for key in entries if key.endswith('startup.json'))] = json.dumps(manifest).encode()
        gma(self.addons/'map.gma', entries)
        result = self.run_prepare()['maps']['gm_fixture']
        self.assertFalse(result['ready'])

    def test_manifest_path_traversal_rejected_without_outside_write(self):
        manifest, entries = package()
        manifest['files'][0]['target'] = '../outside.dds'
        entries[next(key for key in entries if key.endswith('startup.json'))] = json.dumps(manifest).encode()
        gma(self.addons/'map.gma', entries)
        result = self.run_prepare()['maps']['gm_fixture']
        self.assertFalse(result['ready'])
        self.assertFalse((self.mods/'outside.dds').exists())

    def test_loose_addon_explicit_sources_and_game_data_static(self):
        _, entries = package('gm_loose')
        loose = self.root/'loose_addon'
        for name, data in entries.items():
            path = loose/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        result = self.run_prepare('--source', loose)
        self.assertTrue(result['maps']['gm_loose']['ready'])
        result = self.run_prepare('--no-addons')
        self.assertTrue(result['maps']['gm_loose']['removed'])

    def test_reparse_output_is_rejected_without_following_it(self):
        _, entries = package()
        gma(self.addons/'map.gma', entries)
        elsewhere = self.root/'not_owned'
        elsewhere.mkdir()
        marker = elsewhere/'mod.usda'
        marker.write_bytes(b'must not change')
        self.directory_link(elsewhere, self.output())
        result = self.run_prepare()['maps']['gm_fixture']
        self.assertFalse(result['ready'])
        self.assertIn('Reparse points', result['errors'][0])
        self.assertEqual(marker.read_bytes(), b'must not change')

    def test_unrelated_linked_addon_is_skipped_without_aborting_valid_map(self):
        _, entries = package()
        gma(self.addons/'map.gma', entries)
        target = self.root/'unrelated_addon'
        target.mkdir()
        self.directory_link(target, self.addons/'linked_other_addon')
        result = self.run_prepare()
        self.assertTrue(result['ready'], result)
        self.assertTrue(result['maps']['gm_fixture']['ready'])
        self.assertEqual(len(result['warnings']), 1)
        self.assertIn('Reparse points', result['warnings'][0]['error'])

    def test_stale_legacy_root_backup_then_empty_without_warm_mtime_changes(self):
        _, entries = package()
        gma(self.addons/'map.gma', entries)
        root = self.mods/'!astra_map_importer/mod.usda'
        root.parent.mkdir()
        old = legacy_root().replace(b'\n', b'\r\n')
        root.write_bytes(old)
        profile = root.parent/'profiles/untouched.usda'
        profile.parent.mkdir()
        profile.write_bytes(b'profile preserved')
        first = self.run_prepare()
        self.assertTrue(first['ready'], first)
        migration = first['legacy_migration']
        self.assertTrue(migration['changed'])
        self.assertEqual(migration['before_sha256'], digest(old))
        backup = Path(migration['backup'])
        self.assertEqual(backup.read_bytes(), old)
        self.assertEqual(migration['backup_sha256'], digest(backup.read_bytes()))
        self.assertEqual(migration['after_sha256'], digest(root.read_bytes()))
        root_time, backup_time = root.stat().st_mtime_ns, backup.stat().st_mtime_ns
        second = self.run_prepare()
        self.assertTrue(second['ready'])
        self.assertFalse(second['legacy_migration']['changed'])
        self.assertEqual(root.stat().st_mtime_ns, root_time)
        self.assertEqual(backup.stat().st_mtime_ns, backup_time)
        self.assertEqual(profile.read_bytes(), b'profile preserved')

    def test_real_never_launched_helper_bootstrap_is_already_empty(self):
        _, entries = package()
        gma(self.addons/'map.gma', entries)
        root = self.mods/'!astra_map_importer/mod.usda'
        root.parent.mkdir()
        old = (Path(__file__).parent/'fixtures/importer_bootstrap_empty.usda').read_bytes()
        root.write_bytes(old)
        mtime = root.stat().st_mtime_ns
        report = self.run_prepare()
        self.assertTrue(report['ready'], report)
        self.assertFalse(report['legacy_migration']['changed'])
        self.assertEqual(root.read_bytes(), old)
        self.assertEqual(root.stat().st_mtime_ns, mtime)

    def test_long_output_paths_and_atomic_suffix_exceeding_max_path(self):
        long_root = self.root/('deep_' + 'a'*80)/('game_' + 'b'*60)
        (long_root/'garrysmod').mkdir(parents=True)
        manifest, entries = package()
        archive = self.addons/'map.gma'
        gma(archive, entries)
        output = long_root/'rtx-remix/mods/!astra_startup_gm_fixture'/manifest['files'][0]['target']
        self.assertGreater(len(str(output)), 260)
        result = subprocess.run([str(EXE), '--game-root', str(long_root), '--source', str(archive)],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(json.loads(result.stdout)['ready'])
        self.assertEqual(digest(output.read_bytes()), manifest['files'][0]['sha256'])

    def test_unicode_game_and_archive_paths_preserve_utf8_report(self):
        unicode_root = self.root/'测试游戏_é'
        (unicode_root/'garrysmod').mkdir(parents=True)
        manifest, entries = package()
        archive = self.addons/'地图资料_é.gma'
        gma(archive, entries)
        result = subprocess.run([str(EXE), '--game-root', str(unicode_root), '--source', str(archive)],
                                capture_output=True, text=True, encoding='utf8', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report['ready'], report)
        self.assertEqual(Path(report['game_root']), unicode_root)
        self.assertTrue(Path(report['maps']['gm_fixture']['mod_directory']).is_dir())
        output = unicode_root/'rtx-remix/mods/!astra_startup_gm_fixture'/manifest['files'][0]['target']
        self.assertEqual(digest(output.read_bytes()), manifest['files'][0]['sha256'])

    def test_unrecognized_legacy_root_preserved_and_startup_maps_fail_closed(self):
        _, entries = package()
        gma(self.addons/'map.gma', entries)
        root = self.mods/'!astra_map_importer/mod.usda'
        root.parent.mkdir()
        root.write_bytes(b'#usda 1.0\ndef Scope "custom_unknown_content" {}\n')
        old = root.read_bytes()
        report = self.run_prepare()
        self.assertFalse(report['ready'])
        self.assertFalse(report['maps']['gm_fixture']['ready'])
        self.assertIn('unexpected structure', report['legacy_migration']['error'])
        self.assertEqual(root.read_bytes(), old)
        self.assertTrue(report['maps']['gm_fixture']['deactivated'])

    def test_legacy_cleanup_failure_preserves_backup_and_blocks_startup_activation(self):
        _, entries = package()
        gma(self.addons/'map.gma', entries)
        root = self.mods/'!astra_map_importer/mod.usda'
        root.parent.mkdir()
        old = legacy_root()
        root.write_bytes(old)
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        handle = kernel.CreateFileW(str(root), 0x80000000, 0x3, None, 3, 0x80, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            report = self.run_prepare()
        finally:
            kernel.CloseHandle(handle)
        self.assertFalse(report['ready'])
        self.assertFalse(report['maps']['gm_fixture']['ready'])
        migration = report['legacy_migration']
        self.assertIn('publication failed', migration['error'])
        self.assertEqual(Path(migration['backup']).read_bytes(), old)
        self.assertEqual(root.read_bytes(), old)
        self.assertTrue(report['maps']['gm_fixture']['deactivated'])

    def test_existing_immutable_legacy_backup_is_never_overwritten(self):
        _, entries = package()
        gma(self.addons/'map.gma', entries)
        root = self.mods/'!astra_map_importer/mod.usda'
        root.parent.mkdir()
        old = legacy_root()
        root.write_bytes(old)
        backup = self.root/'garrysmod/data/astra/startup/legacy_roots'/(digest(old)+'.usda.dat')
        backup.parent.mkdir(parents=True)
        backup.write_bytes(b'conflicting preserved backup')
        report = self.run_prepare()
        self.assertFalse(report['ready'])
        self.assertIn('Immutable legacy root backup differs', report['legacy_migration']['error'])
        self.assertEqual(backup.read_bytes(), b'conflicting preserved backup')
        self.assertEqual(root.read_bytes(), old)


if __name__ == '__main__':
    unittest.main(verbosity=2)
