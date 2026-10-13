#include "TerminalWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSocketNotifier>
#include <QStringList>
#include <QWheelEvent>

#include <cstdlib>
#include <utility>
#ifndef Q_OS_WIN
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

extern char **environ;
#endif

namespace {
const QColor kPalette[16] = {
    QColor(0x1c, 0x1c, 0x1c), QColor(0xe0, 0x6c, 0x75), // black,  red
    QColor(0x98, 0xc3, 0x79), QColor(0xe5, 0xc0, 0x7b), // green,  yellow
    QColor(0x61, 0xaf, 0xef), QColor(0xc6, 0x78, 0xdd), // blue,   magenta
    QColor(0x56, 0xb6, 0xc2), QColor(0xd6, 0xd6, 0xd6), // cyan,   white
    QColor(0x5c, 0x63, 0x70), QColor(0xff, 0x8a, 0x90), // bright black/red
    QColor(0xb5, 0xe8, 0x90), QColor(0xff, 0xe3, 0x95), // bright green/yellow
    QColor(0x82, 0xc8, 0xff), QColor(0xe2, 0x96, 0xfb), // bright blue/magenta
    QColor(0x79, 0xe2, 0xea), QColor(0xff, 0xff, 0xff), // bright cyan/white
};
const QColor kDefaultFg(0xd6, 0xd6, 0xd6);
const QColor kDefaultBg(0x0b, 0x0b, 0x0b);

QColor xtermColor(int n)
{
    if (n < 16)
        return kPalette[n];
    if (n < 232) {
        n -= 16;
        const int r = (n / 36) % 6, g = (n / 6) % 6, b = n % 6;
        auto comp = [](int v) { return v ? v * 40 + 55 : 0; };
        return QColor(comp(r), comp(g), comp(b));
    }
    const int v = (n - 232) * 10 + 8;
    return QColor(v, v, v);
}
} // namespace

TerminalWidget::TerminalWidget(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setCursor(Qt::IBeamCursor);
    setMinimumHeight(180);

    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setPointSize(10);
    QFontMetrics fm(m_font);
    m_cellW = qMax(1, fm.horizontalAdvance(QLatin1Char('W')));
    m_cellH = qMax(1, fm.height());

    m_curFg = kDefaultFg;
    m_curBg = kDefaultBg;
    resizeGrid(m_rows, m_cols);
    setContextMenuPolicy(Qt::DefaultContextMenu);
}

TerminalWidget::~TerminalWidget() { stop(); }

bool TerminalWidget::isRunning() const
{
#ifdef Q_OS_WIN
    return m_process && m_process->state() != QProcess::NotRunning;
#else
    return m_childPid > 0;
#endif
}

TerminalWidget::Cell TerminalWidget::blankCell() const
{
    Cell c;
    c.fg = kDefaultFg;
    c.bg = kDefaultBg;
    return c;
}

void TerminalWidget::clearRow(Row &row) const
{
    const Cell blank = blankCell();
    for (Cell &c : row)
        c = blank;
}

void TerminalWidget::resizeGrid(int rows, int cols)
{
    rows = qMax(1, rows);
    cols = qMax(1, cols);
    auto resizeBuffer = [&](QVector<Row> &buf) {
        for (Row &r : buf) {
            const int old = r.size();
            r.resize(cols);
            for (int i = old; i < cols; ++i)
                r[i] = blankCell();
        }
        while (buf.size() < rows) {
            Row r(cols);
            clearRow(r);
            buf.append(r);
        }
        while (buf.size() > rows)
            buf.removeFirst();
    };
    resizeBuffer(m_screen);
    resizeBuffer(m_alt);
    m_rows = rows;
    m_cols = cols;
    m_scrollTop = 0;
    m_scrollBottom = rows - 1;
    m_cx = qBound(0, m_cx, cols - 1);
    m_cy = qBound(0, m_cy, rows - 1);
    m_wrapPending = false;
}


void TerminalWidget::runCommand(const QString &commandLine, const QString &cwd,
                                const QStringList &extraEnv,
                                bool keepHostShell)
{
    stop();

    m_altActive = false;
    m_scrollback.clear();
    m_viewOffset = 0;
    m_cx = m_cy = 0;
    m_curFg = kDefaultFg;
    m_curBg = kDefaultBg;
    m_curBold = m_curInverse = m_curUnderline = false;
    m_appCursorKeys = m_bracketedPaste = false;
    m_autoWrap = true;
    m_cursorVisible = true;
    clearSelection();
    m_urlScanBuffer.clear();
    m_signInUrlEmitted = false;
    recomputeGrid();
    for (Row &r : m_screen)
        clearRow(r);

#ifdef Q_OS_WIN
    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    if (!cwd.isEmpty())
        process->setWorkingDirectory(cwd);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    environment.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    for (const QString &kv : extraEnv) {
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq > 0)
            environment.insert(kv.left(eq), kv.mid(eq + 1));
        else if (eq < 0 && !kv.isEmpty())
            environment.remove(kv);
    }
    process->setProcessEnvironment(environment);
    process->setProgram(qEnvironmentVariable("COMSPEC", QStringLiteral("cmd.exe")));
    process->setArguments({QStringLiteral("/d"), QStringLiteral("/q"),
                           keepHostShell ? QStringLiteral("/k")
                                         : QStringLiteral("/c"),
                           commandLine});
    connect(process, &QProcess::started, this, &TerminalWidget::started);
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process] {
                const QByteArray chunk = process->readAllStandardOutput();
                parseBytes(chunk);
                scanForSignInUrl(chunk);
                m_viewOffset = 0;
                update();
            });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this, process](int exitCode) {
                if (m_process != process)
                    return;
                const QByteArray footer =
                    QByteArrayLiteral("\r\n[ForkMesh: process exited (") +
                    QByteArray::number(exitCode) + QByteArrayLiteral(")]\r\n");
                parseBytes(footer);
                m_process = nullptr;
                process->deleteLater();
                emit finished(exitCode);
                update();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (m_process != process || error != QProcess::FailedToStart)
                    return;
                const QByteArray message =
                    QByteArrayLiteral("[ForkMesh: process failed to start: ") +
                    process->errorString().toUtf8() + QByteArrayLiteral("]\r\n");
                parseBytes(message);
                m_process = nullptr;
                process->deleteLater();
                emit finished(-1);
                update();
            });
    process->start();
    update();
    return;
#else
    const QByteArray cwdBytes = cwd.toLocal8Bit();
    QByteArray wrapped = commandLine.toUtf8() +
                         "; ec=$?; echo; echo \"[ForkMesh: process exited ($ec)]\"";
    if (keepHostShell) {
        wrapped += "; exec bash -i";
    } else {
        wrapped += "; exit $ec";
    }
    wrapped.append('\0');
    QByteArray a0 = QByteArrayLiteral("bash");
    QByteArray a1 = QByteArrayLiteral("-lc");
    char *argv[] = {a0.data(), a1.data(), wrapped.data(), nullptr};

    QList<QByteArray> envStore;
    auto setVar = [&](const QByteArray &key, const QByteArray &val) {
        const QByteArray prefix = key + '=';
        for (QByteArray &e : envStore)
            if (e.startsWith(prefix)) {
                e = prefix + val;
                return;
            }
        envStore.append(prefix + val);
    };
    auto unsetVar = [&](const QByteArray &key) {
        const QByteArray prefix = key + '=';
        envStore.removeIf(
            [&](const QByteArray &e) { return e.startsWith(prefix); });
    };
    for (char **e = environ; e && *e; ++e)
        envStore.append(QByteArray(*e));
    setVar(QByteArrayLiteral("TERM"), QByteArrayLiteral("xterm-256color"));
    setVar(QByteArrayLiteral("COLORTERM"), QByteArrayLiteral("truecolor"));
    for (const QString &kv : extraEnv) {
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq > 0)
            setVar(kv.left(eq).toLocal8Bit(), kv.mid(eq + 1).toLocal8Bit());
        else if (eq < 0 && !kv.isEmpty())
            unsetVar(kv.toLocal8Bit());
    }
    QVector<char *> envp;
    envp.reserve(envStore.size() + 1);
    for (QByteArray &e : envStore)
        envp.append(e.data());
    envp.append(nullptr);

    struct winsize ws = {};
    ws.ws_row = (unsigned short)m_rows;
    ws.ws_col = (unsigned short)m_cols;

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0)
        return;

    if (pid == 0) {
        if (!cwdBytes.isEmpty() && ::chdir(cwdBytes.constData()) != 0)
            ::_exit(127);
        environ = envp.data();
        ::execvp("bash", argv);
        ::_exit(127);
    }

    m_master = master;
    m_childPid = pid;
    ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL, 0) | O_NONBLOCK);
    m_notifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this,
            &TerminalWidget::onReadyRead);
    applyWinSize();
    emit started();
    update();
#endif
}

// code-health: allow(blocking-wait) teardown path; Windows waits at most 1 s after kill, POSIX reap() polls at most 200 ms
void TerminalWidget::stop()
{
#ifdef Q_OS_WIN
    if (m_process) {
        QProcess *process = m_process;
        m_process = nullptr;
        process->disconnect(this);
        process->kill();
        process->waitForFinished(1000);
        process->deleteLater();
    }
#else
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_childPid > 0) {
        ::kill((pid_t)m_childPid, SIGHUP);
        reap(false);
    }
    if (m_master >= 0) {
        ::close(m_master);
        m_master = -1;
    }
    m_childPid = -1;
#endif
}

void TerminalWidget::reap(bool emitSignal)
{
#ifdef Q_OS_WIN
    Q_UNUSED(emitSignal)
#else
    if (m_childPid <= 0)
        return;
    int status = 0;
    for (int i = 0; i < 40; ++i) {
        const pid_t r = ::waitpid((pid_t)m_childPid, &status, WNOHANG);
        if (r == (pid_t)m_childPid)
            break;
        if (r < 0)
            break;
        if (i == 10)
            ::kill((pid_t)m_childPid, SIGTERM);
        if (i == 30)
            ::kill((pid_t)m_childPid, SIGKILL);
        ::usleep(5000);
    }
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    m_childPid = -1;
    if (emitSignal)
        emit finished(code);
#endif
}

void TerminalWidget::onReadyRead()
{
#ifndef Q_OS_WIN
    if (m_master < 0)
        return;
    char buf[8192];
    bool gotEof = false;
    for (;;) {
        const ssize_t n = ::read(m_master, buf, sizeof(buf));
        if (n > 0) {
            const QByteArray chunk(buf, (int)n);
            parseBytes(chunk);
            scanForSignInUrl(chunk);
            if (n < (ssize_t)sizeof(buf))
                break;
        } else if (n == 0) {
            gotEof = true;
            break;
        } else {
            if (errno == EINTR)
                continue;
            break; // EAGAIN: nothing left for now
        }
    }
    m_viewOffset = 0;
    update();

    if (gotEof) {
        if (m_notifier) {
            m_notifier->setEnabled(false);
            m_notifier->deleteLater();
            m_notifier = nullptr;
        }
        if (m_master >= 0) {
            ::close(m_master);
            m_master = -1;
        }
        reap(true);
    }
#endif
}

void TerminalWidget::writeToPty(const QByteArray &bytes)
{
    if (m_remoteInput) {
        if (!bytes.isEmpty())
            emit inputTyped(bytes);
        return;
    }
#ifdef Q_OS_WIN
    if (m_process && !bytes.isEmpty())
        m_process->write(bytes);
#else
    if (m_master < 0 || bytes.isEmpty())
        return;
    ssize_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::write(m_master, bytes.constData() + off,
                                  bytes.size() - off);
        if (n > 0)
            off += n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
#endif
}

void TerminalWidget::scanForSignInUrl(const QByteArray &chunk)
{
    if (m_signInUrlEmitted)
        return;
    m_urlScanBuffer.append(chunk);
    constexpr int kMaxScan = 8192;
    if (m_urlScanBuffer.size() > kMaxScan)
        m_urlScanBuffer.remove(0, m_urlScanBuffer.size() - kMaxScan);

    // Current CLIs print https://claude.com/cai/oauth/authorize?..., twice:
    // an OSC 8 hyperlink target, then the visible text. A pty read can end
    // anywhere inside it, and opening that prefix is what lands on "Invalid
    // OAuth Request: Missing code_challenge parameter". So only a URL whose
    // terminator has already arrived, still carrying its PKCE challenge, opens.
    static const QRegularExpression re(
        QStringLiteral("https://claude\\.(?:com|ai)(?:/[A-Za-z0-9_-]+)?"
                       "/oauth/authorize\\?[A-Za-z0-9:/?&=%._~+-]*"
                       "|https://auth\\.openai\\.com/codex/device[A-Za-z0-9/]*"));
    const QString text = QString::fromUtf8(m_urlScanBuffer);
    QRegularExpressionMatchIterator matches = re.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch m = matches.next();
        if (m.capturedEnd(0) >= text.size())
            return; // the rest of this URL is still on its way
        const bool deviceCode = m.captured(0).startsWith(
            QStringLiteral("https://auth.openai.com/codex/device"));
        if (!deviceCode &&
            !m.captured(0).contains(QStringLiteral("code_challenge=")))
            continue;
        const QUrl url(m.captured(0));
        if (!url.isValid())
            continue;
        m_signInUrlEmitted = true;
        QDesktopServices::openUrl(url);
        emit signInUrlDetected(url);
        return;
    }
}

void TerminalWidget::pasteFromClipboard()
{
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    QByteArray out;
    const bool bracketed = m_bracketedPaste && !m_remoteInput;
    if (bracketed)
        out += "\x1b[200~";
    out += text.toUtf8();
    if (bracketed)
        out += "\x1b[201~";
    writeToPty(out);
}

void TerminalWidget::applyWinSize()
{
#ifndef Q_OS_WIN
    if (m_master < 0)
        return;
    struct winsize ws = {};
    ws.ws_row = (unsigned short)m_rows;
    ws.ws_col = (unsigned short)m_cols;
    ws.ws_xpixel = (unsigned short)(m_cols * m_cellW);
    ws.ws_ypixel = (unsigned short)(m_rows * m_cellH);
    ::ioctl(m_master, TIOCSWINSZ, &ws); // kernel raises SIGWINCH in the child
#endif
}


void TerminalWidget::recomputeGrid()
{
    const int cols = qMax(1, width() / m_cellW);
    const int rows = qMax(1, height() / m_cellH);
    if (cols != m_cols || rows != m_rows)
        resizeGrid(rows, cols);
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    recomputeGrid();
    applyWinSize();
    update();
}


void TerminalWidget::scrollUp(int top, int bottom, int n)
{
    QVector<Row> &buf = m_altActive ? m_alt : m_screen;
    top = qBound(0, top, m_rows - 1);
    bottom = qBound(0, bottom, m_rows - 1);
    if (top > bottom)
        return;
    for (int i = 0; i < n; ++i) {
        if (!m_altActive && top == 0 && bottom == m_rows - 1) {
            m_scrollback.append(buf[top]);
            if (m_scrollback.size() > 5000)
                m_scrollback.removeFirst();
        }
        for (int y = top; y < bottom; ++y)
            buf[y] = buf[y + 1];
        Row blank(m_cols);
        clearRow(blank);
        buf[bottom] = blank;
    }
}

void TerminalWidget::scrollDown(int top, int bottom, int n)
{
    QVector<Row> &buf = m_altActive ? m_alt : m_screen;
    top = qBound(0, top, m_rows - 1);
    bottom = qBound(0, bottom, m_rows - 1);
    if (top > bottom)
        return;
    for (int i = 0; i < n; ++i) {
        for (int y = bottom; y > top; --y)
            buf[y] = buf[y - 1];
        Row blank(m_cols);
        clearRow(blank);
        buf[top] = blank;
    }
}

void TerminalWidget::newLine()
{
    if (m_cy == m_scrollBottom)
        scrollUp(m_scrollTop, m_scrollBottom, 1);
    else if (m_cy < m_rows - 1)
        ++m_cy;
}

void TerminalWidget::putChar(char32_t ch)
{
    QVector<Row> &buf = m_altActive ? m_alt : m_screen;
    if (m_wrapPending && m_autoWrap) {
        m_cx = 0;
        newLine();
        m_wrapPending = false;
    }
    if (m_cx >= m_cols)
        m_cx = m_cols - 1;
    Cell &cell = buf[m_cy][m_cx];
    cell.ch = ch;
    cell.fg = m_curFg;
    cell.bg = m_curBg;
    cell.bold = m_curBold;
    cell.inverse = m_curInverse;
    cell.underline = m_curUnderline;
    if (m_cx == m_cols - 1)
        m_wrapPending = true; // defer the wrap until the next printable char
    else
        ++m_cx;
}

void TerminalWidget::eraseInDisplay(int mode)
{
    QVector<Row> &buf = m_altActive ? m_alt : m_screen;
    const Cell blank = blankCell();
    auto clearFrom = [&](int y, int x0) {
        for (int x = x0; x < m_cols; ++x)
            buf[y][x] = blank;
    };
    auto clearTo = [&](int y, int x1) {
        for (int x = 0; x <= x1 && x < m_cols; ++x)
            buf[y][x] = blank;
    };
    if (mode == 0) { // cursor to end of screen
        clearFrom(m_cy, m_cx);
        for (int y = m_cy + 1; y < m_rows; ++y)
            clearRow(buf[y]);
    } else if (mode == 1) { // start to cursor
        for (int y = 0; y < m_cy; ++y)
            clearRow(buf[y]);
        clearTo(m_cy, m_cx);
    } else { // whole screen
        for (int y = 0; y < m_rows; ++y)
            clearRow(buf[y]);
    }
}

void TerminalWidget::eraseInLine(int mode)
{
    QVector<Row> &buf = m_altActive ? m_alt : m_screen;
    const Cell blank = blankCell();
    if (mode == 0)
        for (int x = m_cx; x < m_cols; ++x)
            buf[m_cy][x] = blank;
    else if (mode == 1)
        for (int x = 0; x <= m_cx && x < m_cols; ++x)
            buf[m_cy][x] = blank;
    else
        clearRow(buf[m_cy]);
}

void TerminalWidget::useAltScreen(bool on)
{
    if (on == m_altActive)
        return;
    m_altActive = on;
    if (on) {
        for (Row &r : m_alt)
            clearRow(r);
        m_savedCx = m_cx;
        m_savedCy = m_cy;
        m_cx = m_cy = 0;
    } else {
        m_cx = m_savedCx;
        m_cy = m_savedCy;
    }
    m_wrapPending = false;
    m_viewOffset = 0;
}


void TerminalWidget::setRemoteInput(bool remote)
{
    m_remoteInput = remote;
}

void TerminalWidget::feed(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    parseBytes(bytes);
    scanForSignInUrl(bytes);
    m_viewOffset = 0;
    update();
}

void TerminalWidget::parseBytes(const QByteArray &bytes)
{
    for (unsigned char b : bytes) {
        if (m_utf8Remaining > 0 && m_state == State::Ground) {
            if ((b & 0xC0) == 0x80) {
                m_utf8 = (m_utf8 << 6) | (b & 0x3F);
                if (--m_utf8Remaining == 0)
                    putChar(m_utf8);
                continue;
            }
            m_utf8Remaining = 0; // malformed; fall through to reinterpret b
        }

        switch (m_state) {
        case State::Ground:
            if (b == 0x1B) {
                m_state = State::Esc;
            } else if (b < 0x20) {
                handleControl(b);
            } else if (b < 0x80) {
                putChar(b);
            } else if ((b & 0xE0) == 0xC0) {
                m_utf8 = b & 0x1F;
                m_utf8Remaining = 1;
            } else if ((b & 0xF0) == 0xE0) {
                m_utf8 = b & 0x0F;
                m_utf8Remaining = 2;
            } else if ((b & 0xF8) == 0xF0) {
                m_utf8 = b & 0x07;
                m_utf8Remaining = 3;
            } else {
                putChar(b); // stray byte; render as-is
            }
            break;

        case State::Esc:
            if (b == '[') {
                m_state = State::Csi;
                m_csiParams.clear();
                m_csiPrivate = false;
            } else if (b == ']') {
                m_state = State::Osc;
                m_csiParams.clear();
            } else if (b == '(' || b == ')' || b == '*' || b == '+') {
                m_state = State::EscIntermediate; // charset designate: eat next
            } else {
                handleEsc(b);
                m_state = State::Ground;
            }
            break;

        case State::EscIntermediate:
            m_state = State::Ground; // consume the charset id byte, ignore
            break;

        case State::Csi:
            if ((b >= '0' && b <= '9') || b == ';' || b == ':') {
                m_csiParams.append((char)b);
            } else if (b == '?' || b == '>' || b == '!') {
                m_csiPrivate = (b == '?');
            } else if (b >= 0x40 && b <= 0x7E) {
                handleCsi(b);
                m_state = State::Ground;
            }
            break;

        case State::Osc:
            if (b == 0x07) {
                m_state = State::Ground;
            } else if (b == 0x1B) {
                m_state = State::Esc; // likely ST; handleEsc('\\') is a no-op
            }
            break;
        }
    }
}

void TerminalWidget::handleControl(char32_t ch)
{
    switch (ch) {
    case '\r':
        m_cx = 0;
        m_wrapPending = false;
        break;
    case '\n':
    case 0x0B: // VT
    case 0x0C: // FF
        newLine();
        m_wrapPending = false;
        break;
    case '\b':
        if (m_wrapPending)
            m_wrapPending = false;
        else if (m_cx > 0)
            --m_cx;
        break;
    case '\t': {
        int next = (m_cx / 8 + 1) * 8;
        m_cx = qMin(next, m_cols - 1);
        break;
    }
    case 0x07: // BEL
        break;
    default:
        break;
    }
}

void TerminalWidget::handleEsc(char32_t ch)
{
    switch (ch) {
    case 'M': // Reverse Index: move up, scrolling at the top margin.
        if (m_cy == m_scrollTop)
            scrollDown(m_scrollTop, m_scrollBottom, 1);
        else if (m_cy > 0)
            --m_cy;
        break;
    case 'D': // Index
        newLine();
        break;
    case 'E': // Next Line
        m_cx = 0;
        newLine();
        break;
    case '7': // Save cursor
        m_savedCx = m_cx;
        m_savedCy = m_cy;
        break;
    case '8': // Restore cursor
        m_cx = m_savedCx;
        m_cy = m_savedCy;
        break;
    case 'c': // Reset
        for (Row &r : m_screen)
            clearRow(r);
        m_cx = m_cy = 0;
        m_curFg = kDefaultFg;
        m_curBg = kDefaultBg;
        m_curBold = m_curInverse = m_curUnderline = false;
        break;
    default:
        break;
    }
}

void TerminalWidget::handleCsi(char32_t finalByte)
{
    m_params.clear();
    if (!m_csiParams.isEmpty()) {
        const QList<QByteArray> parts = m_csiParams.split(';');
        for (const QByteArray &p : parts) {
            const QByteArray head = p.split(':').value(0);
            m_params.append(head.isEmpty() ? -1 : head.toInt());
        }
    }
    auto param = [&](int i, int def) {
        if (i < m_params.size() && m_params[i] > 0)
            return m_params[i];
        return def;
    };

    switch (finalByte) {
    case 'A': m_cy = qMax(0, m_cy - param(0, 1)); break;
    case 'B': m_cy = qMin(m_rows - 1, m_cy + param(0, 1)); break;
    case 'C': m_cx = qMin(m_cols - 1, m_cx + param(0, 1)); m_wrapPending = false; break;
    case 'D': m_cx = qMax(0, m_cx - param(0, 1)); m_wrapPending = false; break;
    case 'E': m_cx = 0; m_cy = qMin(m_rows - 1, m_cy + param(0, 1)); break;
    case 'F': m_cx = 0; m_cy = qMax(0, m_cy - param(0, 1)); break;
    case 'G': m_cx = qBound(0, param(0, 1) - 1, m_cols - 1); m_wrapPending = false; break;
    case 'd': m_cy = qBound(0, param(0, 1) - 1, m_rows - 1); break;
    case 'H':
    case 'f':
        m_cy = qBound(0, param(0, 1) - 1, m_rows - 1);
        m_cx = qBound(0, param(1, 1) - 1, m_cols - 1);
        m_wrapPending = false;
        break;
    case 'J': eraseInDisplay(param(0, 0)); break;
    case 'K': eraseInLine(param(0, 0)); break;
    case 'L': scrollDown(m_cy, m_scrollBottom, param(0, 1)); break; // insert lines
    case 'M': scrollUp(m_cy, m_scrollBottom, param(0, 1)); break;   // delete lines
    case 'S': scrollUp(m_scrollTop, m_scrollBottom, param(0, 1)); break;
    case 'T': scrollDown(m_scrollTop, m_scrollBottom, param(0, 1)); break;
    case '@': { // Insert blank chars
        QVector<Row> &buf = m_altActive ? m_alt : m_screen;
        const int n = param(0, 1);
        for (int i = 0; i < n; ++i)
            buf[m_cy].insert(m_cx, blankCell());
        buf[m_cy].resize(m_cols);
        break;
    }
    case 'P': { // Delete chars
        QVector<Row> &buf = m_altActive ? m_alt : m_screen;
        const int n = param(0, 1);
        for (int i = 0; i < n && m_cx < buf[m_cy].size(); ++i)
            buf[m_cy].removeAt(m_cx);
        while (buf[m_cy].size() < m_cols)
            buf[m_cy].append(blankCell());
        break;
    }
    case 'X': { // Erase chars
        QVector<Row> &buf = m_altActive ? m_alt : m_screen;
        const int n = param(0, 1);
        for (int i = 0; i < n && m_cx + i < m_cols; ++i)
            buf[m_cy][m_cx + i] = blankCell();
        break;
    }
    case 'r': // DECSTBM scroll region
        m_scrollTop = qBound(0, param(0, 1) - 1, m_rows - 1);
        m_scrollBottom = qBound(0, param(1, m_rows) - 1, m_rows - 1);
        if (m_scrollTop > m_scrollBottom) {
            m_scrollTop = 0;
            m_scrollBottom = m_rows - 1;
        }
        m_cx = 0;
        m_cy = m_scrollTop;
        break;
    case 's': m_savedCx = m_cx; m_savedCy = m_cy; break;
    case 'u': m_cx = m_savedCx; m_cy = m_savedCy; break;
    case 'm': applySgr(); break;
    case 'h':
    case 'l': {
        const bool set = (finalByte == 'h');
        if (m_csiPrivate) {
            for (int p : m_params) {
                switch (p) {
                case 1: m_appCursorKeys = set; break;
                case 7: m_autoWrap = set; break;
                case 25: m_cursorVisible = set; break;
                case 47:
                case 1047:
                case 1049: useAltScreen(set); break;
                case 2004: m_bracketedPaste = set; break;
                default: break;
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

void TerminalWidget::applySgr()
{
    if (m_params.isEmpty())
        m_params.append(0);
    for (int i = 0; i < m_params.size(); ++i) {
        int p = m_params[i];
        if (p < 0)
            p = 0;
        if (p == 0) {
            m_curFg = kDefaultFg;
            m_curBg = kDefaultBg;
            m_curBold = m_curInverse = m_curUnderline = false;
        } else if (p == 1) {
            m_curBold = true;
        } else if (p == 22) {
            m_curBold = false;
        } else if (p == 4) {
            m_curUnderline = true;
        } else if (p == 24) {
            m_curUnderline = false;
        } else if (p == 7) {
            m_curInverse = true;
        } else if (p == 27) {
            m_curInverse = false;
        } else if (p >= 30 && p <= 37) {
            m_curFg = kPalette[p - 30];
        } else if (p >= 90 && p <= 97) {
            m_curFg = kPalette[8 + p - 90];
        } else if (p == 39) {
            m_curFg = kDefaultFg;
        } else if (p >= 40 && p <= 47) {
            m_curBg = kPalette[p - 40];
        } else if (p >= 100 && p <= 107) {
            m_curBg = kPalette[8 + p - 100];
        } else if (p == 49) {
            m_curBg = kDefaultBg;
        } else if (p == 38 || p == 48) {
            QColor col;
            if (i + 1 < m_params.size() && m_params[i + 1] == 5) {
                col = xtermColor(qBound(0, m_params.value(i + 2), 255));
                i += 2;
            } else if (i + 1 < m_params.size() && m_params[i + 1] == 2) {
                col = QColor(qBound(0, m_params.value(i + 2), 255),
                             qBound(0, m_params.value(i + 3), 255),
                             qBound(0, m_params.value(i + 4), 255));
                i += 4;
            }
            if (col.isValid()) {
                if (p == 38)
                    m_curFg = col;
                else
                    m_curBg = col;
            }
        }
    }
}


int TerminalWidget::streamRowCount() const
{
    const int sb = m_altActive ? 0 : m_scrollback.size();
    const QVector<Row> &buf = m_altActive ? m_alt : m_screen;
    return sb + buf.size();
}

const TerminalWidget::Row *TerminalWidget::rowAt(int absRow) const
{
    const int sb = m_altActive ? 0 : m_scrollback.size();
    const QVector<Row> &buf = m_altActive ? m_alt : m_screen;
    if (absRow < 0 || absRow >= sb + buf.size())
        return nullptr;
    return absRow < sb ? &m_scrollback[absRow] : &buf[absRow - sb];
}

TerminalWidget::CellPos TerminalWidget::cellPosAt(const QPoint &widgetPos) const
{
    const int sb = m_altActive ? 0 : m_scrollback.size();
    const int off = m_altActive ? 0 : qBound(0, m_viewOffset, sb);
    const int sy = qBound(0, widgetPos.y() / qMax(1, m_cellH), m_rows - 1);
    const int sx = qBound(0, widgetPos.x() / qMax(1, m_cellW), m_cols - 1);
    CellPos pos;
    pos.row = qBound(0, sb - off + sy, qMax(0, streamRowCount() - 1));
    pos.col = sx;
    return pos;
}

QString TerminalWidget::selectedText() const
{
    if (!m_hasSelection)
        return QString();
    CellPos start = m_selAnchor;
    CellPos end = m_selCursor;
    if (start.row > end.row || (start.row == end.row && start.col > end.col))
        std::swap(start, end);

    QStringList lines;
    for (int r = start.row; r <= end.row; ++r) {
        const Row *row = rowAt(r);
        if (!row)
            continue;
        const int fromCol = (r == start.row) ? start.col : 0;
        const int toCol = (r == end.row) ? end.col : row->size() - 1;
        QString line;
        for (int c = fromCol; c <= toCol && c < row->size(); ++c) {
            const char32_t ch = (*row)[c].ch;
            line += ch == 0 ? QChar(' ') : QString::fromUcs4(&ch, 1);
        }
        while (line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

void TerminalWidget::copySelection() const
{
    const QString text = selectedText();
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

void TerminalWidget::clearSelection()
{
    m_selecting = false;
    if (!m_hasSelection)
        return;
    m_hasSelection = false;
    update();
}


void TerminalWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), kDefaultBg);
    painter.setFont(m_font);
    QFontMetrics fm(m_font);
    const int ascent = fm.ascent();

    const QVector<Row> &buf = m_altActive ? m_alt : m_screen;

    const int sb = m_altActive ? 0 : m_scrollback.size();
    const int off = m_altActive ? 0 : qBound(0, m_viewOffset, sb);

    CellPos selStart = m_selAnchor;
    CellPos selEnd = m_selCursor;
    if (selStart.row > selEnd.row ||
        (selStart.row == selEnd.row && selStart.col > selEnd.col))
        std::swap(selStart, selEnd);

    for (int sy = 0; sy < m_rows; ++sy) {
        const int streamIdx = sb - off + sy;
        const Row *row = nullptr;
        if (streamIdx < sb)
            row = &m_scrollback[streamIdx];
        else if (streamIdx - sb < buf.size())
            row = &buf[streamIdx - sb];
        if (!row)
            continue;

        const int y = sy * m_cellH;
        const int n = qMin(row->size(), m_cols);
        for (int x = 0; x < n; ++x) {
            const Cell &c = (*row)[x];
            QColor fg = c.fg.isValid() ? c.fg : kDefaultFg;
            QColor bg = c.bg.isValid() ? c.bg : kDefaultBg;
            if (c.inverse)
                std::swap(fg, bg);
            const bool selected = m_hasSelection && streamIdx >= selStart.row &&
                streamIdx <= selEnd.row &&
                (streamIdx > selStart.row || x >= selStart.col) &&
                (streamIdx < selEnd.row || x <= selEnd.col);
            if (selected) {
                bg = QColor(0x3a, 0x6e, 0xa5);
                fg = Qt::white;
            }
            const int px = x * m_cellW;
            if (bg != kDefaultBg)
                painter.fillRect(px, y, m_cellW, m_cellH, bg);
            if (c.ch != U' ' && c.ch != 0) {
                QFont f = m_font;
                if (c.bold)
                    f.setBold(true);
                f.setUnderline(c.underline);
                painter.setFont(f);
                painter.setPen(fg);
                painter.drawText(px, y + ascent,
                                 QString::fromUcs4(&c.ch, 1));
            }
        }
    }

    if (m_cursorVisible && off == 0 && m_cx < m_cols && m_cy < m_rows) {
        const int px = m_cx * m_cellW;
        const int py = m_cy * m_cellH;
        if (m_hasFocus) {
            painter.fillRect(px, py, m_cellW, m_cellH, kDefaultFg);
            const Cell &c = buf[m_cy][qMin(m_cx, m_cols - 1)];
            if (c.ch != U' ' && c.ch != 0) {
                painter.setFont(m_font);
                painter.setPen(kDefaultBg);
                painter.drawText(px, py + ascent, QString::fromUcs4(&c.ch, 1));
            }
        } else {
            painter.setPen(kDefaultFg);
            painter.drawRect(px, py, m_cellW - 1, m_cellH - 1);
        }
    }
}


void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    if (m_master < 0 && !m_remoteInput) {
        QWidget::keyPressEvent(event);
        return;
    }

    if (m_viewOffset != 0) {
        m_viewOffset = 0;
        update();
    }

    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();

    if (mods.testFlag(Qt::ControlModifier) && mods.testFlag(Qt::ShiftModifier) &&
        key == Qt::Key_C) {
        copySelection();
        return;
    }

    if ((mods.testFlag(Qt::ControlModifier) && mods.testFlag(Qt::ShiftModifier) &&
         key == Qt::Key_V) ||
        (mods.testFlag(Qt::ShiftModifier) && key == Qt::Key_Insert)) {
        pasteFromClipboard();
        return;
    }

    auto arrow = [&](char c) {
        return m_appCursorKeys ? QByteArray("\x1bO") + c : QByteArray("\x1b[") + c;
    };

    QByteArray seq;
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter: seq = "\r"; break;
    case Qt::Key_Backspace: seq = "\x7f"; break;
    case Qt::Key_Tab: seq = "\t"; break;
    case Qt::Key_Escape: seq = "\x1b"; break;
    case Qt::Key_Up: seq = arrow('A'); break;
    case Qt::Key_Down: seq = arrow('B'); break;
    case Qt::Key_Right: seq = arrow('C'); break;
    case Qt::Key_Left: seq = arrow('D'); break;
    case Qt::Key_Home: seq = "\x1b[H"; break;
    case Qt::Key_End: seq = "\x1b[F"; break;
    case Qt::Key_Insert: seq = "\x1b[2~"; break;
    case Qt::Key_Delete: seq = "\x1b[3~"; break;
    case Qt::Key_PageUp: seq = "\x1b[5~"; break;
    case Qt::Key_PageDown: seq = "\x1b[6~"; break;
    default:
        if (mods.testFlag(Qt::ControlModifier) && key >= Qt::Key_A &&
            key <= Qt::Key_Z) {
            seq = QByteArray(1, (char)(key - Qt::Key_A + 1)); // Ctrl-A..Ctrl-Z
        } else if (!event->text().isEmpty()) {
            seq = event->text().toUtf8();
        }
        break;
    }

    if (!seq.isEmpty())
        writeToPty(seq);
    else
        QWidget::keyPressEvent(event);
}

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
    if (m_altActive || m_scrollback.isEmpty()) {
        event->ignore();
        return;
    }
    const int steps = event->angleDelta().y() / 40;
    m_viewOffset = qBound(0, m_viewOffset + steps, m_scrollback.size());
    update();
    event->accept();
}

void TerminalWidget::focusInEvent(QFocusEvent *)
{
    m_hasFocus = true;
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent *)
{
    m_hasFocus = false;
    update();
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    m_selecting = true;
    m_hasSelection = false;
    m_selAnchor = m_selCursor = cellPosAt(event->position().toPoint());
    update();
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_selecting) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const CellPos pos = cellPosAt(event->position().toPoint());
    if (pos.row != m_selCursor.row || pos.col != m_selCursor.col) {
        m_selCursor = pos;
        m_hasSelection =
            m_selAnchor.row != m_selCursor.row || m_selAnchor.col != m_selCursor.col;
        update();
    }
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_selecting) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_selecting = false;
    if (m_hasSelection)
        copySelection();
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const CellPos pos = cellPosAt(event->position().toPoint());
    const Row *row = rowAt(pos.row);
    if (!row || pos.col >= row->size())
        return;
    auto isWordChar = [](char32_t ch) { return ch != U' ' && ch != 0; };
    if (!isWordChar((*row)[pos.col].ch))
        return;
    int startCol = pos.col;
    int endCol = pos.col;
    while (startCol > 0 && isWordChar((*row)[startCol - 1].ch))
        --startCol;
    while (endCol + 1 < row->size() && isWordChar((*row)[endCol + 1].ch))
        ++endCol;
    m_selAnchor = {pos.row, startCol};
    m_selCursor = {pos.row, endCol};
    m_hasSelection = true;
    update();
    copySelection();
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *copyAction = menu.addAction(QStringLiteral("Copy"));
    copyAction->setEnabled(m_hasSelection);
    QAction *pasteAction = menu.addAction(QStringLiteral("Paste"));
    pasteAction->setEnabled(!QApplication::clipboard()->text().isEmpty());
    QAction *chosen = menu.exec(event->globalPos());
    if (chosen == copyAction) {
        copySelection();
    } else if (chosen == pasteAction) {
        pasteFromClipboard();
    }
}

bool TerminalWidget::event(QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            keyPressEvent(ke);
            return true;
        }
    }
    return QWidget::event(event);
}
