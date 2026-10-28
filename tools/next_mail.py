#!/usr/bin/env python3
"""ForkMesh Next core mail (docs/MAIL_LOG.md "Sending from the cores").

    next_mail.py dkim [--execute]
    next_mail.py dns [--execute]
    next_mail.py rdns [--execute]
    next_mail.py install nN [--execute]
    next_mail.py restart nN [--execute]
    next_mail.py check nN
"""
from __future__ import annotations

import argparse
import base64
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_common as common  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

MAIL_DOMAIN = "forkmesh.com"
SELECTOR = "fmnext"
DKIM_NAME = "%s._domainkey.%s" % (SELECTOR, MAIL_DOMAIN)
TXT_NAMES = (MAIL_DOMAIN, DKIM_NAME)
SPF_KEEP = ("include:zohomail.com",)
SENDER = "founders@forkmesh.com"
SIGN_HEADERS = ("From", "Sender", "Reply-To", "Subject", "Date", "Message-ID",
                "To", "Cc", "MIME-Version", "Content-Type",
                "Content-Transfer-Encoding", "In-Reply-To", "References",
                "List-Id", "List-Unsubscribe", "List-Unsubscribe-Post",
                "Feedback-ID", "Auto-Submitted")
OVERSIGN_HEADERS = ("From", "Reply-To", "Subject", "Date", "Message-ID", "To",
                    "List-Unsubscribe", "List-Unsubscribe-Post", "Feedback-ID")
IDS_PATHS = ("/etc/passwd", "/etc/group", "/etc/shadow", "/etc/gshadow",
             "/etc/systemd/system", "/etc/hostname", "/etc/hosts",
             "/etc/cloud", "/etc/cron.d", "/etc/postfix", "/etc/opendkim",
             "/etc/opendkim.conf", "/etc/mailname", "/etc/default",
             "/usr/local/lib/fmnext")
IDS_ALLOW = ("users", "listeners", "processes", "modules")


def key_path():
    return common.config_dir() / "dkim" / (SELECTOR + ".private")


def dkim_public(private_path):
    der = subprocess.run(
        ["openssl", "rsa", "-in", str(private_path), "-pubout", "-outform",
         "DER"], check=True, capture_output=True).stdout
    return base64.b64encode(der).decode()


def dkim_txt():
    return "v=DKIM1; k=rsa; p=" + dkim_public(key_path())


def ensure_key(runner):
    path = key_path()
    if path.exists():
        runner.say("%s exists" % path)
        return

    def write():
        path.parent.mkdir(mode=0o700, exist_ok=True)
        old = os.umask(0o077)
        try:
            subprocess.run(["openssl", "genrsa", "-out", str(path), "2048"],
                           check=True, capture_output=True)
        finally:
            os.umask(old)
        os.chmod(path, 0o600)
    runner.step("generate the DKIM key %s (2048-bit RSA, 0600)" % path, write)


def mail_host(node):
    return "%s.next.%s" % (node["name"], MAIL_DOMAIN)


def cores(fleet):
    return [n for n in fleet["nodes"] if n.get("role", "core") == "core"]


def spf_value(fleet):
    parts = ["v=spf1"]
    for node in cores(fleet):
        parts.append("ip4:" + node["public_ipv4"])
        if node.get("public_ipv6"):
            parts.append("ip6:" + node["public_ipv6"])
    return " ".join(parts + list(SPF_KEEP) + ["~all"])


class TxtDns:
    def __init__(self):
        import next_dns
        next_dns.check_token_file(next_dns.token_path())
        token = common.credential(next_dns.TOKEN_FILE, "CLOUDFLARE_API_TOKEN")
        self.zone = common.credential(next_dns.TOKEN_FILE, "CLOUDFLARE_ZONE_ID")
        self.http = common.JsonHttp(next_dns.CLOUDFLARE_API, token)

    def records(self, name):
        if name not in TXT_NAMES:
            raise GuardError("refusing TXT %r" % name)
        result = self.http.request("GET", "/zones/%s/dns_records?type=TXT&name=%s"
                                   % (self.zone, name))
        return [r for r in result.get("result") or [] if r.get("name") == name]

    def put(self, name, content, match):
        if name not in TXT_NAMES:
            raise GuardError("refusing TXT %r" % name)
        body = {"type": "TXT", "name": name, "content": content, "ttl": 300}
        live = [r for r in self.records(name)
                if str(r.get("content", "")).strip('"').startswith(match)]
        if len(live) > 1:
            raise GuardError("%s has %d %s records" % (name, len(live), match))
        if live:
            if live[0]["content"].strip('"') == content:
                return "unchanged"
            self.http.request("PUT", "/zones/%s/dns_records/%s" % (
                self.zone, live[0]["id"]), body)
            return "updated"
        self.http.request("POST", "/zones/%s/dns_records" % self.zone, body)
        return "created"


def dns(fleet, runner):
    records = [(MAIL_DOMAIN, spf_value(fleet), "v=spf1"),
               (DKIM_NAME, dkim_txt(), "v=DKIM1")]
    client = TxtDns() if runner.execute else None
    for name, content, match in records:
        runner.step("TXT %s -> %s" % (name, content[:90]),
                    lambda n=name, c=content, m=match:
                    runner.say("  %s" % client.put(n, c, m)))


def rdns(fleet, runner):
    import next_fleet
    vultr = next_fleet.Vultr.from_config() if runner.execute else None
    for node in cores(fleet):
        for ip in (node["public_ipv4"], node.get("public_ipv6")):
            if ip:
                runner.step("PTR %s -> %s" % (ip, mail_host(node)),
                            lambda n=node, i=ip: vultr.set_reverse(
                                n["instance_id"], i, mail_host(n)))


def relay_for(fleet, node):
    name = node.get("mail_relay")
    if not name or not fleet:
        return None
    relay = next((n for n in cores(fleet) if n["name"] == name), None)
    if relay is None or relay is node or relay.get("mail_relay"):
        raise GuardError("%s relays via %s, which is not a direct-sending core"
                         % (node["name"], name))
    return relay


def relay_clients(fleet, node):
    if not fleet:
        return []
    return [n for n in cores(fleet) if n.get("mail_relay") == node["name"]]


def main_cf(node, fleet=None):
    relay = relay_for(fleet, node)
    clients = sorted(n["wg_ip"] for n in relay_clients(fleet, node))
    interfaces = ("127.0.0.1, [::1], " + node["wg_ip"]) if clients else (
        "loopback-only")
    networks = " ".join(["127.0.0.0/8", "[::1]/128"] +
                        [ip + "/32" for ip in clients])
    return """compatibility_level = 3.6
myhostname = {host}
myorigin = {domain}
mydestination =
relayhost = {relayhost}
inet_interfaces = {interfaces}
inet_protocols = all
smtp_address_preference = ipv4
mynetworks = {networks}
smtpd_relay_restrictions = permit_mynetworks, reject
smtpd_recipient_restrictions = permit_mynetworks, reject
smtpd_client_event_limit_exceptions = {exceptions}
smtpd_client_message_rate_limit = 300
anvil_rate_time_unit = 3600s
local_transport = error:local delivery is disabled
alias_maps =
alias_database =
sender_canonical_maps = regexp:/etc/postfix/sender_canonical
disable_vrfy_command = yes
smtpd_banner = $myhostname ESMTP
message_size_limit = 10485760
smtp_tls_security_level = may
smtp_tls_loglevel = 1
smtp_tls_CAfile = /etc/ssl/certs/ca-certificates.crt
smtp_tls_mandatory_protocols = >=TLSv1.2
smtp_tls_protocols = >=TLSv1.2
smtp_helo_name = $myhostname
milter_protocol = 6
milter_default_action = tempfail
smtpd_milters = unix:opendkim/opendkim.sock
non_smtpd_milters = $smtpd_milters
internal_mail_filter_classes = bounce
maximal_queue_lifetime = 2d
bounce_queue_lifetime = 1d
""".format(host=mail_host(node), domain=MAIL_DOMAIN,
           relayhost=("[%s]:25" % relay["wg_ip"]) if relay else "",
           interfaces=interfaces, networks=networks,
           exceptions=" ".join(ip + "/32" for ip in clients))


SENDER_CANONICAL = """if !/^(founders|no-reply)@forkmesh\\.com$/
/@([a-z0-9-]+\\.)*forkmesh\\.com$/ {sender}
endif
""".format(sender=SENDER)


OPENDKIM_CONF = """Syslog yes
UMask 007
Mode s
Canonicalization relaxed/relaxed
Domain {domain}
SubDomains yes
Selector {selector}
KeyFile /etc/opendkim/keys/{selector}.private
Socket local:/var/spool/postfix/opendkim/opendkim.sock
PidFile /run/opendkim/opendkim.pid
UserID opendkim
SignHeaders {sign}
OversignHeaders {oversign}
InternalHosts 127.0.0.1,::1
""".format(domain=MAIL_DOMAIN, selector=SELECTOR, sign=",".join(SIGN_HEADERS),
           oversign=",".join(OVERSIGN_HEADERS))

AGENT_PATH = "/usr/local/lib/fmnext/mail_events_agent.py"
AGENT_UNIT = """[Unit]
Description=ForkMesh Next mail delivery events (docs/MAIL_LOG.md)
After=postfix.service

[Service]
User=fmnext-mail
Group=forkmesh-app
SupplementaryGroups=systemd-journal
UMask=0027
ExecStart=/usr/bin/python3 {agent}
Restart=always
RestartSec=5
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
ProtectKernelTunables=yes
ProtectControlGroups=yes
RestrictAddressFamilies=AF_UNIX
ReadWritePaths=/var/lib/fmnext-mail-spool

[Install]
WantedBy=multi-user.target
""".format(agent=AGENT_PATH)

OPENDKIM_DROPIN = """[Service]
ExecStart=
ExecStart=/usr/sbin/opendkim -x /etc/opendkim.conf
"""

POSTFIX_DROPIN = """[Unit]
Wants=wg-quick@wg0.service
After=wg-quick@wg0.service
"""


def install_script(node, fleet=None):
    host = mail_host(node)
    files = {"/etc/postfix/main.cf": main_cf(node, fleet),
             "/etc/systemd/system/postfix.service.d/fmnext.conf": POSTFIX_DROPIN,
             "/etc/postfix/sender_canonical": SENDER_CANONICAL,
             "/etc/opendkim.conf": OPENDKIM_CONF,
             "/etc/systemd/system/opendkim.service.d/fmnext.conf": OPENDKIM_DROPIN,
             "/etc/cloud/cloud.cfg.d/99-fmnext-hostname.cfg":
                 "preserve_hostname: true\nmanage_etc_hosts: false\n",
             "/etc/mailname": MAIL_DOMAIN + "\n",
             "/etc/systemd/system/fmnext-mail-events.service": AGENT_UNIT,
             AGENT_PATH: (common.REPO / "deploy" / "core" /
                          "mail_events_agent.py").read_text()}
    writes = "".join(
        "mkdir -p %s\ncat > %s <<'FMNEXT_EOF'\n%sFMNEXT_EOF\n" % (
            os.path.dirname(p), p, body) for p, body in files.items())
    return """set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
hostnamectl set-hostname {host}
sed -i '/^127\\.0\\.1\\.1 {host} /d' /etc/hosts
grep -q ' {host}' /etc/hosts || echo "{ip} {host} {name}" >> /etc/hosts
echo "postfix postfix/main_mailer_type select No configuration" | debconf-set-selections
echo "postfix postfix/mailname string {domain}" | debconf-set-selections
apt-get install -y --no-install-recommends postfix opendkim
{writes}
install -d -m 0750 -o opendkim -g opendkim /etc/opendkim /etc/opendkim/keys
umask 077
cat > /etc/opendkim/keys/{selector}.private
chown opendkim:opendkim /etc/opendkim/keys/{selector}.private
chmod 0600 /etc/opendkim/keys/{selector}.private
install -d -m 0750 -o opendkim -g postfix /var/spool/postfix/opendkim
usermod -aG opendkim postfix
postfix check
id fmnext-mail >/dev/null 2>&1 || useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin fmnext-mail
install -d -m 2750 -o fmnext-mail -g forkmesh-app /var/lib/fmnext-mail-spool
chmod 0755 /usr/local/lib/fmnext
chmod 0644 {agent}
systemctl daemon-reload
systemctl enable --now opendkim postfix fmnext-mail-events
systemctl restart opendkim postfix fmnext-mail-events
sleep 2
systemctl is-active opendkim postfix fmnext-mail-events
""".format(host=host, name=node["name"], ip=node["public_ipv4"],
           domain=MAIL_DOMAIN,
           selector=SELECTOR, writes=writes, agent=AGENT_PATH)


def install(fleet, name, ssh, runner):
    import next_ids
    node = next_ids.target(fleet, name)
    if node.get("role", "core") != "core":
        raise GuardError("mail runs on the cores only")
    if not key_path().exists():
        raise ToolError("run next_mail.py dkim --execute first")
    next_ids.push_expectation(fleet, ssh, runner, name, rescan=IDS_PATHS,
                              allow=IDS_ALLOW, reason="core mail install",
                              minutes=30)
    runner.step("%s: hostname %s, postfix (loopback only) and opendkim"
                % (name, mail_host(node)),
                lambda: runner.say(ssh.run(
                    node["public_ipv4"], install_script(node, fleet),
                    input_bytes=key_path().read_bytes()).stdout.decode()))


def restart(fleet, name, ssh, runner, timeout=900):
    import next_dns
    import next_fleet
    import next_ids
    node = next_ids.target(fleet, name)
    vultr = next_fleet.Vultr.from_config()
    ip = node["public_ipv4"]
    voters = next_fleet.healthy_voters(next_fleet.cluster_nodes(fleet, ssh))
    if len(voters) < 3:
        raise GuardError("only %d healthy rqlite voters; restart needs all 3"
                         % len(voters))
    for args in (["drain", name, "--execute"],) if runner.execute else ():
        next_dns.main(args)
    if runner.execute:
        time.sleep(75)
    next_ids.push_expectation(fleet, ssh, runner, name, allow=("reboot",),
                              reason="Vultr halt and start", minutes=30)
    runner.step("%s: Vultr halt %s" % (name, node["instance_id"]),
                lambda: vultr.halt(node["instance_id"]))

    def wait_stopped():
        deadline = time.time() + timeout
        while time.time() < deadline:
            if vultr.instance(node["instance_id"]).get("power_status") == "stopped":
                return
            time.sleep(10)
        raise ToolError("%s did not stop" % name)
    runner.step("%s: wait until stopped" % name, wait_stopped)
    runner.step("%s: Vultr start" % name, lambda: vultr.start(node["instance_id"]))

    def wait_ready():
        deadline = time.time() + timeout
        while time.time() < deadline:
            time.sleep(15)
            try:
                subprocess.run([sys.executable, str(Path(__file__).parent /
                                "next_vault.py"), "unseal", "--if-sealed",
                                "--execute"], check=False, timeout=120)
                if next_fleet.node_ready(ssh, ip):
                    return
            except Exception:
                pass
        raise ToolError("%s did not come back ready" % name)
    runner.step("%s: wait for boot, unseal, readyz" % name, wait_ready)
    if runner.execute:
        next_dns.main(["undrain", name, "--execute"])


def check(fleet, name, ssh):
    import next_ids
    node = next_ids.target(fleet, name)
    relay = relay_for(fleet, node)
    probe = ("timeout 8 bash -c '</dev/tcp/{ip}/25' && echo relay-{name}-open "
             "|| echo relay-{name}-unreachable\n".format(
                 ip=relay["wg_ip"], name=relay["name"]) if relay else "")
    out = ssh.run(node["public_ipv4"], """hostname -f
systemctl is-active postfix opendkim
ss -ltnH 'sport = :25'
timeout 8 bash -c '</dev/tcp/gmail-smtp-in.l.google.com/25' && echo outbound-25-open || echo outbound-25-blocked
{probe}postconf -h relayhost
postqueue -p | tail -1
""".format(probe=probe), check=False, timeout=60).stdout.decode()
    print(out.strip())
    for ip in (node["public_ipv4"], node.get("public_ipv6")):
        if ip:
            print("PTR %s: %s" % (ip, ptr_status(ip, mail_host(node))))


def ptr_status(ip, host, lookup=None, forward=None):
    import ipaddress
    import socket
    lookup = lookup or socket.gethostbyaddr
    forward = forward or (lambda name: {
        a[4][0] for a in socket.getaddrinfo(name, None)})
    try:
        name = lookup(ip)[0].rstrip(".").lower()
    except OSError as error:
        return "no PTR (%s)" % error
    if name != host:
        return "wrong: %s, want %s (rerun rdns --execute)" % (name, host)
    try:
        addresses = {ipaddress.ip_address(a) for a in forward(host)}
    except OSError as error:
        return "%s does not resolve (%s)" % (host, error)
    if ipaddress.ip_address(ip) not in addresses:
        return "%s does not resolve back to it" % host
    return "ok, %s" % host


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("dkim", "dns", "rdns"):
        common.add_execute_flags(sub.add_parser(command))
    for command in ("install", "restart"):
        p = sub.add_parser(command)
        p.add_argument("node")
        common.add_execute_flags(p)
    sub.add_parser("check").add_argument("node")
    args = parser.parse_args(argv)
    try:
        runner = common.runner_from_args(args)
        fleet = common.load_fleet()
        if args.command == "dkim":
            ensure_key(runner)
            if key_path().exists():
                print("%s TXT %s" % (DKIM_NAME, dkim_txt()))
        elif args.command == "dns":
            dns(fleet, runner)
        elif args.command == "rdns":
            rdns(fleet, runner)
        elif args.command == "install":
            install(fleet, args.node, common.Ssh(), runner)
        elif args.command == "restart":
            restart(fleet, args.node, common.Ssh(), runner)
        else:
            check(fleet, args.node, common.Ssh())
    except (GuardError, ToolError) as error:
        print("next_mail: %s" % error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
