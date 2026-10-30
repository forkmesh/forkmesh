#include "AgentWindows.h"

#include "WebEngine.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QLocale>
#include <QTime>
#include <QTimer>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScreen>
#include <QShortcut>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>


#include <functional>
#include <memory>
#include <utility>

namespace forkmesh::agentwindows {

namespace {

QString collapsed(const QString &text)
{
    return text.simplified();
}

class Browser : public QTextBrowser
{
public:
    Browser(const QString &workdir, QWidget *parent)
        : QTextBrowser(parent), m_workdir(workdir)
    {
    }

    QVariant loadResource(int type, const QUrl &name) override
    {
        if (type != QTextDocument::ImageResource || !localResourceAllowed(name))
            return {};
        if (name.scheme() == QLatin1String("data")) {
            const QString raw = name.toString(QUrl::FullyEncoded);
            const int comma = raw.indexOf(QLatin1Char(','));
            if (comma < 0 || !raw.left(comma).endsWith(QLatin1String(";base64")))
                return {};
            const QByteArray bytes =
                QByteArray::fromBase64(raw.mid(comma + 1).toLatin1());
            if (bytes.isEmpty() || bytes.size() > kMaxImageBytes)
                return {};
            const QImage image = QImage::fromData(bytes);
            return image.isNull() ? QVariant() : QVariant(image);
        }
        QString path = name.isLocalFile() ? name.toLocalFile() : name.path();
        if (path.isEmpty())
            return {};
        if (QDir::isRelativePath(path)) {
            if (m_workdir.isEmpty())
                return {};
            path = QDir(m_workdir).filePath(path);
        }
        const QFileInfo info(path);
        if (!info.isFile() || info.size() > kMaxImageBytes)
            return {};
        const QImage image(info.absoluteFilePath());
        return image.isNull() ? QVariant() : QVariant(image);
    }

private:
    QString m_workdir;
};

class Frame : public QWidget
{
public:
    Frame(QWidget *parent, std::function<void()> onClose)
        : QWidget(parent, Qt::Tool), m_onClose(std::move(onClose))
    {
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (m_onClose)
            m_onClose();
        QWidget::closeEvent(event);
    }

private:
    std::function<void()> m_onClose;
};

}

bool windowIdValid(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^w[0-9a-f]{6,32}$"));
    return re.match(id).hasMatch();
}

QString mintWindowId()
{
    return QStringLiteral("w%1").arg(QRandomGenerator::system()->generate64(), 16, 16,
                                     QLatin1Char('0'));
}

bool parseShowRequest(const QJsonObject &params, ShowRequest *out, QString *error)
{
    const auto fail = [error](const char *code) {
        if (error)
            *error = QString::fromLatin1(code);
        return false;
    };
    ShowRequest request;
    const QJsonValue idValue = params.value(QStringLiteral("window_id"));
    if (!idValue.isUndefined() && !idValue.isNull()) {
        if (!idValue.isString() || !windowIdValid(idValue.toString()))
            return fail("bad_window");
        request.windowId = idValue.toString();
    }
    const QJsonValue titleValue = params.value(QStringLiteral("title"));
    if (!titleValue.isString())
        return fail("bad_title");
    request.title = collapsed(titleValue.toString());
    if (request.title.isEmpty() || request.title.size() > kMaxTitleChars)
        return fail("bad_title");
    const QJsonValue bodyValue = params.value(QStringLiteral("body"));
    if (!bodyValue.isString())
        return fail("bad_body");
    request.body = bodyValue.toString();
    if (request.body.trimmed().isEmpty() || request.body.size() > kMaxBodyChars)
        return fail("bad_body");
    const QJsonValue formatValue = params.value(QStringLiteral("format"));
    if (!formatValue.isUndefined() && !formatValue.isNull()) {
        const QString format = formatValue.toString().trimmed().toLower();
        if (format != QLatin1String("markdown") && format != QLatin1String("html") &&
            format != QLatin1String("text"))
            return fail("bad_format");
        request.format = format;
    }
    const QJsonValue buttonsValue = params.value(QStringLiteral("buttons"));
    if (!buttonsValue.isUndefined() && !buttonsValue.isNull()) {
        if (!buttonsValue.isArray() || buttonsValue.toArray().size() > kMaxButtons)
            return fail("bad_buttons");
        for (const QJsonValue &raw : buttonsValue.toArray()) {
            if (!raw.isString())
                return fail("bad_buttons");
            const QString label = collapsed(raw.toString());
            if (label.isEmpty() || label.size() > kMaxButtonChars ||
                request.buttons.contains(label))
                return fail("bad_buttons");
            request.buttons << label;
        }
    }
    const QJsonValue labelValue = params.value(QStringLiteral("input_label"));
    if (!labelValue.isUndefined() && !labelValue.isNull()) {
        if (!labelValue.isString())
            return fail("bad_input");
        request.inputLabel = collapsed(labelValue.toString());
        if (request.inputLabel.size() > kMaxLabelChars)
            return fail("bad_input");
    }
    const QJsonValue multilineValue = params.value(QStringLiteral("input_multiline"));
    if (!multilineValue.isUndefined() && !multilineValue.isNull()) {
        if (!multilineValue.isBool())
            return fail("bad_input");
        request.inputMultiline = multilineValue.toBool();
    }
    const auto size = [&params](const char *key, int low, int high, int *outValue) {
        const QJsonValue value = params.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull())
            return true;
        if (!value.isDouble())
            return false;
        const double number = value.toDouble();
        if (number != double(int(number)) || number < low || number > high)
            return false;
        *outValue = int(number);
        return true;
    };
    if (!size("width", kMinWidth, kMaxWidth, &request.width) ||
        !size("height", kMinHeight, kMaxHeight, &request.height))
        return fail("bad_size");
    if (out)
        *out = request;
    return true;
}

bool parseBrowseRequest(const QJsonObject &params, BrowseRequest *out, QString *error)
{
    const auto fail = [error](const char *code) {
        if (error)
            *error = QString::fromLatin1(code);
        return false;
    };
    BrowseRequest request;
    const QJsonValue idValue = params.value(QStringLiteral("window_id"));
    if (!idValue.isUndefined() && !idValue.isNull()) {
        if (!idValue.isString() || !windowIdValid(idValue.toString()))
            return fail("bad_window");
        request.windowId = idValue.toString();
    }
    const QJsonValue urlValue = params.value(QStringLiteral("url"));
    if (!urlValue.isString() || urlValue.toString().size() > kMaxUrlChars)
        return fail("bad_url");
    request.url = QUrl(urlValue.toString().trimmed(), QUrl::StrictMode);
    if (!browserUrlAllowed(request.url))
        return fail("bad_url");
    const QJsonValue titleValue = params.value(QStringLiteral("title"));
    if (!titleValue.isUndefined() && !titleValue.isNull()) {
        if (!titleValue.isString())
            return fail("bad_title");
        request.title = collapsed(titleValue.toString());
        if (request.title.size() > kMaxTitleChars)
            return fail("bad_title");
    }
    const auto size = [&params](const char *key, int low, int high, int *outValue) {
        const QJsonValue value = params.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull())
            return true;
        if (!value.isDouble())
            return false;
        const double number = value.toDouble();
        if (number != double(int(number)) || number < low || number > high)
            return false;
        *outValue = int(number);
        return true;
    };
    if (!size("width", kMinWidth, kMaxWidth, &request.width) ||
        !size("height", kMinHeight, kMaxHeight, &request.height))
        return fail("bad_size");
    if (out)
        *out = request;
    return true;
}

bool parseCaptureRequest(const QJsonObject &params, CaptureRequest *out, QString *error)
{
    const auto fail = [error](const char *code) {
        if (error)
            *error = QString::fromLatin1(code);
        return false;
    };
    CaptureRequest request;
    const QJsonValue idValue = params.value(QStringLiteral("window_id"));
    if (!idValue.isString() || !windowIdValid(idValue.toString()))
        return fail("bad_window");
    request.windowId = idValue.toString();
    const auto number = [&params](const char *key, int low, int high, int *outValue) {
        const QJsonValue value = params.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull())
            return true;
        if (!value.isDouble())
            return false;
        const double n = value.toDouble();
        if (n != double(qint64(n)) || n < low || n > high)
            return false;
        *outValue = int(n);
        return true;
    };
    if (!number("max_chars", 1, kMaxPageChars, &request.maxChars) ||
        !number("offset", 0, kMaxPageOffset, &request.offset))
        return fail("bad_range");
    for (const auto &[key, target] :
         {std::pair<const char *, bool *>{"include_links", &request.links},
          std::pair<const char *, bool *>{"screenshot", &request.screenshot}}) {
        const QJsonValue value = params.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull())
            continue;
        if (!value.isBool())
            return fail("bad_flag");
        *target = value.toBool();
    }
    if (out)
        *out = request;
    return true;
}

QString pageCaptureScript(const CaptureRequest &request)
{
    return QStringLiteral(
               "(function(maxChars, offset, wantLinks, maxLinks, maxSel) {"
               "var body = document.body;"
               "var text = body ? (body.innerText || '') : '';"
               "var sel = window.getSelection ? String(window.getSelection()) : '';"
               "var links = [];"
               "if (wantLinks) {"
               "var anchors = document.querySelectorAll('a[href]');"
               "for (var i = 0; i < anchors.length && links.length < maxLinks; ++i) {"
               "var a = anchors[i];"
               "links.push({text: (a.innerText || a.title || '').trim().slice(0, 200),"
               " href: String(a.href).slice(0, 2048)});"
               "}"
               "}"
               "var root = document.documentElement;"
               "return JSON.stringify({text: text.slice(offset, offset + maxChars),"
               " total: text.length, selection: sel.slice(0, maxSel), links: links,"
               " scroll_y: Math.round(window.scrollY || 0),"
               " page_height: root ? root.scrollHeight : 0,"
               " viewport_height: window.innerHeight || 0,"
               " ready_state: document.readyState});"
               "})(%1, %2, %3, %4, %5)")
        .arg(request.maxChars)
        .arg(request.offset)
        .arg(request.links ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(kMaxPageLinks)
        .arg(kMaxSelectionChars);
}

QJsonObject pageCaptureReply(const QString &windowId, const QJsonObject &page,
                             const CaptureRequest &request)
{
    const QString text = page.value(QStringLiteral("text")).toString().left(request.maxChars);
    const qint64 total = qint64(page.value(QStringLiteral("total")).toDouble());
    QJsonObject reply{
        {QStringLiteral("ok"), true},
        {QStringLiteral("window_id"), windowId},
        {QStringLiteral("kind"), QStringLiteral("page")},
        {QStringLiteral("open"), true},
        {QStringLiteral("text"), text},
        {QStringLiteral("offset"), request.offset},
        {QStringLiteral("total_chars"), double(total)},
        {QStringLiteral("truncated"), request.offset + text.size() < total},
        {QStringLiteral("selection"),
         page.value(QStringLiteral("selection")).toString().left(kMaxSelectionChars)},
        {QStringLiteral("scroll_y"), page.value(QStringLiteral("scroll_y")).toInt()},
        {QStringLiteral("page_height"), page.value(QStringLiteral("page_height")).toInt()},
        {QStringLiteral("viewport_height"),
         page.value(QStringLiteral("viewport_height")).toInt()},
        {QStringLiteral("ready_state"), page.value(QStringLiteral("ready_state")).toString()},
    };
    if (request.links) {
        QJsonArray links;
        for (const QJsonValue &value : page.value(QStringLiteral("links")).toArray()) {
            if (links.size() >= kMaxPageLinks)
                break;
            const QJsonObject link = value.toObject();
            const QUrl href(link.value(QStringLiteral("href")).toString());
            if (!browserUrlAllowed(href))
                continue;
            links.append(QJsonObject{
                {QStringLiteral("text"), link.value(QStringLiteral("text")).toString().left(200)},
                {QStringLiteral("href"), href.toString()}});
        }
        reply.insert(QStringLiteral("links"), links);
    }
    return reply;
}

bool parseWidgetRequest(const QJsonObject &params, WidgetRequest *out, QString *error)
{
    const auto fail = [error](const char *code) {
        if (error)
            *error = QString::fromLatin1(code);
        return false;
    };
    WidgetRequest request;
    const QJsonValue idValue = params.value(QStringLiteral("window_id"));
    if (!idValue.isUndefined() && !idValue.isNull()) {
        if (!idValue.isString() || !windowIdValid(idValue.toString()))
            return fail("bad_window");
        request.windowId = idValue.toString();
    }
    const QJsonValue titleValue = params.value(QStringLiteral("title"));
    if (!titleValue.isUndefined() && !titleValue.isNull()) {
        if (!titleValue.isString())
            return fail("bad_title");
        request.title = collapsed(titleValue.toString());
        if (request.title.size() > kMaxTitleChars)
            return fail("bad_title");
    }
    const QJsonValue htmlValue = params.value(QStringLiteral("html"));
    if (!htmlValue.isUndefined() && !htmlValue.isNull()) {
        if (!htmlValue.isString())
            return fail("bad_html");
        request.html = htmlValue.toString();
        if (request.html.size() > kMaxWidgetHtmlChars)
            return fail("bad_html");
    }
    if (params.contains(QStringLiteral("data"))) {
        request.data = params.value(QStringLiteral("data"));
        request.hasData = true;
        const QByteArray encoded =
            QJsonDocument(QJsonArray{request.data}).toJson(QJsonDocument::Compact);
        if (encoded.size() > kMaxWidgetDataChars + 2)
            return fail("bad_data");
    }
    if (request.html.trimmed().isEmpty()) {
        if (request.windowId.isEmpty() || !request.hasData)
            return fail("bad_html");
    } else if (request.title.isEmpty() && request.windowId.isEmpty()) {
        return fail("bad_title");
    }
    const auto size = [&params](const char *key, int low, int high, int *outValue) {
        const QJsonValue value = params.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull())
            return true;
        if (!value.isDouble())
            return false;
        const double number = value.toDouble();
        if (number != double(int(number)) || number < low || number > high)
            return false;
        *outValue = int(number);
        return true;
    };
    if (!size("width", kMinWidth, kMaxWidth, &request.width) ||
        !size("height", kMinHeight, kMaxHeight, &request.height))
        return fail("bad_size");
    if (out)
        *out = request;
    return true;
}

bool parseActRequest(const QJsonObject &params, ActRequest *out, QString *error)
{
    const auto fail = [error](const char *code) {
        if (error)
            *error = QString::fromLatin1(code);
        return false;
    };
    static const QStringList actions{
        QStringLiteral("navigate"), QStringLiteral("back"),   QStringLiteral("forward"),
        QStringLiteral("reload"),   QStringLiteral("click"),  QStringLiteral("type"),
        QStringLiteral("press"),    QStringLiteral("scroll"), QStringLiteral("wait_for")};
    ActRequest request;
    const QJsonValue idValue = params.value(QStringLiteral("window_id"));
    if (!idValue.isString() || !windowIdValid(idValue.toString()))
        return fail("bad_window");
    request.windowId = idValue.toString();
    request.action = params.value(QStringLiteral("action")).toString().trimmed().toLower();
    if (!actions.contains(request.action))
        return fail("bad_action");
    const auto text = [&params](const char *key, int limit, QString *target) {
        const QJsonValue value = params.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull())
            return true;
        if (!value.isString() || value.toString().size() > limit)
            return false;
        *target = value.toString();
        return true;
    };
    QString url;
    if (!text("selector", kMaxSelectorChars, &request.selector) ||
        !text("target", kMaxSelectorChars, &request.target) ||
        !text("value", kMaxActionValueChars, &request.value) ||
        !text("url", kMaxUrlChars, &url))
        return fail("bad_argument");
    request.selector = request.selector.trimmed();
    request.target = collapsed(request.target);
    for (const auto &[key, target] :
         {std::pair<const char *, bool *>{"submit", &request.submit},
          std::pair<const char *, bool *>{"clear", &request.clear}}) {
        const QJsonValue value = params.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull())
            continue;
        if (!value.isBool())
            return fail("bad_flag");
        *target = value.toBool();
    }
    const auto number = [&params](const char *key, int low, int high, int *outValue) {
        const QJsonValue value = params.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull())
            return true;
        if (!value.isDouble())
            return false;
        const double n = value.toDouble();
        if (n != double(qint64(n)) || n < low || n > high)
            return false;
        *outValue = int(n);
        return true;
    };
    if (!number("delta", -100000, 100000, &request.delta) ||
        !number("wait_ms", 0, kMaxActionWaitMs, &request.waitMs))
        return fail("bad_range");
    const bool needsElement = request.action == QLatin1String("click") ||
                              request.action == QLatin1String("type") ||
                              request.action == QLatin1String("wait_for");
    if (needsElement && request.selector.isEmpty() && request.target.isEmpty())
        return fail("no_element");
    if (request.action == QLatin1String("navigate")) {
        request.url = browserUrlFromInput(url);
        if (!request.url.isValid())
            return fail("bad_url");
    }
    if (request.action == QLatin1String("press") && collapsed(request.value).isEmpty())
        return fail("no_key");
    if (out)
        *out = request;
    return true;
}

QString pageActionScript(const ActRequest &request)
{
    const QJsonObject args{
        {QStringLiteral("action"), request.action},
        {QStringLiteral("selector"), request.selector},
        {QStringLiteral("target"), request.target},
        {QStringLiteral("value"), request.value},
        {QStringLiteral("submit"), request.submit},
        {QStringLiteral("clear"), request.clear},
        {QStringLiteral("delta"), request.delta},
    };
    return QStringLiteral(
               "(function(req) {"
               "function shown(el) {"
               "var r = el.getBoundingClientRect(); var s = window.getComputedStyle(el);"
               "return r.width > 0 && r.height > 0 && s.visibility !== 'hidden' &&"
               " s.display !== 'none';"
               "}"
               "function name(el) {"
               "var parts = [el.innerText, el.getAttribute('aria-label'),"
               " el.getAttribute('placeholder'), el.getAttribute('title'),"
               " el.getAttribute('name'), el.getAttribute('alt')];"
               "if (el.labels) for (var i = 0; i < el.labels.length; ++i)"
               " parts.push(el.labels[i].innerText);"
               "if (el.tagName === 'INPUT' && (el.type === 'submit' || el.type === 'button'))"
               " parts.unshift(el.value);"
               "for (var j = 0; j < parts.length; ++j) {"
               "var t = String(parts[j] || '').replace(/\\s+/g, ' ').trim();"
               "if (t) return t;"
               "}"
               "return '';"
               "}"
               "function find() {"
               "if (req.selector) {"
               "var all;"
               "try { all = document.querySelectorAll(req.selector); }"
               " catch (e) { return {error: 'bad_selector'}; }"
               "for (var i = 0; i < all.length; ++i) if (shown(all[i])) return {el: all[i]};"
               "return all.length ? {el: all[0]} : {error: 'not_found'};"
               "}"
               "var typing = req.action === 'type';"
               "var query = typing ? 'input,textarea,select,[contenteditable]'"
               " : 'a,button,input,select,textarea,summary,label,[role],[onclick],[tabindex]';"
               "var want = req.target.toLowerCase(), exact = null, partial = null;"
               "var nodes = document.querySelectorAll(query);"
               "for (var k = 0; k < nodes.length; ++k) {"
               "var c = nodes[k]; if (!shown(c)) continue;"
               "var label = name(c).toLowerCase(); if (!label) continue;"
               "if (label === want) { exact = c; break; }"
               "if (!partial && label.indexOf(want) >= 0) partial = c;"
               "}"
               "var hit = exact || partial;"
               "return hit ? {el: hit} : {error: 'not_found'};"
               "}"
               "function describe(el) {"
               "return {tag: el.tagName.toLowerCase(), text: name(el).slice(0, 200)};"
               "}"
               "function key(el, k) {"
               "var opts = {key: k, bubbles: true, cancelable: true};"
               "var go = el.dispatchEvent(new KeyboardEvent('keydown', opts));"
               "if (k.length === 1) el.dispatchEvent(new KeyboardEvent('keypress', opts));"
               "el.dispatchEvent(new KeyboardEvent('keyup', opts));"
               "if (go && k === 'Enter' && el.form && el.tagName === 'INPUT') {"
               "if (el.form.requestSubmit) el.form.requestSubmit(); else el.form.submit();"
               "}"
               "}"
               "var out = {ok: true, action: req.action};"
               "var target = null;"
               "if (req.selector || req.target) {"
               "var found = find();"
               "if (found.error) {"
               "if (req.action === 'wait_for') return JSON.stringify({ok: true, found: false});"
               "return JSON.stringify({ok: false, error: found.error});"
               "}"
               "target = found.el; out.element = describe(target);"
               "}"
               "if (req.action === 'wait_for') { out.found = true; return JSON.stringify(out); }"
               "if (req.action === 'click') {"
               "target.scrollIntoView({block: 'center', inline: 'center'});"
               "var r = target.getBoundingClientRect();"
               "var at = {bubbles: true, cancelable: true, view: window, button: 0,"
               " clientX: r.left + r.width / 2, clientY: r.top + r.height / 2};"
               "if (window.PointerEvent) target.dispatchEvent(new PointerEvent('pointerdown', at));"
               "target.dispatchEvent(new MouseEvent('mousedown', at));"
               "if (target.focus) target.focus();"
               "if (window.PointerEvent) target.dispatchEvent(new PointerEvent('pointerup', at));"
               "target.dispatchEvent(new MouseEvent('mouseup', at));"
               "target.click();"
               "} else if (req.action === 'type') {"
               "if (target.tagName === 'INPUT' && String(target.type).toLowerCase() === 'password')"
               " return JSON.stringify({ok: false, error: 'password_field'});"
               "target.scrollIntoView({block: 'center'});"
               "if (target.focus) target.focus();"
               "if (target.tagName === 'SELECT') {"
               "var pick = null, w = req.value.trim().toLowerCase();"
               "for (var o = 0; o < target.options.length; ++o) {"
               "var opt = target.options[o];"
               "if (opt.value.toLowerCase() === w || opt.text.trim().toLowerCase() === w)"
               " { pick = opt; break; }"
               "}"
               "if (!pick) return JSON.stringify({ok: false, error: 'no_option'});"
               "target.value = pick.value;"
               "} else if (target.isContentEditable) {"
               "if (req.clear) document.execCommand('selectAll', false, null);"
               "document.execCommand('insertText', false, req.value);"
               "} else {"
               "target.value = req.clear ? req.value : String(target.value || '') + req.value;"
               "}"
               "target.dispatchEvent(new Event('input', {bubbles: true}));"
               "target.dispatchEvent(new Event('change', {bubbles: true}));"
               "if (req.submit) key(target, 'Enter');"
               "} else if (req.action === 'press') {"
               "key(target || document.activeElement || document.body, req.value.trim());"
               "} else if (req.action === 'scroll') {"
               "if (target) target.scrollIntoView({block: 'center'});"
               "else window.scrollBy(0, req.delta || Math.round(window.innerHeight * 0.8));"
               "out.scroll_y = Math.round(window.scrollY || 0);"
               "out.page_height = document.documentElement ? "
               "document.documentElement.scrollHeight : 0;"
               "}"
               "return JSON.stringify(out);"
               "})(%1)")
        .arg(QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)));
}

QString widgetEventMarker()
{
    return QStringLiteral("forkmesh-widget-event:");
}

QString widgetBridgeScript(const QJsonValue &data)
{
    const QByteArray encoded =
        QJsonDocument(QJsonArray{data}).toJson(QJsonDocument::Compact);
    return QStringLiteral(
               "(function(initial) {"
               "var listeners = [];"
               "var bridge = {"
               "data: initial[0],"
               "send: function(value) {"
               "var text = JSON.stringify(value === undefined ? null : value);"
               "if (text.length > %2) throw new Error('forkmesh.send: an event is at most "
               "%2 characters');"
               "console.log('%1' + text);"
               "},"
               "onData: function(callback) { if (typeof callback === 'function') "
               "listeners.push(callback); },"
               "_receive: function(value) {"
               "bridge.data = value;"
               "for (var i = 0; i < listeners.length; ++i) {"
               "try { listeners[i](value); } catch (e) { console.error(e); }"
               "}"
               "window.dispatchEvent(new CustomEvent('forkmesh-data', {detail: value}));"
               "}"
               "};"
               "Object.defineProperty(window, 'forkmesh', {value: Object.freeze(bridge)});"
               "})(%3)")
        .arg(widgetEventMarker())
        .arg(kMaxWidgetEventChars)
        .arg(QString::fromUtf8(encoded));
}

QString widgetDataScript(const QJsonValue &data)
{
    const QByteArray encoded =
        QJsonDocument(QJsonArray{data}).toJson(QJsonDocument::Compact);
    return QStringLiteral("window.forkmesh && window.forkmesh._receive((%1)[0]);")
        .arg(QString::fromUtf8(encoded));
}

bool widgetEventFromConsole(const QString &message, QJsonValue *event)
{
    const QString marker = widgetEventMarker();
    if (!message.startsWith(marker))
        return false;
    const QString body = message.mid(marker.size());
    if (body.size() > kMaxWidgetEventChars)
        return false;
    QJsonParseError error;
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray("[") + body.toUtf8() + QByteArray("]"), &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() != 1)
        return false;
    if (event)
        *event = doc.array().at(0);
    return true;
}

bool widgetRequestAllowed(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("data") || scheme == QLatin1String("blob") ||
           scheme == QLatin1String("about");
}

QString actionSummary(const ActRequest &request)
{
    const QString what = !request.target.isEmpty() ? QStringLiteral("“%1”").arg(request.target.left(60))
                                                    : request.selector.left(60);
    if (request.action == QLatin1String("navigate"))
        return QStringLiteral("opened %1").arg(request.url.host());
    if (request.action == QLatin1String("click"))
        return QStringLiteral("clicked %1").arg(what);
    if (request.action == QLatin1String("type"))
        return QStringLiteral("typed into %1").arg(what);
    if (request.action == QLatin1String("press"))
        return QStringLiteral("pressed %1").arg(collapsed(request.value).left(20));
    if (request.action == QLatin1String("scroll"))
        return QStringLiteral("scrolled");
    if (request.action == QLatin1String("wait_for"))
        return QStringLiteral("waited for %1").arg(what);
    return request.action;
}

bool browserUrlAllowed(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
           (scheme == QLatin1String("https") || scheme == QLatin1String("http"));
}

QUrl browserUrlFromInput(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kMaxUrlChars)
        return QUrl();
    static const QRegularExpression scheme(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:(?!\\d)"));
    QUrl url;
    if (trimmed.contains(QLatin1String("://")) || scheme.match(trimmed).hasMatch()) {
        url = QUrl(trimmed, QUrl::StrictMode);
    } else {
        const bool local = trimmed.startsWith(QLatin1String("localhost")) ||
                           trimmed.startsWith(QLatin1String("127."));
        url = QUrl((local ? QStringLiteral("http://") : QStringLiteral("https://")) + trimmed,
                   QUrl::StrictMode);
    }
    return browserUrlAllowed(url) ? url : QUrl();
}

bool embeddedBrowserAvailable()
{
    return web::available();
}

QString actionFromLink(const QUrl &url)
{
    if (url.scheme() != QLatin1String("action"))
        return QString();
    const QString label = collapsed(QUrl::fromPercentEncoding(url.path().toUtf8()));
    return label.size() > kMaxButtonChars ? QString() : label;
}

bool localResourceAllowed(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("data"))
        return url.toString(QUrl::FullyEncoded).startsWith(QLatin1String("data:image/"));
    return scheme.isEmpty() || scheme == QLatin1String("file");
}

QString windowTitle(const QString &agentLabel, const QString &title)
{
    const QString agent = collapsed(agentLabel);
    return agent.isEmpty() ? title : QStringLiteral("%1 — %2").arg(title, agent);
}

Registry::Registry(QWidget *host)
    : m_host(host), m_context(std::make_unique<QObject>()),
      m_externalOpener([](const QUrl &url) { return QDesktopServices::openUrl(url); })
{
}

Registry::~Registry()
{
    QList<QPointer<QWidget>> widgets;
    for (const WindowState &state : std::as_const(m_windows))
        widgets << state.widget;
    m_windows.clear();
    for (const QPointer<QWidget> &widget : widgets)
        delete widget.data();
    delete m_browserProfile;
    delete m_widgetProfile;
    m_context.reset();
}

QJsonObject Registry::handle(int sessionId, const QString &agentLabel,
                             const QString &workdir, const QString &method,
                             const QJsonObject &params)
{
    if (method == ShowMethod) {
        ShowRequest request;
        QString code;
        if (!parseShowRequest(params, &request, &code))
            return agentintake::errorReply(code, QStringLiteral("The window request is "
                                                                "malformed (%1).")
                                                     .arg(code));
        return show(sessionId, agentLabel, workdir, request);
    }
    if (method == BrowseMethod) {
        BrowseRequest request;
        QString code;
        if (!parseBrowseRequest(params, &request, &code))
            return agentintake::errorReply(
                code, code == QLatin1String("bad_url")
                          ? QStringLiteral("url must be an http or https address with "
                                           "a host and no user name or password.")
                          : QStringLiteral("The browser request is malformed (%1).")
                                .arg(code));
        return browse(sessionId, agentLabel, request);
    }
    if (method == WidgetMethod) {
        WidgetRequest request;
        QString code;
        if (!parseWidgetRequest(params, &request, &code))
            return agentintake::errorReply(
                code, code == QLatin1String("bad_html")
                          ? QStringLiteral("html is required (at most %1 characters), "
                                           "unless window_id and data update a widget.")
                                .arg(kMaxWidgetHtmlChars)
                          : QStringLiteral("The widget request is malformed (%1).").arg(code));
        return widget(sessionId, agentLabel, request);
    }
    const QString windowId = params.value(QStringLiteral("window_id")).toString();
    if (!windowIdValid(windowId))
        return agentintake::errorReply(QStringLiteral("bad_window"),
                                       QStringLiteral("window_id is not valid."));
    if (method == ReadMethod)
        return read(sessionId, windowId);
    if (method == CloseMethod)
        return close(sessionId, windowId);
    return agentintake::errorReply(QStringLiteral("unknown_method"),
                                   QStringLiteral("Unknown method."));
}

QString Registry::claimWindow(int sessionId, const QString &requested,
                              QJsonObject *error)
{
    QString id = requested;
    if (!id.isEmpty()) {
        auto it = m_windows.find(id);
        if (it == m_windows.end() || it->sessionId != sessionId) {
            *error = agentintake::errorReply(
                QStringLiteral("not_found"),
                QStringLiteral("No window %1 was opened by this agent.").arg(id));
            return QString();
        }
        if (it->kind == SuggestionKind) {
            *error = agentintake::errorReply(
                QStringLiteral("locked"),
                QStringLiteral("Window %1 asks the user about a new session; the agent "
                               "cannot change it. Close it and suggest again instead.")
                    .arg(id));
            return QString();
        }
        if (it->open)
            return id;
    }
    if (openCount(sessionId) >= kMaxOpenPerSession) {
        *error = agentintake::errorReply(
            QStringLiteral("too_many_windows"),
            QStringLiteral("This agent already has %1 windows open; close one "
                           "first or pass window_id to reuse one.")
                .arg(kMaxOpenPerSession));
        return QString();
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_newWindows.allow(now)) {
        *error = agentintake::errorReply(
            QStringLiteral("rate_limited"),
            QStringLiteral("Agents may open at most %1 windows every %2 minutes on "
                           "this desktop.")
                .arg(kNewWindowLimit)
                .arg(kNewWindowWindowMs / 60000));
        return QString();
    }
    id = mintWindowId();
    WindowState state;
    state.sessionId = sessionId;
    state.createdMs = now;
    m_windows.insert(id, state);
    prune(sessionId);
    return id;
}

QJsonObject Registry::show(int sessionId, const QString &agentLabel,
                           const QString &workdir, const ShowRequest &request)
{
    QJsonObject error;
    const QString id = claimWindow(sessionId, request.windowId, &error);
    if (id.isEmpty())
        return error;
    WindowState &state = m_windows[id];
    const bool reuse = !state.widget.isNull();
    state.agentLabel = agentLabel;
    state.workdir = workdir;
    state.request = request;
    state.request.windowId = id;
    state.browser = false;
    state.kind.clear();
    state.extra = QJsonObject();
    state.events.clear();
    state.droppedEvents = 0;
    state.url.clear();
    state.pageTitle.clear();
    state.open = true;
    state.answered = false;
    state.button.clear();
    state.input.clear();
    QWidget *widget = buildWidget(id, state);
    state.widget = widget;
    if (!reuse) {
        place(widget, state);
        widget->show();
    } else if (request.width > 0 || request.height > 0) {
        widget->resize(request.width > 0 ? request.width : widget->width(),
                       request.height > 0 ? request.height : widget->height());
    }
    return stateReply(id, state);
}

QJsonObject Registry::browse(int sessionId, const QString &agentLabel,
                             const BrowseRequest &request)
{
    if (!embeddedBrowserAvailable()) {
        if (!request.windowId.isEmpty())
            return agentintake::errorReply(
                QStringLiteral("not_found"),
                QStringLiteral("This desktop has no built-in browser, so it has no "
                               "browser window %1.")
                    .arg(request.windowId));
        if (!m_newWindows.allow(QDateTime::currentMSecsSinceEpoch()))
            return agentintake::errorReply(
                QStringLiteral("rate_limited"),
                QStringLiteral("Agents may open at most %1 windows every %2 minutes on "
                               "this desktop.")
                    .arg(kNewWindowLimit)
                    .arg(kNewWindowWindowMs / 60000));
        const bool opened = m_externalOpener && m_externalOpener(request.url);
        if (!opened)
            return agentintake::errorReply(
                QStringLiteral("open_failed"),
                QStringLiteral("The desktop could not open the system browser."));
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("kind"), QStringLiteral("browser")},
            {QStringLiteral("embedded"), false},
            {QStringLiteral("external"), true},
            {QStringLiteral("open"), false},
            {QStringLiteral("url"), request.url.toString()},
            {QStringLiteral("message"),
             QStringLiteral("This desktop was built without Qt WebEngine, so the page "
                            "opened in the user's browser instead of a window.")},
        };
    }
    QJsonObject error;
    const QString id = claimWindow(sessionId, request.windowId, &error);
    if (id.isEmpty())
        return error;
    WindowState &state = m_windows[id];
    const bool reuse = !state.widget.isNull();
    state.agentLabel = agentLabel;
    state.request = ShowRequest();
    state.request.windowId = id;
    state.request.title = request.title.isEmpty() ? request.url.host() : request.title;
    state.request.width = request.width;
    state.request.height = request.height;
    state.browser = true;
    state.kind.clear();
    state.extra = QJsonObject();
    state.events.clear();
    state.droppedEvents = 0;
    state.url = request.url;
    state.pageTitle.clear();
    state.open = true;
    state.answered = false;
    state.button.clear();
    state.input.clear();
    QWidget *widget = buildBrowser(id, state);
    state.widget = widget;
    if (!reuse) {
        place(widget, state);
        widget->show();
    } else if (request.width > 0 || request.height > 0) {
        widget->resize(request.width > 0 ? request.width : widget->width(),
                       request.height > 0 ? request.height : widget->height());
    }
    return stateReply(id, state);
}

void Registry::setShared(const QString &windowId, bool shared)
{
    auto it = m_windows.find(windowId);
    if (it != m_windows.end())
        it->shared = shared;
}

void Registry::capture(int sessionId, const QJsonObject &params,
                       std::function<void(const QJsonObject &)> reply)
{
    CaptureRequest request;
    QString code;
    if (!parseCaptureRequest(params, &request, &code)) {
        reply(agentintake::errorReply(code, QStringLiteral("The page request is malformed (%1).")
                                                .arg(code)));
        return;
    }
    auto it = m_windows.find(request.windowId);
    if (it == m_windows.end() || it->sessionId != sessionId) {
        reply(agentintake::errorReply(
            QStringLiteral("not_found"),
            QStringLiteral("No window %1 was opened by this agent.").arg(request.windowId)));
        return;
    }
    if (!it->browser) {
        reply(agentintake::errorReply(
            QStringLiteral("not_browser"),
            QStringLiteral("Window %1 is not a browser window.").arg(request.windowId)));
        return;
    }
    if (!it->open || !it->widget) {
        reply(agentintake::errorReply(
            QStringLiteral("closed"),
            QStringLiteral("The user closed browser window %1.").arg(request.windowId)));
        return;
    }
    if (!it->shared) {
        reply(agentintake::errorReply(
            QStringLiteral("not_shared"),
            QStringLiteral("The user turned off sharing this page with the agent.")));
        return;
    }
    if (!embeddedBrowserAvailable()) {
        reply(agentintake::errorReply(QStringLiteral("no_browser"),
                                      QStringLiteral("This desktop has no built-in browser.")));
        return;
    }
    QWidget *viewWidget = it->widget->findChild<QWidget *>(QStringLiteral("agentWindowWeb"));
    web::View *view = web::viewOf(viewWidget);
    if (!view) {
        reply(agentintake::errorReply(QStringLiteral("closed"),
                                      QStringLiteral("The browser window has no page.")));
        return;
    }
    QPointer<QWidget> guard(viewWidget);
    const QString windowId = request.windowId;
    auto done = std::make_shared<bool>(false);
    auto finish = [done, reply](const QJsonObject &answer) {
        if (*done)
            return;
        *done = true;
        reply(answer);
    };
    QTimer::singleShot(kCaptureTimeoutMs, m_context.get(), [finish] {
        finish(agentintake::errorReply(QStringLiteral("capture_timeout"),
                                       QStringLiteral("The page did not answer in time.")));
    });
    view->runJavaScript(
        pageCaptureScript(request), web::World::Application,
        [this, guard, windowId, request, finish](const QVariant &value) {
            web::View *live = web::viewOf(guard.data());
            if (!live) {
                finish(agentintake::errorReply(QStringLiteral("closed"),
                                               QStringLiteral("The user closed the window.")));
                return;
            }
            const QJsonObject page =
                QJsonDocument::fromJson(value.toString().toUtf8()).object();
            QJsonObject answer = pageCaptureReply(windowId, page, request);
            answer.insert(QStringLiteral("url"), live->url().toString());
            answer.insert(QStringLiteral("title"), live->title());
            answer.insert(QStringLiteral("console"), live->consoleMessages());
            if (request.screenshot) {
                QImage shot = guard->grab().toImage();
                if (shot.width() > kMaxShotWidth)
                    shot = shot.scaledToWidth(kMaxShotWidth, Qt::SmoothTransformation);
                QByteArray png;
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                if (!shot.isNull() && shot.save(&buffer, "PNG"))
                    answer.insert(QStringLiteral("screenshot_png"),
                                  QString::fromLatin1(png.toBase64()));
            }
            auto state = m_windows.find(windowId);
            if (state != m_windows.end()) {
                state->lastReadMs = QDateTime::currentMSecsSinceEpoch();
                if (QWidget *widget = state->widget.data())
                    if (auto *read = widget->findChild<QLabel *>(
                            QStringLiteral("agentWindowRead")))
                        read->setText(QStringLiteral("The agent read this page at %1")
                                          .arg(QLocale().toString(QTime::currentTime(),
                                                                  QLocale::ShortFormat)));
            }
            finish(answer);
        });
}

void Registry::setKind(const QString &windowId, const QString &kind)
{
    auto it = m_windows.find(windowId);
    if (it != m_windows.end())
        it->kind = kind;
}

void Registry::setExtra(const QString &windowId, const QJsonObject &extra)
{
    auto it = m_windows.find(windowId);
    if (it != m_windows.end())
        it->extra = extra;
}

void Registry::addWidgetEvent(const QString &windowId, const QJsonValue &event)
{
    auto it = m_windows.find(windowId);
    if (it == m_windows.end() || it->kind != WidgetKind || !it->open)
        return;
    it->events.append(event);
    while (it->events.size() > kMaxWidgetEvents) {
        it->events.removeFirst();
        ++it->droppedEvents;
    }
}

void Registry::noteAgentUse(const QString &windowId, const QString &what)
{
    auto it = m_windows.find(windowId);
    if (it == m_windows.end())
        return;
    it->lastReadMs = QDateTime::currentMSecsSinceEpoch();
    if (QWidget *widget = it->widget.data())
        if (auto *label = widget->findChild<QLabel *>(QStringLiteral("agentWindowRead")))
            label->setText(QStringLiteral("The agent %1 at %2")
                               .arg(what, QLocale().toString(QTime::currentTime(),
                                                             QLocale::ShortFormat)));
}

QJsonObject Registry::widget(int sessionId, const QString &agentLabel,
                             const WidgetRequest &request)
{
    if (!embeddedBrowserAvailable())
        return agentintake::errorReply(
            QStringLiteral("no_widget_engine"),
            QStringLiteral("This desktop was built without Qt WebEngine, so it cannot run "
                           "widgets; use show_window for static content instead."));
    if (request.html.trimmed().isEmpty()) {
        auto it = m_windows.find(request.windowId);
        if (it == m_windows.end() || it->sessionId != sessionId || it->kind != WidgetKind)
            return agentintake::errorReply(
                QStringLiteral("not_found"),
                QStringLiteral("No widget %1 was opened by this agent.").arg(request.windowId));
        if (!it->open || !it->widget)
            return agentintake::errorReply(
                QStringLiteral("closed"),
                QStringLiteral("The user closed widget %1.").arg(request.windowId));
        it->widgetData = request.data;
        if (web::View *view = web::viewOf(
                it->widget->findChild<QWidget *>(QStringLiteral("agentWindowWeb"))))
            view->runJavaScript(widgetDataScript(request.data), web::World::Main, {});
        if (!request.title.isEmpty()) {
            it->request.title = request.title;
            it->widget->setWindowTitle(windowTitle(it->agentLabel, request.title));
        }
        return stateReply(request.windowId, *it);
    }
    QJsonObject error;
    const QString id = claimWindow(sessionId, request.windowId, &error);
    if (id.isEmpty())
        return error;
    WindowState &state = m_windows[id];
    const bool reuse = !state.widget.isNull();
    state.agentLabel = agentLabel;
    state.request = ShowRequest();
    state.request.windowId = id;
    state.request.title = request.title.isEmpty() ? QStringLiteral("Widget") : request.title;
    state.request.width = request.width > 0 ? request.width : kDefaultWidgetWidth;
    state.request.height = request.height > 0 ? request.height : kDefaultWidgetHeight;
    state.browser = false;
    state.kind = WidgetKind;
    state.extra = QJsonObject();
    state.widgetHtml = request.html;
    state.widgetData = request.hasData ? request.data : QJsonValue(QJsonValue::Null);
    state.events.clear();
    state.droppedEvents = 0;
    state.url.clear();
    state.pageTitle.clear();
    state.open = true;
    state.answered = false;
    state.button.clear();
    state.input.clear();
    QWidget *widget = buildWidgetWindow(id, state);
    state.widget = widget;
    if (!reuse) {
        place(widget, state);
        widget->show();
    } else if (request.width > 0 || request.height > 0) {
        widget->resize(request.width > 0 ? request.width : widget->width(),
                       request.height > 0 ? request.height : widget->height());
    }
    return stateReply(id, state);
}

void Registry::act(int sessionId, const QJsonObject &params,
                   std::function<void(const QJsonObject &)> reply)
{
    ActRequest request;
    QString code;
    if (!parseActRequest(params, &request, &code)) {
        reply(agentintake::errorReply(
            code, code == QLatin1String("bad_url")
                      ? QStringLiteral("url must be an http or https address with a host "
                                       "and no user name or password.")
                  : code == QLatin1String("no_element")
                      ? QStringLiteral("%1 needs selector or target.").arg(request.action)
                      : QStringLiteral("The browser action is malformed (%1).").arg(code)));
        return;
    }
    auto it = m_windows.find(request.windowId);
    if (it == m_windows.end() || it->sessionId != sessionId) {
        reply(agentintake::errorReply(
            QStringLiteral("not_found"),
            QStringLiteral("No window %1 was opened by this agent.").arg(request.windowId)));
        return;
    }
    if (!it->browser) {
        reply(agentintake::errorReply(
            QStringLiteral("not_browser"),
            QStringLiteral("Window %1 is not a browser window.").arg(request.windowId)));
        return;
    }
    if (!it->open || !it->widget) {
        reply(agentintake::errorReply(
            QStringLiteral("closed"),
            QStringLiteral("The user closed browser window %1.").arg(request.windowId)));
        return;
    }
    if (!it->shared) {
        reply(agentintake::errorReply(
            QStringLiteral("not_shared"),
            QStringLiteral("The user turned off sharing this page with the agent.")));
        return;
    }
    if (!embeddedBrowserAvailable()) {
        reply(agentintake::errorReply(QStringLiteral("no_browser"),
                                      QStringLiteral("This desktop has no built-in browser.")));
        return;
    }
    QWidget *viewWidget = it->widget->findChild<QWidget *>(QStringLiteral("agentWindowWeb"));
    web::View *view = web::viewOf(viewWidget);
    if (!view) {
        reply(agentintake::errorReply(QStringLiteral("closed"),
                                      QStringLiteral("The browser window has no page.")));
        return;
    }
    QPointer<QWidget> guard(viewWidget);
    const QString windowId = request.windowId;
    auto done = std::make_shared<bool>(false);
    auto waiter = std::make_shared<QPointer<QObject>>(new QObject(m_context.get()));
    auto finish = [this, done, reply, guard, windowId, request, waiter](QJsonObject answer) {
        if (*done)
            return;
        *done = true;
        if (QObject *object = waiter->data())
            object->deleteLater();
        if (answer.value(QStringLiteral("ok")).toBool()) {
            answer.insert(QStringLiteral("window_id"), windowId);
            answer.insert(QStringLiteral("kind"), QStringLiteral("action"));
            answer.insert(QStringLiteral("action"), request.action);
            answer.insert(QStringLiteral("open"), !guard.isNull());
            if (web::View *live = web::viewOf(guard.data())) {
                answer.insert(QStringLiteral("url"), live->url().toString());
                answer.insert(QStringLiteral("title"), live->title());
            }
            noteAgentUse(windowId, actionSummary(request));
        }
        reply(answer);
    };
    const int wait = request.waitMs > 0 ? request.waitMs : kMaxActionWaitMs;
    QTimer::singleShot(wait + 500, waiter->data(), [finish, request] {
        if (request.action == QLatin1String("wait_for"))
            finish(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("found"), false}});
        else
            finish(agentintake::errorReply(QStringLiteral("action_timeout"),
                                           QStringLiteral("The page did not answer in time.")));
    });
    const bool navigation = request.action == QLatin1String("navigate") ||
                            request.action == QLatin1String("back") ||
                            request.action == QLatin1String("forward") ||
                            request.action == QLatin1String("reload");
    if (navigation) {
        if ((request.action == QLatin1String("back") && !view->canGoBack()) ||
            (request.action == QLatin1String("forward") && !view->canGoForward())) {
            finish(agentintake::errorReply(QStringLiteral("no_history"),
                                           QStringLiteral("There is no page to go %1 to.")
                                               .arg(request.action)));
            return;
        }
        view->onLoadFinished(waiter->data(), [finish](bool ok) {
            finish(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("loaded"), ok}});
        });
        QTimer::singleShot(wait, waiter->data(), [finish] {
            finish(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("loaded"), false}});
        });
        if (request.action == QLatin1String("navigate"))
            view->load(request.url);
        else if (request.action == QLatin1String("back"))
            view->back();
        else if (request.action == QLatin1String("forward"))
            view->forward();
        else
            view->reload();
        return;
    }
    const QString script = pageActionScript(request);
    if (request.action == QLatin1String("wait_for")) {
        auto poll = std::make_shared<std::function<void()>>();
        QElapsedTimer started;
        started.start();
        std::weak_ptr<std::function<void()>> self = poll;
        *poll = [guard, script, finish, done, self, started, wait, waiter] {
            const auto poll = self.lock();
            if (*done || !poll)
                return;
            web::View *live = web::viewOf(guard.data());
            if (!live) {
                finish(agentintake::errorReply(QStringLiteral("closed"),
                                               QStringLiteral("The user closed the window.")));
                return;
            }
            live->runJavaScript(
                script, web::World::Application,
                [finish, done, poll, started, wait, waiter](const QVariant &value) {
                    if (*done)
                        return;
                    const QJsonObject result =
                        QJsonDocument::fromJson(value.toString().toUtf8()).object();
                    if (!result.value(QStringLiteral("ok")).toBool() ||
                        result.value(QStringLiteral("found")).toBool() ||
                        started.elapsed() >= wait) {
                        finish(result.isEmpty()
                                   ? agentintake::errorReply(
                                         QStringLiteral("action_failed"),
                                         QStringLiteral("The page gave no answer."))
                                   : result);
                        return;
                    }
                    QTimer::singleShot(kActionPollMs, waiter->data(), [poll] { (*poll)(); });
                });
        };
        (*poll)();
        return;
    }
    view->runJavaScript(
        script, web::World::Application,
        [finish, waiter](const QVariant &value) {
            QJsonObject result = QJsonDocument::fromJson(value.toString().toUtf8()).object();
            if (result.isEmpty())
                result = agentintake::errorReply(QStringLiteral("action_failed"),
                                                 QStringLiteral("The page gave no answer."));
            else if (!result.value(QStringLiteral("ok")).toBool())
                result = agentintake::errorReply(
                    result.value(QStringLiteral("error")).toString(),
                    result.value(QStringLiteral("error")).toString() ==
                            QLatin1String("password_field")
                        ? QStringLiteral("Agents do not type into password fields; ask the "
                                         "user to sign in themselves.")
                    : result.value(QStringLiteral("error")).toString() ==
                            QLatin1String("not_found")
                        ? QStringLiteral("No element on the page matches.")
                        : QStringLiteral("The action failed on the page (%1).")
                              .arg(result.value(QStringLiteral("error")).toString()));
            if (!result.value(QStringLiteral("ok")).toBool()) {
                finish(result);
                return;
            }
            QTimer::singleShot(kActionSettleMs, waiter->data(),
                               [finish, result] { finish(result); });
        });
}

void Registry::setPageState(const QString &windowId, const QUrl &url, const QString &title)
{
    auto it = m_windows.find(windowId);
    if (it == m_windows.end() || !it->browser)
        return;
    if (url.isValid())
        it->url = url;
    if (!title.isNull())
        it->pageTitle = title.left(kMaxTitleChars * 2);
    if (QWidget *widget = it->widget.data()) {
        const QString shown = it->pageTitle.isEmpty() ? it->request.title : it->pageTitle;
        widget->setWindowTitle(windowTitle(it->agentLabel, shown));
    }
}

QJsonObject Registry::read(int sessionId, const QString &windowId)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end() || it->sessionId != sessionId)
        return agentintake::errorReply(
            QStringLiteral("not_found"),
            QStringLiteral("No window %1 was opened by this agent.").arg(windowId));
    const QJsonObject reply = stateReply(windowId, *it);
    it->events.clear();
    it->droppedEvents = 0;
    return reply;
}

QJsonObject Registry::close(int sessionId, const QString &windowId)
{
    auto it = m_windows.find(windowId);
    if (it == m_windows.end() || it->sessionId != sessionId)
        return agentintake::errorReply(
            QStringLiteral("not_found"),
            QStringLiteral("No window %1 was opened by this agent.").arg(windowId));
    if (QWidget *widget = it->widget.data())
        widget->close();
    markClosed(windowId);
    return stateReply(windowId, m_windows.value(windowId));
}

bool Registry::raise(const QString &windowId)
{
    const auto it = m_windows.constFind(windowId);
    if (it == m_windows.constEnd() || !it->widget)
        return false;
    it->widget->show();
    it->widget->raise();
    it->widget->activateWindow();
    return true;
}

void Registry::answer(const QString &windowId, const QString &button,
                      const QString &input)
{
    auto it = m_windows.find(windowId);
    if (it == m_windows.end() || !it->open)
        return;
    it->answered = true;
    it->button = button;
    it->input = input.left(kMaxInputChars);
    if (m_answerHandler) {
        QPointer<QObject> context = m_context.get();
        QTimer::singleShot(0, m_context.get(), [this, context, windowId] {
            if (context && m_answerHandler)
                m_answerHandler(windowId);
        });
    }
}

void Registry::markClosed(const QString &windowId)
{
    auto it = m_windows.find(windowId);
    if (it == m_windows.end())
        return;
    it->open = false;
    it->widget = nullptr;
}

int Registry::openCount(int sessionId) const
{
    int count = 0;
    for (const WindowState &state : m_windows)
        if (state.sessionId == sessionId && state.open)
            ++count;
    return count;
}

const WindowState *Registry::state(const QString &windowId) const
{
    const auto it = m_windows.constFind(windowId);
    return it == m_windows.constEnd() ? nullptr : &it.value();
}

QJsonObject Registry::stateReply(const QString &windowId, const WindowState &state) const
{
    QJsonObject reply{
        {QStringLiteral("ok"), true},
        {QStringLiteral("window_id"), windowId},
        {QStringLiteral("title"), state.request.title},
        {QStringLiteral("open"), state.open},
        {QStringLiteral("answered"), state.answered},
        {QStringLiteral("has_input"), !state.request.inputLabel.isEmpty()},
        {QStringLiteral("buttons"), QJsonArray::fromStringList(state.request.buttons)},
    };
    if (state.browser) {
        reply.insert(QStringLiteral("kind"), QStringLiteral("browser"));
        reply.insert(QStringLiteral("embedded"), true);
        reply.insert(QStringLiteral("url"), state.url.toString());
        reply.insert(QStringLiteral("page_title"), state.pageTitle);
    }
    if (state.answered) {
        reply.insert(QStringLiteral("button"), state.button);
        if (!state.request.inputLabel.isEmpty())
            reply.insert(QStringLiteral("input"), state.input);
    }
    if (state.kind == WidgetKind) {
        reply.insert(QStringLiteral("kind"), WidgetKind);
        QJsonArray events;
        for (const QJsonValue &event : state.events)
            events.append(event);
        reply.insert(QStringLiteral("events"), events);
        reply.insert(QStringLiteral("answered"), !state.events.isEmpty());
        if (state.droppedEvents > 0)
            reply.insert(QStringLiteral("dropped_events"), state.droppedEvents);
    } else if (!state.kind.isEmpty()) {
        reply.insert(QStringLiteral("kind"), state.kind);
    }
    for (auto it = state.extra.constBegin(); it != state.extra.constEnd(); ++it)
        reply.insert(it.key(), it.value());
    return reply;
}

QWidget *Registry::frameFor(const QString &windowId, const WindowState &state)
{
    QWidget *window = state.widget.data();
    if (!window) {
        QPointer<QObject> context = m_context.get();
        window = new Frame(m_host, [this, context, windowId] {
            if (context)
                markClosed(windowId);
        });
        window->setObjectName(QStringLiteral("agentWindow"));
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->setAttribute(Qt::WA_ShowWithoutActivating);
        auto *outer = new QVBoxLayout(window);
        outer->setContentsMargins(0, 0, 0, 0);
        QObject::connect(window, &QObject::destroyed, m_context.get(),
                         [this, windowId] { markClosed(windowId); });
        auto *escape = new QShortcut(QKeySequence::Cancel, window);
        escape->setObjectName(QStringLiteral("agentWindowEscape"));
        QObject::connect(escape, &QShortcut::activated, window, &QWidget::close);
    }
    if (QWidget *old = window->findChild<QWidget *>(QStringLiteral("agentWindowPage"),
                                                    Qt::FindDirectChildrenOnly)) {
        old->hide();
        old->deleteLater();
        old->setObjectName(QString());
    }
    if (auto *escape = window->findChild<QShortcut *>(QStringLiteral("agentWindowEscape")))
        escape->setEnabled(!state.browser);
    window->setWindowTitle(windowTitle(state.agentLabel, state.request.title));
    return window;
}

QObject *Registry::browserProfile()
{
    if (!m_browserProfile)
        m_browserProfile = web::createProfile(web::ProfileOptions());
    return m_browserProfile;
}

QWidget *Registry::buildBrowser(const QString &windowId, const WindowState &state)
{
    QWidget *window = frameFor(windowId, state);
    web::PageOptions options;
    options.acceptNavigation = [](const QUrl &url, bool, bool isMainFrame) {
        if (isMainFrame)
            return browserUrlAllowed(url) || url.toString() == QLatin1String("about:blank");
        return url.scheme().toLower() != QLatin1String("file");
    };
    options.newWindowsInPlace = true;
    options.consoleLimit = kMaxConsoleMessages;
    auto *page = new QWidget(window);
    web::View *view = web::createView(browserProfile(), options, page);
    if (!view) {
        delete page;
        return window;
    }
    page->setObjectName(QStringLiteral("agentWindowPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(4);
    const auto tool = [page](const QString &text, const QString &tip) {
        auto *button = new QToolButton(page);
        button->setText(text);
        button->setToolTip(tip);
        button->setAutoRaise(true);
        return button;
    };
    QToolButton *back = tool(QStringLiteral("‹"), QStringLiteral("Back"));
    QToolButton *forward = tool(QStringLiteral("›"), QStringLiteral("Forward"));
    QToolButton *reload = tool(QStringLiteral("⟳"), QStringLiteral("Reload"));
    auto *address = new QLineEdit(page);
    address->setObjectName(QStringLiteral("agentWindowAddress"));
    address->setMaxLength(kMaxUrlChars);
    address->setText(state.url.toString());
    auto *external = new QPushButton(QStringLiteral("Open in browser"), page);
    external->setAutoDefault(false);
    bar->addWidget(back);
    bar->addWidget(forward);
    bar->addWidget(reload);
    bar->addWidget(address, 1);
    bar->addWidget(external);
    layout->addLayout(bar);

    QWidget *viewWidget = view->widget();
    viewWidget->setObjectName(QStringLiteral("agentWindowWeb"));
    layout->addWidget(viewWidget, 1);

    auto *from = new QLabel(state.agentLabel.isEmpty()
                                ? QString()
                                : QStringLiteral("Opened by %1").arg(state.agentLabel),
                            page);
    from->setObjectName(QStringLiteral("agentWindowFrom"));
    from->setEnabled(false);
    from->setVisible(!state.agentLabel.isEmpty());
    auto *share = new QCheckBox(QStringLiteral("Share this page with the agent"), page);
    share->setObjectName(QStringLiteral("agentWindowShare"));
    share->setChecked(state.shared);
    share->setToolTip(QStringLiteral("When on, the agent can read this page's text, links "
                                     "and console messages, and take a screenshot. A "
                                     "screenshot shows whatever is on screen, including text "
                                     "typed into fields; password fields stay masked."));
    auto *read = new QLabel(page);
    read->setObjectName(QStringLiteral("agentWindowRead"));
    read->setEnabled(false);
    auto *footer = new QHBoxLayout;
    footer->addWidget(from);
    footer->addStretch(1);
    footer->addWidget(read);
    footer->addWidget(share);
    layout->addLayout(footer);
    QObject::connect(share, &QCheckBox::toggled, m_context.get(),
                     [this, windowId](bool on) { setShared(windowId, on); });

    QObject::connect(back, &QToolButton::clicked, viewWidget, [view] { view->back(); });
    QObject::connect(forward, &QToolButton::clicked, viewWidget, [view] { view->forward(); });
    QObject::connect(reload, &QToolButton::clicked, viewWidget, [view] { view->reload(); });
    QObject::connect(address, &QLineEdit::returnPressed, viewWidget, [view, address] {
        const QUrl url = browserUrlFromInput(address->text());
        if (url.isValid())
            view->load(url);
    });
    QObject::connect(external, &QPushButton::clicked, viewWidget, [this, view] {
        if (m_externalOpener && browserUrlAllowed(view->url()))
            m_externalOpener(view->url());
    });
    view->onUrlChanged(m_context.get(), [this, windowId, address](const QUrl &url) {
        address->setText(url.toString());
        setPageState(windowId, url, QString());
    });
    view->onTitleChanged(m_context.get(), [this, windowId](const QString &title) {
        setPageState(windowId, QUrl(), title);
    });
    view->load(state.url);
    window->layout()->addWidget(page);
    return window;
}

QObject *Registry::widgetProfile()
{
    if (!m_widgetProfile) {
        web::ProfileOptions options;
        options.requestAllowed = [](const QUrl &url) { return widgetRequestAllowed(url); };
        m_widgetProfile = web::createProfile(options);
    }
    return m_widgetProfile;
}

QWidget *Registry::buildWidgetWindow(const QString &windowId, const WindowState &state)
{
    QWidget *window = frameFor(windowId, state);
    QPointer<QObject> context = m_context.get();
    web::PageOptions options;
    options.onConsole = [this, context, windowId](const QString &message) {
        QJsonValue event;
        if (context && widgetEventFromConsole(message, &event))
            addWidgetEvent(windowId, event);
    };
    options.acceptNavigation = [this, context](const QUrl &url, bool linkClicked,
                                               bool isMainFrame) {
        if (widgetRequestAllowed(url) && (!linkClicked || url.hasFragment()))
            return true;
        const bool mailto = url.scheme().toLower() == QLatin1String("mailto");
        if (isMainFrame && linkClicked && context && (browserUrlAllowed(url) || mailto)) {
            if (mailto)
                QDesktopServices::openUrl(url);
            else if (m_externalOpener)
                m_externalOpener(url);
        }
        return false;
    };
    options.restricted = true;
    options.documentScriptName = QStringLiteral("forkmesh-widget-bridge");
    options.documentScript = widgetBridgeScript(state.widgetData);
    auto *page = new QWidget(window);
    web::View *view = web::createView(widgetProfile(), options, page);
    if (!view) {
        delete page;
        return window;
    }
    page->setObjectName(QStringLiteral("agentWindowPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 6);
    layout->setSpacing(4);

    QWidget *viewWidget = view->widget();
    viewWidget->setObjectName(QStringLiteral("agentWindowWeb"));
    layout->addWidget(viewWidget, 1);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(8, 0, 8, 0);
    auto *from = new QLabel(state.agentLabel.isEmpty()
                                ? QStringLiteral("Widget made by an agent")
                                : QStringLiteral("Widget from %1").arg(state.agentLabel),
                            page);
    from->setObjectName(QStringLiteral("agentWindowFrom"));
    from->setEnabled(false);
    footer->addWidget(from, 1);
    layout->addLayout(footer);

    view->setHtml(state.widgetHtml, QUrl(QStringLiteral("about:blank")));
    window->layout()->addWidget(page);
    return window;
}

QWidget *Registry::buildWidget(const QString &windowId, const WindowState &state)
{
    QWidget *window = frameFor(windowId, state);

    auto *page = new QWidget(window);
    page->setObjectName(QStringLiteral("agentWindowPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 10);
    layout->setSpacing(8);

    auto *browser = new Browser(state.workdir, page);
    browser->setObjectName(QStringLiteral("agentWindowBody"));
    browser->setOpenLinks(false);
    browser->setOpenExternalLinks(false);
    if (!state.workdir.isEmpty())
        browser->setSearchPaths({state.workdir});
    if (state.request.format == QLatin1String("html"))
        browser->setHtml(state.request.body);
    else if (state.request.format == QLatin1String("text"))
        browser->setPlainText(state.request.body);
    else
        browser->setMarkdown(state.request.body);
    layout->addWidget(browser, 1);

    QLineEdit *line = nullptr;
    QPlainTextEdit *multi = nullptr;
    if (!state.request.inputLabel.isEmpty()) {
        auto *label = new QLabel(state.request.inputLabel, page);
        label->setWordWrap(true);
        layout->addWidget(label);
        if (state.request.inputMultiline) {
            multi = new QPlainTextEdit(page);
            multi->setObjectName(QStringLiteral("agentWindowInput"));
            multi->setMaximumHeight(120);
            layout->addWidget(multi);
        } else {
            line = new QLineEdit(page);
            line->setObjectName(QStringLiteral("agentWindowInput"));
            line->setMaxLength(kMaxInputChars);
            layout->addWidget(line);
        }
    }

    auto *status = new QLabel(page);
    status->setObjectName(QStringLiteral("agentWindowStatus"));
    status->setWordWrap(true);
    status->hide();

    auto *row = new QHBoxLayout;
    row->setSpacing(6);
    auto *from = new QLabel(state.agentLabel.isEmpty()
                                ? QString()
                                : QStringLiteral("From %1").arg(state.agentLabel),
                            page);
    from->setObjectName(QStringLiteral("agentWindowFrom"));
    from->setEnabled(false);
    row->addWidget(from, 1);
    row->addWidget(status, 1);

    QStringList labels = state.request.buttons;
    const bool asks = !labels.isEmpty() || line || multi;
    if (labels.isEmpty() && (line || multi))
        labels << QStringLiteral("Send");
    QList<QPushButton *> buttons;
    const auto inputText = [line, multi] {
        return line ? line->text() : multi ? multi->toPlainText() : QString();
    };
    const auto submit = [this, windowId, inputText, status, from, page](const QString &label) {
        answer(windowId, label, inputText());
        for (QPushButton *button : page->findChildren<QPushButton *>())
            if (button->property("agentWindowAnswer").toBool())
                button->setEnabled(false);
        for (QWidget *input : page->findChildren<QWidget *>(QStringLiteral("agentWindowInput")))
            input->setEnabled(false);
        from->hide();
        status->setText(QStringLiteral("Sent “%1” to the agent.").arg(label));
        status->show();
    };
    for (const QString &label : std::as_const(labels)) {
        auto *button = new QPushButton(label, page);
        button->setProperty("agentWindowAnswer", true);
        button->setAutoDefault(false);
        QObject::connect(button, &QPushButton::clicked, page,
                         [submit, label] { submit(label); });
        buttons << button;
        row->addWidget(button);
    }
    if (!asks) {
        auto *closeButton = new QPushButton(QStringLiteral("Close"), page);
        closeButton->setAutoDefault(false);
        QObject::connect(closeButton, &QPushButton::clicked, window, &QWidget::close);
        row->addWidget(closeButton);
    }
    if (line && !buttons.isEmpty()) {
        QPushButton *first = buttons.first();
        QObject::connect(line, &QLineEdit::returnPressed, first, &QPushButton::click);
    }
    QObject::connect(browser, &QTextBrowser::anchorClicked, page,
                     [submit, labels](const QUrl &url) {
                         const QString action = actionFromLink(url);
                         if (!action.isEmpty()) {
                             submit(action);
                             return;
                         }
                         const QString scheme = url.scheme().toLower();
                         if (scheme == QLatin1String("https") ||
                             scheme == QLatin1String("http") ||
                             scheme == QLatin1String("mailto"))
                             QDesktopServices::openUrl(url);
                     });
    layout->addLayout(row);

    window->layout()->addWidget(page);
    return window;
}

void Registry::place(QWidget *widget, const WindowState &state)
{
    const int width = state.request.width > 0 ? state.request.width
                      : state.browser       ? kDefaultBrowserWidth
                                            : kDefaultWidth;
    const int height = state.request.height > 0 ? state.request.height
                       : state.browser        ? kDefaultBrowserHeight
                                              : kDefaultHeight;
    widget->resize(width, height);
    QRect anchor;
    QScreen *screen = nullptr;
    if (m_host && m_host->isVisible()) {
        anchor = m_host->frameGeometry();
        screen = m_host->screen();
    }
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect();
    if (anchor.isEmpty())
        anchor = available;
    if (anchor.isEmpty())
        return;
    const int step = 28 * (m_cascade++ % 8);
    QPoint topLeft(anchor.right() - width - 24 - step, anchor.top() + 72 + step);
    if (!available.isEmpty()) {
        topLeft.setX(qBound(available.left(), topLeft.x(),
                            qMax(available.left(), available.right() - width)));
        topLeft.setY(qBound(available.top(), topLeft.y(),
                            qMax(available.top(), available.bottom() - height)));
    }
    widget->move(topLeft);
}

void Registry::prune(int sessionId)
{
    QList<QPair<qint64, QString>> closed;
    int total = 0;
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        if (it->sessionId != sessionId)
            continue;
        ++total;
        if (!it->open && !it->widget)
            closed << qMakePair(it->createdMs, it.key());
    }
    std::sort(closed.begin(), closed.end());
    for (const auto &entry : std::as_const(closed)) {
        if (total <= kMaxRecordsPerSession)
            break;
        m_windows.remove(entry.second);
        --total;
    }
}

}
