import next_ids


def test_files_expectation_lists_unit_dropin_directories(monkeypatch):
    import next_fleet
    files = {
        "/etc/systemd/system/fmnext-turn.service": ("[Unit]\n", 0o644),
        "/etc/systemd/system/fmnext-turn.service.d/50-fmnext-ids.conf": ("[Unit]\n", 0o644),
        "/etc/forkmesh-next/app.env": ("A=1\n", 0o600),
    }
    monkeypatch.setattr(next_fleet, "render_node", lambda fleet, name, replacement=False: (files, {}))
    expected, _ = next_ids.files_expectation({}, "n1")
    assert expected["/etc/systemd/system/fmnext-turn.service.d"] == "dir"
    assert "/etc/forkmesh-next" not in expected
    assert "/etc/systemd/system" not in expected
