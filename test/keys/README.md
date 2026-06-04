# Test-only keys

These files exist **only** to drive the offline JWT-bearer tests
(`test/sql/salesforce_jwt.test`). They are fake, throwaway fixtures — **not**
credentials for any real Salesforce org. Do not reuse them anywhere.

- `jwt_test_key.pem` — a freshly generated, unencrypted PKCS#8 RSA test key.
- `corrupt_key.pem` — has PEM markers but an unparseable body (signing-error path).
- `not_pem.txt` — plain text, no PEM markers (the "file is not a PEM key" case).
