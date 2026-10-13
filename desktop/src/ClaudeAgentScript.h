#pragma once


#include <QString>

inline QString forkmeshClaudeAgentScript()
{
    return QString::fromUtf8(R"PYAGENT(#!/usr/bin/env python3
"""Minimal Claude coding agent for ForkMesh (Anthropic Messages API, stdlib only)."""
import contextlib
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

API_URL = "https://api.anthropic.com/v1/messages"
API_VERSION = "2023-06-01"
MAX_TURNS = 60
TOOL_OUTPUT_LIMIT = 30000
GATE_TIMEOUT = 880
GATE_MAX_REPLY = 65536


def command_environment():
    """Build the credential-free environment used by model-run commands."""
    sensitive_words = re.compile(
        r"(?:^|_)(?:AUTH|TOKEN|SECRET|PASSWORD|PASSWD|CREDENTIALS?"
        r"|API_KEY|PRIVATE_KEY|ACCESS_KEY|SESSION|COOKIE|DSN|JWT|PAT"
        r"|DATABASE_URL|CONNECTION_STRING)(?:_|$)", re.IGNORECASE)
    sensitive_names = {
        "AWS_CONFIG_FILE", "AWS_SHARED_CREDENTIALS_FILE",
        "AWS_WEB_IDENTITY_TOKEN_FILE", "AZURE_AUTH_LOCATION",
        "BASH_ENV", "DOCKER_CONFIG", "DYLD_INSERT_LIBRARIES", "ENV",
        "GIT_ASKPASS", "GIT_PROXY_COMMAND", "GIT_SSH", "GIT_SSH_COMMAND",
        "GOOGLE_APPLICATION_CREDENTIALS", "KUBECONFIG", "NETRC",
        "LD_PRELOAD", "NPM_CONFIG_USERCONFIG", "PIP_CONFIG_FILE", "SSH_ASKPASS",
        "SSH_AUTH_SOCK",
    }
    clean = {name: value for name, value in os.environ.items()
             if name.upper() not in sensitive_names
             and not name.upper().startswith("GIT_CONFIG_")
             and not name.upper().startswith("FORKMESH_AGENT_GATE")
             and not sensitive_words.search(name)}
    clean["HOME"] = COMMAND_HOME.name
    clean["USERPROFILE"] = COMMAND_HOME.name
    clean["XDG_CONFIG_HOME"] = os.path.join(COMMAND_HOME.name, ".config")
    clean["GIT_TERMINAL_PROMPT"] = "0"

    values = []
    for key in ("name", "email"):
        result = subprocess.run(
            ["git", "config", "--get", "user." + key], text=True,
            capture_output=True)
        if result.returncode == 0 and result.stdout.strip():
            values.append((key, result.stdout.strip().splitlines()[0]))
    if values:
        config = os.path.join(COMMAND_HOME.name, "gitconfig")
        with open(config, "w", encoding="utf-8") as stream:
            stream.write("[user]\n" + "".join(
                "\t%s = %s\n" % pair for pair in values))
        os.chmod(config, 0o600)
        clean["GIT_CONFIG_GLOBAL"] = config
    else:
        clean.pop("GIT_CONFIG_GLOBAL", None)
    return clean


COMMAND_HOME = tempfile.TemporaryDirectory(prefix="forkmesh-claude-agent-")
COMMAND_ENV = command_environment()


def protect_process_credentials():
    """Prevent child commands from reading this process through Linux /proc."""
    if not sys.platform.startswith("linux"):
        return
    try:
        import ctypes
        ctypes.CDLL(None, use_errno=True).prctl(4, 0, 0, 0, 0)
    except Exception:
        pass


def log(text):
    sys.stdout.write(text)
    sys.stdout.write("\n")
    sys.stdout.flush()


def read_prompt():
    # Prompt may arrive as an argument path or on stdin.
    for arg in sys.argv[1:]:
        if arg and os.path.isfile(arg):
            with open(arg, "r", encoding="utf-8") as handle:
                return handle.read()
    data = sys.stdin.read()
    return data


def tool_families(name):
    found = []
    for part in os.environ.get(name, "").split(","):
        family = part.strip().lower()
        if family and family not in found:
            found.append(family)
    return found


def bash_permitted():
    disabled = tool_families("FORKMESH_AGENT_DISABLED_TOOL_FAMILIES")
    allowed = tool_families("FORKMESH_AGENT_ALLOWED_TOOL_FAMILIES")
    if "bash" in disabled:
        return False
    return not allowed or "bash" in allowed


def gate_exchange(path, request):
    family = getattr(socket, "AF_UNIX", None)
    if family is None:
        raise OSError("this system has no Unix sockets")
    with socket.socket(family, socket.SOCK_STREAM) as conn:
        conn.settimeout(GATE_TIMEOUT)
        conn.connect(path)
        conn.sendall(json.dumps(request).encode("utf-8") + b"\n")
        data = b""
        while not data.endswith(b"\n") and len(data) < GATE_MAX_REPLY:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
    return data


def gate_denial(command):
    path = os.environ.get("FORKMESH_AGENT_GATE", "").strip()
    if not path:
        return ""
    request = {
        "v": 1,
        "session": os.environ.get("FORKMESH_AGENT_GATE_SESSION", ""),
        "label": os.environ.get("FORKMESH_AGENT_GATE_LABEL", ""),
        "tool": "Bash",
        "input": {"command": command},
        "cwd": os.getcwd(),
    }
    try:
        data = gate_exchange(path, request)
    except OSError as error:
        return ("ForkMesh could not ask the user about this command (%s), so it "
                "did not run. Commands need the project's permission."
                % (str(error) or type(error).__name__))
    try:
        reply = json.loads(data.decode("utf-8"))
    except ValueError:
        reply = None
    if not isinstance(reply, dict) or reply.get("decision") not in ("allow", "deny"):
        return ("ForkMesh's permission check sent an unreadable answer, so this "
                "command did not run.")
    if reply["decision"] == "allow":
        return ""
    return (str(reply.get("reason") or "").strip()
            or "The user did not allow this command.")


def run_tool(command):
    denial = gate_denial(command)
    if denial:
        return "Not run: ForkMesh did not allow this command. %s" % denial
    return run_bash(command)


def run_bash(command):
    try:
        shell_args = (["cmd.exe", "/d", "/s", "/c", command]
                      if os.name == "nt" else ["/bin/sh", "-c", command])
        proc = subprocess.Popen(
            shell_args,
            env=COMMAND_ENV,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=os.name != "nt",
        )
        try:
            out, _ = proc.communicate(timeout=600)
        except subprocess.TimeoutExpired:
            if os.name != "nt":
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(proc.pid, signal.SIGKILL)
            else:
                proc.kill()
            proc.communicate()
            return "Command timed out after 600s."
    except OSError as error:
        return "Command failed to start: %s" % error
    if len(out) > TOOL_OUTPUT_LIMIT:
        out = out[:TOOL_OUTPUT_LIMIT] + "\n...[output truncated]..."
    return "(exit code %d)\n%s" % (proc.returncode, out)


TOOLS = [
    {
        "name": "bash",
        "description": (
            "Run a shell command in the current working directory (the checked-out "
            "worktree). Use it to read, search, and edit files needed for the issue. "
            "After completing and verifying the requested changes, commit them to the "
            "current agent branch. Do not push or use the network."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "command": {
                    "type": "string",
                    "description": "The shell command to execute.",
                }
            },
            "required": ["command"],
        },
    }
]


def call_api(api_key, model, max_tokens, messages):
    body = json.dumps(
        {
            "model": model,
            "max_tokens": max_tokens,
            "tools": TOOLS,
            "messages": messages,
        }
    ).encode("utf-8")
    request = urllib.request.Request(API_URL, data=body, method="POST")
    request.add_header("content-type", "application/json")
    request.add_header("anthropic-version", API_VERSION)
    request.add_header("x-api-key", api_key)
    with urllib.request.urlopen(request, timeout=600) as response:
        return json.loads(response.read().decode("utf-8"))


# Network-traffic markers. ForkMesh's agent detail page parses lines beginning
# with "==> [net]" to draw a live graphic of API calls and token flow, so keep
# the in=/out= fields machine-readable.
def net_request(turn, model, msg_count):
    log("==> [net] \U0001F310 request #%d → POST api.anthropic.com/v1/messages "
        "(model=%s, messages=%d)" % (turn, model, msg_count))


def net_response(turn, usage, stop_reason):
    inp = int(usage.get("input_tokens") or 0)
    out = int(usage.get("output_tokens") or 0)
    log("==> [net] \U0001F310 response #%d ← in=%d out=%d stop=%s"
        % (turn, inp, out, stop_reason or "end"))


def net_error(turn, code):
    log("==> [net] \U0001F310 error #%d ← HTTP %s" % (turn, code))


def main():
    if not bash_permitted():
        log("!! The Shell (bash) tool family is turned off for this agent, and bash "
            "is the only tool the Claude API agent has, so it cannot work. Turn "
            "Shell back on in the project's agent tool settings or pick another "
            "provider.")
        return 1
    api_key = os.environ.get("ANTHROPIC_API_KEY", "").strip()
    if not api_key:
        log("!! No ANTHROPIC_API_KEY is set. Add a Claude API key in Settings.")
        return 1
    protect_process_credentials()

    model = os.environ.get("FORKMESH_AGENT_MODEL", "").strip() or "claude-sonnet-4-6"
    try:
        max_tokens = int(os.environ.get("FORKMESH_AGENT_MAX_OUTPUT_TOKENS", "2000"))
    except ValueError:
        max_tokens = 2000
    max_tokens = max(256, max_tokens)

    prompt = read_prompt().strip()
    if not prompt:
        log("!! Empty prompt; nothing to do.")
        return 1

    log("==> Claude agent (model %s) talking to the Anthropic API directly." % model)
    messages = [{"role": "user", "content": prompt}]

    for turn in range(1, MAX_TURNS + 1):
        net_request(turn, model, len(messages))
        try:
            data = call_api(api_key, model, max_tokens, messages)
        except urllib.error.HTTPError as err:
            net_error(turn, err.code)
            detail = err.read().decode("utf-8", "replace")
            log("!! Anthropic API error %d: %s" % (err.code, detail))
            if err.code in (401, 403):
                log("!! The API key was rejected; rotate it and re-enter it in Settings.")
            return 1
        except urllib.error.URLError as err:
            net_error(turn, "unreachable")
            log("!! Could not reach the Anthropic API: %s" % err.reason)
            return 1

        net_response(turn, data.get("usage", {}), data.get("stop_reason"))
        content = data.get("content", [])
        tool_results = []
        for block in content:
            if block.get("type") == "text":
                text = block.get("text", "").strip()
                if text:
                    log("\n● %s" % text)  # ● assistant message
            elif block.get("type") == "tool_use":
                command = block.get("input", {}).get("command", "")
                log("\n$ %s" % command)
                result = run_tool(command)
                log(result)
                tool_results.append(
                    {
                        "type": "tool_result",
                        "tool_use_id": block.get("id"),
                        "content": result,
                    }
                )

        messages.append({"role": "assistant", "content": content})
        if data.get("stop_reason") == "tool_use" and tool_results:
            messages.append({"role": "user", "content": tool_results})
            continue
        log("\n==> Agent finished.")
        return 0

    log("\n!! Reached the maximum number of turns without finishing.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
)PYAGENT");
}
