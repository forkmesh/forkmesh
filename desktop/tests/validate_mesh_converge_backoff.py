#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SYNC = (ROOT / "src" / "MainWindowRepoSync.cpp").read_text(encoding="utf-8")

start = SYNC.find("void MainWindow::convergeSourceRepoFromMesh(int index, bool force)")
assert start >= 0, "missing convergeSourceRepoFromMesh"
end = SYNC.find("\nvoid MainWindow::", start + 1)
converge = SYNC[start:end]

gate = 'if (!force && !m_pollBackoff.ready(backoffChannel, nowMs))'
assert gate in converge, "background converges must honour the failure backoff"
assert converge.index(gate) < converge.index("m_sourceConvergeAttemptMs.insert(key, nowMs)"), (
    "a skipped pass must not count as an attempt"
)

done = converge[converge.index("connect(worker, &QThread::finished"):]
assert "if (*reconciliationFetched)" in done
assert "m_pollBackoff.noteSuccess(backoffChannel);" in done
assert "m_pollBackoff.noteFailure(" in done and "kConvergeBackoffCapMs" in done

print("mesh converge backoff contract ok")
