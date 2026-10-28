import re
import socket

import next_mail


def test_dkim_signs_one_click_unsubscribe_and_relaxed_bodies():
    conf = dict(line.split(" ", 1) for line in
                next_mail.OPENDKIM_CONF.strip().splitlines())
    assert conf["Canonicalization"] == "relaxed/relaxed"
    signed = conf["SignHeaders"].split(",")
    for header in ("From", "Subject", "Date", "Message-ID", "To",
                   "List-Unsubscribe", "List-Unsubscribe-Post", "Feedback-ID"):
        assert header in signed
    oversigned = conf["OversignHeaders"].split(",")
    assert {"From", "List-Unsubscribe", "List-Unsubscribe-Post"} <= set(oversigned)
    assert set(oversigned) <= set(signed)


def test_postfix_bounce_notices_are_dkim_signed():
    conf = dict(line.split(" ", 1) for line in
                next_mail.OPENDKIM_CONF.strip().splitlines())
    assert conf["Domain"] == "forkmesh.com"
    assert conf["SubDomains"] == "yes"
    cf = next_mail.main_cf({"name": "n4"})
    assert "non_smtpd_milters = $smtpd_milters\n" in cf
    assert "internal_mail_filter_classes = bounce\n" in cf


def canonical(address):
    lines = next_mail.SENDER_CANONICAL.strip().splitlines()
    keep = re.compile(lines[0][len("if !/"):-1], re.I)
    pattern, _, replacement = lines[1].partition(" ")
    if keep.search(address) or not re.search(pattern[1:-1], address, re.I):
        return address
    return replacement


def test_server_mail_goes_out_as_the_founders_mailbox():
    assert "sender_canonical_maps = regexp:/etc/postfix/sender_canonical\n" in (
        next_mail.main_cf({"name": "n4"}))
    for system in ("root@forkmesh.com", "MAILER-DAEMON@n4.next.forkmesh.com",
                   "postmaster@n3.next.forkmesh.com", "fmnext-mail@forkmesh.com"):
        assert canonical(system) == "founders@forkmesh.com"
    for kept in ("founders@forkmesh.com", "no-reply@forkmesh.com",
                 "No-Reply@ForkMesh.com", "someone@example.org",
                 "x@forkmesh.com.example.org"):
        assert canonical(kept) == kept
    script = next_mail.install_script({"name": "n4",
                                       "public_ipv4": "203.0.113.4"})
    assert "cat > /etc/postfix/sender_canonical <<'FMNEXT_EOF'\n%sFMNEXT_EOF" % (
        next_mail.SENDER_CANONICAL) in script


def test_postfix_prefers_ipv4_and_keeps_tls():
    cf = next_mail.main_cf({"name": "n1"})
    assert "smtp_address_preference = ipv4\n" in cf
    assert "smtp_tls_security_level = may\n" in cf
    assert "myhostname = n1.next.forkmesh.com\n" in cf
    assert "milter_default_action = tempfail\n" in cf


def test_ptr_status_needs_forward_confirmed_reverse_dns():
    host = "n3.next.forkmesh.com"
    ok = next_mail.ptr_status("203.0.113.203", host,
                              lambda ip: (host + ".", [], [ip]),
                              lambda name: {"203.0.113.203"})
    assert ok == "ok, " + host
    generic = next_mail.ptr_status(
        "203.0.113.203", host,
        lambda ip: ("203-0-113-203.constant.com", [], [ip]),
        lambda name: {"203.0.113.203"})
    assert generic.startswith("wrong: 203-0-113-203.constant.com")
    assert "does not resolve back" in next_mail.ptr_status(
        "203.0.113.203", host, lambda ip: (host, [], [ip]),
        lambda name: {"203.0.113.201"})
    v6 = "2001:db8:f1ee::2"
    assert next_mail.ptr_status(
        v6, host, lambda ip: (host, [], [ip]),
        lambda name: {"2001:db8:f1ee::2"}) == "ok, " + host

    def missing(_ip):
        raise socket.herror(1, "Unknown host")
    assert next_mail.ptr_status(v6, host, missing).startswith("no PTR")


FLEET = {"nodes": [
    {"name": "n3", "public_ipv4": "203.0.113.3", "wg_ip": "10.77.0.3"},
    {"name": "n4", "public_ipv4": "203.0.113.4", "wg_ip": "10.77.0.4"},
    {"name": "n5", "public_ipv4": "203.0.113.5", "wg_ip": "10.77.0.5"},
]}


def test_cores_without_ipv4_reverse_dns_relay_through_n4_over_wireguard():
    n3, n4, n5 = FLEET["nodes"]
    for client in (n3, n5):
        cf = next_mail.main_cf(client, FLEET)
        assert "relayhost = [10.77.0.4]:25\n" in cf
        assert "inet_interfaces = loopback-only\n" in cf
        assert "mynetworks = 127.0.0.0/8 [::1]/128\n" in cf
        assert "smtpd_milters = unix:opendkim/opendkim.sock\n" in cf
    relay = next_mail.main_cf(n4, FLEET)
    assert "relayhost = \n" in relay
    assert "inet_interfaces = 127.0.0.1, [::1], 10.77.0.4\n" in relay
    assert ("mynetworks = 127.0.0.0/8 [::1]/128 10.77.0.3/32 10.77.0.5/32\n"
            in relay)
    assert ("smtpd_client_event_limit_exceptions = 10.77.0.3/32 10.77.0.5/32\n"
            in relay)
    assert "smtpd_relay_restrictions = permit_mynetworks, reject\n" in relay


def test_a_core_alone_sends_directly_on_loopback():
    cf = next_mail.main_cf({"name": "n4", "wg_ip": "10.77.0.4"},
                           {"nodes": [FLEET["nodes"][1]]})
    assert "relayhost = \n" in cf
    assert "inet_interfaces = loopback-only\n" in cf
    assert "smtpd_client_event_limit_exceptions = \n" in cf


def test_a_relay_that_is_missing_or_relays_itself_is_refused():
    import pytest
    with pytest.raises(next_mail.GuardError):
        next_mail.main_cf(FLEET["nodes"][0], {"nodes": [FLEET["nodes"][0]]})


def test_postfix_starts_after_wireguard():
    script = next_mail.install_script(FLEET["nodes"][1], FLEET)
    assert ("cat > /etc/systemd/system/postfix.service.d/fmnext.conf "
            "<<'FMNEXT_EOF'\n" + next_mail.POSTFIX_DROPIN) in script
    assert "After=wg-quick@wg0.service\n" in next_mail.POSTFIX_DROPIN
    assert "inet_interfaces = 127.0.0.1, [::1], 10.77.0.4\n" in script
