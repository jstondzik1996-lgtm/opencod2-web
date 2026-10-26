"""Credential-redaction regression tests for the real two-client harness."""
import unittest

from browser_mp_redact import sanitize_test_log


class BrowserHarnessLogTests(unittest.TestCase):
    def test_redacts_join_ticket_in_ws_event(self):
        original = '{"kind":"ws","value":"ws://localhost/gamews?host=127.0.0.1&port=28961&ticket=signedSensitiveToken123&mode=game"}'
        sanitized = sanitize_test_log(original)
        self.assertNotIn('signedSensitiveToken123', sanitized)
        self.assertIn('ticket=[redacted]&mode=game', sanitized)
        self.assertIn('port=28961', sanitized)

    def test_redacts_session_and_asset_access(self):
        for secret_name in ('session', 'ticket', 'access', 'token'):
            example = 'https://example.invalid/play?%s=neverLogThis123' % secret_name
            sanitized = sanitize_test_log(example)
            self.assertNotIn('neverLogThis123', sanitized)
            self.assertIn('%s=[redacted]' % secret_name, sanitized)

    def test_redacts_installed_key_and_prefix(self):
        key = 'ABCDEFGHIJKLMNOPQRST'
        message = '{"key":"%s","part":"%s"}' % (key, key[:16])
        redacted = sanitize_test_log(message, key)
        self.assertNotIn(key, redacted)
        self.assertNotIn(key[:16], redacted)
        self.assertIn('[redacted]', redacted)

    def test_redacts_structured_secrets_and_bearer(self):
        for name in ('ticket', 'session', 'playerKey', 'codkey', 'access_key'):
            raw = '{"%s": "sensitiveValue"}' % name
            self.assertNotIn('sensitiveValue', sanitize_test_log(raw))
        self.assertNotIn('sensitiveValue', sanitize_test_log('Authorization: Bearer sensitiveValue'))

    def test_without_installed_key_does_not_modify_unrelated_log(self):
        sample = '{"kind":"state","health":100,"origin":[1,2,3]}'
        self.assertEqual(sanitize_test_log(sample), sample)


if __name__ == '__main__':
    unittest.main()
