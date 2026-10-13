from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CONTROL_NODE = ROOT / "desktop" / "src" / "MainWindowControlNode.cpp"
SERVER_NODE = ROOT / "desktop" / "src" / "ServerNode.cpp"
SYSTEMD = ROOT / "server" / "packaging" / "systemd" / "forkmesh-mirror-node.service"
DAEMON = ROOT / "server" / "daemon.go"


def test_desktop_no_longer_supervises_mirror_server_processes():
    source = CONTROL_NODE.read_text(encoding="utf-8")
    relay = SERVER_NODE.read_text(encoding="utf-8")
    assert "startDirectMirrorServices" not in source
    assert "stopDirectMirrorServices" not in source
    assert "ensureDirectMirrorRegistrationTimer" not in source
    assert "checkDirectMirrorGatewayHealth" not in source
    assert "writeMirrors(hello" not in relay
    assert 'makeMessage("mirror-refresh")' not in relay


def test_native_node_owns_restart_and_endpoint_renewal():
    service = SYSTEMD.read_text(encoding="utf-8")
    daemon = DAEMON.read_text(encoding="utf-8")
    assert "ExecStart=/usr/local/bin/forkmesh-mirror-node" in service
    assert "Restart=on-failure" in service
    assert "RestartSec=5s" in service
    assert "EndpointPublisher" in daemon
    assert "endpoint.Publish(" in daemon
    assert "lastEndpointAt" in daemon
