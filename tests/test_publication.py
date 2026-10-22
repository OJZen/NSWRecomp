import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from check_publication import content_findings, identity_is_public, private_path


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull,
                        GIT_AUTHOR_NAME='PublicUser', GIT_COMMITTER_NAME='PublicUser',
                        GIT_AUTHOR_EMAIL='PublicUser@users.noreply.github.com',
                        GIT_COMMITTER_EMAIL='PublicUser@users.noreply.github.com')
        self.git('init', '-q', '--initial-branch=main')

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.repo, env=self.env, stderr=subprocess.PIPE)

    def commit(self, content='safe source\n'):
        (self.repo / 'source.txt').write_text(content)
        self.git('add', 'source.txt')
        self.git('commit', '-qm', 'Synthetic change')

    def check(self, *args):
        return subprocess.run([sys.executable, str(ROOT / 'tools/check_publication.py'), *args],
                              cwd=self.repo, env=self.env, capture_output=True, text=True)

    def test_sensitive_values_and_public_examples(self):
        samples = [('/' + 'Users/private-person/project').encode(),
                   ('/private/var/' + 'folders/synthetic/file').encode(),
                   ('private-person' + '@mail.invalid').encode(),
                   ('ghp_' + 'A' * 36).encode(),
                   ('192.' + '168.20.9').encode(),
                   ('-----BEGIN ' + 'PRIVATE KEY-----').encode(),
                   ('master_key_00 = ' + 'a' * 32).encode(),
                   ('1' * 32 + ' = ' + '2' * 32).encode()]
        for data in samples:
            with self.subTest(data_length=len(data)):
                self.assertTrue(content_findings(data))
        self.assertFalse(content_findings(b'PublicUser@users.noreply.github.com /path/to/input'))
        self.assertFalse(content_findings(b'user@example.com C:\\Users\\user\\AppData\\Roaming'))
        self.assertFalse(content_findings((ROOT / 'tools/check_publication.py').read_bytes()))
        self.assertTrue(identity_is_public(b'PublicUser <123+PublicUser@users.noreply.github.com> 1 +0000'))
        self.assertFalse(identity_is_public(b'Private Name <PublicUser@users.noreply.github.com> 1 +0000'))

    def test_clean_ancestry_and_invalid_ref(self):
        self.commit()
        self.assertEqual(self.check('--ref', 'main').returncode, 0)
        self.assertNotEqual(self.check('--ref', 'missing').returncode, 0)

    def test_deleted_secret_remains_in_history_and_output_is_redacted(self):
        secret = 'github_pat_' + 'B' * 50
        self.commit(secret + '\n')
        self.commit()
        result = self.check('--ref', 'HEAD')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('access token', result.stderr)
        self.assertNotIn(secret, result.stdout + result.stderr)

    def test_full_index_ignores_unstaged_content(self):
        self.commit()
        secret = 'ghp_' + 'C' * 36
        (self.repo / 'source.txt').write_text(secret)
        self.assertEqual(self.check('--staged').returncode, 0)
        self.git('add', 'source.txt')
        self.assertNotEqual(self.check('--staged').returncode, 0)

    def test_replace_refs_cannot_hide_original_commit_contents(self):
        self.commit('ghp_' + 'E' * 36)
        old = self.git('rev-parse', 'HEAD').decode().strip()
        self.commit()
        tree = self.git('rev-parse', 'HEAD^{tree}').decode().strip()
        replacement = self.git('commit-tree', tree, '-m', 'Synthetic replacement').decode().strip()
        self.git('replace', old, replacement)
        self.assertNotEqual(self.check('--ref', 'HEAD').returncode, 0)

    def test_shallow_clone_cannot_claim_full_history_review(self):
        self.commit()
        clone = self.repo / 'shallow'
        self.git('clone', '--quiet', '--depth', '1', self.repo.as_uri(), str(clone))
        result = subprocess.run([sys.executable, str(ROOT / 'tools/check_publication.py'), '--ref', 'HEAD'],
                                cwd=clone, env=self.env, capture_output=True)
        self.assertNotEqual(result.returncode, 0)

    def test_private_identity_is_rejected_in_index_and_history(self):
        self.env['GIT_AUTHOR_EMAIL'] = 'private-person' + '@mail.invalid'
        self.commit()
        self.assertNotEqual(self.check('--staged').returncode, 0)
        self.assertNotEqual(self.check('--ref', 'HEAD').returncode, 0)

    def test_path_checks_do_not_depend_on_blob_uniqueness(self):
        self.commit()
        (self.repo / 'work').mkdir()
        (self.repo / 'work/source.txt').write_text('safe source\n')
        self.git('add', 'work/source.txt')
        self.git('commit', '-qm', 'Synthetic private path')
        self.assertNotEqual(self.check('--ref', 'HEAD').returncode, 0)
        for path in ('local/inputs.json', '.env', '.env.local', 'prod.keys', 'save.sav', 'guest.nso'):
            self.assertTrue(private_path(path))
        self.assertFalse(private_path('runtime/load.c'))

    def test_binary_and_symlink_are_rejected(self):
        self.commit()
        (self.repo / 'binary.txt').write_bytes(b'synthetic\0data')
        (self.repo / 'link').symlink_to('source.txt')
        self.git('add', 'binary.txt', 'link')
        result = self.check('--staged')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('binary content', result.stderr)
        self.assertIn('symlink', result.stderr)

    def test_hooks_validate_actual_push_ref_and_staged_tree(self):
        self.commit()
        sha = self.git('rev-parse', 'HEAD').decode().strip()
        for local, remote, expected in (
                ('refs/heads/main', 'refs/heads/main', 0),
                ('refs/heads/topic', 'refs/heads/main', 1),
                ('refs/heads/main', 'refs/heads/topic', 1),
                ('refs/tags/test', 'refs/tags/test', 1),
                ('(delete)', 'refs/heads/main', 1)):
            result = subprocess.run(['sh', str(ROOT / '.githooks/pre-push'), 'origin', 'unused'],
                                    input=f'{local} {sha} {remote} {"0" * 40}\n', text=True,
                                    cwd=self.repo, env=self.env, capture_output=True)
            self.assertEqual(result.returncode, expected, result.stderr)
        self.git('config', 'core.hooksPath', str(ROOT / '.githooks'))
        (self.repo / 'source.txt').write_text('ghp_' + 'D' * 36)
        self.git('add', 'source.txt')
        result = subprocess.run(['git', 'commit', '-qm', 'Must fail'], cwd=self.repo,
                                env=self.env, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.git('rev-parse', 'HEAD').decode().strip(), sha)
        marker = 'synthetic-whitespace-marker'
        (self.repo / 'source.txt').write_text(marker + ' \n')
        self.git('add', 'source.txt')
        result = subprocess.run(['git', 'commit', '-qm', 'Must also fail'], cwd=self.repo,
                                env=self.env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(marker, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
