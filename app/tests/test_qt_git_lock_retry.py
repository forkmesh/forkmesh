from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "desktop" / "src" / "MainWindowInternal.h").read_text(
    encoding="utf-8")


def test_git_capture_retries_only_transient_lock_contention():
    start = SOURCE.index("inline GitCaptureResult runGitCaptureDirect(")
    end = SOURCE.index("inline GitCaptureResult runGitCaptureBackgrounded(", start)
    body = SOURCE[start:end]

    assert "constexpr int lockAttempts = 6" in body
    assert 'QLatin1String("File exists")' in body
    assert 'QLatin1String("lock")' in body
    assert "QThread::msleep" in body
    assert "if (!lockBusy" in body
