#include "SideRail.h"
#include "TerminalBridge.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QMimeData>

#include <iostream>

namespace sr = forkmesh::siderail;
namespace tb = forkmesh::terminalbridge;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QStringList keys(const QList<sr::Item> &items)
{
    QStringList out;
    for (const sr::Item &item : items)
        out << item.key();
    return out;
}

static void testItems()
{
    const sr::Item flow = sr::workflowItem(QStringLiteral("acme/site"), QStringLiteral("deploy"));
    const sr::Item page = sr::pageItem(QStringLiteral("agents"), QStringLiteral("Agents"));
    const sr::Item link = sr::linkItem(QUrl(QStringLiteral("https://example.com/docs/")));
    check(flow.valid() && page.valid() && link.valid(), "workflow, page and link items are valid");
    check(link.url == QLatin1String("https://example.com/docs"), "link drops its trailing slash");
    check(link.title == QLatin1String("example.com"), "link title defaults to its host");
    check(!sr::linkItem(QUrl(QStringLiteral("file:///etc/passwd"))).valid(), "file links are refused");
    check(!sr::linkItem(QUrl(QStringLiteral("javascript:alert(1)"))).valid(), "script links are refused");
    check(!sr::linkItem(QUrl(QStringLiteral("https://user:pw@example.com"))).valid(),
          "links carrying credentials are refused");
    check(!sr::workflowItem(QStringLiteral("norepo"), QStringLiteral("x")).valid(),
          "workflow items need owner/name");

    const QString saved = sr::serializeItems({flow, page, link, page});
    const QList<sr::Item> parsed = sr::parseItems(saved);
    check(keys(parsed) == keys({flow, page, link}), "items round-trip without duplicates");
    check(sr::parseItems(QStringLiteral("not json")).isEmpty(), "bad settings parse to nothing");

    QList<sr::Item> moved = sr::insertItem({flow, page, link}, link, 0);
    check(keys(moved) == keys({link, flow, page}), "insert moves an existing item to the front");
    moved = sr::insertItem({flow, page, link}, flow, 3);
    check(keys(moved) == keys({page, link, flow}), "insert past the end moves to the end");
    moved = sr::insertItem({flow, page, link}, flow, 2);
    check(keys(moved) == keys({page, flow, link}), "insert before an item accounts for removal");
    check(keys(sr::removeItem({flow, page, link}, page.key())) == keys({flow, link}),
          "remove drops one item");
}

static void testWorkflowMerge()
{
    const sr::Item kept = sr::workflowItem(QStringLiteral("acme/site"), QStringLiteral("deploy"));
    const sr::Item gone = sr::workflowItem(QStringLiteral("acme/site"), QStringLiteral("old"));
    const sr::Item page = sr::pageItem(QStringLiteral("agents"));
    const QList<sr::Item> merged = sr::withWorkflows(
        {page, gone, kept},
        {{QStringLiteral("acme/site"), QStringLiteral("deploy")},
         {QStringLiteral("acme/site"), QStringLiteral("lint")}});
    check(keys(merged) == QStringList({page.key(), kept.key(),
                                       sr::workflowItem(QStringLiteral("acme/site"),
                                                        QStringLiteral("lint"))
                                           .key()}),
          "unpinned workflows drop out and new right-rail pins join at the end");
}

static void testMime()
{
    QMimeData flow;
    flow.setData(QLatin1String(sr::kWorkflowMime),
                 sr::encodeWorkflow(QStringLiteral("acme/site"), QStringLiteral("deploy")));
    const QList<sr::Item> flows = sr::itemsFromMime(&flow, QStringLiteral("x-rail"));
    check(flows.size() == 1 && flows.first().kind == sr::kWorkflowKind &&
              flows.first().id == QLatin1String("deploy"),
          "a dragged workflow becomes a workflow item");

    QMimeData pin;
    pin.setData(QLatin1String(sr::kWorkflowPinMime),
                sr::encodeWorkflowPin(QStringLiteral("acme/site"), QStringLiteral("lint")));
    const QList<sr::Item> pins = sr::itemsFromMime(&pin, QString());
    check(pins.size() == 1 && pins.first().key() ==
                                  sr::workflowItem(QStringLiteral("acme/site"), QStringLiteral("lint")).key(),
          "a pin dragged from another strip becomes a workflow item");
    QMimeData badPin;
    badPin.setData(QLatin1String(sr::kWorkflowPinMime), QByteArrayLiteral("acme/site"));
    check(sr::itemsFromMime(&badPin, QString()).isEmpty(), "a malformed pin drag is refused");

    QMimeData page;
    page.setData(QStringLiteral("x-rail"), QByteArrayLiteral("agents"));
    const QList<sr::Item> pages = sr::itemsFromMime(&page, QStringLiteral("x-rail"));
    check(pages.size() == 1 && pages.first().kind == sr::kPageKind, "a left-rail page becomes a page item");

    QMimeData links;
    links.setUrls({QUrl(QStringLiteral("https://a.example")), QUrl(QStringLiteral("file:///tmp"))});
    check(sr::itemsFromMime(&links, QString()).size() == 1, "only web links are taken from a url drop");

    QMimeData text;
    text.setText(QStringLiteral(" https://b.example/x "));
    check(sr::itemsFromMime(&text, QString()).size() == 1, "a dragged web address becomes a link");

    QMimeData junk;
    junk.setText(QStringLiteral("hello"));
    check(sr::itemsFromMime(&junk, QString()).isEmpty(), "plain text is not a rail item");
    check(sr::clampBrowserWidth(0) == sr::kDefaultBrowserWidth &&
              sr::clampBrowserWidth(10) == sr::kMinBrowserWidth &&
              sr::clampBrowserWidth(99999) == sr::kMaxBrowserWidth,
          "browser width is clamped");
}

static void testTerminalRequests()
{
    const QString id = tb::newTerminalId();
    check(tb::validTerminalId(id) && !tb::validTerminalId(QStringLiteral("../x")),
          "terminal ids are 16 hex characters");
    check(tb::newToken().startsWith(QLatin1String("term.")) && tb::newToken() != tb::newToken(),
          "terminal tokens are random and marked");

    bool ok = false;
    check(tb::keyBytes(QStringLiteral("ctrl-c"), false, &ok) == QByteArray("\x03") && ok, "ctrl-c");
    check(tb::keyBytes(QStringLiteral("Up"), true, &ok) == QByteArray("\x1bOA") && ok,
          "arrows follow application cursor mode");
    check(tb::keyBytes(QStringLiteral("ctrl+z"), false, &ok) == QByteArray("\x1a") && ok, "ctrl+z");
    tb::keyBytes(QStringLiteral("hyper-q"), false, &ok);
    check(!ok, "unknown keys are refused");

    tb::InputRequest input;
    QString error;
    check(tb::parseInputRequest(QJsonObject{{QStringLiteral("terminal"), id},
                                            {QStringLiteral("text"), QStringLiteral("ls\nls")},
                                            {QStringLiteral("keys"), QJsonArray{QStringLiteral("tab")}},
                                            {QStringLiteral("enter"), true}},
                                false, &input, &error),
          "input parses");
    check(input.bytes == QByteArray("ls\rls\t\r"), "newlines become carriage returns, keys and enter follow");
    check(input.summary.contains(QStringLiteral("pressed enter")), "input summary names the keys");
    check(!tb::parseInputRequest(QJsonObject{{QStringLiteral("terminal"), id}}, false, nullptr, &error) &&
              error == QLatin1String("empty_input"),
          "empty input is refused");
    check(!tb::parseInputRequest(QJsonObject{{QStringLiteral("terminal"), QStringLiteral("nope")},
                                             {QStringLiteral("text"), QStringLiteral("x")}},
                                 false, nullptr, &error) &&
              error == QLatin1String("bad_terminal"),
          "a bad terminal id is refused");
    check(!tb::parseInputRequest(QJsonObject{{QStringLiteral("terminal"), id},
                                             {QStringLiteral("text"), QString(tb::kMaxInputChars + 1, QLatin1Char('a'))}},
                                 false, nullptr, &error),
          "oversized input is refused");

    tb::OpenRequest open;
    check(tb::parseOpenRequest(QJsonObject{{QStringLiteral("kind"), QStringLiteral("claude")},
                                           {QStringLiteral("repository"), QStringLiteral("acme/site")}},
                               &open, &error) &&
              open.kind == tb::ClaudeKind && open.owner == QLatin1String("acme"),
          "open parses kind and repository");
    check(!tb::parseOpenRequest(QJsonObject{{QStringLiteral("kind"), QStringLiteral("command")}},
                                nullptr, &error),
          "a command terminal needs a command");
    check(!tb::parseOpenRequest(QJsonObject{{QStringLiteral("kind"), QStringLiteral("shell")},
                                            {QStringLiteral("command"), QStringLiteral("rm -rf x")}},
                                nullptr, &error),
          "only command terminals take a command");
    check(!tb::parseOpenRequest(QJsonObject{{QStringLiteral("kind"), QStringLiteral("python")}},
                                nullptr, &error),
          "unknown kinds are refused");

    tb::ReadRequest read;
    check(tb::parseReadRequest(QJsonObject{{QStringLiteral("terminal"), id}}, &read, &error) &&
              read.lines == tb::kDefaultReadLines,
          "read defaults its line count");
    check(!tb::parseReadRequest(QJsonObject{{QStringLiteral("terminal"), id},
                                            {QStringLiteral("lines"), 1.5}},
                                nullptr, &error),
          "fractional line counts are refused");

    check(tb::launchCommand(tb::ClaudeKind, QString(), QStringLiteral("/tmp/it's.json"), false) ==
              QLatin1String("claude --mcp-config '/tmp/it'\\''s.json'"),
          "the Claude Code launch quotes its MCP config path");
    check(tb::launchCommand(tb::CodexKind, QString(), QStringLiteral("/d/mcp-1.json"), false,
                            QStringLiteral("/usr/bin/python3"), QStringLiteral("/s/my \"mcp\".py")) ==
              QLatin1String("codex -c 'mcp_servers.forkmesh.command=\"/usr/bin/python3\"' "
                            "-c 'mcp_servers.forkmesh.args=[\"/s/my \\\"mcp\\\".py\", "
                            "\"--env-file\", \"/d/mcp-1.json\"]'"),
          "a Codex terminal gets the ForkMesh MCP server through -c overrides and an env file");
    check(tb::launchCommand(tb::CodexKind, QString(), QString(), false) == QLatin1String("codex"),
          "Codex without a config file starts plainly");
    check(tb::tomlString(QStringLiteral("a\\b\n")) == QLatin1String("\"a\\\\b\\u000a\""),
          "TOML strings escape backslashes and control characters");
    check(tb::launchCommand(tb::ShellKind, QString(), QString(), false).contains(QStringLiteral("SHELL")),
          "a shell terminal runs the user's shell");
    check(tb::tailLines(QStringLiteral("a\nb\nc\n\n"), 2) == QLatin1String("b\nc"),
          "tail keeps the last lines and drops trailing blanks");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testItems();
    testWorkflowMerge();
    testMime();
    testTerminalRequests();
    if (failures == 0)
        std::cout << "side rail tests passed\n";
    return failures == 0 ? 0 : 1;
}
