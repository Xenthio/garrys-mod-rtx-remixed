"""Validate renderer preflight without loading code or modifying the contract."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RENDERER, PROXY = (Path(p).resolve() for p in sys.argv[1:3])
del sys.argv[1:3]
GENERATOR = Path(__file__).resolve().parents[1] / 'generate_proxy_exports.py'


class RendererContractTests(unittest.TestCase):
    def test_exact_match_and_mismatch_are_read_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = subprocess.run([sys.executable, str(GENERATOR), str(RENDERER),
                                        '--output', str(root)], capture_output=True, text=True)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            contract = root / 'proxy_renderer_exports.json'
            before = {p.name: p.read_bytes() for p in root.iterdir()}
            args = [sys.executable, str(GENERATOR), str(RENDERER), '--output', str(root), '--check']
            checked = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stderr)
            self.assertTrue(json.loads(checked.stdout)['verified'])
            self.assertEqual(before, {p.name: p.read_bytes() for p in root.iterdir()})
            changed = json.loads(contract.read_text())
            changed['renderer_sha256'] = '0' * 64
            contract.write_text(json.dumps(changed))
            before = {p.name: p.read_bytes() for p in root.iterdir()}
            rejected = subprocess.run(args, capture_output=True, text=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn('differs from the audited proxy contract', rejected.stderr)
            self.assertEqual(before, {p.name: p.read_bytes() for p in root.iterdir()})

    def test_wrapper_is_not_an_original_renderer(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, str(GENERATOR), str(PROXY),
                                     '--output', directory, '--check'], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(list(Path(directory).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
