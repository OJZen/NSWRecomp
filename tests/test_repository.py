"""The shareable tree contains source, metadata and synthetic tests only."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RepositoryTests(unittest.TestCase):
    def test_source_tree_excludes_local_inputs_and_outputs(self):
        result = subprocess.run(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'],
                                cwd=ROOT, capture_output=True, check=True)
        allowed_names = {'.gitignore', 'Makefile', 'LICENSE'}
        allowed_suffixes = {'.c', '.h', '.inc', '.m', '.py', '.md', '.json', '.txt', '.cs', '.csproj', '.props', '.glsl', '.sh', '.toml', '.patch'}
        for raw in result.stdout.split(b'\0'):
            if not raw:
                continue
            path = Path(raw.decode())
            with self.subTest(path=str(path)):
                self.assertNotIn(path.parts[0], {'work', 'local', 'generated', 'build'})
                self.assertTrue(path.name in allowed_names or path.suffix in allowed_suffixes)
                self.assertFalse((ROOT / path).is_symlink())


if __name__ == '__main__':
    unittest.main()
