# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoDownloadTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-download')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-response',
                          'fumo-syncml', 'fumo-trigger', 'fumo-command',
                          'fumo-server-auth', 'fumo-download',
                          'fumo-no-production', 'dmengine-fumo-download'})
        self.assertIn('GET /package.bin', scenario['description'])
        self.assertIn('no install or update-success claim',
                      scenario['description'])

    def test_command_uses_download_mode_and_asserts_get(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-download'), load_profile('research'),
            kernel_target='compat')
        for text in ('/tmp/mock-dm 8002 /tmp/mock-dm.log download',
                     'CLIENT_DIGEST=', 'MOCK_DM_DOWNLOAD method=GET',
                     'bytes=4096', 'status=407', 'fumo-server-auth',
                     'fumo-download'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-download')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-download PASS', 'fumo-download FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
