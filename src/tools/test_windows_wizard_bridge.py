"""Exercise the native argument boundary and failure reporting of the GUI bridge."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import windows_wizard as bridge


class BridgeTests(unittest.TestCase):
    @unittest.skipUnless(os.name == 'nt', 'Uses the actual Windows game argument parser')
    def test_windows_paths_and_shell_characters_remain_literal_arguments(self):
        wanted = ['+r_mode', '-1', '--assets', r'D:\Games (test)\assets with spaces' + chr(92),
                  'say "hello"', '&', '%PATH%', '$(not-a-command)', 'two\\slashes']
        self.assertEqual(bridge.windows_arguments(subprocess.list2cmdline(wanted)), wanted)

    def test_failed_check_has_nonzero_exit_and_readable_json(self):
        with tempfile.TemporaryDirectory(prefix='nr bridge (test) ') as room:
            root = Path(room)
            request = root / 'request.json'
            result = root / 'result.json'
            request.write_text(json.dumps({'root': 'ignored old root', 'python': sys.executable,
                'game_exe': str(root / 'game.exe'), 'dll': None}), encoding='utf-8')
            with mock.patch.object(bridge.core, 'validate', return_value={
                    'ok': False, 'checks': [{'name': 'runtime', 'ok': False, 'detail': 'Missing allocator'}]}), \
                 mock.patch.object(sys, 'argv', ['wizard', 'check', '--root', str(root),
                    '--input', str(request), '--output', str(result)]):
                self.assertEqual(bridge.main(), 1)
            data = json.loads(result.read_text(encoding='utf-8'))
            self.assertFalse(data['ok'])
            self.assertEqual(data['profile']['root'], str(root.resolve()))
            self.assertIn('Missing allocator', data['checks'][0]['detail'])
            self.assertFalse(result.with_suffix('.new').exists())

    def test_no_profile_failure_is_reported_instead_of_silent_console_exit(self):
        with tempfile.TemporaryDirectory(prefix='nr no profile ') as room:
            output = Path(room) / 'result.json'
            with mock.patch.object(bridge.core, 'load_profile', return_value=None), \
                 mock.patch.object(sys, 'argv', ['wizard', 'on', '--root', room, '--output', str(output)]):
                self.assertEqual(bridge.main(), 1)
            data = json.loads(output.read_text())
            self.assertFalse(data['ok'])
            self.assertIn('Choose your game', data['error'])


if __name__ == '__main__':
    unittest.main()
