from __future__ import annotations

import argparse
import asyncio
import base64
import ipaddress
import json
import os
import re
import signal
import tempfile

WIREGUARD = ipaddress.ip_network("10.77.0.0/24")
KEY_TYPES = ("ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
             "ecdsa-sha2-nistp521", "rsa-sha2-512", "rsa-sha2-256", "ssh-rsa")
KEY_BLOB_RE = re.compile(r"^[A-Za-z0-9+/]{16,2048}={0,2}$")
USER_RE = re.compile(r"^[a-z_][a-z0-9_.-]{0,31}$")
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
TOKEN_RE = re.compile(r"^fmrt_[0-9a-f]{64}$")
URL_RE = re.compile(r"^https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?/[A-Za-z0-9._/-]*$")
ORIGIN_RE = re.compile(r"^https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?$")
MAX_REQUEST = 512 * 1024
KEYSCAN_TIMEOUT = 20
INSTALL_TIMEOUT = 20 * 60
INSTANCE_TIMEOUT = 50 * 60
MAX_CONFIG = 384 * 1024
INSTANCE_URL_RE = re.compile(
    r"^https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?/[A-Za-z0-9._/-]*\.sh$")
ASKPASS = "#!/bin/sh\nprintf '%s\\n' \"$FORKMESH_SSH_SECRET\"\n"
SSH = "ssh"
SSH_KEYSCAN = "ssh-keyscan"
SSH_AGENT = "ssh-agent"
SSH_ADD = "ssh-add"


def public_address(value):
    try:
        address = ipaddress.ip_address(str(value))
    except ValueError:
        return None
    if address.version == 6 and address.ipv4_mapped is not None:
        return None
    if not address.is_global or address in WIREGUARD:
        return None
    return address


def host_pattern(address, port):
    return address if port == 22 else "[%s]:%d" % (address, port)


def valid_key(key):
    if not isinstance(key, dict):
        return False
    blob = str(key.get("key") or "")
    if key.get("type") not in KEY_TYPES or not KEY_BLOB_RE.fullmatch(blob):
        return False
    try:
        base64.b64decode(blob, validate=True)
    except ValueError:
        return False
    return True


def shq(value):
    return "'" + str(value).replace("'", "'\\''") + "'"


def remote_command(request, user, sudo_password):
    line = "curl -fsSL %s | %sbash -s -- --token %s --name %s --app %s" % (
        shq(request["installUrl"]),
        "" if user == "root" else "sudo -n ",
        shq(request["token"]), shq(request["name"]), shq(request["appUrl"]))
    if user == "root":
        return line
    if sudo_password:
        return "sudo -S -p '' -v && " + line
    return ("sudo -n true 2>/dev/null || { echo 'This user needs passwordless "
            "sudo, or use root, or sign in with the password.' >&2; exit 12; }"
            " && " + line)


def instance_command(request, user, sudo_password):
    run = 'bash "$t" --config-stdin' if user == "root" else (
        'sudo -n bash "$t" --config-stdin')
    line = ('t=$(mktemp) && curl -fsSL --proto =https --retry 3 %s -o "$t" '
            '&& %s; rc=$?; rm -f "$t"; exit $rc') % (
                shq(request["installUrl"]), run)
    if user == "root":
        return line
    if sudo_password:
        return "sudo -S -p '' -v && " + line
    return ("sudo -n true 2>/dev/null || { echo 'This user needs passwordless "
            "sudo, or use root, or sign in with the password.' >&2; exit 12; }"
            " && " + line)


def instance_config_line(config):
    raw = json.dumps(config, separators=(",", ":"), sort_keys=True)
    return base64.b64encode(raw.encode("utf-8")) + b"\n"


def config_secrets(config):
    found = []
    for key in ("adminPassword", "mailApiKey"):
        value = str(config.get(key) or "")
        if value:
            found.append(value)
    for item in config.get("owners") or []:
        value = str((item or {}).get("password") or "") \
            if isinstance(item, dict) else ""
        if value:
            found.append(value)
    return found


def host_key_algorithms(key_type):
    if key_type in ("ssh-rsa", "rsa-sha2-512", "rsa-sha2-256"):
        return "rsa-sha2-512,rsa-sha2-256"
    return key_type


def ssh_options(known_hosts, key_type, port):
    return [
        "-F", "/dev/null",
        "-o", "StrictHostKeyChecking=yes",
        "-o", "UserKnownHostsFile=" + known_hosts,
        "-o", "GlobalKnownHostsFile=/dev/null",
        "-o", "HostKeyAlgorithms=" + host_key_algorithms(key_type),
        "-o", "UpdateHostKeys=no",
        "-o", "CheckHostIP=no",
        "-o", "ConnectTimeout=12",
        "-o", "ServerAliveInterval=15",
        "-o", "ServerAliveCountMax=4",
        "-o", "NumberOfPasswordPrompts=1",
        "-o", "ForwardAgent=no",
        "-o", "ForwardX11=no",
        "-o", "ClearAllForwardings=yes",
        "-o", "PermitLocalCommand=no",
        "-o", "LogLevel=ERROR",
        "-p", str(port),
    ]


def write_private(directory, name, text, mode):
    path = os.path.join(directory, name)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    with os.fdopen(fd, "w") as handle:
        handle.write(text)
    return path


def remove_files(paths, directory):
    for path in paths:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    try:
        os.rmdir(directory)
    except OSError:
        pass


async def send(writer, message):
    writer.write(json.dumps(message).encode("utf-8") + b"\n")
    await writer.drain()


async def keyscan(request, writer):
    address = public_address(request.get("address"))
    port = request.get("port")
    if address is None or not isinstance(port, int) or not 0 < port < 65536:
        await send(writer, {"error": "refused address"})
        return
    try:
        process = await asyncio.create_subprocess_exec(
            SSH_KEYSCAN, "-T", "8", "-p", str(port), "-t",
            "ed25519,ecdsa,rsa", str(address),
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL)
        output, _ = await asyncio.wait_for(process.communicate(),
                                           KEYSCAN_TIMEOUT)
    except (OSError, asyncio.TimeoutError):
        await send(writer, {"error": "no answer"})
        return
    keys = []
    for line in output.decode("utf-8", "replace").splitlines():
        parts = line.split()
        if len(parts) >= 3 and not line.startswith("#"):
            key = {"type": parts[1], "key": parts[2]}
            if valid_key(key) and key not in keys:
                keys.append(key)
    await send(writer, {"keys": keys})


def validate_connection(request):
    address = public_address(request.get("address"))
    port = request.get("port")
    user = str(request.get("user") or "")
    ok = (address is not None and isinstance(port, int) and 0 < port < 65536
          and valid_key(request.get("hostKey")) and USER_RE.fullmatch(user)
          and bool(request.get("password")) != bool(request.get("privateKey")))
    return str(address) if ok else ""


def validate_install(request):
    address = validate_connection(request)
    ok = (address
          and TOKEN_RE.fullmatch(str(request.get("token") or ""))
          and NAME_RE.fullmatch(str(request.get("name") or ""))
          and URL_RE.fullmatch(str(request.get("installUrl") or ""))
          and ORIGIN_RE.fullmatch(str(request.get("appUrl") or "")))
    return address if ok else ""


def validate_instance(request):
    address = validate_connection(request)
    config = request.get("config")
    ok = (address
          and INSTANCE_URL_RE.fullmatch(str(request.get("installUrl") or ""))
          and isinstance(config, dict)
          and len(json.dumps(config)) <= MAX_CONFIG)
    return address if ok else ""


LINE_BREAK = re.compile(rb"\r\n|\r|\n")
MAX_PENDING = 8192


async def send_line(writer, raw, secrets):
    text = raw.decode("utf-8", "replace")
    for secret in secrets:
        if secret and len(secret) >= 4:
            text = text.replace(secret, "[hidden]")
    await send(writer, {"out": text[:2000]})


async def stream(process, writer, secrets):
    pending = b""
    while True:
        chunk = await process.stdout.read(4096)
        if not chunk:
            break
        pending += chunk
        parts = LINE_BREAK.split(pending)
        pending = parts.pop()
        if len(pending) > MAX_PENDING:
            parts.append(pending)
            pending = b""
        for part in parts:
            await send_line(writer, part, secrets)
    if pending:
        await send_line(writer, pending, secrets)


async def install(request, writer):
    instance = request.get("op") == "instance"
    address = (validate_instance if instance else validate_install)(request)
    if not address:
        await send(writer, {"exit": 2, "error": "The request was refused."})
        return
    port = request["port"]
    user = request["user"]
    password = str(request.get("password") or "")
    private_key = str(request.get("privateKey") or "")
    passphrase = str(request.get("passphrase") or "")
    host_key = request["hostKey"]
    if instance:
        config = request["config"]
        secrets = [password, passphrase] + config_secrets(config)
        command = instance_command(request, user, bool(password))
        payload = instance_config_line(config)
        config.clear()
        timeout = INSTANCE_TIMEOUT
    else:
        secrets = [password, passphrase, str(request["token"])]
        command = remote_command(request, user, bool(password))
        payload = b""
        timeout = INSTALL_TIMEOUT
    request.clear()
    directory = tempfile.mkdtemp(prefix="fm-ssh-")
    files = []
    agent = None
    process = None
    try:
        known = write_private(directory, "known_hosts", "%s %s %s\n" % (
            host_pattern(address, port), host_key["type"], host_key["key"]),
            0o600)
        askpass = write_private(directory, "askpass", ASKPASS, 0o700)
        files += [known, askpass]
        env = {
            "PATH": "/usr/bin:/bin",
            "HOME": directory,
            "LANG": "C.UTF-8",
            "DISPLAY": ":0",
            "SSH_ASKPASS": askpass,
            "SSH_ASKPASS_REQUIRE": "force",
        }
        argv = [SSH] + ssh_options(known, host_key["type"], port)
        if password:
            env["FORKMESH_SSH_SECRET"] = password
            argv += ["-o", "PreferredAuthentications=password,keyboard-interactive",
                     "-o", "PubkeyAuthentication=no", "-o", "IdentityAgent=none",
                     "-o", "IdentitiesOnly=yes"]
        else:
            sock = os.path.join(directory, "agent.sock")
            files.append(sock)
            agent = await asyncio.create_subprocess_exec(
                SSH_AGENT, "-D", "-a", sock,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL, env=env)
            for _ in range(50):
                if os.path.exists(sock):
                    break
                await asyncio.sleep(0.05)
            add_env = dict(env, SSH_AUTH_SOCK=sock,
                           FORKMESH_SSH_SECRET=passphrase)
            adder = await asyncio.create_subprocess_exec(
                SSH_ADD, "-q", "-", stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.PIPE, env=add_env)
            _, add_error = await asyncio.wait_for(adder.communicate(
                private_key.encode("utf-8") + b"\n"), 20)
            add_env = None
            private_key = passphrase = ""
            if adder.returncode != 0:
                await send(writer, {"exit": 3, "error": "The private key was "
                                    "refused (wrong passphrase or format)."})
                return
            argv += ["-o", "IdentityAgent=" + sock, "-o",
                     "PreferredAuthentications=publickey",
                     "-o", "PasswordAuthentication=no"]
        argv += ["-l", user, address, command]
        process = await asyncio.create_subprocess_exec(
            *argv, stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT, env=env,
            start_new_session=True)
        env = None
        if password and user != "root":
            process.stdin.write(password.encode("utf-8") + b"\n")
        if payload:
            process.stdin.write(payload)
        await process.stdin.drain()
        process.stdin.close()
        password = ""
        payload = b""
        await send(writer, {"out": "Connected to %s as %s; host key %s "
                                   "verified." % (address, user,
                                                  host_key["type"])})
        try:
            await asyncio.wait_for(stream(process, writer, secrets), timeout)
            code = await asyncio.wait_for(process.wait(), 30)
        except asyncio.TimeoutError:
            os.killpg(process.pid, signal.SIGKILL)
            await send(writer, {"exit": -1, "error": "The setup took too long."})
            return
        if code == 255:
            await send(writer, {"exit": code, "error": "SSH could not sign in "
                                "or the host key did not match."})
        elif code == 12:
            await send(writer, {"exit": code, "error": "This user needs "
                                "passwordless sudo, or use root, or the "
                                "password."})
        else:
            await send(writer, {"exit": code})
    finally:
        secrets.clear()
        if process and process.returncode is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        if agent and agent.returncode is None:
            agent.kill()
            await agent.wait()
        remove_files(files, directory)


async def serve_client(reader, writer):
    try:
        line = await asyncio.wait_for(reader.readline(), 10)
        if not line or len(line) > MAX_REQUEST:
            return
        try:
            request = json.loads(line)
        except ValueError:
            return
        line = b""
        if not isinstance(request, dict):
            return
        if request.get("op") == "keyscan":
            await keyscan(request, writer)
        elif request.get("op") in ("install", "instance"):
            await install(request, writer)
    except (ConnectionError, asyncio.TimeoutError):
        pass
    finally:
        writer.close()


def systemd_socket():
    if os.environ.get("LISTEN_PID") != str(os.getpid()) or os.environ.get(
            "LISTEN_FDS") != "1":
        return None
    import socket
    return socket.socket(fileno=3)


async def main_async(path, inherited=None):
    if inherited is not None:
        server = await asyncio.start_unix_server(
            serve_client, sock=inherited, limit=MAX_REQUEST + 1)
    else:
        old = os.umask(0o117)
        try:
            server = await asyncio.start_unix_server(
                serve_client, path=path, limit=MAX_REQUEST + 1)
        finally:
            os.umask(old)
    async with server:
        await server.serve_forever()


def main(argv=None):
    parser = argparse.ArgumentParser(prog="forkmesh_host.ssh_helper")
    parser.add_argument("--socket", default="")
    parser.add_argument("--systemd-socket", action="store_true")
    args = parser.parse_args(argv)
    if args.systemd_socket:
        inherited = systemd_socket()
        if inherited is None:
            raise SystemExit("no socket from systemd")
        asyncio.run(main_async("", inherited))
        return
    if not args.socket:
        raise SystemExit("--socket or --systemd-socket is required")
    if os.path.lexists(args.socket):
        os.unlink(args.socket)
    asyncio.run(main_async(args.socket))


if __name__ == "__main__":
    main()
