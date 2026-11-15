# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import struct
import subprocess
import sys
import unittest
from pathlib import Path

from harness.dm_queue import (DATA_SIZE, OEM_COMMAND_TYPE, OEM_EVENT_TYPE,
                              QueueError, decode_message, decode_payload,
                              encode_command, encode_event, encode_payload)

VM = Path(__file__).resolve().parents[1]


class DmQueueCodecTests(unittest.TestCase):
    def test_command_roundtrip_and_offsets(self) -> None:
        mtype, data = encode_command(66, 7, -9, 'msgtype=4&cor=LAB')
        self.assertEqual(mtype, OEM_COMMAND_TYPE)
        self.assertEqual(len(data), DATA_SIZE)
        self.assertEqual(struct.unpack_from('<i', data, 0)[0], 66)
        self.assertEqual(struct.unpack_from('<q', data, 8)[0], 7)
        self.assertEqual(struct.unpack_from('<q', data, 16)[0], -9)
        self.assertEqual(data[32:32 + 16], b'msgtype=4&cor=LA')
        decoded = decode_message(mtype, data)
        self.assertEqual(decoded['kind'], 'command')
        self.assertEqual(decoded['code'], 66)
        self.assertEqual(decoded['field1'], 7)
        self.assertEqual(decoded['field2'], -9)
        self.assertEqual(decoded['text'], 'msgtype=4&cor=LAB')

    def test_event_roundtrip(self) -> None:
        mtype, data = encode_event(93)
        self.assertEqual(mtype, OEM_EVENT_TYPE)
        decoded = decode_message(mtype, data)
        self.assertEqual(decoded['kind'], 'event')
        self.assertEqual(decoded['code'], 93)
        self.assertEqual(decoded['text'], '')

    def test_payload_size_and_mtype_validation(self) -> None:
        with self.assertRaisesRegex(QueueError, 'payload must be'):
            decode_payload(b'\0' * 10)
        with self.assertRaisesRegex(QueueError, 'unknown mtype'):
            decode_message(0x99, encode_payload(1))

    def test_field_validation(self) -> None:
        with self.assertRaisesRegex(QueueError, 'outside int32'):
            encode_payload(2 ** 31)
        with self.assertRaisesRegex(QueueError, 'outside int64'):
            encode_payload(1, 2 ** 63)
        with self.assertRaisesRegex(QueueError, 'must be an integer'):
            encode_payload(1, True)
        with self.assertRaisesRegex(QueueError, 'too long'):
            encode_payload(1, text='x' * (DATA_SIZE - 32))
        with self.assertRaisesRegex(QueueError, 'must be a string'):
            encode_payload(1, text=b'bytes')

    def test_cli(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'dmengine-queue',
             'encode', '--code', '66', '--field1', '7',
             '--text', 'msgtype=4&cor=LAB'],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        encoded = json.loads(completed.stdout)
        self.assertTrue(encoded['ok'])
        self.assertEqual(encoded['mtype'], OEM_COMMAND_TYPE)
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'dmengine-queue',
             'decode', '--mtype', '0x36', '--hex', encoded['hex']],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        decoded = json.loads(completed.stdout)
        self.assertEqual(decoded['code'], 66)
        self.assertEqual(decoded['field1'], 7)
        self.assertEqual(decoded['text'], 'msgtype=4&cor=LAB')
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'dmengine-queue',
             'decode', '--hex', '00'], cwd=VM, capture_output=True,
            text=True, check=False)
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(json.loads(completed.stdout)['ok'])


if __name__ == '__main__':
    unittest.main()
