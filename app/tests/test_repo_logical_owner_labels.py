#!/usr/bin/env python3
"""Repository lists are labelled with the logical owner (adhoc #94).

Bytes are published by nodes, so the catalog route for a mirrored repo reads
``mirror8/forkmesh``. The catalog already resolves each route to the user or
organization that owns the work (``ownerKind`` / ``logicalOwner`` /
``logicalOwners``); the dashboard must show that identity — ``forkmesh/forkmesh``
— while every routing key stays physical so lookups keep matching catalog rows.

The same cards show the repository's own root logo, which is served by a mirror
and can therefore fail after the card rendered: the image must fall back to the
generated artwork and then to the icon, never to a broken image.
"""

import json
import subprocess

from _dashboard_bundle import assembled_dashboard_js


DASHBOARD_JS = assembled_dashboard_js()
BOOT = "  void bootDashboard();"
BROWSER_STUBS = """
const assert = require("assert");
const vm = require("vm");
global.location = {
  origin: "https://forkmesh.test",
  hostname: "forkmesh.test",
  pathname: "/dashboard",
  search: "",
};
global.localStorage = { getItem() { return null; }, setItem() {}, removeItem() {} };
const emptyClassList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
global.document = {
  body: { classList: emptyClassList },
  documentElement: { dataset: {}, style: {} },
  cookie: "",
  querySelector() { return null; },
  querySelectorAll() { return []; },
  addEventListener() {},
};
global.window = {
  matchMedia() { return { addEventListener() {}, addListener() {} }; },
  addEventListener() {},
  history: { pushState() {} },
  lucide: { createIcons() {} },
  setTimeout() {},
  setInterval() {},
  clearInterval() {},
};
global.navigator = {};
vm.runInThisContext(SOURCE);
"""


def _run(exports, body):
    transformed = DASHBOARD_JS.replace(
        BOOT, "  globalThis.__dashboardExports = { " + exports + " };")
    assert transformed != DASHBOARD_JS, "boot anchor drifted"
    script = (
        "const SOURCE = " + json.dumps(transformed) + ";\n"
        + BROWSER_STUBS + body
    )
    result = subprocess.run(
        ["node"], input=script, check=True, capture_output=True, text=True)
    assert result.stderr == ""
    return result.stdout


def test_card_titles_prefer_the_organization_over_the_serving_node():
    output = _run(
        "state, groupRepositories, repositoryCard, groupDisplayOwner",
        """
const { state, groupRepositories, repositoryCard, groupDisplayOwner } =
  global.__dashboardExports;
const repos = [
  {
    owner: "mirror8",
    name: "forkmesh",
    source: "local-node",
    rootCommit: "abc",
    liveHost: true,
    lastSync: 3000,
    ownerKind: "user",
    logicalOwner: "lucianoms",
    logicalOwners: [
      { kind: "user", owner: "lucianoms" },
      { kind: "organization", owner: "forkmesh" },
    ],
  },
  {
    owner: "mirror9",
    name: "forkmesh",
    source: "remote-clone",
    cloneUrl: "https://forkmesh.com/mirror8/forkmesh",
    rootCommit: "abc",
    lastSync: 1000,
  },
];
state.repositories = repos;
const group = groupRepositories(repos)[0];
assert.equal(groupDisplayOwner(group), "forkmesh");
const card = repositoryCard(group);
// The visible title is the organization; the node name never appears as the
// owner segment.
assert(card.includes(">forkmesh/</span>"));
assert(!card.includes(">mirror8/</span>"));
// Routing keys stay physical so findRepository/catalog lookups keep working.
assert(card.includes('data-dashboard-open-repo="mirror8/forkmesh"'));
console.log("ok");
""",
    )
    assert output.strip() == "ok"


def test_display_owner_falls_back_to_the_account_then_the_node():
    output = _run(
        "repoDisplayOwner, repoDisplayKey",
        """
const { repoDisplayOwner, repoDisplayKey } = global.__dashboardExports;
// No organization alias: the owning account still beats the machine name.
assert.equal(
  repoDisplayOwner({
    owner: "mirror8",
    name: "forkmesh",
    ownerKind: "user",
    logicalOwner: "lucianoms",
  }),
  "lucianoms",
);
// An unresolved route (old payload, or a node with no account link) keeps the
// physical owner rather than rendering an empty segment.
assert.equal(repoDisplayOwner({ owner: "mirror8", name: "forkmesh" }), "mirror8");
assert.equal(repoDisplayKey({ owner: "mirror8", name: "forkmesh" }), "mirror8/forkmesh");
// Unknown owner kinds are ignored: only user/organization identities are public.
assert.equal(
  repoDisplayOwner({
    owner: "mirror8",
    name: "forkmesh",
    logicalOwners: [{ kind: "node", owner: "mirror9" }],
  }),
  "mirror8",
);
console.log("ok");
""",
    )
    assert output.strip() == "ok"


def test_locally_served_forkmesh_clone_keeps_the_upstream_identity():
    output = _run(
        "repoCanonicalIdentity",
        """
const { repoCanonicalIdentity } = global.__dashboardExports;
assert.deepEqual(
  repoCanonicalIdentity({
    owner: "jett",
    name: "forkmesh",
    source: "local-node",
    cloneUrl: "https://forkmesh.test/forkmesh/forkmesh",
  }),
  { owner: "forkmesh", name: "forkmesh" },
);
// An unrelated local checkout must retain its local repository identity.
assert.deepEqual(
  repoCanonicalIdentity({
    owner: "jett",
    name: "forkmesh",
    source: "local-node",
    cloneUrl: "https://github.com/forkmesh/forkmesh.git",
  }),
  { owner: "jett", name: "forkmesh" },
);
console.log("ok");
""",
    )
    assert output.strip() == "ok"


def test_repository_links_follow_the_organization_label():
    # adhoc #132: a card that reads "forkmesh/forkmesh" must also navigate
    # there. Organization aliases are routable (org_alias_rewrite maps
    # /<org>/<repo> and /api/repo/<org>/<repo>/... onto the serving node), so
    # the link can carry the label. A user identity has no such rewrite, so
    # those links stay on the physical /<node>/<repo> route.
    output = _run(
        "state, groupRepositories, groupLinkUrl, repoLinkUrl,"
        " homeFeedRepositoryCard",
        """
const {
  state,
  groupRepositories,
  groupLinkUrl,
  repoLinkUrl,
  homeFeedRepositoryCard,
} = global.__dashboardExports;
const aliased = [
  {
    owner: "mirror6",
    name: "forkmesh",
    source: "local-node",
    rootCommit: "abc",
    liveHost: true,
    lastSync: 3000,
    ownerKind: "user",
    logicalOwner: "lucianoms",
    logicalOwners: [
      { kind: "user", owner: "lucianoms" },
      { kind: "organization", owner: "forkmesh" },
    ],
  },
];
state.repositories = aliased;
const group = groupRepositories(aliased)[0];
assert.equal(groupLinkUrl(group), "/forkmesh/forkmesh");
assert.equal(groupLinkUrl(group, "blob", "README.md"), "/forkmesh/forkmesh/blob/README.md");
assert.equal(repoLinkUrl(aliased[0]), "/forkmesh/forkmesh");
const card = homeFeedRepositoryCard(group);
assert(card.includes('href="/forkmesh/forkmesh"'));
assert(!card.includes('href="/mirror6/forkmesh"'));
// User identities are labels only — no worker rewrite backs /<user>/<repo>.
const userOwned = [
  {
    owner: "mirror6",
    name: "sidecar",
    source: "local-node",
    rootCommit: "def",
    lastSync: 3000,
    ownerKind: "user",
    logicalOwner: "lucianoms",
    logicalOwners: [{ kind: "user", owner: "lucianoms" }],
  },
];
state.repositories = userOwned;
assert.equal(groupLinkUrl(groupRepositories(userOwned)[0]), "/mirror6/sidecar");
assert.equal(repoLinkUrl(userOwned[0]), "/mirror6/sidecar");
console.log("ok");
""",
    )
    assert output.strip() == "ok"


def test_repository_filter_matches_the_logical_owner():
    matcher = DASHBOARD_JS[
        DASHBOARD_JS.index("function repositoryMatchesQuery(repo, query)")
        : DASHBOARD_JS.index("function repositoryTermsBadge(")
    ]
    assert "repoLogicalOwners(repo).map((value) => value.owner)" in matcher


def test_root_logo_failure_falls_back_instead_of_breaking_the_image():
    hydrate = DASHBOARD_JS[
        DASHBOARD_JS.index("function hydrateNativeRepositoryLogos(root)")
        : DASHBOARD_JS.index("function repoActivitySparkline(")
    ]
    # The committed root logo is a mirror-served URL, so a load failure must
    # degrade to the owner-approved artwork and then to the repository icon.
    assert "image.onerror" in hydrate
    assert "image.dataset.logoFallbackUsed" in hydrate
    assert "hideNativeRepositoryLogo(image)" in hydrate

    loader = DASHBOARD_JS[
        DASHBOARD_JS.index("function loadNativeRepositoryLogo(endpoint)")
        : DASHBOARD_JS.index("function showNativeRepositoryLogo(")
    ]
    assert "body?.logo?.dataUrl" in loader
    assert "body?.logo?.fallbackDataUrl" in loader
    # Both candidates go through the same data-URL/raw-path allowlist.
    assert loader.count("nativeRepositoryLogoDataUrl(") == 2


def test_repo_page_addressed_by_organization_is_titled_with_it():
    # adhoc #1658: /forkmesh/forkmesh resolved the org alias onto the backing
    # catalog row and then read that row's logicalOwner, so the page header
    # read "jett/forkmesh" — the account behind the publishing node, an
    # identity that address never named.
    output = _run(
        "repoDisplayKey, repoLinkUrl",
        """
const { repoDisplayKey, repoLinkUrl } = global.__dashboardExports;
const resolved = {
  owner: "forkmesh",
  name: "forkmesh",
  source: "local-node",
  ownerKind: "organization",
  logicalOwner: "forkmesh",
  logicalOwners: [
    { kind: "organization", owner: "forkmesh" },
    { kind: "user", owner: "jett" },
  ],
  canonicalOwner: "forkmesh",
  canonicalName: "forkmesh",
  servingOwner: "mirror2",
  servingName: "forkmesh",
};
assert.equal(repoDisplayKey(resolved), "forkmesh/forkmesh");
assert.equal(repoLinkUrl(resolved), "/forkmesh/forkmesh");
assert.equal(
  repoLinkUrl(resolved, "blob", "README.md"),
  "/forkmesh/forkmesh/blob/README.md",
);
console.log("ok");
""",
    )
    assert output.strip() == "ok"


def test_an_alias_on_one_mirror_names_every_row_of_the_group():
    # The organization alias is registered on a single catalog row. Every other
    # row of the same mirror group is the same repository, so opening one from
    # a list, a notification, or a stale link must still read and address
    # forkmesh/forkmesh rather than the machine that answered.
    output = _run(
        "state, findRepository, repoDisplayKey, repoLinkUrl, cloneUrl",
        """
const { state, findRepository, repoDisplayKey, repoLinkUrl, cloneUrl } =
  global.__dashboardExports;
state.repositories = [
  {
    owner: "mirror8",
    name: "forkmesh",
    source: "local-node",
    rootCommit: "abc",
    liveHost: true,
    lastSync: 3000,
    ownerKind: "user",
    logicalOwner: "jett",
    logicalOwners: [{ kind: "user", owner: "jett" }],
  },
  {
    owner: "mirror2",
    name: "forkmesh",
    source: "remote-clone",
    lastSync: 2000,
    logicalOwners: [
      { kind: "user", owner: "jett" },
      { kind: "organization", owner: "forkmesh" },
    ],
  },
];
for (const key of ["mirror8/forkmesh", "mirror2/forkmesh"]) {
  const repo = findRepository(key);
  assert.equal(repoDisplayKey(repo), "forkmesh/forkmesh", key);
  assert.equal(repoLinkUrl(repo), "/forkmesh/forkmesh", key);
}
// The advertised clone address is the public one too: the mirror that answers
// today is chosen behind it.
assert.equal(
  cloneUrl(findRepository("mirror8/forkmesh")),
  "https://forkmesh.test/forkmesh/forkmesh",
);
console.log("ok");
""",
    )
    assert output.strip() == "ok"


def test_a_copy_served_from_a_sibling_forkmesh_host_keeps_the_upstream():
    # forkmesh.com and app.forkmesh.com are one deployment. Comparing bare
    # origins made a node advertising https://forkmesh.com/forkmesh/forkmesh
    # read as its own repository while the app was served from app.*.
    output = _run(
        "repoCanonicalIdentity",
        """
const { repoCanonicalIdentity } = global.__dashboardExports;
global.location.hostname = "app.forkmesh.test";
global.location.origin = "https://app.forkmesh.test";
assert.deepEqual(
  repoCanonicalIdentity({
    owner: "mirror8",
    name: "forkmesh",
    source: "local-node",
    cloneUrl: "https://forkmesh.test/forkmesh/forkmesh",
  }),
  { owner: "forkmesh", name: "forkmesh" },
);
// Still not a licence to relabel an unrelated upstream.
assert.deepEqual(
  repoCanonicalIdentity({
    owner: "mirror8",
    name: "forkmesh",
    source: "local-node",
    cloneUrl: "https://gitlab.example/forkmesh/forkmesh.git",
  }),
  { owner: "mirror8", name: "forkmesh" },
);
console.log("ok");
""",
    )
    assert output.strip() == "ok"


def test_groups_split_by_a_rewritten_root_commit_are_listed_once():
    # Grouping is keyed by root commit, so rewriting history splits one
    # repository's mirror fleet into several groups. They resolve to one public
    # address, so the lists show one card carrying every mirror.
    output = _run(
        "state, groupRepositories, mergeGroupsByPublicAddress, groupDisplayOwner,"
        " groupLinkUrl, sourceOfTruth",
        """
const {
  state,
  groupRepositories,
  mergeGroupsByPublicAddress,
  groupDisplayOwner,
  groupLinkUrl,
  sourceOfTruth,
} = global.__dashboardExports;
state.repositories = [
  {
    owner: "mirror8",
    name: "forkmesh",
    source: "remote-clone",
    rootCommit: "old",
    liveHost: true,
    lastSync: 3000,
    cloneUrl: "https://forkmesh.test/forkmesh/forkmesh",
  },
  {
    owner: "mirror9",
    name: "forkmesh",
    source: "remote-clone",
    rootCommit: "old",
    lastSync: 2000,
    cloneUrl: "https://forkmesh.test/forkmesh/forkmesh",
  },
  {
    owner: "jett",
    name: "forkmesh",
    source: "local-node",
    rootCommit: "rewritten",
    lastSync: 1000,
    ownerKind: "user",
    logicalOwner: "jett",
    logicalOwners: [{ kind: "user", owner: "jett" }],
    cloneUrl: "https://forkmesh.test/forkmesh/forkmesh",
  },
];
const groups = groupRepositories(state.repositories);
assert.equal(groups.length, 2);
const merged = mergeGroupsByPublicAddress(groups);
assert.equal(merged.length, 1);
assert.equal(groupDisplayOwner(merged[0]), "forkmesh");
assert.equal(groupLinkUrl(merged[0]), "/forkmesh/forkmesh");
// Every mirror of both histories is counted on the surviving card.
assert.equal(merged[0].members.length, 3);
// A live host keeps the source of truth.
assert.equal(sourceOfTruth(merged[0]).name, "forkmesh");
console.log("ok");
""",
    )
    assert output.strip() == "ok"
