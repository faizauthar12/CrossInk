#!/usr/bin/env python3
"""Exercise catalog coexistence and publication ordering with an in-memory S3."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('publisher', ROOT / 'scripts/publish_language_assets.py')
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


def bundle(source='a', keyset='b', revision='c', text=b'French', channel='stable', kind='translation'):
    prefix = f'snapshots/{revision * 40}/'
    notice = b'Pattern source and notices'
    ref = {'path': prefix + 'source/notice.txt', 'bytes': len(notice), 'sha256': publisher.sha(notice)}
    code = 'FR' if kind == 'translation' else 'fr'
    entry = {'code': code, 'bytes': len(text), 'sha256': publisher.sha(text)}
    catalog = {'schema': 1, 'kind': kind, 'channel': channel, 'revision': revision * 40,
               'format': 1, 'requires': 0, 'notices': 'notices.json', 'sources': 'sources.json', 'entries': [entry]}
    if kind == 'translation':
        catalog.update(source=source * 64, keyset=keyset * 64)
        entry['name'] = 'French'
    extension = 'yaml' if kind == 'translation' else 'cphyph'
    files = {ref['path']: notice, prefix + kind + '/' + entry['sha256'] + '.' + extension: text}
    for name in ('notices.json', 'sources.json'):
        files[prefix + name] = publisher.encode({kind: {code: {'files': [ref]}}})
    return catalog, files


class PublicationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'platformio.ini').write_text('[crossink]\nversion = 2.0.0\n')
        self.store = {}
        self.writes = []
        self.reads = []
        self.current = bundle()
        self.hyphenation = bundle(kind='hyphenation', revision='f')
        self.fail_key = None
        self.corrupt_key = None

    def aws(self, *command):
        key = command[command.index('--key') + 1].removeprefix('v1/')
        if command[0] == 'get-object':
            self.reads.append(key)
            if key not in self.store:
                raise RuntimeError('(NoSuchKey)')
            data = self.store[key]
            Path(command[-1]).write_bytes(data)
            return {'ETag': publisher.sha(data)}
        self.assertEqual(command[0], 'put-object')
        if key == self.fail_key:
            raise RuntimeError('injected publication failure')
        if '--if-none-match' in command:
            self.assertNotIn(key, self.store, 'create must not overwrite an existing object')
        else:
            self.assertIn('--if-match', command)
            self.assertEqual(command[command.index('--if-match') + 1], publisher.sha(self.store[key]))
        # Every new catalog must have its payload and both indexes already readable.
        data = Path(command[command.index('--body') + 1]).read_bytes()
        if key.startswith('catalogs/'):
            catalog = json.loads(data)
            prefix = f"snapshots/{catalog['revision']}/"
            for name in ('notices.json', 'sources.json'):
                self.assertIn(prefix + name, self.store)
            extension = 'yaml' if catalog['kind'] == 'translation' else 'cphyph'
            for entry in catalog['entries']:
                self.assertIn(prefix + catalog['kind'] + '/' + entry['sha256'] + '.' + extension, self.store)
        self.store[key] = b'corrupt' if key == self.corrupt_key else data
        self.writes.append(key)
        return {}

    def run_publisher(self, *options):
        with patch.object(publisher, 'root', self.root), \
             patch.object(publisher, 'aws', self.aws), \
             patch.object(publisher, 'build_languages', side_effect=lambda *_: copy.deepcopy(self.current)), \
             patch.object(publisher, 'build_hyphenation', side_effect=lambda: copy.deepcopy(self.hyphenation)), \
             patch.object(publisher.subprocess, 'check_output', return_value='1' * 40), \
             patch.dict('os.environ', {}, clear=True), \
             patch.object(sys, 'argv', ['publish_language_assets.py', '--assets', 'languages', *options]), \
             contextlib.redirect_stdout(io.StringIO()):
            publisher.main()

    def seed(self, data, legacy=False):
        catalog, files = data
        self.store.update(files)
        key = (f"catalogs/{catalog['channel']}/translation.json" if legacy else publisher.catalog_key(catalog))
        self.store[key] = publisher.encode(catalog)
        return key

    def test_contracts_coexist_and_legacy_keeps_first_contract(self):
        self.run_publisher('--publish')
        original = dict(self.store)
        self.current = bundle(source='d', revision='d')  # English changes, same keys.
        self.run_publisher('--publish')
        second = publisher.catalog_key(self.current[0])
        self.current = bundle(source='d', keyset='e', revision='e')  # Keyset changes too.
        self.run_publisher('--publish')
        for key, data in original.items():
            self.assertEqual(self.store[key], data)
        self.assertIn(second, self.store)
        self.assertIn(publisher.catalog_key(self.current[0]), self.store)
        self.assertEqual(len([key for key in self.store if key.endswith('.json') and '/translation/' in key]), 3)

    def test_new_languages_update_scoped_catalog_without_breaking_legacy(self):
        self.run_publisher('--publish')
        legacy_key = 'catalogs/stable/translation.json'
        legacy_data = self.store[legacy_key]
        self.current = bundle(revision='d', text=b'Corrected French')
        catalog, files = self.current
        extra = dict(catalog['entries'][0], code='NEW', name='New language')
        catalog['entries'].append(extra)
        for path in list(files):
            if path.endswith(('notices.json', 'sources.json')):
                index = json.loads(files[path])
                index['translation']['NEW'] = copy.deepcopy(index['translation']['FR'])
                files[path] = publisher.encode(index)
        self.run_publisher('--publish')
        self.assertEqual(self.store[legacy_key], legacy_data)
        scoped = json.loads(self.store[publisher.catalog_key(catalog)])
        self.assertEqual([entry['code'] for entry in scoped['entries']], ['FR', 'NEW'])
        self.run_publisher('--publish')
        self.assertEqual(self.store[legacy_key], legacy_data)

    def test_migrates_legacy_to_scoped_catalog_before_new_contract(self):
        legacy = self.seed(self.current, legacy=True)
        old_data = self.store[legacy]
        old_key = publisher.catalog_key(self.current[0])
        self.current = bundle(source='d', revision='d')
        self.run_publisher('--publish')
        self.assertEqual(self.store[legacy], old_data)
        self.assertEqual(self.store[old_key], old_data)
        self.assertIn(publisher.catalog_key(self.current[0]), self.store)

    def test_migration_never_replaces_newer_scoped_catalog(self):
        self.seed(self.current, legacy=True)
        updated = bundle(revision='e', text=b'Updated French')
        key = self.seed(updated)
        expected = self.store[key]
        self.current = bundle(source='d', revision='d')
        self.run_publisher('--publish')
        self.assertEqual(self.store[key], expected)

    def test_compatible_corrections_update_legacy_and_remember_previous_hash(self):
        self.seed(self.current, legacy=True)
        old_digest = self.current[0]['entries'][0]['sha256']
        self.current = bundle(revision='d', text=b'Corrected French')
        self.run_publisher('--publish')
        key = publisher.catalog_key(self.current[0])
        catalog = json.loads(self.store[key])
        self.assertEqual(catalog['entries'][0]['previous'], old_digest)
        self.assertEqual(self.store[key], self.store['catalogs/stable/translation.json'])
        count = len(self.writes)
        self.run_publisher('--publish')
        self.assertEqual(len(self.writes), count)

    def test_rc_and_hyphenation_are_independent(self):
        self.run_publisher('--publish', '--assets', 'both')
        stable = dict(self.store)
        self.current = bundle(source='d', channel='rc', revision='d')
        self.run_publisher('--publish', '--channel', 'rc')
        for key, data in stable.items():
            self.assertEqual(self.store[key], data)
        self.assertIn('catalogs/rc/translation.json', self.store)
        self.assertIn('catalogs/stable/hyphenation.json', self.store)
        self.assertNotIn('catalogs/rc/hyphenation.json', self.store)

    def test_build_is_offline_and_check_is_read_only(self):
        self.run_publisher()
        self.assertEqual(self.reads, [])
        self.assertEqual(self.writes, [])
        key = publisher.catalog_key(self.current[0])
        self.assertTrue((self.root / '.local-language-publish/stable' / key).is_file())
        self.seed(self.current, legacy=True)
        original = dict(self.store)
        self.current = bundle(source='d', revision='d')
        self.run_publisher('--check')
        self.assertEqual(self.store, original)
        self.assertEqual(self.writes, [])

    def test_immutable_collision_aborts_before_any_catalog_promotion(self):
        key = next(iter(self.current[1]))
        self.store[key] = b'wrong bytes'
        with self.assertRaisesRegex(RuntimeError, 'immutable snapshot collision'):
            self.run_publisher('--publish')
        self.assertEqual(self.writes, [])

    def test_corrupted_upload_is_not_advertised(self):
        self.corrupt_key = next(iter(self.current[1]))
        with self.assertRaisesRegex(RuntimeError, 'read-back mismatch'):
            self.run_publisher('--publish')
        self.assertFalse(any(key.startswith('catalogs/') for key in self.writes))

    def test_interrupted_catalog_promotion_can_be_retried(self):
        self.fail_key = 'catalogs/stable/translation.json'
        with self.assertRaisesRegex(RuntimeError, 'injected publication failure'):
            self.run_publisher('--publish')
        key = publisher.catalog_key(self.current[0])
        self.assertIn(key, self.store)
        self.fail_key = None
        self.run_publisher('--publish')
        self.assertEqual(self.store[key], self.store['catalogs/stable/translation.json'])

    def test_catalog_under_wrong_contract_path_is_rejected(self):
        catalog, _ = bundle(source='d', revision='d')
        self.store[publisher.catalog_key(self.current[0])] = publisher.encode(catalog)
        with self.assertRaisesRegex(RuntimeError, 'wrong identity'):
            self.run_publisher('--publish')
        self.assertEqual(self.writes, [])

    def test_failed_catalog_read_does_not_create_or_replace_catalogs(self):
        with patch.object(publisher, 'remote_get', side_effect=RuntimeError('AccessDenied')):
            with self.assertRaisesRegex(RuntimeError, 'AccessDenied'):
                self.run_publisher('--publish')
        self.assertEqual(self.writes, [])


if __name__ == '__main__':
    unittest.main()
