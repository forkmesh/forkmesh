#!/usr/bin/env python3
"""TLS for production's names on the Next cores, before the DNS flip."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.x509.oid import NameOID

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "deploy" / "core"))

LE_DIRECTORY = "https://acme-v02.api.letsencrypt.org/directory"
LE_STAGING = "https://acme-staging-v02.api.letsencrypt.org/directory"
CLOUDFLARE_API = "https://api.cloudflare.com/client/v4"
PRODUCTION_NAMES = ("app.forkmesh.com", "world.forkmesh.com")
TLS_DIR = "/etc/forkmesh-next/tls"
USER_AGENT = "fmnext-cutover-tls/1"


class TlsError(RuntimeError):
    pass


class GuardError(TlsError):
    pass


def log(message):
    print(message, flush=True)


def b64u(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def work_dir() -> Path:
    configured = os.environ.get("FMNEXT_CUTOVER_TLS_DIR")
    import next_common
    path = Path(configured) if configured else next_common.data_dir() / "cutover-tls"
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(path, 0o700)
    return path


def challenge_name(name: str) -> str:
    return "_acme-challenge." + name


def guard_txt_name(record_name: str, allowed) -> str:
    clean = str(record_name or "").strip().lower().rstrip(".")
    if clean not in {challenge_name(n) for n in allowed}:
        raise GuardError("refusing DNS record %r: only %s" % (
            record_name, ", ".join(challenge_name(n) for n in allowed)))
    return clean


class CloudflareTxt:

    def __init__(self, token, zone_id, allowed=PRODUCTION_NAMES, opener=None):
        if not zone_id or not all(c in "0123456789abcdef" for c in zone_id):
            raise TlsError("CLOUDFLARE_ZONE_ID must be the 32-hex zone id")
        self.token, self.zone, self.allowed = token, zone_id, tuple(allowed)
        self.opener = opener or urllib.request.urlopen
        self.created = {}

    @classmethod
    def from_config(cls, allowed=PRODUCTION_NAMES):
        import next_common as common
        return cls(common.credential("cloudflare.env", "CLOUDFLARE_API_TOKEN"),
                   common.credential("cloudflare.env", "CLOUDFLARE_ZONE_ID"),
                   allowed)

    def _call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(
            CLOUDFLARE_API + path, data=data, method=method, headers={
                "Authorization": "Bearer " + self.token,
                "Content-Type": "application/json", "User-Agent": USER_AGENT})
        try:
            with self.opener(request, timeout=30) as response:
                payload = json.loads(response.read() or b"{}")
        except urllib.error.HTTPError as error:
            raise TlsError("Cloudflare %s %s: HTTP %d" % (
                method, path.split("?")[0], error.code)) from None
        if not payload.get("success", False):
            raise TlsError("Cloudflare %s failed: %s" % (
                method, [e.get("code") for e in payload.get("errors") or []]))
        return payload.get("result")

    def set_txt(self, name, value):
        record = guard_txt_name(challenge_name(name), self.allowed)
        result = self._call("POST", "/zones/%s/dns_records" % self.zone, {
            "type": "TXT", "name": record, "content": value, "ttl": 60,
            "comment": "fmnext cutover_tls DNS-01; removed after validation"})
        self.created.setdefault(record, []).append(result["id"])
        return result["id"]

    def clear(self, name):
        record = guard_txt_name(challenge_name(name), self.allowed)
        for record_id in self.created.pop(record, []):
            live = self._call("GET", "/zones/%s/dns_records/%s" % (
                self.zone, record_id))
            if live.get("type") != "TXT":
                raise GuardError("record %s is not TXT; not deleting" % record_id)
            guard_txt_name(live.get("name"), self.allowed)
            self._call("DELETE", "/zones/%s/dns_records/%s" % (self.zone, record_id))

    def wait_visible(self, name, value, timeout=180, resolver=None):
        resolver = resolver or doh_txt
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if value in resolver(challenge_name(name)):
                return True
            time.sleep(3)
        raise TlsError("TXT for %s not visible after %d s" % (name, timeout))


def doh_txt(record_name):
    query = urllib.parse.urlencode({"name": record_name, "type": "TXT"})
    request = urllib.request.Request(
        "https://cloudflare-dns.com/dns-query?" + query,
        headers={"accept": "application/dns-json", "User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            answer = json.loads(response.read()).get("Answer") or []
    except (OSError, ValueError):
        return []
    return [a.get("data", "").strip('"') for a in answer]


class ChallTestSrv:

    def __init__(self, url="http://127.0.0.1:8055", allowed=PRODUCTION_NAMES):
        self.url, self.allowed = url.rstrip("/"), tuple(allowed)

    def _post(self, path, body):
        request = urllib.request.Request(self.url + path, data=json.dumps(
            body).encode(), method="POST")
        with urllib.request.urlopen(request, timeout=10):
            pass

    def set_txt(self, name, value):
        record = guard_txt_name(challenge_name(name), self.allowed)
        self._post("/set-txt", {"host": record + ".", "value": value})

    def clear(self, name):
        record = guard_txt_name(challenge_name(name), self.allowed)
        self._post("/clear-txt", {"host": record + "."})

    def wait_visible(self, name, value, timeout=0, resolver=None):
        return True


class Acme:
    def __init__(self, directory, ca_bundle=None, contact=None):
        self.directory_url = directory
        self.context = ssl.create_default_context(cafile=ca_bundle) \
            if ca_bundle else ssl.create_default_context()
        if ca_bundle:
            self.context.verify_flags &= ~getattr(ssl, "VERIFY_X509_STRICT", 0)
        self.key = ec.generate_private_key(ec.SECP256R1())
        numbers = self.key.public_key().public_numbers()
        self.jwk = {"crv": "P-256", "kty": "EC",
                    "x": b64u(numbers.x.to_bytes(32, "big")),
                    "y": b64u(numbers.y.to_bytes(32, "big"))}
        self.thumbprint = b64u(hashlib.sha256(json.dumps(
            self.jwk, sort_keys=True, separators=(",", ":")).encode()).digest())
        self.kid = None
        self.contact = contact
        self.directory = self._get(directory)[0]
        self.nonce = None

    def _open(self, request):
        try:
            return urllib.request.urlopen(request, timeout=60, context=self.context)
        except urllib.error.HTTPError as error:
            body = error.read()[:600]
            try:
                problem = json.loads(body)
                detail = "%s: %s" % (problem.get("type"), problem.get("detail"))
            except ValueError:
                detail = body.decode("utf-8", "replace")
            raise TlsError("ACME %s: HTTP %d %s" % (
                request.full_url, error.code, detail)) from None

    def _get(self, url):
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with self._open(request) as response:
            return json.loads(response.read() or b"{}"), response.headers

    def _new_nonce(self):
        request = urllib.request.Request(self.directory["newNonce"], method="HEAD",
                                         headers={"User-Agent": USER_AGENT})
        with self._open(request) as response:
            return response.headers["Replay-Nonce"]

    def post(self, url, payload, raw=False):
        nonce = self.nonce or self._new_nonce()
        protected = {"alg": "ES256", "nonce": nonce, "url": url}
        if self.kid:
            protected["kid"] = self.kid
        else:
            protected["jwk"] = self.jwk
        protected64 = b64u(json.dumps(protected).encode())
        payload64 = "" if payload is None else b64u(json.dumps(payload).encode())
        der = self.key.sign(("%s.%s" % (protected64, payload64)).encode(),
                            ec.ECDSA(hashes.SHA256()))
        r, s = decode_dss_signature(der)
        body = json.dumps({"protected": protected64, "payload": payload64,
                           "signature": b64u(r.to_bytes(32, "big") + s.to_bytes(32, "big"))})
        request = urllib.request.Request(url, data=body.encode(), method="POST", headers={
            "Content-Type": "application/jose+json", "User-Agent": USER_AGENT})
        with self._open(request) as response:
            self.nonce = response.headers.get("Replay-Nonce")
            data = response.read()
            headers = response.headers
        if raw:
            return data, headers
        return (json.loads(data) if data else {}), headers

    def register(self):
        payload = {"termsOfServiceAgreed": True}
        if self.contact:
            payload["contact"] = ["mailto:" + self.contact]
        _body, headers = self.post(self.directory["newAccount"], payload)
        self.kid = headers["Location"]

    def poll(self, url, done=("valid",), fail=("invalid",), timeout=180):
        deadline = time.monotonic() + timeout
        while True:
            body, _ = self.post(url, None)
            status = body.get("status")
            if status in done:
                return body
            if status in fail:
                raise TlsError("ACME %s is %s: %s" % (
                    url, status, json.dumps(body.get("challenges") or body.get("error"))[:400]))
            if time.monotonic() > deadline:
                raise TlsError("ACME %s still %s after %d s" % (url, status, timeout))
            time.sleep(2)

    def key_authorization(self, token):
        return "%s.%s" % (token, self.thumbprint)

    def dns_value(self, token):
        return b64u(hashlib.sha256(self.key_authorization(token).encode()).digest())


def make_csr(name):
    key = ec.generate_private_key(ec.SECP256R1())
    csr = (x509.CertificateSigningRequestBuilder()
           .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)]))
           .add_extension(x509.SubjectAlternativeName([x509.DNSName(name)]),
                          critical=False)
           .sign(key, hashes.SHA256()))
    key_pem = key.private_bytes(serialization.Encoding.PEM,
                                serialization.PrivateFormat.PKCS8,
                                serialization.NoEncryption())
    return key_pem, csr.public_bytes(serialization.Encoding.DER)


def issue_one(acme, name, dns):
    order, headers = acme.post(acme.directory["newOrder"], {
        "identifiers": [{"type": "dns", "value": name}]})
    order_url = headers["Location"]
    for authz_url in order["authorizations"]:
        authz, _ = acme.post(authz_url, None)
        if authz.get("status") == "valid":
            continue
        challenge = next((c for c in authz["challenges"] if c["type"] == "dns-01"),
                         None)
        if challenge is None:
            raise TlsError("%s: the CA offered no dns-01 challenge" % name)
        value = acme.dns_value(challenge["token"])
        dns.set_txt(name, value)
        try:
            dns.wait_visible(name, value)
            acme.post(challenge["url"], {})
            acme.poll(authz_url)
        finally:
            dns.clear(name)
    key_pem, csr_der = make_csr(name)
    acme.poll(order_url, done=("ready", "valid"))
    acme.post(order["finalize"], {"csr": b64u(csr_der)})
    order = acme.poll(order_url)
    cert_pem, _ = acme.post(order["certificate"], None, raw=True)
    return cert_pem, key_pem


def validate(name, cert_pem, key_pem, ca_file=None, max_days=100):
    import certsync
    return certsync.openssl_validate(name, cert_pem, key_pem, ca_file=ca_file,
                                     max_days=max_days)


def write_private(path: Path, data: bytes):
    fd = os.open(str(path), os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as handle:
        handle.write(data)
    os.chmod(path, 0o600)


def cert_summary(cert_pem):
    cert = x509.load_pem_x509_certificate(cert_pem)
    return "notAfter %s, issuer %s, sha256 %s" % (
        cert.not_valid_after_utc.strftime("%Y-%m-%d"),
        cert.issuer.rfc4514_string()[:60],
        cert.fingerprint(hashes.SHA256()).hex()[:16])


def issue(names, dns, directory, execute=False, ca_bundle=None,
          validate_ca=None, out_dir=None, contact=None):
    out_dir = Path(out_dir) if out_dir else work_dir()
    for name in names:
        guard_txt_name(challenge_name(name), getattr(dns, "allowed", PRODUCTION_NAMES))
    log("issue: %s from %s via DNS-01 (%s)" % (", ".join(names), directory,
                                              type(dns).__name__))
    if not execute:
        log("dry-run: would create TXT %s, validate, delete them, and write "
            "<name>.crt/.key (0600) under %s" % (
                ", ".join(challenge_name(n) for n in names), out_dir))
        return {}
    acme = Acme(directory, ca_bundle, contact)
    acme.register()
    results = {}
    for name in names:
        cert_pem, key_pem = issue_one(acme, name, dns)
        validate(name, cert_pem, key_pem, ca_file=validate_ca)
        write_private(out_dir / (name + ".key"), key_pem)
        write_private(out_dir / (name + ".crt"), cert_pem)
        results[name] = cert_summary(cert_pem)
        log("issue: %s: %s" % (name, results[name]))
    return results


PUSH_SCRIPT = (
    "install -d -o caddy -g caddy -m 0700 {tls}\n"
    "install -o caddy -g caddy -m 0600 /dev/stdin {tls}/.cutover-{name}.{ext}\n"
    "mv -f {tls}/.cutover-{name}.{ext} {tls}/{name}.{ext}\n")
SYNC_SCRIPT = ("systemctl start fmnext-cert-sync.service || true\n"
               "for f in {names}; do printf '%s ' \"$f\"; "
               "openssl x509 -in {tls}/$f.crt -noout -enddate 2>/dev/null "
               "|| echo missing; done\n")


def push(fleet, names, ssh, execute=False, only=(), source=None, tls=TLS_DIR):
    source = Path(source) if source else work_dir()
    pairs = {}
    for name in names:
        cert = (source / (name + ".crt")).read_bytes()
        key = (source / (name + ".key")).read_bytes()
        validate(name, cert, key)
        pairs[name] = (cert, key)
    nodes = [n for n in fleet["nodes"] if not only or n["name"] in only]
    for node in nodes:
        log("push: %s (%s): %s -> %s" % (node["name"], node["public_ipv4"],
                                          ", ".join(names), tls))
        if not execute:
            continue
        for name, (cert, key) in pairs.items():
            for ext, data in (("key", key), ("crt", cert)):
                ssh.run(node["public_ipv4"], PUSH_SCRIPT.format(
                    tls=tls, name=name, ext=ext), input_bytes=data)
        done = ssh.run(node["public_ipv4"], SYNC_SCRIPT.format(
            names=" ".join(names), tls=tls))
        log(done.stdout.decode("utf-8", "replace").strip())
    if not execute:
        log("dry-run: pass --execute")


def status(fleet, names, ssh, only=(), tls=TLS_DIR):
    for node in fleet["nodes"]:
        if only and node["name"] not in only:
            continue
        done = ssh.run(node["public_ipv4"], SYNC_SCRIPT.replace(
            "systemctl start fmnext-cert-sync.service || true\n", "").format(
                names=" ".join(names), tls=tls), check=False)
        log("%s: %s" % (node["name"], done.stdout.decode("utf-8", "replace").strip()))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("issue")
    p.add_argument("--execute", action="store_true")
    p.add_argument("--staging", action="store_true",
                   help="Let's Encrypt staging (untrusted; certsync refuses it)")
    p.add_argument("--directory", default=None, help="ACME directory URL")
    p.add_argument("--ca-bundle", default=None, help="TLS trust for the ACME server")
    p.add_argument("--validate-ca", default=None,
                   help="CA file for the pair check (rehearsal CAs only)")
    p.add_argument("--challtestsrv", default=None,
                   help="rehearsal: pebble-challtestsrv management URL instead "
                        "of Cloudflare")
    p.add_argument("--out", default=None)
    p.add_argument("--contact", default=None)
    p.add_argument("names", nargs="*", default=list(PRODUCTION_NAMES))
    for name in ("push", "status"):
        p = sub.add_parser(name)
        p.add_argument("nodes", nargs="*")
        if name == "push":
            p.add_argument("--execute", action="store_true")
            p.add_argument("--from", dest="source", default=None)
    args = parser.parse_args(argv)
    try:
        if args.command == "issue":
            if args.challtestsrv:
                dns = ChallTestSrv(args.challtestsrv, allowed=args.names)
            else:
                if sorted(args.names) != sorted(set(args.names)) or not set(
                        args.names) <= set(PRODUCTION_NAMES):
                    raise GuardError("with Cloudflare only %s" % ", ".join(PRODUCTION_NAMES))
                dns = CloudflareTxt.from_config() if args.execute else \
                    CloudflareTxt("dry-run", "0" * 32)
            directory = args.directory or (LE_STAGING if args.staging else LE_DIRECTORY)
            issue(args.names, dns, directory, args.execute, args.ca_bundle,
                  args.validate_ca, args.out, args.contact)
            return 0
        import next_common as common
        sys.path.insert(0, str(REPO / "deploy" / "core"))
        import render
        fleet = render.load_fleet(REPO / "deploy" / "fleet.toml")
        ssh = common.Ssh()
        if args.command == "push":
            push(fleet, list(PRODUCTION_NAMES), ssh, args.execute, args.nodes,
                 args.source)
        else:
            status(fleet, list(PRODUCTION_NAMES), ssh, args.nodes)
        return 0
    except (TlsError, OSError) as error:
        print("cutover_tls: %s" % error, file=sys.stderr)
        return 1
    except Exception as error:
        if type(error).__name__ in ("CertSyncError", "ToolError", "RenderError"):
            print("cutover_tls: %s" % error, file=sys.stderr)
            return 1
        raise


if __name__ == "__main__":
    sys.exit(main())
