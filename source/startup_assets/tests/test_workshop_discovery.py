"""Discover installed Steam Workshop GMAs in isolated fake Steam libraries.

Only the native preparation executable is launched. No live Steam configuration,
registry state, game installation, subscribed item, or published asset is edited.
"""
import hashlib
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


def write_gma(path, entries):
    path.parent.mkdir(parents=True, exist_ok=True)
    header = b'GMAD\x03' + struct.pack('<QQ', 0, 0) + b'\0fixture\0{}\0astra\0' + struct.pack('<I', 1)
    table = b''.join(struct.pack('<I', index) + name.encode() + b'\0' +
                     struct.pack('<QI', len(data), zlib.crc32(data))
                     for index, (name, data) in enumerate(entries.items(), 1)) + b'\0' * 4
    data = header + table + b''.join(entries.values())
    path.write_bytes(data + struct.pack('<I', zlib.crc32(data)))
    return path


def package(name='gm_workshop_fixture', marker=b'W'):
    dds = b'DDS ' + marker * 124
    sha = digest(dds)
    prefix = f'data_static/astra/{name}/rtx/'
    target = f'textures/{sha}.dds'
    layer = ('#usda 1.0\ndef Scope "Looks" {\n def Material "fixture" {\n'
             f' asset inputs:diffuse_texture = @./{target}@\n}}\n}}\n').encode()
    asset = {'path': prefix + sha + '.dds.dat', 'target': target, 'sha256': sha, 'bytes': len(dds)}
    desc = {'path': prefix + 'mod.usda.dat', 'target': 'mod.usda',
            'sha256': digest(layer), 'bytes': len(layer)}
    manifest = {'version': 1, 'map': name, 'generation': digest(marker + name.encode()),
                'files': [asset], 'layer': desc}
    return manifest, {prefix + 'startup.json': json.dumps(manifest).encode(),
                      desc['path']: layer, asset['path']: dds}


def kv_quote(text):
    return '"' + str(text).replace('\\', '\\\\').replace('"', '\\"') + '"'


def kv_object(entries, depth=0):
    indent = '\t' * depth
    text = []
    for key, value in entries.items():
        if isinstance(value, dict):
            text.extend([indent + kv_quote(key), indent + '{', kv_object(value, depth + 1), indent + '}'])
        else:
            text.append(indent + kv_quote(key) + '\t\t' + kv_quote(value))
    return '\n'.join(text)


class WorkshopDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='astra_workshop_native_')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.steam = self.root / 'Steam'
        self.library = self.steam
        self.account = '123'
        self.game = self.library / 'steamapps/common/GarrysMod RTX'
        (self.game / 'garrysmod/addons').mkdir(parents=True)
        self.mods = self.game / 'rtx-remix/mods'
        self.mods.mkdir(parents=True)
        self.unrelated = self.mods / 'unrelated_mod/mod.usda'
        self.unrelated.parent.mkdir()
        self.unrelated.write_bytes(b'unrelated mod remains unchanged')
        self.write_game_manifest(self.library)

    def write_game_manifest(self, library):
        path = library / 'steamapps/appmanifest_4000.acf'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(kv_object({'AppState': {'appid': '4000', 'name': "Garry's Mod",
            'StateFlags': '4', 'installdir': 'GarrysMod RTX'}}), encoding='utf-8')

    def write_workshop_manifest(self, ids, library=None, details_only=()):
        library = library or self.library
        path = library / 'steamapps/workshop/appworkshop_4000.acf'
        path.parent.mkdir(parents=True, exist_ok=True)
        records = {str(item): {'manifest': str(900000000000000000 + int(item)),
            'size': '2048', 'timeupdated': '1788800000'} for item in ids}
        details = {str(item): {'manifest': str(900000000000000000 + int(item)), 'latest_manifest': str(900000000000000000 + int(item)), 'timetouched': '1788800000'}
                   for item in (*ids, *details_only)}
        path.write_text(kv_object({'AppWorkshop': {'appid': '4000', 'SizeOnDisk': '4096',
            'NeedsUpdate': '0', 'NeedsDownload': '0', 'TimeLastUpdated': '1788800000',
            'WorkshopItemsInstalled': records, 'WorkshopItemDetails': details}}), encoding='utf-8')
        self.write_subscriptions(ids)
        return path

    def write_subscriptions(self, ids, account=None, disabled=()):
        account = account or self.account
        path = self.steam / f'userdata/{account}/ugc/4000_subscriptions.vdf'
        path.parent.mkdir(parents=True, exist_ok=True)
        records = {str(index): {'publishedfileid': str(item), 'time_subscribed': '1788700000',
            'disabled_locally': '1' if str(item) in set(map(str, disabled)) else '0'}
            for index, item in enumerate(ids)}
        path.write_text(kv_object({'subscribedfiles': {'appid': '4000', **records}}), encoding='utf-8')
        return path

    def workshop_gma(self, item_id, name='gm_workshop_fixture', library=None, entries=None):
        library = library or self.library
        if entries is None:
            _, entries = package(name)
        path = library / f'steamapps/workshop/content/4000/{item_id}/{item_id}.gma'
        return write_gma(path, entries)

    def run_prepare(self, *args):
        account_args = ['--steam-user', self.account] if self.account else []
        environment = dict(os.environ)
        environment.pop('ASTRA_RTX_STEAM_USER', None)
        completed = subprocess.run([str(EXE), '--game-root', str(self.game), '--steam-root', str(self.steam),
             *account_args, *map(str, args)],
            capture_output=True, text=True, timeout=20, env=environment)
        self.assertIn(completed.returncode, (0, 1), completed.stdout + completed.stderr)
        report = json.loads(completed.stdout)
        self.assertEqual(self.unrelated.read_bytes(), b'unrelated mod remains unchanged')
        self.assertEqual(json.loads((self.game / 'garrysmod/data/astra/startup/status.json').read_text()), report)
        return report

    def output(self, name='gm_workshop_fixture'):
        return self.mods / ('!astra_startup_' + name)


    def test_installed_workshop_gma_cold_then_warm(self):
        manifest, entries = package()
        archive = self.workshop_gma('3800000001', entries=entries)
        self.write_workshop_manifest(['3800000001'])
        original_archive = archive.read_bytes()
        cold = self.run_prepare()
        self.assertTrue(cold['ready'], cold)
        self.assertIn('gm_workshop_fixture', cold['maps'], cold)
        result = cold['maps']['gm_workshop_fixture']
        self.assertTrue(result['ready'], result)
        self.assertEqual(result['generation'], manifest['generation'])
        self.assertEqual(result['assets'], 1)
        self.assertEqual(result['cache_hits'], 0)
        layer = self.output() / 'mod.usda'
        initial_mtime = layer.stat().st_mtime_ns
        target = self.output() / manifest['files'][0]['target']
        self.assertEqual(digest(target.read_bytes()), manifest['files'][0]['sha256'])
        warm = self.run_prepare()['maps']['gm_workshop_fixture']
        self.assertTrue(warm['ready'], warm)
        self.assertEqual(warm['cache_hits'], 2)
        self.assertEqual(warm['bytes_written'], 0)
        self.assertEqual(layer.stat().st_mtime_ns, initial_mtime)
        self.assertEqual(archive.read_bytes(), original_archive)

    def test_no_addons_and_explicit_sources_replace_workshop_discovery(self):
        self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        no_addons = self.run_prepare('--no-addons')
        self.assertTrue(no_addons['ready'], no_addons)
        self.assertEqual(no_addons['maps'], {})
        explicit = write_gma(self.root / 'explicit.gma', package('gm_explicit')[1])
        selected = self.run_prepare('--source', explicit)
        self.assertTrue(selected['ready'], selected)
        self.assertEqual(set(selected['maps']), {'gm_explicit'})

    def test_companion_assets_can_live_in_second_installed_item(self):
        manifest, entries = package()
        asset = manifest['files'][0]['path']
        self.workshop_gma('3800000001', entries={key: raw for key, raw in entries.items() if key != asset})
        companion = self.workshop_gma('3800000002', entries={asset: entries[asset]})
        self.write_workshop_manifest(['3800000001', '3800000002'])
        cold = self.run_prepare()
        self.assertIn('gm_workshop_fixture', cold['maps'], cold)
        self.assertTrue(cold['maps']['gm_workshop_fixture']['ready'], cold)
        self.assertEqual(self.run_prepare()['maps']['gm_workshop_fixture']['cache_hits'], 2)
        companion.unlink()
        missing = self.run_prepare()['maps']['gm_workshop_fixture']
        self.assertFalse(missing['ready'], missing)
        self.assertTrue(missing['deactivated'], missing)
        self.assertIn('Missing package member', missing['errors'][0])
        self.assertIn(b'subLayers = []', (self.output() / 'mod.usda').read_bytes())


    def ready_names(self, report):
        return {name for name, state in report['maps'].items()
                if state.get('ready') and not state.get('removed')}

    def write_libraries(self, libraries):
        path = self.steam / 'steamapps/libraryfolders.vdf'
        path.parent.mkdir(parents=True, exist_ok=True)
        entries = {str(index): {'path': str(library), 'label': '', 'contentid': str(index + 10),
            'apps': {'4000': '100000'}} for index, library in enumerate(libraries)}
        path.write_text(kv_object({'libraryfolders': entries}), encoding='utf-8')
        return path

    def test_cached_unsubscribed_items_and_other_accounts_are_not_mounted(self):
        self.workshop_gma('3800000001', 'gm_current')
        self.workshop_gma('3800000002', 'gm_other_account')
        self.workshop_gma('3800000003', 'gm_retained_cache')
        self.write_workshop_manifest(['3800000001', '3800000002', '3800000003'])
        self.write_subscriptions(['3800000001'])
        self.write_subscriptions(['3800000002'], account='456')
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), {'gm_current'}, selected)
        self.account = '456'
        switched = self.run_prepare()
        self.assertEqual(self.ready_names(switched), {'gm_other_account'}, switched)
        self.assertTrue(switched['maps']['gm_current']['removed'], switched)

    def test_steam_and_gmod_disabled_items_are_excluded(self):
        for item, name in [('3800000001', 'gm_enabled'), ('3800000002', 'gm_steam_disabled'),
                           ('3800000003', 'gm_gmod_disabled')]:
            self.workshop_gma(item, name)
        ids = ['3800000001', '3800000002', '3800000003']
        self.write_workshop_manifest(ids)
        self.write_subscriptions(ids, disabled=['3800000002'])
        disabled = self.game / 'garrysmod/cfg/addonnomount.txt'
        disabled.parent.mkdir(parents=True)
        disabled.write_text(kv_object({'addonnomount': {'1': '3800000003'}}), encoding='utf-8')
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), {'gm_enabled'}, selected)

    def test_unsubscribe_deactivates_warm_output_without_deleting_cached_gma(self):
        archive = self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        self.assertIn('gm_workshop_fixture', self.ready_names(self.run_prepare()))
        original = archive.read_bytes()
        self.write_subscriptions([])
        removed = self.run_prepare()
        self.assertEqual(self.ready_names(removed), set(), removed)
        self.assertTrue(removed['maps']['gm_workshop_fixture']['removed'], removed)
        self.assertIn(b'subLayers = []', (self.output() / 'mod.usda').read_bytes())
        self.assertEqual(archive.read_bytes(), original)

    def test_no_workshop_preserves_local_addon_loading(self):
        self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        write_gma(self.game / 'garrysmod/addons/local.gma', package('gm_local')[1])
        selected = self.run_prepare('--no-workshop')
        self.assertEqual(self.ready_names(selected), {'gm_local'}, selected)

    def test_secondary_library_metadata_and_duplicate_paths(self):
        second = self.root / 'Other Steam Library'
        libraries = self.write_libraries([self.steam, second, second / '.'])
        duplicated_metadata_bytes = libraries.stat().st_size
        archive = self.workshop_gma('3800000001', library=second)
        self.write_workshop_manifest(['3800000001'], library=second)
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), {'gm_workshop_fixture'}, selected)
        baseline_table_bytes = selected['table_bytes_read']
        self.write_libraries([self.steam, second])
        single = self.run_prepare()
        self.assertEqual(single['table_bytes_read'] - libraries.stat().st_size,
                         baseline_table_bytes - duplicated_metadata_bytes)
        self.assertEqual(single['maps']['gm_workshop_fixture']['cache_hits'], 2)
        self.assertTrue(archive.exists())

    def test_missing_subscription_file_never_mounts_installed_cache(self):
        self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        (self.steam / f'userdata/{self.account}/ugc/4000_subscriptions.vdf').unlink()
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)

    def test_details_only_and_mismatched_latest_manifest_are_not_complete_installs(self):
        self.workshop_gma('3800000001', 'gm_stale')
        self.workshop_gma('3800000002', 'gm_details_only')
        metadata = self.write_workshop_manifest(['3800000001'], details_only=['3800000002'])
        self.write_subscriptions(['3800000001', '3800000002'])
        raw = metadata.read_text(encoding='utf-8')
        # Alter only latest_manifest; the installed and selected manifest agree.
        expected = str(900000000000000000 + 3800000001)
        raw = raw.replace('"latest_manifest"\t\t"' + expected + '"',
                          '"latest_manifest"\t\t"999999999"')
        metadata.write_text(raw, encoding='utf-8')
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)

    def test_malformed_subscription_metadata_fails_closed(self):
        self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        subscription = self.steam / f'userdata/{self.account}/ugc/4000_subscriptions.vdf'
        subscription.write_text('"subscribedfiles" { "appid" "4000" "0" {', encoding='utf-8')
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)

    def test_nested_workshop_archive_is_not_recursively_discovered(self):
        manifest, entries = package()
        item = self.library / 'steamapps/workshop/content/4000/3800000001'
        write_gma(item / 'deeper/hidden.gma', entries)
        self.write_workshop_manifest(['3800000001'])
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)


    def test_unknown_steam_disable_state_is_not_implicitly_enabled(self):
        self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        subscription = self.steam / f'userdata/{self.account}/ugc/4000_subscriptions.vdf'
        subscription.write_text(kv_object({'subscribedfiles': {'appid': '4000',
            '0': {'publishedfileid': '3800000001', 'time_subscribed': '1788700000'}}}), encoding='utf-8')
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)

    def test_linked_workshop_item_is_not_followed(self):
        actual = self.root / 'outside_workshop'
        write_gma(actual / 'unexpected.gma', package()[1])
        self.write_workshop_manifest(['3800000001'])
        link = self.library / 'steamapps/workshop/content/4000/3800000001'
        link.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.symlink(actual, link, target_is_directory=True)
        except OSError:
            if os.name != 'nt':
                raise
            # Both endpoints are explicit owned children of this temporary root.
            self.assertTrue(actual.resolve().is_relative_to(self.root.resolve()))
            self.assertTrue(link.resolve().is_relative_to(self.root.resolve()))
            result = subprocess.run(['cmd', '/c', 'mklink', '/J', str(link), str(actual)],
                capture_output=True, text=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
            if result.returncode:
                self.skipTest('Directory link capability unavailable: ' + result.stdout + result.stderr)
        original = (actual / 'unexpected.gma').read_bytes()
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)
        self.assertEqual((actual / 'unexpected.gma').read_bytes(), original)
        self.assertTrue(selected['warnings'] or selected['errors'] or
                        selected.get('workshop', {}).get('warnings'), selected)


    def test_downloading_items_are_not_loaded_from_retained_content(self):
        for item, name in [('3800000001', 'gm_pending_directory'), ('3800000002', 'gm_pending_flag')]:
            self.workshop_gma(item, name)
        metadata = self.write_workshop_manifest(['3800000001', '3800000002'])
        pending = self.library / 'steamapps/workshop/downloads/4000/3800000001'
        pending.mkdir(parents=True)
        raw = metadata.read_text(encoding='utf-8')
        # Place the per-item flag directly in the second installed record.
        raw = '"NeedsDownload"\t\t"1"\n"size"\t\t"2048"'.join(raw.rsplit('"size"\t\t"2048"', 1))
        metadata.write_text(raw, encoding='utf-8')
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)


    def test_explicit_steam_root_resolves_only_its_unambiguous_recent_account(self):
        self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        self.account = None
        recent = self.steam / 'config/loginusers.vdf'
        recent.parent.mkdir(parents=True)
        recent.write_text(kv_object({'users': {'76561197960265851': {'MostRecent': '1'}}}), encoding='utf-8')
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), {'gm_workshop_fixture'}, selected)
        recent.write_text(kv_object({'users': {
            '76561197960265851': {'MostRecent': '1'},
            '76561197960266184': {'MostRecent': '1'}}}), encoding='utf-8')
        ambiguous = self.run_prepare()
        self.assertEqual(self.ready_names(ambiguous), set(), ambiguous)
        self.assertEqual(ambiguous['workshop']['phase'], 'unavailable', ambiguous)
        recent.unlink()
        missing = self.run_prepare()
        self.assertEqual(self.ready_names(missing), set(), missing)
        self.assertEqual(missing['workshop']['phase'], 'unavailable', missing)


    def test_workshop_cache_in_nonowning_library_is_not_activated(self):
        stale = self.root / 'Old Library'
        self.workshop_gma('3800000001', 'gm_stale_library', library=stale)
        self.write_workshop_manifest(['3800000001'], library=stale)
        self.write_libraries([self.steam, stale])
        libraries = self.steam / 'steamapps/libraryfolders.vdf'
        libraries.write_text(kv_object({'libraryfolders': {
            '0': {'path': str(self.steam), 'apps': {'4000': '100000'}},
            '1': {'path': str(stale), 'apps': {'440': '100000'}}}}), encoding='utf-8')
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)
        self.assertEqual(selected['workshop']['selected'], 0, selected)

    def test_without_app_ownership_metadata_workshop_is_unavailable(self):
        self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        (self.steam / 'steamapps/appmanifest_4000.acf').unlink()
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)
        self.assertEqual(selected['workshop']['selected'], 0, selected)

    def test_conflicting_installed_versions_in_equal_priority_libraries_are_excluded(self):
        second = self.root / 'Second Owner Library'
        self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        self.workshop_gma('3800000001', library=second)
        metadata = self.write_workshop_manifest(['3800000001'], library=second)
        expected = str(900000000000000000 + 3800000001)
        metadata.write_text(metadata.read_text(encoding='utf-8').replace(expected, '999999999'), encoding='utf-8')
        self.write_libraries([self.steam, second])
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)
        self.assertEqual(selected['workshop']['selected'], 0, selected)

    def test_library_limit_never_activates_partial_discovery(self):
        self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        others = [self.root / f'Library_{index:02d}' for index in range(65)]
        for library in others:
            library.mkdir()
        self.write_libraries([self.steam, *others])
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)
        self.assertEqual(selected['workshop']['selected'], 0, selected)
        self.assertEqual(selected['workshop']['phase'], 'unavailable', selected)


    def test_identical_installed_version_in_multiple_libraries_is_deduplicated(self):
        second = self.root / 'Duplicate Owner Library'
        self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        self.workshop_gma('3800000001', library=second)
        self.write_workshop_manifest(['3800000001'], library=second)
        self.write_libraries([self.steam, second])
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), {'gm_workshop_fixture'}, selected)
        self.assertEqual(selected['workshop']['selected'], 1, selected)


    def test_offline_declared_owner_does_not_activate_stale_appmanifest_library(self):
        self.workshop_gma('3800000001')
        self.write_workshop_manifest(['3800000001'])
        offline = self.root / 'Offline Owner Library'
        libraries = self.steam / 'steamapps/libraryfolders.vdf'
        libraries.write_text(kv_object({'libraryfolders': {
            '0': {'path': str(offline), 'apps': {'4000': '100000'}},
            '1': {'path': str(self.steam), 'apps': {'440': '100000'}}}}), encoding='utf-8')
        selected = self.run_prepare()
        self.assertEqual(self.ready_names(selected), set(), selected)
        self.assertEqual(selected['workshop']['selected'], 0, selected)
        self.assertEqual(selected['workshop']['phase'], 'unavailable', selected)


if __name__ == '__main__':
    unittest.main(verbosity=2)
