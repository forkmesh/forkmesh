#include "AgentWindows.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#ifdef FORKMESH_HAVE_WEBENGINE
#include <QTcpServer>
#include <QTcpSocket>
#endif
#include <QJsonArray>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QWidget>

#include <iostream>

using namespace forkmesh::agentwindows;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QString parseError(const QJsonObject &params)
{
    ShowRequest request;
    QString error;
    return parseShowRequest(params, &request, &error) ? QString() : error;
}

static QJsonObject basic()
{
    return QJsonObject{{QStringLiteral("title"), QStringLiteral("  Release   plan ")},
                       {QStringLiteral("body"), QStringLiteral("# Steps\n- one")}};
}

static void testParse()
{
    ShowRequest request;
    QString error;
    check(parseShowRequest(basic(), &request, &error), "a title and body parse");
    check(request.title == QLatin1String("Release plan"), "the title is collapsed");
    check(request.format == QLatin1String("markdown"), "markdown is the default format");
    check(request.buttons.isEmpty() && request.inputLabel.isEmpty(), "no controls by default");

    QJsonObject full = basic();
    full.insert(QStringLiteral("format"), QStringLiteral("HTML"));
    full.insert(QStringLiteral("buttons"),
                QJsonArray{QStringLiteral("Approve"), QStringLiteral(" Reject ")});
    full.insert(QStringLiteral("input_label"), QStringLiteral("Why?"));
    full.insert(QStringLiteral("input_multiline"), true);
    full.insert(QStringLiteral("width"), 600);
    full.insert(QStringLiteral("height"), 400);
    check(parseShowRequest(full, &request, &error), "every optional field parses");
    check(request.format == QLatin1String("html"), "format is case-insensitive");
    check(request.buttons == QStringList({QStringLiteral("Approve"), QStringLiteral("Reject")}),
          "button labels are trimmed");
    check(request.inputMultiline && request.width == 600 && request.height == 400,
          "input and size are kept");

    QJsonObject missing = basic();
    missing.remove(QStringLiteral("title"));
    check(parseError(missing) == QLatin1String("bad_title"), "a title is required");
    QJsonObject blank = basic();
    blank.insert(QStringLiteral("body"), QStringLiteral("   "));
    check(parseError(blank) == QLatin1String("bad_body"), "a blank body is refused");
    QJsonObject huge = basic();
    huge.insert(QStringLiteral("body"), QString(kMaxBodyChars + 1, QLatin1Char('x')));
    check(parseError(huge) == QLatin1String("bad_body"), "an oversized body is refused");
    QJsonObject format = basic();
    format.insert(QStringLiteral("format"), QStringLiteral("javascript"));
    check(parseError(format) == QLatin1String("bad_format"), "an unknown format is refused");
    QJsonObject dup = basic();
    dup.insert(QStringLiteral("buttons"), QJsonArray{QStringLiteral("Yes"), QStringLiteral("Yes")});
    check(parseError(dup) == QLatin1String("bad_buttons"), "duplicate buttons are refused");
    QJsonObject many = basic();
    QJsonArray seven;
    for (int i = 0; i < kMaxButtons + 1; ++i)
        seven.append(QStringLiteral("b%1").arg(i));
    many.insert(QStringLiteral("buttons"), seven);
    check(parseError(many) == QLatin1String("bad_buttons"), "too many buttons are refused");
    QJsonObject wide = basic();
    wide.insert(QStringLiteral("width"), kMaxWidth + 1);
    check(parseError(wide) == QLatin1String("bad_size"), "an oversized width is refused");
    QJsonObject fractional = basic();
    fractional.insert(QStringLiteral("height"), 300.5);
    check(parseError(fractional) == QLatin1String("bad_size"), "a fractional height is refused");
    QJsonObject badId = basic();
    badId.insert(QStringLiteral("window_id"), QStringLiteral("../etc"));
    check(parseError(badId) == QLatin1String("bad_window"), "a malformed window id is refused");
}

static void testHelpers()
{
    check(windowIdValid(mintWindowId()), "minted ids are valid");
    check(!windowIdValid(QStringLiteral("w12")), "short ids are invalid");
    check(actionFromLink(QUrl(QStringLiteral("action:Ship%20it"))) == QLatin1String("Ship it"),
          "action links carry their label");
    check(actionFromLink(QUrl(QStringLiteral("https://example.com"))).isEmpty(),
          "web links are not actions");
    check(localResourceAllowed(QUrl(QStringLiteral("chart.png"))), "relative images load");
    check(localResourceAllowed(QUrl::fromLocalFile(QStringLiteral("/tmp/a.png"))),
          "local file images load");
    check(localResourceAllowed(QUrl(QStringLiteral("data:image/png;base64,AAAA"))),
          "data images load");
    check(!localResourceAllowed(QUrl(QStringLiteral("https://example.com/x.png"))),
          "remote images never load");
    check(!localResourceAllowed(QUrl(QStringLiteral("data:text/html;base64,AAAA"))),
          "non-image data never loads");
    check(windowTitle(QStringLiteral("Fix login"), QStringLiteral("Plan")) ==
              QStringLiteral("Plan — Fix login"),
          "the title names the agent");
}

static void testRegistry()
{
    QWidget host;
    Registry registry(&host);
    const QJsonObject unknown =
        registry.handle(1, QString(), QString(), QStringLiteral("windows.nope"),
                        QJsonObject{{QStringLiteral("window_id"), mintWindowId()}});
    check(unknown.value(QStringLiteral("error")).toString() == QLatin1String("unknown_method"),
          "an unknown window method is refused");

    QJsonObject ask = basic();
    ask.insert(QStringLiteral("buttons"), QJsonArray{QStringLiteral("Approve"),
                                                     QStringLiteral("Reject")});
    ask.insert(QStringLiteral("input_label"), QStringLiteral("Notes"));
    const QJsonObject shown =
        registry.handle(7, QStringLiteral("Agent seven"), QString(), ShowMethod, ask);
    check(shown.value(QStringLiteral("ok")).toBool(), "show opens a window");
    const QString id = shown.value(QStringLiteral("window_id")).toString();
    check(windowIdValid(id), "show returns a window id");
    check(shown.value(QStringLiteral("open")).toBool() &&
              !shown.value(QStringLiteral("answered")).toBool() &&
              shown.value(QStringLiteral("has_input")).toBool(),
          "a new window is open and unanswered");
    const WindowState *state = registry.state(id);
    check(state && state->widget && state->widget->isVisible(), "the window is shown");

    const QJsonObject foreign =
        registry.handle(8, QString(), QString(), ReadMethod,
                        QJsonObject{{QStringLiteral("window_id"), id}});
    check(foreign.value(QStringLiteral("error")).toString() == QLatin1String("not_found"),
          "another agent cannot read the window");
    QJsonObject hijack = basic();
    hijack.insert(QStringLiteral("window_id"), id);
    check(registry.handle(8, QString(), QString(), ShowMethod, hijack)
                  .value(QStringLiteral("error"))
                  .toString() == QLatin1String("not_found"),
          "another agent cannot replace the window");

    QLineEdit *input = state->widget->findChild<QLineEdit *>();
    check(input != nullptr, "the input field is built");
    if (input)
        input->setText(QStringLiteral("looks good"));
    QPushButton *approve = nullptr;
    for (QPushButton *button : state->widget->findChildren<QPushButton *>())
        if (button->text() == QLatin1String("Approve"))
            approve = button;
    check(approve != nullptr, "the Approve button is built");
    if (approve)
        approve->click();
    const QJsonObject read = registry.handle(7, QString(), QString(), ReadMethod,
                                             QJsonObject{{QStringLiteral("window_id"), id}});
    check(read.value(QStringLiteral("answered")).toBool(), "a click answers the window");
    check(read.value(QStringLiteral("button")).toString() == QLatin1String("Approve"),
          "the pressed button is reported");
    check(read.value(QStringLiteral("input")).toString() == QLatin1String("looks good"),
          "the typed input is reported");

    QJsonObject update = basic();
    update.insert(QStringLiteral("window_id"), id);
    update.insert(QStringLiteral("format"), QStringLiteral("text"));
    const QJsonObject updated =
        registry.handle(7, QStringLiteral("Agent seven"), QString(), ShowMethod, update);
    check(updated.value(QStringLiteral("window_id")).toString() == id,
          "show with window_id reuses the window");
    check(!updated.value(QStringLiteral("answered")).toBool(),
          "replacing the content clears the answer");
    check(registry.openCount(7) == 1, "an update does not open a second window");

    const QJsonObject closed = registry.handle(7, QString(), QString(), CloseMethod,
                                               QJsonObject{{QStringLiteral("window_id"), id}});
    check(!closed.value(QStringLiteral("open")).toBool(), "close closes the window");
    QApplication::processEvents();
    check(registry.openCount(7) == 0, "a closed window no longer counts");

    for (int i = 0; i < kMaxOpenPerSession; ++i)
        registry.handle(9, QString(), QString(), ShowMethod, basic());
    const QJsonObject over = registry.handle(9, QString(), QString(), ShowMethod, basic());
    check(over.value(QStringLiteral("error")).toString() == QLatin1String("too_many_windows"),
          "an agent cannot open more than the open-window cap");

    QJsonObject userClose = basic();
    const QString other = registry.handle(10, QString(), QString(), ShowMethod, userClose)
                              .value(QStringLiteral("window_id"))
                              .toString();
    if (const WindowState *s = registry.state(other); s && s->widget)
        s->widget->close();
    QApplication::processEvents();
    const QJsonObject afterUserClose =
        registry.handle(10, QString(), QString(), ReadMethod,
                        QJsonObject{{QStringLiteral("window_id"), other}});
    check(afterUserClose.value(QStringLiteral("ok")).toBool() &&
              !afterUserClose.value(QStringLiteral("open")).toBool(),
          "a window the user closed reads as closed");
}

static void testBrowser()
{
    BrowseRequest request;
    QString error;
    check(parseBrowseRequest(QJsonObject{{QStringLiteral("url"),
                                          QStringLiteral("http://localhost:3000/app")},
                                         {QStringLiteral("title"), QStringLiteral(" Dev ")}},
                             &request, &error),
          "a localhost URL parses");
    check(request.url.port() == 3000 && request.title == QLatin1String("Dev"),
          "the URL and title are kept");
    for (const char *bad : {"file:///etc/passwd", "javascript:alert(1)", "https://",
                            "https://user:pw@example.com/", "ftp://example.com/"}) {
        check(!parseBrowseRequest(QJsonObject{{QStringLiteral("url"),
                                               QString::fromLatin1(bad)}},
                                  nullptr, &error) &&
                  error == QLatin1String("bad_url"),
              "a non-web or credentialed URL is refused");
    }
    check(browserUrlFromInput(QStringLiteral("forkmesh.com")) ==
              QUrl(QStringLiteral("https://forkmesh.com")),
          "a typed host gets https");
    check(!browserUrlFromInput(QStringLiteral("file:///etc/passwd")).isValid(),
          "the address bar refuses file URLs");
    check(!browserUrlFromInput(QStringLiteral("javascript:alert(1)")).isValid(),
          "the address bar refuses script URLs");
    check(browserUrlFromInput(QStringLiteral("localhost:3000")) ==
              QUrl(QStringLiteral("http://localhost:3000")),
          "a typed localhost gets http");

    QWidget host;
    Registry registry(&host);
    QList<QUrl> opened;
    registry.setExternalOpener([&opened](const QUrl &url) {
        opened << url;
        return true;
    });
    const QJsonObject reply = registry.handle(
        4, QStringLiteral("Agent four"), QString(), BrowseMethod,
        QJsonObject{{QStringLiteral("url"), QStringLiteral("http://localhost:3000/")}});
    check(reply.value(QStringLiteral("ok")).toBool(), "browse succeeds");
    check(reply.value(QStringLiteral("kind")).toString() == QLatin1String("browser"),
          "browse replies with a browser window");
    if (embeddedBrowserAvailable()) {
        const QString id = reply.value(QStringLiteral("window_id")).toString();
        check(windowIdValid(id) && reply.value(QStringLiteral("embedded")).toBool() &&
                  reply.value(QStringLiteral("open")).toBool(),
              "with WebEngine the page opens in a window");
        check(opened.isEmpty(), "with WebEngine nothing opens outside the app");
        check(registry.state(id) && registry.state(id)->widget &&
                  registry.state(id)->widget->findChild<QLineEdit *>(
                      QStringLiteral("agentWindowAddress")),
              "the browser window has an address bar");
        registry.setPageState(id, QUrl(QStringLiteral("http://localhost:3000/login")),
                              QStringLiteral("Login"));
        const QJsonObject read = registry.handle(
            4, QString(), QString(), ReadMethod, QJsonObject{{QStringLiteral("window_id"), id}});
        check(read.value(QStringLiteral("url")).toString() ==
                      QLatin1String("http://localhost:3000/login") &&
                  read.value(QStringLiteral("page_title")).toString() == QLatin1String("Login"),
              "read_window reports the page the user is on");
        const QJsonObject navigated = registry.handle(
            4, QString(), QString(), BrowseMethod,
            QJsonObject{{QStringLiteral("url"), QStringLiteral("https://forkmesh.com/")},
                        {QStringLiteral("window_id"), id}});
        check(navigated.value(QStringLiteral("window_id")).toString() == id &&
                  registry.openCount(4) == 1,
              "browse with window_id navigates the same window");
        QJsonObject show = basic();
        show.insert(QStringLiteral("window_id"), id);
        registry.handle(4, QString(), QString(), ShowMethod, show);
        check(!registry.state(id)->browser, "a browser window can turn into a content window");
    } else {
        check(reply.value(QStringLiteral("external")).toBool() &&
                  !reply.value(QStringLiteral("open")).toBool(),
              "without WebEngine the reply says the page opened outside");
        check(opened == QList<QUrl>{QUrl(QStringLiteral("http://localhost:3000/"))},
              "without WebEngine the system browser gets the URL");
        check(registry.openCount(4) == 0, "an external page is not an open window");
    }
}

static void testCaptureParsing()
{
    CaptureRequest request;
    QString error;
    check(parseCaptureRequest(QJsonObject{{QStringLiteral("window_id"), mintWindowId()}},
                              &request, &error) &&
              request.maxChars == kDefaultPageChars && !request.screenshot,
          "a page read defaults to text only");
    check(!parseCaptureRequest(QJsonObject{{QStringLiteral("window_id"), mintWindowId()},
                                           {QStringLiteral("max_chars"), kMaxPageChars + 1}},
                               nullptr, &error) &&
              error == QLatin1String("bad_range"),
          "an oversized read is refused");
    check(!parseCaptureRequest(QJsonObject{{QStringLiteral("window_id"), mintWindowId()},
                                           {QStringLiteral("screenshot"), 1}},
                               nullptr, &error) &&
              error == QLatin1String("bad_flag"),
          "a non-boolean flag is refused");
    request.maxChars = 5;
    request.offset = 2;
    request.links = true;
    const QString script = pageCaptureScript(request);
    check(script.contains(QLatin1String("(5, 2, true, 200, 5000)")),
          "the script carries the requested range");
    const QJsonObject reply = pageCaptureReply(
        QStringLiteral("w0123456789abcdef"),
        QJsonObject{{QStringLiteral("text"), QStringLiteral("abcdefgh")},
                    {QStringLiteral("total"), 20},
                    {QStringLiteral("links"),
                     QJsonArray{QJsonObject{{QStringLiteral("text"), QStringLiteral("ok")},
                                            {QStringLiteral("href"),
                                             QStringLiteral("https://forkmesh.com/")}},
                                QJsonObject{{QStringLiteral("text"), QStringLiteral("bad")},
                                            {QStringLiteral("href"),
                                             QStringLiteral("javascript:alert(1)")}}}}},
        request);
    check(reply.value(QStringLiteral("text")).toString() == QLatin1String("abcde") &&
              reply.value(QStringLiteral("truncated")).toBool(),
          "the reply is capped and says it was truncated");
    check(reply.value(QStringLiteral("links")).toArray().size() == 1,
          "non-web links are dropped");

    QWidget host;
    Registry registry(&host);
    QJsonObject last;
    const auto capture = [&](int session, const QJsonObject &params) {
        last = QJsonObject();
        registry.capture(session, params, [&last](const QJsonObject &r) { last = r; });
        return last;
    };
    check(capture(1, QJsonObject{{QStringLiteral("window_id"), mintWindowId()}})
                  .value(QStringLiteral("error"))
                  .toString() == QLatin1String("not_found"),
          "an unknown window cannot be read");
    const QString content = registry.handle(1, QString(), QString(), ShowMethod, basic())
                                .value(QStringLiteral("window_id"))
                                .toString();
    check(capture(1, QJsonObject{{QStringLiteral("window_id"), content}})
                  .value(QStringLiteral("error"))
                  .toString() == QLatin1String("not_browser"),
          "a content window is not a page");
    check(capture(2, QJsonObject{{QStringLiteral("window_id"), content}})
                  .value(QStringLiteral("error"))
                  .toString() == QLatin1String("not_found"),
          "another agent cannot read the window");
}

#ifdef FORKMESH_HAVE_WEBENGINE
static void testCaptureLive()
{
    QTcpServer http;
    http.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&http, &QTcpServer::newConnection, &http, [&http] {
        while (QTcpSocket *socket = http.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                const QByteArray body =
                    "<html><head><title>Dev page</title></head><body><h1>Hello from the "
                    "dev server</h1><a href=\"https://forkmesh.com/\">ForkMesh</a>"
                    "<script>console.error('boom');</script></body></html>";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                              QByteArray::number(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });
    QWidget host;
    Registry registry(&host);
    const QString url = QStringLiteral("http://127.0.0.1:%1/").arg(http.serverPort());
    const QString id = registry.handle(6, QString(), QString(), BrowseMethod,
                                       QJsonObject{{QStringLiteral("url"), url}})
                           .value(QStringLiteral("window_id"))
                           .toString();
    QJsonObject reply;
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 20000) {
        QEventLoop loop;
        bool answered = false;
        registry.capture(6,
                         QJsonObject{{QStringLiteral("window_id"), id},
                                     {QStringLiteral("include_links"), true},
                                     {QStringLiteral("screenshot"), true}},
                         [&](const QJsonObject &r) {
                             reply = r;
                             answered = true;
                             loop.quit();
                         });
        if (!answered)
            loop.exec();
        if (reply.value(QStringLiteral("text")).toString().contains(QLatin1String("Hello")) &&
            !reply.value(QStringLiteral("console")).toArray().isEmpty())
            break;
        QTimer::singleShot(200, &loop, &QEventLoop::quit);
        loop.exec();
    }
    check(reply.value(QStringLiteral("text")).toString().contains(
              QLatin1String("Hello from the dev server")),
          "the page text is read");
    check(reply.value(QStringLiteral("title")).toString() == QLatin1String("Dev page"),
          "the page title is read");
    check(reply.value(QStringLiteral("links")).toArray().size() == 1,
          "the page links are read");
    const QJsonArray console = reply.value(QStringLiteral("console")).toArray();
    check(!console.isEmpty() &&
              console.first().toObject().value(QStringLiteral("message")).toString() ==
                  QLatin1String("boom"),
          "console errors are read");
    const QByteArray png = QByteArray::fromBase64(
        reply.value(QStringLiteral("screenshot_png")).toString().toLatin1());
    check(png.startsWith("\x89PNG"), "a screenshot is a PNG");
    registry.setShared(id, false);
    QJsonObject refused;
    registry.capture(6, QJsonObject{{QStringLiteral("window_id"), id}},
                     [&refused](const QJsonObject &r) { refused = r; });
    check(refused.value(QStringLiteral("error")).toString() == QLatin1String("not_shared"),
          "a page the user stopped sharing cannot be read");
}
#endif

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    testParse();
    testHelpers();
    testRegistry();
    testBrowser();
    testCaptureParsing();
#ifdef FORKMESH_HAVE_WEBENGINE
    testCaptureLive();
#endif
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "agent window tests passed\n";
    return 0;
}
