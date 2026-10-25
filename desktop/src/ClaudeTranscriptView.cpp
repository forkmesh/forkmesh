#include "ClaudeTranscriptView.h"
#include "WorkspaceLayout.h"

#include "AgentLogText.h"
#include "AgentPromptImages.h"
#include "ClaudeTranscriptTools.h"
#include "CodexTranscriptStyle.h"
#include "PromptDeckBar.h"
#include "ScrollJumpButtons.h"
#include "Theme.h"
#include "DesignTokens.h"
#include "Tooltips.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QImageReader>
#include <QtConcurrentRun>
#include <cmath>
#include <functional>
#include <utility>
#include <QHelpEvent>
#include <QBuffer>
#include <QButtonGroup>
#include <QClipboard>
#include <QColor>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPalette>
#include <QPen>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QPixmap>
#include <QPointer>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QScrollBar>
#include <QSvgRenderer>
#include <QCache>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <iterator>

namespace {
QString esc(const QString &s) { return s.toHtmlEscaped(); }

// Resolved once: QFontDatabase::systemFont() round-trips the platform theme
// every call, and a single command row asked for it five or six times (the
// rail glyph painters asked again on every repaint).
QFont monoFont()
{
    static const QFont font = [] {
        QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        const QStringList installed = QFontDatabase::families();
        for (const QString &preferred :
             {QStringLiteral("JetBrains Mono"), QStringLiteral("Cascadia Mono"),
              QStringLiteral("SF Mono"), QStringLiteral("Menlo"),
              QStringLiteral("DejaVu Sans Mono")}) {
            if (installed.contains(preferred, Qt::CaseInsensitive)) {
                f.setFamily(preferred);
                break;
            }
        }
        f.setStyleHint(QFont::Monospace);
        f.setFixedPitch(true);
        f.setStyleStrategy(QFont::PreferAntialias);
        return f;
    }();
    return Theme::monospaceFont(font);
}

const QFont &monoBoldFont()
{
    static const QFont font = [] {
        QFont f = monoFont();
        f.setBold(true);
        return f;
    }();
    return font;
}

QFont proseFont()
{
    if (!forkmesh::ui::workspaceLayoutNew())
        return monoFont();
    QFont font = Theme::interfaceFont(Theme::Design::Next, QGuiApplication::font());
    font.setPixelSize(15);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

const QFont &monoBold13Font()
{
    static const QFont font = [] {
        QFont f = monoFont();
        f.setBold(true);
        f.setPixelSize(13);
        return f;
    }();
    return font;
}

QString permissionModeLabel(const QString &raw)
{
    if (raw == QLatin1String("bypassPermissions") || raw == QLatin1String("acceptAll"))
        return QStringLiteral("Auto");
    if (raw == QLatin1String("acceptEdits"))
        return QStringLiteral("Edit");
    if (raw == QLatin1String("plan"))
        return QStringLiteral("Plan");
    if (raw == QLatin1String("default"))
        return QStringLiteral("Ask");
    return raw; // an unknown/future mode reads better verbatim than dropped
}

const QString kMatchBg = QStringLiteral("#ffd33d");
const QString kMatchFg = QStringLiteral("#1f2328");
const QString kCurMatchBg = QStringLiteral("#ff8c42");

int countOccurrences(const QString &hay, const QString &needle)
{
    int n = 0, from = 0;
    while (true) {
        const int i = hay.indexOf(needle, from, Qt::CaseInsensitive);
        if (i < 0)
            break;
        ++n;
        from = i + needle.size();
    }
    return n;
}

int countMatchesIn(const QString &orig, Qt::TextFormat fmt, const QString &query)
{
    if (query.isEmpty() || orig.indexOf(query, 0, Qt::CaseInsensitive) < 0)
        return 0;
    if (fmt != Qt::MarkdownText && fmt != Qt::RichText)
        return countOccurrences(orig, query);
    QTextDocument d;
    if (fmt == Qt::MarkdownText)
        d.setMarkdown(orig);
    else
        d.setHtml(orig);
    int n = 0;
    for (QTextCursor c = d.find(query); !c.isNull(); c = d.find(query, c))
        ++n;
    return n;
}

const QRegularExpression &transcriptReferenceRegex()
{
    // adhoc #2104: the file alternative is tried before the branch one. Every
    // branch prefix the branch alternative knows — docs/, test/, fix/, build/ —
    // is also an ordinary directory name, so with branch first a path like
    // `docs/operations/database-audit.md` was linked as a branch and clicking it
    // reported "Branch docs/operations/database-audit.md is not available in
    // this repository". A reference whose last segment carries an extension is a
    // file; branch names that lack one (agent/…, main) still fall through to the
    // branch alternative because the file one needs a dotted tail to match.
    static const QRegularExpression re(QStringLiteral(
        "(?<permalink>forkmesh://(?:issue|pull|commit|cove)/[^\\s<>()\\[\\]]*[^\\s<>()\\[\\].,;:!?'\"])"
        "|(?<![\\w/#])(?<pull>(?:pull[-\\s]+request|pull|pr)[-\\s]*#(?<pullNumber>\\d+))\\b"
        "|(?<![\\w/#])(?<issue>issue[-\\s]*#(?<issueNumber>\\d+))\\b"
        "|(?<![\\w/#])#(?<bareNumber>\\d+)\\b"
        "|(?<![\\w/#])(?<task>task(?:[-\\h]+id)?[-:#\\h]*(?<taskId>[0-9a-f]{32}))(?![\\w/-]|\\.[\\w/-])"
        "|(?<![\\w./-])(?=[0-9a-f]{7,40}(?![0-9a-f]))(?=[0-9a-f]*[a-f])(?<commit>[0-9a-f]{7,40})(?![\\w./-])"
        "|(?<![\\w./-])(?<file>(?:/|\\./)?(?:(?:[A-Za-z0-9_.-]+/)+)?(?:[A-Za-z_][A-Za-z0-9_.-]*\\.[A-Za-z][A-Za-z0-9]{0,11}|\\.[A-Za-z0-9][A-Za-z0-9_.-]*|CMakeLists\\.txt|Dockerfile|Makefile|README(?:\\.[A-Za-z0-9]+)?|LICENSE(?:\\.[A-Za-z0-9]+)?))(?::(?<line>\\d+)|#L(?<hashLine>\\d+))?"
        "|(?<![\\w./-])(?<branch>(?:(?:api-pr|feature|feat|fix|bugfix|hotfix|release|agent|pr|chore|refactor|docs|test|ci|build|perf|style)/[A-Za-z0-9._/-]*[A-Za-z0-9_-]|main|master|develop|development|trunk))(?![\\w./-])"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

enum class TranscriptRefKind { Permalink, Pull, Issue, Task, Commit, Branch, File };

TranscriptRefKind transcriptRefKind(const QRegularExpressionMatch &m)
{
    if (!m.captured(QStringLiteral("permalink")).isEmpty())
        return TranscriptRefKind::Permalink;
    if (!m.captured(QStringLiteral("pull")).isEmpty())
        return TranscriptRefKind::Pull;
    if (!m.captured(QStringLiteral("issue")).isEmpty() ||
        !m.captured(QStringLiteral("bareNumber")).isEmpty())
        return TranscriptRefKind::Issue;
    if (!m.captured(QStringLiteral("task")).isEmpty())
        return TranscriptRefKind::Task;
    if (!m.captured(QStringLiteral("commit")).isEmpty())
        return TranscriptRefKind::Commit;
    if (!m.captured(QStringLiteral("branch")).isEmpty())
        return TranscriptRefKind::Branch;
    return TranscriptRefKind::File;
}

QString transcriptRefHref(const QRegularExpressionMatch &m)
{
    switch (transcriptRefKind(m)) {
    case TranscriptRefKind::Permalink:
        return m.captured(QStringLiteral("permalink"));
    case TranscriptRefKind::Pull:
        return QStringLiteral("forkmesh-pull:%1")
            .arg(m.captured(QStringLiteral("pullNumber")));
    case TranscriptRefKind::Issue:
        if (!m.captured(QStringLiteral("issueNumber")).isEmpty())
            return QStringLiteral("forkmesh-issue:%1")
                .arg(m.captured(QStringLiteral("issueNumber")));
        return QStringLiteral("forkmesh-ref:%1")
            .arg(m.captured(QStringLiteral("bareNumber")));
    case TranscriptRefKind::Task:
        return QStringLiteral("forkmesh-task:%1")
            .arg(m.captured(QStringLiteral("taskId")).toLower());
    case TranscriptRefKind::Commit:
        return QStringLiteral("forkmesh-commit:%1")
            .arg(m.captured(QStringLiteral("commit")));
    case TranscriptRefKind::Branch:
        return QStringLiteral("forkmesh-branch:%1")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(
                m.captured(QStringLiteral("branch")))));
    case TranscriptRefKind::File: {
        QString href = QStringLiteral("forkmesh-file:%1")
                           .arg(QString::fromLatin1(
                               QUrl::toPercentEncoding(
                                   m.captured(QStringLiteral("file")))));
        const QString line = !m.captured(QStringLiteral("line")).isEmpty()
                                 ? m.captured(QStringLiteral("line"))
                                 : m.captured(QStringLiteral("hashLine"));
        if (!line.isEmpty())
            href += QStringLiteral("?line=%1").arg(line);
        return href;
    }
    }
    return QString();
}

QString linkifyTranscriptText(const QString &text, bool html,
                              const QString &linkColour = QString())
{
    QString out;
    int last = 0;
    auto it = transcriptReferenceRegex().globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString before = text.mid(last, m.capturedStart() - last);
        out += html ? esc(before) : before;
        const QString shown = m.captured(0);
        const QString href = transcriptRefHref(m);
        if (transcriptRefKind(m) == TranscriptRefKind::Permalink && !html)
            out += QStringLiteral("<%1>").arg(href);
        else if (html) {
            const QString style =
                linkColour.isEmpty()
                    ? QString()
                    : QStringLiteral(
                          " style=\"color:%1;text-decoration:underline;\"")
                          .arg(linkColour);
            out += QStringLiteral("<a href=\"%1\"%2>%3</a>")
                       .arg(href.toHtmlEscaped(), style,
                            shown.toHtmlEscaped());
        }
        else
            out += QStringLiteral("[%1](%2)").arg(shown, href);
        last = m.capturedEnd();
    }
    out += html ? esc(text.mid(last)) : text.mid(last);
    return out;
}

QString linkifiedTerminalHtml(const QString &text, const QString &linkColour,
                              bool wrap)
{
    QString html = linkifyTranscriptText(text, true, linkColour);
    html.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return QStringLiteral("<span style='white-space:%1'>%2</span>")
        .arg(wrap ? QStringLiteral("pre-wrap") : QStringLiteral("pre"), html);
}

int transcriptMarkdownLinkEnd(const QString &line, int start)
{
    const int close = line.indexOf(QLatin1Char(']'), start + 1);
    if (close < 0 || close + 1 >= line.size() ||
        line.at(close + 1) != QLatin1Char('('))
        return -1;
    const int end = line.indexOf(QLatin1Char(')'), close + 2);
    return end < 0 ? -1 : end + 1;
}

int transcriptAutolinkEnd(const QString &line, int start)
{
    const int close = line.indexOf(QLatin1Char('>'), start + 1);
    if (close < 0 ||
        !line.mid(start + 1, close - start - 1).contains(QStringLiteral("://")))
        return -1;
    return close + 1;
}

bool exactBranchOrFileReference(const QString &text)
{
    const QRegularExpressionMatch m = transcriptReferenceRegex().match(text);
    if (!m.hasMatch() || m.capturedStart() != 0 || m.capturedEnd() != text.size())
        return false;
    const TranscriptRefKind kind = transcriptRefKind(m);
    return kind == TranscriptRefKind::Branch || kind == TranscriptRefKind::File ||
           kind == TranscriptRefKind::Task;
}

QString taskIdCodeHref(const QString &before, const QString &code)
{
    static const QRegularExpression id(QStringLiteral("^[0-9a-fA-F]{32}$"));
    static const QRegularExpression lead(
        QStringLiteral("(?<![\\w/#])task(?:[-\\h]+id)?[-:#\\h]*$"),
        QRegularExpression::CaseInsensitiveOption);
    if (!id.match(code).hasMatch() || !lead.match(before).hasMatch())
        return QString();
    return QStringLiteral("forkmesh-task:%1").arg(code.toLower());
}

QString linkifyTranscriptLine(const QString &line)
{
    QString out;
    int i = 0;
    while (i < line.size()) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('`')) {
            int run = 1;
            while (i + run < line.size() && line.at(i + run) == QLatin1Char('`'))
                ++run;
            const QString ticks = line.mid(i, run);
            const int close = line.indexOf(ticks, i + run);
            if (close >= 0) {
                const QString code = line.mid(i + run, close - i - run);
                const QString taskHref = taskIdCodeHref(out, code);
                if (exactBranchOrFileReference(code))
                    out += linkifyTranscriptText(code, false);
                else if (!taskHref.isEmpty())
                    out += QStringLiteral("[%1](%2)").arg(code, taskHref);
                else
                    out += line.mid(i, close + run - i);
                i = close + run;
            } else {
                out += ticks;
                i += run;
            }
            continue;
        }
        if (c == QLatin1Char('[')) {
            const int end = transcriptMarkdownLinkEnd(line, i);
            if (end > i) {
                out += line.mid(i, end - i);
                i = end;
                continue;
            }
        }
        if (c == QLatin1Char('<')) {
            const int end = transcriptAutolinkEnd(line, i);
            if (end > i) {
                out += line.mid(i, end - i);
                i = end;
                continue;
            }
        }
        int next = i;
        while (next < line.size()) {
            const QChar d = line.at(next);
            if (d == QLatin1Char('`') || d == QLatin1Char('[') ||
                d == QLatin1Char('<'))
                break;
            ++next;
        }
        if (next == i) {
            out += c;
            ++i;
        } else {
            out += linkifyTranscriptText(line.mid(i, next - i), false);
            i = next;
        }
    }
    return out;
}
} // namespace

static QString capLabelText(const QString &text, bool collapseLines);

bool applicationUsesDarkPalette()
{
    const QColor base = QGuiApplication::palette().color(QPalette::Base);
    return !base.isValid() || base.lightness() < 128;
}

void remapTranscriptPalette(ClaudeTranscriptView::Palette &palette)
{
    const auto remap = [](QString &value) {
        value = Theme::remapColor(QColor(value)).name(QColor::HexRgb);
    };
    for (QString *value : {&palette.canvas, &palette.surface, &palette.border,
                           &palette.text, &palette.muted, &palette.accent,
                           &palette.add, &palette.del, &palette.addBg,
                           &palette.delBg, &palette.userBg, &palette.aiBg,
                           &palette.aiEdge, &palette.userEdge, &palette.termBg,
                           &palette.termEdge, &palette.chipBg,
                           &palette.shellCommand, &palette.shellKeyword,
                           &palette.shellString, &palette.shellVariable,
                           &palette.shellOperator, &palette.shellComment,
                           &palette.shellPath})
        remap(*value);
}

// A small pulsing node that marks "this is happening right now". It replaces a
// QGraphicsOpacityEffect breathing across the whole card, which re-rendered the
// entire live section into an offscreen pixmap 60 times a second for as long as
// the agent kept thinking — one of the two things that made a streaming
// transcript feel heavy.
class PulseDot : public QWidget
{
public:
    explicit PulseDot(const QString &color, QWidget *parent = nullptr)
        : QWidget(parent), m_color(color)
    {
        setFixedSize(12, 12);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        m_anim = new QVariantAnimation(this);
        m_anim->setDuration(1300);
        m_anim->setStartValue(0.3);
        m_anim->setKeyValueAt(0.5, 1.0);
        m_anim->setEndValue(0.3);
        m_anim->setEasingCurve(QEasingCurve::InOutSine);
        m_anim->setLoopCount(-1);
        QObject::connect(m_anim, &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &v) {
                             m_level = v.toDouble();
                             update();
                         });
    }

    void setRunning(bool on)
    {
        if (on) {
            if (m_anim->state() != QAbstractAnimation::Running)
                m_anim->start();
            return;
        }
        m_anim->stop();
        m_level = 1.0;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        QColor c(m_color);
        c.setAlphaF(float(m_level));
        g.setPen(Qt::NoPen);
        g.setBrush(c);
        g.drawEllipse(QPointF(6, 6), 3.0, 3.0);
    }

private:
    QString m_color;
    QVariantAnimation *m_anim = nullptr;
    double m_level = 1.0;
};

class Collapsible : public QFrame
{
public:
    Collapsible(const QString &header, bool expanded,
                const ClaudeTranscriptView::Palette &p,
                QWidget *parent = nullptr)
        : QFrame(parent), m_open(expanded), m_label(header)
    {
        setObjectName(QStringLiteral("xscript_section"));
        setStyleSheet(QStringLiteral(
            "QFrame#xscript_section{background:transparent;border:none;}"));
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);
        auto *head = new QWidget(this);
        head->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
        auto *hh = new QHBoxLayout(head);
        hh->setContentsMargins(0, 0, 0, 0);
        hh->setSpacing(4);
        m_pulse = new PulseDot(p.accent, head);
        m_pulse->setVisible(false);
        hh->addWidget(m_pulse, 0, Qt::AlignVCenter);
        m_btn = new QPushButton(head);
        m_btn->setCursor(Qt::PointingHandCursor);
        m_btn->setFont(monoFont());
        m_btn->setStyleSheet(Theme::remapSheet(QStringLiteral(
            "QPushButton{border:none;background:transparent;text-align:left;"
            "color:%1;font-style:italic;padding:0;}"
            "QPushButton:hover{color:%2;}").arg(p.muted, p.accent)));
        hh->addWidget(m_btn, 1);
        v->addWidget(head);
        m_bodyWidget = new QWidget(this);
        m_bodyWidget->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;border:none;")));
        m_bodyLayout = new QVBoxLayout(m_bodyWidget);
        m_bodyLayout->setContentsMargins(14, 2, 0, 0); // indent under the header
        m_bodyLayout->setSpacing(6);
        v->addWidget(m_bodyWidget);
        m_bodyWidget->setVisible(m_open);
        QObject::connect(m_btn, &QPushButton::clicked, m_btn,
                         [this] { setExpanded(!m_open); });
        updateHeaderText();
    }
    QVBoxLayout *body() { return m_bodyLayout; }
    void setHeaderText(const QString &t) { m_label = t; updateHeaderText(); }

    void setExpanded(bool on)
    {
        if (on == m_open)
            return;
        m_open = on;
        updateHeaderText();
        animateBody();
    }

    void setPulsing(bool on)
    {
        if (!m_pulse)
            return;
        m_pulse->setVisible(on);
        m_pulse->setRunning(on);
    }

private:
    void updateHeaderText()
    {
        m_btn->setText((m_open ? QStringLiteral("▾ ") : QStringLiteral("▸ "))
                       + m_label);
    }
    void animateBody()
    {
        if (!m_anim) {
            m_anim = new QPropertyAnimation(m_bodyWidget, "maximumHeight", this);
            QObject::connect(m_anim, &QPropertyAnimation::finished, this, [this] {
                if (m_open)
                    m_bodyWidget->setMaximumHeight(QWIDGETSIZE_MAX);
                else
                    m_bodyWidget->setVisible(false);
            });
        }
        m_anim->stop();
        if (m_open) {
            m_bodyWidget->setVisible(true);
            m_anim->setDuration(180);
            m_anim->setEasingCurve(QEasingCurve::OutCubic);
            m_anim->setStartValue(0);
            m_anim->setEndValue(qMax(0, m_bodyWidget->sizeHint().height()));
        } else {
            m_anim->setDuration(160);
            m_anim->setEasingCurve(QEasingCurve::InCubic);
            m_anim->setStartValue(m_bodyWidget->height());
            m_anim->setEndValue(0);
        }
        m_anim->start();
    }
    QPushButton *m_btn = nullptr;
    QWidget *m_bodyWidget = nullptr;
    QVBoxLayout *m_bodyLayout = nullptr;
    bool m_open = true;
    QString m_label;
    QPropertyAnimation *m_anim = nullptr; // body fold animation
    PulseDot *m_pulse = nullptr;          // live "breathing" while streaming
};

class CacheLabel : public QLabel
{
public:
    using QLabel::QLabel;

    int heightForWidth(int w) const override
    {
        const int len = text().size();
        if (len != m_len) {
            m_len = len;
            m_heights.clear();
        }
        const auto it = m_heights.constFind(w);
        if (it != m_heights.constEnd())
            return it.value();

        // Once this label is on screen at a measured width, a resize must not synchronously
        // reshape it (and every other transcript row) on the GUI stack. Return a
        // close temporary height and let ClaudeTranscriptView warm the exact new
        // width in a time-bounded event-loop slice. The old height is exact when
        // the view grows; scale it when the view shrinks so the temporary layout
        // remains close until the queued measurement lands.
        const auto shown = m_shownWidth > 0 ? m_heights.constFind(m_shownWidth)
                                             : m_heights.constEnd();
        if (shown != m_heights.constEnd()) {
            m_pendingWidth = w;
            if (!m_relayoutQueued) {
                for (QWidget *ancestor = parentWidget(); ancestor;
                     ancestor = ancestor->parentWidget()) {
                    if (auto *view = qobject_cast<ClaudeTranscriptView *>(ancestor)) {
                        m_relayoutQueued = true;
                        view->queueLabelRelayout(
                            const_cast<CacheLabel *>(this));
                        break;
                    }
                }
            }
            if (m_relayoutQueued) {
                const int oldWidth = qMax(1, shown.key());
                const int oldHeight = shown.value();
                return w > 0 && w < oldWidth
                           ? qMax(oldHeight,
                                  qCeil(double(oldHeight) * oldWidth / w))
                           : oldHeight;
            }
        }

        const int h = QLabel::heightForWidth(w);
        if (m_heights.size() >= 32)
            m_heights.clear();
        m_heights.insert(w, h);
        return h;
    }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        if (isVisible())
            m_shownWidth = e->size().width();
        QLabel::resizeEvent(e);
    }

    void changeEvent(QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::FontChange:
        case QEvent::ApplicationFontChange:
        case QEvent::StyleChange:
            m_heights.clear(); // metrics may have shifted; recompute on next query
            break;
        default:
            break;
        }
        QLabel::changeEvent(e);
    }

private:
    friend class ClaudeTranscriptView;

    bool warmPendingHeight()
    {
        m_relayoutQueued = false;
        const int width = m_pendingWidth;
        if (width < 0)
            return false;
        const int len = text().size();
        if (len != m_len) {
            // The text changed again before this slice ran. Its next geometry
            // query will establish a fresh exact baseline for the new content.
            m_len = len;
            m_heights.clear();
            m_pendingWidth = -1;
            updateGeometry();
            return true;
        }
        m_heights.insert(width, QLabel::heightForWidth(width));
        if (m_heights.size() > 32)
            m_heights.erase(m_heights.constBegin());
        m_pendingWidth = -1;
        updateGeometry();
        return true;
    }

    mutable int m_len = -1;
    mutable QHash<int, int> m_heights; // width → cached heightForWidth
    mutable int m_pendingWidth = -1;
    mutable bool m_relayoutQueued = false;
    int m_shownWidth = -1;
};

static QString capLabelText(const QString &text, bool collapseLines);

static CacheLabel *monoSlice(const QString &text, const QString &fg,
                             const QString &linkColour, bool capCollapse)
{
    auto *l = new CacheLabel;
    l->setTextFormat(Qt::RichText);
    l->setText(linkifiedTerminalHtml(capLabelText(text, capCollapse), linkColour,
                                     true));
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    l->setStyleSheet(Theme::remapSheet(
        QStringLiteral("color:%1;background:transparent;border:none;").arg(fg)));
    return l;
}

class OutputPeek : public QWidget
{
public:
    static constexpr int kHead = 2;
    static constexpr int kTail = 2;

    OutputPeek(const QString &text, const QString &fg,
               const ClaudeTranscriptView::Palette &p, bool plusLabel = false,
               QWidget *parent = nullptr)
        : QWidget(parent), m_plus(plusLabel)
    {
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(2);

        const QStringList lines = text.split(QLatin1Char('\n'));
        m_hidden = lines.size() - kHead - kTail;
        if (m_hidden <= 0) {
            v->addWidget(monoSlice(text, fg, p.shellPath, false));
            return;
        }

        v->addWidget(monoSlice(lines.mid(0, kHead).join(QLatin1Char('\n')), fg,
                               p.shellPath, false));

        m_toggle = new QPushButton(this);
        m_toggle->setCursor(Qt::PointingHandCursor);
        if (m_plus)
            m_toggle->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_toggle->setStyleSheet(Theme::remapSheet(
            QStringLiteral("QPushButton{border:none;background:transparent;"
                           "text-align:left;color:%1;%2padding:1px 0;}"
                           "QPushButton:hover{color:%3;}")
                .arg(p.muted,
                     m_plus ? QString() : QStringLiteral("font-size:11px;"),
                     p.accent)));
        v->addWidget(m_toggle, 0, Qt::AlignLeft);

        m_middle = monoSlice(lines.mid(kHead, m_hidden).join(QLatin1Char('\n')), fg,
                             p.shellPath, true);
        m_middle->setVisible(false);
        v->addWidget(m_middle);

        v->addWidget(monoSlice(
            lines.mid(lines.size() - kTail, kTail).join(QLatin1Char('\n')), fg,
            p.shellPath, false));

        updateToggle();
        QObject::connect(m_toggle, &QPushButton::clicked, m_toggle,
                         [this] { setOpen(!m_open); });
    }

    void expand() { setOpen(true); }

private:
    void setOpen(bool on)
    {
        m_open = on;
        if (m_middle)
            m_middle->setVisible(on);
        updateToggle();
    }
    void updateToggle()
    {
        if (!m_toggle)
            return;
        const QString unit = m_hidden == 1 ? QStringLiteral("line")
                                           : QStringLiteral("lines");
        const QString shape = m_open ? QStringLiteral("hide %1 %2")
                                     : (m_plus ? QStringLiteral("… +%1 %2")
                                               : QStringLiteral("⋯ %1 %2"));
        m_toggle->setText(shape.arg(m_hidden).arg(unit));
    }
    QPushButton *m_toggle = nullptr;
    QWidget *m_middle = nullptr;
    int m_hidden = 0;
    bool m_open = false;
    bool m_plus = false;
};

struct ShellLine {
    int indent = 0;
    QString text;
    bool continued = false;
};

QVector<ShellLine> prettyShellLine(const QString &line)
{
    constexpr int kFoldAt = 60;
    if (line.size() <= kFoldAt)
        return {ShellLine{0, line, false}};
    static const QRegularExpression caseWord(QStringLiteral("(^|[\\s;(])case\\s"));
    if (caseWord.match(line).hasMatch())
        return {ShellLine{0, line, false}};

    struct Piece {
        QString op;
        QString text;
    };
    QVector<Piece> pieces;
    QString op;
    int start = 0;
    int depth = 0;
    bool inTest = false;
    QChar quote;
    bool escaped = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == QLatin1Char('\\') && quote != QLatin1Char('\'')) {
            escaped = true;
            continue;
        }
        if (!quote.isNull()) {
            if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')) {
            quote = c;
            continue;
        }
        const bool wordStart = i == 0 || line.at(i - 1).isSpace();
        if (c == QLatin1Char('#') && wordStart)
            break;
        if (c == QLatin1Char('(')) {
            ++depth;
            continue;
        }
        if (c == QLatin1Char(')')) {
            depth = qMax(0, depth - 1);
            continue;
        }
        const QString two = line.mid(i, 2);
        if (wordStart && two == QLatin1String("[[")) {
            inTest = true;
            continue;
        }
        if (inTest && two == QLatin1String("]]")) {
            inTest = false;
            ++i;
            continue;
        }
        if (depth > 0 || inTest)
            continue;
        QString found;
        if (two == QLatin1String("&&") || two == QLatin1String("||")
            || two == QLatin1String("|&"))
            found = two;
        else if (c == QLatin1Char('|') && (i == 0 || line.at(i - 1) != QLatin1Char('>')))
            found = QStringLiteral("|");
        else if (c == QLatin1Char(';') && two != QLatin1String(";;")
                 && (i == 0 || line.at(i - 1) != QLatin1Char(';')))
            found = QStringLiteral(";");
        if (found.isEmpty())
            continue;
        pieces.push_back({op, line.mid(start, i - start).trimmed()});
        op = found;
        i += int(found.size()) - 1;
        start = i + 1;
    }
    pieces.push_back({op, line.mid(start).trimmed()});
    if (pieces.size() < 2)
        return {ShellLine{0, line, false}};

    const auto firstWord = [](const QString &text) {
        return text.section(QLatin1Char(' '), 0, 0);
    };
    const auto afterWord = [](const QString &text, const QString &word) {
        return text.mid(word.size()).trimmed();
    };
    struct Open {
        int saved;
        int indent;
    };
    QVector<ShellLine> out;
    QVector<Open> open;
    int level = 0;
    int statement = 0;
    for (const Piece &piece : std::as_const(pieces)) {
        if (piece.op != QLatin1String(";") && !piece.op.isEmpty()) {
            if (!out.isEmpty())
                out.back().continued = true;
            QString text = piece.op + QLatin1Char(' ') + piece.text;
            out.push_back({statement + 1, text.trimmed(), false});
            continue;
        }
        QString text = piece.text;
        if (text.isEmpty())
            continue;
        QString word = firstWord(text);
        if ((word == QLatin1String("do") || word == QLatin1String("then"))
            && !out.isEmpty()) {
            out.back().text += QStringLiteral("; ") + word;
            open.push_back({level, out.back().indent});
            level = out.back().indent + 1;
            text = afterWord(text, word);
            if (text.isEmpty())
                continue;
            word = firstWord(text);
        }
        if ((word == QLatin1String("done") || word == QLatin1String("fi")
             || word == QLatin1String("esac"))
            && !open.isEmpty()) {
            const Open closed = open.takeLast();
            out.push_back({closed.indent, text, false});
            statement = closed.indent;
            level = closed.saved;
            continue;
        }
        if (word == QLatin1String("elif") && !open.isEmpty()) {
            const Open closed = open.takeLast();
            out.push_back({closed.indent, text, false});
            statement = closed.indent;
            level = closed.saved;
            continue;
        }
        if (word == QLatin1String("else") && !open.isEmpty()) {
            out.push_back({open.last().indent, word, false});
            text = afterWord(text, word);
            if (text.isEmpty())
                continue;
        }
        out.push_back({level, text, false});
        statement = level;
    }
    return out;
}

constexpr int kHoverLineIconPx = 13;

QString commandLineIconHtml(const QString &key,
                            const ClaudeTranscriptView::Palette &p);

// The whole command, highlighted line by line, for the hover card. Lines keep
// their own breaks (the row folds them onto one line), a string that runs over
// a line break stays string-coloured, and a heredoc body is shown as the text
// it is rather than parsed as shell. Bounded so a huge heredoc stays a card.
QString commandHoverHtml(const QString &raw,
                         const ClaudeTranscriptView::Palette &p,
                         const std::function<QString(const QString &)> &highlight)
{
    constexpr int kMaxLines = 24;
    constexpr qsizetype kMaxChars = 4000;

    QString text = raw;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    while (!text.isEmpty() && text.back().isSpace())
        text.chop(1);
    QStringList lines = text.split(QLatin1Char('\n'));
    const int totalLines = int(lines.size());
    int moreLines = 0;
    if (lines.size() > kMaxLines) {
        moreLines = int(lines.size()) - kMaxLines;
        lines = lines.mid(0, kMaxLines);
    }
    qsizetype budget = kMaxChars;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).size() > budget) {
            lines[i] = lines.at(i).left(qMax<qsizetype>(0, budget))
                       + QStringLiteral("…");
            moreLines = totalLines - (i + 1);
            lines = lines.mid(0, i + 1);
            break;
        }
        budget -= lines.at(i).size() + 1;
    }

    const auto span = [](const QString &s, const QString &colour) {
        return QStringLiteral("<span style='color:%1;'>%2</span>")
            .arg(colour, s.toHtmlEscaped());
    };
    // Where `quote` (already open) closes in `line`: the index just past it,
    // or -1 when the string carries on to the next line.
    const auto closeAt = [](const QString &line, QChar quote) {
        bool escaped = false;
        for (int i = 0; i < line.size(); ++i) {
            const QChar c = line.at(i);
            if (escaped) {
                escaped = false;
            } else if (c == QLatin1Char('\\') && quote != QLatin1Char('\'')) {
                escaped = true;
            } else if (c == quote) {
                return i + 1;
            }
        }
        return -1;
    };
    // The quote still open at the end of `line`, if any.
    const auto openQuoteAfter = [](const QString &line) {
        QChar quote;
        bool escaped = false;
        for (int i = 0; i < line.size(); ++i) {
            const QChar c = line.at(i);
            if (escaped) {
                escaped = false;
            } else if (c == QLatin1Char('\\') && quote != QLatin1Char('\'')) {
                escaped = true;
            } else if (quote.isNull()) {
                if (c == QLatin1Char('#')
                    && (i == 0 || line.at(i - 1).isSpace()))
                    break; // the rest is a comment
                if (c == QLatin1Char('\'') || c == QLatin1Char('"'))
                    quote = c;
            } else if (c == quote) {
                quote = QChar();
            }
        }
        return quote;
    };
    static const QRegularExpression heredoc(QStringLiteral(
        "(?<!<)<<(-?)(?!<)\\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\\2"));

    const auto lineIcon = [](const QString &shell) {
        const QVector<forkmesh::transcripttools::CommandIcon> icons =
            forkmesh::transcripttools::commandIcons(shell);
        return icons.isEmpty() ? QString() : icons.first().key;
    };

    QStringList out;
    QStringList outIcons;
    QChar carried;          // a string left open by the previous line
    QString heredocEnd;     // terminator of the heredoc being shown
    bool heredocTabs = false;
    bool joined = false;
    for (const QString &line : std::as_const(lines)) {
        while (outIcons.size() < out.size())
            outIcons << QString();
        if (!heredocEnd.isEmpty()) {
            const QString bare = heredocTabs
                ? QString(line).remove(QRegularExpression(QStringLiteral("^\\t+")))
                : line;
            if (bare == heredocEnd) {
                out << QStringLiteral("<span style='color:%1;font-weight:600;'>%2</span>")
                           .arg(p.shellOperator, line.toHtmlEscaped());
                heredocEnd.clear();
            } else {
                out << span(line, p.shellString);
            }
            continue;
        }
        QString rest = line;
        QString html;
        if (!carried.isNull()) {
            const int end = closeAt(line, carried);
            if (end < 0) {
                out << span(line, p.shellString);
                continue;
            }
            html = span(line.left(end), p.shellString);
            rest = line.mid(end);
        }
        const QVector<ShellLine> pretty =
            html.isEmpty() ? prettyShellLine(rest) : QVector<ShellLine>{};
        if (pretty.size() > 1) {
            for (const ShellLine &part : pretty) {
                outIcons << (joined ? QString() : lineIcon(part.text));
                QString folded = QString(part.indent * 2, QLatin1Char(' '))
                                 + highlight(part.text);
                if (part.continued)
                    folded += QStringLiteral(" <span style='color:%1;'>\\</span>")
                                  .arg(p.muted);
                out << folded;
            }
        } else {
            outIcons << (joined || !html.isEmpty() ? QString() : lineIcon(rest));
            html += highlight(rest);
            out << html;
        }
        carried = openQuoteAfter(rest);
        joined = carried.isNull() && rest.endsWith(QLatin1Char('\\'));
        if (carried.isNull()) {
            const QRegularExpressionMatch m = heredoc.match(rest);
            if (m.hasMatch()) {
                heredocEnd = m.captured(3);
                heredocTabs = !m.captured(1).isEmpty();
            }
        }
    }

    while (outIcons.size() < out.size())
        outIcons << QString();
    const int iconTop =
        qMax(0, (QFontMetrics(monoFont()).lineSpacing() - kHoverLineIconPx) / 2);
    QString rows;
    for (int i = 0; i < out.size(); ++i)
        rows += QStringLiteral("<tr><td valign='top' style='padding:%1px 8px 0 0;'>%2"
                               "</td><td style='white-space:pre-wrap;'>%3</td></tr>")
                    .arg(iconTop)
                    .arg(commandLineIconHtml(outIcons.at(i), p), out.at(i));
    QString html = QStringLiteral("<table cellspacing='0' cellpadding='0' "
                                  "style='color:%1;'>%2</table>")
                       .arg(p.text, rows);
    if (moreLines > 0)
        html += QStringLiteral(
                    "<div style='color:%1;font-style:italic;margin-top:6px;'>"
                    "… %2 more line%3</div>")
                    .arg(p.muted)
                    .arg(moreLines)
                    .arg(moreLines == 1 ? QString() : QStringLiteral("s"));
    return html;
}

// A solid card beside the pointer with the full highlighted command. It is not
// a QToolTip on purpose: Qt hides its tip on any Leave, focus change or expiry
// timer, and a live transcript re-lays itself out under a resting pointer
// (streamed output, the activity row), each of which sends the row a Leave.
// The tip then hid and re-showed on the next pixel of movement, which read as
// a blinking box however it was sized. This card only goes away once the
// pointer is really off the command row, a click or wheel lands on it, or the
// row itself is hidden.
class CommandHoverCard : public QFrame
{
public:
    CommandHoverCard(const QString &html, const ClaudeTranscriptView::Palette &p,
                     QWidget *source, bool command = true)
        : QFrame(source, Qt::ToolTip | Qt::FramelessWindowHint),
          m_bg(p.termBg), m_edge(p.termEdge),
          m_accent(Theme::edgeIndicators() ? QColor(p.accent) : QColor())
    {
        m_bg.setAlpha(255); // always a solid card, never see-through
        m_edge.setAlpha(255);
        setObjectName(QStringLiteral("agentCommandHoverCard"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground, false);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAutoFillBackground(true);
        QPalette solid = palette();
        solid.setColor(QPalette::Window, m_bg);
        setPalette(solid);
        setFocusPolicy(Qt::NoFocus);

        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(command ? 14 : 12, command ? 10 : 7, command ? 16 : 12,
                                command ? 11 : 8);
        row->setSpacing(8);
        if (command) {
            auto *prompt = new QLabel(QStringLiteral("❯"), this);
            prompt->setFont(monoFont());
            prompt->setStyleSheet(Theme::remapSheet(QStringLiteral(
                "color:%1;background:transparent;border:none;font-weight:700;")
                .arg(p.accent)));
            row->addWidget(prompt, 0, Qt::AlignTop);
        }

        m_text = new QLabel(this);
        m_text->setObjectName(QStringLiteral("agentCommandHoverText"));
        m_text->setTextFormat(Qt::RichText);
        m_text->setWordWrap(true);
        m_text->setFont(command ? monoFont() : QApplication::font(this));
        m_text->setStyleSheet(Theme::remapSheet(QStringLiteral(
            "color:%1;background:transparent;border:none;").arg(p.text)));
        m_text->setText(html);
        row->addWidget(m_text, 1);
    }

    QLabel *textLabel() const { return m_text; }

    // Size the card for `screen`: as wide as its longest line up to a readable
    // measure, wrapping anything longer, then as tall as that wrap needs.
    void fitTo(const QRect &screen)
    {
        ensurePolished();
        m_text->ensurePolished();
        QTextDocument doc;
        doc.setDefaultFont(m_text->font());
        doc.setDocumentMargin(0);
        doc.setHtml(m_text->text());
        const int maxText = qMax(240, qMin(760, screen.width() / 2));
        const int textWidth =
            qMin(maxText, int(std::ceil(doc.idealWidth())) + 2);
        m_text->setFixedWidth(textWidth);
        m_text->setFixedHeight(m_text->heightForWidth(textWidth));
        setFixedSize(layout()->sizeHint());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.fillRect(rect(), m_bg);
        if (m_accent.isValid())
            g.fillRect(QRect(0, 0, 3, height()), m_accent);
        g.setPen(QPen(m_edge, 1.0));
        g.setBrush(Qt::NoBrush);
        g.drawRect(rect().adjusted(0, 0, -1, -1));
    }

private:
    QColor m_bg, m_edge, m_accent;
    QLabel *m_text = nullptr;
};

class HoverCardHost
{
public:
    HoverCardHost(QWidget *source, const ClaudeTranscriptView::Palette &p)
        : m_source(source), m_palette(p)
    {
    }

    CommandHoverCard *visibleCard() const
    {
        const auto it = m_cards.constFind(m_key);
        return it != m_cards.constEnd() && it.value()->isVisible() ? it.value()
                                                                    : nullptr;
    }

    CommandHoverCard *card(const QString &key) const { return m_cards.value(key); }

    void show(const QString &key, const QRect &zone, const QPoint &globalPos,
              bool command, const std::function<QString()> &html)
    {
        if (m_key == key && visibleCard())
            return;
        hide();
        QScreen *screen = QGuiApplication::screenAt(globalPos);
        if (!screen)
            screen = m_source->screen();
        if (!screen)
            return;
        const QRect area = screen->availableGeometry();
        CommandHoverCard *card = m_cards.value(key);
        if (!card) {
            card = new CommandHoverCard(html(), m_palette, m_source, command);
            m_cards.insert(key, card);
            m_fitted.remove(key);
        }
        if (m_fitted.value(key) != area) {
            card->fitTo(area);
            m_fitted.insert(key, area);
        }
        m_key = key;
        m_zone = zone;
        card->setGeometry(Tooltips::placeBeside(QRect(QPoint(), card->size()),
                                                globalPos, area));
        card->show();
        card->raise();
        if (!m_watch) {
            m_watch = new QTimer(m_source);
            m_watch->setInterval(120);
            QObject::connect(m_watch, &QTimer::timeout, m_source,
                             [this] { hideIfLeft(); });
        }
        m_watch->start();
    }

    void hide()
    {
        if (m_watch)
            m_watch->stop();
        if (CommandHoverCard *card = visibleCard())
            card->hide();
    }

    void hideIfLeft()
    {
        const QRect zone(m_source->mapToGlobal(m_zone.topLeft()), m_zone.size());
        if (!m_source->isVisible() || m_source->visibleRegion().isEmpty()
            || !zone.contains(QCursor::pos()))
            hide();
    }

private:
    QWidget *m_source;
    ClaudeTranscriptView::Palette m_palette;
    QHash<QString, CommandHoverCard *> m_cards;
    QHash<QString, QRect> m_fitted;
    QString m_key;
    QRect m_zone;
    QTimer *m_watch = nullptr;
};

// Keep even a long shell invocation on one visual line. A compact horizontal
// scrollbar appears only when the card is narrower than the command; the label
// remains selectable and its path anchors remain clickable.
class CommandLineView : public QScrollArea
{
public:
    CommandLineView(const QString &html,
                    const ClaudeTranscriptView::Palette &p,
                    QWidget *parent = nullptr)
        : QScrollArea(parent), m_palette(p)
    {
        setObjectName(QStringLiteral("agentCommandLine"));
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(false);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFont(monoFont());
        setStyleSheet(QStringLiteral(
            "QScrollArea{background:transparent;border:none;padding:0;}"
            "QScrollArea>QWidget>QWidget{background:transparent;}"
            "QScrollBar:horizontal{height:6px;background:transparent;margin:0;}"
            "QScrollBar::handle:horizontal{background:%1;min-width:28px;"
            "border-radius:3px;}"
            "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{"
            "width:0;height:0;background:transparent;}"
            "QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{"
            "background:transparent;}")
                          .arg(p.termEdge));

        m_label = new QLabel;
        m_label->setObjectName(QStringLiteral("agentCommandText"));
        m_label->setTextFormat(Qt::RichText);
        m_label->setWordWrap(false);
        m_label->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                         Qt::LinksAccessibleByMouse);
        m_label->setOpenExternalLinks(false);
        m_label->setFont(monoFont());
        m_label->setStyleSheet(Theme::remapSheet(
            QStringLiteral("background:transparent;border:none;margin:0;")));
        m_label->setText(html);
        m_label->adjustSize();
        setWidget(m_label);

        // Reserve six pixels for the scrollbar so an overflow never covers the
        // command's descenders. On wide cards it is simply transparent padding.
        setFixedHeight(qCeil(QFontMetricsF(font()).lineSpacing()) + 7);

        // Hover arrives as a ToolTip event on whichever part is under the
        // pointer; watch the label and the viewport so none of it reaches
        // QToolTip, and so a click or wheel there dismisses the card.
        m_label->installEventFilter(this);
        viewport()->installEventFilter(this);
    }

    QLabel *commandLabel() const { return m_label; }

    // The command shown in the hover card. `hoverHtml` builds its rich text
    // on first hover only; most rows are never hovered.
    void setHoverCommand(std::function<QString()> hoverHtml)
    {
        m_hoverHtml = std::move(hoverHtml);
    }

    CommandHoverCard *hoverCard() const
    {
        return m_hover.card(QStringLiteral("command"));
    }

    void showHoverCard(const QPoint &globalPos)
    {
        if (m_hoverHtml)
            m_hover.show(QStringLiteral("command"), rect(), globalPos, true,
                         m_hoverHtml);
    }

    void hideHoverCard() { m_hover.hide(); }

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            showHoverCard(static_cast<QHelpEvent *>(e)->globalPos());
            return true;
        }
        if (e->type() == QEvent::Hide)
            hideHoverCard();
        return QScrollArea::event(e);
    }

    void showEvent(QShowEvent *e) override
    {
        if (m_label->size() != m_label->sizeHint())
            m_label->adjustSize();
        QScrollArea::showEvent(e);
    }

    void wheelEvent(QWheelEvent *e) override
    {
        if (qAbs(e->angleDelta().y()) >= qAbs(e->angleDelta().x())) {
            e->ignore();
            return;
        }
        QScrollArea::wheelEvent(e);
    }

    bool eventFilter(QObject *watched, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::ToolTip:
            showHoverCard(static_cast<QHelpEvent *>(e)->globalPos());
            return true;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
            hideHoverCard();
            break;
        default:
            break;
        }
        return QScrollArea::eventFilter(watched, e);
    }

private:
    QLabel *m_label = nullptr;
    ClaudeTranscriptView::Palette m_palette;
    std::function<QString()> m_hoverHtml;
    HoverCardHost m_hover{this, m_palette};
};

// A five-row terminal viewport must not make QLabel parse and shape megabytes
// of output that can never be visible. Keep the newest bounded tail on this
// surface; the complete stream remains in ToolCard::liveOutputText and in the
// Raw transcript surface.
QString terminalOutputTail(const QString &text)
{
    constexpr qsizetype kMaxChars = 20000;
    constexpr int kMaxLines = 32;
    if (text.size() <= kMaxChars &&
        text.count(QLatin1Char('\n')) < kMaxLines)
        return text;

    qsizetype start = qMax<qsizetype>(0, text.size() - kMaxChars);
    qsizetype lineStart = 0;
    qsizetype at = text.size();
    for (int lines = 0; lines < kMaxLines; ++lines) {
        at = text.lastIndexOf(QLatin1Char('\n'), at - 1);
        if (at < 0) {
            lineStart = 0;
            break;
        }
        lineStart = at + 1;
    }
    start = qMax(start, lineStart);
    if (start > 0 && start < text.size() &&
        text.at(start - 1) != QLatin1Char('\n')) {
        const qsizetype nextLine = text.indexOf(QLatin1Char('\n'), start);
        if (nextLine >= 0)
            start = nextLine + 1;
    }
    return QStringLiteral(
               "\u2026 earlier output omitted; open Raw for the full stream\n")
           + text.mid(start);
}

// A command's live stdout/stderr behaves like a small terminal: it stays a
// stable five-row viewport instead of making the transcript jump as a long
// build grows, and every streamed repaint follows the newest output. The plain
// label stays part of transcript search and supplies native selection, while
// the scroll area supplies wheel and scrollbar behaviour.
class TerminalOutputView : public QScrollArea
{
public:
    static constexpr int kVisibleLines = 5;

    TerminalOutputView(const QString &text, const QString &fg,
                       const QString &linkColour,
                       QWidget *parent = nullptr)
        : QScrollArea(parent), m_linkColour(linkColour)
    {
        setObjectName(QStringLiteral("agentCommandOutputTerminal"));
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(false);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setFont(monoFont());
        setStyleSheet(QStringLiteral(
            "QScrollArea{background:transparent;border:none;padding:0;}"
            "QScrollArea>QWidget>QWidget{background:transparent;}"
            "QScrollBar:vertical{width:8px;}"
            "QScrollBar:horizontal{height:8px;}"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_output = new CacheLabel;
        m_output->setObjectName(QStringLiteral("agentCommandOutputText"));
        m_output->setTextFormat(Qt::RichText);
        m_output->setWordWrap(false);
        m_output->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                          Qt::LinksAccessibleByMouse);
        m_output->setOpenExternalLinks(false);
        m_output->setFont(monoFont());
        m_output->setStyleSheet(Theme::remapSheet(
            QStringLiteral("color:%1;background:transparent;border:none;").arg(fg)));
        setWidget(m_output);
        m_output->installEventFilter(this);
        viewport()->installEventFilter(this);
        setMinimumHeight(heightForLines(1));
        setMaximumHeight(heightForLines(kVisibleLines));
        setOutputText(text);
    }

    QSize sizeHint() const override
    {
        QSize hint = QScrollArea::sizeHint();
        hint.setHeight(heightForLines(qBound(1, lineCount(), kVisibleLines)));
        return hint;
    }

    void setOutputText(const QString &text)
    {
        if (m_text != text) {
            m_text = text;
            const QString display = terminalOutputTail(text);
            if (m_displayText != display) {
                m_displayText = display;
                m_lineCount = m_displayText.isEmpty()
                                  ? 1
                                  : m_displayText.count(QLatin1Char('\n')) + 1;
                m_output->setText(
                    linkifiedTerminalHtml(m_displayText, m_linkColour, false));
                m_output->adjustSize();
            }
        }
        if (QScrollBar *bar = verticalScrollBar())
            bar->setValue(bar->maximum());
        QPointer<TerminalOutputView> self(this);
        QTimer::singleShot(0, this, [self] {
            if (self)
                self->verticalScrollBar()->setValue(
                    self->verticalScrollBar()->maximum());
        });
        updateGeometry();
    }

    QLabel *outputLabel() const { return m_output; }

protected:
    bool eventFilter(QObject *watched, QEvent *e) override
    {
        if (e->type() == QEvent::MouseButtonPress)
            m_engaged = true;
        return QScrollArea::eventFilter(watched, e);
    }

    void leaveEvent(QEvent *e) override
    {
        m_engaged = false;
        QScrollArea::leaveEvent(e);
    }

    void wheelEvent(QWheelEvent *e) override
    {
        if (!m_engaged) {
            e->ignore();
            return;
        }
        QScrollArea::wheelEvent(e);
    }

private:
    int lineCount() const { return m_lineCount; }

    int heightForLines(int lines) const
    {
        // A small extra pixel keeps the fifth row's descenders clear across
        // platform font renderers.
        return qCeil(lines * QFontMetricsF(font()).lineSpacing())
               + 2 * frameWidth() + 1;
    }

    CacheLabel *m_output = nullptr;
    QString m_text;
    QString m_displayText;
    int m_lineCount = 1;
    QString m_linkColour;
    bool m_engaged = false;
};

struct CommandIconSpec {
    QString path;
    QString fallback;
    int family = 0;
};

const CommandIconSpec &commandIconSpec(const QString &key)
{
    static const QHash<QString, CommandIconSpec> specs = [] {
        const auto oct = [](const char *name) {
            return QStringLiteral(":/icons/octicons/%1.svg").arg(QLatin1String(name));
        };
        const auto own = [](const char *name) {
            return QStringLiteral(":/icons/commands/%1.svg").arg(QLatin1String(name));
        };
        const auto brand = [](const char *name) {
            return QStringLiteral(":/icons/tree/file_type_%1.svg").arg(QLatin1String(name));
        };
        enum { Blue, Teal, Purple, Amber, Red, Orange, Green, Slate, Pink };
        QHash<QString, CommandIconSpec> h;
        const auto add = [&](const char *key, const QString &path, int family,
                             const QString &fallback = QString()) {
            h.insert(QLatin1String(key), {path, fallback, family});
        };
        add("search", oct("search"), Blue);
        add("find", own("find"), Blue);
        add("web", own("web"), Blue);
        add("network", oct("broadcast"), Blue);
        add("ssh", oct("server"), Blue);
        add("cloud", oct("cloud"), Blue);
        add("read", own("read"), Teal);
        add("head", own("head"), Teal);
        add("tail", own("tail"), Teal);
        add("folder", oct("file-directory"), Teal);
        add("cd", own("cd"), Teal);
        add("json", own("json"), Teal);
        add("replace", own("replace"), Purple);
        add("cut", own("cut"), Purple);
        add("count", own("count"), Purple);
        add("sort", oct("sort-desc"), Purple);
        add("diff", oct("diff"), Purple);
        add("echo", own("echo"), Purple);
        add("mkdir", own("mkdir"), Amber);
        add("copy", oct("copy"), Amber);
        add("move", own("move"), Amber);
        add("link", oct("link"), Amber);
        add("write", own("write"), Amber);
        add("chmod", oct("lock"), Amber);
        add("archive", own("archive"), Amber);
        add("package", oct("package"), Amber);
        add("key", oct("key"), Amber);
        add("delete", oct("trash"), Red);
        add("kill", oct("stop"), Red);
        add("build", own("build"), Orange);
        add("gh", oct("git-pull-request"), Orange);
        add("git", brand("git"), Orange, oct("git-branch"));
        add("cmake", brand("cmake"), Orange, own("build"));
        add("ninja", brand("ninja"), Orange, own("build"));
        add("cpp", brand("cpp"), Orange, own("build"));
        add("c", brand("c"), Orange, own("build"));
        add("test", own("test"), Green);
        add("python", brand("python"), Blue, oct("code"));
        add("node", brand("node"), Green, oct("code"));
        add("npm", brand("npm"), Red, oct("package"));
        add("flutter", brand("flutter"), Blue, oct("device-mobile"));
        add("dart", brand("dartlang"), Blue, oct("code"));
        add("swift", brand("swift"), Orange, oct("code"));
        add("xcode", brand("xcode"), Blue, own("build"));
        add("go", brand("go"), Teal, oct("code"));
        add("rust", brand("rust"), Orange, oct("code"));
        add("cargo", brand("cargo"), Orange, oct("package"));
        add("gradle", brand("gradle"), Teal, own("build"));
        add("java", brand("java"), Orange, oct("code"));
        add("kotlin", brand("kotlin"), Purple, oct("code"));
        add("ruby", brand("ruby"), Red, oct("code"));
        add("docker", brand("docker"), Blue, oct("package"));
        add("sqlite", brand("sqlite"), Blue, oct("database"));
        add("claude", brand("claude"), Pink, oct("sparkle"));
        add("database", oct("database"), Slate);
        add("service", oct("gear"), Slate);
        add("logs", own("logs"), Slate);
        add("process", own("process"), Slate);
        add("disk", own("disk"), Slate);
        add("clock", oct("clock"), Slate);
        add("date", oct("calendar"), Slate);
        add("image", own("image"), Pink);
        add("phone", oct("device-mobile"), Slate);
        add("ai", oct("smart-toy"), Pink);
        add("open", QStringLiteral(":/icons/codicons/link-external.svg"), Slate);
        add("terminal", oct("terminal"), Slate);
        return h;
    }();
    const auto it = specs.constFind(key);
    return it != specs.constEnd() ? it.value()
                                  : specs.constFind(QStringLiteral("terminal")).value();
}

QColor commandIconTint(int family, bool dark)
{
    static const char *const tints[][2] = {
        {"#58a6ff", "#0969da"}, {"#39c5bb", "#1a7f72"}, {"#bc8cff", "#8250df"},
        {"#d29922", "#9a6700"}, {"#ff7b72", "#cf222e"}, {"#f0883e", "#bc4c00"},
        {"#56d364", "#1a7f37"}, {"#9198a1", "#59636e"}, {"#f778ba", "#bf3989"},
    };
    const int index = qBound(0, family, int(std::size(tints)) - 1);
    return QColor(QLatin1String(tints[index][dark ? 0 : 1]));
}

bool commandIconIsBrand(const QString &key)
{
    const CommandIconSpec &spec = commandIconSpec(key);
    return spec.path.startsWith(QLatin1String(":/icons/tree/"))
           && QFile::exists(spec.path);
}

QPixmap commandIconPixmap(const QString &key, const QColor &tint, int px, qreal dpr)
{
    static QHash<QString, QPixmap> cache;
    const QString cacheKey = key + QLatin1Char('|') + QString::number(tint.rgba(), 16)
                             + QLatin1Char('|') + QString::number(px)
                             + QLatin1Char('|') + QString::number(dpr);
    const auto cached = cache.constFind(cacheKey);
    if (cached != cache.constEnd())
        return cached.value();
    const CommandIconSpec &spec = commandIconSpec(key);
    const bool brand = commandIconIsBrand(key);
    QString path = spec.path;
    if (!QFile::exists(path))
        path = spec.fallback;
    if (!QFile::exists(path))
        path = QStringLiteral(":/icons/octicons/terminal.svg");
    QPixmap pixmap(QSize(px, px) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QSvgRenderer renderer(path);
    if (renderer.isValid()) {
        QPainter g(&pixmap);
        g.setRenderHint(QPainter::Antialiasing);
        g.setRenderHint(QPainter::SmoothPixmapTransform);
        renderer.render(&g, QRectF(0, 0, px, px));
        if (!brand) {
            g.setCompositionMode(QPainter::CompositionMode_SourceIn);
            g.fillRect(QRectF(0, 0, px, px), tint);
        }
    }
    cache.insert(cacheKey, pixmap);
    return pixmap;
}

QString commandLineIconHtml(const QString &key,
                            const ClaudeTranscriptView::Palette &p)
{
    static QHash<QString, QString> cache;
    const bool dark = QColor(p.canvas).lightness() < 128;
    const QColor tint = key.isEmpty()
        ? QColor(Qt::transparent)
        : commandIconTint(commandIconSpec(key).family, dark);
    const QString cacheKey = key + QLatin1Char('|') + QString::number(tint.rgba(), 16);
    const auto cached = cache.constFind(cacheKey);
    if (cached != cache.constEnd())
        return cached.value();
    constexpr qreal kDpr = 2.0;
    QImage image;
    if (key.isEmpty()) {
        image = QImage(QSize(kHoverLineIconPx, kHoverLineIconPx) * kDpr,
                       QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
    } else {
        image = commandIconPixmap(key, tint, kHoverLineIconPx, kDpr).toImage();
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    const QString html =
        QStringLiteral("<img src='data:image/png;base64,%1' width='%2' height='%2'>")
            .arg(QString::fromLatin1(png.toBase64()))
            .arg(kHoverLineIconPx);
    cache.insert(cacheKey, html);
    return html;
}

class CommandHeader : public QWidget
{
public:
    static constexpr int kBadge = 20;
    static constexpr int kStep = 23;
    static constexpr int kMaxBadges = 10;
    static constexpr int kIconPx = 14;
    static constexpr int kChevronW = 18;
    static constexpr int kStatusW = 20;
    static constexpr int kLead = 3;

    CommandHeader(const QVector<forkmesh::transcripttools::CommandIcon> &icons,
                  const QString &title, const QString &command,
                  const ClaudeTranscriptView::Palette &p,
                  QWidget *parent = nullptr)
        : QWidget(parent), m_icons(icons), m_title(title), m_command(command), m_p(p)
    {
        setObjectName(QStringLiteral("agentCommandSummary"));
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setFont(monoFont());
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setFixedHeight(qMax(kBadge + 6, QFontMetrics(font()).height() + 10));
        setAccessibleName(QStringLiteral("Command: %1").arg(m_title));
        setAccessibleDescription(QStringLiteral("Press to show the command and its output"));
    }

    std::function<void()> onToggle;
    std::function<void()> onApproval;
    std::function<QString()> hoverCommand;
    std::function<void()> onStateChange;

    void setStatusDot(bool on)
    {
        if (m_statusDot == on)
            return;
        m_statusDot = on;
        update();
    }
    void markSettled()
    {
        m_settled = true;
        update();
        if (onStateChange)
            onStateChange();
    }
    bool runningFlag() const { return m_pulse && !m_pulse->isHidden(); }
    bool failedFlag() const { return !m_outcome.isEmpty(); }

    CommandHoverCard *hoverCard() const { return m_hover.visibleCard(); }

    QString title() const { return m_title; }
    QStringList iconKeys() const
    {
        QStringList keys;
        for (const auto &icon : m_icons)
            keys << icon.key;
        return keys;
    }
    bool expanded() const { return m_expanded; }
    ClaudeTranscriptView::CommandApproval::State approvalState() const
    {
        return m_approvalState;
    }
    QString outcome() const { return m_outcome; }
    QString markers() const
    {
        QStringList parts;
        if (!m_lines.isEmpty())
            parts << m_lines;
        if (!m_elapsed.isEmpty())
            parts << m_elapsed;
        return parts.join(QStringLiteral(" · "));
    }
    bool running() const { return m_pulse && m_pulse->isVisible(); }

    void setExpanded(bool on)
    {
        if (m_expanded == on)
            return;
        m_expanded = on;
        update();
    }

    void setApproval(ClaudeTranscriptView::CommandApproval::State state,
                     const QColor &color, const QString &tip)
    {
        m_approvalState = state;
        m_approvalColor = color;
        m_approvalTip = tip;
        update();
    }

    void setRunning(bool on)
    {
        if (on && !m_pulse) {
            m_pulse = new PulseDot(m_p.accent, this);
            placePulse();
        }
        if (!m_pulse)
            return;
        m_pulse->setVisible(on);
        m_pulse->setRunning(on);
        update();
        if (onStateChange)
            onStateChange();
    }

    void setOutcome(const QString &text)
    {
        m_outcome = text;
        update();
        if (onStateChange)
            onStateChange();
    }

    void setLineCount(int lines)
    {
        m_lines = lines <= 0 ? QString()
                  : lines == 1 ? QStringLiteral("1 line")
                               : QStringLiteral("%1 lines")
                                     .arg(QLocale().toString(lines));
        describe();
    }

    void setElapsed(qint64 ms)
    {
        if (ms < 1000)
            m_elapsed.clear();
        else if (ms < 60000)
            m_elapsed = QStringLiteral("%1s").arg(ms / 1000.0, 0, 'f', 1);
        else if (ms < 3600000)
            m_elapsed = QStringLiteral("%1m %2s")
                            .arg(ms / 60000)
                            .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'));
        else
            m_elapsed = QStringLiteral("%1h %2m")
                            .arg(ms / 3600000)
                            .arg((ms / 60000) % 60, 2, 10, QLatin1Char('0'));
        describe();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        const bool dark = QColor(m_p.canvas).lightness() < 128;
        const bool hot = underMouse() || (hasFocus() && m_keyFocus);
        const QColor rowBg = hot ? QColor(m_p.chipBg) : QColor(m_p.canvas);
        if (hot) {
            g.setPen(Qt::NoPen);
            g.setBrush(rowBg);
            g.drawRoundedRect(QRectF(rect()).adjusted(0, 1, 0, -1), 7, 7);
        }

        const QPointF tip = QRectF(chevronRect()).center();
        QPen chevron(QColor(hot ? m_p.text : m_p.muted), 1.6, Qt::SolidLine,
                     Qt::RoundCap, Qt::RoundJoin);
        g.setPen(chevron);
        g.setBrush(Qt::NoBrush);
        QPainterPath arrow;
        if (m_expanded) {
            arrow.moveTo(tip.x() - 3.5, tip.y() + 1.75);
            arrow.lineTo(tip.x(), tip.y() - 1.75);
            arrow.lineTo(tip.x() + 3.5, tip.y() + 1.75);
        } else {
            arrow.moveTo(tip.x() - 3.5, tip.y() - 1.75);
            arrow.lineTo(tip.x(), tip.y() + 1.75);
            arrow.lineTo(tip.x() + 3.5, tip.y() - 1.75);
        }
        g.drawPath(arrow);

        if (m_statusDot) {
            const QColor dot = !m_outcome.isEmpty() ? QColor(m_p.del)
                               : runningFlag()      ? QColor(m_p.accent)
                               : m_settled          ? QColor(m_p.add)
                                                    : QColor(m_p.muted);
            g.setPen(Qt::NoPen);
            g.setBrush(dot);
            g.drawEllipse(QPointF(kLead + 4.0, height() / 2.0), 3.0, 3.0);
        }

        const int shown = shownBadges();
        const bool overflow = m_icons.size() > shown;
        const qreal dpr = devicePixelRatioF();
        const auto plate = [&](int i, const QColor &tint, bool brand) {
            const QRectF r = QRectF(badgeRect(i)).adjusted(0.5, 0.5, -0.5, -0.5);
            const QColor base(m_p.termBg);
            QColor fill = base;
            QColor edge(m_p.termEdge);
            if (!brand) {
                const qreal mix = dark ? 0.16 : 0.11;
                fill = QColor::fromRgbF(base.redF() * (1 - mix) + tint.redF() * mix,
                                        base.greenF() * (1 - mix) + tint.greenF() * mix,
                                        base.blueF() * (1 - mix) + tint.blueF() * mix);
                edge = tint;
                edge.setAlphaF(i == m_hoverBadge ? 0.85f : 0.38f);
            } else if (i == m_hoverBadge) {
                edge = QColor(m_p.muted);
            }
            g.setPen(QPen(edge, 1.0));
            g.setBrush(fill);
            g.drawRoundedRect(r, 5.5, 5.5);
            return r;
        };
        for (int i = 0; i < shown; ++i) {
            const QString &key = m_icons.at(i).key;
            const QColor tint = commandIconTint(commandIconSpec(key).family, dark);
            const bool brand = commandIconIsBrand(key);
            const QRectF r = plate(i, tint, brand);
            const int px = brand ? kIconPx : kIconPx - 1;
            const QPixmap icon = commandIconPixmap(key, tint, px, dpr);
            g.drawPixmap(QPointF(std::round(r.center().x() - px / 2.0),
                                 std::round(r.center().y() - px / 2.0)),
                         icon);
        }
        if (overflow) {
            const QRectF r = plate(shown, QColor(m_p.muted), false);
            QFont small = font();
            small.setPixelSize(10);
            small.setBold(true);
            g.setFont(small);
            g.setPen(QColor(m_p.muted));
            g.drawText(r, Qt::AlignCenter,
                       QStringLiteral("+%1").arg(m_icons.size() - shown));
            g.setFont(font());
        }

        const QRect text = textRect();
        const QFontMetrics fm(font());
        if (text.width() > 8) {
            g.setPen(QColor(m_p.text));
            g.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                       fm.elidedText(m_title, Qt::ElideRight, text.width()));
        }

        if (!m_outcome.isEmpty()) {
            g.setPen(QColor(m_p.del));
            g.drawText(outcomeRect(), Qt::AlignRight | Qt::AlignVCenter, m_outcome);
        }
        if (!markers().isEmpty()) {
            g.setPen(QColor(m_p.muted));
            g.drawText(markersRect(), Qt::AlignRight | Qt::AlignVCenter, markers());
        }

        const QRectF status = statusRect();
        const QPointF c = status.center();
        QPen mark(m_approvalColor, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        switch (m_approvalState) {
        case ClaudeTranscriptView::CommandApproval::Allowed: {
            g.setPen(mark);
            QPainterPath tick;
            tick.moveTo(c.x() - 4, c.y() + 0.2);
            tick.lineTo(c.x() - 1.2, c.y() + 3);
            tick.lineTo(c.x() + 4.2, c.y() - 3.4);
            g.drawPath(tick);
            break;
        }
        case ClaudeTranscriptView::CommandApproval::Asks: {
            QFont bold = font();
            bold.setBold(true);
            g.setFont(bold);
            g.setPen(m_approvalColor);
            g.drawText(status, Qt::AlignCenter, QStringLiteral("?"));
            g.setFont(font());
            break;
        }
        case ClaudeTranscriptView::CommandApproval::Blocked:
        case ClaudeTranscriptView::CommandApproval::Denied:
            g.setPen(mark);
            g.drawLine(QPointF(c.x() - 3.5, c.y() - 3.5), QPointF(c.x() + 3.5, c.y() + 3.5));
            g.drawLine(QPointF(c.x() + 3.5, c.y() - 3.5), QPointF(c.x() - 3.5, c.y() + 3.5));
            break;
        case ClaudeTranscriptView::CommandApproval::Unknown:
            break;
        }
    }

    bool event(QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::ToolTip: {
            const auto *help = static_cast<QHelpEvent *>(e);
            if (!showHoverAt(help->pos(), help->globalPos())) {
                m_hover.hide();
                e->ignore();
            }
            return true;
        }
        case QEvent::Hide:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
            m_hover.hide();
            break;
        default:
            break;
        }
        return QWidget::event(e);
    }

    void enterEvent(QEnterEvent *e) override
    {
        update();
        QWidget::enterEvent(e);
    }

    void leaveEvent(QEvent *e) override
    {
        m_hoverBadge = -1;
        update();
        QWidget::leaveEvent(e);
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int badge = badgeAt(e->position().toPoint());
        if (badge != m_hoverBadge) {
            m_hoverBadge = badge;
            update();
        }
        QWidget::mouseMoveEvent(e);
    }

    void focusInEvent(QFocusEvent *e) override
    {
        m_keyFocus = e->reason() == Qt::TabFocusReason
                     || e->reason() == Qt::BacktabFocusReason;
        update();
        QWidget::focusInEvent(e);
    }

    void focusOutEvent(QFocusEvent *e) override
    {
        update();
        QWidget::focusOutEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || !rect().contains(e->position().toPoint())) {
            QWidget::mouseReleaseEvent(e);
            return;
        }
        if (approvalHit(e->position().toPoint()) && onApproval)
            onApproval();
        else if (onToggle)
            onToggle();
        e->accept();
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if ((e->key() == Qt::Key_Space || e->key() == Qt::Key_Return
             || e->key() == Qt::Key_Enter) && onToggle) {
            onToggle();
            e->accept();
            return;
        }
        QWidget::keyPressEvent(e);
    }

    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        placePulse();
    }

private:
    void describe()
    {
        const QString press = QStringLiteral("Press to show the command and its output");
        setAccessibleDescription(markers().isEmpty()
                                     ? press
                                     : QStringLiteral("%1. %2").arg(markers(), press));
        update();
    }

    int dotLead() const { return m_statusDot ? 14 : 0; }

    int shownBadges() const
    {
        return qMin(int(m_icons.size()), kMaxBadges);
    }

    int badgeSlots() const
    {
        return shownBadges() + (m_icons.size() > shownBadges() ? 1 : 0);
    }

    QRect badgeRect(int i) const
    {
        return QRect(kLead + dotLead() + i * kStep, (height() - kBadge) / 2, kBadge,
                     kBadge);
    }

    QRect chevronRect() const
    {
        return QRect(width() - kChevronW - 2, 0, kChevronW, height());
    }

    QRect statusRect() const
    {
        return QRect(chevronRect().left() - kStatusW, 0, kStatusW, height());
    }

    QRect markersRect() const
    {
        const QString text = markers();
        const int w = text.isEmpty() ? 0 : QFontMetrics(font()).horizontalAdvance(text);
        const int right = m_outcome.isEmpty() ? statusRect().left() - 4
                                              : outcomeRect().left() - 8;
        return QRect(right - w, 0, w, height());
    }

    QRect outcomeRect() const
    {
        const int w = m_outcome.isEmpty()
                          ? 0
                          : QFontMetrics(font()).horizontalAdvance(m_outcome);
        const int right = statusRect().left() - 4;
        return QRect(right - w, 0, w, height());
    }

    QRect textRect() const
    {
        const int badgeCount = badgeSlots();
        const int left = kLead + dotLead() +
                         (badgeCount ? kBadge + (badgeCount - 1) * kStep + 8 : 2);
        int right = statusRect().left() - 6;
        if (!m_outcome.isEmpty())
            right = outcomeRect().left() - 8;
        if (!markers().isEmpty())
            right = markersRect().left() - 8;
        if (m_pulse && m_pulse->isVisible())
            right = qMin(right, m_pulse->x() - 6);
        return QRect(left, 0, qMax(0, right - left), height());
    }

    void placePulse()
    {
        if (!m_pulse)
            return;
        const QRect status = statusRect();
        m_pulse->move(status.left() - m_pulse->width() - 2,
                      (height() - m_pulse->height()) / 2);
    }

    bool approvalHit(const QPoint &pos) const
    {
        return m_approvalState != ClaudeTranscriptView::CommandApproval::Unknown
               && statusRect().adjusted(-4, 0, 2, 0).contains(pos);
    }

    int badgeAt(const QPoint &pos) const
    {
        for (int i = 0; i < badgeSlots(); ++i)
            if (badgeRect(i).contains(pos))
                return i;
        return -1;
    }

    QString badgeHoverHtml(int from, int to) const
    {
        QStringList rows;
        for (int i = from; i < to && i < m_icons.size(); ++i) {
            const auto &icon = m_icons.at(i);
            rows << QStringLiteral(
                        "<span style='color:%1;font-weight:600;'>%2</span>"
                        "&nbsp;&nbsp;<span style='color:%3;font-family:\"%4\";'>%5</span>")
                        .arg(m_p.text,
                             forkmesh::transcripttools::commandIconLabel(icon.key)
                                 .toHtmlEscaped(),
                             m_p.muted, monoFont().family().toHtmlEscaped(),
                             icon.programs.join(QStringLiteral(", ")).toHtmlEscaped());
        }
        return QStringLiteral("<div style='white-space:pre;'>%1</div>")
            .arg(rows.join(QStringLiteral("<br>")));
    }

    bool showHoverAt(const QPoint &pos, const QPoint &globalPos)
    {
        if (approvalHit(pos) && !m_approvalTip.isEmpty()) {
            const QString tip = m_approvalTip;
            m_hover.show(QStringLiteral("approval|") + tip, statusRect().adjusted(-4, 0, 2, 0),
                         globalPos, false, [this, tip] {
                             return QStringLiteral("<div style='color:%1;'>%2</div>")
                                 .arg(m_p.text, tip.toHtmlEscaped().replace(
                                                    QLatin1Char('\n'),
                                                    QStringLiteral("<br>")));
                         });
            return true;
        }
        const int badge = badgeAt(pos);
        if (badge >= 0) {
            const int shown = shownBadges();
            const bool rest = badge >= shown;
            m_hover.show(QStringLiteral("badge|%1").arg(badge), badgeRect(badge), globalPos,
                         false, [this, badge, rest, shown] {
                             return rest ? badgeHoverHtml(shown, int(m_icons.size()))
                                         : badgeHoverHtml(badge, badge + 1);
                         });
            return true;
        }
        if (textRect().contains(pos)) {
            m_hover.show(QStringLiteral("command"), textRect(), globalPos, true,
                         hoverCommand ? hoverCommand
                                      : std::function<QString()>([this] {
                                            return QStringLiteral(
                                                       "<div style='white-space:pre-wrap;'>"
                                                       "%1</div>")
                                                .arg(m_command.toHtmlEscaped());
                                        }));
            return true;
        }
        return false;
    }

    QVector<forkmesh::transcripttools::CommandIcon> m_icons;
    QString m_title;
    QString m_command;
    ClaudeTranscriptView::Palette m_p;
    bool m_expanded = false;
    bool m_keyFocus = false;
    ClaudeTranscriptView::CommandApproval::State m_approvalState =
        ClaudeTranscriptView::CommandApproval::Unknown;
    QColor m_approvalColor;
    QString m_approvalTip;
    QString m_outcome;
    QString m_lines;
    QString m_elapsed;
    PulseDot *m_pulse = nullptr;
    int m_hoverBadge = -1;
    bool m_statusDot = false;
    bool m_settled = false;
    HoverCardHost m_hover{this, m_p};
};

class CommandCard : public QFrame
{
public:
    CommandCard(CommandHeader *header, QWidget *body, QWidget *parent = nullptr)
        : QFrame(parent), m_header(header)
    {
        setObjectName(QStringLiteral("agentCommandCard"));
        setStyleSheet(QStringLiteral(
            "QFrame#agentCommandCard{background:transparent;border:none;}"));
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);
        header->setParent(this);
        v->addWidget(header);
        m_body = new QWidget(this);
        m_body->setObjectName(QStringLiteral("agentCommandBody"));
        m_body->setMinimumWidth(0);
        m_body->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_body->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        m_bodyLayout = new QVBoxLayout(m_body);
        m_bodyLayout->setContentsMargins(0, 0, 0, 2);
        m_bodyLayout->setSpacing(4);
        m_bodyLayout->addWidget(body);
        m_body->setVisible(false);
        v->addWidget(m_body);
        header->onToggle = [this] { setExpanded(!m_open); };
    }

    QVBoxLayout *bodyLayout() const { return m_bodyLayout; }
    CommandHeader *header() const { return m_header; }
    void setBodyInset(int left) { m_bodyLayout->setContentsMargins(left, 0, 0, 2); }
    bool isExpanded() const { return m_open; }

    void setExpanded(bool on)
    {
        if (on == m_open)
            return;
        m_open = on;
        m_header->setExpanded(on);
        if (!m_anim) {
            m_anim = new QPropertyAnimation(m_body, "maximumHeight", this);
            QObject::connect(m_anim, &QPropertyAnimation::finished, this, [this] {
                if (m_open)
                    m_body->setMaximumHeight(QWIDGETSIZE_MAX);
                else
                    m_body->setVisible(false);
            });
        }
        m_anim->stop();
        if (m_open) {
            m_body->setMaximumHeight(0);
            m_body->setVisible(true);
            followTerminals();
            m_anim->setDuration(170);
            m_anim->setEasingCurve(QEasingCurve::OutCubic);
            m_anim->setStartValue(0);
            m_anim->setEndValue(qMax(0, m_body->sizeHint().height()));
        } else {
            m_anim->setDuration(150);
            m_anim->setEasingCurve(QEasingCurve::InCubic);
            m_anim->setStartValue(m_body->height());
            m_anim->setEndValue(0);
        }
        m_anim->start();
    }

private:
    void followTerminals()
    {
        for (QScrollArea *terminal : m_body->findChildren<QScrollArea *>(
                 QStringLiteral("agentCommandOutputTerminal"))) {
            QPointer<QScrollArea> guard(terminal);
            QTimer::singleShot(0, terminal, [guard] {
                if (guard)
                    guard->verticalScrollBar()->setValue(
                        guard->verticalScrollBar()->maximum());
            });
        }
    }

    CommandHeader *m_header = nullptr;
    QWidget *m_body = nullptr;
    QVBoxLayout *m_bodyLayout = nullptr;
    bool m_open = false;
    QPropertyAnimation *m_anim = nullptr;
};

class EditChevron : public QWidget
{
public:
    EditChevron(const ClaudeTranscriptView::Palette &p, QWidget *parent)
        : QWidget(parent), m_p(p)
    {
        setFixedSize(18, 18);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void setExpanded(bool on)
    {
        m_open = on;
        update();
    }
    void setHot(bool on)
    {
        m_hot = on;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        const QPointF tip = QRectF(rect()).center();
        g.setPen(QPen(QColor(m_hot ? m_p.text : m_p.muted), 1.6, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        g.setBrush(Qt::NoBrush);
        QPainterPath arrow;
        const qreal dy = m_open ? -1.75 : 1.75;
        arrow.moveTo(tip.x() - 3.5, tip.y() - dy);
        arrow.lineTo(tip.x(), tip.y() + dy);
        arrow.lineTo(tip.x() + 3.5, tip.y() - dy);
        g.drawPath(arrow);
    }

private:
    ClaudeTranscriptView::Palette m_p;
    bool m_open = false;
    bool m_hot = false;
};

class FileEditCard : public QFrame
{
public:
    FileEditCard(QWidget *headerLabel, QWidget *body,
                 const ClaudeTranscriptView::Palette &p, QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("agentFileEditCard"));
        setStyleSheet(QStringLiteral(
            "QFrame#agentFileEditCard{background:transparent;border:none;}"));
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);
        m_head = new QWidget(this);
        m_head->setObjectName(QStringLiteral("agentFileEditSummary"));
        m_head->setCursor(Qt::PointingHandCursor);
        m_head->setAttribute(Qt::WA_Hover);
        m_head->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        m_head->setAccessibleDescription(QStringLiteral("Press to show the change"));
        auto *h = new QHBoxLayout(m_head);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        headerLabel->setParent(m_head);
        h->addWidget(headerLabel, 1);
        m_chevron = new EditChevron(p, m_head);
        h->addWidget(m_chevron, 0, Qt::AlignVCenter);
        v->addWidget(m_head);
        m_body = body;
        m_body->setParent(this);
        m_body->setVisible(false);
        v->addWidget(m_body);
        m_head->installEventFilter(this);
        headerLabel->installEventFilter(this);
        if (auto *label = qobject_cast<QLabel *>(headerLabel))
            QObject::connect(label, &QLabel::linkHovered, this,
                             [this](const QString &link) { m_hoverLink = link; });
    }

    bool isExpanded() const { return m_open; }
    void setExpanded(bool on)
    {
        if (on == m_open)
            return;
        m_open = on;
        m_chevron->setExpanded(on);
        m_body->setVisible(on);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Enter:
            m_chevron->setHot(true);
            break;
        case QEvent::Leave:
            if (watched == m_head)
                m_chevron->setHot(false);
            break;
        case QEvent::MouseButtonRelease: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() != Qt::LeftButton || !m_hoverLink.isEmpty())
                break;
            if (watched == m_head &&
                qobject_cast<QLabel *>(m_head->childAt(mouse->position().toPoint())))
                break;
            auto *label = qobject_cast<QLabel *>(watched);
            if (label && label->hasSelectedText())
                break;
            setExpanded(!m_open);
            break;
        }
        default:
            break;
        }
        return QFrame::eventFilter(watched, event);
    }

private:
    QWidget *m_head = nullptr;
    QWidget *m_body = nullptr;
    EditChevron *m_chevron = nullptr;
    QString m_hoverLink;
    bool m_open = false;
};

// The speaker's picture as a circle with a faint ring. A missing picture falls
// back to one letter.
void paintSpeakerPortrait(QPainter &g, const QRectF &box,
                          const ClaudeTranscriptView::RowStyle &style)
{
    QPainterPath clip;
    clip.addEllipse(box);
    g.save();
    g.setClipPath(clip);
    g.setPen(Qt::NoPen);
    g.setBrush(QColor(style.plate));
    g.drawRect(box);
    if (!style.avatar.isNull())
        g.drawPixmap(box, style.avatar, QRectF(style.avatar.rect()));
    else {
        g.setFont(monoBold13Font());
        g.setPen(QColor(style.nodeColor));
        g.drawText(box, Qt::AlignCenter, style.monogram);
    }
    g.restore();
    if (!style.avatarRing.isEmpty()) {
        QColor ring(style.avatarRing);
        ring.setAlphaF(0.55f);
        g.setBrush(Qt::NoBrush);
        g.setPen(QPen(ring, 1.0));
        g.drawEllipse(box.adjusted(0.5, 0.5, -0.5, -0.5));
    }
}

class SpeakerPortrait : public QWidget
{
public:
    explicit SpeakerPortrait(const ClaudeTranscriptView::RowStyle &style,
                             QWidget *parent = nullptr)
        : QWidget(parent), m_style(style)
    {
        setFixedSize(CommandHeader::kBadge, CommandHeader::kBadge);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        g.setRenderHint(QPainter::SmoothPixmapTransform);
        paintSpeakerPortrait(g, QRectF(rect()), m_style);
    }

private:
    ClaudeTranscriptView::RowStyle m_style;
};

// One transcript row: a fixed gutter carrying a machine row's small glyph beside
// a bare content column that runs to the right edge.
class RailItem : public QWidget
{
public:
    static constexpr int kAvatarPx = 26;
    static constexpr int kAvatarX = 2;
    static constexpr int kRailW = kAvatarX + kAvatarPx + 8;
    static constexpr int kGap = 12;         // vertical space between items
    static constexpr int kNodeY = kGap + 8; // glyph aligned to the first text line
    static constexpr int kCommandRailW =
        kAvatarX + kAvatarPx / 2 - CommandHeader::kLead - CommandHeader::kBadge / 2;
    static constexpr int kSpeakerRailW = kCommandRailW + CommandHeader::kLead;

    QWidget *content() const { return m_content; }

    RailItem(QWidget *content, const ClaudeTranscriptView::RowStyle &style,
             std::function<void()> onActivate = {}, QWidget *parent = nullptr)
        : QWidget(parent), m_style(style), m_onActivate(std::move(onActivate))
    {
        setAttribute(Qt::WA_StyledBackground, false);
        auto *h = new QHBoxLayout(this);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        auto *command = dynamic_cast<CommandCard *>(content);
        const bool speaker = !style.avatar.isNull() || !style.monogram.isEmpty();
        const int railW = command ? kCommandRailW : speaker ? kSpeakerRailW : kRailW;
        m_glyph = !command && !speaker;
        if (command)
            command->setBodyInset(kRailW - railW);
        auto *rail = new QWidget(this);
        rail->setFixedWidth(railW);
        if (m_onActivate) {
            rail->setCursor(Qt::PointingHandCursor);
            rail->setToolTip(QStringLiteral("Show this line in the raw log"));
        } else {
            rail->setAttribute(Qt::WA_TransparentForMouseEvents);
        }
        rail->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
        h->addWidget(rail);
        auto *holder = new QWidget(this);
        // A transcript is often shown beside the session list or on a narrow
        // laptop screen. Do not let a long command/path become this row's
        // minimum width: the content labels can reflow, so give the layout
        // permission to make them narrower and ask them for a taller height.
        holder->setMinimumWidth(0);
        holder->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        holder->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
        auto *hv = new QVBoxLayout(holder);
        hv->setContentsMargins(0, kGap, 0, 0);
        hv->setSpacing(0);
        m_content = content;
        content->setParent(holder);
        content->setMinimumWidth(0);
        content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        hv->addWidget(content);
        h->addWidget(holder, 1);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        if (m_glyph)
            paintGlyph(g);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (!m_onActivate || e->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(e);
            return;
        }
        m_pressAt = e->globalPosition().toPoint();
        m_pressed = true;
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        const bool click =
            m_pressed && e->button() == Qt::LeftButton &&
            (e->globalPosition().toPoint() - m_pressAt).manhattanLength() <
                QApplication::startDragDistance();
        m_pressed = false;
        if (!click) {
            QWidget::mouseReleaseEvent(e);
            return;
        }
        e->accept();
        m_onActivate();
    }

private:
    void paintGlyph(QPainter &g)
    {
        const double cx = kAvatarX + kAvatarPx / 2.0;
        switch (m_style.glyph) {
        case ClaudeTranscriptView::GlyphDot:
            // A ringed node rather than a filled blob: it stays legible at the
            // small size the gutter allows and reads as a step on a track.
            g.setPen(Qt::NoPen);
            g.setBrush(QColor(m_style.nodeColor));
            g.drawEllipse(QPointF(cx, m_nodeY), 3.0, 3.0);
            {
                QColor ring(m_style.nodeColor);
                ring.setAlphaF(0.35f);
                g.setBrush(Qt::NoBrush);
                g.setPen(QPen(ring, 1.0));
                g.drawEllipse(QPointF(cx, m_nodeY), 5.5, 5.5);
            }
            break;
        case ClaudeTranscriptView::GlyphChevron:
        case ClaudeTranscriptView::GlyphStar: {
            g.setFont(monoBoldFont());
            g.setPen(QColor(m_style.nodeColor));
            g.drawText(QRectF(0, kGap, kRailW, 18), Qt::AlignCenter,
                       m_style.glyph == ClaudeTranscriptView::GlyphChevron
                           ? QStringLiteral("❯")
                           : QStringLiteral("✻"));
            break;
        }
        case ClaudeTranscriptView::GlyphNone:
            break;
        }
    }

    QPointer<QWidget> m_content;
    ClaudeTranscriptView::RowStyle m_style;
    std::function<void()> m_onActivate;
    QPoint m_pressAt;
    bool m_pressed = false;
    qreal m_nodeY = kNodeY;
    bool m_glyph = true;
};

// A rounded panel with a hairline edge, used for terminal output and code. Drawn
// rather than stylesheet-ed so the fill can carry the same faint top lift the
// bubbles do, and so a child label's own transparent background shows it.
class TranscriptPanel : public QWidget
{
public:
    TranscriptPanel(const QString &bg, const QString &edge, int radius = 8,
                    QWidget *parent = nullptr)
        : QWidget(parent), m_bg(bg), m_edge(edge), m_radius(radius)
    {
        setAttribute(Qt::WA_StyledBackground, false);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        const QRectF box(0.5, 0.5, width() - 1.0, height() - 1.0);
        if (box.width() <= 0 || box.height() <= 0)
            return;
        const QColor bg(m_bg);
        QLinearGradient lift(box.topLeft(), box.bottomLeft());
        lift.setColorAt(0.0, bg.lighter(105));
        lift.setColorAt(qMin(1.0, 60.0 / qMax(1.0, box.height())), bg);
        g.setPen(Qt::NoPen);
        g.setBrush(lift);
        g.drawRoundedRect(box, m_radius, m_radius);
        g.setBrush(Qt::NoBrush);
        g.setPen(QPen(QColor(m_edge), 1.0));
        g.drawRoundedRect(box, m_radius, m_radius);
    }

private:
    QString m_bg, m_edge;
    int m_radius;
};

// The thin vertical rule that ties a tool's output to its header. It replaces
// the CLI's ⎿ glyph: a rule scales with however tall the output turns out to be,
// where a single character always sat orphaned at the top of it.
class ConnectorRule : public QWidget
{
public:
    explicit ConnectorRule(const QString &color, QWidget *parent = nullptr)
        : QWidget(parent), m_color(color)
    {
        setFixedWidth(7);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        // Ignored vertically: the rule stretches to whatever the content beside
        // it turns out to need, and never asks the row to be taller than that.
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        QColor c(m_color);
        c.setAlphaF(0.5f);
        g.setPen(Qt::NoPen);
        g.setBrush(c);
        g.drawRoundedRect(QRectF(3, 2, 1.4, qMax(0, height() - 4)), 0.7, 0.7);
    }

private:
    QString m_color;
};

QImage decodeScaledAttachment(const QString &path, int cap)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize src = reader.size(); // header only, no pixel decode
    if (src.isValid() && src.width() > cap)
        reader.setScaledSize(
            QSize(cap, qMax(1, qRound(double(cap) * src.height() / src.width()))));
    QImage img = reader.read();
    if (!img.isNull() && img.width() > cap)
        img = img.scaledToWidth(cap, Qt::SmoothTransformation);
    return img;
}

class ThumbImage : public QWidget
{
public:
    ThumbImage(const QString &path, const QString &border, QWidget *parent = nullptr)
        : QWidget(parent), m_path(path), m_border(border)
    {
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Click to expand"));
        m_srcSize = QImageReader(path).size(); // header read; reserves the box
        applyScale();
    }

protected:
    void mousePressEvent(QMouseEvent *) override
    {
        m_expanded = !m_expanded;
        setToolTip(m_expanded ? QStringLiteral("Click to shrink")
                              : QStringLiteral("Click to expand"));
        applyScale();
    }

    QSize sizeHint() const override
    {
        const QSize pixels = !m_image.isNull() ? m_image.size() : minimumSize();
        return pixels.isValid() ? pixels + QSize(2, 2) : QWidget::sizeHint();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (!m_image.isNull()) {
            QPainterPath clip;
            clip.addRoundedRect(frame.adjusted(1, 1, -1, -1), 5, 5);
            painter.setClipPath(clip);
            painter.drawImage(frame.adjusted(1, 1, -1, -1), m_image);
            painter.setClipping(false);
        }
        painter.setPen(QPen(m_border, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(frame, 6, 6);
    }

private:
    void applyScale()
    {
        const int cap = m_expanded ? 560 : 160;
        m_wanted = cap;
        const auto cached = m_scaledImages.constFind(cap);
        if (cached != m_scaledImages.constEnd()) {
            showScaled(*cached);
            return;
        }
        if (m_srcSize.isValid() && m_srcSize.width() > 0) {
            const int w = qMin(m_srcSize.width(), cap);
            setMinimumSize(w, qMax(1, qRound(double(w) * m_srcSize.height() /
                                             m_srcSize.width())));
        }
        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this,
                [this, watcher, cap] {
                    watcher->deleteLater();
                    const QImage img = watcher->result();
                    if (img.isNull())
                        return;
                    m_scaledImages.insert(cap, img);
                    if (m_wanted == cap) // a later toggle already superseded this
                        showScaled(img);
                });
        watcher->setFuture(QtConcurrent::run(decodeScaledAttachment, m_path, cap));
    }

    void showScaled(const QImage &image)
    {
        setMinimumSize(0, 0);
        m_image = image;
        updateGeometry();
        update();
    }

    QString m_path;
    QSize m_srcSize;
    QColor m_border;
    QHash<int, QImage> m_scaledImages;
    QImage m_image;
    int m_wanted = 0;
    bool m_expanded = false;
};

// A speaker's content: their name and when they said it in small type on top,
// then what they said.
QWidget *speakerBody(QWidget *caption, QWidget *content)
{
    auto *frame = new QWidget;
    frame->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
    auto *v = new QVBoxLayout(frame);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(caption ? 5 : 0);
    if (caption)
        v->addWidget(caption);
    v->addWidget(content);
    return frame;
}

const char *const kProseMarkdown = "proseMarkdown";

QPixmap tintedOcticon(const QString &name, const QColor &tint, int px, qreal dpr)
{
    static QHash<QString, QPixmap> cache;
    const QColor colour = Theme::remapColor(tint);
    const QString cacheKey = name + QLatin1Char('|') + QString::number(colour.rgba(), 16)
                             + QLatin1Char('|') + QString::number(px)
                             + QLatin1Char('|') + QString::number(dpr);
    const auto cached = cache.constFind(cacheKey);
    if (cached != cache.constEnd())
        return cached.value();
    QPixmap pixmap(QSize(px, px) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QSvgRenderer renderer(QStringLiteral(":/icons/octicons/%1.svg").arg(name));
    if (renderer.isValid()) {
        QPainter g(&pixmap);
        g.setRenderHint(QPainter::Antialiasing);
        renderer.render(&g, QRectF(0, 0, px, px));
        g.setCompositionMode(QPainter::CompositionMode_SourceIn);
        g.fillRect(QRectF(0, 0, px, px), colour);
    }
    cache.insert(cacheKey, pixmap);
    return pixmap;
}

void addProseCopyButton(QWidget *caption, QLabel *prose,
                        const ClaudeTranscriptView::Palette &p)
{
    auto *row = caption ? qobject_cast<QHBoxLayout *>(caption->layout()) : nullptr;
    if (!row || !prose)
        return;
    constexpr int kIconPx = 14;
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QIcon copyIcon;
    copyIcon.addPixmap(tintedOcticon(QStringLiteral("copy"), QColor(p.muted), kIconPx, dpr),
                       QIcon::Normal);
    copyIcon.addPixmap(tintedOcticon(QStringLiteral("copy"), QColor(p.accent), kIconPx, dpr),
                       QIcon::Active);
    const QIcon copiedIcon(
        tintedOcticon(QStringLiteral("check-circle"), QColor(p.add), kIconPx, dpr));
    const QString tip = QStringLiteral("Copy as Markdown");
    auto *button = new QToolButton(caption);
    button->setObjectName(QStringLiteral("proseCopy"));
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(22, 22);
    button->setIconSize(QSize(kIconPx, kIconPx));
    button->setIcon(copyIcon);
    button->setToolTip(tip);
    button->setAccessibleName(QStringLiteral("Copy"));
    button->setStyleSheet(Theme::remapSheet(
        QStringLiteral("QToolButton{border:none;border-radius:5px;background:transparent;}"
                       "QToolButton:hover{background:%1;}")
            .arg(p.chipBg)));
    QObject::connect(button, &QToolButton::clicked, button,
                     [button, prose = QPointer<QLabel>(prose), copyIcon, copiedIcon, tip] {
        if (!prose)
            return;
        QString text = prose->property(kProseMarkdown).toString();
        if (text.isEmpty())
            text = prose->text();
        QGuiApplication::clipboard()->setText(text.trimmed());
        button->setIcon(copiedIcon);
        button->setToolTip(QStringLiteral("Copied"));
        QTimer::singleShot(1500, button, [button, copyIcon, tip] {
            button->setIcon(copyIcon);
            button->setToolTip(tip);
        });
    });
    row->addWidget(button, 0, Qt::AlignVCenter);
}

class ChoiceOption : public QFrame
{
public:
    explicit ChoiceOption(const QString &text, QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setCursor(Qt::PointingHandCursor);
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(10, 8, 10, 8);
        auto *l = new QLabel(text);
        l->setWordWrap(true);
        l->setAttribute(Qt::WA_TransparentForMouseEvents); // clicks reach the frame
        v->addWidget(l);
    }

    std::function<void()> onClick;

protected:
    void mousePressEvent(QMouseEvent *) override
    {
        if (onClick)
            onClick();
    }
};

class HeldFrame : public QLabel
{
public:
    explicit HeldFrame(QWidget *parent) : QLabel(parent)
    {
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        setAutoFillBackground(true);
        hide();
    }

protected:
    void wheelEvent(QWheelEvent *e) override { e->accept(); }
};

namespace {

QString nextProseHtml(const QString &markdown, const QString &codeBg)
{
    static QCache<QString, QString> cache(4 * 1024 * 1024);
    const QFont prose = proseFont();
    const QString key = codeBg + QLatin1Char('\x1f') + prose.key() + QLatin1Char('\x1f') +
                        markdown;
    if (const QString *hit = cache.object(key))
        return *hit;
    QTextDocument doc;
    doc.setDefaultFont(prose);
    doc.setMarkdown(markdown);
    QList<QPair<int, int>> spans;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        const QTextBlockFormat format = block.blockFormat();
        if (format.nonBreakableLines() || format.hasProperty(QTextFormat::BlockCodeFence) ||
            format.hasProperty(QTextFormat::BlockCodeLanguage))
            continue;
        QTextBlockFormat spaced = format;
        spaced.setLineHeight(165, QTextBlockFormat::ProportionalHeight);
        QTextCursor(block).setBlockFormat(spaced);
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().fontFixedPitch())
                spans.append({fragment.position(), fragment.length()});
        }
    }
    if (!spans.isEmpty()) {
        QFont mono = monoFont();
        mono.setPixelSize(qMax(10, (prose.pixelSize() > 0 ? prose.pixelSize() : 15) - 2));
        QTextCharFormat chip;
        chip.setFont(mono, QTextCharFormat::FontPropertiesSpecifiedOnly);
        chip.setBackground(QColor(codeBg));
        for (const auto &span : std::as_const(spans)) {
            QTextCursor cursor(&doc);
            cursor.setPosition(span.first);
            cursor.setPosition(span.first + span.second, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(chip);
        }
    }
    const QString html = doc.toHtml();
    cache.insert(key, new QString(html), qMax<qsizetype>(1, html.size()));
    return html;
}

QString commandCountText(int count)
{
    return count == 1 ? QStringLiteral("1 command")
                      : QStringLiteral("%1 commands").arg(count);
}

class TranscriptCommandGroupHeader : public QWidget
{
public:
    explicit TranscriptCommandGroupHeader(const ClaudeTranscriptView::Palette &p,
                                          QWidget *parent = nullptr)
        : QWidget(parent), m_p(p)
    {
        setObjectName(QStringLiteral("agentCommandGroupSummary"));
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setFixedHeight(28);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        QFont font = proseFont();
        font.setPixelSize(13);
        setFont(font);
    }

    std::function<void()> onToggle;

    void setState(int total, int running, int failed, bool open)
    {
        m_total = total;
        m_running = running;
        m_failed = failed;
        m_open = open;
        setAccessibleName(summaryText() +
                          (failed > 0 ? QStringLiteral(" (%1 failed)").arg(failed)
                                      : QString()));
        setAccessibleDescription(open ? QStringLiteral("Press to hide the commands")
                                      : QStringLiteral("Press to show the commands"));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        const bool hot = underMouse() || hasFocus();
        const QColor tone(hot ? m_p.text : m_p.muted);
        const qreal dpr = devicePixelRatioF();
        const QPixmap icon = tintedOcticon(QStringLiteral("terminal"), tone, 14, dpr);
        g.drawPixmap(QPointF(2, std::round((height() - 14) / 2.0)), icon);
        const QFontMetrics fm(font());
        int x = 22;
        const QString main = summaryText();
        g.setPen(tone);
        g.drawText(QRect(x, 0, width() - x, height()), Qt::AlignLeft | Qt::AlignVCenter, main);
        x += fm.horizontalAdvance(main);
        if (m_failed > 0) {
            const QString failed = QStringLiteral(" (%1 failed)").arg(m_failed);
            g.setPen(QColor(m_p.del));
            g.drawText(QRect(x, 0, qMax(0, width() - x), height()),
                       Qt::AlignLeft | Qt::AlignVCenter, failed);
            x += fm.horizontalAdvance(failed);
        }
        const QPointF tip(x + 10.0, height() / 2.0);
        QPen pen(tone, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        g.setPen(pen);
        g.setBrush(Qt::NoBrush);
        QPainterPath arrow;
        if (m_open) {
            arrow.moveTo(tip + QPointF(-3.5, -1.75));
            arrow.lineTo(tip + QPointF(0.0, 1.75));
            arrow.lineTo(tip + QPointF(3.5, -1.75));
        } else {
            arrow.moveTo(tip + QPointF(-1.75, -3.5));
            arrow.lineTo(tip + QPointF(1.75, 0.0));
            arrow.lineTo(tip + QPointF(-1.75, 3.5));
        }
        g.drawPath(arrow);
    }

    void enterEvent(QEnterEvent *e) override
    {
        update();
        QWidget::enterEvent(e);
    }

    void leaveEvent(QEvent *e) override
    {
        update();
        QWidget::leaveEvent(e);
    }

    void focusInEvent(QFocusEvent *e) override
    {
        update();
        QWidget::focusInEvent(e);
    }

    void focusOutEvent(QFocusEvent *e) override
    {
        update();
        QWidget::focusOutEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()) &&
            onToggle) {
            onToggle();
            e->accept();
            return;
        }
        QWidget::mouseReleaseEvent(e);
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if ((e->key() == Qt::Key_Space || e->key() == Qt::Key_Return ||
             e->key() == Qt::Key_Enter) &&
            onToggle) {
            onToggle();
            e->accept();
            return;
        }
        QWidget::keyPressEvent(e);
    }

private:
    QString summaryText() const
    {
        return (m_running > 0 ? QStringLiteral("Running ") : QStringLiteral("Ran ")) +
               commandCountText(m_total);
    }

    ClaudeTranscriptView::Palette m_p;
    int m_total = 0;
    int m_running = 0;
    int m_failed = 0;
    bool m_open = false;
};

class TranscriptCommandGroup : public QFrame
{
public:
    explicit TranscriptCommandGroup(const ClaudeTranscriptView::Palette &p,
                                    QWidget *parent = nullptr)
        : QFrame(parent), m_edge(p.aiEdge)
    {
        setObjectName(QStringLiteral("agentCommandGroup"));
        setStyleSheet(QStringLiteral(
            "QFrame#agentCommandGroup{background:transparent;border:none;}"));
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(4);
        m_header = new TranscriptCommandGroupHeader(p, this);
        m_header->onToggle = [this] { setExpanded(!m_open); };
        m_header->hide();
        m_layout->addWidget(m_header);
        m_panel = new TranscriptPanel(p.aiBg, p.aiEdge, 12, this);
        m_cards = new QVBoxLayout(m_panel);
        m_cards->setContentsMargins(10, 4, 10, 4);
        m_cards->setSpacing(0);
        m_panel->hide();
        m_layout->addWidget(m_panel);
    }

    void addCard(CommandCard *card)
    {
        if (!card)
            return;
        card->header()->onStateChange = [this] { refresh(); };
        m_list.append(card);
        if (m_list.size() == 1) {
            m_layout->addWidget(card);
        } else {
            if (m_list.size() == 2) {
                CommandCard *first = m_list.first();
                if (first) {
                    first->header()->setStatusDot(true);
                    m_cards->addWidget(first);
                    first->show();
                }
                m_header->show();
            }
            auto *rule = new QWidget(m_panel);
            rule->setFixedHeight(1);
            rule->setStyleSheet(Theme::remapSheet(
                QStringLiteral("background:%1;border:none;").arg(m_edge)));
            m_cards->addWidget(rule);
            card->header()->setStatusDot(true);
            m_cards->addWidget(card);
            m_panel->setVisible(m_open);
        }
        refresh();
    }

    void setExpanded(bool on)
    {
        m_open = on;
        if (m_list.size() > 1)
            m_panel->setVisible(on);
        refresh();
    }

    void refresh()
    {
        int running = 0;
        int failed = 0;
        int total = 0;
        for (const QPointer<CommandCard> &card : std::as_const(m_list)) {
            if (!card)
                continue;
            ++total;
            if (card->header()->runningFlag())
                ++running;
            if (card->header()->failedFlag())
                ++failed;
        }
        m_header->setState(total, running, failed, m_open);
    }

private:
    QString m_edge;
    QVBoxLayout *m_layout = nullptr;
    TranscriptCommandGroupHeader *m_header = nullptr;
    TranscriptPanel *m_panel = nullptr;
    QVBoxLayout *m_cards = nullptr;
    QList<QPointer<CommandCard>> m_list;
    bool m_open = false;
};

TranscriptCommandGroup *commandGroupBefore(QVBoxLayout *column, int pos)
{
    if (!column || pos <= 0 || pos > column->count())
        return nullptr;
    QLayoutItem *item = column->itemAt(pos - 1);
    auto *rail = item ? dynamic_cast<RailItem *>(item->widget()) : nullptr;
    return rail ? dynamic_cast<TranscriptCommandGroup *>(rail->content()) : nullptr;
}

class TranscriptUserBubble : public QWidget
{
public:
    explicit TranscriptUserBubble(const QColor &fill, QWidget *parent = nullptr)
        : QWidget(parent), m_fill(fill)
    {
        setObjectName(QStringLiteral("transcriptUserBubble"));
        setAttribute(Qt::WA_StyledBackground, false);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        const QRectF r(rect());
        if (r.width() <= 0 || r.height() <= 0)
            return;
        const qreal big = qMin<qreal>(16.0, qMin(r.width(), r.height()) / 2.0);
        const qreal small = qMin<qreal>(6.0, big);
        QPainterPath path;
        path.moveTo(r.left() + big, r.top());
        path.lineTo(r.right() - big, r.top());
        path.arcTo(QRectF(r.right() - 2 * big, r.top(), 2 * big, 2 * big), 90, -90);
        path.lineTo(r.right(), r.bottom() - small);
        path.arcTo(QRectF(r.right() - 2 * small, r.bottom() - 2 * small, 2 * small, 2 * small),
                   0, -90);
        path.lineTo(r.left() + big, r.bottom());
        path.arcTo(QRectF(r.left(), r.bottom() - 2 * big, 2 * big, 2 * big), 270, -90);
        path.lineTo(r.left(), r.top() + big);
        path.arcTo(QRectF(r.left(), r.top(), 2 * big, 2 * big), 180, -90);
        path.closeSubpath();
        g.fillPath(path, m_fill);
    }

private:
    QColor m_fill;
};

class TranscriptUserBubbleRow : public QWidget
{
public:
    TranscriptUserBubbleRow(TranscriptUserBubble *bubble, QLabel *time, int textWidth,
                            const QList<QPointer<QWidget>> &images,
                            QWidget *parent = nullptr)
        : QWidget(parent), m_bubble(bubble), m_time(time), m_textWidth(textWidth),
          m_images(images)
    {
        m_bubble->setParent(this);
        if (m_time)
            m_time->setParent(this);
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setMinimumWidth(0);
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int w) const override
    {
        int h = bubbleHeight(bubbleWidth(w));
        if (m_time && !m_time->isHidden())
            h += kTimeGap + m_time->sizeHint().height();
        return h;
    }

    QSize sizeHint() const override
    {
        const int w = naturalWidth();
        return QSize(w, heightForWidth(w));
    }

    QSize minimumSizeHint() const override { return QSize(0, 0); }

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::LayoutRequest) {
            updateGeometry();
            place();
        }
        return QWidget::event(e);
    }

    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        place();
    }

private:
    static constexpr int kTimeGap = 4;

    int naturalWidth() const
    {
        int w = m_textWidth;
        for (const QPointer<QWidget> &image : m_images)
            if (image)
                w = qMax(w, image->sizeHint().width());
        const QMargins margins =
            m_bubble->layout() ? m_bubble->layout()->contentsMargins() : QMargins();
        return w + margins.left() + margins.right();
    }

    int bubbleWidth(int w) const
    {
        const int natural = naturalWidth();
        if (w <= 0)
            return natural;
        return qMax(1, qMin(w, qMin(natural, qMax(120, int(w * 0.8)))));
    }

    int bubbleHeight(int w) const
    {
        const int h = m_bubble->heightForWidth(w);
        return h > 0 ? h : m_bubble->sizeHint().height();
    }

    void place()
    {
        const int bw = bubbleWidth(width());
        const int bh = bubbleHeight(bw);
        m_bubble->setGeometry(width() - bw, 0, bw, bh);
        if (m_time && !m_time->isHidden()) {
            const QSize ts = m_time->sizeHint();
            m_time->setGeometry(qMax(0, width() - ts.width() - 4), bh + kTimeGap,
                                qMin(width(), ts.width()), ts.height());
        }
    }

    TranscriptUserBubble *m_bubble = nullptr;
    QPointer<QLabel> m_time;
    int m_textWidth = 0;
    QList<QPointer<QWidget>> m_images;
};

}

ClaudeTranscriptView::ClaudeTranscriptView(QWidget *parent) : QScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setFont(monoFont());
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_container = new QWidget;
    m_container->setMinimumWidth(0);
    m_container->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_container->setFont(monoFont());
    m_col = new QVBoxLayout(m_container);
    m_col->setContentsMargins(8, 14, 14, 14);
    m_col->setSpacing(0); // RailItems supply their own inter-item gap
    m_bottomSpacer = new QWidget;
    m_bottomSpacer->setMinimumHeight(8);
    m_bottomSpacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_col->addWidget(m_bottomSpacer);
    setWidget(m_container);
    // Installed after setWidget() so it runs ahead of the scroll area's own
    // filter on the same widget; see eventFilter().
    m_container->installEventFilter(this);

    m_jumpButtons = new ScrollJumpButtons(this);
    connect(m_jumpButtons, &ScrollJumpButtons::topClicked, this,
            &ClaudeTranscriptView::scrollToTop);
    connect(m_jumpButtons, &ScrollJumpButtons::bottomClicked, this,
            &ClaudeTranscriptView::scrollToBottom);

    m_promptDeck = new PromptDeckBar(this);
    m_promptDeck->setObjectName(QStringLiteral("transcriptPromptDeck"));
    m_promptDeck->setFont(proseFont());
    connect(m_promptDeck, &PromptDeckBar::heightChanged, this,
            &ClaudeTranscriptView::layoutPromptDeck);

    m_coverageButton = new QToolButton(this);
    m_coverageButton->setObjectName(QStringLiteral("transcriptCoverageGaps"));
    m_coverageButton->setCursor(Qt::PointingHandCursor);
    m_coverageButton->setAutoRaise(true);
    m_coverageButton->hide();
    m_coverageButton->installEventFilter(this);
    connect(m_coverageButton, &QToolButton::clicked, this,
            &ClaudeTranscriptView::coverageGapsActivated);

    QScrollBar *sb = verticalScrollBar();
    connect(sb, &QScrollBar::actionTriggered, this, [this](int) {
        QScrollBar *b = verticalScrollBar();
        if (m_scrollAnim)
            m_scrollAnim->stop();
        m_stickBottom = b->sliderPosition() >= b->maximum() - 4;
    });
    connect(sb, &QScrollBar::valueChanged, this, [this](int v) {
        if (m_batchBuildFrozen || m_batchThawing)
            return;
        QScrollBar *b = verticalScrollBar();
        if (m_skippedNotice && !m_loadEarlierPending && v <= b->minimum() + 200) {
            m_loadEarlierPending = true;
            emit loadEarlierRequested();
        }
    });
    connect(sb, &QScrollBar::rangeChanged, this, [this](int, int max) {
        if (!m_stickBottom)
            return;
        if (m_scrollAnim && m_scrollAnim->state() == QAbstractAnimation::Running)
            m_scrollAnim->setEndValue(max);
        else
            verticalScrollBar()->setValue(max);
    });

    applyScheme(applicationUsesDarkPalette());
}

void ClaudeTranscriptView::resizeEvent(QResizeEvent *e)
{
    QScrollArea::resizeEvent(e);
    layoutPromptDeck();
    if (m_heldFrame && m_heldFrame->isVisible())
        m_heldFrame->setGeometry(rect());
    if (m_stickBottom)
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void ClaudeTranscriptView::layoutPromptDeck()
{
    if (!m_promptDeck)
        return;
    const int left = 8;
    const int right = 14;
    const int top = 6;
    const int w = qMax(0, width() - left - right);
    const int h = m_promptDeck->isVisible() ? m_promptDeck->heightForWidth(w) : 0;
    const bool atBottom = m_stickBottom;
    if (h > 0) {
        m_promptDeck->setGeometry(left, top, w, h);
        m_promptDeck->raise();
        setViewportMargins(0, h + top + 4, 0, 0);
    } else {
        setViewportMargins(0, 0, 0, 0);
    }
    if (atBottom)
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    layoutCoverageButton();
}

void ClaudeTranscriptView::noteCoverage(const QJsonObject &ev)
{
    if (m_coverage.noteEvent(ev))
        refreshCoverageButton();
}

void ClaudeTranscriptView::refreshCoverageButton()
{
    if (!m_coverageButton)
        return;
    if (m_coverage.isEmpty()) {
        m_coverageButton->hide();
        return;
    }
    const int kinds = m_coverage.kindCount();
    m_coverageButton->setText(
        QStringLiteral("\u25CF %1 missing").arg(kinds));
    m_coverageButton->setAccessibleName(
        QStringLiteral("%1 kind%2 of raw log entries missing from the transcript")
            .arg(kinds)
            .arg(kinds == 1 ? QString() : QStringLiteral("s")));
    m_coverageButton->adjustSize();
    m_coverageButton->show();
    layoutCoverageButton();
}

void ClaudeTranscriptView::layoutCoverageButton()
{
    if (!m_coverageButton || m_coverageButton->isHidden())
        return;
    const QRect area = viewport()->geometry();
    const QSize size = m_coverageButton->sizeHint();
    m_coverageButton->setGeometry(area.right() - size.width() - 8, area.top() + 6,
                                  size.width(), size.height());
    m_coverageButton->raise();
}

QString ClaudeTranscriptView::coverageToolTip() const
{
    return QStringLiteral(
               "The raw log holds entries this transcript does not show:\n%1\n\n"
               "Click to start an agent with the current settings that adds them "
               "to the transcript.")
        .arg(forkmesh::transcriptcoverage::summary(m_coverage.gaps()));
}

void ClaudeTranscriptView::queueLabelRelayout(CacheLabel *label)
{
    if (!label)
        return;
    m_labelRelayoutQueue.append(label);
    if (m_labelRelayoutScheduled)
        return;
    m_labelRelayoutScheduled = true;
    QTimer::singleShot(1, this, &ClaudeTranscriptView::processLabelRelayouts);
}

void ClaudeTranscriptView::processLabelRelayouts()
{
    // QTextDocument shaping is GUI-thread-only. Bound each turn instead of
    // moving it to a worker: input, paint, and network events get a chance to
    // run between slices, while repeated resize widths stay coalesced inside
    // each CacheLabel.
    constexpr qint64 kSliceBudgetMs = 6;
    QElapsedTimer slice;
    slice.start();
    do {
        if (m_labelRelayoutQueue.isEmpty())
            break;
        const QPointer<QLabel> guarded = m_labelRelayoutQueue.takeFirst();
        if (auto *label = dynamic_cast<CacheLabel *>(guarded.data()))
            label->warmPendingHeight();
    } while (!m_labelRelayoutQueue.isEmpty() &&
             slice.elapsed() < kSliceBudgetMs);

    if (!m_labelRelayoutQueue.isEmpty()) {
        QTimer::singleShot(1, this, &ClaudeTranscriptView::processLabelRelayouts);
        return;
    }
    m_labelRelayoutScheduled = false;
}

#ifdef FORKMESH_WINDOW_TESTS
int ClaudeTranscriptView::pendingLabelRelayoutsForTest() const
{
    return m_labelRelayoutQueue.size();
}
#endif

void ClaudeTranscriptView::scrollToTop()
{
    m_stickBottom = false;
    smoothScrollTo(verticalScrollBar()->minimum());
}

void ClaudeTranscriptView::scrollToBottom()
{
    m_stickBottom = true;
    smoothScrollTo(verticalScrollBar()->maximum());
}

void ClaudeTranscriptView::jumpToBottom()
{
    m_stickBottom = true;
    if (m_scrollAnim)
        m_scrollAnim->stop(); // don't let an in-flight smooth scroll pull us back
    QScrollBar *sb = verticalScrollBar();
    sb->setValue(sb->maximum());
}

void ClaudeTranscriptView::setBulkPopulate(bool on)
{
    if (on == m_bulkPopulate)
        return;
    m_bulkPopulate = on;
    if (on)
        beginBatchBuild();
    else
        endBatchBuild();
}

// Opening a stored session builds hundreds of rows, a slice per event-loop
// turn. Left to itself the column lays itself out again on every one of those
// turns — quadratic work costing more than building the rows does — and, with
// the view pinned to the tail, it runs the whole history past the reader before
// settling on the newest turn. Build the batch behind a blank canvas instead,
// with the column's layout held: the transcript appears once, already at its
// latest turn.
void ClaudeTranscriptView::setBatchHoldsFrame(bool hold, bool rowsLandAbove)
{
    m_holdFrame = hold;
    m_holdRowsAbove = hold && rowsLandAbove;
    if (!hold)
        releaseHeldFrame();
}

void ClaudeTranscriptView::beginBatchBuild()
{
    if (m_batchBuildFrozen || !m_col || !m_container)
        return;
    const bool hold = m_holdFrame && isVisible() && m_container->isVisible() &&
                      m_col->count() > 1 && width() > 0 && height() > 0;
    if (hold) {
        if (m_scrollAnim)
            m_scrollAnim->stop();
        QScrollBar *sb = verticalScrollBar();
        m_heldMax = sb->maximum();
        m_heldValue = sb->value();
        if (!m_heldFrame)
            m_heldFrame = new HeldFrame(this);
        m_heldFrame->setStyleSheet(
            QStringLiteral("background:%1;").arg(m_p.canvas));
        m_heldFrame->setPixmap(grab());
        m_heldFrame->setGeometry(rect());
        m_heldFrame->raise();
        m_heldFrame->show();
    }
    m_batchBuildFrozen = true;
    m_col->setEnabled(false);
    m_container->hide();
    // Nothing is laid out behind the blank canvas, so there is nothing to
    // scroll either. Retiring the old range keeps a stale handle from sitting
    // over it, and the thaw restores the real one.
    if (!hold)
        verticalScrollBar()->setRange(0, 0);
}

void ClaudeTranscriptView::releaseHeldFrame()
{
    if (!m_heldFrame || m_heldFrame->isHidden())
        return;
    m_heldFrame->hide();
    m_heldFrame->setPixmap(QPixmap());
    if (m_batchBuildFrozen)
        verticalScrollBar()->setRange(0, 0);
}

void ClaudeTranscriptView::endBatchBuild()
{
    if (!m_batchBuildFrozen)
        return;
    m_batchBuildFrozen = false;
    m_batchThawing = true;
    const bool held = m_heldFrame && m_heldFrame->isVisible();
    m_col->setEnabled(true);
    m_col->activate();
    m_container->show();
    m_col->activate();
    // The scroll range only follows the container on a posted LayoutRequest,
    // and this batch's were swallowed. Deliver one now so the rows are already
    // at the tail in the frame that reveals them, not one frame later.
    QEvent layoutRequest(QEvent::LayoutRequest);
    QCoreApplication::sendEvent(m_container, &layoutRequest);
    QEvent viewLayoutRequest(QEvent::LayoutRequest);
    QCoreApplication::sendEvent(this, &viewLayoutRequest);
    m_batchThawing = false;
    QScrollBar *sb = verticalScrollBar();
    if (m_stickBottom) {
        jumpToBottom();
    } else if (held) {
        sb->setValue(m_holdRowsAbove
                         ? m_heldValue + (sb->maximum() - m_heldMax)
                         : m_heldValue);
    }
    m_holdFrame = false;
    m_holdRowsAbove = false;
    releaseHeldFrame();
}

// The column posts a layout request for every row that lands in it. While a
// batch is building those heights are meaningless — the rows exist but are not
// laid out — and each one would resize the scroll range under the reader.
// Swallow them; endBatchBuild() delivers the one that counts.
bool ClaudeTranscriptView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_coverageButton && event && event->type() == QEvent::ToolTip)
        m_coverageButton->setToolTip(coverageToolTip());
    if (m_batchBuildFrozen && watched == m_container && event &&
        event->type() == QEvent::LayoutRequest)
        return true;
    return QScrollArea::eventFilter(watched, event);
}

bool ClaudeTranscriptView::event(QEvent *e)
{
    if (m_batchBuildFrozen && e && e->type() == QEvent::LayoutRequest)
        return true;
    return QScrollArea::event(e);
}

void ClaudeTranscriptView::setSplitDiffs(bool on) { m_splitDiffs = on; }

void ClaudeTranscriptView::setCodexStyle(bool on) { m_codexStyle = on; }

void ClaudeTranscriptView::setTheme(bool dark) { applyScheme(dark); }

void ClaudeTranscriptView::setSessionContext(const QString &branch,
                                             const QString &mode,
                                             const QString &strength,
                                             const QString &account)
{
    m_ctxBranch = branch.trimmed();
    m_ctxMode = mode.trimmed();
    m_ctxStrength = strength.trimmed();
    m_ctxAccount = account.trimmed();
}

void ClaudeTranscriptView::cycleActivityWord()
{
    if (m_codexStyle) {
        if (!m_activityLabel)
            return;
        const qint64 secs =
            qMax(qint64(0), (QDateTime::currentMSecsSinceEpoch() - m_activityStartMs)
                                / 1000);
        const QString elapsed =
            secs < 60 ? QStringLiteral("%1s").arg(secs)
                      : QStringLiteral("%1m %2s")
                            .arg(secs / 60)
                            .arg(secs % 60, 2, 10, QLatin1Char('0'));
        m_activityLabel->setText(QStringLiteral("Working (%1)").arg(elapsed));
        return;
    }
    static const char *kWords[] = {
        "Thinking", "Pondering", "Cogitating", "Percolating", "Noodling",
        "Conjuring", "Ruminating", "Tinkering", "Synthesizing", "Scheming",
        "Forking", "Meshing", "Enmeshing", "Forkmeshing", "Remeshing",
        "Reticulating meshes", "Untangling forks", "Weaving the mesh",
        "Spinning up forks", "Meshulating", "Defragging the mesh", "Reforking",
        "Coalescing nodes", "Threading the mesh", "Herding forks",
        "Greasing the mesh", "Forkstrapping", "Demeshing", "Transmeshing"};
    constexpr int n = int(sizeof(kWords) / sizeof(kWords[0]));
    if (m_activityLabel)
        m_activityLabel->setText(
            QString::fromUtf8(kWords[QRandomGenerator::global()->bounded(n)])
            + QString::fromUtf8("\xE2\x80\xA6")); // …
}

void ClaudeTranscriptView::ensureActivity()
{
    if (m_activity || m_liveThinking)
        return; // the thinking card is its own live indicator
    m_activityLabel = new QLabel;
    m_activityLabel->setFont(monoFont());
    m_activityLabel->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "color:%1;background:transparent;border:none;font-style:italic;")
        .arg(m_p.muted)));
    m_activityStartMs = QDateTime::currentMSecsSinceEpoch();
    cycleActivityWord();
    m_keepExplore = true;
    m_activity = addRow(m_activityLabel, m_p.accent,
                        m_codexStyle ? GlyphDot : GlyphStar);
    m_keepExplore = false;
    if (!m_activityTimer) {
        m_activityTimer = new QTimer(this);
        connect(m_activityTimer, &QTimer::timeout, this,
                &ClaudeTranscriptView::cycleActivityWord);
    }
    m_activityTimer->start(m_codexStyle ? 1000 : 2200);
}

void ClaudeTranscriptView::clearActivity()
{
    if (m_activityTimer)
        m_activityTimer->stop();
    if (m_activity) {
        m_col->removeWidget(m_activity);
        m_activity->deleteLater();
        m_activity = nullptr;
        m_activityLabel = nullptr;
    }
}

void ClaudeTranscriptView::applyScheme(bool dark)
{
    if (dark)
        m_p = {"#0d1117", "#161b22", "#30363d", "#e6edf3", "#8b949e", "#58a6ff",
               "#3fb950", "#f85149", "#12261a", "#2d1416", "#20242c", "#141d2b",
               // aiEdge, userEdge, termBg, termEdge, chipBg
               "#26364b", "#31363f", "#090d13", "#222b36", "#1c2531",
               // Bash: command, keyword, string, variable, operator, comment, path
               "#79c0ff", "#ff7b72", "#a5d6ff", "#ffa657", "#d2a8ff", "#8b949e",
               "#7ee787"};
    else
        m_p = {"#ffffff", "#f6f8fa", "#d0d7de", "#1f2328", "#656d76", "#0969da",
               "#1a7f37", "#cf222e", "#e6ffec", "#ffebe9", "#eef1f5", "#eaf2fd",
               "#cfe2f7", "#dde1e7", "#f4f6f9", "#dbe1e8", "#eaeef3",
               "#0550ae", "#cf222e", "#0a3069", "#953800", "#8250df", "#57606a",
               "#116329"};
    remapTranscriptPalette(m_p);
    if (Theme::isNext()) {
        const auto role = [dark](Theme::Role value) {
            return Theme::hex(value, Theme::Design::Next, dark);
        };
        m_p.userBg = role(Theme::Role::SurfaceRaised);
        m_p.userEdge = role(Theme::Role::BorderStrong);
        m_p.aiBg = role(Theme::Role::Surface);
        m_p.aiEdge = role(Theme::Role::Border);
        m_p.termBg = role(Theme::Role::BgSubtle);
        m_p.termEdge = role(Theme::Role::Border);
        m_p.chipBg = role(Theme::Role::SurfaceRaised);
    }
    setStyleSheet(QStringLiteral("QScrollArea{background:%1;border:none;}").arg(m_p.canvas));
    if (m_container)
        m_container->setStyleSheet(Theme::remapSheet(QStringLiteral("background:%1;").arg(m_p.canvas)));
    const QString btnCss = QStringLiteral(
        "QPushButton{background:%1;color:%2;border:1px solid %3;border-radius:15px;"
        "font-size:12px;font-weight:700;}"
        "QPushButton:hover{background:%4;}")
        .arg(m_p.surface, m_p.text, m_p.border, m_p.userBg);
    if (m_jumpButtons) {
        m_jumpButtons->setButtonStyle(btnCss);
        m_jumpButtons->setTheme(dark);
    }
    if (m_coverageButton)
        m_coverageButton->setStyleSheet(QStringLiteral(
            "QToolButton{background:%1;color:#ffffff;border:none;border-radius:10px;"
            "padding:2px 9px;font-size:11px;font-weight:700;}"
            "QToolButton:hover{background:%2;}")
            .arg(m_p.del, QColor(m_p.del).darker(115).name()));
    if (m_promptDeck) {
        QColor tint(m_p.accent);
        tint.setAlphaF(dark ? 0.12 : 0.08);
        m_promptDeck->setColors(QColor(m_p.surface), tint, QColor(m_p.border),
                                QColor(m_p.text), QColor(m_p.muted), QColor(m_p.accent));
    }
}

void ClaudeTranscriptView::enableReferenceLinks(QLabel *label)
{
    if (!label)
        return;
    label->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                   Qt::LinksAccessibleByMouse);
    label->setOpenExternalLinks(false);
    connect(label, &QLabel::linkActivated, this,
            [this](const QString &href) { emit referenceActivated(href); });
}

QString ClaudeTranscriptView::referenceHtml(const QString &plainText) const
{
    QString html = linkifyTranscriptText(plainText, true);
    html.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return html;
}

void ClaudeTranscriptView::clear()
{
    clearActivity();
    if (m_textFlushTimer)
        m_textFlushTimer->stop();
    m_dirtyAgentText.clear();
    m_dirtyToolOutput.clear();
    m_dirtyThinking = false;
    m_searchLabels.clear();
    m_searchQuery.clear();
    m_searchTotal = 0;
    m_searchCurrent = -1;
    emit searchResultsChanged(0, 0);
    m_toolCards.clear();
    m_commandApprovals.clear();
    m_liveAgentText.clear();
    m_liveAgentTextValue.clear();
    m_liveAgentRows.clear();
    m_streamMessageId.clear();
    m_streamMessageAt = QDateTime();
    m_streamTextKeys.clear();
    m_pendingStreamText.clear();
    m_outputTokens.clear();
    m_askCards.clear();
    m_skippedNotice = nullptr;
    m_skippedCount = 0;
    m_loadEarlierPending = false;
    m_prependAt = -1;
    m_exploreBlock = nullptr;
    m_exploreLines = nullptr;
    m_resultLabel = nullptr;
    m_resultLabelText.clear();
    m_backgroundWorkNote.clear();
    m_liveThinking = nullptr;
    m_thinkingBody = nullptr;
    m_thinkingText.clear();
    m_lastFinalizedThinkingText.clear();
    m_thinkingTokens = 0;
    m_totalTokens = 0;
    m_totalCost = 0.0;
    m_labelRelayoutQueue.clear();
    m_labelRelayoutScheduled = false;
    emit statsChanged(0, 0.0);
    if (m_promptDeck)
        m_promptDeck->clearPrompts();
    m_coverage.clear();
    refreshCoverageButton();
    if (m_col->count() <= 1)
        return;
    QWidget *host = m_col->parentWidget();
    const bool hostUpdates = host && host->updatesEnabled();
    if (hostUpdates)
        host->setUpdatesEnabled(false);
    while (m_col->count() > 1) { // keep the trailing spacer
        QLayoutItem *it = m_col->takeAt(m_col->count() - 2);
        if (QWidget *w = it->widget())
            retireRow(w);
        delete it;
    }
    if (hostUpdates)
        host->setUpdatesEnabled(true);
}

void ClaudeTranscriptView::retireRow(QWidget *row)
{
    row->hide();
    m_retiredRows.append(row);
    if (m_retiredRowsScheduled)
        return;
    m_retiredRowsScheduled = true;
    QTimer::singleShot(0, this, [this] { deleteRetiredRows(); });
}

void ClaudeTranscriptView::deleteRetiredRows()
{
    m_retiredRowsScheduled = false;
    QElapsedTimer budget;
    budget.start();
    while (!m_retiredRows.isEmpty() && budget.elapsed() < 8) {
        const QPointer<QWidget> row = m_retiredRows.takeLast();
        delete row.data();
    }
    if (m_retiredRows.isEmpty() || m_retiredRowsScheduled)
        return;
    m_retiredRowsScheduled = true;
    QTimer::singleShot(0, this, [this] { deleteRetiredRows(); });
}

QString ClaudeTranscriptView::accentFor(const QString &name) const
{
    if (name == QLatin1String("Bash"))
        return m_p.add;
    if (name == QLatin1String("Edit") || name == QLatin1String("Write")
        || name == QLatin1String("MultiEdit") || name == QLatin1String("NotebookEdit"))
        return m_p.accent;
    if (name == QLatin1String("Read") || name == QLatin1String("Grep")
        || name == QLatin1String("Glob"))
        return m_p.muted;
    if (forkmesh::transcripttools::isSubagentTool(name))
        return Theme::nextOrHex(Theme::Role::FgMuted, QStringLiteral("#a371f7"));
    if (name == QLatin1String("TodoWrite"))
        return Theme::nextOrHex(Theme::Role::AccentFg, QStringLiteral("#d29922"));
    return m_p.accent;
}

QWidget *ClaudeTranscriptView::addRow(QWidget *card, const RowStyle &style)
{
    if (!m_keepExplore) {
        m_exploreBlock = nullptr;
        m_exploreLines = nullptr;
    }
    int pos;
    if (m_prependAt >= 0) {
        pos = m_prependAt++;
    } else {
        pos = m_col->count() - 1; // before the trailing spacer
        if (m_activity) {
            const int ai = m_col->indexOf(m_activity);
            if (ai >= 0)
                pos = ai;
        }
    }
    RowStyle resolved = style;
    if (resolved.nodeColor.isEmpty())
        resolved.nodeColor = m_p.muted;
    std::function<void()> onActivate;
    if (!m_rowSource.isEmpty() && card != m_activityLabel)
        onActivate = [this, source = m_rowSource] { emit rawLogRequested(source); };
    auto *item = new RailItem(card, resolved, std::move(onActivate));
    const QMargins margins = m_col->contentsMargins();
    const int rowWidth = m_container->width() - margins.left() - margins.right();
    if (rowWidth > 0)
        item->resize(rowWidth, item->height());
    m_col->insertWidget(pos, item);
    fadeIn(item);
    return item;
}

QWidget *ClaudeTranscriptView::addRow(QWidget *card, const QString &nodeColor,
                                      RowGlyph glyph)
{
    RowStyle style;
    style.nodeColor = nodeColor;
    style.glyph = glyph;
    return addRow(card, style);
}

ClaudeTranscriptView::RowStyle ClaudeTranscriptView::speakerStyle(bool user) const
{
    RowStyle style;
    style.glyph = GlyphNone;
    style.nodeColor = user ? m_p.text : m_p.accent;
    style.plate = user ? m_p.userBg : m_p.aiBg;
    style.avatar = user ? m_userAvatar : m_agentAvatar;
    style.avatarRing = user ? m_p.userEdge : m_p.accent;
    // No portrait yet (nothing signed in, an unknown model): one letter still
    // tells the two speakers apart.
    const QString name = user ? m_userName : m_agentName;
    style.monogram = name.isEmpty()
                         ? (user ? QStringLiteral("Y") : QStringLiteral("✻"))
                         : name.left(1).toUpper();
    return style;
}

QWidget *ClaudeTranscriptView::addAgentRow(QWidget *card)
{
    return addRow(card, speakerStyle(false));
}

QWidget *ClaudeTranscriptView::addUserRow(QWidget *card)
{
    return addRow(card, speakerStyle(true));
}

void ClaudeTranscriptView::setSpeakerAvatars(const QPixmap &agent,
                                             const QString &agentName,
                                             const QPixmap &user,
                                             const QString &userName)
{
    m_agentAvatar = agent;
    m_agentName = agentName.trimmed();
    m_userAvatar = user;
    m_userName = userName.trimmed();
}

void ClaudeTranscriptView::accumulateStatsOnly(const QJsonObject &ev)
{
    noteCoverage(ev);
    const QString type = ev.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("_local_user") && m_promptDeck)
        m_promptDeck->addPrompt(ev.value(QStringLiteral("text")).toString());
    if (type == QLatin1String("assistant")) {
        m_totalTokens += m_outputTokens.add(
            ev.value(QStringLiteral("message")).toObject());
    } else if (type == QLatin1String("result")) {
        const double cost = ev.value(QStringLiteral("total_cost_usd")).toDouble();
        if (cost > 0)
            m_totalCost = cost; // result carries the run's cumulative cost
    } else if (type == QLatin1String("_codex_usage")) {
        m_totalTokens = static_cast<qint64>(
            ev.value(QStringLiteral("total_tokens")).toDouble());
    }
}

void ClaudeTranscriptView::addSkippedNotice(int count)
{
    if (m_skippedNotice) {
        m_col->removeWidget(m_skippedNotice);
        m_skippedNotice->deleteLater();
        m_skippedNotice = nullptr;
    }
    m_skippedCount = count;
    auto *btn = new QPushButton(
        QStringLiteral("Load %1 earlier event%2")
            .arg(count)
            .arg(count == 1 ? QString() : QStringLiteral("s")));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFont(monoFont());
    btn->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "QPushButton{border:none;background:transparent;text-align:left;"
        "color:%1;padding:0;}QPushButton:hover{color:%2;}")
        .arg(m_p.muted, m_p.accent)));
    connect(btn, &QPushButton::clicked, this, [this] {
        if (m_loadEarlierPending)
            return;
        m_loadEarlierPending = true;
        emit loadEarlierRequested();
    });
    m_skippedNotice = addRow(btn, QString(), GlyphNone);
    emit statsChanged(m_totalTokens, m_totalCost);
}

void ClaudeTranscriptView::prependEarlierEvents(const QList<QJsonObject> &events,
                                                int stillSkipped)
{
    flushStreamingText(); // the live rows are about to be set aside
    m_loadEarlierPending = false;
    if (events.isEmpty()) {
        if (stillSkipped <= 0 && m_skippedNotice) {
            m_col->removeWidget(m_skippedNotice);
            m_skippedNotice->deleteLater();
            m_skippedNotice = nullptr;
            m_skippedCount = 0;
        }
        return;
    }

    QWidget *liveActivity = m_activity;
    QLabel *liveActivityLabel = m_activityLabel;
    QTimer *liveActivityTimer = m_activityTimer;
    Collapsible *liveThinking = m_liveThinking;
    QLabel *liveThinkingBody = m_thinkingBody;
    QString liveThinkingText = m_thinkingText;
    int liveThinkingTokens = m_thinkingTokens;
    qint64 liveThinkingStart = m_thinkingStartMs;
    m_activity = nullptr;
    m_activityLabel = nullptr;
    m_activityTimer = nullptr; // ensureActivity() allocates its own scratch timer
    m_liveThinking = nullptr;
    m_thinkingBody = nullptr;
    m_thinkingText.clear();
    m_thinkingTokens = 0;
    m_thinkingStartMs = 0;

    setBatchHoldsFrame(true, true);
    setBulkPopulate(true); // no per-row fade-in for a whole batch landing at once
    m_prependAt = 0;
    m_approvalPrependAt = 0;
    m_exploreBlock = nullptr;
    m_exploreLines = nullptr;
    if (stillSkipped > 0) {
        addSkippedNotice(stillSkipped); // replaces the old notice, at the prepend head
    } else if (m_skippedNotice) {
        m_col->removeWidget(m_skippedNotice);
        m_skippedNotice->deleteLater();
        m_skippedNotice = nullptr;
        m_skippedCount = 0;
    }
    for (const QJsonObject &ev : events) {
        if (ev.value(QStringLiteral("type")).toString() == QLatin1String("_local_user"))
            addUserTurn(ev.value(QStringLiteral("text")).toString(), eventTime(ev));
        else
            handleEvent(ev, /*countStats=*/false); // already folded into totals
    }
    m_prependAt = -1;
    m_approvalPrependAt = -1;
    setBulkPopulate(false);
    m_exploreBlock = nullptr; // the batch's last block ends with the batch
    m_exploreLines = nullptr;

    if (m_activity) {
        m_col->removeWidget(m_activity);
        m_activity->deleteLater();
    }
    if (m_activityTimer) {
        m_activityTimer->stop();
        m_activityTimer->deleteLater();
    }
    if (m_liveThinking) {
        m_liveThinking->setPulsing(false);
        m_liveThinking->setExpanded(false);
    }

    m_activity = liveActivity;
    m_activityLabel = liveActivityLabel;
    m_activityTimer = liveActivityTimer;
    m_liveThinking = liveThinking;
    m_thinkingBody = liveThinkingBody;
    m_thinkingText = liveThinkingText;
    m_thinkingTokens = liveThinkingTokens;
    m_thinkingStartMs = liveThinkingStart;
}

void ClaudeTranscriptView::fadeIn(QWidget *card)
{
    if (m_bulkPopulate)
        return; // replaying a stored session: rows appear at once, no per-row anim
    // An opacity effect renders its widget into an offscreen pixmap on every
    // frame. One row arriving is a nice touch; a whole turn's worth landing at
    // once turns the same trick into the stutter it was meant to soften. Rate
    // limit rather than count in flight: a burst fades its leading row and the
    // rest simply appear, and nothing has to survive a row being deleted
    // mid-animation for the budget to come back.
    constexpr qint64 kFadeSpacingMs = 90;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastFadeMs < kFadeSpacingMs)
        return;
    m_lastFadeMs = now;
    auto *eff = new QGraphicsOpacityEffect(card);
    card->setGraphicsEffect(eff);
    auto *a = new QPropertyAnimation(eff, "opacity", card);
    a->setDuration(220);
    a->setStartValue(0.0);
    a->setEndValue(1.0);
    a->setEasingCurve(QEasingCurve::OutCubic);
    QPointer<QWidget> pc = card;
    connect(a, &QPropertyAnimation::finished, card, [pc] {
        if (pc)
            pc->setGraphicsEffect(nullptr); // drop the effect once shown
    });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void ClaudeTranscriptView::smoothScrollTo(int value)
{
    QScrollBar *sb = verticalScrollBar();
    value = qBound(sb->minimum(), value, sb->maximum());
    if (!m_scrollAnim) {
        m_scrollAnim = new QPropertyAnimation(sb, "value", this);
        m_scrollAnim->setDuration(220);
        m_scrollAnim->setEasingCurve(QEasingCurve::OutCubic);
    }
    m_scrollAnim->stop();
    m_scrollAnim->setStartValue(sb->value());
    m_scrollAnim->setEndValue(value);
    m_scrollAnim->start();
}

void ClaudeTranscriptView::handleEvent(const QJsonObject &ev, bool countStats)
{
    if (m_prependAt < 0)
        noteCoverage(ev);
    m_eventTime = eventTime(ev);
    const auto restoreRowSource =
        qScopeGuard([this, outer = std::exchange(m_rowSource, ev)] {
            m_rowSource = outer;
        });
    const QString type = ev.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("system")) {
        const QString sub = ev.value(QStringLiteral("subtype")).toString();
        if (sub == QLatin1String("init")) {
            // What the CLI announces (model, cwd) plus the run's own context —
            // branch, permission mode, reasoning strength — which only the host
            // knows. Empty parts drop out rather than leaving gaps.
            QStringList parts{ev.value(QStringLiteral("model")).toString().trimmed(),
                              ev.value(QStringLiteral("cwd")).toString().trimmed()};
            QStringList context;
            if (!m_ctxBranch.isEmpty())
                context << QStringLiteral("branch %1").arg(m_ctxBranch);
            const QString mode =
                m_ctxMode.isEmpty()
                    ? permissionModeLabel(
                          ev.value(QStringLiteral("permissionMode")).toString().trimmed())
                    : m_ctxMode;
            if (!mode.isEmpty())
                context << mode;
            if (!m_ctxStrength.isEmpty())
                context << QStringLiteral("%1 thinking").arg(m_ctxStrength);
            if (!m_ctxAccount.isEmpty())
                context << (m_ctxAccount.contains(QStringLiteral("account"),
                                                  Qt::CaseInsensitive)
                                ? m_ctxAccount
                                : QStringLiteral("account %1").arg(m_ctxAccount));
            parts.removeAll(QString());
            QString line = QStringLiteral("session started ") + parts.join(QLatin1Char(' '));
            if (!context.isEmpty())
                line += QStringLiteral(" · ") + context.join(QStringLiteral(" · "));
            QString lineHtml = referenceHtml(line);
            if (!m_ctxBranch.isEmpty()) {
                const QString encoded = QString::fromLatin1(
                    QUrl::toPercentEncoding(m_ctxBranch));
                const QString href = QStringLiteral("forkmesh-branch:%1").arg(encoded);
                if (!lineHtml.contains(href)) {
                    const QString needle = esc(
                        QStringLiteral("branch %1").arg(m_ctxBranch));
                    const QString replacement = QStringLiteral(
                        "branch <a href=\"%1\">%2</a>")
                                                    .arg(href, esc(m_ctxBranch));
                    lineHtml.replace(needle, replacement);
                }
            }
            auto *l = new QLabel(lineHtml);
            l->setTextFormat(Qt::RichText);
            l->setWordWrap(true); // long branch names shouldn't widen the view
            enableReferenceLinks(l);
            l->setFont(monoFont());
            l->setStyleSheet(Theme::remapSheet(QStringLiteral("color:%1;background:transparent;").arg(m_p.muted)));
            addRow(l, QString(), GlyphNone);
        } else if (sub == QLatin1String("thinking_tokens")) {
            ensureLiveThinking();
            setThinkingTokens(ev.value(QStringLiteral("estimated_tokens")).toInt());
        } else if (forkmesh::transcripttools::parentToolUseId(ev).isEmpty()) {
            const bool agentDone =
                sub == QLatin1String("task_notification") &&
                ev.contains(QStringLiteral("usage")) &&
                addAgentTaskNotification(
                    ev.value(QStringLiteral("tool_use_id")).toString(),
                    ev.value(QStringLiteral("status")).toString().trimmed().toLower(),
                    ev.value(QStringLiteral("summary")).toString());
            const QString notice =
                agentDone ? QString() : forkmesh::transcripttools::systemNotice(ev);
            if (!notice.isEmpty())
                addNotice(notice);
        }
    } else if (type == QLatin1String("stream_event")) {
        if (forkmesh::transcripttools::parentToolUseId(ev).isEmpty())
            handleClaudeStreamEvent(ev.value(QStringLiteral("event")).toObject());
    } else if (type == QLatin1String("assistant")) {
        const QJsonObject message = ev.value(QStringLiteral("message")).toObject();
        const qint64 out = m_outputTokens.add(message);
        if (out > 0 && countStats) {
            m_totalTokens += out;
            emit statsChanged(m_totalTokens, m_totalCost);
        }
        const QString parent = forkmesh::transcripttools::parentToolUseId(ev);
        if (!parent.isEmpty())
            noteSubagentActivity(parent, message);
        else
            addAssistantBlocks(message);
    } else if (type == QLatin1String("user")) {
        if (!forkmesh::transcripttools::parentToolUseId(ev).isEmpty() ||
            ev.value(QStringLiteral("isSidechain")).toBool())
            return;
        finalizeThinking(QString());
        const QJsonValue contentValue = ev.value(QStringLiteral("message")).toObject()
                                            .value(QStringLiteral("content"));
        if (contentValue.isString()) {
            const QString text = contentValue.toString();
            if (AgentLogText::isTaskNotificationText(text))
                addTaskNotificationText(text);
            return;
        }
        for (const QJsonValue &bv : contentValue.toArray()) {
            const QJsonObject b = bv.toObject();
            const QString blockType = b.value(QStringLiteral("type")).toString();
            if (blockType == QLatin1String("tool_result")) {
                addToolResult(b.value(QStringLiteral("tool_use_id")).toString(),
                              forkmesh::transcripttools::toolResultText(
                                  b.value(QStringLiteral("content"))),
                              b.value(QStringLiteral("is_error")).toBool());
            } else if (blockType == QLatin1String("text")) {
                const QString text = b.value(QStringLiteral("text")).toString();
                if (AgentLogText::isTaskNotificationText(text))
                    addTaskNotificationText(text);
            }
        }
        if (!contentValue.toArray().isEmpty())
            ensureActivity(); // a tool came back; the agent keeps going
    } else if (type == QLatin1String("result")) {
        finalizeThinking(QString());
        flushStreamingText(); // nothing more is coming; show it all now
        m_pendingStreamText.clear();
        m_streamTextKeys.clear();
        m_streamMessageId.clear();
        clearActivity(); // the turn is done
        const double cost = ev.value(QStringLiteral("total_cost_usd")).toDouble();
        if (cost > 0 && countStats) {
            m_totalCost = cost; // result carries the run's cumulative cost
            emit statsChanged(m_totalTokens, m_totalCost);
        }
        addResult(ev);
    } else if (type == QLatin1String("_codex_agent_delta")) {
        finalizeThinking(QString());
        const QString delta = ev.value(QStringLiteral("delta")).toString(
            ev.value(QStringLiteral("text")).toString());
        appendAgentText(ev.value(QStringLiteral("item_id")).toString(),
                        delta);
    } else if (type == QLatin1String("_codex_agent_complete")) {
        finalizeThinking(QString());
        completeAgentText(ev.value(QStringLiteral("item_id")).toString(),
                          ev.value(QStringLiteral("text")).toString());
        clearActivity();
    } else if (type == QLatin1String("_codex_reasoning_complete")) {
        finalizeThinking(ev.value(QStringLiteral("text")).toString());
    } else if (type == QLatin1String("_codex_tool_delta")) {
        appendToolOutput(ev.value(QStringLiteral("item_id")).toString(),
                         ev.value(QStringLiteral("delta")).toString());
    } else if (type == QLatin1String("_codex_usage")) {
        if (countStats) {
            m_totalTokens = static_cast<qint64>(
                ev.value(QStringLiteral("total_tokens")).toDouble());
            emit statsChanged(m_totalTokens, m_totalCost);
        }
    } else if (type == QLatin1String("_local_ask_answer")) {
        markAskAnswered(ev.value(QStringLiteral("tool_use_id")).toString(),
                        ev.value(QStringLiteral("text")).toString());
    } else if (type == QLatin1String("_local_notice")) {
        addNotice(ev.value(QStringLiteral("text")).toString(),
                  ev.value(QStringLiteral("level")).toString() ==
                      QLatin1String("error"));
    }
}

void ClaudeTranscriptView::addNotice(const QString &text, bool error)
{
    if (text.trimmed().isEmpty())
        return;
    auto *l = new QLabel(referenceHtml(text));
    l->setTextFormat(Qt::RichText);
    l->setWordWrap(true);
    enableReferenceLinks(l);
    l->setFont(monoFont());
    l->setStyleSheet(
        QStringLiteral("color:%1;background:transparent;").arg(error ? m_p.del : m_p.muted));
    addRow(l, error ? m_p.del : QString(), GlyphNone);
}

void ClaudeTranscriptView::handleClaudeStreamEvent(const QJsonObject &e)
{
    const QString eventType = e.value(QStringLiteral("type")).toString();
    if (eventType == QLatin1String("message_start")) {
        m_streamMessageId = e.value(QStringLiteral("message")).toObject()
                                .value(QStringLiteral("id")).toString();
        m_streamMessageAt = m_eventTime;
        m_streamTextKeys.clear();
        return;
    }
    const int index = e.value(QStringLiteral("index")).toInt(-1);
    if (eventType == QLatin1String("content_block_start")) {
        const QJsonObject block = e.value(QStringLiteral("content_block")).toObject();
        if (block.value(QStringLiteral("type")).toString() != QLatin1String("text") ||
            m_streamMessageId.isEmpty() || index < 0)
            return;
        const QString key =
            QStringLiteral("claude:%1:%2").arg(m_streamMessageId).arg(index);
        m_streamTextKeys.insert(index, key);
        m_pendingStreamText.append(qMakePair(m_streamMessageId, key));
        if (m_pendingStreamText.size() > 64)
            m_pendingStreamText.removeFirst();
        return;
    }
    if (eventType != QLatin1String("content_block_delta"))
        return;
    const QJsonObject d = e.value(QStringLiteral("delta")).toObject();
    const QString deltaType = d.value(QStringLiteral("type")).toString();
    if (deltaType == QLatin1String("thinking_delta")) {
        ensureLiveThinking();
        appendThinkingDelta(d.value(QStringLiteral("thinking")).toString());
        if (d.contains(QStringLiteral("estimated_tokens")))
            setThinkingTokens(d.value(QStringLiteral("estimated_tokens")).toInt());
    } else if (deltaType == QLatin1String("text_delta")) {
        const QString key = m_streamTextKeys.value(index);
        const QString text = d.value(QStringLiteral("text")).toString();
        if (key.isEmpty() || text.isEmpty())
            return;
        finalizeThinking(QString());
        clearActivity();
        if (!m_eventTime.isValid())
            m_eventTime = m_streamMessageAt;
        appendAgentText(key, text);
    }
}

bool ClaudeTranscriptView::completeStreamedText(const QString &messageId,
                                                const QString &text)
{
    if (messageId.isEmpty())
        return false;
    for (int i = 0; i < m_pendingStreamText.size(); ++i) {
        if (m_pendingStreamText.at(i).first != messageId)
            continue;
        const QString key = m_pendingStreamText.takeAt(i).second;
        for (auto it = m_streamTextKeys.begin(); it != m_streamTextKeys.end();) {
            if (it.value() == key)
                it = m_streamTextKeys.erase(it);
            else
                ++it;
        }
        if (!m_liveAgentText.value(key))
            return false;
        QStringList options;
        if (parseInlineChoices(text, options)) {
            dropLiveAgentText(key);
            return false;
        }
        completeAgentText(key, text);
        stampSpeakerCaption(m_liveAgentRows.value(key), m_eventTime);
        m_liveAgentText.remove(key);
        m_liveAgentTextValue.remove(key);
        m_liveAgentRows.remove(key);
        return true;
    }
    return false;
}

void ClaudeTranscriptView::dropLiveAgentText(const QString &key)
{
    m_dirtyAgentText.remove(key);
    m_liveAgentText.remove(key);
    m_liveAgentTextValue.remove(key);
    if (QPointer<QWidget> row = m_liveAgentRows.take(key)) {
        m_col->removeWidget(row);
        row->hide();
        row->deleteLater();
    }
}

void ClaudeTranscriptView::noteSubagentActivity(const QString &parentId,
                                                const QJsonObject &message)
{
    auto it = m_toolCards.find(parentId);
    if (it == m_toolCards.end() || !it->io)
        return;
    ToolCard &tc = it.value();
    QString last;
    for (const QJsonValue &bv : message.value(QStringLiteral("content")).toArray()) {
        const QJsonObject b = bv.toObject();
        if (b.value(QStringLiteral("type")).toString() != QLatin1String("tool_use"))
            continue;
        const QString name = b.value(QStringLiteral("name")).toString();
        const QJsonObject input = b.value(QStringLiteral("input")).toObject();
        ++tc.subagentTools;
        last = forkmesh::transcripttools::displayName(name, input);
        const QString detail = forkmesh::transcripttools::subtitle(name, input).simplified();
        if (!detail.isEmpty())
            last += QLatin1Char(' ') + detail;
    }
    if (last.isEmpty())
        return;
    if (last.size() > 140)
        last = last.left(139) + QStringLiteral("…");
    if (!tc.subagentLine) {
        auto *line = new QLabel;
        line->setTextFormat(Qt::PlainText);
        line->setWordWrap(true);
        line->setFont(monoFont());
        line->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;")
                                .arg(m_p.muted));
        tc.io->addWidget(connectorRow(line));
        tc.subagentLine = line;
    }
    tc.subagentLine->setText(
        QStringLiteral("↳ %1 tool call%2 · %3")
            .arg(QLocale().toString(tc.subagentTools),
                 tc.subagentTools == 1 ? QString() : QStringLiteral("s"), last));
}

void ClaudeTranscriptView::showAgentReport(ToolCard &tc, const QString &report,
                                           const QString &status)
{
    if (!tc.io)
        return;
    for (QPointer<QWidget> *old : {&tc.agentState, &tc.agentSummary}) {
        if (QWidget *row = old->data()) {
            tc.io->removeWidget(row);
            row->hide();
            row->deleteLater();
        }
        *old = nullptr;
    }
    const bool failed = status == QLatin1String("failed") || status == QLatin1String("error");
    const bool stopped = !failed && !status.isEmpty() && status != QLatin1String("completed");
    auto *box = new QWidget;
    box->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    auto *caption = new QLabel(failed    ? QStringLiteral("Failed · summary")
                               : stopped ? QStringLiteral("Summary · %1").arg(status)
                                         : QStringLiteral("Summary"));
    caption->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;background:transparent;border:none;")
            .arg(failed ? m_p.del : m_p.muted));
    auto *head = new QWidget;
    head->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *h = new QHBoxLayout(head);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    h->addWidget(caption);
    h->addStretch(1);
    v->addWidget(head);
    const auto markdownLabel = [this](const QString &markdown) {
        auto *l = new CacheLabel;
        l->setTextFormat(Qt::MarkdownText);
        l->setText(linkifyReferences(markdown));
        l->setWordWrap(true);
        enableReferenceLinks(l);
        l->setFont(monoFont());
        l->setStyleSheet(
            QStringLiteral("color:%1;background:transparent;border:none;").arg(m_p.text));
        return l;
    };
    const QString summary = forkmesh::transcripttools::reportSummary(report);
    QLabel *summaryLabel = markdownLabel(summary);
    summaryLabel->setProperty(kProseMarkdown, report);
    addProseCopyButton(head, summaryLabel, m_p);
    v->addWidget(summaryLabel);
    if (summary.simplified() != report.simplified())
        v->addWidget(makeCollapsible(QStringLiteral("Full report"), markdownLabel(report), false));
    tc.agentSummary = connectorRow(box);
    tc.io->addWidget(tc.agentSummary);
}

bool ClaudeTranscriptView::addAgentTaskNotification(const QString &toolUseId,
                                                    const QString &status,
                                                    const QString &report)
{
    if (toolUseId.isEmpty())
        return false;
    auto it = m_toolCards.find(toolUseId);
    if (it == m_toolCards.end() || !it->io ||
        !forkmesh::transcripttools::isSubagentTool(it->toolName))
        return false;
    const QString body = forkmesh::transcripttools::agentReport(report);
    if (body.trimmed().isEmpty())
        return false;
    ToolCard &tc = it.value();
    tc.hasResult = true;
    showAgentReport(tc, body, status);
    const QString done = status.isEmpty() || status == QLatin1String("completed")
                             ? QStringLiteral("finished")
                             : status;
    const QString who = tc.agentTitle.isEmpty()
                            ? QStringLiteral("Agent")
                            : QStringLiteral("Agent “%1”").arg(tc.agentTitle);
    addNotice(QStringLiteral("%1 %2: %3")
                  .arg(who, done,
                       forkmesh::transcripttools::plainSummary(
                           forkmesh::transcripttools::reportSummary(body))));
    return true;
}

void ClaudeTranscriptView::addTaskNotificationText(const QString &text)
{
    QStringList notices;
    for (const auto &n : forkmesh::transcripttools::taskNotifications(text)) {
        if (!n.result.trimmed().isEmpty() &&
            addAgentTaskNotification(n.toolUseId, n.status, n.result))
            continue;
        QString notice = QStringLiteral("Task %1: %2")
                             .arg(n.status.isEmpty() ? QStringLiteral("update") : n.status,
                                  n.summary.isEmpty() ? QStringLiteral("Background task")
                                                      : n.summary);
        if (!n.result.trimmed().isEmpty())
            notice += QLatin1Char('\n') +
                      forkmesh::transcripttools::plainSummary(
                          forkmesh::transcripttools::reportSummary(
                              forkmesh::transcripttools::agentReport(n.result)),
                          420);
        else if (!n.event.trimmed().isEmpty())
            notice += QLatin1Char('\n') + n.event.trimmed();
        notices << notice;
    }
    if (!notices.isEmpty())
        addNotice(notices.join(QStringLiteral("\n\n")));
}

void ClaudeTranscriptView::appendAgentText(const QString &id, const QString &text)
{
    if (id.isEmpty() || text.isEmpty())
        return;
    QPointer<QLabel> label = m_liveAgentText.value(id);
    if (!label) {
        auto *created = new CacheLabel;
        created->setTextFormat(Qt::MarkdownText);
        created->setWordWrap(true);
        enableReferenceLinks(created);
        created->setFont(Theme::isNext() ? proseFont() : monoFont());
        created->setStyleSheet(Theme::remapSheet(
            QStringLiteral("color:%1;background:transparent;border:none;")
                .arg(m_p.text)));
        QWidget *caption = speakerCaption(false, m_eventTime);
        addProseCopyButton(caption, created, m_p);
        QWidget *row = addAgentRow(speakerBody(caption, created));
        label = created;
        m_liveAgentText.insert(id, label);
        m_liveAgentRows.insert(id, row);
    }
    if (label->textFormat() != Qt::MarkdownText)
        label->setTextFormat(Qt::MarkdownText);
    m_liveAgentTextValue[id] += text;
    label->setProperty(kProseMarkdown, m_liveAgentTextValue.value(id));
    m_dirtyAgentText.insert(id);
    markTextDirty();
}

void ClaudeTranscriptView::completeAgentText(const QString &id,
                                             const QString &text)
{
    QPointer<QLabel> label = m_liveAgentText.value(id);
    if (label) {
        const QString complete = text.isEmpty() ? m_liveAgentTextValue.value(id) : text;
        m_dirtyAgentText.remove(id); // this call writes the final text itself
        if (Theme::isNext()) {
            label->setTextFormat(Qt::RichText);
            label->setText(nextProseHtml(linkifyReferences(complete), m_p.chipBg));
        } else {
            label->setText(linkifyReferences(complete));
        }
        label->setProperty(kProseMarkdown, complete);
        m_liveAgentTextValue[id] = complete;
        return;
    }
    if (!text.trimmed().isEmpty())
        addAssistantText(text);
}

void ClaudeTranscriptView::addAssistantBlocks(const QJsonObject &message)
{
    const QJsonArray content = message.value(QStringLiteral("content")).toArray();
    const QString messageId = message.value(QStringLiteral("id")).toString();
    QString thinkingText;
    bool hasThinking = false;
    for (const QJsonValue &bv : content) {
        const QString blockType = bv.toObject().value(QStringLiteral("type")).toString();
        if (blockType == QLatin1String("thinking")) {
            hasThinking = true;
            thinkingText += bv.toObject().value(QStringLiteral("thinking")).toString();
        } else if (blockType == QLatin1String("redacted_thinking")) {
            hasThinking = true;
        }
    }
    if (hasThinking)
        finalizeThinking(thinkingText);
    else if (m_liveThinking)
        finalizeThinking(QString());

    bool hadTool = false;
    bool askedQuestion = false;
    for (const QJsonValue &bv : content) {
        const QJsonObject b = bv.toObject();
        const QString t = b.value(QStringLiteral("type")).toString();
        if (t == QLatin1String("text")) {
            const QString text = b.value(QStringLiteral("text")).toString();
            if (!completeStreamedText(messageId, text) && !text.trimmed().isEmpty() &&
                addAssistantText(text))
                askedQuestion = true;
        } else if (forkmesh::transcripttools::isServerToolResultBlock(t)) {
            bool isError = false;
            const QString text = forkmesh::transcripttools::serverToolResultText(b, &isError);
            addToolResult(b.value(QStringLiteral("tool_use_id")).toString(), text, isError);
        } else if (t == QLatin1String("tool_use") || t == QLatin1String("server_tool_use") ||
                   t == QLatin1String("mcp_tool_use")) {
            hadTool = true;
            QString name = b.value(QStringLiteral("name")).toString();
            if (t == QLatin1String("mcp_tool_use"))
                name = QStringLiteral("mcp__%1__%2")
                           .arg(b.value(QStringLiteral("server_name")).toString(), name);
            if (name == QLatin1String("AskUserQuestion")) {
                askedQuestion = true;
                addAskUserQuestion(b.value(QStringLiteral("id")).toString(),
                                   b.value(QStringLiteral("input")).toObject());
            } else {
                addToolUse(b.value(QStringLiteral("id")).toString(), name,
                           b.value(QStringLiteral("input")).toObject());
            }
        }
    }
    if (askedQuestion)
        clearActivity();
    else if (hadTool)
        ensureActivity();
    else
        clearActivity();
}

void ClaudeTranscriptView::ensureLiveThinking()
{
    if (m_liveThinking)
        return;
    clearActivity(); // the thinking card becomes the live indicator
    m_thinkingText.clear();
    m_lastFinalizedThinkingText.clear();
    m_thinkingTokens = 0;
    m_thinkingStartMs = QDateTime::currentMSecsSinceEpoch();
    m_thinkingBody = new CacheLabel(QStringLiteral("…"));
    m_thinkingBody->setTextFormat(Qt::RichText);
    m_thinkingBody->setWordWrap(true);
    enableReferenceLinks(m_thinkingBody);
    m_thinkingBody->setFont(monoFont());
    m_thinkingBody->setStyleSheet(Theme::remapSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(m_p.muted)));
    m_liveThinking = makeCollapsible(QStringLiteral("Thinking…"), m_thinkingBody, false);
    m_liveThinking->setPulsing(true);
    addRow(m_liveThinking, m_p.accent, GlyphStar);
}

void ClaudeTranscriptView::setThinkingTokens(int tokens)
{
    m_thinkingTokens = tokens;
    if (m_liveThinking)
        m_liveThinking->setHeaderText(
            QStringLiteral("Thinking… ~%1 tokens").arg(QLocale().toString(tokens)));
}

void ClaudeTranscriptView::appendThinkingDelta(const QString &text)
{
    if (text.isEmpty())
        return;
    m_thinkingText += text;
    m_dirtyThinking = true;
    markTextDirty();
    const int est = qMax(1, m_thinkingText.length() / 4);
    if (m_liveThinking)
        m_liveThinking->setHeaderText(
            QStringLiteral("Thinking… ~%1 tokens").arg(QLocale().toString(est)));
}

// A streamed turn arrives as hundreds of deltas a second. Reshaping a rich-text
// label per delta pegged the GUI thread; batching them into one repaint every
// 60ms is invisible to read and costs a fraction of the layout work.
void ClaudeTranscriptView::markTextDirty()
{
    if (!m_textFlushTimer) {
        m_textFlushTimer = new QTimer(this);
        m_textFlushTimer->setSingleShot(true);
        m_textFlushTimer->setInterval(60);
        connect(m_textFlushTimer, &QTimer::timeout, this,
                &ClaudeTranscriptView::flushStreamingText);
    }
    if (m_textFlushTimer->isActive())
        return;
    // Each flush re-linkifies and re-lays out the whole accumulated text of
    // every dirty label, so at a fixed 60 ms a long answer costs more per
    // frame the longer it gets (quadratic across the turn). Back the cadence
    // off with the size: short replies still update at 60 ms, a 100 KB reply
    // at ~260 ms, capped at 400 ms — nobody reads a wall of text at 16 Hz.
    qsizetype pending = m_dirtyThinking ? m_thinkingText.size() : 0;
    for (const QString &id : std::as_const(m_dirtyAgentText))
        pending = qMax(pending, m_liveAgentTextValue.value(id).size());
    for (const QString &id : std::as_const(m_dirtyToolOutput)) {
        const auto it = m_toolCards.constFind(id);
        if (it != m_toolCards.cend())
            pending = qMax(pending, it.value().liveOutputText.size());
    }
    m_textFlushTimer->setInterval(
        qBound(60, 60 + int(qMin<qsizetype>(pending, 400000) / 500), 400));
    m_textFlushTimer->start();
}

void ClaudeTranscriptView::flushStreamingText()
{
    if (m_textFlushTimer)
        m_textFlushTimer->stop();
    if (m_dirtyThinking) {
        m_dirtyThinking = false;
        if (m_thinkingBody)
            m_thinkingBody->setText(referenceHtml(m_thinkingText));
    }
    for (const QString &id : std::as_const(m_dirtyAgentText)) {
        if (QPointer<QLabel> label = m_liveAgentText.value(id))
            label->setText(linkifyReferences(m_liveAgentTextValue.value(id)));
    }
    m_dirtyAgentText.clear();
    for (const QString &id : std::as_const(m_dirtyToolOutput)) {
        auto it = m_toolCards.find(id);
        if (it == m_toolCards.end())
            continue;
        if (it.value().terminalOutput)
            static_cast<TerminalOutputView *>(it.value().terminalOutput.data())
                ->setOutputText(it.value().liveOutputText);
        else if (it.value().liveOutput)
            it.value().liveOutput->setText(
                capLabelText(it.value().liveOutputText, true));
    }
    m_dirtyToolOutput.clear();
}

void ClaudeTranscriptView::finalizeThinking(const QString &fullText)
{
    m_dirtyThinking = false; // this call writes the body's final text itself
    const QString completed = fullText.trimmed();
    if (!m_liveThinking) {
        if (completed.isEmpty() || completed == m_lastFinalizedThinkingText)
            return;
        auto *body = new CacheLabel(referenceHtml(completed));
        body->setTextFormat(Qt::RichText);
        body->setWordWrap(true);
        enableReferenceLinks(body);
        body->setFont(monoFont());
        body->setStyleSheet(Theme::remapSheet(
            QStringLiteral("color:%1;background:transparent;border:none;")
                .arg(m_p.muted)));
        addRow(makeCollapsible(QStringLiteral("Thought"), body, false), m_p.accent,
               GlyphStar);
        m_lastFinalizedThinkingText = completed;
        return;
    }
    m_liveThinking->setPulsing(false);
    const qint64 secs = m_thinkingStartMs > 0
        ? (QDateTime::currentMSecsSinceEpoch() - m_thinkingStartMs) / 1000
        : 0;
    const QString label = secs > 0
        ? QStringLiteral("Thought for %1s").arg(secs)
        : QStringLiteral("Thought");

    const QString finalText = completed.isEmpty() ? m_thinkingText.trimmed() : completed;
    m_liveThinking->setHeaderText(label);
    m_liveThinking->setExpanded(false);
    if (m_thinkingBody)
        m_thinkingBody->setText(referenceHtml(finalText));
    m_lastFinalizedThinkingText = finalText;
    m_liveThinking = nullptr;
    m_thinkingBody = nullptr;
    m_thinkingText.clear();
    m_thinkingTokens = 0;
    m_thinkingStartMs = 0;
}

Collapsible *ClaudeTranscriptView::makeCollapsible(const QString &header,
                                                  QWidget *body, bool expanded)
{
    auto *c = new Collapsible(header, expanded, m_p);
    if (body)
        c->body()->addWidget(body);
    return c;
}

QString ClaudeTranscriptView::linkifyReferences(const QString &markdown)
{
    if (markdown.isEmpty())
        return markdown;
    QString out;
    out.reserve(markdown.size() + 48);
    bool inFence = false;
    QString fenceMarker;
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        if (i > 0)
            out += QLatin1Char('\n');
        const QString &line = lines.at(i);
        const QString trimmed = line.trimmed();
        const bool fenceLine = trimmed.startsWith(QStringLiteral("```")) ||
                               trimmed.startsWith(QStringLiteral("~~~"));
        if (inFence) {
            out += line;
            if (fenceLine && trimmed.startsWith(fenceMarker))
                inFence = false;
        } else if (fenceLine) {
            inFence = true;
            fenceMarker = trimmed.left(3);
            out += line;
        } else {
            out += linkifyTranscriptLine(line);
        }
    }
    return out;
}

bool ClaudeTranscriptView::parseInlineChoices(const QString &markdown, QStringList &options)
{
    static const QRegularExpression item(
        QStringLiteral("(?m)^[ \\t]{0,3}(\\d{1,2})[.)][ \\t]+(.+)$"));
    static constexpr int kMaxOptions = 6;      // more than this is a list, not a choice
    static constexpr int kMaxOptionChars = 160; // an answer is a phrase, not a paragraph
    static constexpr int kMaxTrailerChars = 200; // room for a closing "which one?"
    QStringList found;
    QList<int> numbers;
    int prevEnd = -1;
    int firstStart = -1;
    int lastEnd = -1;
    bool contiguous = true;
    auto it = item.globalMatch(markdown);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (prevEnd >= 0
            && !QStringView(markdown).mid(prevEnd, m.capturedStart(0) - prevEnd)
                    .trimmed().isEmpty())
            contiguous = false;
        if (firstStart < 0)
            firstStart = m.capturedStart(0);
        numbers << m.captured(1).toInt();
        found << m.captured(2).trimmed();
        prevEnd = lastEnd = m.capturedEnd(0);
    }
    if (found.size() < 2 || found.size() > kMaxOptions || !contiguous)
        return false;
    for (int i = 0; i < numbers.size(); ++i) {
        if (numbers.at(i) != i + 1)
            return false;
    }
    for (const QString &opt : std::as_const(found)) {
        if (opt.size() > kMaxOptionChars)
            return false;
    }
    const QString trailer = QStringView(markdown).mid(lastEnd).toString().trimmed();
    if (trailer.size() > kMaxTrailerChars)
        return false;
    QString prompt;
    const QStringList leadInLines =
        QStringView(markdown).left(qMax(firstStart, 0)).toString().split(QLatin1Char('\n'));
    for (int i = leadInLines.size() - 1; i >= 0; --i) {
        if (!leadInLines.at(i).trimmed().isEmpty()) {
            prompt = leadInLines.at(i).trimmed();
            break;
        }
    }
    const QString ask = prompt + QLatin1Char('\n') + trailer;
    if (!ask.contains(QLatin1Char('?'))) {
        static const QRegularExpression cue(QStringLiteral(
            "(?i)\\b(which|choose|choice|choices|select|pick|prefer|"
            "do you want|would you like|want me to|should i|shall i|should we|"
            "let me know|your call|tell me|option|options)\\b"));
        if (!cue.match(ask).hasMatch())
            return false;
    }

    options = found;
    return true;
}

QDateTime ClaudeTranscriptView::eventTime(const QJsonObject &ev)
{
    const QJsonValue stamp = ev.value(QStringLiteral("_ts"));
    if (stamp.isDouble() && stamp.toDouble() > 0)
        return QDateTime::fromMSecsSinceEpoch(qint64(stamp.toDouble()));
    const QDateTime at = QDateTime::fromString(
        ev.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
    return at.isValid() ? at.toLocalTime() : QDateTime();
}

QWidget *ClaudeTranscriptView::speakerCaption(bool user, const QDateTime &at)
{
    const QString name = user && m_userName.isEmpty() ? QStringLiteral("You")
                         : user                       ? m_userName
                                                      : m_agentName;
    auto *caption = new QWidget;
    caption->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
    auto *l = new QLabel(esc(name), caption);
    l->setFont(monoFont());
    l->setStyleSheet(Theme::remapSheet(QStringLiteral("color:%1;background:transparent;border:none;"
                                    "font-size:11px;font-weight:700;")
                         .arg(user ? m_p.muted : m_p.accent)));
    l->setVisible(!name.isEmpty());
    auto *time = new QLabel(caption);
    time->setObjectName(QStringLiteral("speakerCaptionTime"));
    time->setFont(monoFont());
    time->setStyleSheet(Theme::remapSheet(QStringLiteral("color:%1;background:transparent;border:none;"
                                       "font-size:10px;font-weight:400;")
                            .arg(m_p.muted)));
    auto *h = new QHBoxLayout(caption);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    h->addWidget(new SpeakerPortrait(speakerStyle(user)));
    h->addWidget(l);
    h->addWidget(time);
    h->addStretch(1);
    stampSpeakerCaption(caption, at);
    return caption;
}

void ClaudeTranscriptView::stampSpeakerCaption(QWidget *row, const QDateTime &at)
{
    QLabel *time = row ? row->findChild<QLabel *>(QStringLiteral("speakerCaptionTime"))
                       : nullptr;
    if (!time || !time->text().isEmpty())
        return;
    time->setVisible(at.isValid());
    if (at.isValid())
        time->setText(at.toString(QStringLiteral("dddd, MMMM d, yyyy h:mm:ss AP")));
}

bool ClaudeTranscriptView::addAssistantText(const QString &markdown)
{
    auto *l = new CacheLabel;
    if (Theme::isNext()) {
        l->setTextFormat(Qt::RichText);
        l->setText(nextProseHtml(linkifyReferences(markdown), m_p.chipBg));
    } else {
        l->setTextFormat(Qt::MarkdownText);
        l->setText(linkifyReferences(markdown));
    }
    l->setProperty(kProseMarkdown, markdown);
    l->setWordWrap(true);
    enableReferenceLinks(l);
    l->setFont(proseFont());
    l->setStyleSheet(Theme::remapSheet(
        QStringLiteral("color:%1;background:transparent;border:none;").arg(m_p.text)));

    QWidget *content = l;
    QStringList options;
    const bool asked = parseInlineChoices(markdown, options);
    if (asked) {
        if (m_openInlineChoices)
            lockInlineChoices(m_openInlineChoices); // the conversation moved on
        auto *wrap = new QWidget;
        wrap->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
        auto *v = new QVBoxLayout(wrap);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(8);
        v->addWidget(l);
        v->addWidget(addInlineChoices(options));
        content = wrap;
    }
    QWidget *caption = speakerCaption(false, m_eventTime);
    addProseCopyButton(caption, l, m_p);
    addAgentRow(speakerBody(caption, content));
    return asked;
}

void ClaudeTranscriptView::addUserTurn(const QString &text, const QDateTime &at)
{
    if (m_openInlineChoices)
        lockInlineChoices(m_openInlineChoices); // a reply arrived; stop offering it

    const bool next = Theme::isNext();
    auto *frame = new QWidget;
    frame->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
    auto *v = new QVBoxLayout(frame);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);
    if (!next)
        v->addWidget(speakerCaption(true, at));
    if (m_prependAt < 0 && m_promptDeck)
        m_promptDeck->addPrompt(text);

    static const QRegularExpression imgLine(
        QStringLiteral("^Attached image:\\s*(.+?)\\s*$"));
    QStringList prose;
    QStringList images;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch m = imgLine.match(line);
        const QString path =
            m.hasMatch() ? AgentPromptImages::resolve(m.captured(1)) : QString();
        if (!path.isEmpty() && QImageReader(path).canRead())
            images << path;
        else
            prose << line;
    }

    const QString bodyText = prose.join(QLatin1Char('\n')).trimmed();
    if (next) {
        auto *bubble = new TranscriptUserBubble(Theme::color(Theme::Role::SurfaceActive));
        auto *bv = new QVBoxLayout(bubble);
        bv->setContentsMargins(16, 10, 16, 10);
        bv->setSpacing(6);
        int textWidth = 0;
        if (!bodyText.isEmpty()) {
            auto *body = new CacheLabel(
                QStringLiteral("<p style=\"margin:0;line-height:155%;\">%1</p>")
                    .arg(referenceHtml(bodyText)));
            body->setTextFormat(Qt::RichText);
            body->setWordWrap(true);
            enableReferenceLinks(body);
            body->setFont(proseFont());
            body->setStyleSheet(Theme::remapSheet(
                QStringLiteral("color:%1;background:transparent;border:none;").arg(m_p.text)));
            const QFontMetrics metrics(body->font());
            for (const QString &line : bodyText.split(QLatin1Char('\n')))
                textWidth = qMax(textWidth, metrics.horizontalAdvance(line));
            textWidth += 4;
            bv->addWidget(body);
        }
        QList<QPointer<QWidget>> thumbs;
        for (const QString &path : images) {
            auto *thumb = new ThumbImage(path, m_p.border);
            thumbs.append(thumb);
            bv->addWidget(thumb, 0, Qt::AlignLeft);
        }
        auto *time = new QLabel;
        time->setObjectName(QStringLiteral("speakerCaptionTime"));
        QFont timeFont = proseFont();
        timeFont.setPixelSize(11);
        time->setFont(timeFont);
        time->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;")
                                .arg(Theme::hex(Theme::Role::FgSubtle)));
        time->setVisible(at.isValid());
        if (at.isValid()) {
            const QDateTime local = at.toLocalTime();
            time->setText(local.date() == QDate::currentDate()
                              ? local.toString(QStringLiteral("h:mm AP"))
                              : local.toString(QStringLiteral("MMM d, h:mm AP")));
            time->setToolTip(local.toString(QStringLiteral("dddd, MMMM d, yyyy h:mm:ss AP")));
        }
        v->addWidget(new TranscriptUserBubbleRow(bubble, time, textWidth, thumbs));
        addUserRow(frame);
        return;
    }
    if (!bodyText.isEmpty()) {
        auto *body = new CacheLabel(referenceHtml(bodyText));
        body->setTextFormat(Qt::RichText);
        body->setWordWrap(true);
        enableReferenceLinks(body);
        body->setFont(proseFont());
        body->setStyleSheet(Theme::remapSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(m_p.text)));
        v->addWidget(body);
    }
    for (const QString &path : images)
        v->addWidget(new ThumbImage(path, m_p.border), 0, Qt::AlignLeft);
    addUserRow(frame);
}

void ClaudeTranscriptView::addToolUse(const QString &id, const QString &name,
                                      const QJsonObject &input)
{
    if (m_codexStyle && addCodexExplore(id, name, input))
        return;
    // Both CLIs run shell commands, and a command reads best as a terminal
    // panel whichever of them ran it.
    if (name == QLatin1String("Bash")) {
        const QString cmd = CodexTranscriptStyle::commandText(input);
        if (!cmd.isEmpty()) {
            addCommand(id, cmd, toolSubtitle(name, input),
                       input.value(QStringLiteral("cwd")).toString());
            return;
        }
    }

    QString shown = forkmesh::transcripttools::displayName(name, input);
    bool bare = false;
    if (m_codexStyle) {
        const QString verb = CodexTranscriptStyle::toolVerb(name);
        if (!verb.isEmpty()) {
            shown = verb;
            bare = true;
        }
    }

    auto *card = new QFrame;
    card->setStyleSheet(Theme::remapSheet(QStringLiteral("QFrame{background:transparent;border:none;}")));
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    const QString editPath = forkmesh::transcripttools::isFileEditTool(name)
                                 ? input.value(QStringLiteral("file_path")).toString().trimmed()
                                 : QString();
    QStringList editPaths;
    if (!editPath.isEmpty())
        editPaths << editPath;
    else if (name == QLatin1String("FileChange"))
        for (const QJsonValue &value : input.value(QStringLiteral("changes")).toArray()) {
            const QString path =
                value.toObject().value(QStringLiteral("path")).toString().trimmed();
            if (!path.isEmpty())
                editPaths << path;
        }
    if (!editPaths.isEmpty()) {
        QWidget *header = dotHeader(shown,
                                    fileEditSubtitleHtml(editPaths,
                                                         forkmesh::transcripttools::editStats(
                                                             name, input)),
                                    true, true);
        if (QWidget *inBody = toolBody(name, input))
            v->addWidget(new FileEditCard(header, connectorRow(inBody), m_p));
        else
            v->addWidget(header);
    } else {
        v->addWidget(dotHeader(shown, toolSubtitle(name, input), bare));
        if (QWidget *inBody = toolBody(name, input))
            v->addWidget(connectorRow(inBody));
    }

    ToolCard tc;
    tc.io = v;
    tc.toolName = name;
    tc.windowTool = forkmesh::transcripttools::isWindowTool(name);
    tc.summarizeResult = forkmesh::transcripttools::summarizeResult(name);
    tc.suppressResult = forkmesh::transcripttools::suppressResult(name);
    tc.quietSuccess = !editPath.isEmpty();
    if (forkmesh::transcripttools::isSubagentTool(name))
        tc.agentTitle = input.value(QStringLiteral("description")).toString().simplified();
    if (m_codexStyle && name == QLatin1String("FileChange"))
        tc.suppressResult = true;
    m_toolCards.insert(id, tc);
    addRow(card, accentFor(name));
}

void ClaudeTranscriptView::addToolResult(const QString &id, const QString &text,
                                         bool isError)
{
    auto it = m_toolCards.find(id);
    if (it == m_toolCards.end())
        return;
    if (auto *header = static_cast<CommandHeader *>(it->commandHeader.data())) {
        header->setRunning(false);
        const QDateTime endedAt = m_eventTime.isValid() ? m_eventTime
                                  : m_bulkPopulate      ? QDateTime()
                                                        : QDateTime::currentDateTime();
        if (it->startedAt.isValid() && endedAt.isValid())
            header->setElapsed(it->startedAt.msecsTo(endedAt));
        header->markSettled();
    }
    if (text.trimmed().isEmpty())
        return;
    m_dirtyToolOutput.remove(id); // the final text lands below, in full
    ToolCard &tc = it.value();
    if (tc.exploreLine) {
        tc.hasResult = true;
        if (isError)
            tc.exploreLine->setText(
                tc.exploreLine->text()
                + QStringLiteral("<span style='color:%1'> (failed)</span>")
                      .arg(m_p.del));
        return;
    }
    if (!tc.io || tc.hasResult)
        return; // a result already attached
    if (tc.isCommand) {
        settleCommandApproval(id, text, isError);
        addCommandOutput(tc, text, isError);
        return;
    }
    if (tc.suppressResult || (tc.quietSuccess && !isError)) {
        tc.hasResult = true;
        return;
    }
    if (tc.windowTool && !isError && addWindowResult(tc, text))
        return;
    if (forkmesh::transcripttools::isSubagentTool(tc.toolName) && !isError) {
        if (forkmesh::transcripttools::isAgentLaunchResult(text)) {
            auto *state = new QLabel(QStringLiteral(
                "Working in the background. Its summary shows here when it finishes."));
            state->setWordWrap(true);
            state->setFont(monoFont());
            state->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;")
                                     .arg(m_p.muted));
            tc.agentState = connectorRow(state);
            tc.io->addWidget(tc.agentState);
            tc.hasResult = true;
            return;
        }
        const QString report = forkmesh::transcripttools::agentReport(text);
        if (!report.isEmpty()) {
            tc.hasResult = true;
            showAgentReport(tc, report, QString());
            return;
        }
    }
    if (tc.liveOutput) {
        tc.liveOutput->setText(capLabelText(text, true));
        tc.liveOutputText = text;
        tc.hasResult = true;
        return;
    }
    tc.hasResult = true;
    if (tc.summarizeResult && !isError) {
        const QStringList lines = text.trimmed().split(QLatin1Char('\n'));
        QString summary;
        if (lines.size() > 1)
            summary = QStringLiteral("%1 lines")
                          .arg(QLocale().toString(qint64(lines.size())));
        else {
            summary = lines.value(0).trimmed();
            if (summary.size() > 160)
                summary = summary.left(160) + QStringLiteral("…");
        }
        auto *l = new QLabel(summary);
        l->setFont(monoFont());
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setStyleSheet(Theme::remapSheet(QStringLiteral(
            "color:%1;background:transparent;border:none;").arg(m_p.muted)));
        tc.io->addWidget(connectorRow(l));
        return;
    }
    QWidget *out =
        new OutputPeek(text, isError ? m_p.del : m_p.muted, m_p, m_codexStyle);
    for (QLabel *label : out->findChildren<QLabel *>())
        enableReferenceLinks(label);
    tc.io->addWidget(connectorRow(out));
}

bool ClaudeTranscriptView::addWindowResult(ToolCard &tc, const QString &text)
{
    const auto result = forkmesh::transcripttools::parseWindowResult(text);
    if (!result.valid)
        return false;
    tc.hasResult = true;
    QString html = esc(forkmesh::transcripttools::windowResultSummary(result));
    if (result.open && !result.windowId.isEmpty())
        html += QStringLiteral(" · <a href=\"forkmesh-window:%1\">Show window</a>")
                    .arg(esc(result.windowId));
    auto *l = new QLabel(html);
    l->setTextFormat(Qt::RichText);
    l->setWordWrap(true);
    l->setFont(monoFont());
    enableReferenceLinks(l);
    l->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;")
                         .arg(result.answered ? m_p.text : m_p.muted));
    tc.io->addWidget(connectorRow(l));
    return true;
}

void ClaudeTranscriptView::appendToolOutput(const QString &id,
                                            const QString &text)
{
    auto it = m_toolCards.find(id);
    if (it == m_toolCards.end() || text.isEmpty())
        return;
    ToolCard &tc = it.value();
    if (!tc.io || tc.hasResult)
        return;
    if (tc.isCommand && m_terminalOutputMode) {
        if (!tc.terminalOutput) {
            QVBoxLayout *column = commandOutputColumn(tc);
            if (!column)
                return;
            auto *terminal = new TerminalOutputView(
                QString(), m_p.muted, m_p.shellPath, tc.panelBox);
            enableReferenceLinks(terminal->outputLabel());
            tc.terminalOutput = terminal;
            column->addWidget(tc.terminalOutput);
        }
        tc.liveOutputText += text;
        m_dirtyToolOutput.insert(id);
        markTextDirty();
        return;
    }
    if (!tc.liveOutput) {
        auto *label = new CacheLabel;
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setFont(monoFont());
        label->setStyleSheet(Theme::remapSheet(
            QStringLiteral("color:%1;background:transparent;border:none;")
                .arg(m_p.muted)));
        QVBoxLayout *column = tc.isCommand ? commandOutputColumn(tc) : nullptr;
        if (column)
            column->addWidget(label);
        else
            tc.io->addWidget(connectorRow(label));
        tc.liveOutput = label;
    }
    tc.liveOutputText += text;
    m_dirtyToolOutput.insert(id);
    markTextDirty();
}

// Claude Code's AskUserQuestion tool: render an interactive multiple-choice card
// (Anthropic Agent SDK "Handle approvals and user input" shape — a `questions`
// array, each with a question/header/options[label,description]/multiSelect) so
// the user can answer the clarifying question right in the transcript. Each
// option is a checkable button; a free-text "Other" field covers answers the
// options don't. On submit it emits questionAnswered(id, answer, sensitive); the
// host responds through the active CLI protocol and records a redacted
// "_local_ask_answer" event so the answered state replays on rebuild.
void ClaudeTranscriptView::addAskUserQuestion(const QString &id,
                                              const QJsonObject &input)
{
    clearActivity(); // the agent is now waiting on the user, not working

    auto *card = new QFrame;
    card->setStyleSheet(Theme::remapSheet(
        (Theme::edgeIndicators()
             ? QStringLiteral("QFrame{background:%1;border:1px solid %2;"
                              "border-left:3px solid %3;border-radius:8px;}")
                   .arg(m_p.userBg, m_p.border, m_p.accent)
             : QStringLiteral("QFrame{background:%1;border:1px solid %2;"
                              "border-radius:8px;}")
                   .arg(m_p.userBg, m_p.border))));
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(14, 12, 14, 12);
    v->setSpacing(10);

    const QString headingText =
        input.value(QStringLiteral("title")).toString().simplified().left(160);
    auto *heading = new QLabel(headingText.isEmpty()
                                   ? QStringLiteral("Agent needs your input")
                                   : headingText);
    heading->setTextFormat(Qt::PlainText);
    heading->setWordWrap(true);
    heading->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "color:%1;font-weight:700;background:transparent;border:none;").arg(m_p.accent)));
    v->addWidget(heading);

    auto *controls = new QWidget;
    controls->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
    auto *cv = new QVBoxLayout(controls);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(16);
    v->addWidget(controls);

    struct QState {
        QString question;
        bool multi = false;
        bool secret = false;
        QList<QPushButton *> optionButtons;
        QStringList optionLabels;
        QLineEdit *other = nullptr;
    };
    QVector<QState> states;
    bool cardSensitive = false;

    const QString optCss = QStringLiteral(
        "QPushButton{background:%1;color:%2;border:1px solid %3;border-radius:6px;"
        "padding:7px 12px;text-align:left;font-weight:600;}"
        "QPushButton:hover{border-color:%4;}"
        "QPushButton:checked{background:%4;color:%1;border-color:%4;}")
        .arg(m_p.canvas, m_p.text, m_p.border, m_p.accent);
    const QString editCss = QStringLiteral(
        "QLineEdit{background:%1;color:%2;border:1px solid %3;border-radius:6px;"
        "padding:6px 10px;}QLineEdit:focus{border-color:%4;}")
        .arg(m_p.canvas, m_p.text, m_p.border, m_p.accent);

    // code-health: allow(stylesheet-in-loop) built once per question card; the AskUserQuestion schema caps it at 4 questions of 2-4 options
    for (const QJsonValue &qv : input.value(QStringLiteral("questions")).toArray()) {
        const QJsonObject q = qv.toObject();
        QState st;
        st.question = q.value(QStringLiteral("question")).toString();
        st.multi = q.value(QStringLiteral("multiSelect")).toBool();
        st.secret = q.value(QStringLiteral("isSecret")).toBool();
        cardSensitive = cardSensitive || st.secret;

        auto *qbox = new QWidget;
        qbox->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
        auto *qv2 = new QVBoxLayout(qbox);
        qv2->setContentsMargins(0, 0, 0, 0);
        qv2->setSpacing(6);

        auto *qlabel = new QLabel(st.question);
        qlabel->setTextFormat(Qt::PlainText);
        qlabel->setWordWrap(true);
        qlabel->setStyleSheet(Theme::remapSheet(QStringLiteral(
            "color:%1;font-weight:600;background:transparent;border:none;").arg(m_p.text)));
        qv2->addWidget(qlabel);

        auto *group = new QButtonGroup(controls);
        group->setExclusive(!st.multi);

        for (const QJsonValue &ov : q.value(QStringLiteral("options")).toArray()) {
            const QJsonObject o = ov.toObject();
            const QString label = o.value(QStringLiteral("label")).toString();
            const QString desc = o.value(QStringLiteral("description")).toString();
            auto *btn = new QPushButton(label);
            btn->setCheckable(true);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(Theme::remapSheet(optCss));
            group->addButton(btn);
            qv2->addWidget(btn);
            st.optionButtons.append(btn);
            st.optionLabels.append(label);
            if (!desc.trimmed().isEmpty()) {
                auto *d = new QLabel(desc);
                d->setTextFormat(Qt::PlainText);
                d->setWordWrap(true);
                d->setStyleSheet(Theme::remapSheet(QStringLiteral(
                    "color:%1;background:transparent;border:none;"
                    "margin:0 0 2px 4px;font-size:12px;").arg(m_p.muted)));
                qv2->addWidget(d);
            }
        }

        auto *other = new QLineEdit;
        if (st.secret)
            other->setEchoMode(QLineEdit::Password);
        other->setPlaceholderText(
            st.multi ? QStringLiteral("Other… (adds your own answer)")
                     : QStringLiteral("Other… (type your own answer)"));
        other->setStyleSheet(Theme::remapSheet(editCss));
        qv2->addWidget(other);
        st.other = other;

        if (!st.multi) {
            connect(other, &QLineEdit::textEdited, group, [group](const QString &t) {
                if (t.isEmpty())
                    return;
                if (QAbstractButton *b = group->checkedButton()) {
                    group->setExclusive(false);
                    b->setChecked(false);
                    group->setExclusive(true);
                }
            });
            connect(group, &QButtonGroup::buttonClicked, other,
                    [other](QAbstractButton *) { other->clear(); });
        }

        states.append(st);
        cv->addWidget(qbox);
    }

    auto *submit = new QPushButton(QStringLiteral("Send answer"));
    submit->setCursor(Qt::PointingHandCursor);
    submit->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "QPushButton{background:%1;color:%2;border:none;border-radius:6px;"
        "padding:8px 16px;font-weight:600;}"
        "QPushButton:hover{background:%3;}")
        .arg(m_p.accent, m_p.canvas, m_p.add)));
    cv->addWidget(submit, 0, Qt::AlignLeft);

    auto *status = new QLabel;
    status->setWordWrap(true);
    status->setVisible(false);
    status->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "color:%1;font-weight:600;background:transparent;border:none;").arg(m_p.add)));
    v->addWidget(status);

    m_askCards.insert(id, AskCard{controls, status, cardSensitive});

    connect(submit, &QPushButton::clicked, this,
            [this, id, states, cardSensitive] {
        QStringList lines;
        for (const QState &st : states) {
            QStringList picks;
            for (int i = 0; i < st.optionButtons.size(); ++i)
                if (st.optionButtons[i]->isChecked())
                    picks << st.optionLabels[i];
            const QString otherText = st.other ? st.other->text().trimmed() : QString();
            QString value;
            if (st.multi) {
                if (!otherText.isEmpty())
                    picks << otherText;
                value = picks.join(QStringLiteral(", "));
            } else {
                value = !otherText.isEmpty()
                            ? otherText
                            : (picks.isEmpty() ? QString() : picks.first());
            }
            if (value.isEmpty())
                return;
            lines << (states.size() > 1
                          ? QStringLiteral("%1 → %2").arg(st.question, value)
                          : value);
        }
        const QString answer = lines.join(QStringLiteral("\n"));
        if (answer.isEmpty())
            return; // nothing chosen yet — keep the card open
        markAskAnswered(id, answer); // instant local feedback
        emit questionAnswered(id, answer, cardSensitive);
    });

    addRow(card, m_p.accent);
}

void ClaudeTranscriptView::markAskAnswered(const QString &id, const QString &answer)
{
    auto it = m_askCards.find(id);
    if (it == m_askCards.end())
        return;
    AskCard &c = it.value();
    if (c.buttons) {
        c.buttons->setEnabled(false);
        auto *fx = new QGraphicsOpacityEffect(c.buttons);
        fx->setOpacity(0.5);
        c.buttons->setGraphicsEffect(fx);
    }
    if (c.status) {
        QString shown = c.sensitive ? QStringLiteral("Secret answer submitted")
                                    : answer;
        if (!c.sensitive)
            shown.replace(QLatin1Char('\n'), QStringLiteral(" · "));
        c.status->setText(QStringLiteral("✓ You answered: %1").arg(shown));
        c.status->setVisible(true);
    }
}

QWidget *ClaudeTranscriptView::addInlineChoices(const QStringList &options)
{
    auto *box = new QWidget;
    box->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);

    const QString css = QStringLiteral(
        "color:%1;background:%2;border:1px solid %3;border-radius:6px;")
        .arg(m_p.text, m_p.canvas, m_p.border);

    for (const QString &opt : options) {
        auto *row = new ChoiceOption(opt);
        row->setStyleSheet(Theme::remapSheet(css));
        v->addWidget(row);
        row->onClick = [this, box, opt] {
            lockInlineChoices(box);
            emit inlineChoiceAnswered(opt);
        };
    }

    m_openInlineChoices = box;
    return box;
}

void ClaudeTranscriptView::lockInlineChoices(QWidget *box)
{
    if (!box || !box->isEnabled())
        return; // already locked
    box->setEnabled(false);
    auto *fx = new QGraphicsOpacityEffect(box);
    fx->setOpacity(0.5);
    box->setGraphicsEffect(fx);
    if (m_openInlineChoices == box)
        m_openInlineChoices = nullptr;
}

bool ClaudeTranscriptView::resultIsError(const QJsonObject &ev)
{
    return ev.value(QStringLiteral("is_error")).toBool()
           || ev.value(QStringLiteral("subtype")).toString()
                  .startsWith(QLatin1String("error"));
}

QString ClaudeTranscriptView::failureReason(const QJsonObject &ev)
{
    QString detail = ev.value(QStringLiteral("result")).toString().trimmed();
    if (detail.isEmpty()) {
        const QJsonValue e = ev.value(QStringLiteral("error"));
        detail = e.isObject()
                     ? e.toObject().value(QStringLiteral("message")).toString().trimmed()
                     : e.toString().trimmed();
    }
    if (!detail.isEmpty())
        return detail;

    QString sub = ev.value(QStringLiteral("subtype")).toString().trimmed();
    if (sub == QLatin1String("error_max_turns"))
        return QStringLiteral("the run hit its maximum number of turns");
    if (sub == QLatin1String("error_during_execution"))
        return QStringLiteral("the CLI hit an error while running the turn");
    if (sub.isEmpty() || sub == QLatin1String("error"))
        return QStringLiteral("the CLI reported an error but sent no detail — "
                              "check the raw log for this session");
    if (sub.startsWith(QLatin1String("error_")))
        sub = sub.mid(6);
    return sub.replace(QLatin1Char('_'), QLatin1Char(' '));
}

void ClaudeTranscriptView::addResult(const QJsonObject &ev)
{
    for (const ToolCard &card : std::as_const(m_toolCards))
        if (auto *header = static_cast<CommandHeader *>(card.commandHeader.data()))
            header->setRunning(false);
    const bool err = resultIsError(ev);
    const double cost = ev.value(QStringLiteral("total_cost_usd")).toDouble();
    const double ms = ev.value(QStringLiteral("duration_ms")).toDouble();
    const int turns = ev.value(QStringLiteral("num_turns")).toInt();
    QStringList details;
    if (turns > 0)
        details << QStringLiteral("%1 turn%2")
                       .arg(turns)
                       .arg(turns == 1 ? QString() : QStringLiteral("s"));
    if (ms > 0)
        details << QStringLiteral("%1s").arg(ms / 1000.0, 0, 'f', 1);
    const QString tokens = forkmesh::transcripttools::resultTokenSummary(ev);
    if (!tokens.isEmpty())
        details << tokens;
    if (cost > 0)
        details << QStringLiteral("$%1").arg(cost, 0, 'f', 4);
    QString label = QStringLiteral("%1 %2")
                        .arg(err ? QStringLiteral("✗") : QStringLiteral("✓"),
                             err ? QStringLiteral("Failed") : QStringLiteral("Done"));
    if (!details.isEmpty())
        label += QStringLiteral(" · ") + details.join(QStringLiteral(" · "));
    auto *l = new QLabel(label);
    l->setFont(monoFont());
    l->setStyleSheet(Theme::remapSheet(QStringLiteral("color:%1;font-weight:600;background:transparent;")
                         .arg(err ? m_p.del : m_p.add)));
    if (!err) {
        // Keep hold of the row: whether this really was the end of the work is
        // decided by the process tree, which the host reports separately.
        m_resultLabel = l;
        m_resultLabelText = label;
        addRow(l, m_p.add);
        applyBackgroundWorkNote();
        return;
    }

    auto *card = new QWidget;
    card->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    v->addWidget(l);
    v->addWidget(makeMono(failureReason(ev), true));
    addRow(card, m_p.del);
}

void ClaudeTranscriptView::setBackgroundWorkNote(const QString &note)
{
    if (m_backgroundWorkNote == note)
        return;
    m_backgroundWorkNote = note;
    applyBackgroundWorkNote();
}

void ClaudeTranscriptView::applyBackgroundWorkNote()
{
    if (!m_resultLabel || m_resultLabelText.isEmpty())
        return;
    if (m_backgroundWorkNote.isEmpty()) {
        m_resultLabel->setText(m_resultLabelText);
        m_resultLabel->setStyleSheet(Theme::remapSheet(
            QStringLiteral("color:%1;font-weight:600;background:transparent;")
                .arg(m_p.add)));
        return;
    }
    QString text = m_resultLabelText;
    const QString done = QStringLiteral("✓ Done");
    if (text.startsWith(done))
        text.replace(0, done.size(), QStringLiteral("⟳ Finishing"));
    text += QStringLiteral(" · ") + m_backgroundWorkNote;
    m_resultLabel->setText(text);
    m_resultLabel->setStyleSheet(Theme::remapSheet(
        QStringLiteral("color:%1;font-weight:600;background:transparent;")
            .arg(m_p.accent)));
}

bool ClaudeTranscriptView::addCodexExplore(const QString &id, const QString &name,
                                           const QJsonObject &input)
{
    QString verb, target;
    if (name == QLatin1String("Bash")) {
        if (!CodexTranscriptStyle::exploreLine(
                CodexTranscriptStyle::commandText(input), verb, target))
            return false;
    } else if (name == QLatin1String("Read")) {
        verb = QStringLiteral("Read");
        target = input.value(QStringLiteral("file_path")).toString();
    } else if (name == QLatin1String("Grep")) {
        verb = QStringLiteral("Search");
        target = input.value(QStringLiteral("pattern")).toString();
        const QString path = input.value(QStringLiteral("path")).toString();
        if (!path.isEmpty())
            target += QStringLiteral(" in ") + path;
    } else if (name == QLatin1String("Glob")) {
        verb = QStringLiteral("List");
        target = input.value(QStringLiteral("pattern")).toString();
    } else {
        return false;
    }
    if (target.trimmed().isEmpty())
        return false;

    if (!m_exploreBlock)
        m_exploreLines = nullptr; // the row itself went away; start a new block
    if (!m_exploreLines) {
        auto *card = new QFrame;
        card->setStyleSheet(Theme::remapSheet(
            QStringLiteral("QFrame{background:transparent;border:none;}")));
        auto *v = new QVBoxLayout(card);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);
        v->addWidget(dotHeader(QStringLiteral("Explored"), QString()));
        auto *lines = new QWidget;
        lines->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
        m_exploreLines = new QVBoxLayout(lines);
        m_exploreLines->setContentsMargins(0, 0, 0, 0);
        m_exploreLines->setSpacing(2);
        v->addWidget(connectorRow(lines));
        m_keepExplore = true;
        m_exploreBlock = addRow(card, m_p.muted);
        m_keepExplore = false;
    }

    auto *l = new CacheLabel;
    l->setTextFormat(Qt::RichText);
    l->setWordWrap(true);
    enableReferenceLinks(l);
    l->setFont(monoFont());
    l->setText(QStringLiteral("<span style='color:%1'>%2</span> "
                              "<span style='color:%3'>%4</span>")
                   .arg(m_p.accent, esc(verb), m_p.text,
                        linkifyTranscriptText(target.trimmed(), true)));
    l->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;border:none;")));
    m_exploreLines->addWidget(l);

    ToolCard tc;
    tc.exploreLine = l;
    m_toolCards.insert(id, tc);
    return true;
}

QString ClaudeTranscriptView::commandHtml(const QString &line) const
{
    QString out;
    bool commandWord = true; // the next shell word starts a command
    int i = 0;
    const int n = line.size();

    const auto styled = [](const QString &text, const QString &colour,
                           bool bold = false) {
        return QStringLiteral("<span style='color:%1;%2'>%3</span>")
            .arg(colour, bold ? QStringLiteral("font-weight:600;") : QString(),
                 esc(text));
    };
    const auto operatorAt = [&line, n](int at) {
        static const QStringList operators{
            QStringLiteral("<<<"), QStringLiteral("<<-"),
            QStringLiteral("&&"),  QStringLiteral("||"),
            QStringLiteral(">>"),  QStringLiteral("<<"),
            QStringLiteral("&>"),  QStringLiteral("|&"),
            QStringLiteral(">|"),  QStringLiteral(";;&"),
            QStringLiteral(";;"),  QStringLiteral(";&"),
            QStringLiteral("|"),   QStringLiteral(";"),
            QStringLiteral("&"),   QStringLiteral(">"),
            QStringLiteral("<"),   QStringLiteral("("),
            QStringLiteral(")"),   QStringLiteral("{"),
            QStringLiteral("}")};
        for (const QString &op : operators)
            if (at + op.size() <= n && line.mid(at, op.size()) == op)
                return op;
        return QString();
    };
    const auto plainWord = [](const QString &word) {
        QString plain;
        plain.reserve(word.size());
        QChar quote;
        bool escaped = false;
        for (const QChar c : word) {
            if (escaped) {
                plain += c;
                escaped = false;
            } else if (c == QLatin1Char('\\') && quote != QLatin1Char('\'')) {
                escaped = true;
            } else if (quote.isNull()
                       && (c == QLatin1Char('\'') || c == QLatin1Char('"'))) {
                quote = c;
            } else if (c == quote) {
                quote = QChar();
            } else {
                plain += c;
            }
        }
        if (escaped)
            plain += QLatin1Char('\\');
        return plain;
    };
    const auto fileHref = [](const QString &word) {
        QString path = word.trimmed();
        if (path.isEmpty() || path.startsWith(QLatin1Char('-'))
            || path.contains(QLatin1Char('$'))
            || path.contains(QLatin1String("://"))
            || path.contains(QRegularExpression(QStringLiteral("[*?\\[\\]{}]"))))
            return QString();
        if (path.startsWith(QLatin1String("/dev/")))
            return QString(); // devices are not files the repository can open

        QString pathOnly = path;
        QString lineNumber;
        static const QRegularExpression lineTail(
            QStringLiteral("(?::(\\d+)|#L(\\d+))$"));
        const QRegularExpressionMatch lineMatch = lineTail.match(pathOnly);
        if (lineMatch.hasMatch()) {
            lineNumber = !lineMatch.captured(1).isEmpty()
                             ? lineMatch.captured(1)
                             : lineMatch.captured(2);
            pathOnly.truncate(lineMatch.capturedStart());
        }

        const QString base = pathOnly.section(QLatin1Char('/'), -1);
        static const QRegularExpression extension(
            QStringLiteral("^[A-Za-z0-9_+@-]+\\.[A-Za-z][A-Za-z0-9_-]{0,15}$"));
        static const QSet<QString> specialNames{
            QStringLiteral("CMakeLists.txt"), QStringLiteral("Dockerfile"),
            QStringLiteral("Makefile"),       QStringLiteral("README"),
            QStringLiteral("LICENSE")};
        const bool dotFile = base.size() > 1 && base.startsWith(QLatin1Char('.'));
        const bool absoluteFile = QDir::isAbsolutePath(pathOnly)
                                  && base != QLatin1String(".")
                                  && base != QLatin1String("..");
        if (!dotFile && !absoluteFile && !extension.match(base).hasMatch()
            && !specialNames.contains(base))
            return QString();

        QString href = QStringLiteral("forkmesh-file:%1")
                           .arg(QString::fromLatin1(QUrl::toPercentEncoding(pathOnly)));
        if (!lineNumber.isEmpty())
            href += QStringLiteral("?line=%1").arg(lineNumber);
        return href;
    };

    static const QSet<QString> keywords{
        QStringLiteral("if"),       QStringLiteral("then"),
        QStringLiteral("else"),     QStringLiteral("elif"),
        QStringLiteral("fi"),       QStringLiteral("for"),
        QStringLiteral("while"),    QStringLiteral("until"),
        QStringLiteral("do"),       QStringLiteral("done"),
        QStringLiteral("case"),     QStringLiteral("esac"),
        QStringLiteral("in"),       QStringLiteral("function"),
        QStringLiteral("select"),   QStringLiteral("time"),
        QStringLiteral("coproc")};
    static const QSet<QString> commandPrefixes{
        QStringLiteral("command"), QStringLiteral("builtin"),
        QStringLiteral("exec"),    QStringLiteral("env"),
        QStringLiteral("nohup"),   QStringLiteral("sudo"),
        QStringLiteral("time")};
    static const QRegularExpression assignment(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(?:\\+)?="));

    while (i < n) {
        if (line.at(i).isSpace()) {
            const int start = i;
            while (i < n && line.at(i).isSpace())
                ++i;
            out += esc(line.mid(start, i - start));
            continue;
        }
        if (line.at(i) == QLatin1Char('#')) {
            out += styled(line.mid(i), m_p.shellComment);
            break;
        }
        const QString op = operatorAt(i);
        if (!op.isEmpty()) {
            out += styled(op, m_p.shellOperator, true);
            if (op == QLatin1String("&&") || op == QLatin1String("||")
                || op == QLatin1String("|") || op == QLatin1String("|&")
                || op.startsWith(QLatin1Char(';')) || op == QLatin1String("&")
                || op == QLatin1String("("))
                commandWord = true;
            i += op.size();
            continue;
        }

        const int start = i;
        QChar quote;
        bool escaped = false;
        while (i < n) {
            const QChar c = line.at(i);
            if (escaped) {
                escaped = false;
                ++i;
                continue;
            }
            if (c == QLatin1Char('\\') && quote != QLatin1Char('\'')) {
                escaped = true;
                ++i;
                continue;
            }
            if (!quote.isNull()) {
                if (c == quote)
                    quote = QChar();
                ++i;
                continue;
            }
            if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
                quote = c;
                ++i;
                continue;
            }
            if (c.isSpace() || !operatorAt(i).isEmpty())
                break;
            ++i;
        }
        const QString token = line.mid(start, i - start);
        if (token.isEmpty()) { // defensive progress for an unknown punctuation
            out += esc(QString(line.at(i)));
            ++i;
            continue;
        }

        const QString plain = plainWord(token);
        const bool isAssignment = assignment.match(plain).hasMatch();
        const bool isKeyword = keywords.contains(plain);
        const bool isOption = plain.size() > 1 && plain.startsWith(QLatin1Char('-'));
        const QString href = commandWord ? QString() : fileHref(plain);
        QString colour = m_p.text;
        bool bold = false;
        if (!href.isEmpty()) {
            colour = m_p.shellPath;
        } else if (isAssignment || plain.startsWith(QLatin1Char('$'))) {
            colour = m_p.shellVariable;
        } else if (isKeyword) {
            colour = m_p.shellKeyword;
            bold = true;
        } else if (token.contains(QLatin1Char('\''))
                   || token.contains(QLatin1Char('"'))) {
            colour = m_p.shellString;
        } else if (commandWord) {
            colour = m_p.shellCommand;
            bold = true;
        } else if (isOption) {
            colour = m_p.shellComment;
        }

        if (!href.isEmpty()) {
            out += QStringLiteral(
                       "<a href='%1' style='color:%2;text-decoration:underline;'>"
                       "%3</a>")
                       .arg(href.toHtmlEscaped(), colour, esc(token));
        } else {
            out += styled(token, colour, bold);
        }

        if (isAssignment) {
            // Prefix assignments do not consume the command position.
        } else if (isKeyword) {
            commandWord = plain == QLatin1String("if")
                          || plain == QLatin1String("then")
                          || plain == QLatin1String("else")
                          || plain == QLatin1String("elif")
                          || plain == QLatin1String("do")
                          || plain == QLatin1String("while")
                          || plain == QLatin1String("until")
                          || plain == QLatin1String("time");
        } else if (commandWord && !isOption) {
            commandWord = commandPrefixes.contains(
                plain.section(QLatin1Char('/'), -1));
        }
    }
    return out;
}

// The command and everything it printed as a single terminal panel: an accent ❯
// prompt with the command syntax-highlighted beside it, a hairline, then the
// output. Before this, a command was loose mono text and its output a separate
// ⎿ row, so nothing said where one ended and the other began.
QWidget *ClaudeTranscriptView::commandPanel(const QString &command,
                                            const QString &cwd, ToolCard &tc)
{
    const QString displayCommand =
        CodexTranscriptStyle::singleLineCommand(command);
    auto *panel = new TranscriptPanel(m_p.termBg, m_p.termEdge);
    auto *v = new QVBoxLayout(panel);
    v->setContentsMargins(12, 9, 12, 9);
    v->setSpacing(0);

    auto *promptRow = new QWidget(panel);
    promptRow->setMinimumWidth(0);
    promptRow->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    promptRow->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
    auto *ph = new QHBoxLayout(promptRow);
    ph->setContentsMargins(0, 0, 0, 0);
    ph->setSpacing(8);
    auto *prompt = new QLabel(QStringLiteral("❯"));
    prompt->setFont(monoFont());
    prompt->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "color:%1;background:transparent;border:none;font-weight:700;")
        .arg(m_p.accent)));
    ph->addWidget(prompt, 0, Qt::AlignTop);
    auto *commandLine =
        new CommandLineView(commandHtml(displayCommand), m_p, promptRow);
    // Hovering shows the full command, highlighted, in a solid card rather
    // than a QToolTip, which blinked on a live transcript (CommandHoverCard).
    commandLine->setHoverCommand([this, command] {
        return commandHoverHtml(command, m_p, [this](const QString &line) {
            return commandHtml(line);
        });
    });
    enableReferenceLinks(commandLine->commandLabel());
    ph->addWidget(commandLine, 1);

    auto *run = new QToolButton(promptRow);
    run->setObjectName(QStringLiteral("agentCommandRunButton"));
    run->setText(QStringLiteral("Run"));
    run->setCursor(Qt::PointingHandCursor);
    run->setToolTip(
        QStringLiteral("Run this command in a new ForkMesh terminal window"));
    run->setAccessibleName(QStringLiteral("Run command in new terminal"));
    run->setAutoRaise(false);
    run->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "QToolButton{color:%1;background:%2;border:1px solid %3;"
        "border-radius:5px;padding:2px 8px;font-size:11px;font-weight:700;}"
        "QToolButton:hover{color:%4;border-color:%4;background:%5;}"
        "QToolButton:pressed{background:%3;}")
                           .arg(m_p.text, m_p.chipBg, m_p.termEdge, m_p.accent,
                                m_p.aiEdge)));
    connect(run, &QToolButton::clicked, this,
            [this, original = command, cwd] {
                emit commandRunRequested(original, cwd);
            });
    ph->addWidget(run, 0, Qt::AlignVCenter);
    v->addWidget(promptRow);

    // The divider and the output column exist from the start but the whole lot
    // stays hidden — and so takes no vertical space at all — until something is
    // printed, so a silent command is just its prompt line.
    auto *outBox = new QWidget(panel);
    outBox->setMinimumWidth(0);
    outBox->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    outBox->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
    auto *wrap = new QVBoxLayout(outBox);
    wrap->setContentsMargins(0, 8, 0, 0);
    wrap->setSpacing(8);
    auto *divider = new QWidget(outBox);
    divider->setFixedHeight(1);
    divider->setStyleSheet(Theme::remapSheet(QStringLiteral("background:%1;").arg(m_p.termEdge)));
    wrap->addWidget(divider);
    auto *col = new QWidget(outBox);
    col->setMinimumWidth(0);
    col->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    col->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
    auto *ov = new QVBoxLayout(col);
    ov->setContentsMargins(0, 0, 0, 0);
    ov->setSpacing(3);
    wrap->addWidget(col);
    outBox->setVisible(false);
    v->addWidget(outBox);

    tc.panelOutput = ov;
    tc.panelBox = outBox;
    tc.isCommand = true;
    return panel;
}

QVBoxLayout *ClaudeTranscriptView::commandOutputColumn(ToolCard &tc)
{
    if (!tc.panelOutput)
        return nullptr;
    if (tc.panelBox)
        tc.panelBox->setVisible(true);
    return tc.panelOutput;
}

void ClaudeTranscriptView::addCommand(const QString &id, const QString &command,
                                      const QString &description,
                                      const QString &cwd)
{
    const QString display = CodexTranscriptStyle::singleLineCommand(command);
    if (display.isEmpty())
        return;

    ToolCard tc;
    if (m_approvalResolver) {
        CommandApprovalRow row;
        row.toolUseId = id;
        row.command = command;
        row.cwd = cwd;
        row.approval = m_approvalResolver(command, cwd);
        if (m_approvalPrependAt >= 0)
            m_commandApprovals.insert(
                qMin(m_approvalPrependAt++, int(m_commandApprovals.size())), row);
        else
            m_commandApprovals.append(row);
        tc.toolUseId = id;
    }
    QWidget *panel = commandPanel(command, cwd, tc);

    const QString summary = description.trimmed();
    auto *header = new CommandHeader(
        forkmesh::transcripttools::commandIcons(command),
        summary.isEmpty() ? display : summary, command, m_p);
    auto *card = new CommandCard(header, panel);
    if (!tc.toolUseId.isEmpty())
        header->onApproval = [this, id] { emit commandApprovalActivated(id); };
    header->hoverCommand = [this, command] {
        return commandHoverHtml(command, m_p, [this](const QString &line) {
            return commandHtml(line);
        });
    };
    tc.io = card->bodyLayout();
    tc.commandHeader = header;
    tc.startedAt = m_eventTime.isValid() ? m_eventTime
                   : m_bulkPopulate      ? QDateTime()
                                         : QDateTime::currentDateTime();
    paintCommandApproval(header, commandApprovalFor(tc.toolUseId));
    header->setRunning(!m_bulkPopulate);
    m_toolCards.insert(id, tc);
    if (Theme::isNext()) {
        int pos = m_prependAt >= 0 ? m_prependAt : m_col->count() - 1;
        if (m_prependAt < 0 && m_activity) {
            const int at = m_col->indexOf(m_activity);
            if (at >= 0)
                pos = at;
        }
        if (TranscriptCommandGroup *group = commandGroupBefore(m_col, pos)) {
            if (!m_keepExplore) {
                m_exploreBlock = nullptr;
                m_exploreLines = nullptr;
            }
            group->addCard(card);
            return;
        }
        auto *group = new TranscriptCommandGroup(m_p);
        group->addCard(card);
        addRow(group, accentFor(QStringLiteral("Bash")));
        return;
    }
    addRow(card, accentFor(QStringLiteral("Bash")));
}

void ClaudeTranscriptView::setCommandApprovalResolver(
    CommandApprovalResolver resolver)
{
    m_approvalResolver = std::move(resolver);
}

void ClaudeTranscriptView::refreshCommandApprovals()
{
    if (!m_approvalResolver)
        return;
    for (CommandApprovalRow &row : m_commandApprovals) {
        // What happened to a finished call is history; only the policy that
        // would apply to it is re-read.
        if (row.finished && (row.approval.state == CommandApproval::Denied ||
                             row.approval.label == QLatin1String("Approved")))
            continue;
        row.approval = m_approvalResolver(row.command, row.cwd);
        if (QWidget *header = m_toolCards.value(row.toolUseId).commandHeader)
            paintCommandApproval(header, row.approval);
    }
}

QVector<ClaudeTranscriptView::CommandApprovalRow>
ClaudeTranscriptView::commandApprovals() const
{
    return m_commandApprovals;
}

bool ClaudeTranscriptView::isPermissionDenial(const QString &resultText)
{
    // The gate's answers (MainWindowAgentAccess.cpp) and Claude Code's own
    // refusals when a call needs a permission nobody granted.
    static const QStringList markers = {
        QStringLiteral("The user denied this call"),
        QStringLiteral("This project blocks external calls"),
        QStringLiteral("The user dismissed the request"),
        QStringLiteral("permission gate"),
        QStringLiteral("haven't granted it yet"),
        QStringLiteral("Permission to use"),
        QStringLiteral("The user doesn't want to proceed with this tool use"),
    };
    const QString head = resultText.left(600);
    for (const QString &marker : markers)
        if (head.contains(marker, Qt::CaseInsensitive))
            return true;
    return false;
}

ClaudeTranscriptView::CommandApproval
ClaudeTranscriptView::commandApprovalFor(const QString &toolUseId) const
{
    if (toolUseId.isEmpty())
        return {};
    const auto row = std::find_if(
        m_commandApprovals.cbegin(), m_commandApprovals.cend(),
        [&](const CommandApprovalRow &r) { return r.toolUseId == toolUseId; });
    return row == m_commandApprovals.cend() ? CommandApproval{} : row->approval;
}

void ClaudeTranscriptView::paintCommandApproval(
    QWidget *header, const CommandApproval &approval) const
{
    auto *summary = static_cast<CommandHeader *>(header);
    QString color = m_p.muted;
    switch (approval.state) {
    case CommandApproval::Allowed:
        color = m_p.add;
        break;
    case CommandApproval::Asks:
        color = QColor(m_p.canvas).lightness() < 128 ? QStringLiteral("#d29922")
                                                     : QStringLiteral("#9a6700");
        break;
    case CommandApproval::Blocked:
    case CommandApproval::Denied:
        color = m_p.del;
        break;
    case CommandApproval::Unknown:
        break;
    }
    const QString tip =
        approval.state == CommandApproval::Unknown
            ? QString()
            : QStringLiteral("%1: %2\nClick to see this run's commands and permissions.")
                  .arg(approval.label, approval.reason);
    summary->setApproval(approval.state, QColor(color), tip);
    if (approval.state != CommandApproval::Unknown)
        summary->setAccessibleName(QStringLiteral("Command: %1 (%2)")
                                       .arg(summary->title(), approval.label));
}

void ClaudeTranscriptView::settleCommandApproval(const QString &id,
                                                 const QString &resultText,
                                                 bool isError)
{
    auto row = std::find_if(
        m_commandApprovals.begin(), m_commandApprovals.end(),
        [&](const CommandApprovalRow &r) { return r.toolUseId == id; });
    if (row == m_commandApprovals.end() || row->finished)
        return;
    row->finished = true;
    row->failed = isError;
    if (isError && isPermissionDenial(resultText)) {
        row->approval.state = CommandApproval::Denied;
        row->approval.label = QStringLiteral("Denied");
        row->approval.reason =
            QStringLiteral("The permission layer refused this call, so it never ran.");
    } else if (row->approval.state == CommandApproval::Asks) {
        // It asked, and it ran: somebody said yes.
        row->approval.state = CommandApproval::Allowed;
        row->approval.label = QStringLiteral("Approved");
        row->approval.reason = QStringLiteral(
            "This call needed approval and was allowed to run.");
    } else {
        return;
    }
    if (QWidget *header = m_toolCards.value(id).commandHeader)
        paintCommandApproval(header, row->approval);
}

// Whatever the command printed, dropped into its own panel. The trailing
// "Exit code: N" the Codex CLI appends becomes a chip rather than a stray
// output line; Claude Code sends no such tail and simply has no chip.
void ClaudeTranscriptView::addCommandOutput(ToolCard &tc, const QString &text,
                                            bool isError)
{
    tc.hasResult = true;
    QString body = text;
    QLabel *exitLabel = nullptr;
    static const QRegularExpression exitTail(
        QStringLiteral("\\n?Exit code: (-?\\d+)\\s*$"));
    const QRegularExpressionMatch m = exitTail.match(body);
    if (auto *header = static_cast<CommandHeader *>(tc.commandHeader.data())) {
        header->setRunning(false);
        const int code = m.hasMatch() ? m.captured(1).toInt() : 0;
        if (code != 0)
            header->setOutcome(QStringLiteral("exit %1").arg(code));
        else if (isError && !isPermissionDenial(text))
            header->setOutcome(QStringLiteral("failed"));
        const QString printed = m.hasMatch() ? body.left(m.capturedStart()) : body;
        header->setLineCount(printed.trimmed().isEmpty()
                                 ? 0
                                 : int(printed.trimmed().count(QLatin1Char('\n'))) + 1);
    }
    if (m.hasMatch()) {
        const int code = m.captured(1).toInt();
        body = body.left(m.capturedStart());
        exitLabel = new QLabel(QStringLiteral("exit %1").arg(code));
        exitLabel->setFont(monoFont());
        exitLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        exitLabel->setStyleSheet(Theme::remapSheet(
            QStringLiteral("color:%1;background:%2;border:1px solid %3;"
                           "border-radius:5px;padding:1px 6px;font-size:11px;"
                           "font-weight:700;")
                .arg(code == 0 ? m_p.add : m_p.del, m_p.chipBg, m_p.termEdge)));
    }

    if (tc.terminalOutput) {
        tc.liveOutputText = body;
        static_cast<TerminalOutputView *>(tc.terminalOutput.data())
            ->setOutputText(body);
        if (exitLabel)
            if (QVBoxLayout *column = commandOutputColumn(tc))
                column->addWidget(exitLabel, 0, Qt::AlignLeft);
        return;
    }
    if (tc.liveOutput) {
        tc.liveOutputText = body;
        tc.liveOutput->setText(capLabelText(body, true));
        if (exitLabel)
            if (QVBoxLayout *column = commandOutputColumn(tc))
                column->addWidget(exitLabel, 0, Qt::AlignLeft);
        return;
    }
    QVBoxLayout *column = commandOutputColumn(tc);
    if (!column) {
        delete exitLabel;
        return;
    }
    if (!body.trimmed().isEmpty()) {
        if (m_terminalOutputMode) {
            auto *terminal = new TerminalOutputView(
                body, isError ? m_p.del : m_p.muted, m_p.shellPath,
                tc.panelBox);
            enableReferenceLinks(terminal->outputLabel());
            tc.terminalOutput = terminal;
            tc.liveOutputText = body;
            column->addWidget(tc.terminalOutput);
        } else {
            auto *peek =
                new OutputPeek(body, isError ? m_p.del : m_p.muted, m_p, true);
            for (QLabel *label : peek->findChildren<QLabel *>())
                enableReferenceLinks(label);
            column->addWidget(peek);
        }
    }
    if (exitLabel)
        column->addWidget(exitLabel, 0, Qt::AlignLeft);
}

QString ClaudeTranscriptView::toolSubtitle(const QString &name,
                                           const QJsonObject &input) const
{
    return forkmesh::transcripttools::subtitle(name, input);
}

QWidget *ClaudeTranscriptView::makeMarkdown(const QString &markdown)
{
    auto *l = new CacheLabel;
    l->setTextFormat(Qt::MarkdownText);
    l->setText(linkifyReferences(markdown));
    l->setWordWrap(true);
    enableReferenceLinks(l);
    l->setFont(monoFont());
    l->setStyleSheet(
        QStringLiteral("color:%1;background:transparent;border:none;").arg(m_p.text));
    if (markdown.size() > 1500)
        return makeCollapsible(QStringLiteral("Details"), l, false);
    return l;
}

QWidget *ClaudeTranscriptView::toolBody(const QString &name, const QJsonObject &input)
{
    // Bash never reaches here: addToolUse gives every command its own terminal
    // panel, and the only input that falls through to this is one with no
    // command to show at all.
    if (input.value(QStringLiteral("_summaryOnly")).toBool())
        return nullptr;
    if (name == QLatin1String("Edit"))
        return makeDiff(input.value(QStringLiteral("old_string")).toString(),
                        input.value(QStringLiteral("new_string")).toString());
    if (name == QLatin1String("Write"))
        return makeMono(input.value(QStringLiteral("content")).toString(), true);
    if (name == QLatin1String("MultiEdit")) {
        auto *holder = new QWidget;
        holder->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
        auto *v = new QVBoxLayout(holder);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(6);
        for (const QJsonValue &ev : input.value(QStringLiteral("edits")).toArray()) {
            const QJsonObject e = ev.toObject();
            if (QWidget *diff = makeDiff(e.value(QStringLiteral("old_string")).toString(),
                                         e.value(QStringLiteral("new_string")).toString()))
                v->addWidget(diff);
        }
        if (v->isEmpty()) {
            delete holder;
            return nullptr;
        }
        return holder;
    }
    if (name == QLatin1String("TodoWrite")) {
        QStringList rows;
        for (const QJsonValue &tv : input.value(QStringLiteral("todos")).toArray()) {
            const QJsonObject t = tv.toObject();
            const QString st = t.value(QStringLiteral("status")).toString();
            const QString item = esc(t.value(QStringLiteral("content")).toString());
            if (st == QLatin1String("completed"))
                rows << QStringLiteral("<span style='color:%1'>☒ <s>%2</s></span>")
                            .arg(m_p.muted, item);
            else if (st == QLatin1String("in_progress"))
                rows << QStringLiteral(
                            "<span style='color:%1'>■</span> <b>%2</b>")
                            .arg(accentFor(name), item);
            else
                rows << QStringLiteral("☐ %1").arg(item);
        }
        if (rows.isEmpty())
            return nullptr;
        auto *l = new CacheLabel;
        l->setTextFormat(Qt::RichText);
        l->setText(QStringLiteral("<div style='color:%1'>%2</div>")
                       .arg(m_p.text, rows.join(QStringLiteral("<br>"))));
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setFont(monoFont());
        l->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;border:none;")));
        return l;
    }
    if (forkmesh::transcripttools::isSubagentTool(name)) {
        const QString prompt =
            input.value(QStringLiteral("prompt")).toString().trimmed();
        return prompt.isEmpty() ? nullptr : makeMono(prompt, true);
    }
    if (name == QLatin1String("FileChange")) {
        auto *holder = new QWidget;
        holder->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
        auto *layout = new QVBoxLayout(holder);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        for (const QJsonValue &value : input.value(QStringLiteral("changes")).toArray()) {
            const QJsonObject change = value.toObject();
            auto *path = new QLabel(
                referenceHtml(change.value(QStringLiteral("path")).toString()));
            path->setTextFormat(Qt::RichText);
            enableReferenceLinks(path);
            path->setStyleSheet(Theme::remapSheet(
                QStringLiteral("color:%1;font-weight:600;background:transparent;")
                    .arg(m_p.muted)));
            layout->addWidget(path);
            layout->addWidget(makeCode(
                change.value(QStringLiteral("diff")).toString(), true));
        }
        return holder;
    }
    const QString markdown = forkmesh::transcripttools::bodyMarkdown(name, input);
    if (!markdown.isEmpty())
        return makeMarkdown(markdown);
    if (forkmesh::transcripttools::bodyHidden(name))
        return nullptr; // the subtitle already says the file/pattern/query
    if (!input.isEmpty())
        return makeMono(QString::fromUtf8(
                            QJsonDocument(input).toJson(QJsonDocument::Indented))
                            .trimmed(),
                        true);
    return nullptr;
}

QString ClaudeTranscriptView::fileEditSubtitleHtml(
    const QStringList &paths, const forkmesh::transcripttools::EditStats &stats) const
{
    constexpr int kShownPaths = 3;
    QStringList links;
    for (const QString &path : paths.mid(0, kShownPaths)) {
        QString name = path.section(QLatin1Char('/'), -1);
        if (name.isEmpty())
            name = path;
        links << QStringLiteral("<a href=\"forkmesh-file:%1\">%2</a>")
                     .arg(QString::fromLatin1(QUrl::toPercentEncoding(path)), esc(name));
    }
    QString html = links.join(QStringLiteral(", "));
    if (paths.size() > kShownPaths)
        html += QStringLiteral(" <span style='color:%1'>and %2 more</span>")
                    .arg(m_p.muted)
                    .arg(paths.size() - kShownPaths);
    if (stats.known && stats.added > 0)
        html += QStringLiteral(" <span style='color:%1;font-weight:700'>+%2</span>")
                    .arg(m_p.add)
                    .arg(stats.added);
    if (stats.known && stats.removed > 0)
        html += QStringLiteral(" <span style='color:%1;font-weight:700'>−%2</span>")
                    .arg(m_p.del)
                    .arg(stats.removed);
    return html;
}

QWidget *ClaudeTranscriptView::dotHeader(const QString &name,
                                         const QString &subtitle,
                                         bool bareSubtitle, bool html)
{
    auto *l = new CacheLabel;
    l->setTextFormat(Qt::RichText);
    l->setWordWrap(true);
    enableReferenceLinks(l);
    l->setFont(monoFont());
    QString markup = QStringLiteral("<span style='color:%1;font-weight:700'>%2</span>")
                         .arg(m_p.text, esc(name));
    if (!subtitle.trimmed().isEmpty()) {
        const QString arg = html ? subtitle.trimmed()
                                 : linkifyTranscriptText(subtitle.trimmed(), true);
        if (bareSubtitle)
            markup += QStringLiteral(" <span style='color:%1'>%2</span>")
                          .arg(m_p.text, arg);
        else
            // A dim middot rather than parentheses: the path is the interesting
            // half of the line, and brackets around it only added noise.
            markup += QStringLiteral(
                          " <span style='color:%1'>·</span> "
                          "<span style='color:%2'>%3</span>")
                          .arg(m_p.border, m_p.muted, arg);
    }
    l->setText(markup);
    l->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;border:none;")));
    return l;
}

QWidget *ClaudeTranscriptView::connectorRow(QWidget *content, const QString &glyph)
{
    Q_UNUSED(glyph); // the rule spans the row, so there is no glyph to choose
    auto *row = new QWidget;
    row->setMinimumWidth(0);
    row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->setStyleSheet(Theme::remapSheet(QStringLiteral("background:transparent;")));
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 1, 0, 1);
    h->setSpacing(7);
    h->addWidget(new ConnectorRule(m_p.border, row));
    content->setParent(row);
    content->setMinimumWidth(0);
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    h->addWidget(content, 1);
    return row;
}

static QString capLabelText(const QString &text, bool collapseLines)
{
    constexpr int kMaxLines = 16;        // matches the long-output collapse below
    constexpr int kMaxLineChars = 2000;  // one wrapped line stays cheap to lay out
    constexpr int kMaxTotalChars = 20000;
    const QStringList lines = text.split(QLatin1Char('\n'));
    const bool longText = collapseLines && lines.size() > kMaxLines;
    QStringList shownLines = longText ? lines.mid(0, kMaxLines) : lines;
    for (QString &ln : shownLines)
        if (ln.size() > kMaxLineChars)
            ln = ln.left(kMaxLineChars) + QStringLiteral(" …");
    QString shown = shownLines.join(QLatin1Char('\n'));
    if (longText)
        shown += QStringLiteral("\n… (%1 more lines)").arg(lines.size() - kMaxLines);
    if (shown.size() > kMaxTotalChars)
        shown = shown.left(kMaxTotalChars) + QStringLiteral("\n… (truncated)");
    return shown;
}

QWidget *ClaudeTranscriptView::makeMono(const QString &text, bool collapsedIfLong)
{
    const QString shown = capLabelText(text, collapsedIfLong);
    auto *l = new CacheLabel;
    l->setTextFormat(Qt::RichText);
    l->setText(referenceHtml(shown));
    l->setWordWrap(true);
    enableReferenceLinks(l);
    l->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    l->setStyleSheet(Theme::remapSheet(
        QStringLiteral("color:%1;background:transparent;border:none;").arg(m_p.text)));
    return l;
}

QWidget *ClaudeTranscriptView::makeCode(const QString &text, bool collapsedIfLong)
{
    const QString shown = capLabelText(text, collapsedIfLong);
    auto *l = new CacheLabel;
    l->setTextFormat(Qt::PlainText);
    l->setText(shown);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    // The canvas colour made a code block invisible against the transcript it
    // sat in. Its own surface plus a hairline gives it an edge to be read from.
    l->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "color:%1;background:%2;border:1px solid %3;border-radius:8px;"
        "padding:8px 10px;").arg(m_p.text, m_p.termBg, m_p.termEdge)));
    return l;
}

QWidget *ClaudeTranscriptView::makeDiff(const QString &oldText, const QString &newText)
{
    using forkmesh::transcripttools::DiffLine;
    QVector<QVector<DiffLine>> hunks;
    bool inHunk = false;
    for (const DiffLine &line : forkmesh::transcripttools::lineDiff(oldText, newText)) {
        if (line.kind == QLatin1Char(' ')) {
            inHunk = false;
            continue;
        }
        if (!inHunk)
            hunks.append(QVector<DiffLine>());
        inHunk = true;
        hunks.last().append(line);
    }
    if (hunks.isEmpty())
        return nullptr;

    QString html;
    if (m_splitDiffs) {
        html = QStringLiteral("<table style='border-collapse:collapse;"
                              "font-family:monospace;white-space:pre;width:100%'>");
        auto cell = [&](const QString &s, const QString &color, const QString &bg) {
            return QStringLiteral("<td style='width:50%;color:%1;background:%2;"
                                  "padding:0 6px;vertical-align:top'>%3</td>")
                .arg(color, bg, s.isEmpty() ? QStringLiteral("&nbsp;") : esc(s));
        };
        for (int h = 0; h < hunks.size(); ++h) {
            if (h > 0)
                html += QStringLiteral("<tr>") + cell(QStringLiteral("⋯"), m_p.muted, m_p.termBg)
                        + cell(QStringLiteral("⋯"), m_p.muted, m_p.termBg)
                        + QStringLiteral("</tr>");
            QStringList dels, adds;
            for (const DiffLine &line : hunks[h])
                (line.kind == QLatin1Char('-') ? dels : adds) << line.text;
            for (int i = 0; i < qMax(dels.size(), adds.size()); ++i) {
                const bool hasDel = i < dels.size(), hasAdd = i < adds.size();
                html += QStringLiteral("<tr>")
                        + cell(hasDel ? QStringLiteral("- ") + dels[i] : QString(),
                               m_p.del, hasDel ? m_p.delBg : m_p.termBg)
                        + cell(hasAdd ? QStringLiteral("+ ") + adds[i] : QString(),
                               m_p.add, hasAdd ? m_p.addBg : m_p.termBg)
                        + QStringLiteral("</tr>");
            }
        }
        html += QStringLiteral("</table>");
    } else {
        html = QStringLiteral("<div style='font-family:monospace;white-space:pre'>");
        for (int h = 0; h < hunks.size(); ++h) {
            if (h > 0)
                html += QStringLiteral("<div style='color:%1'>  ⋯</div>").arg(m_p.muted);
            for (const DiffLine &line : hunks[h]) {
                const bool add = line.kind == QLatin1Char('+');
                html += QStringLiteral("<div style='color:%1;background:%2'>%3 %4</div>")
                            .arg(add ? m_p.add : m_p.del, add ? m_p.addBg : m_p.delBg,
                                 QString(line.kind), esc(line.text));
            }
        }
        html += QStringLiteral("</div>");
    }

    auto *l = new CacheLabel(html);
    l->setObjectName(QStringLiteral("agentEditDiff"));
    l->setTextFormat(Qt::RichText);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "background:%1;border:1px solid %2;border-radius:8px;padding:8px 10px;")
        .arg(m_p.termBg, m_p.termEdge)));
    return l;
}

QString ClaudeTranscriptView::highlightedTextFor(const QString &orig,
                                                 Qt::TextFormat fmt,
                                                 int currentLocalOcc) const
{
    const QString normalSpan =
        QStringLiteral("background:%1;color:%2;").arg(kMatchBg, kMatchFg);
    const QString currentSpan =
        QStringLiteral("background:%1;color:%2;").arg(kCurMatchBg, kMatchFg);

    if (fmt != Qt::MarkdownText && fmt != Qt::RichText) {
        QString out;
        int from = 0, occ = 0;
        const int qlen = m_searchQuery.size();
        while (true) {
            const int i = orig.indexOf(m_searchQuery, from, Qt::CaseInsensitive);
            if (i < 0) {
                out += esc(orig.mid(from));
                break;
            }
            out += esc(orig.mid(from, i - from));
            out += QStringLiteral("<span style='%1'>%2</span>")
                       .arg(occ == currentLocalOcc ? currentSpan : normalSpan,
                            esc(orig.mid(i, qlen)));
            from = i + qlen;
            ++occ;
        }
        return QStringLiteral("<div style='white-space:pre-wrap'>%1</div>").arg(out);
    }

    QTextDocument d;
    if (fmt == Qt::MarkdownText)
        d.setMarkdown(orig);
    else
        d.setHtml(orig);
    QTextCharFormat normal;
    normal.setBackground(QColor(kMatchBg));
    normal.setForeground(QColor(kMatchFg));
    QTextCharFormat current;
    current.setBackground(QColor(kCurMatchBg));
    current.setForeground(QColor(kMatchFg));
    int occ = 0;
    for (QTextCursor c = d.find(m_searchQuery); !c.isNull();
         c = d.find(m_searchQuery, c)) {
        c.mergeCharFormat(occ == currentLocalOcc ? current : normal);
        ++occ;
    }
    return d.toHtml();
}

void ClaudeTranscriptView::rebuildSearchMatches()
{
    flushStreamingText(); // never search a label that is one flush out of date
    m_searchLabels.clear();
    m_searchTotal = 0;
    if (m_searchQuery.isEmpty() || !m_container || !m_col)
        return;
    for (int i = 0; i < m_col->count(); ++i) {
        QLayoutItem *item = m_col->itemAt(i);
        QWidget *row = item ? item->widget() : nullptr;
        if (!row)
            continue;
        const QList<QLabel *> labels = row->findChildren<QLabel *>();
        for (QLabel *l : labels) {
            if (!l)
                continue;
            const Qt::TextFormat fmt = l->textFormat();
            const int c = countMatchesIn(l->text(), fmt, m_searchQuery);
            if (c > 0) {
                m_searchLabels.push_back({l, fmt, l->text(), c});
                m_searchTotal += c;
            }
        }
    }
}

void ClaudeTranscriptView::renderSearchHighlights()
{
    int base = 0;
    for (LabelHit &h : m_searchLabels) {
        if (!h.label) {
            base += h.count;
            continue;
        }
        const int curLocal = (m_searchCurrent >= base
                              && m_searchCurrent < base + h.count)
                                 ? m_searchCurrent - base
                                 : -1;
        h.label->setTextFormat(Qt::RichText);
        h.label->setText(highlightedTextFor(h.orig, h.fmt, curLocal));
        base += h.count;
    }
}

void ClaudeTranscriptView::restoreSearchOriginals()
{
    for (LabelHit &h : m_searchLabels) {
        if (!h.label)
            continue;
        h.label->setTextFormat(h.fmt);
        h.label->setText(h.orig);
    }
}

QLabel *ClaudeTranscriptView::currentMatchLabel() const
{
    if (m_searchCurrent < 0)
        return nullptr;
    int base = 0;
    for (const LabelHit &h : m_searchLabels) {
        if (m_searchCurrent < base + h.count)
            return h.label;
        base += h.count;
    }
    return nullptr;
}

void ClaudeTranscriptView::scrollToCurrentMatch()
{
    QLabel *l = currentMatchLabel();
    if (!l)
        return;
    for (QWidget *w = l->parentWidget(); w; w = w->parentWidget()) {
        if (auto *c = dynamic_cast<Collapsible *>(w))
            c->setExpanded(true);
        else if (auto *card = dynamic_cast<CommandCard *>(w))
            card->setExpanded(true);
        else if (auto *group = dynamic_cast<TranscriptCommandGroup *>(w))
            group->setExpanded(true);
        else if (auto *edit = dynamic_cast<FileEditCard *>(w))
            edit->setExpanded(true);
        else if (auto *o = dynamic_cast<OutputPeek *>(w))
            o->expand();
    }
    m_stickBottom = false; // jumping to a match takes us off the live tail
    ensureWidgetVisible(l, 40, 80);
}

int ClaudeTranscriptView::search(const QString &query)
{
    restoreSearchOriginals();
    m_searchQuery = query;
    rebuildSearchMatches();
    m_searchCurrent = m_searchTotal > 0 ? 0 : -1;
    renderSearchHighlights();
    scrollToCurrentMatch();
    emit searchResultsChanged(m_searchTotal > 0 ? m_searchCurrent + 1 : 0,
                              m_searchTotal);
    return m_searchTotal;
}

void ClaudeTranscriptView::clearSearch()
{
    restoreSearchOriginals();
    m_searchLabels.clear();
    m_searchQuery.clear();
    m_searchTotal = 0;
    m_searchCurrent = -1;
    emit searchResultsChanged(0, 0);
}

void ClaudeTranscriptView::stepMatch(int delta)
{
    if (m_searchTotal <= 0)
        return;
    m_searchCurrent =
        ((m_searchCurrent + delta) % m_searchTotal + m_searchTotal) % m_searchTotal;
    renderSearchHighlights();
    scrollToCurrentMatch();
    emit searchResultsChanged(m_searchCurrent + 1, m_searchTotal);
}

void ClaudeTranscriptView::searchNext() { stepMatch(+1); }
void ClaudeTranscriptView::searchPrev() { stepMatch(-1); }
