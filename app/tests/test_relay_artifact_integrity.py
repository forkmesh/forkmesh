"""Tamper-proofing served bytes (adhoc #86).

Two halves of the same guarantee:

1. Gateway-side: a content-addressed release artifact is hashed before the
   direct-HTTPS gateway opens its bounded file stream. A mismatch is rejected
   before response construction, so the retired repository socket is not part
   of the integrity boundary.

2. Client-side: the desktop Mirror-nodes table underlines any content column
   whose value doesn't match the source of truth, so a node quietly serving
   divergent data is visible at a glance.

3. Reachability: a node that advertises the release-blob operation must also
   name the store those bytes live in, or it hashes nothing and 404s every
   download it is the only holder of.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
APP_RUNTIME = ROOT / "app" / "src" / "app_runtime.py"
GATEWAY = ROOT / "tools" / "mirror_gateway.py"
RELEASES = ROOT / "desktop" / "src" / "MainWindowReleases.cpp"
CONTROL_NODE = ROOT / "desktop" / "src" / "MainWindowControlNode.cpp"
INTERNAL = ROOT / "desktop" / "src" / "MainWindowInternal.h"


def test_release_blob_gateway_verifies_content_hash_before_streaming():
    worker = APP_RUNTIME.read_text(encoding="utf-8")
    gateway = GATEWAY.read_text(encoding="utf-8")

    route = worker[
        worker.index("release_blob_match = RELEASE_BLOB_RE.match"):
        worker.index("host_match = REPO_HOST_RE.match")
    ]
    assert 'operation="release-blob"' not in route
    assert '"release-blob"' in route
    assert "release_sha=sha256" in route
    release_spec = gateway[
        gateway.index("def release_spec("):
        gateway.index("\n    def ", gateway.index("def release_spec(") + 1)
    ]
    assert "SHA256_RE.fullmatch(digest)" in release_spec
    assert "(root / \"sha256\" / digest[:2] / digest / \"data\").resolve()" in release_spec
    assert "hmac.compare_digest(_sha256_file(path), digest)" in release_spec
    assert 'raise GatewayError("release blob integrity check failed")' in release_spec
    assert "return StreamSpec(" in release_spec


def test_desktop_gateway_config_names_the_release_store_it_serves():
    source = CONTROL_NODE.read_text(encoding="utf-8")
    internal = INTERNAL.read_text(encoding="utf-8")
    body = source[
        source.index("bool MainWindow::rebuildDirectMirrorGatewayConfiguration"):
        source.index("void MainWindow::provisionDirectMirrorEndpoint")
    ]

    # The repository entry advertises release-blob, so it must also say where
    # those bytes are: mirror_gateway.py serves no artifact without a store.
    assert 'QStringLiteral("release-blob")' in body
    assert 'QStringLiteral("releaseStore")' in body
    assert "mirrorReleaseStoreRoot(repo.mirrorPath)" in body

    # One definition of the layout the publisher and the gateway both address:
    # <mirror>/forkmesh-releases, sharded as sha256/<aa>/<hash>/data below it.
    assert (
        'inline QString mirrorReleaseStoreRoot(const QString &mirrorPath)'
        in internal
    )
    assert 'filePath(QStringLiteral("forkmesh-releases"))' in internal
    assert "QDir(mirrorReleaseStoreRoot(mirrorPath))" in internal


def test_gateway_defaults_the_release_store_beside_the_mirror():
    gateway = GATEWAY.read_text(encoding="utf-8")
    start = gateway.index("\n        release_store = None")
    loader = gateway[start:gateway.index("\n        repositories.append(", start)]

    # Configs that never name a store (the desktop host's, and a headless
    # refresh with releaseStore unset) still serve the published CAS, exactly
    # as server/gateway.go's loader defaults it.
    assert 'default_store = git_dir / "forkmesh-releases"' in loader
    assert "if default_store.is_dir() and not default_store.is_symlink():" in loader
    assert "release_store = default_store.resolve()" in loader


def test_mirror_nodes_table_underlines_mismatched_columns():
    source = RELEASES.read_text(encoding="utf-8")
    body = source[source.index("void MainWindow::loadMirrorNodesPanel()"):]

    # A reference row (source of truth, else freshest) supplies the canonical values.
    assert "const MirrorAdvert *referenceAdvert =" in body
    assert "const int refIssues = referenceAdvert ? referenceAdvert->issueCount : -1;" in body

    # The underline helper flips the cell font and notes the mismatch in the tooltip.
    assert "auto markMismatch = [](QTableWidgetItem *item, bool mismatch," in body
    assert "f.setUnderline(true);" in body
    assert "Doesn't match the source of truth" in body

    # Applied to the content columns in BOTH the live-roster and catalog-backed rows.
    assert "markMismatch(commitItem," in body
    assert "markMismatch(issuesItem, countMismatch(nodeIssues, refIssues)," in body
    assert "markMismatch(artifactsItem, countMismatch(nodeArtifacts, refArtifacts)," in body
    assert "markMismatch(catCommitItem," in body
    assert "markMismatch(catIssuesItem, countMismatch(catIssues, refIssues)," in body
