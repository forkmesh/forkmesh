"""Web issue details offer a copyable MCP prompt that works the issue end to end."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DASHBOARD = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")


def test_issue_detail_renders_copy_mcp_prompt_action():
    for contract in (
        "data-repo-issue-mcp-prompt",
        "Copy MCP prompt",
        # It sits in the issue page's Agent section, beside the agent it drives.
        '${sidebarSection("Agent", agentValue)}',
    ):
        assert contract in DASHBOARD


def test_prompt_carries_the_full_mcp_server_configuration_and_token():
    # A paste has to be enough: the block names the stdio server, the checkouts
    # it needs, and the connector token the signed write tools require.
    for contract in (
        "mcpServers: {",
        '"<FORKMESH_CHECKOUT>/tools/forkmesh_mcp_server.py"',
        "FORKMESH_REPO: repoCheckout",
        "FORKMESH_MCP_TOKEN: connectorToken",
        "JSON.stringify(configuration, null, 2)",
        "claude mcp add forkmesh --scope user",
        "git clone ${forkmeshCloneUrl}",
        "never print it in logs, commits, pull requests, or chat",
    ):
        assert contract in DASHBOARD


def test_connector_token_is_generated_on_the_spot_and_remembered():
    # One click has to be enough, so the token is minted in the browser in the
    # desktop's own fmcp_ format, remembered, and reused on every later copy.
    for contract in (
        '"forkmesh.mcpConnectorToken"',
        "function loadMcpConnectorToken()",
        "function saveMcpConnectorToken(token)",
        "function generateMcpConnectorToken()",
        "window.crypto.getRandomValues(bytes)",
        '"fmcp_" +',
        "function ensureMcpConnectorToken(options)",
        "const token = ensureMcpConnectorToken(options);",
        "MCP_CONNECTOR_TOKEN_PLACEHOLDER",
    ):
        assert contract in DASHBOARD


def test_plain_copy_never_opens_a_dialog():
    # The paste box hangs off the shift-click branch only, so the ordinary
    # path from click to clipboard asks the user nothing.
    start = DASHBOARD.index("function ensureMcpConnectorToken(options)")
    body = DASHBOARD[start : DASHBOARD.index("function issueMcpPrompt", start)]
    replace_branch = body.index("if (options?.replaceToken) {")
    prompt_call = body.index("window.prompt(")
    assert replace_branch < prompt_call
    # ...and that branch has returned before the un-shifted path generates.
    assert body.index("if (!token) {") > prompt_call
    assert "replaceToken: event.shiftKey === true," in DASHBOARD


def test_prompt_installs_the_generated_token_without_clobbering_a_connector():
    # A browser-minted token only unlocks the write tools once this machine's
    # connector file holds it, and an existing connector has to win: replacing
    # it would revoke every other agent config still holding the old string.
    for contract in (
        "mcp/connector.json",
        "Activate it before step 1",
        # The record McpConnector::saveConnector writes, field for field, so
        # the desktop's Settings -> MCP tab renders what the agent installed.
        'create it (mode 0600) containing {"version": 1, "token"',
        '"created_ms": <epoch milliseconds>',
        "leave it exactly as it is and use its own",
        "overwriting it would revoke every other agent",
    ):
        assert contract in DASHBOARD


def test_prompt_covers_pull_work_submit_and_model_report():
    for contract in (
        "function issueMcpPrompt(repo, number, values, token)",
        # The agent pulls the issue, works a branch, and submits the PR
        # through the forkmesh MCP server's own tools.
        'the "forkmesh" MCP server',
        "search_issues",
        "read_file",
        "open_pr_from_branch",
        # It must report back the model and thinking setting on the issue.
        "comment_on_issue",
        "which model you ran as (model name/id) and your thinking setting",
        "Do not skip this report.",
    ):
        assert contract in DASHBOARD


def test_signed_out_click_routes_through_login_and_back():
    # The button renders regardless of session, but copying requires an
    # authenticated account: the handler bounces to /login with a next hop
    # back to the issue instead of handing out the prompt.
    assert "async function copyIssueMcpPrompt(button, options)" in DASHBOARD
    assert (
        '"/login?next=" + encodeURIComponent(`${location.pathname}${location.search}`)'
        in DASHBOARD
    )
    assert "if (!state.session?.sessionToken) {" in DASHBOARD
