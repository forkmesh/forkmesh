import asyncio
import smtplib
import socket
from email.message import EmailMessage
from email.utils import formataddr, formatdate, make_msgid

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 25
TIMEOUT_SECONDS = 20
FEEDBACK_SENDER = "forkmesh"


def enabled(env):
    return (getattr(env, "MAIL_TRANSPORT", "") or "").strip().lower() == "smtp"


def node():
    try:
        return socket.gethostname().split(".")[0].strip().lower()
    except OSError:
        return ""


def _header(value, limit=300):
    return " ".join(str(value or "").split())[:limit]


def _feedback_part(value):
    return "".join(c for c in str(value or "") if c.isalnum() or c in "_-")[:40]


def build(sender, sender_name, to_email, subject, text, html=None, kind="",
          headers=None):
    domain = sender.rsplit("@", 1)[-1] or "forkmesh.com"
    message = EmailMessage()
    message["From"] = formataddr((_header(sender_name, 80), _header(sender, 200)))
    message["To"] = _header(to_email, 320)
    message["Subject"] = _header(subject, 300)
    message["Date"] = formatdate(usegmt=True)
    message["Message-ID"] = make_msgid(domain=domain)
    message["Auto-Submitted"] = "auto-generated"
    if kind:
        message["X-ForkMesh-Kind"] = _header(kind, 40)
        message["Feedback-ID"] = "%s:%s" % (_feedback_part(kind), FEEDBACK_SENDER)
    for name, value in (headers or {}).items():
        message[name] = _header(value, 900)
    message.set_content(str(text or ""))
    if html:
        message.add_alternative(str(html), subtype="html")
        for part in message.iter_parts():
            del part["MIME-Version"]
    return message


def _deliver(host, port, message, timeout):
    with smtplib.SMTP(host, port, timeout=timeout) as client:
        refused = client.send_message(message)
    if refused:
        raise smtplib.SMTPRecipientsRefused(refused)


async def send(env, message):
    host = (getattr(env, "MAIL_SMTP_HOST", "") or DEFAULT_HOST).strip()
    if host not in ("127.0.0.1", "::1", "localhost"):
        raise ValueError("MAIL_SMTP_HOST must be loopback")
    port = int(getattr(env, "MAIL_SMTP_PORT", "") or DEFAULT_PORT)
    await asyncio.to_thread(_deliver, host, port, message, TIMEOUT_SECONDS)
    return str(message["Message-ID"])
