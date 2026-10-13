from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "desktop" / "src" / "main.cpp"


def test_display_endpoint_is_ready_before_qapplication_is_constructed():
    source = MAIN.read_text(encoding="utf-8")

    crash_handler = source.index("forkmesh::installCrashHandler(")
    preflight = source.index("forkmesh::waitForDesktopDisplay(")
    application = source.index("ForkMeshApplication app(argc, argv);")

    assert crash_handler < preflight < application
    assert (
        "DesktopDisplayState::WaitingForEndpoint" in source[crash_handler:application]
    )
    assert "DesktopDisplayState::Ready" in source[crash_handler:application]
    assert "return 3;" in source[preflight:application]
