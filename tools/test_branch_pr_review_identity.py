#!/usr/bin/env python3
"""Cold-start identity tests for branch_pr_review.py."""

import importlib.util
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("bpr", HERE / "branch_pr_review.py")
bpr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bpr)


class IdentityBootstrapTest(unittest.TestCase):
    def setUp(self):
        self._temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self._temporary_directory.cleanup)
        self.key_path = (
            Path(self._temporary_directory.name) / "identity" / "ed25519.pem"
        )
        self.key_path_patch = mock.patch.object(bpr, "KEY_PATH", self.key_path)
        self.key_path_patch.start()
        self.addCleanup(self.key_path_patch.stop)
        bpr._key = None
        bpr._pub = None

    def test_missing_identity_is_generated_as_owner_only_ed25519_key(self):
        bpr.ensure_identity()

        key = bpr.serialization.load_pem_private_key(
            self.key_path.read_bytes(), password=None
        )
        self.assertIsInstance(key, Ed25519PrivateKey)
        if os.name == "posix":
            self.assertEqual(stat.S_IMODE(self.key_path.stat().st_mode), 0o600)

    def test_existing_identity_is_never_replaced(self):
        self.key_path.parent.mkdir(parents=True)
        existing = b"existing identity bytes\n"
        self.key_path.write_bytes(existing)

        bpr.ensure_identity()

        self.assertEqual(self.key_path.read_bytes(), existing)

    def test_main_continues_initializing_after_cold_start(self):
        server = mock.Mock()
        with mock.patch.object(bpr, "ThreadingHTTPServer", return_value=server):
            bpr.main()

        self.assertTrue(self.key_path.exists())
        server.serve_forever.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
