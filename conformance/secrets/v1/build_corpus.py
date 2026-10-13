#!/usr/bin/env python3
"""Build corpus.json, the maintained benign/malicious secret-scan corpus.

This file is the readable source; ``corpus.json`` is generated from it.

    python3 conformance/secrets/v1/build_corpus.py          # rewrite
    python3 conformance/secrets/v1/build_corpus.py --check  # exit 1 if stale

Every credential here is synthetic: a real provider prefix followed by
deterministic pseudo-random characters. None was ever issued. Case content is
stored in short fragments so that neither this repository's own push scanner
nor the public report flags the corpus; ``tools/secret_gate.py corpus`` joins
the fragments before scanning.

A case marked ``knownLimitation`` documents a boundary the contract is known
to misclassify. It still counts toward the measured rates. When a scanner
change fixes one, the evaluation fails until the flag is removed here, so the
published rates stay honest.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys


HERE = Path(__file__).resolve().parent
OUTPUT = HERE / "corpus.json"
FRAGMENT = 12
BOUNDARIES = ["publication", "review-diff", "pre-commit", "git-objects"]

ALNUM = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
UPPER_DIGITS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"
HEX = "0123456789abcdef"
URLSAFE = ALNUM + "-_"
BASE64 = ALNUM + "+/"


def synth(seed: str, length: int, alphabet: str = ALNUM) -> str:
    out = []
    counter = 0
    while len(out) < length:
        block = hashlib.sha256(("forkmesh-corpus\0%s\0%d" % (seed, counter))
                               .encode()).digest()
        out.extend(alphabet[byte % len(alphabet)] for byte in block)
        counter += 1
    return "".join(out[:length])


def pem(kind: str, seed: str) -> str:
    body = synth(seed, 64 * 6, BASE64)
    lines = [body[i:i + 64] for i in range(0, len(body), 64)]
    return "-----BEGIN %s-----\n%s\n-----END %s-----\n" % (
        kind, "\n".join(lines), kind)


def malicious(case_id, rule, path, content, note, known=False):
    return {"id": case_id, "label": "malicious", "rule": rule, "path": path,
            "note": note, "content": content, "known": known}


def benign(case_id, path, content, note, known=False):
    return {"id": case_id, "label": "benign", "rule": "", "path": path,
            "note": note, "content": content, "known": known}


CASES = [
    # ---- Malicious: one or more per contract rule -------------------------
    malicious("m-github-token-env", "github-token", "deploy/.env",
              "GITHUB_TOKEN=ghp_%s\n" % synth("gh1", 36),
              "classic personal access token in a dotenv file"),
    malicious("m-github-token-yaml", "github-token", "ci/workflow.yml",
              "env:\n  token: gho_%s\n" % synth("gh2", 36),
              "OAuth token in YAML"),
    malicious("m-github-fine-grained", "github-fine-grained-token",
              "scripts/release.sh",
              "export GH_TOKEN=github_pat_%s_%s\n" % (
                  synth("ghf1", 22), synth("ghf2", 59)),
              "fine-grained token exported from a script"),
    malicious("m-aws-access-key", "aws-access-key", "home/.aws/credentials",
              "[default]\naws_access_key_id = AKIA%s\n" % synth(
                  "aws1", 16, UPPER_DIGITS),
              "long-lived IAM access key id"),
    malicious("m-aws-temporary-key", "aws-temporary-access-key",
              "logs/session.json",
              '{"AccessKeyId": "ASIA%s"}\n' % synth("aws2", 16, UPPER_DIGITS),
              "STS session key id pasted from a CLI dump"),
    malicious("m-slack-bot-token", "slack-token", "src/notify.js",
              "const token = 'xoxb-%s-%s-%s';\n" % (
                  synth("sl1", 12, "0123456789"), synth("sl2", 13, "0123456789"),
                  synth("sl3", 24)),
              "Slack bot token in JavaScript"),
    malicious("m-openai-project-key", "openai-api-key", "app/settings.py",
              'OPENAI_API_KEY = "sk-proj-%s"\n' % synth("oa1", 48, URLSAFE),
              "project-scoped OpenAI key"),
    malicious("m-openai-legacy-key", "openai-api-key", "notebook/cell.py",
              "client = OpenAI(api_key=sk-%s)\n" % synth("oa2", 48),
              "legacy unscoped OpenAI key"),
    malicious("m-anthropic-key", "anthropic-api-key", "agent/config.toml",
              'key = "sk-ant-api03-%s"\n' % synth("an1", 64, URLSAFE),
              "Anthropic API key in TOML"),
    malicious("m-stripe-live-secret", "stripe-secret-key", "billing/.env",
              "STRIPE_KEY=sk_live_%s\n" % synth("st1", 32),
              "Stripe live secret key"),
    malicious("m-stripe-restricted", "stripe-restricted-key", "billing/rk.txt",
              "rk_live_%s\n" % synth("st2", 32),
              "Stripe restricted key on its own line"),
    malicious("m-google-api-key", "google-api-key", "web/index.html",
              '<script src="https://maps.example.test/js?key=AIza%s"></script>\n'
              % synth("gg1", 35, URLSAFE),
              "browser-embedded Google API key"),
    malicious("m-google-oauth-token", "google-oauth-token", "tmp/curl.sh",
              "curl -H 'Authorization: Bearer ya29.%s' https://api.test/\n"
              % synth("gg2", 60, URLSAFE),
              "Google OAuth access token in a shell history snippet"),
    malicious("m-google-client-secret", "google-oauth-client-secret",
              "oauth/client.json",
              '{"installed": {"client_secret": "GOCSPX-%s"}}\n'
              % synth("gg3", 28, URLSAFE),
              "Google OAuth client secret JSON"),
    malicious("m-sendgrid-key", "sendgrid-api-key", "mail/send.rb",
              "SENDGRID = 'SG.%s.%s'\n" % (synth("sg1", 22, URLSAFE),
                                          synth("sg2", 43, URLSAFE)),
              "SendGrid API key in Ruby"),
    malicious("m-twilio-api-key", "twilio-api-key", "sms/twilio.cfg",
              "api_key_sid SK%s\n" % synth("tw1", 32, HEX),
              "Twilio API key sid"),
    malicious("m-npm-token", "npm-access-token", "pkg/.npmrc",
              "//registry.npmjs.org/:_authToken=npm_%s\n" % synth("np1", 36),
              "npm automation token in .npmrc"),
    malicious("m-vault-token", "vault-token", "ops/vault.env",
              "VAULT_TOKEN=hvs.%s\n" % synth("vt1", 90),
              "HashiCorp Vault service token"),
    malicious("m-cloudflare-token", "cloudflare-api-token", "infra/deploy.sh",
              'CLOUDFLARE_API_TOKEN="%s"\n' % synth("cf1", 40, URLSAFE),
              "Cloudflare API token assignment"),
    malicious("m-pem-rsa", "private-key-material", "certs/server.key",
              pem("RSA PRIVATE KEY", "pem1"), "PKCS#1 RSA private key"),
    malicious("m-pem-openssh", "private-key-material", "home/.ssh/id_ed25519",
              pem("OPENSSH PRIVATE KEY", "pem2"), "OpenSSH private key"),
    malicious("m-pem-pkcs8", "private-key-material", "keys/signing.pem",
              pem("PRIVATE KEY", "pem3"), "PKCS#8 private key"),
    malicious("m-generic-db-password", "generic-secret-assignment",
              "config/database.py",
              'DB_PASSWORD = "%s"\n' % synth("gp1", 24, URLSAFE),
              "database password assignment"),
    malicious("m-generic-access-token-yaml", "generic-secret-assignment",
              "k8s/secret.yaml",
              "access_token: '%s'\n" % synth("gp2", 40),
              "quoted token in YAML"),
    malicious("m-generic-continuation", "generic-secret-assignment",
              "scripts/fetch.sh",
              "curl https://api.test/ \\\n  -d auth_token=\"%s\" \\\n  -o out\n"
              % synth("gp3", 30),
              "assignment inside a backslash-continued shell command"),
    malicious("m-generic-json-key", "generic-secret-assignment",
              "config/app.json",
              '{"api_key": "%s"}\n' % synth("gp4", 32),
              "JSON object key: the closing quote sits between the name and "
              "the colon, which the assignment pattern does not allow",
              known=True),
    malicious("m-split-concatenation", "github-token", "src/obfuscated.py",
              'TOKEN = "ghp_" + "%s"\n' % synth("gh3", 36),
              "a token split by string concatenation defeats every "
              "line-level pattern", known=True),
    malicious("m-base64-wrapped", "aws-access-key", "config/encoded.txt",
              "creds=QUtJQ%s\n" % synth("b64", 30, BASE64),
              "a key id wrapped in base64 is not decoded", known=True),

    # ---- Benign ----------------------------------------------------------
    benign("b-placeholder-password", "docs/setup.py",
           'password = "correct-horse-battery-staple-example"\n',
           "documentation placeholder"),
    benign("b-env-indirection", "infra/wrapper.sh",
           'CLOUDFLARE_API_TOKEN="$CLOUDFLARE_API_TOKEN" wrangler deploy\n',
           "token threaded through the environment"),
    benign("b-template-hole", "ci/deploy.yml",
           "with:\n  api_token: \"${{ secrets.DEPLOY_API_TOKEN }}\"\n",
           "CI secret reference"),
    benign("b-your-key-here", "README.md",
           'Set `api_key = "your-api-key-goes-right-here"` in config.\n',
           "instructional placeholder"),
    benign("b-ignore-marker", "tests/parser_test.py",
           'password = "%s"  # %s\n' % (
               synth("im1", 24), "forkmesh-secret-scan:" + "ignore-line"),
           "reviewed fixture carrying the inline ignore marker"),
    benign("b-ignore-marker-continuation", "tests/fixture.sh",
           "run --user demo \\\n  --password=\"%s\" \\\n  --verbose  # %s\n" % (
               synth("im2", 26), "forkmesh-secret-scan:" + "ignore-line"),
           "marker at the end of a continued logical line"),
    benign("b-allowlisted-fixture", "tests/fixtures/fake/github.txt",
           "ghp_%s\n" % synth("al1", 36),
           "provider-shaped fake inside the corpus policy's allowlisted path"),
    benign("b-short-password", "app/dev.py", 'password = "hunter2"\n',
           "too short to be a generated credential"),
    benign("b-prose", "docs/security.md",
           "Rotate each API key every 90 days and never share a password or "
           "access token in chat.\n", "prose about credentials"),
    benign("b-git-shas", "CHANGELOG.md",
           "- fix: %s\n- feat: %s\n" % (synth("sha1", 40, HEX),
                                        synth("sha2", 40, HEX)),
           "commit ids"),
    benign("b-sha256-digests", "release/SHA256SUMS",
           "%s  forkmesh.tar.gz\n%s  forkmesh.zip\n" % (
               synth("d1", 64, HEX), synth("d2", 64, HEX)),
           "release digests"),
    benign("b-uuids", "db/seed.sql",
           "INSERT INTO t VALUES ('%s-%s-4%s-a%s-%s');\n" % (
               synth("u1", 8, HEX), synth("u2", 4, HEX), synth("u3", 3, HEX),
               synth("u4", 3, HEX), synth("u5", 12, HEX)),
           "UUID literal"),
    benign("b-ssh-public-key", "keys/authorized_keys",
           "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAA%s dev@laptop\n"
           % synth("pub1", 43, BASE64),
           "public keys are not secrets"),
    benign("b-pem-public-key", "keys/public.pem", pem("PUBLIC KEY", "pub2"),
           "PEM public key"),
    benign("b-pem-certificate", "certs/ca.crt", pem("CERTIFICATE", "pub3"),
           "PEM certificate"),
    benign("b-lockfile-integrity", "web/package-lock.json",
           '"integrity": "sha512-%s=="\n' % synth("int1", 86, BASE64),
           "lockfile integrity hash"),
    benign("b-sk-lookalike-prose", "docs/risk.md",
           "The desk-side risk-assessment checklist covers task-level "
           "sk-rated items.\n", "words containing sk-"),
    benign("b-password-field-name", "web/form.js",
           "const passwordStrengthMeterEnabled = true;\n"
           "input.type = 'password';\n", "password as an identifier"),
    benign("b-aws-documentation-key", "docs/aws.md",
           "aws_access_key_id = AKIA" "IOSFODNN7EXAMPLE\n",
           "AWS's published documentation key id: provider rules have no "
           "placeholder filter, so this is flagged", known=True),
]

POLICY = {
    "schemaVersion": 1,
    "mode": "block",
    "allowlist": [{
        "paths": ["*/tests/fixtures/fake/*"],
        "rules": ["*"],
        "reason": "synthetic corpus fixtures",
    }],
}

BOUNDS = {
    # Measured on this corpus, including the documented known limitations;
    # a regression beyond these bounds fails the gate.
    "maxFalseNegativeRate": 0.15,
    "maxFalsePositiveRate": 0.10,
}


def fragments(text: str) -> list[str]:
    return [text[i:i + FRAGMENT] for i in range(0, len(text), FRAGMENT)]


def build() -> str:
    cases = []
    for case in CASES:
        item = {
            "id": case["id"],
            "label": case["label"],
            "path": case["path"],
            "note": case["note"],
            "content": fragments(case["content"]),
        }
        if case["rule"]:
            item["rule"] = case["rule"]
        if case["known"]:
            item["knownLimitation"] = list(BOUNDARIES)
        cases.append(item)
    document = {
        "id": "forkmesh-secret-corpus-v1",
        "contract": "forkmesh-secret-scan-v1",
        "generatedBy": "conformance/secrets/v1/build_corpus.py",
        "boundaries": BOUNDARIES,
        "bounds": BOUNDS,
        "policy": POLICY,
        "cases": cases,
    }
    return json.dumps(document, indent=1, ensure_ascii=True) + "\n"


def main(argv: list[str]) -> int:
    text = build()
    if "--check" in argv:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != text:
            print("corpus.json is stale; run build_corpus.py", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
