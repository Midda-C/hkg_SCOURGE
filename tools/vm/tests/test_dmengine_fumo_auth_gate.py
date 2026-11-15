# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate

VM = Path(__file__).resolve().parents[1]


class DmengineFumoAuthGateTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('dmengine-fumo-auth-gate')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertEqual(set(scenario['checks']),
                         {'fumo-ready', 'fumo-http-post', 'fumo-response',
                          'fumo-syncml', 'fumo-trigger', 'fumo-command',
                          'fumo-auth-407', 'fumo-no-download', 'fumo-abort',
                          'fumo-no-production', 'dmengine-fumo-auth-gate'})
        self.assertIn('negative auth-gate', scenario['description'])
        self.assertIn('No package download occurs', scenario['description'])

    def test_command_uses_download_mode_and_asserts_407(self) -> None:
        command = scenario_command(
            load_scenario('dmengine-fumo-auth-gate'), load_profile('research'),
            kernel_target='compat')
        for text in ('/tmp/mock-dm 8002 /tmp/mock-dm.log download',
                     'MOCK_DM_CLIENT_STATUS status=407',
                     'exec=1', 'DM session aborted, 0x6011',
                     'MOCK_DM_DOWNLOAD', 'fumo-no-download'):
            self.assertIn(text, command)

    def test_checks_fail_closed(self) -> None:
        scenario = load_scenario('dmengine-fumo-auth-gate')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(
            text.replace('fumo-auth-407 PASS', 'fumo-auth-407 FAIL'),
            0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
