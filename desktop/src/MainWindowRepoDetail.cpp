
#include "MainWindow.h"
#include "OfficeChannelMirror.h"
#include "MainWindowInternal.h"
#include "NetworkFilesPanel.h"
#include "SecretDiffBanner.h"
#include "ActionStore.h"
#include "GitRefusal.h"
#include "SettingsSectionsView.h"
#include "CurrentPageStack.h"
#include "PageSwitchTrace.h"
#include "CoveFileEditor.h"
#include "CoveFileSupport.h"
#include "FlowLayout.h"
#include "ForkMeshPackageRegistry.h"
#include "GlobalSearchMatch.h"
#include "HiddenTerms.h"
#include "SearchHitDelegate.h"
#include "HeaderWrapLayout.h"
#include "KebabHeaderView.h"
#include "MarkdownPreviewStyle.h"
#include "MarkdownRender.h"
#include "RestartTiming.h"
#include "UpstreamCheckoutSync.h"

#include <QBoxLayout>
#include <QCheckBox>
#include <QFrame>
#include <QPushButton>
#include <QSettings>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QToolButton>
#include <QSignalBlocker>
#include <QTextFrame>
#include <QTextTable>
#include <QDesktopServices>
#include <QGridLayout>
#include <QHeaderView>
#include <QMenu>
#include <QPainter>
#include <QPointer>

#include <algorithm>
#include <functional>
#include <optional>

using namespace forkmesh::ui;
using namespace forkmesh::search;

// The Code page hangs directly off the activity rail, so it carries its own
// left inset; the latest-commit line leads with a portrait this size.
constexpr int kRepoPageLeftInset = 14;
constexpr int kCommitBarAvatarPx = 20;

namespace {

constexpr int kNextRepoTabHeight = 44;
constexpr int kNextRepoToolHeight = 32;
constexpr int kNextRepoGhostHeight = 28;
constexpr int kNextCodeRowHeight = 36;
constexpr int kNextCodeHeaderHeight = 48;
constexpr int kNextCodeNameWidth = 300;
constexpr int kNextAboutWidth = 296;
constexpr int kNextAboutMinBodyWidth = 860;
constexpr int kNextOverviewTimeRole = Qt::UserRole + 2;
const char *const kNextButtonKind = "nextRepoButtonKind";
const char *const kNextButtonFitted = "nextRepoButtonFitted";
const char *const kNextButtonFitPending = "nextRepoButtonFitPending";
const char *const kNextStylerName = "nextRepoButtonStyler";
const char *const kNextAboutName = "repoAboutSidebar";

enum NextRepoButtonKind { NextRepoTab = 1, NextRepoAction = 2, NextRepoTool = 3 };

QFont nextRepoFont(const QWidget *widget, int px, QFont::Weight weight)
{
    QFont font = widget->font();
    font.setPixelSize(px);
    font.setWeight(weight);
    return font;
}

int nextRepoPillWidth(const QFont &font, const QString &text)
{
    return qMax(18, QFontMetrics(font).horizontalAdvance(text) + 12);
}

void paintNextRepoPill(QPainter &painter, const QRectF &rect, const QString &text,
                       const QColor &fill, const QColor &fg, const QFont &font)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, rect.height() / 2.0, rect.height() / 2.0);
    painter.setFont(font);
    painter.setPen(fg);
    painter.drawText(rect, Qt::AlignCenter, text);
}

class NextRepoButtonStyler : public QObject
{
public:
    explicit NextRepoButtonStyler(QObject *parent) : QObject(parent)
    {
        setObjectName(QLatin1String(kNextStylerName));
    }

    static NextRepoButtonStyler *forWindow(QObject *window)
    {
        if (QObject *found = window->findChild<QObject *>(
                QLatin1String(kNextStylerName), Qt::FindDirectChildrenOnly))
            return static_cast<NextRepoButtonStyler *>(found);
        return new NextRepoButtonStyler(window);
    }

    void adopt(QWidget *widget, NextRepoButtonKind kind)
    {
        auto *button = dynamic_cast<VerticalIconButton *>(widget);
        if (!button || button->property(kNextButtonKind).toInt() != 0)
            return;
        button->setProperty(kNextButtonKind, int(kind));
        button->installEventFilter(this);
        fit(button);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        if (type != QEvent::Paint && type != QEvent::Show && type != QEvent::FontChange &&
            type != QEvent::Polish && type != QEvent::StyleChange)
            return false;
        auto *button = dynamic_cast<VerticalIconButton *>(watched);
        if (!button)
            return false;
        if (type == QEvent::Polish || type == QEvent::StyleChange) {
            scheduleFit(button);
            return false;
        }
        if (type != QEvent::Paint) {
            fit(button);
            return false;
        }
        if (!active(button)) {
            release(button);
            return false;
        }
        const QSize want = preferredSize(button);
        if (button->minimumSize() != want || button->maximumSize() != want)
            scheduleFit(button);
        paint(button);
        return true;
    }

private:
    static int kindOf(const QWidget *button)
    {
        return button->property(kNextButtonKind).toInt();
    }

    static bool active(const VerticalIconButton *button)
    {
        return Theme::isNext() && !button->inRail() && kindOf(button) != 0;
    }

    static QSize preferredSize(const VerticalIconButton *button)
    {
        const int kind = kindOf(button);
        const QString text = button->text();
        const QFont pill = nextRepoFont(button, 11, QFont::Medium);
        if (kind == NextRepoTool) {
            const QFont label = nextRepoFont(button, 13, QFont::Normal);
            const QFont count = nextRepoFont(button, 13, QFont::DemiBold);
            int width = 10 + 16 + 6 + labelWidth(label, text) + 10;
            if (button->badgeCount() > 0)
                width += QFontMetrics(count).horizontalAdvance(
                             formatCount(button->badgeCount())) + 5;
            if (button->menu())
                width += 4 + 12;
            return QSize(width, kNextRepoToolHeight);
        }
        const QFont label = nextRepoFont(button, 13, QFont::Medium);
        const int pad = kind == NextRepoTab ? 12 : 10;
        int width = pad + 16 + 6 + labelWidth(label, text) + pad;
        if (button->badgeCount() > 0)
            width += 6 + nextRepoPillWidth(pill, formatCount(button->badgeCount()));
        if (button->alertBadgeCount() > 0)
            width += 4 + nextRepoPillWidth(pill, formatCount(button->alertBadgeCount()));
        if (button->menu())
            width += 4 + 12;
        return QSize(width, kNextRepoTabHeight);
    }

    static int labelWidth(const QFont &font, const QString &text)
    {
        return qCeil(QFontMetricsF(font).horizontalAdvance(text)) + 2;
    }

    static void relayoutScrollArea(QWidget *button)
    {
        for (QWidget *ancestor = button->parentWidget(); ancestor;
             ancestor = ancestor->parentWidget()) {
            if (auto *area = qobject_cast<QScrollArea *>(ancestor)) {
                QCoreApplication::postEvent(area, new QEvent(QEvent::LayoutRequest));
                return;
            }
        }
    }

    static void fit(VerticalIconButton *button)
    {
        if (!active(button)) {
            release(button);
            return;
        }
        const QSize want = preferredSize(button);
        if (button->minimumSize() != want || button->maximumSize() != want) {
            button->setFixedSize(want);
            relayoutScrollArea(button);
        }
        button->setProperty(kNextButtonFitted, true);
    }

    static void release(VerticalIconButton *button)
    {
        if (!button->property(kNextButtonFitted).toBool())
            return;
        button->setProperty(kNextButtonFitted, false);
        if (!button->property("forkmeshRailFitted").toBool()) {
            button->setMinimumWidth(0);
            button->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        button->setMinimumHeight(0);
        button->setMaximumHeight(QWIDGETSIZE_MAX);
    }

    static void scheduleFit(VerticalIconButton *button)
    {
        if (button->property(kNextButtonFitPending).toBool())
            return;
        button->setProperty(kNextButtonFitPending, true);
        QPointer<VerticalIconButton> guarded(button);
        QTimer::singleShot(0, button, [guarded] {
            if (!guarded)
                return;
            guarded->setProperty(kNextButtonFitPending, false);
            fit(guarded);
        });
    }

    static void paint(VerticalIconButton *button)
    {
        QPainter painter(button);
        painter.setRenderHint(QPainter::Antialiasing);
        const int kind = kindOf(button);
        const bool enabled = button->isEnabled();
        const bool hovered = enabled && button->underMouse();
        const bool pressed = enabled && button->isDown();
        const bool checked = button->isChecked();
        const int width = button->width();
        const int height = button->height();

        QColor fg = Theme::color(checked || hovered || pressed ? Theme::Role::Fg
                                                               : Theme::Role::FgMuted);
        if (!enabled)
            fg = Theme::color(Theme::Role::FgSubtle);
        const QColor accent = button->accentColor();
        const QColor glyph = accent.isValid() && enabled ? accent : fg;

        if (kind != NextRepoTab && (hovered || pressed || checked)) {
            const QRectF body =
                kind == NextRepoAction
                    ? QRectF(0.5, (height - kNextRepoGhostHeight) / 2.0, width - 1.0,
                             kNextRepoGhostHeight)
                    : QRectF(0.5, 0.5, width - 1.0, height - 1.0);
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::color(pressed || checked ? Theme::Role::SurfaceActive
                                                             : Theme::Role::SurfaceRaised));
            painter.drawRoundedRect(body, 6, 6);
        }

        int x = kind == NextRepoTab ? 12 : 10;
        const QRect iconRect(x, (height - 16) / 2, 16, 16);
        const QString iconName = button->property("forkmeshOcticon").toString();
        if (iconName.isEmpty())
            button->icon().paint(&painter, iconRect, Qt::AlignCenter,
                                 enabled ? QIcon::Normal : QIcon::Disabled);
        else
            painter.drawPixmap(iconRect.topLeft(),
                               tintedOcticonPixmap(iconName, glyph, 16));
        if (button->hasAttentionLight()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::color(Theme::Role::AccentFg));
            painter.drawEllipse(QRectF(iconRect.right() - 2, iconRect.top() - 2, 6, 6));
        }
        x = iconRect.right() + 1 + 6;

        const QFont pillFont = nextRepoFont(button, 11, QFont::Medium);
        int trailing = kind == NextRepoTab ? 12 : 10;
        if (button->menu())
            trailing += 4 + 12;
        if (kind != NextRepoTool) {
            if (button->badgeCount() > 0)
                trailing += 6 + nextRepoPillWidth(pillFont, formatCount(button->badgeCount()));
            if (button->alertBadgeCount() > 0)
                trailing += 4 + nextRepoPillWidth(pillFont,
                                                  formatCount(button->alertBadgeCount()));
        }

        if (kind == NextRepoTool && button->badgeCount() > 0) {
            const QFont countFont = nextRepoFont(button, 13, QFont::DemiBold);
            const QString count = formatCount(button->badgeCount());
            const int countWidth = QFontMetrics(countFont).horizontalAdvance(count);
            painter.setFont(countFont);
            painter.setPen(Theme::color(enabled ? Theme::Role::Fg : Theme::Role::FgSubtle));
            painter.drawText(QRect(x, 0, countWidth, height), Qt::AlignVCenter | Qt::AlignLeft,
                             count);
            x += countWidth + 5;
        }

        const QFont labelFont =
            nextRepoFont(button, 13, kind == NextRepoTool ? QFont::Normal : QFont::Medium);
        const int labelRoom = qMax(0, width - x - trailing);
        const QString label =
            QFontMetrics(labelFont).elidedText(button->text(), Qt::ElideRight, labelRoom);
        painter.setFont(labelFont);
        painter.setPen(accent.isValid() && enabled && (checked || hovered) ? accent : fg);
        painter.drawText(QRect(x, 0, labelRoom, height), Qt::AlignVCenter | Qt::AlignLeft,
                         label);
        x += QFontMetrics(labelFont).horizontalAdvance(label);

        if (kind != NextRepoTool) {
            const double pillTop = (height - 18) / 2.0;
            if (button->badgeCount() > 0) {
                x += 6;
                const QString count = formatCount(button->badgeCount());
                const int pillWidth = nextRepoPillWidth(pillFont, count);
                paintNextRepoPill(painter, QRectF(x, pillTop, pillWidth, 18), count,
                                  Theme::color(Theme::Role::SurfaceActive),
                                  Theme::color(Theme::Role::FgSecondary), pillFont);
                x += pillWidth;
            }
            if (button->alertBadgeCount() > 0) {
                x += 4;
                const QString count = formatCount(button->alertBadgeCount());
                const int pillWidth = nextRepoPillWidth(pillFont, count);
                paintNextRepoPill(painter, QRectF(x, pillTop, pillWidth, 18), count,
                                  Theme::color(Theme::Role::Danger),
                                  Theme::color(Theme::Role::OnAccent), pillFont);
                x += pillWidth;
            }
        }

        if (button->menu()) {
            x += 4;
            painter.drawPixmap(QPoint(x, (height - 12) / 2),
                               tintedOcticonPixmap(QStringLiteral("chevron-down"),
                                                   Theme::color(Theme::Role::FgMuted), 12));
        }

        if (kind == NextRepoTab && checked) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(accent.isValid() ? accent : Theme::color(Theme::Role::AccentFg));
            painter.drawRoundedRect(QRectF(8, height - 2, qMax(0, width - 16), 2), 1, 1);
        }
    }
};

class NextOverviewRowDelegate : public HoverRowDelegate
{
public:
    explicit NextOverviewRowDelegate(QAbstractItemView *view) : HoverRowDelegate(view)
    {
        m_hoverFill = true;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        HoverRowDelegate::paint(painter, option, index);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);
        if (index.row() > 0 && !index.parent().isValid()) {
            painter->setPen(QPen(Theme::color(Theme::Role::Border), 1));
            painter->drawLine(option.rect.topLeft(), option.rect.topRight());
        }
        if (index.column() == 1) {
            const QString when = index.data(kNextOverviewTimeRole).toString();
            QFont timeFont = option.font;
            timeFont.setPixelSize(12);
            const int timeWidth =
                when.isEmpty() ? 0 : QFontMetrics(timeFont).horizontalAdvance(when);
            const QRect inner = option.rect.adjusted(12, 0, -16, 0);
            if (!when.isEmpty()) {
                painter->setFont(timeFont);
                painter->setPen(Theme::color(Theme::Role::FgSubtle));
                painter->drawText(inner, Qt::AlignVCenter | Qt::AlignRight, when);
            }
            QFont subjectFont = option.font;
            subjectFont.setPixelSize(13);
            const QRect subjectRect =
                inner.adjusted(0, 0, -(timeWidth > 0 ? timeWidth + 16 : 0), 0);
            painter->setFont(subjectFont);
            painter->setPen(Theme::color(Theme::Role::FgMuted));
            painter->drawText(subjectRect, Qt::AlignVCenter | Qt::AlignLeft,
                              QFontMetrics(subjectFont).elidedText(
                                  index.data(Qt::DisplayRole).toString(),
                                  Qt::ElideRight, qMax(0, subjectRect.width())));
        }
        painter->restore();
    }

protected:
    void initStyleOption(QStyleOptionViewItem *option,
                         const QModelIndex &index) const override
    {
        HoverRowDelegate::initStyleOption(option, index);
        if (index.column() == 1)
            option->text.clear();
    }
};

class NextOverviewCommitHeader : public QHeaderView
{
public:
    NextOverviewCommitHeader(QWidget *card, QWidget *parent)
        : QHeaderView(Qt::Horizontal, parent), m_card(card)
    {
        setObjectName(QStringLiteral("overviewCommitHeader"));
        setSectionsClickable(false);
        card->setParent(this);
        card->show();
    }

    QSize sizeHint() const override
    {
        QSize size = QHeaderView::sizeHint();
        size.setHeight(kNextCodeHeaderHeight);
        return size;
    }

    void reclaim()
    {
        if (!m_card)
            return;
        if (m_card->parentWidget() != this)
            m_card->setParent(this);
        m_card->setGeometry(rect());
        m_card->show();
        m_card->raise();
    }

protected:
    void paintEvent(QPaintEvent *) override {}

    void resizeEvent(QResizeEvent *event) override
    {
        QHeaderView::resizeEvent(event);
        if (m_card) {
            m_card->setGeometry(rect());
            m_card->raise();
        }
    }

private:
    QPointer<QWidget> m_card;
};

class NextWidthWatcher : public QObject
{
public:
    NextWidthWatcher(QWidget *watched, std::function<void(int)> onWidth)
        : QObject(watched), m_onWidth(std::move(onWidth))
    {
        watched->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Resize && m_onWidth)
            m_onWidth(static_cast<QWidget *>(watched)->width());
        return false;
    }

private:
    std::function<void(int)> m_onWidth;
};

class NextTextFollower : public QObject
{
public:
    NextTextFollower(QPushButton *source, QPushButton *follower)
        : QObject(follower), m_source(source), m_follower(follower)
    {
        if (source)
            source->installEventFilter(this);
        follower->installEventFilter(this);
        sync();
    }

    void sync()
    {
        if (m_source && m_follower && m_follower->text() != m_source->text()) {
            m_follower->setText(m_source->text());
            m_follower->setToolTip(m_source->toolTip());
        }
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint || event->type() == QEvent::Show)
            sync();
        return false;
    }

private:
    QPointer<QPushButton> m_source;
    QPointer<QPushButton> m_follower;
};

class NextLanguageBar : public QWidget
{
public:
    explicit NextLanguageBar(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("repoAboutLanguageBar"));
        setFixedHeight(8);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setSegments(const QList<QPair<QColor, double>> &segments)
    {
        m_segments = segments;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bar(0, 0, width(), height());
        QPainterPath clip;
        clip.addRoundedRect(bar, height() / 2.0, height() / 2.0);
        painter.setClipPath(clip);
        painter.fillRect(bar, Theme::color(Theme::Role::SurfaceActive));
        double x = 0;
        const int gaps = qMax(0, int(m_segments.size()) - 1);
        const double room = qMax(0.0, width() - gaps * 2.0);
        for (int i = 0; i < m_segments.size(); ++i) {
            const double segment = room * m_segments.at(i).second;
            painter.fillRect(QRectF(x, 0, segment, height()), m_segments.at(i).first);
            x += segment + 2.0;
        }
    }

private:
    QList<QPair<QColor, double>> m_segments;
};

QLabel *nextAboutLabel(const QString &role, const QString &text = QString())
{
    auto *label = new QLabel(text);
    label->setProperty("aboutRole", role);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QFrame *nextAboutRule()
{
    auto *rule = new QFrame;
    rule->setProperty("aboutRole", QStringLiteral("rule"));
    rule->setFixedHeight(1);
    return rule;
}

void clearNextLayout(QLayout *layout)
{
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

QWidget *nextAboutFact(const QString &icon, QWidget *text)
{
    auto *row = new QWidget;
    row->setProperty("aboutRole", QStringLiteral("factRow"));
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto *glyph = new QLabel;
    glyph->setProperty("aboutRole", QStringLiteral("glyph"));
    glyph->setProperty("aboutIcon", icon);
    glyph->setFixedSize(16, 16);
    glyph->setPixmap(tintedOcticonPixmap(icon, Theme::color(Theme::Role::FgMuted), 16));
    layout->addWidget(glyph, 0, Qt::AlignVCenter);
    layout->addWidget(text, 1, Qt::AlignVCenter);
    return row;
}

QString nextInitials(const QString &name)
{
    QString initials;
    for (const QString &part : name.split(QRegularExpression(QStringLiteral("[\\s._-]+")),
                                          Qt::SkipEmptyParts)) {
        initials += part.at(0).toUpper();
        if (initials.size() == 2)
            break;
    }
    return initials.isEmpty() ? QStringLiteral("?") : initials;
}

void refreshNextAboutGlyphs(QWidget *sidebar)
{
    for (QLabel *glyph : sidebar->findChildren<QLabel *>()) {
        if (glyph->property("aboutRole").toString() != QLatin1String("glyph"))
            continue;
        glyph->setPixmap(tintedOcticonPixmap(glyph->property("aboutIcon").toString(),
                                             Theme::color(Theme::Role::FgMuted), 16));
    }
}

void updateNextAboutInfo(QWidget *sidebar, const RepositoryRecord &repo,
                         const RepoInfo &info)
{
    if (!sidebar)
        return;
    const QString about = info.about.trimmed().isEmpty() ? repo.description.trimmed()
                                                         : info.about.trimmed();
    if (auto *text = sidebar->findChild<QLabel *>(QStringLiteral("repoAboutText")))
        text->setText(about.isEmpty() ? QStringLiteral("No description provided.") : about);
    if (auto *website = sidebar->findChild<QLabel *>(QStringLiteral("repoAboutWebsite"))) {
        const QString url = info.website.trimmed();
        website->setText(url.isEmpty()
                             ? QString()
                             : QStringLiteral("<a href=\"%1\" style=\"color:%2;"
                                              "text-decoration:none\">%3</a>")
                                   .arg(url.toHtmlEscaped(),
                                        Theme::hex(Theme::Role::AccentFg),
                                        url.toHtmlEscaped()));
        website->setVisible(!url.isEmpty());
    }
    if (auto *topics = sidebar->findChild<QWidget *>(QStringLiteral("repoAboutTopics"))) {
        clearNextLayout(topics->layout());
        for (const QString &topic : info.topics) {
            if (topic.trimmed().isEmpty())
                continue;
            topics->layout()->addWidget(nextAboutLabel(QStringLiteral("topic"),
                                                       topic.trimmed()));
        }
        topics->setVisible(topics->layout()->count() > 0);
    }
    if (auto *stars = sidebar->findChild<QLabel *>(QStringLiteral("repoAboutStars")))
        stars->setText(QStringLiteral("%1 %2").arg(formatCount(info.stars),
                                                   info.stars == 1 ? QStringLiteral("star")
                                                                   : QStringLiteral("stars")));
    if (auto *forks = sidebar->findChild<QLabel *>(QStringLiteral("repoAboutForks"))) {
        const int mirrors = repo.previewOnly ? 0 : qMax(1, info.mirrors);
        forks->setText(QString::fromUtf8("%1 %2 \xC2\xB7 %3 %4")
                           .arg(formatCount(info.forks),
                                info.forks == 1 ? QStringLiteral("fork")
                                                : QStringLiteral("forks"),
                                formatCount(mirrors),
                                mirrors == 1 ? QStringLiteral("mirror")
                                             : QStringLiteral("mirrors")));
    }
    if (auto *people = sidebar->findChild<QWidget *>(QStringLiteral("repoAboutContributors"))) {
        clearNextLayout(people->layout());
        QStringList names = info.contributorAvatars.keys();
        names.sort(Qt::CaseInsensitive);
        for (const QString &name : std::as_const(names)) {
            auto *face = nextAboutLabel(QStringLiteral("avatar"), nextInitials(name));
            face->setFixedSize(28, 28);
            face->setAlignment(Qt::AlignCenter);
            face->setToolTip(name);
            people->layout()->addWidget(face);
        }
        if (QWidget *section = people->parentWidget())
            section->setVisible(!names.isEmpty());
    }
    refreshNextAboutGlyphs(sidebar);
}

void updateNextAboutReleases(QWidget *sidebar, qint64 count)
{
    if (!sidebar)
        return;
    if (auto *pill = sidebar->findChild<QLabel *>(QStringLiteral("repoAboutReleaseCount"))) {
        pill->setText(formatCount(count));
        pill->setVisible(count > 0);
    }
    if (auto *none = sidebar->findChild<QLabel *>(QStringLiteral("repoAboutNoReleases")))
        none->setVisible(count <= 0);
    if (auto *all = sidebar->findChild<QPushButton *>(QStringLiteral("repoAboutReleasesLink"))) {
        all->setText(count == 1 ? QStringLiteral("1 tagged release")
                                : QStringLiteral("%1 tagged releases").arg(formatCount(count)));
        if (QWidget *row = all->parentWidget())
            row->setVisible(count > 0);
    }
}

void updateNextAboutLicense(QWidget *sidebar, const QString &path)
{
    if (!sidebar)
        return;
    auto *license = sidebar->findChild<QPushButton *>(QStringLiteral("repoAboutLicense"));
    if (!license)
        return;
    license->setProperty("licensePath", path);
    license->setText(QFileInfo(path).fileName());
    if (QWidget *row = license->parentWidget())
        row->setVisible(!path.isEmpty());
}

QList<QPair<QString, qint64>> nextLanguageTotals(const QByteArray &sizeOut)
{
    QHash<QString, qint64> bytes;
    for (const QByteArray &record : sizeOut.split('\0')) {
        const int tab = record.indexOf('\t');
        if (tab < 0)
            continue;
        const QList<QByteArray> meta = record.left(tab).simplified().split(' ');
        if (meta.size() < 4 || meta.at(1) != "blob")
            continue;
        const qint64 size = meta.at(3).toLongLong();
        if (size <= 0)
            continue;
        const QString language = languageForFile(QString::fromUtf8(record.mid(tab + 1)));
        if (!language.isEmpty())
            bytes[language] += size;
    }
    QList<QPair<QString, qint64>> totals;
    for (auto it = bytes.cbegin(); it != bytes.cend(); ++it)
        totals.append({it.key(), it.value()});
    std::sort(totals.begin(), totals.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    return totals;
}

void updateNextAboutLanguages(QWidget *sidebar, const QList<QPair<QString, qint64>> &totals)
{
    if (!sidebar)
        return;
    auto *section = sidebar->findChild<QWidget *>(QStringLiteral("repoAboutLanguages"));
    auto *bar = sidebar->findChild<QWidget *>(QStringLiteral("repoAboutLanguageBar"));
    auto *legend = sidebar->findChild<QWidget *>(QStringLiteral("repoAboutLanguageLegend"));
    if (!section || !bar || !legend)
        return;
    qint64 total = 0;
    for (const auto &entry : totals)
        total += entry.second;
    section->setVisible(total > 0);
    clearNextLayout(legend->layout());
    if (total <= 0)
        return;
    QList<QPair<QString, qint64>> shown = totals.mid(0, 5);
    qint64 rest = 0;
    for (int i = 5; i < totals.size(); ++i)
        rest += totals.at(i).second;
    if (rest > 0)
        shown.append({QStringLiteral("Other"), rest});
    QList<QPair<QColor, double>> segments;
    auto *grid = static_cast<QGridLayout *>(legend->layout());
    for (int i = 0; i < shown.size(); ++i) {
        const bool other = shown.at(i).first == QLatin1String("Other") && rest > 0 &&
                           i == shown.size() - 1;
        const QColor color = other ? Theme::color(Theme::Role::FgSubtle)
                                   : QColor(languageColor(shown.at(i).first));
        const double fraction = double(shown.at(i).second) / double(total);
        segments.append({color, fraction});
        auto *entry = new QLabel;
        entry->setProperty("aboutRole", QStringLiteral("legend"));
        entry->setTextFormat(Qt::RichText);
        entry->setText(QString::fromUtf8("<span style=\"color:%1\">\xE2\x97\x8F</span>"
                                         "&nbsp;%2&nbsp;<span style=\"color:%3\">%4%</span>")
                           .arg(color.name(), shown.at(i).first.toHtmlEscaped(),
                                Theme::hex(Theme::Role::FgSubtle),
                                QString::number(fraction * 100.0, 'f', 1)));
        grid->addWidget(entry, i / 2, i % 2);
    }
    static_cast<NextLanguageBar *>(bar)->setSegments(segments);
}

bool nextIsLicenseName(const QString &name)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(LICEN[CS]E|COPYING)([._-].*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(name).hasMatch();
}

QWidget *nextAboutSidebarIn(QWidget *root)
{
    return root ? root->findChild<QWidget *>(QLatin1String(kNextAboutName)) : nullptr;
}

}

void MainWindow::updateIssueAgentUi(const Issue &issue)
{
    m_currentIssueTitle = issue.title;
    const AgentSession *session =
        issue.number > 0 ? latestAgentSessionForIssue(issue.number) : nullptr;
    if (m_issueAgentValue) {
        if (session) {
            const QPixmap portrait = roundedAvatar(
                agentModelIcon(session->provider, session->model).pixmap(20, 20),
                20, 0.5);
            // Anchored, but painted in the sidebar's own muted grey: the block
            // has to keep reading as the section's value, not turn into a wall
            // of blue link the moment an agent is assigned.
            const QString summary =
                QStringLiteral("%1&nbsp;<b>%2</b> session #%3<br>%4 · ~%5 credits · cost ~%6")
                    .arg(inlinePixmapMarkup(portrait, 20),
                         agentProviderName(session->provider))
                    .arg(session->id)
                    .arg(agentStatusText(session->status))
                    .arg(session->estimatedCredits)
                    .arg(agentCostText(session->costUsd));
            const QString href = kAgentLinkScheme + QString::number(session->id);
            m_issueAgentValue->setText(
                QStringLiteral("<a href=\"%1\" style=\"color:%2;"
                               "text-decoration:none\">%3</a>")
                    .arg(href, issueSidebarValueColor(m_issueAgentValue),
                         summary));
            // QLabel raises the pointing hand over the anchor itself, so the
            // hover affordance needs no cursor of our own here.
            m_issueAgentValue->setToolTip(
                QStringLiteral("Open agent session #%1").arg(session->id));
        } else {
            m_issueAgentValue->setText("No agent assigned");
            m_issueAgentValue->setToolTip(QString());
        }
    }
    if (m_issueAgentViewButton)
        m_issueAgentViewButton->setVisible(session);
}

#ifdef FORKMESH_WINDOW_TESTS
int MainWindow::testRepoDetailPage() const
{
    return m_repoDetailStack ? m_repoDetailStack->currentIndex() : -1;
}

QString MainWindow::testIssueAgentSummaryHtml() const
{
    return m_issueAgentValue ? m_issueAgentValue->text() : QString();
}

bool MainWindow::testFollowIssueAgentLink()
{
    if (!m_issueAgentValue)
        return false;
    // Follow the anchor the label really renders, through the same signal a
    // click raises. A hook that called switchToAgentsTab itself would keep
    // passing after the summary stopped carrying a link at all.
    static const QRegularExpression href(QStringLiteral("href=\"([^\"]+)\""));
    const QRegularExpressionMatch anchor = href.match(m_issueAgentValue->text());
    if (!anchor.hasMatch())
        return false;
    return QMetaObject::invokeMethod(m_issueAgentValue, "linkActivated",
                                     Qt::DirectConnection,
                                     Q_ARG(QString, anchor.captured(1)));
}
#endif

static GitCaptureResult readGit(const QString &dir, const QStringList &args)
{
    GitCaptureResult result;
    result.ok = runGitCapture(dir, args, &result.output, &result.error);
    return result;
}

void MainWindow::refreshIssueFilesPanel(const Issue &issue)
{
    if (!m_issueDetailTabs || m_issueFilesTabIndex < 0)
        return;

    auto hideTab = [this] {
        if (m_issueFilesList)
            m_issueFilesList->clear();
        if (m_issueDiffView)
            m_issueDiffView->clear();
        if (m_issueDetailTabs->currentIndex() == m_issueFilesTabIndex)
            m_issueDetailTabs->setCurrentIndex(0);
        m_issueDetailTabs->setTabVisible(m_issueFilesTabIndex, false);
        m_issueDetailTabs->setTabText(m_issueFilesTabIndex,
                                      QStringLiteral("Changes in Git"));
        syncIssueDetailTabBar();
    };

    if (issue.number <= 0) {
        hideTab();
        return;
    }

    if (const AgentSession *s = latestAgentSessionForIssue(issue.number)) {
        const QString dir = sessionWorkdir(s->id);
        if (!dir.isEmpty()) {
            const QString base = sessionBaseRef(s->id);
            QStringList args{QStringLiteral("diff")};
            if (!base.isEmpty())
                args << base;
            m_issueDetailTabs->setTabVisible(m_issueFilesTabIndex, true);
            syncIssueDetailTabBar();
            const int issueNo = issue.number;
            runGitDetached(
                dir, args,
                [this, issueNo, dir, base](bool ok, const QByteArray &patch) {
                    if (!ok || issueNo != m_currentIssueNumber)
                        return;
                    runOffThread<bool>(
                        [patch, dir, base] {
                            prefetchDiffImageBlobs(QString::fromUtf8(patch),
                                                   dir, base, QString());
                            return true;
                        },
                        [this, issueNo, patch, dir, base](bool) {
                            if (issueNo == m_currentIssueNumber)
                                renderIssueDiff(issueNo, patch, dir, base);
                        });
                });
            return;
        }
    }

    const QList<int> pulls = pullsLinkedToIssue(issue.number);
    for (auto it = pulls.crbegin(); it != pulls.crend(); ++it) {
        for (const PullRequest &pr : m_currentPulls) {
            if (pr.number == *it && !pr.patch.isEmpty()) {
                m_issueDetailTabs->setTabVisible(m_issueFilesTabIndex, true);
                syncIssueDetailTabBar();
                renderIssueDiff(issue.number, pr.patch.toUtf8(), QString(), QString());
                return;
            }
        }
    }

    hideTab();
}

void MainWindow::renderIssueDiff(int issueNumber, const QByteArray &patch,
                                 const QString &dir, const QString &base)
{
    if (!m_issueDiffView || issueNumber != m_currentIssueNumber)
        return;
    QList<DiffFileEntry> files;
    const QString html =
        renderDiffHtml(QString::fromUtf8(patch), files, dir, base, QString(),
                       QString(), QHash<QString, QString>(), QSet<QString>());
    setDiffHtml(m_issueDiffView,
        html.isEmpty()
            ? QStringLiteral("<p style='color:#8b949e'>No changes yet.</p>")
            : html);

    if (m_issueFilesList) {
        QSignalBlocker block(m_issueFilesList);
        m_issueFilesList->clear();
        for (const DiffFileEntry &f : files) {
            const QString name = f.path.section(QLatin1Char('/'), -1);
            auto *item = new QListWidgetItem(
                QString::fromUtf8("%1   +%2 \xE2\x88\x92%3")
                    .arg(name, QString::number(f.adds), QString::number(f.dels)));
            QColor tint("#d29922");
            QString icon = "file-diff";
            if (f.status == QLatin1String("added")) { icon = "diff"; tint = QColor("#3fb950"); }
            else if (f.status == QLatin1String("deleted")) { icon = "trash"; tint = QColor("#f85149"); }
            item->setIcon(themedOcticon(icon, tint, 14));
            const QString abs = dir.isEmpty() ? f.path : QDir(dir).filePath(f.path);
            item->setData(Qt::UserRole, abs);          // open on activate
            item->setData(Qt::UserRole + 1, f.anchor); // scroll diff on select
            item->setToolTip(QString::fromUtf8("%1 \xC2\xB7 %2").arg(f.status, f.path));
            m_issueFilesList->addItem(item);
        }
        fitFileListToWidestEntry(m_issueFilesList);
    }
    if (!m_issueDiffNav && m_issueFilesList)
        m_issueDiffNav = new DiffFileNavigator(m_issueDiffView, m_issueFilesList,
                                               Qt::UserRole + 1, this);
    if (m_issueDiffNav)
        m_issueDiffNav->rebuild(files, m_diffFontPt);

    const int n = files.size();
    if (m_issueDetailTabs && m_issueFilesTabIndex >= 0)
        m_issueDetailTabs->setTabText(
            m_issueFilesTabIndex,
            n > 0 ? QStringLiteral("Changes in Git (%1)").arg(n)
                  : QStringLiteral("Changes in Git"));
    if (m_issueFilesChangedSummary)
        m_issueFilesChangedSummary->setText(
            QStringLiteral("%1 file%2 changed").arg(n).arg(n == 1 ? "" : "s"));
}

namespace {
constexpr int kIssueFilesColumn = 15;
constexpr int kAttachmentIconSize = 14;
constexpr int kAttachmentIconGap = 2;
constexpr int kMaxIssueListAttachmentIcons = 8;

QStringList visibleIssueAttachments(const Issue &issue)
{
    QHash<QString, IssueEvent> edits;
    QSet<QString> deleted;
    for (const IssueEvent &ev : issue.events) {
        if (ev.type == QLatin1String("edit") && !ev.target.isEmpty())
            edits.insert(ev.target, ev);
        else if (ev.type == QLatin1String("delete") && !ev.target.isEmpty() &&
                 ev.target != QLatin1String("self"))
            deleted.insert(ev.target);
    }

    QStringList files;
    QSet<QString> seen;
    auto append = [&](const QString &rel) {
        const QString clean = rel.trimmed();
        if (clean.isEmpty() || seen.contains(clean))
            return;
        seen.insert(clean);
        files << clean;
    };

    for (const IssueEvent &ev : issue.events) {
        if (ev.type != QLatin1String("open") && ev.type != QLatin1String("comment"))
            continue;
        if (ev.type == QLatin1String("comment") && deleted.contains(ev.id))
            continue;
        const IssueEvent shown = edits.value(ev.id, ev);
        for (const QString &rel : shown.attachments)
            append(rel);
    }
    return files;
}

QString issueAttachmentToolTip(const QStringList &attachments)
{
    if (attachments.isEmpty())
        return QString();
    QStringList lines;
    lines << QStringLiteral("Attached files:");
    const int shown = qMin(attachments.size(), 20);
    for (int i = 0; i < shown; ++i)
        lines << QStringLiteral("- %1").arg(attachments.at(i));
    if (attachments.size() > shown)
        lines << QStringLiteral("- ... and %1 more").arg(attachments.size() - shown);
    return lines.join(QLatin1Char('\n'));
}

QWidget *makeIssueAttachmentStrip(const QStringList &attachments, QWidget *parent)
{
    auto *strip = new QWidget(parent);
    strip->setAttribute(Qt::WA_TransparentForMouseEvents);
    strip->setToolTip(issueAttachmentToolTip(attachments));
    strip->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(strip);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(kAttachmentIconGap);
    layout->setAlignment(Qt::AlignCenter);

    const QPixmap icon =
        tintedOcticonPixmap(QStringLiteral("file"),
                            Theme::nextOr(Theme::Role::FgMuted, QColor("#58a6ff")),
                            kAttachmentIconSize);
    const int shown = qMin(attachments.size(), kMaxIssueListAttachmentIcons);
    for (int i = 0; i < shown; ++i) {
        auto *label = new QLabel(strip);
        label->setFixedSize(kAttachmentIconSize, kAttachmentIconSize);
        label->setPixmap(icon);
        label->setToolTip(attachments.at(i));
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(label);
    }
    if (attachments.size() > shown) {
        auto *more =
            new QLabel(QStringLiteral("+%1").arg(attachments.size() - shown), strip);
        more->setStyleSheet(Theme::remapSheet(QStringLiteral(
            "color:#8b949e; background:transparent; font-size:11px;")));
        more->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(more);
    }
    return strip;
}

int issueAttachmentCellWidth(int count)
{
    if (count <= 0)
        return 24;
    const int shown = qMin(count, kMaxIssueListAttachmentIcons);
    int width = 8 + shown * kAttachmentIconSize +
                qMax(0, shown - 1) * kAttachmentIconGap;
    if (count > shown)
        width += 28;
    return width;
}
} // namespace

QString MainWindow::issueAttachmentPreviewPath(
    const Issue &issue, const QStringList &attachments) const
{
    static const QStringList kPreviewable{
        QStringLiteral("png"),  QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("webp"), QStringLiteral("gif"), QStringLiteral("bmp")};
    const int repoIndex = issuesRepoIndex();
    if (attachments.isEmpty() || repoIndex < 0)
        return QString();
    const QString workTree =
        writableRecordFor(m_repositories.at(repoIndex)).localPath;
    if (workTree.isEmpty())
        return QString();
    const QString dir = IssueStore::issueDirPath(workTree, issue.number);
    for (const QString &name : attachments) {
        if (!kPreviewable.contains(QFileInfo(name).suffix().toLower()))
            continue;
        const QString path = QDir(dir).filePath(name);
        if (QFileInfo::exists(path))
            return path;
    }
    return QString();
}

void MainWindow::populateIssueFilesCell(int row, const Issue &issue)
{
    if (!m_issueTable)
        return;

    const QStringList attachments = visibleIssueAttachments(issue);
    const int count = attachments.size();
    auto *item = new SortTableWidgetItem(count > 0 ? QString::number(count)
                                                   : QString());
    item->setTextAlignment(Qt::AlignCenter);
    item->setData(kTableSortRole, count);
    item->setToolTip(issueAttachmentToolTip(attachments));
    // The summary delegate paints the first image attachment as the row's
    // preview tile; resolving it here reuses the scan this cell already did.
    item->setData(Qt::UserRole, issueAttachmentPreviewPath(issue, attachments));
    item->setSizeHint(QSize(issueAttachmentCellWidth(count), 20));
    m_issueTable->setItem(row, kIssueFilesColumn, item);
    if (count > 0)
        m_issueTable->setCellWidget(
            row, kIssueFilesColumn,
            makeIssueAttachmentStrip(attachments, m_issueTable));
}

QWidget *MainWindow::buildRepoFilesPanel()
{
    m_filesStack = new CurrentPageStack;
    m_filesStack->addWidget(buildRepoOverviewPage());

    m_filesModeOverviewButton =
        new VerticalIconButton("Overview", VerticalIconButton::Tab);
    m_filesModeOverviewButton->setObjectName("repoTab");
    m_filesModeOverviewButton->setCheckable(true);
    m_filesModeOverviewButton->setChecked(true);
    m_filesModeOverviewButton->setCursor(Qt::PointingHandCursor);
    m_filesModeOverviewButton->setToolTip(
        "Show the repository overview (latest commit, file list and README)");
    setOcticon(m_filesModeOverviewButton, "code", 16);
    connect(m_filesModeOverviewButton, &QPushButton::clicked, this, [this] {
        if (m_repoDetailTabs && m_repoDetailTabs->button(0))
            m_repoDetailTabs->button(0)->setChecked(true);
        if (m_repoDetailStack)
            m_repoDetailStack->setCurrentIndex(0);
        showRepoOverview();
    });

    auto *repoSizeChart = new ResourceSparkline(QStringLiteral("SIZE"), nullptr,
                                                kResourceSparklineSide, 30);
    auto *repoLinesChart = new ResourceSparkline(QStringLiteral("LOC"), nullptr,
                                                 kResourceSparklineSide, 30);
    auto *repoFilesChart = new ResourceSparkline(
        QStringLiteral("FILES"), nullptr, kResourceSparklineSide, 30);
    repoSizeChart->setObjectName(QStringLiteral("repoSizeChart"));
    repoLinesChart->setObjectName(QStringLiteral("repoLinesChart"));
    repoFilesChart->setObjectName(QStringLiteral("repoFilesChart"));
    m_repoSizeChart = repoSizeChart;
    m_repoLinesChart = repoLinesChart;
    m_repoFilesChart = repoFilesChart;
    for (ResourceSparkline *chart : {repoSizeChart, repoLinesChart, repoFilesChart})
        chart->setLoading(true);
    m_repoRatchetButton = new RatchetToggleButton;
    m_repoRatchetButton->setObjectName(QStringLiteral("repoRatchetButton"));
    m_repoRatchetButton->setToolTip(
        QStringLiteral("Hold the repository at or below the tracked size it started "
                       "today at. The ceiling re-bases each day."));
    m_repoRatchetButton->setAccessibleName(QStringLiteral("Ratchet mode"));
    connect(m_repoRatchetButton, &QToolButton::toggled, this,
            &MainWindow::toggleRepositoryRatchet);

    const bool next = Theme::isNext();
    m_repoFilesModeBar = new QWidget;
    auto *modeRow = new QHBoxLayout(m_repoFilesModeBar);
    // The page carries the left inset for the whole Code panel (below), so the
    // mode row only owns the gap on its right.
    modeRow->setContentsMargins(0, 0, 16, 0);
    modeRow->setSpacing(next ? 8 : 10);
    m_repoCodeBarLayout = new QHBoxLayout;
    m_repoCodeBarLayout->setContentsMargins(0, 0, 0, 0);
    m_repoCodeBarLayout->setSpacing(next ? 0 : 10);
    m_repoCodeBarButtons = {
        {QStringLiteral("repoCode:overview"), m_filesModeOverviewButton},
        {QStringLiteral("repoCode:branches"), m_branchesButton},
        {QStringLiteral("repoCode:worktrees"), m_worktreesButton},
        {QStringLiteral("repoCode:remotes"), m_remotesButton},
        {QStringLiteral("repoCode:tags"), m_tagsButton},
    };
    if (m_repoReleasesTab)
        m_repoCodeBarButtons.append(
            {QStringLiteral("repoCode:releases"), m_repoReleasesTab});
    for (const auto &entry : std::as_const(m_repoCodeBarButtons)) {
        QWidget *button = entry.second.data();
        m_repoCodeBarLayout->addWidget(button);
        if (button == m_repoReleasesTab)
            continue;
        connect(static_cast<QPushButton *>(button), &QPushButton::clicked, this,
                [this, button] {
                    if (isInRail(button))
                        showSection(0);
                });
    }
    NextRepoButtonStyler *styler = NextRepoButtonStyler::forWindow(this);
    for (const auto &entry : std::as_const(m_repoCodeBarButtons))
        styler->adopt(entry.second.data(), NextRepoTool);
    if (next) {
        auto *branchPicker = new QPushButton;
        branchPicker->setObjectName(QStringLiteral("repoBranchPicker"));
        branchPicker->setCursor(Qt::PointingHandCursor);
        branchPicker->setToolTip(QStringLiteral("Switch branch"));
        branchPicker->setProperty("nextIconRole", int(Theme::Role::FgMuted));
        setOcticon(branchPicker, QStringLiteral("git-branch"), 14);
        auto *branchChevron = new QLabel(branchPicker);
        branchChevron->setObjectName(QStringLiteral("repoBranchPickerChevron"));
        branchChevron->setAttribute(Qt::WA_TransparentForMouseEvents);
        setLabelOcticon(branchChevron, QStringLiteral("chevron-down"), 12);
        auto *branchPickerRow = new QHBoxLayout(branchPicker);
        branchPickerRow->setContentsMargins(0, 0, 9, 0);
        branchPickerRow->addStretch(1);
        branchPickerRow->addWidget(branchChevron, 0, Qt::AlignVCenter);
        branchPicker->setText(currentRef());
        auto *branchFollower = new NextTextFollower(m_branchButton, branchPicker);
        connect(branchPicker, &QPushButton::clicked, this,
                [this, branchPicker, branchFollower] {
                    branchFollower->sync();
                    QMenu *menu = m_branchButton ? m_branchButton->menu() : nullptr;
                    if (menu)
                        menu->popup(branchPicker->mapToGlobal(
                            QPoint(0, branchPicker->height() + 4)));
                });

        auto *cloneButton = new QPushButton(QStringLiteral("Code"));
        cloneButton->setObjectName(QStringLiteral("repoCloneButton"));
        cloneButton->setCursor(Qt::PointingHandCursor);
        cloneButton->setToolTip(m_sourceButton ? m_sourceButton->toolTip() : QString());
        cloneButton->setProperty("nextIconRole", int(Theme::Role::OnAccent));
        setOcticon(cloneButton, QStringLiteral("code"), 14);
        auto *cloneChevron = new QLabel(cloneButton);
        cloneChevron->setAttribute(Qt::WA_TransparentForMouseEvents);
        cloneChevron->setPixmap(tintedOcticonPixmap(
            QStringLiteral("chevron-down"), Theme::color(Theme::Role::OnAccent), 12));
        cloneChevron->setFixedSize(12, 12);
        auto *cloneRow = new QHBoxLayout(cloneButton);
        cloneRow->setContentsMargins(0, 0, 9, 0);
        cloneRow->addStretch(1);
        cloneRow->addWidget(cloneChevron, 0, Qt::AlignVCenter);
        connect(cloneButton, &QPushButton::clicked, this, [this, cloneButton] {
            if (!m_sourceMenu || (m_sourceButton && !m_sourceButton->isEnabled()))
                return;
            updateRepoActionMenus();
            const QSize menuSize = m_sourceMenu->sizeHint();
            m_sourceMenu->popup(cloneButton->mapToGlobal(
                QPoint(cloneButton->width() - menuSize.width(),
                       cloneButton->height() + 4)));
        });

        m_fileSearch->setObjectName(QStringLiteral("repoFileSearch"));
        m_fileSearch->setMinimumWidth(200);
        m_fileSearch->setMaximumWidth(240);
        m_fileSearch->addAction(
            QIcon(tintedOcticonPixmap(QStringLiteral("search"),
                                      Theme::color(Theme::Role::FgSubtle), 14)),
            QLineEdit::LeadingPosition);

        modeRow->addWidget(branchPicker);
        modeRow->addLayout(m_repoCodeBarLayout);
        modeRow->addStretch(1);
        modeRow->addWidget(repoSizeChart);
        modeRow->addWidget(repoLinesChart);
        modeRow->addWidget(repoFilesChart);
        modeRow->addWidget(m_repoRatchetButton);
        modeRow->addSpacing(4);
        modeRow->addWidget(m_fileSearch);
        modeRow->addWidget(cloneButton);
    } else {
        modeRow->addLayout(m_repoCodeBarLayout);
        modeRow->addSpacing(10);
        modeRow->addWidget(m_fileSearch);
        modeRow->addStretch(1);
        modeRow->addWidget(repoSizeChart);
        modeRow->addWidget(repoLinesChart);
        modeRow->addWidget(repoFilesChart);
        modeRow->addWidget(m_repoRatchetButton);
    }

    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    // One inset for the whole page: the mode row, the commit line, the file
    // list and the README all start at the same left edge instead of butting
    // up against the activity rail.
    layout->setContentsMargins(kRepoPageLeftInset + (next ? 2 : 0), next ? 12 : 0, 0, 0);
    layout->setSpacing(next ? 12 : 8);
    layout->addWidget(m_repoFilesModeBar);
    layout->addWidget(m_filesStack, 1);
    if (QWidget *about = nextAboutSidebarIn(panel);
        about && m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        updateNextAboutInfo(about, m_repositories.at(m_repoDetailIndex), m_repoInfo);
        if (auto *tags = dynamic_cast<VerticalIconButton *>(m_tagsButton))
            updateNextAboutReleases(about, tags->badgeCount());
    }
    refreshRepositoryStats();
    return panel;
}

QWidget *MainWindow::buildRepoOverviewPage()
{
    auto *page = new QWidget;

    auto *commitCard = new QWidget;
    commitCard->setObjectName("commitBar");
    // The author portrait leads the line as a real pixmap label rather than an
    // inline <img>: the layout can then centre it against the text, and it
    // stays crisp at whatever device pixel ratio roundedAvatar rendered for.
    m_commitBarAvatar = new QLabel;
    m_commitBarAvatar->setObjectName("commitBarAvatar");
    m_commitBarAvatar->setFixedSize(kCommitBarAvatarPx, kCommitBarAvatarPx);
    m_commitBarAvatar->hide(); // shown once a commit is loaded
    const bool next = Theme::isNext();
    m_commitBar = new QLabel;
    m_commitBar->setObjectName("commitBarText");
    m_commitBar->setTextFormat(Qt::RichText);
    m_commitBar->setWordWrap(!next);
    auto *commitRow = new QHBoxLayout(commitCard);
    commitRow->setContentsMargins(next ? QMargins(16, 0, 16, 0) : QMargins(10, 7, 12, 7));
    commitRow->setSpacing(next ? 10 : 8);
    commitRow->addWidget(m_commitBarAvatar, 0, Qt::AlignVCenter);
    commitRow->addWidget(m_commitBar, 1);
    if (next) {
        m_commitBar->setMinimumWidth(0);
        m_commitBar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *commitMeta = new QLabel;
        commitMeta->setObjectName(QStringLiteral("commitBarMeta"));
        commitMeta->setTextFormat(Qt::RichText);
        commitMeta->setTextInteractionFlags(Qt::TextSelectableByMouse);
        commitRow->addWidget(commitMeta, 0, Qt::AlignVCenter);
    }

    m_overviewCrumb = new QLabel;
    m_overviewCrumb->setObjectName("statusLine");
    m_overviewCrumb->setTextFormat(Qt::RichText);
    // The scroll body is resized to fill the viewport. Keep the breadcrumb a
    // single compact row so unused viewport height cannot push the file list
    // down into the middle of the page.
    m_overviewCrumb->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_overviewCrumb->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_overviewCrumb->setTextInteractionFlags(Qt::TextBrowserInteraction);
    connect(m_overviewCrumb, &QLabel::linkActivated, this,
            [this](const QString &href) {
                loadRepoOverview(href == "/" ? QString() : href);
            });

    m_overviewList = new QTreeWidget;
    m_overviewList->setObjectName("overviewList");
    if (next) {
        m_overviewList->setProperty("nextCodeTable", true);
        m_overviewList->setHeader(new NextOverviewCommitHeader(commitCard, m_overviewList));
    }
    m_overviewList->setColumnCount(2);
    m_overviewList->setHeaderLabels({"Name and updated time", "Last commit"});
    m_overviewList->setRootIsDecorated(false);
    m_overviewList->setUniformRowHeights(true);
    m_overviewList->setSortingEnabled(false); // we sort the cached rows ourselves
    m_overviewList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_overviewList->setAllColumnsShowFocus(true);
    if (!next)
        m_overviewList->header()->hide();
    m_overviewList->header()->setStretchLastSection(false);
    m_overviewList->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_overviewList->setColumnWidth(0, next ? kNextCodeNameWidth : 360);
    m_overviewList->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_overviewList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_overviewList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_overviewList->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    enableHoverRowHighlight(m_overviewList);
    if (next) {
        QAbstractItemDelegate *stock = m_overviewList->itemDelegate();
        m_overviewList->setItemDelegate(new NextOverviewRowDelegate(m_overviewList));
        if (stock)
            stock->deleteLater();
    }
    connect(m_overviewList, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) {
                const QString path = item->data(0, Qt::UserRole).toString();
                const int kind = item->data(0, Qt::UserRole + 1).toInt();
                if (kind == 1)
                    loadRepoOverview(path); // navigate into the directory (or up)
                else if (!path.isEmpty())
                    openRepoFile(path); // open the file (in the Files section)
            });

    m_readmeView = new QTextBrowser;
    m_readmeView->setObjectName("readmeView");
    m_readmeView->setOpenLinks(false);
    connect(m_readmeView, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        openRepoMarkdownLink(m_readmeView,
                             m_readmeView->property("markdownPath").toString(), url);
    });
    m_readmeView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_readmeView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_readmeView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    m_branchesButton =
        new VerticalIconButton("Branches", VerticalIconButton::Tab);
    m_branchesButton->setCursor(Qt::PointingHandCursor);
    m_branchesButton->setToolTip(
        "Open the Branches panel to manage branches \xE2\x80\x94 click one to "
        "open it in the Git view, where its history and its diff against the "
        "base branch show side by side");
    setOcticon(m_branchesButton, "git-branch", 16);
    connect(m_branchesButton, &QPushButton::clicked, this, [this] {
        showOverviewBranches();
        loadBranchesPanel();
    });
    m_worktreesButton =
        new VerticalIconButton("Worktrees", VerticalIconButton::Tab);
    m_worktreesButton->setCursor(Qt::PointingHandCursor);
    m_worktreesButton->setToolTip(
        "Open the Worktrees panel to view agent checkouts and their changes");
    setOcticon(m_worktreesButton, "worktree", 16);
    connect(m_worktreesButton, &QPushButton::clicked, this, [this] {
        showOverviewWorktrees();
        loadWorktreesPanel();
    });
    m_remotesButton =
        new VerticalIconButton("Remotes", VerticalIconButton::Tab);
    m_remotesButton->setCursor(Qt::PointingHandCursor);
    m_remotesButton->setToolTip(
        "List this repository's git remotes; pick one to copy its URL");
    setOcticon(m_remotesButton, "server", 16);
    m_tagsButton = new VerticalIconButton("Tags", VerticalIconButton::Tab);
    m_tagsButton->setCursor(Qt::PointingHandCursor);
    m_tagsButton->setToolTip("Open the Releases panel to create and manage tagged releases");
    setOcticon(m_tagsButton, "tag", 16);
    connect(m_tagsButton, &QPushButton::clicked, this, [this] {
        if (m_releasesTabIndex >= 0 && m_repoDetailTabs &&
            m_repoDetailTabs->button(m_releasesTabIndex)) {
            m_repoDetailTabs->button(m_releasesTabIndex)->setChecked(true);
            m_repoDetailStack->setCurrentIndex(m_releasesTabIndex);
            loadReleasesPanel();
        }
    });
    m_codeMapButton = new QPushButton("Code map");
    m_codeMapButton->setCursor(Qt::PointingHandCursor);
    m_codeMapButton->setToolTip("See the whole codebase as a zoomable treemap, in 2D or 3D");
    setOcticon(m_codeMapButton, "graph", 16);
    connect(m_codeMapButton, &QPushButton::clicked, this, [this] { openRepoCodeMap(); });
    m_fileSearch = new QLineEdit;
    m_fileSearch->setPlaceholderText("Go to file\xE2\x80\xA6");
    m_fileSearch->setMaximumWidth(220);
    m_fileSearch->setClearButtonEnabled(true);
    m_fileCompleter = new QCompleter(this);
    m_fileCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_fileCompleter->setFilterMode(Qt::MatchContains);
    m_fileCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_fileSearch->setCompleter(m_fileCompleter);
    connect(m_fileCompleter, QOverload<const QString &>::of(&QCompleter::activated),
            this, [this](const QString &path) {
                if (!path.isEmpty())
                    openRepoFile(path);
                m_fileSearch->clear();
            });
    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(8);
    toolbar->addWidget(m_branchesButton);
    toolbar->addWidget(m_worktreesButton);
    toolbar->addWidget(m_remotesButton);
    toolbar->addWidget(m_tagsButton);
    if (m_repoReleasesTab)
        toolbar->addWidget(m_repoReleasesTab);
    toolbar->addWidget(m_codeMapButton);
    toolbar->addWidget(m_fileSearch, 1);

    auto *filesBody = new QWidget;
    QWidget *filesMain = filesBody;
    if (next) {
        filesMain = new QWidget;
        filesMain->setObjectName(QStringLiteral("repoCodeMain"));
    }
    auto *filesBodyLayout = new QVBoxLayout(filesMain);
    // Line the crumb, the file rows and the README up with the portrait in the
    // commit card above (which sits a card padding in from the same edge).
    filesBodyLayout->setContentsMargins(
        Theme::isNext() ? QMargins(0, 0, 16, 0) : QMargins(6, 0, 0, 0));
    filesBodyLayout->setSpacing(next ? 12 : 8);
    filesBodyLayout->addWidget(m_overviewCrumb);
    filesBodyLayout->addWidget(m_overviewList);
    auto *readmeCard = new QFrame;
    readmeCard->setObjectName(QStringLiteral("readmeCard"));
    readmeCard->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto *readmeCardLayout = new QVBoxLayout(readmeCard);
    readmeCardLayout->setContentsMargins(0, 0, 0, 0);
    readmeCardLayout->setSpacing(0);
    auto *readmeCardHeader = new QFrame;
    readmeCardHeader->setObjectName(QStringLiteral("readmeCardHeader"));
    readmeCardHeader->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto *readmeHeaderLayout = new QHBoxLayout(readmeCardHeader);
    readmeHeaderLayout->setContentsMargins(16, 10, 16, 10);
    readmeHeaderLayout->setSpacing(8);
    auto *readmeCardIcon = new QLabel;
    setLabelOcticon(readmeCardIcon, QStringLiteral("book"), 14);
    readmeHeaderLayout->addWidget(readmeCardIcon, 0, Qt::AlignVCenter);
    auto *readmeCardTitle = new QLabel(QStringLiteral("README"));
    readmeCardTitle->setObjectName(QStringLiteral("readmeCardTitle"));
    readmeCardTitle->setTextFormat(Qt::PlainText);
    readmeHeaderLayout->addWidget(readmeCardTitle, 1, Qt::AlignVCenter);
    readmeCardHeader->setVisible(false);
    readmeCardLayout->addWidget(readmeCardHeader);
    readmeCardLayout->addWidget(m_readmeView);
    filesBodyLayout->addWidget(readmeCard);
    // QScrollArea makes filesBody at least as tall as its viewport. Route that
    // extra height to the bottom instead of allowing a content widget to grow
    // and create a gap before the file rows.
    filesBodyLayout->addStretch(1);
    if (next) {
        auto *sidebar = new QWidget;
        sidebar->setObjectName(QLatin1String(kNextAboutName));
        sidebar->setFixedWidth(kNextAboutWidth);
        auto *sidebarLayout = new QVBoxLayout(sidebar);
        sidebarLayout->setContentsMargins(0, 0, 0, 0);
        sidebarLayout->setSpacing(20);

        auto *aboutSection = new QWidget;
        auto *aboutLayout = new QVBoxLayout(aboutSection);
        aboutLayout->setContentsMargins(0, 0, 0, 0);
        aboutLayout->setSpacing(10);
        auto *aboutHeading = new QHBoxLayout;
        aboutHeading->setContentsMargins(0, 0, 0, 0);
        aboutHeading->addWidget(nextAboutLabel(QStringLiteral("heading"),
                                               QStringLiteral("About")));
        aboutHeading->addStretch(1);
        auto *aboutEdit = new QPushButton;
        aboutEdit->setObjectName(QStringLiteral("repoAboutEdit"));
        aboutEdit->setProperty("aboutRole", QStringLiteral("edit"));
        aboutEdit->setCursor(Qt::PointingHandCursor);
        aboutEdit->setToolTip(QStringLiteral("Edit the description, website and topics"));
        aboutEdit->setProperty("nextIconRole", int(Theme::Role::FgMuted));
        setOcticon(aboutEdit, QStringLiteral("gear"), 14);
        aboutEdit->setFixedSize(24, 24);
        connect(aboutEdit, &QPushButton::clicked, this, &MainWindow::editRepoAbout);
        aboutHeading->addWidget(aboutEdit);
        aboutLayout->addLayout(aboutHeading);
        auto *aboutText = nextAboutLabel(QStringLiteral("body"));
        aboutText->setObjectName(QStringLiteral("repoAboutText"));
        aboutText->setWordWrap(true);
        aboutText->setTextInteractionFlags(Qt::TextSelectableByMouse);
        aboutLayout->addWidget(aboutText);
        auto *website = new QLabel;
        website->setObjectName(QStringLiteral("repoAboutWebsite"));
        website->setProperty("aboutRole", QStringLiteral("body"));
        website->setTextFormat(Qt::RichText);
        website->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        website->hide();
        connect(website, &QLabel::linkActivated, this, [](const QString &href) {
            const QUrl url(href);
            const QString scheme = url.scheme().toLower();
            if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
                QDesktopServices::openUrl(url);
        });
        aboutLayout->addWidget(website);
        auto *topics = new QWidget;
        topics->setObjectName(QStringLiteral("repoAboutTopics"));
        new FlowLayout(topics, 0, 4, 4);
        topics->hide();
        aboutLayout->addWidget(topics);
        auto *licenseLink = new QPushButton;
        licenseLink->setObjectName(QStringLiteral("repoAboutLicense"));
        licenseLink->setProperty("aboutRole", QStringLiteral("link"));
        licenseLink->setCursor(Qt::PointingHandCursor);
        licenseLink->setToolTip(QStringLiteral("Open the license file"));
        connect(licenseLink, &QPushButton::clicked, this, [this, licenseLink] {
            const QString path = licenseLink->property("licensePath").toString();
            if (!path.isEmpty())
                openRepoFile(path);
        });
        QWidget *licenseRow = nextAboutFact(QStringLiteral("book"), licenseLink);
        licenseRow->hide();
        auto *stars = nextAboutLabel(QStringLiteral("fact"));
        stars->setObjectName(QStringLiteral("repoAboutStars"));
        auto *forks = nextAboutLabel(QStringLiteral("fact"));
        forks->setObjectName(QStringLiteral("repoAboutForks"));
        auto *facts = new QVBoxLayout;
        facts->setContentsMargins(0, 4, 0, 0);
        facts->setSpacing(8);
        facts->addWidget(licenseRow);
        facts->addWidget(nextAboutFact(QStringLiteral("star"), stars));
        facts->addWidget(nextAboutFact(QStringLiteral("repo-forked"), forks));
        aboutLayout->addLayout(facts);
        sidebarLayout->addWidget(aboutSection);

        auto *releasesSection = new QWidget;
        auto *releasesLayout = new QVBoxLayout(releasesSection);
        releasesLayout->setContentsMargins(0, 0, 0, 0);
        releasesLayout->setSpacing(10);
        releasesLayout->addWidget(nextAboutRule());
        auto *releasesHeading = new QHBoxLayout;
        releasesHeading->setContentsMargins(0, 8, 0, 0);
        releasesHeading->setSpacing(8);
        releasesHeading->addWidget(nextAboutLabel(QStringLiteral("heading"),
                                                  QStringLiteral("Releases")));
        auto *releaseCount = nextAboutLabel(QStringLiteral("count"));
        releaseCount->setObjectName(QStringLiteral("repoAboutReleaseCount"));
        releaseCount->hide();
        releasesHeading->addWidget(releaseCount);
        releasesHeading->addStretch(1);
        releasesLayout->addLayout(releasesHeading);
        auto *noReleases = nextAboutLabel(QStringLiteral("body"),
                                          QStringLiteral("No releases published"));
        noReleases->setObjectName(QStringLiteral("repoAboutNoReleases"));
        releasesLayout->addWidget(noReleases);
        auto *releasesLink = new QPushButton;
        releasesLink->setObjectName(QStringLiteral("repoAboutReleasesLink"));
        releasesLink->setProperty("aboutRole", QStringLiteral("link"));
        releasesLink->setCursor(Qt::PointingHandCursor);
        releasesLink->setToolTip(QStringLiteral("Open the Releases panel"));
        connect(releasesLink, &QPushButton::clicked, this, [this] {
            if (m_tagsButton)
                m_tagsButton->click();
        });
        QWidget *releasesRow = nextAboutFact(QStringLiteral("tag"), releasesLink);
        releasesRow->hide();
        releasesLayout->addWidget(releasesRow);
        sidebarLayout->addWidget(releasesSection);

        auto *languagesSection = new QWidget;
        languagesSection->setObjectName(QStringLiteral("repoAboutLanguages"));
        auto *languagesLayout = new QVBoxLayout(languagesSection);
        languagesLayout->setContentsMargins(0, 0, 0, 0);
        languagesLayout->setSpacing(10);
        languagesLayout->addWidget(nextAboutRule());
        auto *languagesHeading = nextAboutLabel(QStringLiteral("heading"),
                                                QStringLiteral("Languages"));
        languagesHeading->setContentsMargins(0, 8, 0, 2);
        languagesLayout->addWidget(languagesHeading);
        languagesLayout->addWidget(new NextLanguageBar);
        auto *legend = new QWidget;
        legend->setObjectName(QStringLiteral("repoAboutLanguageLegend"));
        auto *legendGrid = new QGridLayout(legend);
        legendGrid->setContentsMargins(0, 2, 0, 0);
        legendGrid->setHorizontalSpacing(12);
        legendGrid->setVerticalSpacing(6);
        languagesLayout->addWidget(legend);
        languagesSection->hide();
        sidebarLayout->addWidget(languagesSection);

        auto *peopleSection = new QWidget;
        auto *peopleLayout = new QVBoxLayout(peopleSection);
        peopleLayout->setContentsMargins(0, 0, 0, 0);
        peopleLayout->setSpacing(10);
        peopleLayout->addWidget(nextAboutRule());
        auto *peopleHeading = nextAboutLabel(QStringLiteral("heading"),
                                             QStringLiteral("Contributors"));
        peopleHeading->setContentsMargins(0, 8, 0, 2);
        peopleLayout->addWidget(peopleHeading);
        auto *people = new QWidget(peopleSection);
        people->setObjectName(QStringLiteral("repoAboutContributors"));
        new FlowLayout(people, 0, 6, 6);
        peopleLayout->addWidget(people);
        peopleSection->hide();
        sidebarLayout->addWidget(peopleSection);
        sidebarLayout->addStretch(1);

        auto *bodyRow = new QHBoxLayout(filesBody);
        bodyRow->setContentsMargins(0, 0, 16, 24);
        bodyRow->setSpacing(16);
        bodyRow->addWidget(filesMain, 1);
        bodyRow->addWidget(sidebar, 0, Qt::AlignTop);
        new NextWidthWatcher(filesBody, [sidebar](int width) {
            const bool wide = width >= kNextAboutMinBodyWidth;
            if (sidebar->isVisibleTo(sidebar->parentWidget()) != wide)
                sidebar->setVisible(wide);
        });
    }

    auto *overviewScroll = new QScrollArea;
    overviewScroll->setObjectName("overviewScroll");
    overviewScroll->setWidgetResizable(true);
    overviewScroll->setFrameShape(QFrame::NoFrame);
    overviewScroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    overviewScroll->setWidget(filesBody);

    m_overviewBodyStack = new CurrentPageStack;
    m_overviewBodyStack->addWidget(overviewScroll); // 0 files + README
    // Source control, branches and worktrees are independent destinations.
    // Constructing all three while opening Code made the first repository
    // click pay for pages the user may never visit. Keep stable stack indices
    // and replace each placeholder on its first real navigation.
    for (int i = 1; i <= 3; ++i) {
        auto *placeholder = new QWidget;
        placeholder->setProperty("forkmeshDeferredOverviewBody", true);
        auto *loading = new QLabel(QStringLiteral("Loading workspace\u2026"),
                                   placeholder);
        loading->setObjectName(QStringLiteral("hintLabel"));
        loading->setAlignment(Qt::AlignCenter);
        auto *placeholderLayout = new QVBoxLayout(placeholder);
        placeholderLayout->addStretch(1);
        placeholderLayout->addWidget(loading);
        placeholderLayout->addStretch(1);
        m_overviewBodyStack->addWidget(placeholder);
    }
    m_overviewBodyStack->setMinimumHeight(0);
    m_overviewBodyStack->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Ignored);

    m_repoOverviewChrome = new QWidget;
    auto *overviewChromeLayout = new QVBoxLayout(m_repoOverviewChrome);
    // The commit line is a bordered card, so keep it off the window edge and
    // in line with the right end of the mode row above it.
    overviewChromeLayout->setContentsMargins(0, 0, 16, 0);
    overviewChromeLayout->setSpacing(0);
    if (!next)
        overviewChromeLayout->addWidget(commitCard);
    if (auto *commitHeader =
            next ? dynamic_cast<NextOverviewCommitHeader *>(m_overviewList->header())
                 : nullptr) {
        commitCard->setMinimumHeight(40);
        QPointer<QWidget> card(commitCard);
        connect(m_overviewBodyStack, &QStackedWidget::currentChanged, commitHeader,
                [commitHeader, card, overviewChromeLayout](int index) {
                    if (!card)
                        return;
                    if (index == 0) {
                        overviewChromeLayout->removeWidget(card);
                        commitHeader->reclaim();
                    } else if (overviewChromeLayout->indexOf(card) < 0) {
                        overviewChromeLayout->addWidget(card);
                        card->show();
                    }
                });
    }

    auto *leftColumn = new QWidget;
    auto *leftLayout = new QVBoxLayout(leftColumn);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(next ? 0 : 8);
    leftLayout->addWidget(m_repoOverviewChrome);
    leftLayout->addWidget(m_overviewBodyStack, 1);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(leftColumn);
    return page;
}

static bool coveDocPathIsSafe(const QString &path)
{
    const QString clean = QDir::cleanPath(path.trimmed());
    return !clean.isEmpty() && clean != QLatin1String(".") &&
           !QDir::isAbsolutePath(clean) && !clean.startsWith("../") &&
           !clean.contains("/../") && clean != QLatin1String(".git") &&
           !clean.startsWith(".git/");
}

static QString coveDocMimeForPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("md") || suffix == QLatin1String("markdown"))
        return QStringLiteral("text/markdown");
    if (suffix == QLatin1String("json") || suffix == QLatin1String("jsonc"))
        return QStringLiteral("application/json");
    if (suffix == QLatin1String("html") || suffix == QLatin1String("htm"))
        return QStringLiteral("text/html");
    return QStringLiteral("text/plain");
}

static QString coveDocTabKey(const QString &coveId, const QString &path)
{
    return coveId + QChar(0x1f) + path;
}

bool MainWindow::coveEditorIsReadOnly(const QWidget *widget) const
{
    if (const auto *spreadsheet =
            dynamic_cast<const forkmesh::cove::SpreadsheetEditor *>(widget))
        return spreadsheet->isReadOnly();
    if (const auto *pipeline =
            dynamic_cast<const forkmesh::cove::PipelineEditor *>(widget))
        return pipeline->isReadOnly();
    if (const auto *text = qobject_cast<const QPlainTextEdit *>(widget))
        return text->isReadOnly();
    return true;
}

bool MainWindow::coveEditorIsModified(QWidget *widget) const
{
    if (auto *spreadsheet =
            dynamic_cast<forkmesh::cove::SpreadsheetEditor *>(widget))
        return spreadsheet->isModified();
    if (auto *pipeline = dynamic_cast<forkmesh::cove::PipelineEditor *>(widget))
        return pipeline->isModified();
    if (auto *text = qobject_cast<QPlainTextEdit *>(widget))
        return text->document()->isModified();
    return false;
}

QString MainWindow::coveEditorBody(QWidget *widget) const
{
    if (auto *spreadsheet =
            dynamic_cast<forkmesh::cove::SpreadsheetEditor *>(widget))
        return spreadsheet->body();
    if (auto *pipeline = dynamic_cast<forkmesh::cove::PipelineEditor *>(widget))
        return pipeline->body();
    if (auto *text = qobject_cast<QPlainTextEdit *>(widget))
        return text->toPlainText();
    return {};
}

void MainWindow::setCoveEditorModified(QWidget *widget, bool modified)
{
    if (auto *spreadsheet =
            dynamic_cast<forkmesh::cove::SpreadsheetEditor *>(widget)) {
        spreadsheet->setModified(modified);
    } else if (auto *pipeline =
                   dynamic_cast<forkmesh::cove::PipelineEditor *>(widget)) {
        pipeline->setModified(modified);
    } else if (auto *text = qobject_cast<QPlainTextEdit *>(widget)) {
        text->document()->setModified(modified);
    }
}

QWidget *MainWindow::buildFilesCovePanel()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("filesCovePanel"));

    auto *selectorRow = new QHBoxLayout;
    selectorRow->setContentsMargins(0, 0, 0, 0);
    selectorRow->setSpacing(6);

    m_coveExplorerStatus = new QLabel;
    m_coveExplorerStatus->setObjectName("statusLine");
    m_coveExplorerStatus->setText(QStringLiteral("Cove Explorer"));
    m_coveExplorerStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_coveExplorerSelector = new QComboBox;
    m_coveExplorerSelector->setObjectName("repoPicker");
    m_coveExplorerSelector->setMinimumWidth(160);
    m_coveExplorerSelector->setToolTip(
        QStringLiteral("Account-invited coves in this repository"));
    connect(m_coveExplorerSelector,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                m_coveExplorerCurrentId =
                    m_coveExplorerSelector
                        ? m_coveExplorerSelector->currentData().toString()
                        : QString();
                refreshCoveExplorerTree();
            });

    m_coveExplorerNewButton = new QPushButton("New cove");
    m_coveExplorerNewButton->setObjectName("ghostButton");
    m_coveExplorerNewButton->setCursor(Qt::PointingHandCursor);
    m_coveExplorerNewButton->setToolTip(
        QStringLiteral("Create an account-scoped cove in this repo"));
    setOcticon(m_coveExplorerNewButton, "plus", 16);
    connect(m_coveExplorerNewButton, &QPushButton::clicked, this,
            &MainWindow::createCoveExplorerCove);

    m_coveExplorerInviteButton = new QPushButton("Share");
    m_coveExplorerInviteButton->setObjectName("ghostButton");
    m_coveExplorerInviteButton->setCursor(Qt::PointingHandCursor);
    m_coveExplorerInviteButton->setToolTip(
        QStringLiteral("See everyone with access, invite people, and send the cove link"));
    setOcticon(m_coveExplorerInviteButton, "person", 16);
    connect(m_coveExplorerInviteButton, &QPushButton::clicked, this,
            &MainWindow::inviteUserToCurrentCove);

    m_coveExplorerNewFileButton = new QPushButton("Quick create");
    m_coveExplorerNewFileButton->setObjectName("ghostButton");
    m_coveExplorerNewFileButton->setCursor(Qt::PointingHandCursor);
    m_coveExplorerNewFileButton->setToolTip(
        QStringLiteral("Create from ten ready-to-edit Cove file templates"));
    setOcticon(m_coveExplorerNewFileButton, "file", 16);
    connect(m_coveExplorerNewFileButton, &QPushButton::clicked, this,
            &MainWindow::createCoveExplorerDocument);

    m_coveExplorerDeleteButton = new QPushButton("Delete");
    m_coveExplorerDeleteButton->setObjectName("ghostButton");
    m_coveExplorerDeleteButton->setCursor(Qt::PointingHandCursor);
    m_coveExplorerDeleteButton->setToolTip(
        QStringLiteral("Delete the selected cove document"));
    setOcticon(m_coveExplorerDeleteButton, "trash", 16);
    connect(m_coveExplorerDeleteButton, &QPushButton::clicked, this,
            &MainWindow::deleteCurrentCoveExplorerDocument);

    selectorRow->addWidget(m_coveExplorerSelector, 1);
    selectorRow->addWidget(m_coveExplorerStatus);

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(6);
    actions->addWidget(m_coveExplorerNewButton);
    actions->addWidget(m_coveExplorerInviteButton);
    actions->addWidget(m_coveExplorerNewFileButton);
    actions->addWidget(m_coveExplorerDeleteButton);
    actions->addStretch();

    auto *sharingBar = new QWidget;
    sharingBar->setObjectName(QStringLiteral("coveSharingBar"));
    auto *sharingLayout = new QHBoxLayout(sharingBar);
    sharingLayout->setContentsMargins(8, 5, 8, 5);
    sharingLayout->setSpacing(8);
    auto *sharingIcon = new QLabel;
    sharingIcon->setPixmap(tintedOcticonPixmap(
        QStringLiteral("lock"), QColor(QStringLiteral("#8b949e")), 14));
    sharingLayout->addWidget(sharingIcon);
    m_coveExplorerSharedWithLabel = new QLabel(QStringLiteral("Shared with —"));
    m_coveExplorerSharedWithLabel->setObjectName(QStringLiteral("statusLine"));
    m_coveExplorerSharedWithLabel->setWordWrap(true);
    m_coveExplorerSharedWithLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_coveExplorerSharedWithLabel->setToolTip(
        QStringLiteral("People listed here can open and edit files in this cove"));
    sharingLayout->addWidget(m_coveExplorerSharedWithLabel, 1);
    m_coveExplorerCopyLinkButton = new QPushButton(QStringLiteral("Copy link"));
    m_coveExplorerCopyLinkButton->setObjectName(QStringLiteral("ghostButton"));
    m_coveExplorerCopyLinkButton->setCursor(Qt::PointingHandCursor);
    m_coveExplorerCopyLinkButton->setToolTip(
        QStringLiteral("Copy a secret-free link to this encrypted cove"));
    setOcticon(m_coveExplorerCopyLinkButton, QStringLiteral("link"), 14);
    connect(m_coveExplorerCopyLinkButton, &QPushButton::clicked, this, [this] {
        int coveIndex = -1;
        for (int i = 0; i < m_coveExplorerCoves.size(); ++i) {
            if (m_coveExplorerCoves.at(i).id == m_coveExplorerCurrentId) {
                coveIndex = i;
                break;
            }
        }
        if (coveIndex < 0 || m_repoDetailIndex < 0 ||
            m_repoDetailIndex >= m_repositories.size())
            return;
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        const QString link = forkmesh::cove::shareUrl(
            repo.owner, repo.name, m_coveExplorerCoves.at(coveIndex).id);
        if (link.isEmpty())
            return;
        QApplication::clipboard()->setText(link);
        setRepoDetailNotice(QStringLiteral(
            "Cove link copied. Only people already listed in Shared with can unlock it."));
    });
    sharingLayout->addWidget(m_coveExplorerCopyLinkButton);

    m_coveExplorerTree = new QTreeWidget;
    m_coveExplorerTree->setObjectName("fileTree");
    enableHoverRowHighlight(m_coveExplorerTree);
    m_coveExplorerTree->setColumnCount(2);
    m_coveExplorerTree->setHeaderHidden(true);
    m_coveExplorerTree->header()->setStretchLastSection(false);
    m_coveExplorerTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_coveExplorerTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_coveExplorerTree->setMinimumWidth(280);
    m_coveExplorerTree->setIndentation(14);
    connect(m_coveExplorerTree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!item)
                    return;
                if (item->data(0, Qt::UserRole + 1).toBool()) {
                    item->setExpanded(!item->isExpanded());
                    return;
                }
                openCoveExplorerDocument(item->data(0, Qt::UserRole).toString());
            });
    connect(m_coveExplorerTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (item && !item->data(0, Qt::UserRole + 1).toBool())
                    openCoveExplorerDocument(item->data(0, Qt::UserRole).toString());
            });
    connectFileTreeFolderIcons(m_coveExplorerTree,
                               [this](bool opened) { return iconForDir(opened); });

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addLayout(selectorRow);
    layout->addLayout(actions);
    layout->addWidget(sharingBar);
    layout->addWidget(m_coveExplorerTree, 1);
    return page;
}

QString MainWindow::iconsDir() const
{
    // Shared with the rich-text rows in the diff views, which inline the same
    // icon set as base64 rather than handing QIcons to an item view.
    return treeIconsDir();
}

QIcon MainWindow::iconForFile(const QString &fileName) const
{
    const QString dir = iconsDir();
    if (dir.isEmpty())
        return {};
    const QString name = fileTypeIconName(fileName.toLower());
    // One row per changed file, each stat()ing the icon directory and building
    // a fresh QIcon that re-parses the SVG on its first pixmap() — sampled
    // inside showRangeFilesInSourceControl() on a wide branch diff. The icon
    // set does not change while the app runs.
    const auto cached = m_fileTypeIcons.constFind(name);
    if (cached != m_fileTypeIcons.cend())
        return *cached;
    QString path = dir + "/" + name + ".svg";
    if (!QFileInfo::exists(path))
        path = dir + "/default_file.svg";
    return *m_fileTypeIcons.insert(name, QIcon(path));
}

QIcon MainWindow::iconForDir(bool opened) const
{
    const QString dir = iconsDir();
    if (dir.isEmpty())
        return {};
    return QIcon(dir + (opened ? "/default_folder_opened.svg" : "/default_folder.svg"));
}

QString MainWindow::repoGitDir() const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return {};
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    if (!repo.localPath.isEmpty() && QDir(repo.localPath).exists())
        return repo.localPath;
    if (!repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists())
        return repo.mirrorPath;
    return {};
}

void MainWindow::setRepoDetailNotice(const QString &message, bool error,
                                     const QString &kind)
{
    if (m_repoDetailNotice) {
        m_repoDetailNotice->clear();
        m_repoDetailNotice->hide();
    }
    flashMessage(message, error, QString(), kind);
}

void MainWindow::forkCurrentRepo()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord src = m_repositories.at(m_repoDetailIndex);

    const QString owner = accountOwner();
    const QString name = repoSegment(src.name, QStringLiteral("repository"));
    if (owner.isEmpty() || name.isEmpty())
        return;
    for (const RepositoryRecord &r : std::as_const(m_repositories))
        if (!r.previewOnly && r.owner == owner && r.name == name) {
            QMessageBox::information(
                this, "Fork repository",
                QStringLiteral("You already have %1/%2.").arg(owner, name));
            return;
        }

    const QString source =
        (!src.mirrorPath.isEmpty() && QDir(src.mirrorPath).exists())
            ? src.mirrorPath
            : repositorySource(src);
    if (source.isEmpty()) {
        QMessageBox::warning(
            this, "Fork repository",
            "There is no local mirror or source to fork from yet. Sync the "
            "repository first, then fork.");
        return;
    }

    const QString parentDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose a folder to fork %1 into").arg(name),
        QDir::homePath());
    if (parentDir.isEmpty())
        return; // cancelled
    const QString targetDir = QDir(parentDir).absoluteFilePath(name);
    if (QDir(targetDir).exists() && !QDir(targetDir).isEmpty()) {
        QMessageBox::information(
            this, "Fork repository",
            QStringLiteral("%1 already exists and isn't empty. Choose another "
                           "location for the fork.")
                .arg(QDir::toNativeSeparators(targetDir)));
        return;
    }

    RepositoryRecord fork;
    fork.owner = owner;
    fork.name = name;
    fork.forkedFrom = (src.owner.trimmed() + QLatin1Char('/') +
                       src.name.trimmed()).toLower();
    fork.description = src.description;
    fork.cloneUrl = src.cloneUrl.trimmed().isEmpty()
                        ? repositorySource(src)
                        : src.cloneUrl.trimmed();
    fork.solanaAddress = savedSolanaAddress();
    fork.publishToNetwork = true;
    fork.actionsEnabled = src.actionsEnabled;
    fork.disabledWorkflows = src.disabledWorkflows;
    fork.workflowNodes = src.workflowNodes;
    fork.hostedSinceMs = QDateTime::currentMSecsSinceEpoch();
    fork.mirrorPath = repositoryMirrorRoot() + "/" +
                      repoSegment(owner, QStringLiteral("owner")) + "-" +
                      repoSegment(name, QStringLiteral("repository")) + ".git";
    fork.localPath = targetDir;

    fork.relayUrl = src.relayUrl;
    stampRepositoryRelay(fork);
    m_repositories.append(fork);
    saveRepositories();
    refreshRepositoryList();
    logSystem(QStringLiteral("Forking %1/%2 to %3/%4 in %5 from %6")
                  .arg(src.owner, src.name, owner, name,
                       QDir::toNativeSeparators(targetDir), source));

    QDir().mkpath(QFileInfo(fork.mirrorPath).absolutePath());
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this,
            [this, process, owner, name, targetDir](int code, QProcess::ExitStatus) {
                const QString err =
                    QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                const int idx = repoIndexFor(owner, name);
                if (idx < 0)
                    return;
                if (code != 0) {
                    logSystem("Fork failed: " + err.right(300));
                    QMessageBox::warning(this, "Fork repository",
                                         "Could not clone the fork" +
                                             (err.isEmpty() ? QString()
                                                            : ":\n" + err.right(400)));
                    m_repositories.removeAt(idx);
                    saveRepositories();
                    refreshRepositoryList();
                    return;
                }
                RepositoryRecord &f = m_repositories[idx];
                runGitDetached(f.mirrorPath,
                               {QStringLiteral("remote"), QStringLiteral("remove"),
                                QStringLiteral("origin")},
                               [](bool, const QByteArray &) {});
                f.lastSyncMs = QDateTime::currentMSecsSinceEpoch();
                saveRepositories();
                ensurePushHook(f);
                const QString mirrorPath = f.mirrorPath;
                if (m_backend)
                    m_backend->addChannel(repositoryChannel(f));
                QString gatewayError;
                if (!rebuildDirectMirrorGatewayConfiguration(
                        &gatewayError, true)) {
                    logSystem(
                        QStringLiteral(
                            "Fork: direct mesh publication will retry after setup: %1")
                            .arg(gatewayError));
                }
                startNodeEventSocket();
                refreshRepositoryList();
                logSystem(QStringLiteral("Forked into %1/%2.").arg(owner, name));

                QDir().mkpath(QFileInfo(targetDir).absolutePath());
                auto *checkout = new QProcess(this);
                connect(
                    checkout, &QProcess::finished, this,
                    [this, checkout, owner, name, targetDir](int wcode,
                                                             QProcess::ExitStatus) {
                        const QString werr =
                            QString::fromUtf8(checkout->readAllStandardError())
                                .trimmed();
                        checkout->deleteLater();
                        const int i = repoIndexFor(owner, name);
                        if (i < 0)
                            return;
                        if (wcode != 0) {
                            logSystem("Fork checkout failed: " + werr.right(300));
                            QMessageBox::warning(
                                this, "Fork repository",
                                "The fork was created, but checking it out into "
                                "the chosen folder failed" +
                                    (werr.isEmpty() ? QString()
                                                    : ":\n" + werr.right(400)));
                            m_repositories[i].localPath.clear();
                            saveRepositories();
                        } else {
                            configureForkSource(i, QString(), true);
                            ensurePushHook(m_repositories.at(i));
                            logSystem(QStringLiteral("Checked out %1/%2 into %3.")
                                          .arg(owner, name,
                                               QDir::toNativeSeparators(targetDir)));
                        }
                        refreshRepositoryList();
                        openRepoDetail(i);
                    });
                checkout->start(QStringLiteral("git"),
                                {QStringLiteral("clone"),
                                 QStringLiteral("--no-local"),
                                 QStringLiteral("--reference-if-able"),
                                 mirrorPath, mirrorPath, targetDir});
            });
    trackProcessActivity(process, QStringLiteral("fork"),
                         QStringLiteral("Forking %1/%2").arg(owner, name));
    // Fork without duplicating the code: when the upstream already keeps a bare
    // mirror on this machine, the new mirror borrows its object store through
    // git alternates (--reference) rather than copying every object. --no-local
    // turns off the local-clone shortcut that would otherwise hardlink the
    // whole objects directory in and make the reference pointless, so the clone
    // transfers only what the reference does not already have - for a fresh
    // fork, essentially just the refs. deleteRepositoryAt() re-materializes the
    // borrowed objects into each fork before an upstream mirror is removed.
    QStringList cloneArgs{QStringLiteral("clone"), QStringLiteral("--mirror")};
    if (!src.mirrorPath.trimmed().isEmpty() &&
        QDir(src.mirrorPath.trimmed()).exists()) {
        cloneArgs << QStringLiteral("--no-local")
                  << QStringLiteral("--reference-if-able")
                  << src.mirrorPath.trimmed();
    }
    cloneArgs << source << fork.mirrorPath;
    process->start(QStringLiteral("git"), cloneArgs);
}

namespace {

// Where git records borrowed object stores, for either a bare mirror or a
// working checkout.
QString alternatesFileFor(const QString &gitDir)
{
    const QString root = QDir::cleanPath(gitDir.trimmed());
    if (root.isEmpty())
        return {};
    const QString bare = root + QStringLiteral("/objects/info/alternates");
    if (QFile::exists(bare))
        return bare;
    const QString worktree = root + QStringLiteral("/.git/objects/info/alternates");
    return QFile::exists(worktree) ? worktree : QString();
}

// True when `alternatesFile` lists an object store inside `parent`.
bool alternatesPointInto(const QString &alternatesFile, const QString &parent)
{
    QFile file(alternatesFile);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QStringList entries = QString::fromUtf8(file.readAll())
                                    .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const QString cleaned = QDir::cleanPath(entry.trimmed());
        if (cleaned == parent || cleaned.startsWith(parent + QLatin1Char('/')))
            return true;
    }
    return false;
}

} // namespace

void MainWindow::detachForksBorrowingMirror(const QString &mirrorPath)
{
    const QString parent = QDir::cleanPath(mirrorPath.trimmed());
    if (parent.isEmpty())
        return;
    for (const RepositoryRecord &fork : std::as_const(m_repositories)) {
        for (const QString &gitDir : {fork.mirrorPath, fork.localPath}) {
            if (QDir::cleanPath(gitDir.trimmed()) == parent)
                continue;
            const QString alternates = alternatesFileFor(gitDir);
            if (alternates.isEmpty() ||
                !alternatesPointInto(alternates, parent))
                continue;
            logSystem(
                QStringLiteral("Fork: copying borrowed objects into %1/%2 "
                               "before its upstream mirror is removed.")
                    .arg(fork.owner, fork.name));
            // `repack -a -d` *without* --local rewrites the pack to include
            // every reachable object, borrowed ones included; only then is the
            // alternates entry safe to drop.
            const QString dir = QDir::cleanPath(gitDir.trimmed());
            if (QProcess::execute(QStringLiteral("git"),
                                  {QStringLiteral("-C"), dir,
                                   QStringLiteral("repack"),
                                   QStringLiteral("-a"),
                                   QStringLiteral("-d")}) != 0) {
                logSystem(QStringLiteral(
                              "Fork: could not repack %1; leaving it attached "
                              "to the mirror it borrows from.")
                              .arg(QDir::toNativeSeparators(dir)));
                continue;
            }
            QFile::remove(alternates);
        }
    }
}

bool MainWindow::configureForkSource(int index, const QString &sourceUrl,
                                     bool notifyOnError)
{
    if (index < 0 || index >= m_repositories.size())
        return false;
    RepositoryRecord &fork = m_repositories[index];
    const QString source = sourceUrl.trimmed().isEmpty()
                               ? fork.cloneUrl.trimmed()
                               : sourceUrl.trimmed();
    if (source.isEmpty() || fork.localPath.trimmed().isEmpty() ||
        fork.mirrorPath.trimmed().isEmpty())
        return false;

    const bool sourceChanged = fork.cloneUrl.trimmed() != source;
    if (sourceChanged) {
        fork.cloneUrl = source;
        saveRepositories();
    }

    QString error;
    if (!forkmesh::upstream::configureForkCheckoutRemote(
            fork.localPath, source, fork.mirrorPath, &error)) {
        logSystem(QStringLiteral("Fork: could not configure %1/%2 to pull from %3: %4")
                      .arg(fork.owner, fork.name, source, error));
        if (notifyOnError)
            flashMessage(QStringLiteral("The fork was created, but its source remote "
                                        "could not be configured: %1")
                             .arg(error),
                         true);
        return false;
    }
    if (sourceChanged)
        logSystem(QStringLiteral("Fork: %1/%2 now pulls from %3 and pushes to its local mirror.")
                      .arg(fork.owner, fork.name, source));
    return true;
}

void MainWindow::downloadCurrentRepoZip()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString dir = repoGitDir();
    if (dir.isEmpty()) {
        setRepoDetailNotice(
            "Sync this repository first; there is no local mirror to archive yet.",
            true);
        return;
    }

    const QString downloads =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString outDir = downloads.isEmpty() ? QDir::homePath() : downloads;
    QDir().mkpath(outDir);

    const QString safeOwner = repoSegment(repo.owner, QStringLiteral("owner"));
    const QString safeName = repoSegment(repo.name, QStringLiteral("repository"));
    const QString safeRef = repoSegment(currentRef(), QStringLiteral("head"));
    const QString base = safeOwner + "-" + safeName + "-" + safeRef;
    QString target = QDir(outDir).filePath(base + ".zip");
    for (int i = 2; QFileInfo::exists(target); ++i)
        target = QDir(outDir).filePath(base + "-" + QString::number(i) + ".zip");

    if (m_sourceButton) {
        m_sourceButton->setEnabled(false);
        m_sourceButton->setText("Zipping...");
    }
    setRepoDetailNotice("Creating ZIP archive in Downloads...");

    auto *process = new QProcess(this);
    auto handled = std::make_shared<bool>(false);
    auto finishButton = [this] {
        if (m_sourceButton) {
            m_sourceButton->setEnabled(true);
            m_sourceButton->setText("Source");
            setOcticon(m_sourceButton, "code", 16);
        }
    };
    connect(process, &QProcess::errorOccurred, this,
            [this, process, handled, finishButton](QProcess::ProcessError) {
                if (*handled)
                    return;
                *handled = true;
                finishButton();
                setRepoDetailNotice("Could not start git archive.", true);
                process->deleteLater();
            });
    connect(process, &QProcess::finished, this,
            [this, process, handled, finishButton, target](int code,
                                                           QProcess::ExitStatus status) {
                if (*handled)
                    return;
                *handled = true;
                const QString err =
                    QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                finishButton();
                if (status != QProcess::NormalExit || code != 0) {
                    setRepoDetailNotice(
                        "Could not create ZIP archive" +
                            (err.isEmpty() ? QString() : ": " + err.left(240)),
                        true);
                    return;
                }
                setRepoDetailNotice("Saved ZIP archive to " +
                                    QFileInfo(target).absoluteFilePath());
                logSystem("Saved repository ZIP to " + target);
            });
    process->start(
        QStringLiteral("git"),
        {QStringLiteral("-C"), dir, QStringLiteral("archive"),
         QStringLiteral("--format=zip"), QStringLiteral("-o"), target,
         QStringLiteral("--prefix=") + safeOwner + "-" + safeName + "/",
         currentRef()});
}

bool MainWindow::bindRepoDetailToRepo(int repoIndex)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return false;
    // Copy the identity out before the load: openRepoDetail pumps the event loop
    // over a dozen blocking git reads, and a roster callback landing in that pump
    // can reallocate m_repositories (git-pump UAF family).
    const QString owner = m_repositories.at(repoIndex).owner;
    const QString name = m_repositories.at(repoIndex).name;
    if (repoIndex != m_repoDetailIndex)
        openRepoDetail(repoIndex);
    return m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()
           && m_repositories.at(m_repoDetailIndex).owner == owner
           && m_repositories.at(m_repoDetailIndex).name == name;
}

void MainWindow::openRepoDetail(int repoIndex, int landingTab)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    m_mirrorNodesRepoKey.clear();
    if (landingTab == kRetiredMirrorNodesDetailTab) {
        landingTab = kRepoLandingTab;
        QTimer::singleShot(0, this, [this, repoIndex] {
            showMirrorNodesOnNetwork(repoIndex);
        });
    }
    ensureRepoDetailSectionBuilt();
    if (landingTab < 0 || !m_repoDetailStack ||
        landingTab >= m_repoDetailStack->count())
        landingTab = kRepoLandingTab;
    ensureRepoDetailTabBuilt(kRepoLandingTab);
    if (landingTab != kRepoLandingTab)
        ensureRepoDetailTabBuilt(landingTab);
    // Guard against re-entrancy: a node switch yields the event loop between load
    // steps (see nodeSwitchStep), so a queued call must not start a second load
    // on top of this one.
    if (m_repoDetailLoading)
        return;
    m_repoDetailLoading = true;
    // A caller landing past Code switches before the first blocking read so
    // a page already on screen goes straight to that tab.
    if (landingTab != kRepoLandingTab && m_repoDetailStack) {
        if (m_repoDetailTabs && m_repoDetailTabs->button(landingTab))
            m_repoDetailTabs->button(landingTab)->setChecked(true);
        m_repoDetailStack->setCurrentIndex(landingTab);
    }
    GitKeepAlive keepAlive;
    // Every git read this load asks for is attributed to it on the footer's
    // outcome lines (adhoc #186).
    const forkmesh::BackgroundRoutine routine(
        QStringLiteral("opening repo %1/%2")
            .arg(m_repositories.at(repoIndex).owner,
                 m_repositories.at(repoIndex).name));
    if (repoIndex != m_repoDetailIndex)
        clearBranchDiffCache();
    const bool repositoryChanged = m_repoDetailIndex != repoIndex;
    m_repoDetailIndex = repoIndex;
    renderPageNotices();
    if (repositoryChanged)
        QTimer::singleShot(0, this, [this] { notifyNativePackagesRepositoryChanged(); });
    {
        const RepositoryRecord &opened = m_repositories.at(repoIndex);
        const QString source =
            repoSegment(opened.owner, QStringLiteral("owner")) +
            QLatin1Char('/') +
            repoSegment(opened.name, QStringLiteral("repository"));
        QJsonObject cached = m_mirrorPendingCache.value(source);
        if (!cached.isEmpty()) {
            cached.insert(QStringLiteral("clientFetchedAt"), 0);
            m_mirrorPendingCache.insert(source, cached);
        }
    }
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    nodeSwitchStep(QStringLiteral("Reading %1/%2 metadata…")
                       .arg(repo.owner, repo.name));
    if (!repo.previewOnly)
        QSettings().setValue(kLastRepositorySetting, repo.owner + "/" + repo.name);
    QWidget *nextHeader = m_repoHeaderTitle ? m_repoHeaderTitle->parentWidget() : nullptr;
    if (nextHeader && nextHeader->objectName() != QLatin1String("repoNextHeader"))
        nextHeader = nullptr;
    if (m_repoHeaderTitle && nextHeader)
        m_repoHeaderTitle->setText(
            QStringLiteral("<span style='color:%3'>%1</span>"
                           "<span style='color:%4'>&nbsp;/&nbsp;</span>"
                           "<span style='color:%5; font-weight:600'>%2</span>")
                .arg(repo.owner.toHtmlEscaped(), repo.name.toHtmlEscaped(),
                     Theme::hex(Theme::Role::FgMuted), Theme::hex(Theme::Role::FgSubtle),
                     Theme::hex(Theme::Role::Fg)));
    else if (m_repoHeaderTitle)
        m_repoHeaderTitle->setText(
            QStringLiteral("%1 / <b>%2</b>")
                .arg(repo.owner.toHtmlEscaped(), repo.name.toHtmlEscaped()));
    if (nextHeader) {
        if (auto *tile = nextHeader->findChild<QLabel *>(QStringLiteral("repoNextHeaderTile")))
            tile->setPixmap(tintedOcticonPixmap(QStringLiteral("repo"),
                                                Theme::color(Theme::Role::FgMuted), 16));
        if (auto *pill = nextHeader->findChild<QLabel *>(QStringLiteral("repoVisibilityPill"))) {
            pill->setText(repo.previewOnly         ? QStringLiteral("Preview")
                          : repo.isPrivate         ? QStringLiteral("Private")
                          : repo.publishToNetwork  ? QStringLiteral("Public")
                                                   : QStringLiteral("Local only"));
            pill->show();
        }
    }
    setRepoDetailNotice(QString());

    m_repoInfo = RepoInfo();
    m_repoBranch.clear();
    loadRepoInfo();
    loadBranchesAndTags();
    if (auto *fork = dynamic_cast<VerticalIconButton *>(m_forkButton)) {
        fork->setText(QStringLiteral("Fork"));
        fork->setBadgeCount(m_repoInfo.forks);
    }
    if (auto *mirror = dynamic_cast<VerticalIconButton *>(m_mirrorButton)) {
        if (repo.previewOnly) {
            mirror->setText(QStringLiteral("Mirror it"));
            mirror->setBadgeCount(0);
            mirror->setToolTip(
                "Clone this preview into your local mirrors and host it");
        } else {
            mirror->setText(QStringLiteral("Mirror"));
            mirror->setBadgeCount(qMax(1, m_repoInfo.mirrors));
            mirror->setToolTip("Sync this repository's mirror now");
        }
    }
    updateRepoDetailStatus();
    if (QWidget *about = nextAboutSidebarIn(m_filesStack)) {
        updateNextAboutInfo(about, repo, m_repoInfo);
        if (auto *tags = dynamic_cast<VerticalIconButton *>(m_tagsButton))
            updateNextAboutReleases(about, tags->badgeCount());
    }
    updateRepoActionMenus();
    refreshRepoSettings();
    updateRepoCodeSize();
    updateFooterGitIdentity();
    updateFooterCommitInfo();

    refreshIssuesRepoCombo();
    if (m_issuesRepoCombo) {
        const int combo = m_issuesRepoCombo->findData(repoIndex);
        if (combo >= 0)
            m_issuesRepoCombo->setCurrentIndex(combo);
    }
    if (m_agentComposeRepo) {
        const int combo = m_agentComposeRepo->findData(repoIndex);
        if (combo >= 0)
            m_agentComposeRepo->setCurrentIndex(combo);
    }
    logStartup(QStringLiteral("  openRepo: info+branches+codeSize done"));
    m_currentPulls.clear();
    m_pullOpenTarget = -1;
    m_pullRegisteredCount = -1;
    // The list this repository is about to load has never been rendered, so
    // the previous repository's snapshot digest must not let
    // applyLoadedPulls() mistake it for one already on screen.
    m_pullSnapshotDigest.clear();
    if (m_pullTable)
        m_pullTable->setRowCount(0);
    updateRepoPullCount();
    reloadPullsInBackground();

    m_currentIssues.clear();
    m_issuesLoadedSig.clear();
    if (m_issueTable)
        m_issueTable->setRowCount(0);
    updateRepoIssueCount();
    m_currentDiscussions.clear();
    if (m_discussionTable)
        m_discussionTable->setRowCount(0);
    updateRepoDiscussionCount();
    logStartup(QStringLiteral("  openRepo: selected metadata scheduled"));

    // The Code tab's overview still loads below, but the stack sits on the
    // caller's tab so the loads' event-loop pumps never paint the file list
    // on the way to it.
    if (m_repoDetailTabs && m_repoDetailTabs->button(landingTab))
        m_repoDetailTabs->button(landingTab)->setChecked(true);
    if (m_repoDetailStack)
        m_repoDetailStack->setCurrentIndex(landingTab);
    clearRepoFileTabs();
    if (m_repoFileTree)
        m_repoFileTree->clear();
    if (m_coveExplorerTree)
        m_coveExplorerTree->clear();
    if (m_coveExplorerSelector)
        m_coveExplorerSelector->clear();
    m_coveExplorerCoves.clear();
    m_coveExplorerCurrentId.clear();
    m_treeLoadedForIndex = -1;

    const int searchIndexFor = m_repoDetailIndex;
    QTimer::singleShot(0, this, [this, searchIndexFor] {
        if (m_repoDetailIndex != searchIndexFor)
            return;
        loadFileSearchIndex();
    });
    nodeSwitchStep(QStringLiteral("Loading commit history…"));
    showOverviewFiles();
    if (m_commitsTable)
        m_commitsTable->setRowCount(0);
    m_commitsLoadedTip.clear();
    ++m_commitsLoadGen;
    logStartup(QStringLiteral("  openRepo: commits loaded"));
    m_overviewLoadedKey.clear();
    if (m_overviewList)
        m_overviewList->clear();
    nodeSwitchStep(QStringLiteral("Rendering overview…"));
    loadRepoOverview(QString());
    logStartup(QStringLiteral("  openRepo: overview loaded"));
    showRepoOverview();
    showSection(0);
    updateBreadcrumb();
    m_repoWorkflows.clear();
    if (m_actionsTable)
        m_actionsTable->setRowCount(0);
    updateActionsTabIndicator(); // reflect any in-flight runs for this repo
    const int countsFor = m_repoDetailIndex;
    QTimer::singleShot(0, this, [this, countsFor] {
        if (m_repoDetailIndex == countsFor)
            refreshRepoTabCounts();
    });
    refreshRepoPinBanner();      // warn if the relay's integrity pin is stale
    refreshRepoChangeBadge();
    refreshNodeDotMatrix();
    m_repoDetailLoading = false;
    ensureTeammateScheduleTimer();
    QTimer::singleShot(0, this, &MainWindow::checkTeammateSchedules);
}

void MainWindow::updateRepoCodeSize()
{
    if (m_repoCodeTab)
        m_repoCodeTab->setText(QStringLiteral("Code"));
    m_repoCodeSizePath.clear();
}

void MainWindow::updateRepoIssueCount()
{
    if (!m_repoIssuesTab)
        return;
    const auto showCount = [this](int mirrored) {
        int openCount = 0;
        for (const Issue &issue : std::as_const(m_currentIssues)) {
            if (issue.status != "closed" && !issue.isDeleted())
                ++openCount;
        }
        if (auto *b = dynamic_cast<VerticalIconButton *>(m_repoIssuesTab))
            b->setBadgeCount(mirrored >= 0 ? mirrored : openCount);
    };

    const quint64 gen = ++m_repoIssueCountGen;
    if (m_repoIssueCountInFlight) {
        m_repoIssueCountReloadQueued = true;
        return;
    }
    m_repoIssueCountReloadQueued = false;
    const QString mirrorPath =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()
            ? m_repositories.at(m_repoDetailIndex).mirrorPath
            : QString();
    if (mirrorPath != m_repoIssueCountMirror) {
        m_repoIssueCountMirror = mirrorPath;
        showCount(-1);
    }
    if (mirrorPath.isEmpty())
        return;
    const int repoIndex = m_repoDetailIndex;
    m_repoIssueCountInFlight = true;
    runOffThread<int>(
        [mirrorPath] {
            const forkmesh::BackgroundScope action(
                QStringLiteral("issues"),
                QStringLiteral("count mirrored issues"),
                forkmesh::ActionTelemetry::Execution::Worker);
            const MirrorBranchTip tip =
                mirrorPrimaryBranchTip(mirrorPath, QString());
            return mirrorIssueCount(mirrorPath, tip.branch);
        },
        [this, gen, repoIndex, mirrorPath, showCount](int mirrored) {
            m_repoIssueCountInFlight = false;
            if (std::exchange(m_repoIssueCountReloadQueued, false)) {
                updateRepoIssueCount();
                return;
            }
            if (gen != m_repoIssueCountGen || repoIndex != m_repoDetailIndex ||
                repoIndex < 0 || repoIndex >= m_repositories.size() ||
                m_repositories.at(repoIndex).mirrorPath != mirrorPath)
                return;
            showCount(mirrored);
        });
}

void MainWindow::updateRepoDiscussionCount()
{
    auto *b = dynamic_cast<VerticalIconButton *>(m_repoDiscussionsTab);
    if (!b)
        return;
    QString repoKey;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        repoKey = repo.owner + "/" + repo.name;
    }
    const QVariantMap stamps = discussionReadStamps();
    b->setBadgeCount(std::count_if(
        m_currentDiscussions.begin(), m_currentDiscussions.end(),
        [&stamps, &repoKey](const Discussion &discussion) {
            // Status is deliberately ignored: closing or archiving a thread
            // is itself activity worth a look, and a read closed thread
            // drops out on its stamp anyway.
            return discussion.updatedAt() >
                   stamps
                       .value(discussionReadStampKey(repoKey,
                                                     discussion.number))
                       .toLongLong();
        }));
}

void MainWindow::updateRepoPullCount()
{
    // The badge counts work in flight, the way the list's Open filter does.
    // Counting every record instead read as three digits on a repository
    // whose merged history is long and whose open queue is four.
    int loadedOpen = 0;
    for (const PullRequest &pull : std::as_const(m_currentPulls))
        if (pullDisplayStatus(pull) == QLatin1String("open"))
            ++loadedOpen;
    auto *button = dynamic_cast<VerticalIconButton *>(m_repoPullsTab);
    if (button)
        button->setBadgeCount(loadedOpen);

    const quint64 gen = ++m_repoPullCountGen;
    if (m_repoPullCountInFlight) {
        m_repoPullCountReloadQueued = true;
        return;
    }
    m_repoPullCountReloadQueued = false;
    if (m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size()) {
        return;
    }
    const int repoIndex = m_repoDetailIndex;
    const QString mirrorPath = m_repositories.at(repoIndex).mirrorPath;
    if (mirrorPath.isEmpty())
        return;

    // mirrorPrimaryBranchTip() and mirrorPullCount() each issue Git reads.
    // This badge is refreshed from several pull/list paths, so doing them here
    // synchronously created a nested event loop in the middle of showPull().
    // The stall log caught that nesting fourteen times in its recent window.
    // Both figures come back: the badge shows the open ones, while the list's
    // All button still reports every registered record.
    struct MirroredPullCounts {
        int registered = -1;
        int open = -1;
    };
    m_repoPullCountInFlight = true;
    runOffThread<MirroredPullCounts>(
        [mirrorPath] {
            const forkmesh::BackgroundScope action(
                QStringLiteral("pulls"),
                QStringLiteral("count mirrored pull requests"),
                forkmesh::ActionTelemetry::Execution::Worker);
            const MirrorBranchTip tip =
                mirrorPrimaryBranchTip(mirrorPath, QString());
            return MirroredPullCounts{
                mirrorPullCount(mirrorPath, tip.branch),
                mirrorOpenPullCount(mirrorPath, tip.branch)};
        },
        [this, gen, repoIndex, mirrorPath](MirroredPullCounts mirrored) {
            m_repoPullCountInFlight = false;
            const bool reload = std::exchange(m_repoPullCountReloadQueued,
                                              false);
            if (reload) {
                updateRepoPullCount();
                return;
            }
            if (gen != m_repoPullCountGen || repoIndex != m_repoDetailIndex ||
                repoIndex < 0 || repoIndex >= m_repositories.size() ||
                m_repositories.at(repoIndex).mirrorPath != mirrorPath ||
                mirrored.registered < 0) {
                return;
            }
            if (mirrored.open >= 0) {
                if (auto *current =
                        dynamic_cast<VerticalIconButton *>(m_repoPullsTab)) {
                    current->setBadgeCount(mirrored.open);
                }
            }
            m_pullRegisteredCount = mirrored.registered;
            if (m_pullTable)
                refreshPullList();
        });
}

#ifdef FORKMESH_WINDOW_TESTS
int MainWindow::testRepoIssuesTabBadgeCount() const
{
    const auto *tab =
        dynamic_cast<const VerticalIconButton *>(m_repoIssuesTab);
    return tab ? int(tab->badgeCount()) : -1;
}

int MainWindow::testRepoPullsTabBadgeCount() const
{
    const auto *tab =
        dynamic_cast<const VerticalIconButton *>(m_repoPullsTab);
    return tab ? int(tab->badgeCount()) : -1;
}

int MainWindow::testRepoDiscussionsTabBadgeCount() const
{
    const auto *tab =
        dynamic_cast<const VerticalIconButton *>(m_repoDiscussionsTab);
    return tab ? int(tab->badgeCount()) : -1;
}
#endif

void MainWindow::refreshRepoTabCounts()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    reloadIssuesInBackground();
    // Pulls live on their own metadata ref, so a code-branch refresh does not
    // invalidate the already-loaded list. Always re-read it when refreshing
    // header badges, just as issues are re-read above.
    reloadPullsInBackground();
    reloadDiscussionCountInBackground();
    reloadWorkflowCountInBackground();
}

void MainWindow::loadRepoFileTree()
{
    if (!m_repoFileTree)
        return;

    const QString dir = repoGitDir();
    if (dir.isEmpty()) {
        ++m_repoFileTreeGen;
        m_repoFileTreeLoading = false;
        m_repoFileTreeReloadPending = false;
        m_repoFileTree->clear();
        new QTreeWidgetItem(m_repoFileTree,
                            {"No local copy of this repository to browse."});
        return;
    }
    if (m_repoFileTreeLoading) {
        m_repoFileTreeReloadPending = true;
        return;
    }
    const QString ref = currentRef();
    const QString recoverBase =
        m_repoBranch.isEmpty() ? QString() : browsedRefRecoveryBase();
    const bool workingTree = repoHasWorkingTree();
    const quint64 generation = ++m_repoFileTreeGen;
    m_repoFileTreeLoading = true;
    m_repoFileTreeReloadPending = false;
    runOffThread<RepoFileTreeLoad>(
        [dir, ref, recoverBase, workingTree] {
            return readRepoFileTree(dir, ref, recoverBase, workingTree);
        },
        [this, generation, dir, ref](RepoFileTreeLoad load) {
            if (generation != m_repoFileTreeGen)
                return;
            m_repoFileTreeLoading = false;
            const bool reload = std::exchange(m_repoFileTreeReloadPending, false);
            if (reload || (dir == repoGitDir() && ref != currentRef())) {
                loadRepoFileTree();
                return;
            }
            if (dir != repoGitDir())
                return;
            const BrowsedRefRecovery &recovery = load.recovery;
            if (!recovery.gone.isEmpty() && !recovery.goneResolves &&
                !applyBrowsedRefRecovery(recovery)) {
                loadRepoFileTree();
                return;
            }
            applyRepoFileTree(dir, load);
        });
}

MainWindow::RepoFileTreeLoad
MainWindow::readRepoFileTree(const QString &dir, const QString &ref,
                             const QString &recoverBase, bool workingTree)
{
    RepoFileTreeLoad load;
    load.ref = ref;
    QString err;
    if (!runGitCapture(dir, {"ls-tree", "-r", "-l", "-z", ref}, &load.tree, &err)) {
        bool listed = false;
        if (ref != QLatin1String("HEAD")) {
            load.recovery = readBrowsedRefRecovery(dir, ref, recoverBase);
            if (!load.recovery.goneResolves) {
                const QString fallback = load.recovery.baseResolves
                                             ? load.recovery.base
                                             : QStringLiteral("HEAD");
                load.tree.clear();
                listed = runGitCapture(dir, {"ls-tree", "-r", "-l", "-z", fallback},
                                       &load.tree, &err);
                if (listed)
                    load.ref = fallback;
            }
        }
        if (!listed) {
            const bool noCommits = !gitRefResolves(dir, QStringLiteral("HEAD"));
            load.treeError = err.isEmpty() || noCommits
                                 ? QStringLiteral("This repository has no commits yet.")
                                 : QStringLiteral("Could not read files: ") + err.left(120);
            load.tree.clear();
        }
    }
    if (workingTree)
        load.othersOk = runGitCapture(dir,
                                      {"ls-files", "-z", "--others", "--directory",
                                       "--no-empty-directory"},
                                      &load.others, nullptr);
    return load;
}

void MainWindow::applyRepoFileTree(const QString &dir, const RepoFileTreeLoad &load)
{
    if (!m_repoFileTree)
        return;
    const QSet<QString> expanded = fileTreeExpandedPaths(m_repoFileTree);
    const int scrollValue = fileTreeScrollOffset(m_repoFileTree);
    m_repoFileTree->clear();
    const QByteArray &out = load.tree;
    const QString &treeError = load.treeError;

    QStringList paths;
    QHash<QString, qint64> fileSize; // path -> blob size in bytes
    QHash<QString, qint64> dirSize;  // accumulated folder path -> total bytes
    auto addFile = [&](const QString &path, qint64 size) {
        paths << path;
        fileSize.insert(path, size);
        const QStringList parts = path.split('/', Qt::SkipEmptyParts);
        QString acc;
        for (int i = 0; i + 1 < parts.size(); ++i) {
            acc = acc.isEmpty() ? parts.at(i) : acc + "/" + parts.at(i);
            dirSize[acc] += size;
        }
    };
    for (const QByteArray &record : out.split('\0')) {
        if (record.isEmpty())
            continue;
        const int tab = record.indexOf('\t');
        if (tab < 0)
            continue;
        const QString path = QString::fromUtf8(record.mid(tab + 1));
        const QList<QByteArray> meta = record.left(tab).simplified().split(' ');
        addFile(path,
                meta.size() >= 4 ? QString::fromUtf8(meta.at(3)).toLongLong() : 0);
    }

    // Everything else in the checkout. ls-tree only knows the commit, so an
    // untracked or ignored file — app/.env.production, a local config, a build
    // artefact — was missing from this tree entirely, with no way to reach it
    // from the Files section at all. This is a file browser, not a commit
    // viewer: list them. --directory collapses a wholly-untracked directory
    // into one row, which is what keeps an ignored node_modules from adding
    // tens of thousands of rows; those rows fill in from disk when expanded.
    QStringList diskDirs;
    QSet<QString> untracked;
    const QByteArray &others = load.others;
    if (load.othersOk) {
        for (const QByteArray &record : others.split('\0')) {
            if (record.isEmpty())
                continue;
            QString path = QString::fromUtf8(record);
            if (path.endsWith(QLatin1Char('/'))) {
                path.chop(1);
                if (!path.isEmpty())
                    diskDirs << path;
                continue;
            }
            if (fileSize.contains(path))
                continue;
            untracked.insert(path);
            addFile(path, QFileInfo(QDir(dir).filePath(path)).size());
        }
    }
    paths.sort(Qt::CaseInsensitive);

    auto setSize = [](QTreeWidgetItem *item, qint64 bytes) {
        setFileTreeSizeCell(item, 1, formatByteSize(bytes));
    };

    QHash<QString, QTreeWidgetItem *> dirs; // accumulated path -> directory node
    std::function<QTreeWidgetItem *(const QString &)> ensureDir =
        [&](const QString &path) -> QTreeWidgetItem * {
        if (path.isEmpty())
            return nullptr;
        if (QTreeWidgetItem *existing = dirs.value(path))
            return existing;
        QTreeWidgetItem *parent = ensureDir(path.section('/', 0, -2));
        auto *node = parent ? new QTreeWidgetItem(parent)
                            : new QTreeWidgetItem(m_repoFileTree);
        node->setText(0, path.section('/', -1));
        node->setIcon(0, iconForDir(false));
        node->setData(0, Qt::UserRole, path);
        node->setData(0, Qt::UserRole + 1, true);
        setSize(node, dirSize.value(path));
        dirs.insert(path, node);
        return node;
    };

    for (const QString &path : std::as_const(paths)) {
        QTreeWidgetItem *parent = ensureDir(path.section('/', 0, -2));
        auto *item = parent ? new QTreeWidgetItem(parent)
                            : new QTreeWidgetItem(m_repoFileTree);
        const QString name = path.section('/', -1);
        item->setText(0, name);
        item->setIcon(0, iconForFile(name));
        item->setData(0, Qt::UserRole, path);
        item->setData(0, Qt::UserRole + 1, false);
        setSize(item, fileSize.value(path));
        if (untracked.contains(path)) {
            item->setToolTip(0, QStringLiteral("%1 — in the working tree, not in %2")
                                    .arg(path, currentRef()));
        }
    }
    for (const QString &path : std::as_const(diskDirs)) {
        if (dirs.contains(path))
            continue; // already built as a parent of something listed above
        QTreeWidgetItem *node = ensureDir(path);
        if (!node)
            continue;
        markFileTreeDiskDir(node);
        // The commit knows nothing about what is in here, and reading it off
        // disk is the expansion's job — leave the total blank rather than
        // claiming 0 B for a directory holding a gigabyte of build output.
        setFileTreeSizeCell(node, 1, QString());
        node->setToolTip(0, QStringLiteral("%1 — in the working tree, not in %2")
                                .arg(path, currentRef()));
    }

    std::function<void(QTreeWidgetItem *)> sortChildren = [&](QTreeWidgetItem *parent) {
        const auto kids = parent->takeChildren();
        QList<QTreeWidgetItem *> sorted = kids;
        std::sort(sorted.begin(), sorted.end(),
                  [](QTreeWidgetItem *a, QTreeWidgetItem *b) {
                      const bool ad = a->data(0, Qt::UserRole + 1).toBool();
                      const bool bd = b->data(0, Qt::UserRole + 1).toBool();
                      if (ad != bd)
                          return ad;
                      return a->text(0).toLower() < b->text(0).toLower();
                  });
        for (QTreeWidgetItem *child : std::as_const(sorted)) {
            parent->addChild(child);
            sortChildren(child);
        }
    };
    sortChildren(m_repoFileTree->invisibleRootItem());

    if (paths.isEmpty() && diskDirs.isEmpty()) {
        new QTreeWidgetItem(m_repoFileTree,
                            {treeError.isEmpty() ? QStringLiteral("(empty repository)")
                                                 : treeError});
    }

    restoreFileTreeExpandedPaths(m_repoFileTree, expanded, scrollValue);
}

static bool repoRelPathIsSafe(const QString &cleanPath)
{
    return !cleanPath.isEmpty() && cleanPath != QLatin1String(".") &&
           !cleanPath.startsWith("../") && !cleanPath.contains("/../") &&
           !QDir::isAbsolutePath(cleanPath) && cleanPath != QLatin1String(".git") &&
           !cleanPath.startsWith(".git/");
}

void MainWindow::showRepoFileTreeMenu(const QPoint &pos)
{
    if (!m_repoFileTree)
        return;
    QTreeWidgetItem *item = m_repoFileTree->itemAt(pos);
    const QString path = item ? item->data(0, Qt::UserRole).toString() : QString();
    const bool isDir = item && item->data(0, Qt::UserRole + 1).toBool();
    const bool isEntry = item && !path.isEmpty();
    const bool canWrite = repoHasWorkingTree();
    const QString parentDir =
        !isEntry ? QString() : (isDir ? path : path.section('/', 0, -2));

    QMenu menu(this);
    QAction *open = (isEntry && !isDir) ? menu.addAction(QStringLiteral("Open"))
                                        : nullptr;

    QAction *newFile = nullptr;
    QAction *newFolder = nullptr;
    QAction *rename = nullptr;
    QAction *del = nullptr;
    if (canWrite) {
        if (open)
            menu.addSeparator();
        newFile = menu.addAction(QString::fromUtf8("New file\xE2\x80\xA6"));
        newFolder = menu.addAction(QString::fromUtf8("New folder\xE2\x80\xA6"));
        if (isEntry) {
            menu.addSeparator();
            rename = menu.addAction(QString::fromUtf8("Rename\xE2\x80\xA6"));
            del = menu.addAction(QStringLiteral("Delete"));
        }
    }

    QAction *copyRel = nullptr;
    QAction *copyAbs = nullptr;
    QAction *reveal = nullptr;
    if (isEntry) {
        menu.addSeparator();
        copyRel = menu.addAction(QStringLiteral("Copy relative path"));
        if (canWrite) {
            copyAbs = menu.addAction(QStringLiteral("Copy full path"));
            reveal = menu.addAction(isDir ? QStringLiteral("Reveal folder")
                                          : QStringLiteral("Reveal in file manager"));
        }
    }

    if (menu.isEmpty())
        return;
    QAction *chosen = menu.exec(m_repoFileTree->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == open)
        openRepoFile(path);
    else if (chosen == newFile)
        newRepoFileEntry(parentDir, false);
    else if (chosen == newFolder)
        newRepoFileEntry(parentDir, true);
    else if (chosen == rename)
        renameRepoFileEntry(path, isDir);
    else if (chosen == del)
        deleteRepoFileEntry(path, isDir);
    else if (chosen == copyRel)
        QApplication::clipboard()->setText(path);
    else if (chosen == copyAbs)
        QApplication::clipboard()->setText(QDir(repoGitDir()).filePath(path));
    else if (chosen == reveal)
        revealInDesktopFileManager(QDir(repoGitDir()).filePath(path));
}

QString MainWindow::repoFileOpBranch()
{
    if (!repoHasWorkingTree()) {
        setRepoDetailNotice(
            "File operations need a local working copy of this repository.", true);
        return {};
    }
    if (m_repoFileOpRunning)
        return {};
    const QString branch = repoDefaultBranch(repoBranches());
    if (branch.isEmpty()) {
        setRepoDetailNotice("This repository has no branch to commit onto.", true);
        return {};
    }
    return branch;
}

std::pair<bool, QString> MainWindow::runRepoFileOp(const QString &dir,
                                                   const QString &branch,
                                                   const RepoFileOpRequest &op)
{
    QByteArray status;
    QString err;
    if (!runGitCapture(dir, {"status", "--porcelain"}, &status, &err) ||
        !status.trimmed().isEmpty())
        return {false, err.isEmpty()
                           ? QStringLiteral("Commit or stash local changes before "
                                            "changing files.")
                           : err.left(240)};
    if (!runGitCapture(dir, {"checkout", branch}, nullptr, &err))
        return {false, QStringLiteral("Could not check out %1: %2")
                           .arg(branch, err.left(200))};
    if (op.kind == RepoFileOpRequest::Create) {
        const QString full = QDir(dir).filePath(op.trackRel);
        if (QFileInfo::exists(full))
            return {false, QStringLiteral("%1 already exists.").arg(op.path)};
        QDir().mkpath(QFileInfo(full).absolutePath());
        QFile file(full);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return {false, QStringLiteral("Could not create %1.").arg(op.path)};
        file.close();
        if (!runGitCapture(dir, {"add", "--", op.trackRel}, nullptr, &err) ||
            !runGitCapture(dir, {"commit", "-m", QStringLiteral("Add %1").arg(op.path)},
                           nullptr, &err)) {
            file.remove();
            runGitCapture(dir, {"reset", "--hard"}, nullptr, nullptr);
            return {false, err.isEmpty() ? QStringLiteral("Could not commit the new file.")
                                         : err.left(240)};
        }
        return {true, QString()};
    }
    if (op.kind == RepoFileOpRequest::Rename) {
        if (QFileInfo::exists(QDir(dir).filePath(op.dest)))
            return {false, QStringLiteral("%1 already exists.").arg(op.dest)};
        if (!runGitCapture(dir, {"mv", "--", op.path, op.dest}, nullptr, &err) ||
            !runGitCapture(dir,
                           {"commit", "-m",
                            QStringLiteral("Rename %1 to %2").arg(op.path, op.dest)},
                           nullptr, &err)) {
            runGitCapture(dir, {"reset", "--hard"}, nullptr, nullptr);
            return {false, err.isEmpty() ? QStringLiteral("Could not rename.")
                                         : err.left(240)};
        }
        return {true, QString()};
    }
    QStringList args = {"rm", "-q"};
    if (op.isDir)
        args << "-r";
    args << "--" << op.path;
    if (!runGitCapture(dir, args, nullptr, &err) ||
        !runGitCapture(dir, {"commit", "-m", QStringLiteral("Delete %1").arg(op.path)},
                       nullptr, &err)) {
        runGitCapture(dir, {"reset", "--hard"}, nullptr, nullptr);
        return {false, err.isEmpty() ? QStringLiteral("Could not delete.")
                                     : err.left(240)};
    }
    return {true, QString()};
}

void MainWindow::startRepoFileOp(const QString &branch, const RepoFileOpRequest &op)
{
    const QString dir = repoGitDir();
    if (dir.isEmpty() || branch.isEmpty() || m_repoFileOpRunning)
        return;
    m_repoFileOpRunning = true;
    runOffThread<std::pair<bool, QString>>(
        [dir, branch, op] { return runRepoFileOp(dir, branch, op); },
        [this, dir, branch, op](std::pair<bool, QString> done) {
            m_repoFileOpRunning = false;
            const bool current = dir == repoGitDir();
            if (!done.first) {
                if (current)
                    setRepoDetailNotice(done.second, true);
                else
                    logSystem(QStringLiteral("File operation in %1 failed: %2")
                                  .arg(dir, done.second));
                return;
            }
            if (op.kind == RepoFileOpRequest::Create)
                logSystem(QStringLiteral("Created %1 in %2.").arg(op.path, branch));
            else if (op.kind == RepoFileOpRequest::Rename)
                logSystem(QStringLiteral("Renamed %1 to %2.").arg(op.path, op.dest));
            else
                logSystem(QStringLiteral("Deleted %1 from %2.").arg(op.path, branch));
            if (!current)
                return;
            if (op.kind == RepoFileOpRequest::Create) {
                setRepoDetailNotice(QStringLiteral("Created %1.").arg(op.path));
            } else if (op.kind == RepoFileOpRequest::Rename) {
                closeRepoFileTabsUnder(op.path, op.isDir);
                setRepoDetailNotice(QStringLiteral("Renamed to %1.").arg(op.dest));
            } else {
                closeRepoFileTabsUnder(op.path, op.isDir);
                setRepoDetailNotice(QStringLiteral("Deleted %1.").arg(op.path));
            }
            finishRepoFileOp(branch);
            if (op.kind == RepoFileOpRequest::Create && !op.isDir)
                openRepoFile(op.path);
        });
}

void MainWindow::finishRepoFileOp(const QString &base)
{
    setRepoBranch(base);
    const int filesPage = m_filesStack ? m_filesStack->currentIndex() : 0;
    refreshOpenRepoDetail();
    loadRepoFileTree();
    m_treeLoadedForIndex = m_repoDetailIndex;
    if (m_filesStack)
        m_filesStack->setCurrentIndex(filesPage);
}

void MainWindow::newRepoFileEntry(const QString &parentDir, bool folder)
{
    bool ok = false;
    const QString label = folder ? QStringLiteral("New folder")
                                  : QStringLiteral("New file");
    const QString name =
        QInputDialog::getText(this, label,
                              folder ? QStringLiteral("Folder name:")
                                     : QStringLiteral("File name:"),
                              QLineEdit::Normal, QString(), &ok)
            .trimmed();
    if (!ok || name.isEmpty())
        return;
    const QString rel =
        QDir::cleanPath(parentDir.isEmpty() ? name : parentDir + "/" + name);
    if (!repoRelPathIsSafe(rel)) {
        setRepoDetailNotice("Refusing to create that path.", true);
        return;
    }
    const QString branch = repoFileOpBranch();
    if (branch.isEmpty())
        return;
    RepoFileOpRequest op;
    op.kind = RepoFileOpRequest::Create;
    op.path = rel;
    op.trackRel = folder ? rel + "/.gitkeep" : rel;
    op.isDir = folder;
    startRepoFileOp(branch, op);
}

void MainWindow::renameRepoFileEntry(const QString &path, bool isDir)
{
    bool ok = false;
    const QString oldName = path.section('/', -1);
    const QString newName =
        QInputDialog::getText(this, QStringLiteral("Rename"),
                              QStringLiteral("New name for \"%1\":").arg(oldName),
                              QLineEdit::Normal, oldName, &ok)
            .trimmed();
    if (!ok || newName.isEmpty() || newName == oldName)
        return;
    if (newName.contains('/')) {
        setRepoDetailNotice("Enter a name, not a path.", true);
        return;
    }
    const QString parent = path.section('/', 0, -2);
    const QString dest =
        QDir::cleanPath(parent.isEmpty() ? newName : parent + "/" + newName);
    if (!repoRelPathIsSafe(dest)) {
        setRepoDetailNotice("Refusing to rename to that path.", true);
        return;
    }
    const QString branch = repoFileOpBranch();
    if (branch.isEmpty())
        return;
    RepoFileOpRequest op;
    op.kind = RepoFileOpRequest::Rename;
    op.path = path;
    op.dest = dest;
    op.isDir = isDir;
    startRepoFileOp(branch, op);
}

void MainWindow::deleteRepoFileEntry(const QString &path, bool isDir)
{
    if (QMessageBox::question(
            this, QStringLiteral("Delete"),
            QStringLiteral("Delete %1 \"%2\" from the repository? The removal is "
                           "committed to the default branch.")
                .arg(isDir ? QStringLiteral("folder") : QStringLiteral("file"),
                     path),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    const QString branch = repoFileOpBranch();
    if (branch.isEmpty())
        return;
    RepoFileOpRequest op;
    op.kind = RepoFileOpRequest::Delete;
    op.path = path;
    op.isDir = isDir;
    startRepoFileOp(branch, op);
}

void MainWindow::closeRepoFileTabsUnder(const QString &path, bool isDir)
{
    if (!m_repoFileTabs)
        return;
    const QString prefix = path + "/";
    const QList<QString> openPaths = m_openFileTabs.keys();
    for (const QString &p : openPaths) {
        if (p != path && !(isDir && p.startsWith(prefix)))
            continue;
        QWidget *w = m_openFileTabs.value(p);
        const int idx = w ? m_repoFileTabs->indexOf(w) : -1;
        if (idx >= 0)
            m_repoFileTabs->removeTab(idx);
        m_openFileTabs.remove(p);
        if (w)
            w->deleteLater();
    }
    updateRepoFileSaveActions();
}

void MainWindow::openRepoFile(const QString &path, std::function<void()> opened)
{
    if (path.isEmpty())
        return;
    // A cove is encrypted; open it in the cove viewer (unlocking as needed) rather
    // than dumping ciphertext into the code editor.
    if (path.endsWith(QStringLiteral(".cove"), Qt::CaseInsensitive) &&
        path.startsWith(CoveStore::covesDirRel() + "/")) {
        openCove(path);
        return;
    }
    if (QDir::isAbsolutePath(path)) {
        openLocalFileTab(path);
        return;
    }
    ensureSectionBuilt(kFilesSectionIndex);
    if (!m_repoFileTabs)
        return;
    showFilesRepoTree();

    if (m_openFileTabs.contains(path)) {
        m_repoFileTabs->setCurrentWidget(m_openFileTabs.value(path));
        showSection(kFilesSectionIndex);
        if (opened)
            QTimer::singleShot(0, this, std::move(opened));
        return;
    }
    const QString dir = repoGitDir();
    if (dir.isEmpty()) {
        showRepoFileContent(dir, path, GitCaptureResult{});
        if (opened)
            QTimer::singleShot(0, this, std::move(opened));
        return;
    }
    const QString ref = currentRef();
    const QString waitKey =
        dir + QLatin1Char('\x1f') + ref + QLatin1Char('\x1f') + path;
    const bool alreadyReading = m_repoFileOpenWaiters.contains(waitKey);
    QList<std::function<void()>> &waiters = m_repoFileOpenWaiters[waitKey];
    if (opened)
        waiters.append(std::move(opened));
    if (alreadyReading)
        return;
    runOffThread<GitCaptureResult>(
        [dir, ref, path] { return readGit(dir, {"show", ref + ":" + path}); },
        [this, dir, ref, path, waitKey](GitCaptureResult read) {
            const QList<std::function<void()>> callbacks =
                m_repoFileOpenWaiters.take(waitKey);
            if (dir != repoGitDir())
                return;
            if (ref != currentRef()) {
                for (const std::function<void()> &callback : callbacks)
                    openRepoFile(path, callback);
                if (callbacks.isEmpty())
                    openRepoFile(path);
                return;
            }
            showRepoFileContent(dir, path, read);
            for (const std::function<void()> &callback : callbacks)
                QTimer::singleShot(0, this, callback);
        });
}

void MainWindow::showRepoFileContent(const QString &dir, const QString &path,
                                     const GitCaptureResult &read)
{
    if (!m_repoFileTabs)
        return;
    if (m_openFileTabs.contains(path)) {
        m_repoFileTabs->setCurrentWidget(m_openFileTabs.value(path));
        showSection(kFilesSectionIndex);
        return;
    }
    const QByteArray &out = read.output;
    QString content;
    bool editable = false;
    bool readable = false;
    if (dir.isEmpty() || !read.ok) {
        if (!dir.isEmpty() && QFileInfo(QDir(dir).filePath(path)).isFile()) {
            openWorkingTreeFile(dir, path);
            return;
        }
        content = "Could not read file: " + read.error.left(200);
    } else if (out.size() > kEditableFileByteLimit) {
        content = QStringLiteral("File is too large to preview (%1 KB).")
                      .arg(out.size() / 1024);
    } else if (out.contains('\0')) {
        content = QString::fromUtf8("Binary file (%1 bytes) \xE2\x80\x94 not shown.")
                      .arg(out.size());
    } else {
        content = QString::fromUtf8(out);
        readable = true;
        editable = repoHasWorkingTree() || repoCanProposePull();
        if (path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) ||
            path.endsWith(QStringLiteral(".jsonc"), Qt::CaseInsensitive)) {
            const QJsonDocument doc = QJsonDocument::fromJson(out);
            if (!doc.isNull())
                content = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
        }
    }

    auto *editor = new CodePreviewEditor(path);
    editor->setReadOnly(!editable);
    editor->setPlainText(content);
    editor->document()->setModified(false);
    new CodePreviewHighlighter(editor->document(), path);
    wireFilesEditor(editor);
    configureRepoMarkdownPreview(editor, path);

    const QString name = path.section('/', -1);
    const int index = m_repoFileTabs->addTab(editor, iconForFile(name), name);
    m_repoFileTabs->setTabToolTip(index, path);
    m_repoFileTabs->setCurrentIndex(index);
    m_openFileTabs.insert(path, editor);
    // A README reads as a document first, as it does on the web; the Preview
    // button flips to the source.
    if (readable && editor->markdownRenderer)
        editor->setMarkdownPreviewVisible(true);
    connect(editor->document(), &QTextDocument::modificationChanged, this,
            [this, editor, name](bool modified) {
                const int idx = m_repoFileTabs ? m_repoFileTabs->indexOf(editor) : -1;
                if (idx >= 0)
                    m_repoFileTabs->setTabText(
                        idx, modified ? QString::fromUtf8("\xE2\x97\x8F ") + name : name);
            });
    updateRepoFileSaveActions();
    showSection(kFilesSectionIndex);
}

void MainWindow::openWorkingTreeFile(const QString &dir, const QString &relPath)
{
    if (dir.isEmpty() || relPath.isEmpty() || !m_repoFileTabs)
        return;
    const QString abs = QDir(dir).absoluteFilePath(relPath);
    ensureSectionBuilt(kFilesSectionIndex);
    if (!m_repoFileTabs)
        return;
    showFilesRepoTree();

    const QString tabKey = worktreeTabKey(abs);
    if (m_openFileTabs.contains(tabKey)) {
        m_repoFileTabs->setCurrentWidget(m_openFileTabs.value(tabKey));
        showSection(kFilesSectionIndex);
        return;
    }

    QString content;
    bool editable = false;
    QFile file(abs);
    const qint64 size = QFileInfo(abs).size();
    if (!file.open(QIODevice::ReadOnly)) {
        content = QStringLiteral("Could not read %1.").arg(abs);
    } else {
        const QByteArray bytes = file.readAll();
        file.close();
        if (bytes.size() > kEditableFileByteLimit)
            content = QStringLiteral("File is too large to edit (%1 KB).")
                          .arg(size / 1024);
        else if (bytes.contains('\0'))
            content = QString::fromUtf8("Binary file (%1 bytes) \xE2\x80\x94 not shown.")
                          .arg(size);
        else {
            content = QString::fromUtf8(bytes);
            editable = true;
        }
    }

    auto *editor = new CodePreviewEditor(relPath);
    editor->setProperty("worktreeFile", editable ? abs : QString());
    editor->setReadOnly(!editable);
    editor->setPlainText(content);
    editor->document()->setModified(false);
    new CodePreviewHighlighter(editor->document(), relPath);
    wireFilesEditor(editor);
    configureRepoMarkdownPreview(editor, relPath);

    const QString name = relPath.section('/', -1);
    const int index = m_repoFileTabs->addTab(editor, iconForFile(name), name);
    m_repoFileTabs->setTabToolTip(index, abs);
    m_repoFileTabs->setCurrentIndex(index);
    m_openFileTabs.insert(tabKey, editor);
    if (editable && editor->markdownRenderer)
        editor->setMarkdownPreviewVisible(true);
    connect(editor->document(), &QTextDocument::modificationChanged, this,
            [this, editor, name](bool modified) {
                const int idx = m_repoFileTabs ? m_repoFileTabs->indexOf(editor) : -1;
                if (idx >= 0)
                    m_repoFileTabs->setTabText(
                        idx, modified ? QString::fromUtf8("\xE2\x97\x8F ") + name : name);
            });
    updateRepoFileSaveActions();
    showSection(kFilesSectionIndex);
}

bool MainWindow::saveWorkingTreeFileEdit(const QString &absPath,
                                         const QString &content)
{
    QFile file(absPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setRepoDetailNotice(QStringLiteral("Could not write %1.").arg(absPath), true);
        return false;
    }
    const QByteArray bytes = content.toUtf8();
    const bool wrote = file.write(bytes) == bytes.size();
    file.close();
    if (!wrote) {
        setRepoDetailNotice(QStringLiteral("Could not write %1.").arg(absPath), true);
        return false;
    }
    setRepoDetailNotice(
        QStringLiteral("Saved %1.").arg(QFileInfo(absPath).fileName()));
    refreshSourceControl(true);
    return true;
}

void MainWindow::updateRepoFileSaveActions()
{
    const QWidget *w = m_repoFileTabs ? m_repoFileTabs->currentWidget() : nullptr;
    const auto *editor = qobject_cast<const QPlainTextEdit *>(w);
    const QString path = w ? w->property("previewPath").toString() : QString();
    const QString covePath =
        w ? w->property("coveDocPath").toString() : QString();
    const bool coveFile =
        w && !w->property("coveId").toString().isEmpty() && !covePath.isEmpty();
    const bool network = w && !w->property("networkFile").toString().isEmpty();
    const bool anyFile = editor && !path.isEmpty();
    const bool haveFile = anyFile && !QDir::isAbsolutePath(path) && !network;
    // An absolute path belongs to the filesystem explorer rather than the
    // repository tree, but it can still be a writable on-disk text file.
    const bool editable = editor && !editor->isReadOnly();
    const bool onDisk = w && (!w->property("worktreeFile").toString().isEmpty() || network);
    const bool coveEditable = coveFile && !coveEditorIsReadOnly(w);
    const bool coveDirectSave =
        coveFile && coveStoreForRepo(m_repoDetailIndex).canWrite();
    const bool coveProposesPull =
        coveEditable && !coveDirectSave && repoCanProposePull();
    if (m_repoFileSaveButton) {
        m_repoFileSaveButton->setVisible(onDisk || coveFile);
        m_repoFileSaveButton->setEnabled(coveEditable || (onDisk && editable));
        m_repoFileSaveButton->setText(coveProposesPull
                                          ? QStringLiteral("Save as PR")
                                          : QStringLiteral("Save"));
        m_repoFileSaveButton->setToolTip(
            coveProposesPull
                ? QStringLiteral("Propose this encrypted Cove edit to the "
                                 "repository owner")
                : coveFile
                      ? QStringLiteral("Save this encrypted Cove document")
                      : network
                            ? QStringLiteral("Write this file back to the network share")
                            : QStringLiteral("Write this file back to the working tree"));
    }
    if (m_repoFileCommitButton)
        m_repoFileCommitButton->setVisible(!onDisk && !coveFile);
    if (m_repoFilePullButton)
        m_repoFilePullButton->setVisible(!onDisk && !coveFile);
    if (m_repoFilePreviewButton) {
        const QString previewPath = coveFile ? covePath : path;
        const bool isMarkdown =
            (anyFile || coveFile) &&
            previewSyntaxForPath(previewPath) == PreviewSyntax::Markdown;
        const auto *preview = dynamic_cast<const CodePreviewEditor *>(w);
        m_repoFilePreviewButton->setEnabled(isMarkdown);
        m_repoFilePreviewButton->setVisible(isMarkdown);
        m_repoFilePreviewButton->setChecked(preview && preview->markdownPreviewVisible());
    }
    if (m_repoFileHistoryButton) {
        m_repoFileHistoryButton->setVisible(!coveFile);
        m_repoFileHistoryButton->setEnabled(haveFile);
    }
    if (m_repoFileCommitButton)
        m_repoFileCommitButton->setEnabled(!onDisk && editable && repoHasWorkingTree());
    if (m_repoFilePullButton)
        m_repoFilePullButton->setEnabled(
            !onDisk && editable && (repoHasWorkingTree() || repoCanProposePull()));
    for (QWidget *repoControl :
         {static_cast<QWidget *>(m_filesCommitMessage),
          static_cast<QWidget *>(m_filesCommitButton),
          static_cast<QWidget *>(m_filesPushButton)}) {
        if (repoControl)
            repoControl->setVisible(!coveFile);
    }
}

void MainWindow::saveCurrentRepoFile(bool createPull)
{
    QWidget *w = m_repoFileTabs ? m_repoFileTabs->currentWidget() : nullptr;
    if (w && !w->property("coveId").toString().isEmpty()) {
        saveCurrentCoveExplorerDocument();
        updateRepoFileSaveActions();
        return;
    }
    auto *editor = qobject_cast<QPlainTextEdit *>(w);
    if (!editor || editor->isReadOnly())
        return;
    const QString path = w->property("previewPath").toString();
    if (path.isEmpty())
        return;
    const QString networkFile = w->property("networkFile").toString();
    if (!networkFile.isEmpty()) {
        if (!m_networkFilesPanel)
            return;
        const QString text = editor->toPlainText();
        const QPointer<QPlainTextEdit> guard(editor);
        m_networkFilesPanel->saveText(networkFile, text, [this, guard, text](bool ok, const QString &error) {
            if (ok && guard && guard->toPlainText() == text)
                guard->document()->setModified(false);
            flashMessage(ok ? QStringLiteral("Saved to the network share.") : error, !ok);
            updateRepoFileSaveActions();
        });
        return;
    }
    const QString worktreeFile = w->property("worktreeFile").toString();
    if (!worktreeFile.isEmpty()) {
        if (saveWorkingTreeFileEdit(worktreeFile, editor->toPlainText()))
            editor->document()->setModified(false);
        updateRepoFileSaveActions();
        return;
    }
    const QString text = editor->toPlainText();
    const QPointer<QPlainTextEdit> guard(editor);
    saveRepoFileEdit(path, text, createPull, [this, guard, text](bool ok) {
        if (ok && guard && guard->toPlainText() == text)
            guard->document()->setModified(false);
        updateRepoFileSaveActions();
    });
}

QString MainWindow::coveAccountName()
{
    return hostLinkUserName();
}

bool MainWindow::coveInviteAccountValid(const QString &accountName, QString *error)
{
    const QString account = accountName.trimmed().toLower();
    if (account.isEmpty()) {
        if (error)
            *error = QStringLiteral("Enter a ForkMesh account name.");
        return false;
    }
    int status = 0;
    const QJsonObject lookup = getAccountSync(account, &status);
    if (status != 200 || !lookup.value("exists").toBool()) {
        if (error)
            *error = QStringLiteral("%1 is not a ForkMesh account.").arg(account);
        return false;
    }
    cacheAccountRecord(account, lookup);
    const QByteArray avatarBytes = QByteArray::fromBase64(
        lookup.value(QStringLiteral("avatarPng")).toString().toLatin1());
    QPixmap avatar;
    if (!avatarBytes.isEmpty() && avatar.loadFromData(avatarBytes))
        m_avatars.insert(QStringLiteral("user:") + account, avatar);
    if (lookup.value("status").toString() != QLatin1String("active")) {
        if (error)
            *error = QStringLiteral("%1 is not an active ForkMesh account.").arg(account);
        return false;
    }
    return true;
}

void MainWindow::loadCoveExplorer()
{
    if (!m_coveExplorerTree || !m_coveExplorerSelector)
        return;

    const QString keepId = m_coveExplorerCurrentId;
    m_coveExplorerCoves.clear();
    m_coveExplorerTree->clear();

    QSignalBlocker block(m_coveExplorerSelector);
    m_coveExplorerSelector->clear();

    CoveStore store = coveStoreForRepo(m_repoDetailIndex);
    const bool canCreate = store.canWrite();
    const QString account = coveAccountName();
    if (account.isEmpty()) {
        if (m_coveExplorerStatus)
            m_coveExplorerStatus->setText(
                QStringLiteral("Sign in with a ForkMesh account."));
        if (m_coveExplorerNewButton)
            m_coveExplorerNewButton->setEnabled(false);
        if (m_coveExplorerInviteButton)
            m_coveExplorerInviteButton->setEnabled(false);
        if (m_coveExplorerInviteButton)
            m_coveExplorerInviteButton->setText(QStringLiteral("Share"));
        if (m_coveExplorerSharedWithLabel)
            m_coveExplorerSharedWithLabel->setText(QStringLiteral("Shared with —"));
        if (m_coveExplorerCopyLinkButton)
            m_coveExplorerCopyLinkButton->setEnabled(false);
        if (m_coveExplorerNewFileButton)
            m_coveExplorerNewFileButton->setEnabled(false);
        if (m_coveExplorerDeleteButton)
            m_coveExplorerDeleteButton->setEnabled(false);
        new QTreeWidgetItem(m_coveExplorerTree,
                            {QStringLiteral("(sign in required)")});
        updateRepoFileSaveActions();
        return;
    }

    QString err;
    const QList<Cove> envelopes = store.listCoves(&err);
    for (Cove cove : envelopes) {
        if (!CoveStore::unlockForAccount(cove, account))
            continue;
        if (cove.name.trimmed().isEmpty())
            cove.name = QStringLiteral("Secure cove");
        m_coveExplorerCoves << cove;
    }

    int selectIndex = -1;
    for (int i = 0; i < m_coveExplorerCoves.size(); ++i) {
        const Cove &cove = m_coveExplorerCoves.at(i);
        m_coveExplorerSelector->addItem(cove.name, cove.id);
        if (!keepId.isEmpty() && cove.id == keepId)
            selectIndex = i;
    }
    if (selectIndex < 0 && !m_coveExplorerCoves.isEmpty())
        selectIndex = 0;
    if (selectIndex >= 0) {
        m_coveExplorerSelector->setCurrentIndex(selectIndex);
        m_coveExplorerCurrentId = m_coveExplorerCoves.at(selectIndex).id;
    } else {
        m_coveExplorerCurrentId.clear();
    }

    if (m_coveExplorerStatus) {
        m_coveExplorerStatus->setText(
            m_coveExplorerCoves.isEmpty()
                ? QStringLiteral("No invited account coves in this repo.")
                : QStringLiteral("%1 invited cove%2")
                      .arg(m_coveExplorerCoves.size())
                      .arg(m_coveExplorerCoves.size() == 1 ? QString() : QStringLiteral("s")));
    }
    if (m_coveExplorerNewButton)
        m_coveExplorerNewButton->setEnabled(canCreate);
    refreshCoveExplorerTree();
}

void MainWindow::refreshCoveExplorerTree()
{
    if (!m_coveExplorerTree)
        return;
    m_coveExplorerTree->clear();

    int coveIndex = -1;
    for (int i = 0; i < m_coveExplorerCoves.size(); ++i) {
        if (m_coveExplorerCoves.at(i).id == m_coveExplorerCurrentId) {
            coveIndex = i;
            break;
        }
    }
    const QString account = coveAccountName();
    const bool haveCove = coveIndex >= 0;
    const bool canWrite =
        haveCove && coveStoreForRepo(m_repoDetailIndex).canWrite();
    const bool canPropose = haveCove && !canWrite && repoCanProposePull();
    const bool canEdit = canWrite || canPropose;
    const bool isOwner =
        haveCove &&
        m_coveExplorerCoves.at(coveIndex).creatorAccount.compare(account, Qt::CaseInsensitive) == 0;
    if (m_coveExplorerInviteButton)
        m_coveExplorerInviteButton->setEnabled(canWrite && isOwner);
    if (m_coveExplorerNewFileButton)
        m_coveExplorerNewFileButton->setEnabled(canEdit);
    if (m_coveExplorerDeleteButton)
        m_coveExplorerDeleteButton->setEnabled(canEdit);

    if (m_repoFileTabs) {
        for (int index = 0; index < m_repoFileTabs->count(); ++index) {
            QWidget *editor = m_repoFileTabs->widget(index);
            const QString path =
                editor ? editor->property("coveDocPath").toString() : QString();
            const QString name = path.section(QLatin1Char('/'), -1);
            if (!name.isEmpty()) {
                m_repoFileTabs->setTabText(
                    index, coveEditorIsModified(editor)
                               ? QString::fromUtf8("\xE2\x97\x8F ") + name
                               : name);
            }
        }
    }

    if (m_coveExplorerSharedWithLabel) {
        if (!haveCove) {
            m_coveExplorerSharedWithLabel->setText(QStringLiteral("Shared with —"));
            if (m_coveExplorerInviteButton)
                m_coveExplorerInviteButton->setText(QStringLiteral("Share"));
        } else {
            const Cove &sharedCove = m_coveExplorerCoves.at(coveIndex);
            QStringList people;
            const QString creator = sharedCove.creatorAccount.trimmed().toLower();
            if (!creator.isEmpty())
                people << QStringLiteral("@%1 (owner)").arg(creator);
            QStringList invitees = sharedCove.invitedAccounts;
            invitees.sort(Qt::CaseInsensitive);
            for (const QString &invitee : std::as_const(invitees)) {
                const QString normalized = invitee.trimmed().toLower();
                if (!normalized.isEmpty() && normalized != creator)
                    people << QStringLiteral("@%1").arg(normalized);
            }
            m_coveExplorerSharedWithLabel->setText(
                QStringLiteral("Shared with %1")
                    .arg(people.isEmpty() ? QStringLiteral("only you")
                                          : people.join(QStringLiteral("  ·  "))));
            m_coveExplorerSharedWithLabel->setToolTip(
                QStringLiteral("%1\nEveryone listed can edit directly from a "
                               "writable checkout or propose an encrypted edit "
                               "from a mirror.")
                    .arg(people.join(QLatin1Char('\n'))));
            if (m_coveExplorerInviteButton)
                m_coveExplorerInviteButton->setText(
                    QStringLiteral("Share · %1").arg(people.size()));
        }
    }
    if (m_coveExplorerCopyLinkButton)
        m_coveExplorerCopyLinkButton->setEnabled(haveCove);
    updateRepoFileSaveActions();

    if (!haveCove) {
        new QTreeWidgetItem(m_coveExplorerTree,
                            {QStringLiteral("(no invited coves)")});
        return;
    }

    const Cove &cove = m_coveExplorerCoves.at(coveIndex);
    if (cove.documents.isEmpty()) {
        new QTreeWidgetItem(m_coveExplorerTree,
                            {QStringLiteral("(empty cove)")});
        return;
    }

    QHash<QString, QTreeWidgetItem *> dirs;
    QTreeWidgetItem *root = m_coveExplorerTree->invisibleRootItem();
    for (const CoveDocument &doc : std::as_const(cove.documents)) {
        QString path = QDir::cleanPath(doc.name.trimmed());
        if (!coveDocPathIsSafe(path))
            path = doc.id.isEmpty() ? QStringLiteral("untitled.txt")
                                    : doc.id + QStringLiteral(".txt");
        const QStringList parts = path.split('/', Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;
        QTreeWidgetItem *parent = root;
        QString dirPath;
        for (int i = 0; i < parts.size() - 1; ++i) {
            dirPath = dirPath.isEmpty() ? parts.at(i) : dirPath + "/" + parts.at(i);
            QTreeWidgetItem *folder = dirs.value(dirPath, nullptr);
            if (!folder) {
                folder = new QTreeWidgetItem(parent, {parts.at(i), QString()});
                folder->setIcon(0, iconForDir(false));
                folder->setData(0, Qt::UserRole, dirPath);
                folder->setData(0, Qt::UserRole + 1, true);
                dirs.insert(dirPath, folder);
            }
            parent = folder;
        }
        const QString leaf = parts.last();
        const QString updated =
            doc.updatedAtMs > 0
                ? QDateTime::fromMSecsSinceEpoch(doc.updatedAtMs)
                      .toString(QStringLiteral("yyyy-MM-dd"))
                : QString();
        auto *item = new QTreeWidgetItem(parent, {leaf, updated});
        item->setIcon(0, iconForFile(leaf));
        item->setData(0, Qt::UserRole, path);
        item->setData(0, Qt::UserRole + 1, false);
        item->setToolTip(0, path);
    }
    m_coveExplorerTree->sortItems(0, Qt::AscendingOrder);
    m_coveExplorerTree->expandToDepth(0);
}

void MainWindow::syncCoveTreeToOpenTab()
{
    if (!m_repoFileTabs || !m_coveExplorerTree || !m_coveExplorerSelector)
        return;
    QWidget *editor = m_repoFileTabs->currentWidget();
    const QString coveId =
        editor ? editor->property("coveId").toString() : QString();
    const QString path =
        editor ? editor->property("coveDocPath").toString() : QString();
    if (coveId.isEmpty() || path.isEmpty())
        return;

    const int selectorIndex = m_coveExplorerSelector->findData(coveId);
    if (selectorIndex >= 0 &&
        m_coveExplorerSelector->currentIndex() != selectorIndex)
        m_coveExplorerSelector->setCurrentIndex(selectorIndex);

    std::function<QTreeWidgetItem *(QTreeWidgetItem *)> findPath =
        [&](QTreeWidgetItem *parent) -> QTreeWidgetItem * {
        if (!parent)
            return nullptr;
        for (int i = 0; i < parent->childCount(); ++i) {
            QTreeWidgetItem *child = parent->child(i);
            if (!child->data(0, Qt::UserRole + 1).toBool() &&
                child->data(0, Qt::UserRole).toString() == path)
                return child;
            if (QTreeWidgetItem *found = findPath(child))
                return found;
        }
        return nullptr;
    };
    QTreeWidgetItem *item =
        findPath(m_coveExplorerTree->invisibleRootItem());
    if (!item)
        return;
    for (QTreeWidgetItem *parent = item->parent(); parent;
         parent = parent->parent())
        parent->setExpanded(true);
    m_coveExplorerTree->setCurrentItem(item);
    m_coveExplorerTree->scrollToItem(item);
}

void MainWindow::openCoveExplorerDocument(const QString &path)
{
    if (path.isEmpty())
        return;
    ensureSectionBuilt(kFilesSectionIndex);
    if (!m_repoFileTabs)
        return;
    if (!m_filesTreeStack || m_filesTreeStack->currentIndex() != 2)
        showFilesCoveTree();
    showSection(kFilesSectionIndex);

    int coveIndex = -1;
    for (int i = 0; i < m_coveExplorerCoves.size(); ++i) {
        if (m_coveExplorerCoves.at(i).id == m_coveExplorerCurrentId) {
            coveIndex = i;
            break;
        }
    }
    if (coveIndex < 0)
        return;
    Cove &cove = m_coveExplorerCoves[coveIndex];
    int docIndex = -1;
    for (int i = 0; i < cove.documents.size(); ++i) {
        if (QDir::cleanPath(cove.documents.at(i).name) == path) {
            docIndex = i;
            break;
        }
    }
    if (docIndex < 0)
        return;

    const QString key = coveDocTabKey(cove.id, path);
    if (m_openCoveExplorerTabs.contains(key)) {
        m_repoFileTabs->setCurrentWidget(m_openCoveExplorerTabs.value(key));
        syncCoveTreeToOpenTab();
        return;
    }

    const CoveDocument &doc = cove.documents.at(docIndex);
    const bool directSave = coveStoreForRepo(m_repoDetailIndex).canWrite();
    const bool readOnly = !directSave && !repoCanProposePull();
    QWidget *editor = nullptr;
    const forkmesh::cove::EditorKind kind =
        forkmesh::cove::editorKindForFile(path, doc.mime);
    if (kind == forkmesh::cove::EditorKind::Spreadsheet) {
        auto *spreadsheet =
            new forkmesh::cove::SpreadsheetEditor(doc.body, readOnly);
        spreadsheet->setChangeHandler(
            [this](bool) { refreshCoveExplorerTree(); });
        editor = spreadsheet;
    } else if (kind == forkmesh::cove::EditorKind::Pipeline) {
        QJsonObject parsed;
        QString pipelineError;
        if (forkmesh::cove::parsePipeline(doc.body, &parsed, &pipelineError)) {
            auto *pipeline = new forkmesh::cove::PipelineEditor(
                doc.body, coveAccountName(), readOnly);
            pipeline->setChangeHandler(
                [this](bool) { refreshCoveExplorerTree(); });
            editor = pipeline;
        } else {
            setRepoDetailNotice(
                QStringLiteral("Pipeline view unavailable: %1. Opened as JSON so it can be repaired.")
                    .arg(pipelineError),
                true);
        }
    }
    if (!editor) {
        auto *textEditor = new CodePreviewEditor(path);
        textEditor->setReadOnly(readOnly);
        textEditor->setPlainText(doc.body);
        textEditor->document()->setModified(false);
        new CodePreviewHighlighter(textEditor->document(), path);
        wireFilesEditor(textEditor);
        connect(textEditor->document(), &QTextDocument::modificationChanged, this,
                [this](bool) { refreshCoveExplorerTree(); });
        editor = textEditor;
    }
    editor->setProperty("coveId", cove.id);
    editor->setProperty("coveDocPath", path);
    editor->setProperty("coveName", cove.name);
    editor->setProperty("coveMime", doc.mime);

    const QString name = path.section('/', -1);
    const int index = m_repoFileTabs->addTab(editor, iconForFile(name), name);
    m_repoFileTabs->setTabToolTip(
        index, QStringLiteral("Cove: %1\n%2").arg(cove.name, path));
    m_repoFileTabs->setCurrentIndex(index);
    m_openCoveExplorerTabs.insert(key, editor);
    refreshCoveExplorerTree();
    updateFilesEditorStatus();
    syncCoveTreeToOpenTab();
}

void MainWindow::saveCurrentCoveExplorerDocument()
{
    QWidget *w = m_repoFileTabs ? m_repoFileTabs->currentWidget() : nullptr;
    if (!w || coveEditorIsReadOnly(w))
        return;
    const QString coveId = w->property("coveId").toString();
    const QString path = w->property("coveDocPath").toString();
    if (coveId.isEmpty() || path.isEmpty())
        return;

    for (Cove &savedCove : m_coveExplorerCoves) {
        if (savedCove.id != coveId)
            continue;
        Cove updated = savedCove;
        for (CoveDocument &doc : updated.documents) {
            if (QDir::cleanPath(doc.name) != path)
                continue;
            doc.body = coveEditorBody(w);
            doc.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
            CoveAccessEntry entry;
            entry.who = m_profileIdentity.publicKey();
            entry.name = coveAccountName();
            entry.ts = doc.updatedAtMs;
            entry.action = QStringLiteral("edit");
            CoveStore::appendAccess(updated, entry);

            QString err;
            if (!persistCoveExplorerChange(savedCove, updated, &err)) {
                if (!err.isEmpty()) {
                    QMessageBox::warning(this, QStringLiteral("Save cove file"),
                                         QStringLiteral("Could not save: ") + err);
                }
                return;
            }
            setCoveEditorModified(w, false);
            if (coveStoreForRepo(m_repoDetailIndex).canWrite())
                setRepoDetailNotice(QStringLiteral("Saved %1 in cove.").arg(path));
            refreshCoveExplorerTree();
            return;
        }
    }
}

bool MainWindow::persistCoveExplorerChange(Cove &current, const Cove &updated,
                                           QString *error)
{
    if (error)
        error->clear();
    CoveStore store = coveStoreForRepo(m_repoDetailIndex);
    if (store.canWrite()) {
        if (!store.saveAccountCove(updated, error))
            return false;
        current = updated;
        return true;
    }
    if (!repoCanProposePull()) {
        if (error)
            *error = QStringLiteral("This repository cannot accept edits from "
                                    "the local mirror.");
        return false;
    }

    QByteArray envelope;
    if (!CoveStore::encryptedAccountEnvelope(updated, &envelope, error))
        return false;
    if (!proposePullFromMirrorEdit(updated.relPath,
                                   QString::fromUtf8(envelope)))
        return false;
    current = updated;
    return true;
}

void MainWindow::createCoveExplorerCove()
{
    const QString account = coveAccountName();
    if (account.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("New cove"),
                             QStringLiteral("Sign in with a ForkMesh account "
                                            "before creating a cove."));
        return;
    }
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("New cove"),
                              QStringLiteral("Cove name:"), QLineEdit::Normal,
                              QString(), &ok)
            .trimmed();
    if (!ok || name.isEmpty())
        return;
    CoveStore store = coveStoreForRepo(m_repoDetailIndex);
    Cove created;
    QString err;
    if (!store.createAccountCove(name, account, {account}, false, {}, &created, &err)) {
        QMessageBox::warning(this, QStringLiteral("New cove"),
                             QStringLiteral("Could not create cove: ") + err);
        return;
    }
    m_coveExplorerCurrentId = created.id;
    loadCoveExplorer();
    setRepoDetailNotice(QStringLiteral("Created account cove."));
}

void MainWindow::inviteUserToCurrentCove()
{
    const QString account = coveAccountName();
    if (account.isEmpty())
        return;
    int coveIndex = -1;
    for (int i = 0; i < m_coveExplorerCoves.size(); ++i) {
        if (m_coveExplorerCoves.at(i).id == m_coveExplorerCurrentId) {
            coveIndex = i;
            break;
        }
    }
    if (coveIndex < 0)
        return;
    Cove &cove = m_coveExplorerCoves[coveIndex];
    if (cove.creatorAccount.compare(account, Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(this, QStringLiteral("Share cove"),
                             QStringLiteral("Only the cove creator can change sharing."));
        return;
    }
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QString link =
        forkmesh::cove::shareUrl(repo.owner, repo.name, cove.id);
    if (link.isEmpty())
        return;

    QStringList candidates = mentionCandidateNames();
    for (const RepositoryRecord &repo : std::as_const(m_repositories))
        candidates << repo.owner;
    for (const QString &invitee : std::as_const(cove.invitedAccounts))
        candidates << invitee;
    candidates << cove.creatorAccount << account;

    QSet<QString> seenCandidates;
    QStringList inviteCandidates;
    for (const QString &raw : std::as_const(candidates)) {
        const QString name = raw.trimmed().toLower();
        if (name.isEmpty() || seenCandidates.contains(name))
            continue;
        seenCandidates.insert(name);
        if (name == account || name == cove.creatorAccount ||
            cove.invitedAccounts.contains(name))
            continue;
        inviteCandidates << name;
    }
    inviteCandidates.sort(Qt::CaseInsensitive);

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("coveShareDialog"));
    dialog.setWindowTitle(QStringLiteral("Share cove"));
    dialog.setModal(true);
    dialog.resize(620, 520);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    auto *heading = new QLabel(QStringLiteral("Share “%1”").arg(cove.name));
    heading->setStyleSheet(Theme::remapSheet(QStringLiteral("font-size:17px; font-weight:600;")));
    layout->addWidget(heading);
    auto *privacy = new QLabel(QStringLiteral(
        "Only the accounts below receive an encryption grant. The link contains no key or cove contents."));
    privacy->setObjectName(QStringLiteral("statusLine"));
    privacy->setWordWrap(true);
    layout->addWidget(privacy);

    auto *linkRow = new QHBoxLayout;
    auto *linkEdit = new QLineEdit(link);
    linkEdit->setReadOnly(true);
    linkEdit->setObjectName(QStringLiteral("coveShareLink"));
    linkEdit->setCursorPosition(0);
    auto *copyButton = new QPushButton(QStringLiteral("Copy link"));
    copyButton->setObjectName(QStringLiteral("ghostButton"));
    setOcticon(copyButton, QStringLiteral("copy"), 14);
    linkRow->addWidget(linkEdit, 1);
    linkRow->addWidget(copyButton);
    layout->addLayout(linkRow);

    auto *membersLabel = new QLabel(QStringLiteral("People with access"));
    membersLabel->setStyleSheet(Theme::remapSheet(QStringLiteral("font-weight:600;")));
    layout->addWidget(membersLabel);
    auto *members = new QTreeWidget;
    members->setObjectName(QStringLiteral("coveSharedWithList"));
    members->setColumnCount(2);
    members->setHeaderLabels(
        {QStringLiteral("Account"), QStringLiteral("Access")});
    members->setRootIsDecorated(false);
    members->setAlternatingRowColors(true);
    members->setSelectionMode(QAbstractItemView::SingleSelection);
    members->setIconSize(QSize(20, 20));
    members->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    members->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(members, 1);

    auto *addRow = new QHBoxLayout;
    auto *edit = new QLineEdit;
    edit->setObjectName(QStringLiteral("coveInviteAccount"));
    edit->setPlaceholderText(QStringLiteral("Add a ForkMesh account"));
    edit->setClearButtonEnabled(true);
    auto *model = new QStringListModel(inviteCandidates, &dialog);
    auto *completer = new QCompleter(model, &dialog);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setFilterMode(Qt::MatchContains);
    edit->setCompleter(completer);
    auto *addButton = new QPushButton(QStringLiteral("Add person"));
    addButton->setObjectName(QStringLiteral("primaryButton"));
    setOcticon(addButton, QStringLiteral("person"), 14);
    addRow->addWidget(edit, 1);
    addRow->addWidget(addButton);
    layout->addLayout(addRow);

    auto *status = new QLabel;
    status->setObjectName(QStringLiteral("statusLine"));
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *footer = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *sendButton = footer->addButton(QStringLiteral("Send link"),
                                         QDialogButtonBox::ActionRole);
    sendButton->setObjectName(QStringLiteral("primaryButton"));
    setOcticon(sendButton, QStringLiteral("paper-airplane"), 14);
    sendButton->setEnabled(false);
    layout->addWidget(footer);

    auto renderMembers = [this, members, &cove, account] {
        const QString selected = members->currentItem()
                                     ? members->currentItem()
                                           ->data(0, Qt::UserRole)
                                           .toString()
                                     : QString();
        members->clear();
        QStringList people = cove.invitedAccounts;
        if (!people.contains(cove.creatorAccount, Qt::CaseInsensitive))
            people << cove.creatorAccount;
        people.removeDuplicates();
        people.sort(Qt::CaseInsensitive);
        QTreeWidgetItem *restore = nullptr;
        QTreeWidgetItem *firstRecipient = nullptr;
        for (const QString &raw : std::as_const(people)) {
            const QString person = raw.trimmed().toLower();
            if (person.isEmpty())
                continue;
            const bool owner =
                person.compare(cove.creatorAccount, Qt::CaseInsensitive) == 0;
            auto *item = new QTreeWidgetItem(
                members, {QStringLiteral("@%1").arg(person),
                          owner ? QStringLiteral("Owner · can edit")
                                : QStringLiteral("Can edit")});
            item->setData(0, Qt::UserRole, person);
            const QString avatarIdentity =
                person == account ? m_profileIdentity.publicKey() : QString();
            item->setIcon(
                0, QIcon(issueTimelineAvatar(avatarIdentity, person, 20)));
            if (person == selected)
                restore = item;
            if (!firstRecipient && person != account)
                firstRecipient = item;
        }
        members->setCurrentItem(restore ? restore : firstRecipient);
    };
    auto deliverLink = [this, &cove, account, link, status](const QString &recipient) {
        if (recipient.isEmpty() || recipient == account)
            return;
        if (!m_officeChannelMirror || !m_officeChannelMirror->isActive())
            startOfficeChannelMirror();
        if (m_officeChannelMirror && m_officeChannelMirror->isActive() &&
            m_officeChannelMirror->sendDirectText(
                recipient, QStringLiteral("@%1 invited you to the cove %2: %3")
                               .arg(account, cove.name.left(160), link))) {
            status->setText(
                QStringLiteral("Sent the Cove link to @%1 as a direct message.").arg(recipient));
        } else {
            QApplication::clipboard()->setText(link);
            status->setText(QStringLiteral(
                "Direct messages are not connected, so the link was copied. Send it to @%1 when you reconnect.")
                                .arg(recipient));
        }
    };

    connect(copyButton, &QPushButton::clicked, &dialog, [link, status] {
        QApplication::clipboard()->setText(link);
        status->setText(QStringLiteral(
            "Link copied. It opens only for an account already listed above."));
    });
    connect(members, &QTreeWidget::currentItemChanged, &dialog,
            [sendButton, account](QTreeWidgetItem *current) {
                const QString recipient =
                    current ? current->data(0, Qt::UserRole).toString() : QString();
                sendButton->setEnabled(!recipient.isEmpty() && recipient != account);
                sendButton->setText(recipient.isEmpty()
                                        ? QStringLiteral("Send link")
                                        : QStringLiteral("Send link to @%1")
                                              .arg(recipient));
            });
    renderMembers();
    connect(sendButton, &QPushButton::clicked, &dialog,
            [members, deliverLink] {
                if (QTreeWidgetItem *selected = members->currentItem())
                    deliverLink(selected->data(0, Qt::UserRole).toString());
            });

    auto addPerson = [this, edit, &cove, account, status, renderMembers,
                      deliverLink] {
        const QString grantee = edit->text().trimmed().toLower();
        if (grantee.isEmpty())
            return;
        if (grantee == account ||
            cove.invitedAccounts.contains(grantee, Qt::CaseInsensitive)) {
            status->setText(
                QStringLiteral("@%1 already has access to this cove.").arg(grantee));
            return;
        }
        QString err;
        if (!coveInviteAccountValid(grantee, &err)) {
            status->setText(err);
            return;
        }
        Cove updated = cove;
        updated.invitedAccounts << grantee;
        updated.invitedAccounts.removeDuplicates();
        updated.invitedAccounts.sort(Qt::CaseInsensitive);
        if (!coveStoreForRepo(m_repoDetailIndex).saveAccountCove(updated, &err)) {
            status->setText(QStringLiteral("Could not save invitation: ") + err);
            return;
        }
        cove = updated;
        edit->clear();
        renderMembers();
        deliverLink(grantee);
        refreshCoveExplorerTree();
    };
    connect(addButton, &QPushButton::clicked, &dialog, addPerson);
    connect(edit, &QLineEdit::returnPressed, &dialog, addPerson);
    connect(footer, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(footer, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QTimer::singleShot(0, edit, [edit] { edit->setFocus(); });
    dialog.exec();

    m_coveExplorerCurrentId = cove.id;
    refreshCoveExplorerTree();
}

void MainWindow::createCoveExplorerDocument()
{
    int coveIndex = -1;
    for (int i = 0; i < m_coveExplorerCoves.size(); ++i) {
        if (m_coveExplorerCoves.at(i).id == m_coveExplorerCurrentId) {
            coveIndex = i;
            break;
        }
    }
    if (coveIndex < 0)
        return;

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("coveQuickCreateDialog"));
    dialog.setWindowTitle(QStringLiteral("Quick create in Cove"));
    dialog.resize(760, 560);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    auto *heading = new QLabel(QStringLiteral("Choose a file template"));
    heading->setStyleSheet(Theme::remapSheet(QStringLiteral("font-size:17px; font-weight:600;")));
    layout->addWidget(heading);
    auto *subheading = new QLabel(QStringLiteral(
        "All ten templates stay encrypted in this cove. CSV spreadsheets and lead pipelines open in purpose-built editors."));
    subheading->setObjectName(QStringLiteral("statusLine"));
    subheading->setWordWrap(true);
    layout->addWidget(subheading);

    auto *templates = new QListWidget;
    templates->setObjectName(QStringLiteral("coveTemplatePicker"));
    templates->setViewMode(QListView::IconMode);
    templates->setMovement(QListView::Static);
    templates->setResizeMode(QListView::Adjust);
    templates->setSelectionMode(QAbstractItemView::SingleSelection);
    templates->setWordWrap(true);
    templates->setSpacing(6);
    templates->setGridSize(QSize(230, 92));
    templates->setIconSize(QSize(28, 28));
    for (const forkmesh::cove::FileTemplate &item :
         forkmesh::cove::fileTemplates()) {
        auto *choice = new QListWidgetItem(
            iconForFile(item.defaultPath),
            QStringLiteral("%1\n%2").arg(item.title, item.description), templates);
        choice->setData(Qt::UserRole, item.id);
        choice->setData(Qt::UserRole + 1, item.defaultPath);
        choice->setToolTip(
            QStringLiteral("%1\nCreates %2").arg(item.description, item.defaultPath));
    }
    layout->addWidget(templates, 1);

    auto *pathRow = new QHBoxLayout;
    auto *pathLabel = new QLabel(QStringLiteral("File path"));
    auto *pathEdit = new QLineEdit;
    pathEdit->setObjectName(QStringLiteral("coveTemplatePath"));
    pathEdit->setClearButtonEnabled(true);
    pathRow->addWidget(pathLabel);
    pathRow->addWidget(pathEdit, 1);
    layout->addLayout(pathRow);
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create file"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(templates, &QListWidget::currentItemChanged, &dialog,
            [pathEdit](QListWidgetItem *current) {
                if (current)
                    pathEdit->setText(current->data(Qt::UserRole + 1).toString());
            });
    connect(pathEdit, &QLineEdit::textChanged, &dialog,
            [buttons](const QString &value) {
                buttons->button(QDialogButtonBox::Ok)
                    ->setEnabled(!value.trimmed().isEmpty());
            });
    if (templates->count() > 0)
        templates->setCurrentRow(0);
    if (dialog.exec() != QDialog::Accepted || !templates->currentItem())
        return;
    const forkmesh::cove::FileTemplate *chosen = forkmesh::cove::fileTemplate(
        templates->currentItem()->data(Qt::UserRole).toString());
    if (!chosen)
        return;
    const QString clean = QDir::cleanPath(pathEdit->text().trimmed());
    if (!coveDocPathIsSafe(clean)) {
        QMessageBox::warning(this, QStringLiteral("Quick create in Cove"),
                             QStringLiteral("Refusing to create that path."));
        return;
    }

    Cove &cove = m_coveExplorerCoves[coveIndex];
    for (const CoveDocument &doc : std::as_const(cove.documents)) {
        if (QDir::cleanPath(doc.name) == clean) {
            QMessageBox::warning(this, QStringLiteral("Quick create in Cove"),
                                 QStringLiteral("%1 already exists.").arg(clean));
            return;
        }
    }
    CoveDocument doc;
    doc.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    doc.name = clean;
    doc.mime = chosen->mime.isEmpty() ? coveDocMimeForPath(clean) : chosen->mime;
    doc.body = chosen->body;
    doc.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
    Cove updated = cove;
    updated.documents << doc;

    QString err;
    if (!persistCoveExplorerChange(cove, updated, &err)) {
        if (!err.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Quick create in Cove"),
                                 QStringLiteral("Could not save: ") + err);
        }
        return;
    }
    m_coveExplorerCurrentId = cove.id;
    refreshCoveExplorerTree();
    openCoveExplorerDocument(clean);
    setRepoDetailNotice(
        QStringLiteral("Created %1 from the %2 template.").arg(clean, chosen->title));
}

void MainWindow::deleteCurrentCoveExplorerDocument()
{
    int coveIndex = -1;
    for (int i = 0; i < m_coveExplorerCoves.size(); ++i) {
        if (m_coveExplorerCoves.at(i).id == m_coveExplorerCurrentId) {
            coveIndex = i;
            break;
        }
    }
    if (coveIndex < 0)
        return;
    Cove &cove = m_coveExplorerCoves[coveIndex];
    QString path;
    QWidget *w = m_repoFileTabs ? m_repoFileTabs->currentWidget() : nullptr;
    if (w && w->property("coveId").toString() == cove.id)
        path = w->property("coveDocPath").toString();
    if (path.isEmpty() && m_coveExplorerTree && m_coveExplorerTree->currentItem()) {
        QTreeWidgetItem *item = m_coveExplorerTree->currentItem();
        if (!item->data(0, Qt::UserRole + 1).toBool())
            path = item->data(0, Qt::UserRole).toString();
    }
    if (path.isEmpty())
        return;

    if (QMessageBox::question(this, QStringLiteral("Delete cove file"),
                              QStringLiteral("Delete \"%1\" from this cove?")
                                  .arg(path),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;
    Cove updated = cove;
    for (int i = 0; i < updated.documents.size(); ++i) {
        if (QDir::cleanPath(updated.documents.at(i).name) != path)
            continue;
        updated.documents.removeAt(i);
        QString err;
        if (!persistCoveExplorerChange(cove, updated, &err)) {
            if (!err.isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("Delete cove file"),
                                     QStringLiteral("Could not save: ") + err);
            }
            return;
        }
        const QString key = coveDocTabKey(cove.id, path);
        QWidget *openEditor = m_openCoveExplorerTabs.take(key);
        const int tab = openEditor && m_repoFileTabs
                            ? m_repoFileTabs->indexOf(openEditor)
                            : -1;
        if (tab >= 0)
            m_repoFileTabs->removeTab(tab);
        if (openEditor)
            openEditor->deleteLater();
        refreshCoveExplorerTree();
        setRepoDetailNotice(QStringLiteral("Deleted %1 from cove.").arg(path));
        return;
    }
}

void MainWindow::toggleRepoFileMarkdownPreview()
{
    QWidget *w = m_repoFileTabs ? m_repoFileTabs->currentWidget() : nullptr;
    auto *editor = dynamic_cast<CodePreviewEditor *>(w);
    if (!editor)
        return;
    editor->setMarkdownPreviewVisible(!editor->markdownPreviewVisible());
    updateRepoFileSaveActions();
}

void MainWindow::showRepoFileHistory(const QString &path)
{
    const QString dir = repoGitDir();
    if (dir.isEmpty() || path.isEmpty())
        return;
    const QString ref = currentRef();
    runOffThread<GitCaptureResult>(
        [dir, ref, path] {
            return readGit(dir, {"log", "--follow", "--date=format:%b %e, %Y",
                                 "--format=%H%x1f%an%x1f%ad%x1f%s", ref, "--",
                                 path});
        },
        [this, dir, ref, path](GitCaptureResult read) {
            if (dir != repoGitDir() || ref != currentRef())
                return;
            showRepoFileHistoryDialog(dir, path, read);
        });
}

void MainWindow::showRepoFileHistoryDialog(const QString &dir, const QString &path,
                                           const GitCaptureResult &read)
{
    if (!read.ok) {
        setRepoDetailNotice("Could not read history: " + read.error.left(200), true);
        return;
    }
    const QByteArray &out = read.output;

    struct HistEntry {
        QString hash, author, date, subject;
    };
    QList<HistEntry> entries;
    for (const QByteArray &lineRaw : out.split('\n')) {
        const QString line = QString::fromUtf8(lineRaw);
        if (line.trimmed().isEmpty())
            continue;
        const QStringList f = line.split(QLatin1Char('\x1f'));
        if (f.size() < 4)
            continue;
        entries.append(
            {f[0].trimmed(), f[1].trimmed(), f[2].trimmed(), f[3].trimmed()});
    }
    if (entries.isEmpty()) {
        setRepoDetailNotice("No commit history for " + path, false);
        return;
    }

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("fileHistoryDialog");
    dialog->setWindowTitle(QString::fromUtf8("History \xE2\x80\x94 ") + path);
    dialog->resize(940, 620);

    auto *list = new QListWidget;
    list->setObjectName("commitFileList");
    list->setMinimumWidth(260);
    list->setMaximumWidth(380);
    for (const HistEntry &e : entries) {
        auto *item = new QListWidgetItem(
            QString::fromUtf8("%1\n%2 \xC2\xB7 %3 \xC2\xB7 %4")
                .arg(e.subject, e.hash.left(7), e.author, e.date));
        item->setData(Qt::UserRole, e.hash);
        list->addItem(item);
    }

    auto *diffView = new QTextBrowser;
    diffView->setObjectName("commitDiffView");
    diffView->setOpenExternalLinks(false);
    registerDiffView(diffView);

    const QPointer<QTextBrowser> diffGuard(diffView);
    const QPointer<QListWidget> listGuard(list);
    connect(list, &QListWidget::currentItemChanged, this,
            [this, diffGuard, listGuard, dir, path](QListWidgetItem *item,
                                                    QListWidgetItem *) {
                if (!item)
                    return;
                const QString hash = item->data(Qt::UserRole).toString();
                runOffThread<QByteArray>(
                    [dir, path, hash] {
                        QByteArray patch;
                        runGitCapture(dir, {"show", "-M", "--format=", hash, "--", path},
                                      &patch, nullptr);
                        prefetchDiffImageBlobs(QString::fromUtf8(patch), dir,
                                               hash + "^", hash);
                        return patch;
                    },
                    [this, diffGuard, listGuard, dir, hash](QByteArray patch) {
                        if (!diffGuard || !listGuard || !listGuard->currentItem() ||
                            listGuard->currentItem()->data(Qt::UserRole).toString() !=
                                hash)
                            return;
                        QList<DiffFileEntry> files;
                        QString html = renderDiffHtml(QString::fromUtf8(patch), files,
                                                      dir, hash + "^", hash);
                        if (html.trimmed().isEmpty())
                            html = QStringLiteral(
                                "<p style='color:#8b949e'>No textual changes to this "
                                "file in the selected commit.</p>");
                        setDiffHtml(diffGuard, html);
                    });
            });

    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(list);
    split->addWidget(diffView);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({300, 640});

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(split, 1);
    layout->addWidget(buttons);

    list->setCurrentRow(0);
    dialog->show();
}

void MainWindow::saveRepoFileEdit(const QString &path, const QString &content,
                                  bool createPull, std::function<void(bool)> done)
{
    const auto finish = [done](bool ok) {
        if (done)
            done(ok);
    };
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return finish(false);
    const QString cleanPath = QDir::cleanPath(path);
    if (cleanPath.isEmpty() || cleanPath.startsWith("../") ||
        cleanPath.contains("/../") || QDir::isAbsolutePath(cleanPath)) {
        setRepoDetailNotice("Refusing to write outside the repository.", true);
        return finish(false);
    }
    if (!repoHasWorkingTree()) {
        if (!createPull) {
            setRepoDetailNotice(
                "This is a mirror; commit direct isn't available. Use \"Save as PR\".",
                true);
            return finish(false);
        }
        if (!repoCanProposePull()) {
            setRepoDetailNotice(
                "This is a read-only mirror; files can't be edited here.", true);
            return finish(false);
        }
        proposePullFromMirrorEdit(cleanPath, content,
                                  done ? done : std::function<void(bool)>([](bool) {}));
        return;
    }
    if (m_repoFileSaveRunning) {
        flashMessage(QStringLiteral("A file save is already in progress."), true);
        return finish(false);
    }

    const QString dir = repoGitDir();
    m_repoFileSaveRunning = true;
    runOffThread<GitCaptureResult>(
        [dir] { return readGit(dir, {"status", "--porcelain"}); },
        [this, dir, cleanPath, content, createPull, finish](GitCaptureResult status) {
            m_repoFileSaveRunning = false;
            if (dir != repoGitDir())
                return finish(false);
            if (!status.ok || !status.output.trimmed().isEmpty()) {
                setRepoDetailNotice(
                    status.error.isEmpty()
                        ? "Commit or stash local changes before saving a file."
                        : status.error.left(240),
                    true);
                return finish(false);
            }
            continueRepoFileEdit(dir, cleanPath, content, createPull, finish);
        });
}

void MainWindow::continueRepoFileEdit(const QString &dir, const QString &cleanPath,
                                      const QString &content, bool createPull,
                                      std::function<void(bool)> finish)
{
    const QStringList branches = repoBranches();
    const QString base = repoDefaultBranch(branches);
    if (base.isEmpty()) {
        setRepoDetailNotice("This repository has no branch to commit onto.", true);
        return finish(false);
    }

    QString title = QStringLiteral("Edit %1").arg(cleanPath);
    QString description;
    QString branch;
    if (createPull) {
        QDialog dialog(this);
        dialog.setWindowTitle("Save as pull request");
        auto *branchEdit = new QLineEdit(&dialog);
        branch = QStringLiteral("edit/%1-%2")
                     .arg(repoSegment(cleanPath, QStringLiteral("file")),
                          QString::number(QDateTime::currentSecsSinceEpoch()));
        branchEdit->setText(branch.left(80));
        auto *titleEdit = new QLineEdit(title, &dialog);
        auto *bodyEdit = new QPlainTextEdit(&dialog);
        bodyEdit->setPlaceholderText("Describe the change...");
        auto *form = new QFormLayout;
        form->addRow("Branch", branchEdit);
        form->addRow("Title", titleEdit);
        form->addRow("Description", bodyEdit);
        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                 &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText("Create pull request");
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addLayout(form);
        layout->addWidget(buttons);
        dialog.resize(520, 320);
        if (dialog.exec() != QDialog::Accepted)
            return finish(false);
        branch = branchEdit->text().trimmed();
        title = titleEdit->text().trimmed();
        description = bodyEdit->toPlainText();
        if (branch.isEmpty() || title.isEmpty()) {
            setRepoDetailNotice("A pull request needs a branch and title.", true);
            return finish(false);
        }
    } else {
        bool ok = false;
        title = QInputDialog::getText(this, "Commit file edit", "Commit message:",
                                      QLineEdit::Normal, title, &ok)
                    .trimmed();
        if (!ok)
            return finish(false);
        if (title.isEmpty()) {
            setRepoDetailNotice("A commit message is required.", true);
            return finish(false);
        }
    }
    if (m_repoFileSaveRunning || dir != repoGitDir())
        return finish(false);
    m_repoFileSaveRunning = true;
    const PullStore store = pullStoreForCurrentRepo();
    runOffThread<RepoFileEditCommit>(
        [dir, base, cleanPath, content, createPull, branch, title, description, store] {
            RepoFileEditCommit commit = commitRepoFileEdit(dir, base, cleanPath, content,
                                                           createPull, branch, title);
            if (createPull && commit.state == RepoFileEditCommit::Committed) {
                PullStore writer = store;
                commit.pullNumber = writer.createPull(
                    title, description, base, branch, QString::fromUtf8(commit.diff),
                    QString::fromUtf8(commit.mbox), /*branchBacked=*/true,
                    &commit.pullError);
            }
            return commit;
        },
        [this, dir, base, cleanPath, createPull, branch, title,
         finish](RepoFileEditCommit commit) {
            m_repoFileSaveRunning = false;
            const bool current = dir == repoGitDir();
            const auto report = [this, current](const QString &message, bool error) {
                if (current)
                    setRepoDetailNotice(message, error);
                else
                    logSystem(message);
            };
            if (commit.state != RepoFileEditCommit::Committed) {
                report(commit.message, commit.state == RepoFileEditCommit::Failed);
                return finish(false);
            }
            if (createPull) {
                const int number = commit.pullNumber;
                if (number < 0) {
                    report(commit.pullError.isEmpty()
                               ? QStringLiteral("Could not create the pull request.")
                               : commit.pullError,
                           true);
                    return finish(false);
                }
                logSystem(QStringLiteral("Saved %1 on %2 and opened pull #%3.")
                              .arg(cleanPath, branch)
                              .arg(number));
                if (current) {
                    setRepoDetailNotice(QStringLiteral("Opened pull request #%1 from %2.")
                                            .arg(number)
                                            .arg(branch));
                    m_currentPullNumber = number;
                    switchToPullTab(number);
                    triggerTeammatesForEvent(
                        QStringLiteral("new-pulls"),
                        QStringLiteral("Pull request #%1 · %2").arg(number).arg(title));
                }
            } else {
                logSystem(QStringLiteral("Committed %1 to %2.").arg(cleanPath, base));
                if (current)
                    setRepoDetailNotice(
                        QStringLiteral("Committed %1 to %2.").arg(cleanPath, base));
            }
            if (current) {
                setRepoBranch(base);
                const int filesPage = m_filesStack ? m_filesStack->currentIndex() : 0;
                refreshOpenRepoDetail();
                if (!createPull && m_filesStack)
                    m_filesStack->setCurrentIndex(filesPage);
            }
            finish(true);
        });
}

MainWindow::RepoFileEditCommit
MainWindow::commitRepoFileEdit(const QString &dir, const QString &base,
                               const QString &cleanPath, const QString &content,
                               bool createPull, const QString &branch,
                               const QString &title)
{
    RepoFileEditCommit result;
    const auto fail = [&result, &dir, &base](const QString &message, bool restore) {
        result.state = RepoFileEditCommit::Failed;
        result.message = message;
        if (restore)
            runGitCapture(dir, {"checkout", base}, nullptr, nullptr);
        return result;
    };
    QString err;
    if (!runGitCapture(dir, {"checkout", base}, nullptr, &err))
        return fail(QStringLiteral("Could not check out %1: %2").arg(base, err.left(200)),
                    false);
    if (createPull &&
        (!runGitCapture(dir, {"check-ref-format", "--branch", branch}, nullptr, &err) ||
         !runGitCapture(dir, {"checkout", "-b", branch, base}, nullptr, &err)))
        return fail(QStringLiteral("Could not create branch %1: %2")
                        .arg(branch, err.left(200)),
                    true);

    const QString fullPath = QDir(dir).filePath(cleanPath);
    QDir().mkpath(QFileInfo(fullPath).absolutePath());
    QFile file(fullPath);
    bool wrote = file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text) &&
                 file.write(content.toUtf8()) >= 0;
    file.close();
    if (!wrote)
        err = QStringLiteral("Could not write %1").arg(cleanPath);
    if (!wrote || !runGitCapture(dir, {"add", "--", cleanPath}, nullptr, &err))
        return fail(err.isEmpty() ? QStringLiteral("Could not stage the edited file.")
                                  : err.left(240),
                    true);
    if (runGitCapture(dir, {"diff", "--cached", "--quiet"}, nullptr, nullptr)) {
        runGitCapture(dir, {"checkout", base}, nullptr, nullptr);
        result.state = RepoFileEditCommit::NoChanges;
        result.message = QStringLiteral("No changes to save.");
        return result;
    }
    if (!runGitCapture(dir, {"commit", "-m", title, "--", cleanPath}, nullptr, &err))
        return fail(QStringLiteral("git commit failed: %1").arg(err.left(240)), true);
    if (createPull) {
        if (!runGitCapture(dir, {"diff", "--binary", base + ".." + branch}, &result.diff,
                           &err))
            return fail(QStringLiteral("Could not create pull request diff: %1")
                            .arg(err.left(240)),
                        true);
        runGitCapture(dir, {"format-patch", "--stdout", base + ".." + branch},
                      &result.mbox, nullptr);
        runGitCapture(dir, {"checkout", base}, nullptr, nullptr);
    }
    result.state = RepoFileEditCommit::Committed;
    return result;
}

bool MainWindow::repoCanProposePull() const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return false;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    if (repo.previewOnly)
        return false;
    if (repoHasWorkingTree())
        return true;
    return !repo.mirrorPath.trimmed().isEmpty() && QDir(repo.mirrorPath).exists();
}

bool MainWindow::proposePullFromMirrorEdit(const QString &cleanPath,
                                           const QString &content,
                                           std::function<void(bool)> done)
{
    const auto finish = [done](bool ok) {
        if (done)
            done(ok);
        return ok;
    };
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return finish(false);
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString mirror = repo.mirrorPath.trimmed();
    if (mirror.isEmpty() || !QDir(mirror).exists()) {
        setRepoDetailNotice("No local mirror to base a pull request on.", true);
        return finish(false);
    }

    const QStringList branches = repoBranches();
    const QString base = repoDefaultBranch(branches);
    if (base.isEmpty()) {
        setRepoDetailNotice("This repository has no branch to base a pull request on.",
                            true);
        return finish(false);
    }

    QString branch = QStringLiteral("edit/%1-%2")
                         .arg(repoSegment(cleanPath, QStringLiteral("file")),
                              QString::number(QDateTime::currentSecsSinceEpoch()));
    QString title = QStringLiteral("Edit %1").arg(cleanPath);
    QString description;
    {
        QDialog dialog(this);
        dialog.setWindowTitle("Save as pull request");
        auto *info = new QLabel(
            QStringLiteral("This repository is mirrored from <b>%1/%2</b>. Your "
                           "change will be sent to its owner as a pull request.")
                .arg(repo.owner.toHtmlEscaped(), repo.name.toHtmlEscaped()),
            &dialog);
        info->setWordWrap(true);
        info->setTextFormat(Qt::RichText);
        auto *branchEdit = new QLineEdit(branch.left(80), &dialog);
        auto *titleEdit = new QLineEdit(title, &dialog);
        auto *bodyEdit = new QPlainTextEdit(&dialog);
        bodyEdit->setPlaceholderText("Describe the change...");
        auto *form = new QFormLayout;
        form->addRow("Branch", branchEdit);
        form->addRow("Title", titleEdit);
        form->addRow("Description", bodyEdit);
        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                 &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText("Create pull request");
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(info);
        layout->addLayout(form);
        layout->addWidget(buttons);
        dialog.resize(520, 340);
        if (dialog.exec() != QDialog::Accepted)
            return finish(false);
        branch = branchEdit->text().trimmed();
        title = titleEdit->text().trimmed();
        description = bodyEdit->toPlainText();
        if (branch.isEmpty() || title.isEmpty()) {
            setRepoDetailNotice("A pull request needs a branch and title.", true);
            return finish(false);
        }
    }

    const QString authorName =
        m_userName.trimmed().isEmpty() ? QStringLiteral("ForkMesh contributor")
                                       : m_userName.trimmed();
    const QString authorEmail =
        (m_profileIdentity.publicKey().isEmpty() ? QStringLiteral("contributor")
                                                 : m_profileIdentity.publicKey()) +
        QStringLiteral("@forkmesh");
    if (!done) {
        const RepoFileEditCommit commit = buildMirrorEditPull(
            mirror, base, cleanPath, content, title, authorName, authorEmail);
        return sendMirrorEditPull(repo, pullStoreForCurrentRepo(), true, cleanPath,
                                  base, branch, title, description, commit);
    }
    if (m_repoFileSaveRunning)
        return finish(false);
    m_repoFileSaveRunning = true;
    const PullStore store = pullStoreForCurrentRepo();
    runOffThread<RepoFileEditCommit>(
        [mirror, base, cleanPath, content, title, authorName, authorEmail] {
            return buildMirrorEditPull(mirror, base, cleanPath, content, title,
                                       authorName, authorEmail);
        },
        [this, repo, store, cleanPath, base, branch, title, description,
         finish](RepoFileEditCommit commit) {
            m_repoFileSaveRunning = false;
            const bool current =
                m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size() &&
                m_repositories.at(m_repoDetailIndex).mirrorPath == repo.mirrorPath;
            finish(sendMirrorEditPull(repo, store, current, cleanPath, base, branch,
                                      title, description, commit));
        });
    return true;
}

MainWindow::RepoFileEditCommit
MainWindow::buildMirrorEditPull(const QString &mirror, const QString &base,
                                const QString &cleanPath, const QString &content,
                                const QString &title, const QString &authorName,
                                const QString &authorEmail)
{
    RepoFileEditCommit result;
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        result.message = QStringLiteral("Could not create a temporary work area.");
        return result;
    }
    const QString work = tmp.filePath(QStringLiteral("wt"));
    QString err;
    const auto fail = [&](const QString &message) {
        runGitCapture(mirror, {"worktree", "remove", "--force", work}, nullptr,
                      nullptr);
        runGitCapture(mirror, {"worktree", "prune"}, nullptr, nullptr);
        result.state = RepoFileEditCommit::Failed;
        result.message = message;
        return result;
    };
    if (!runGitCapture(mirror, {"worktree", "add", "--detach", work, base}, nullptr,
                       &err))
        return fail(QStringLiteral("Could not prepare a pull request: %1")
                        .arg(err.left(200)));

    const QString fullPath = QDir(work).filePath(cleanPath);
    bool wrote = QDir().mkpath(QFileInfo(fullPath).absolutePath());
    if (wrote) {
        QFile file(fullPath);
        wrote = file.open(QIODevice::WriteOnly | QIODevice::Truncate |
                          QIODevice::Text) &&
                file.write(content.toUtf8()) >= 0;
    }
    if (!wrote)
        return fail(QStringLiteral("Could not write %1.").arg(cleanPath));
    if (!runGitCapture(work, {"add", "--", cleanPath}, nullptr, &err))
        return fail(err.isEmpty() ? QStringLiteral("Could not stage the edited file.")
                                  : err.left(240));
    if (runGitCapture(work, {"diff", "--cached", "--quiet"}, nullptr, nullptr)) {
        fail(QStringLiteral("No changes to save."));
        result.state = RepoFileEditCommit::NoChanges;
        return result;
    }
    if (!runGitCapture(work,
                       {"-c", "user.name=" + authorName,
                        "-c", "user.email=" + authorEmail, "commit", "-m", title,
                        "--", cleanPath},
                       nullptr, &err))
        return fail(QStringLiteral("git commit failed: %1").arg(err.left(240)));
    if (!runGitCapture(work, {"diff", "--binary", base + "..HEAD"}, &result.diff, &err) ||
        result.diff.trimmed().isEmpty())
        return fail(QStringLiteral("Could not create pull request diff: %1")
                        .arg(err.left(240)));
    runGitCapture(work, {"format-patch", "--stdout", base + "..HEAD"}, &result.mbox,
                  nullptr);
    fail(QString());
    result.state = RepoFileEditCommit::Committed;
    return result;
}

bool MainWindow::sendMirrorEditPull(const RepositoryRecord &repo, PullStore store,
                                    bool current,
                                    const QString &cleanPath, const QString &base,
                                    const QString &branch, const QString &title,
                                    const QString &description,
                                    const RepoFileEditCommit &commit)
{
    if (commit.state != RepoFileEditCommit::Committed) {
        if (current)
            setRepoDetailNotice(commit.message,
                                commit.state == RepoFileEditCommit::Failed);
        else
            logSystem(commit.message);
        return false;
    }
    PullRequest pr;
    pr.title = title;
    pr.description = description;
    pr.base = base;
    pr.head = branch;
    pr.patch = QString::fromUtf8(commit.diff);
    pr.commits = QString::fromUtf8(commit.mbox);

    store.setSigningRepository(repo.owner, repo.name);
    submitPullToInbox(store.makeSignedPull(pr), repo);
    logSystem(QStringLiteral("Sent %1 to %2/%3 as a pull request.")
                  .arg(cleanPath, repo.owner, repo.name));
    return true;
}

void MainWindow::showRepoOverview()
{
    if (!m_filesStack)
        return;
    loadRepoOverview(m_overviewPath);
    m_filesStack->setCurrentIndex(0);
    if (m_filesModeOverviewButton)
        m_filesModeOverviewButton->setChecked(true);
    showOverviewFiles();
}

void MainWindow::showRepoCoveExplorer()
{
    ensureSectionBuilt(kFilesSectionIndex);
    showSection(kFilesSectionIndex);
    showFilesCoveTree();
}

void MainWindow::showOverviewCommits()
{
    clearGitMergedCover();
    ensureRepoOverviewBodyBuilt(1);
    if (m_repoDetailTabs && m_repoDetailTabs->button(0))
        m_repoDetailTabs->button(0)->setChecked(true);
    if (m_repoDetailStack)
        m_repoDetailStack->setCurrentIndex(0);
    if (m_filesStack)
        m_filesStack->setCurrentIndex(0);
    if (m_filesModeOverviewButton)
        m_filesModeOverviewButton->setChecked(true);
    if (m_overviewBodyStack)
        m_overviewBodyStack->setCurrentIndex(1);
    // This is an activity-rail destination, not a Code Overview sub-page. Some
    // routes arrive while every stack is already on index 0/1, so no
    // currentChanged signal fires and the Code chrome would otherwise remain
    // visible above the Git workspace. Apply the ownership state explicitly
    // after all stacks have moved, then reassert it on the next event-loop turn
    // in case a queued repository refresh completed during navigation.
    updateRepoActivityRail();
    QTimer::singleShot(0, this, [this] {
        if (gitWorkspaceIsVisible())
            updateRepoActivityRail();
    });
}

bool MainWindow::gitWorkspaceIsVisible() const
{
    return m_repoDetailStack && m_repoDetailStack->currentIndex() == 0 &&
           m_filesStack && m_filesStack->currentIndex() == 0 &&
           m_overviewBodyStack && m_overviewBodyStack->currentIndex() == 1;
}

void MainWindow::showOverviewFiles()
{
    if (m_overviewBodyStack)
        m_overviewBodyStack->setCurrentIndex(0);
}

void MainWindow::showOverviewBranches()
{
    ensureRepoOverviewBodyBuilt(2);
    if (m_repoDetailTabs && m_repoDetailTabs->button(0))
        m_repoDetailTabs->button(0)->setChecked(true);
    if (m_repoDetailStack)
        m_repoDetailStack->setCurrentIndex(0);
    if (m_filesStack)
        m_filesStack->setCurrentIndex(0);
    if (m_filesModeOverviewButton)
        m_filesModeOverviewButton->setChecked(true);
    if (m_overviewBodyStack)
        m_overviewBodyStack->setCurrentIndex(2);
    updateRepoActivityRail();
}

void MainWindow::showOverviewWorktrees()
{
    ensureRepoOverviewBodyBuilt(3);
    if (m_repoDetailTabs && m_repoDetailTabs->button(0))
        m_repoDetailTabs->button(0)->setChecked(true);
    if (m_repoDetailStack)
        m_repoDetailStack->setCurrentIndex(0);
    if (m_filesStack)
        m_filesStack->setCurrentIndex(0);
    if (m_filesModeOverviewButton)
        m_filesModeOverviewButton->setChecked(true);
    if (m_overviewBodyStack)
        m_overviewBodyStack->setCurrentIndex(3);
}

void MainWindow::openRepoReadme()
{
    if (!m_repoFileTabs)
        return;
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return;
    const QString ref = currentRef();
    runOffThread<GitCaptureResult>(
        [dir, ref] { return readGit(dir, {"ls-tree", "--name-only", ref}); },
        [this, dir, ref](GitCaptureResult read) {
            if (!read.ok || !m_repoFileTabs || dir != repoGitDir() ||
                ref != currentRef() || !m_openFileTabs.isEmpty() ||
                !m_repoFileOpenWaiters.isEmpty())
                return;
            QString readme;
            for (const QString &name :
                 QString::fromUtf8(read.output).split('\n', Qt::SkipEmptyParts)) {
                if (!name.startsWith(QStringLiteral("README"), Qt::CaseInsensitive))
                    continue;
                if (name.compare(QStringLiteral("README.md"), Qt::CaseInsensitive) == 0) {
                    readme = name;
                    break;
                }
                if (readme.isEmpty())
                    readme = name;
            }
            if (!readme.isEmpty())
                openRepoFile(readme);
        });
}

// Size the images a rendered README brought in. Markdown has no width syntax,
// so MarkdownRender parks the width an <img width="120"> asked for in a "#w="
// fragment on the URL: strip it, scale to it (never wider than the panel, so a
// 1254px logo cannot push the overview sideways), and fall back to the alt
// text when the bytes cannot be loaded at all - a broken-image box reads worse
// than the words it replaced, and remote badge URLs never load here.
void forkmesh::ui::fitReadmeImages(QTextBrowser *view, int maxWidth)
{
    QTextDocument *doc = view->document();
    struct Sized {
        int start = 0;
        int length = 0;
        QTextImageFormat format;
        QString alt;
        bool loaded = false;
    };
    QList<Sized> sized;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat())
                continue;
            QTextImageFormat image = fragment.charFormat().toImageFormat();
            QString name = image.name();
            int declared = 0;
            const int hint = name.lastIndexOf(MarkdownRender::imageWidthHint());
            if (hint >= 0) {
                declared =
                    name.mid(hint + MarkdownRender::imageWidthHint().size()).toInt();
                name = name.left(hint);
                image.setName(name);
            }
            QImage source;
            const QVariant resource =
                doc->resource(QTextDocument::ImageResource, QUrl(name));
            switch (resource.typeId()) {
            case QMetaType::QImage:
                source = resource.value<QImage>();
                break;
            case QMetaType::QPixmap:
                source = resource.value<QPixmap>().toImage();
                break;
            case QMetaType::QByteArray:
                source.loadFromData(resource.toByteArray());
                break;
            default:
                break;
            }
            Sized entry;
            entry.start = fragment.position();
            entry.length = fragment.length();
            entry.format = image;
            entry.loaded = !source.isNull() && source.width() > 0;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
            entry.alt = image.stringProperty(QTextFormat::ImageAltText);
#endif
            if (entry.loaded) {
                int width = declared > 0 ? declared : source.width();
                if (maxWidth > 0)
                    width = qMin(width, maxWidth);
                entry.format.setWidth(width);
                entry.format.setHeight(qMax(
                    1, qRound(double(source.height()) * width / source.width())));
            }
            sized.append(entry);
        }
    }
    // Applied back to front so an alt-text replacement cannot shift the
    // positions of the entries still to come.
    for (int i = sized.size() - 1; i >= 0; --i) {
        const Sized &entry = sized.at(i);
        QTextCursor cursor(doc);
        cursor.setPosition(entry.start);
        cursor.setPosition(entry.start + entry.length, QTextCursor::KeepAnchor);
        if (entry.loaded) {
            cursor.setCharFormat(entry.format);
            continue;
        }
        cursor.removeSelectedText();
        if (!entry.alt.isEmpty())
            cursor.insertText(entry.alt, QTextCharFormat());
    }
}

// Qt imports markdown with every block margin at zero, so a README arrives as
// one dense wall: headings sit on the paragraph above them and a fenced block
// is indistinguishable from prose. Give the document the spacing a README is
// written for, and tint code blocks so they read as code.
void forkmesh::ui::styleReadmeDocument(QTextBrowser *view)
{
    const bool dark = view->palette().color(QPalette::Text).lightness() > 128;
    const auto classic = [dark](const char *darkHex, const char *lightHex) {
        return QColor(QLatin1String(dark ? darkHex : lightHex));
    };
    markdownstyle::Colors colors;
    colors.text = Theme::nextOr(Theme::Role::Fg, view->palette().color(QPalette::Text));
    colors.muted = Theme::nextOr(Theme::Role::FgMuted, classic("#9198a1", "#59636e"));
    colors.border = Theme::nextOr(Theme::Role::Border, classic("#3d444d", "#d1d9e0"));
    colors.quoteBar = Theme::nextOr(Theme::Role::BorderStrong, classic("#3d444d", "#d1d9e0"));
    colors.codeBackground =
        Theme::nextOr(Theme::Role::SurfaceRaised, classic("#161b22", "#f6f8fa"));
    colors.inlineCodeBackground = Theme::nextOr(
        Theme::Role::SurfaceActive, dark ? QColor(110, 118, 129, 102) : QColor(129, 139, 152, 31));
    colors.rowBackground =
        Theme::nextOr(Theme::Role::SurfaceRaised, classic("#161b22", "#f6f8fa"));
    colors.link = Theme::nextOr(Theme::Role::AccentFg, classic("#58a6ff", "#0969da"));
    colors.monoFamilies =
        Theme::monospaceFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)).families();
    markdownstyle::apply(view, colors);
}

// Heading blocks carry GitHub-style anchor names, so a "#section" link can
// scrollToAnchor() to them. Repeats are numbered "-1", "-2" as GitHub does.
// The anchors have no href: they neither look nor act like links.
void forkmesh::ui::anchorReadmeHeadings(QTextDocument *doc)
{
    QHash<QString, int> seen;
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        if (block.blockFormat().headingLevel() <= 0 || block.length() <= 1)
            continue;
        const QString base = MarkdownRender::headingAnchor(block.text());
        const int repeat = seen.value(base);
        seen.insert(base, repeat + 1);
        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorNames({repeat ? QStringLiteral("%1-%2").arg(base).arg(repeat) : base});
        cursor.setPosition(block.position());
        cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    cursor.endEditBlock();
}

// Markdown as the overview README shows it: raw HTML flattened first (Qt's
// importer swallows the prose after an unclosed <img>), relative images
// resolved against `searchDir`, and headings anchored for "#section" links.
static void renderReadmeMarkdown(QTextBrowser *view, const QString &markdown,
                                 const QString &searchDir)
{
    view->setSearchPaths(searchDir.isEmpty() ? QStringList() : QStringList{searchDir});
    view->setMarkdown(MarkdownRender::forPreview(markdown));
    anchorReadmeHeadings(view->document());
}

void MainWindow::configureRepoMarkdownPreview(QPlainTextEdit *plain, const QString &path)
{
    auto *editor = dynamic_cast<CodePreviewEditor *>(plain);
    if (!editor || previewSyntaxForPath(path) != PreviewSyntax::Markdown)
        return;
    const QString dir = repoGitDir();
    const QString searchDir =
        dir.isEmpty() ? QString() : QFileInfo(QDir(dir).filePath(path)).absolutePath();
    editor->markdownRenderer = [searchDir](QTextBrowser *view, const QString &markdown) {
        renderReadmeMarkdown(view, markdown, searchDir);
        // Sized once the browser has its real viewport width.
        QTimer::singleShot(0, view, [view] {
            fitReadmeImages(view, qMax(1, view->viewport()->width() - 8));
            styleReadmeDocument(view);
        });
    };
    editor->markdownLinkHandler = [this, path](QTextBrowser *view, const QUrl &url) {
        openRepoMarkdownLink(view, path, url);
    };
}

void MainWindow::openRepoMarkdownLink(QTextBrowser *view, QString basePath, QUrl url)
{
    const QString href = url.toString();
    if (href.startsWith(QLatin1Char('#'))) {
        if (view)
            view->scrollToAnchor(QUrl::fromPercentEncoding(href.mid(1).toUtf8()));
        return;
    }
    const std::optional<MarkdownRender::RepoLink> link =
        MarkdownRender::resolveRepoLink(basePath, href);
    if (!link) {
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https") ||
            scheme == QLatin1String("mailto"))
            QDesktopServices::openUrl(url);
        return;
    }

    const QString dir = repoGitDir();
    if (link->directory || dir.isEmpty()) {
        openRepoMarkdownTarget(dir, *link, link->directory);
        return;
    }
    const QString ref = currentRef();
    const QString linkPath = link->path;
    const MarkdownRender::RepoLink target = *link;
    runOffThread<int>(
        [dir, ref, linkPath] {
            QByteArray type;
            if (!runGitCapture(dir, {"cat-file", "-t", ref + ":" + linkPath}, &type,
                               nullptr))
                return -1;
            return type.trimmed() == "tree" ? 1 : 0;
        },
        [this, dir, ref, target](int kind) {
            if (dir != repoGitDir() || ref != currentRef())
                return;
            const bool directory =
                kind < 0 ? QFileInfo(QDir(dir).filePath(target.path)).isDir()
                         : kind == 1;
            openRepoMarkdownTarget(dir, target, directory);
        });
}

void MainWindow::openRepoMarkdownTarget(const QString &dir,
                                        const MarkdownRender::RepoLink &link,
                                        bool directory)
{
    Q_UNUSED(dir);
    if (directory) {
        ensureSectionBuilt(kFilesSectionIndex);
        m_overviewPath = link.path;
        showRepoOverview();
        showSection(kFilesSectionIndex);
        return;
    }
    if (link.fragment.isEmpty()) {
        openRepoFile(link.path);
        return;
    }
    openRepoFile(link.path, [this, path = link.path, anchor = link.fragment] {
        auto *editor = dynamic_cast<CodePreviewEditor *>(m_openFileTabs.value(path));
        QTextBrowser *preview = editor ? editor->markdownPreview() : nullptr;
        if (!preview)
            return;
        QTimer::singleShot(0, preview, [preview, anchor] {
            preview->scrollToAnchor(anchor);
        });
    });
}

// The overview's per-commit reads, memoised while the browsed tree is the same
// commit. It sits at file scope because the deferred fill below writes into it
// from the GUI thread after its worker returns.
struct OverviewTreeCache {
    QString key;                         // dir|head
    QByteArray sizeOut;                  // ls-tree -r -l -z output
    QByteArray locOut;                   // empty-tree numstat output
    QHash<QString, QByteArray> entryLog; // entry path -> `git log -1` record
};

static OverviewTreeCache &overviewTreeCache()
{
    static OverviewTreeCache cache;
    return cache;
}

QString MainWindow::overviewRequestFor(const QString &path) const
{
    return QString::number(m_repoDetailIndex) + QLatin1Char('|') + path +
           QLatin1Char('|') + currentRef() + QLatin1Char('|') + repoGitDir();
}

void MainWindow::loadRepoOverview(const QString &path)
{
    if (!m_overviewList)
        return;

    const QString request = overviewRequestFor(path);
    m_overviewPath = path;
    scheduleNavRecord();
    if (m_overviewLoading && request == m_overviewLoadRequest) {
        m_overviewReloadPending = true;
        return;
    }

    const QString dir = repoGitDir();
    const QString ref = currentRef();
    const QString keyPrefix = QStringLiteral("%1|%2|%3|")
                                  .arg(m_repoDetailIndex)
                                  .arg(path, ref);
    const bool showingThis = m_overviewList->topLevelItemCount() > 0 &&
                             m_overviewLoadedKey.startsWith(keyPrefix);
    if (!showingThis) {
        m_overviewLoadedKey.clear();
        showOverviewLoadingPlaceholders();
    }

    const OverviewTreeCache &cache = overviewTreeCache();
    OverviewRead read;
    read.dir = dir;
    read.path = path;
    read.ref = ref;
    read.keyPrefix = keyPrefix;
    read.loadedKey = showingThis ? m_overviewLoadedKey : QString();
    read.recoverBase = m_repoBranch.isEmpty() ? QString() : browsedRefRecoveryBase();
    read.wantCommitBar = m_commitBar != nullptr;
    read.cacheKey = cache.key;
    read.cachedSizeOut = cache.sizeOut;
    read.cachedLocOut = cache.locOut;
    read.cachedEntryLog = cache.entryLog;

    const quint64 generation = ++m_overviewLoadGen;
    m_overviewLoading = true;
    m_overviewLoadRequest = request;
    m_overviewReloadPending = false;
    runOffThread<OverviewLoad>(
        [read] { return readRepoOverview(read); },
        [this, generation, request, path, dir, keyPrefix](OverviewLoad load) {
            applyRepoOverview(generation, request, path, dir, keyPrefix,
                              std::move(load));
        });
}

MainWindow::OverviewLoad MainWindow::readRepoOverview(const OverviewRead &request)
{
    OverviewLoad load;
    const QString &dir = request.dir;
    const QString &path = request.path;
    const QString &ref = request.ref;
    if (dir.isEmpty()) {
        load.messageRow = QStringLiteral("No local copy of this repository to browse.");
        return load;
    }

    QByteArray headOut;
    load.refMissing = !runGitCapture(dir, {"rev-parse", ref}, &headOut, nullptr);
    if (load.refMissing && ref != QLatin1String("HEAD"))
        load.recovery = readBrowsedRefRecovery(dir, ref, request.recoverBase);
    load.head = QString::fromUtf8(headOut).trimmed();
    if (!request.loadedKey.isEmpty() &&
        request.keyPrefix + load.head == request.loadedKey) {
        load.unchanged = true;
        return load;
    }

    if (request.wantCommitBar) {
        QByteArray logOut;
        QStringList logArgs{"log", "-1", "--format=%H%x1f%an%x1f%ar%x1f%s", ref};
        if (!path.isEmpty())
            logArgs << "--" << path;
        if (runGitCapture(dir, logArgs, &logOut, nullptr) &&
            !logOut.trimmed().isEmpty()) {
            const QStringList f = QString::fromUtf8(logOut).trimmed().split('\x1f');
            load.hasCommit = true;
            load.commitHash = f.value(0);
            load.commitAuthor = f.value(1);
            load.commitWhen = f.value(2);
            load.commitSubject = f.value(3);
        }
    }

    const QString treeish = path.isEmpty() ? ref : ref + ":" + path;
    QByteArray out;
    QString err;
    if (!runGitCapture(dir, {"ls-tree", "-z", treeish}, &out, &err)) {
        load.messageRow = err.isEmpty() || !gitRefResolves(dir, QStringLiteral("HEAD"))
                              ? QStringLiteral("This repository has no commits yet.")
                              : "Could not read files: " + err.left(120);
        return load;
    }

    const QString treeKey = load.head.isEmpty() ? QString() : dir + QLatin1Char('|') + load.head;
    const bool cached = !treeKey.isEmpty() && request.cacheKey == treeKey;
    const QString prefix = path.isEmpty() ? QString() : path + "/";

    QHash<QString, qint64> childBytes;
    QHash<QString, qint64> childFiles;
    load.sizeOut = cached ? request.cachedSizeOut : QByteArray();
    if (load.sizeOut.isEmpty())
        runGitCapture(dir, {"ls-tree", "-r", "-l", "-z", ref}, &load.sizeOut, nullptr);
    for (const QByteArray &record : load.sizeOut.split('\0')) {
        if (record.isEmpty())
            continue;
        const int tab = record.indexOf('\t');
        if (tab < 0)
            continue;
        const QStringList meta =
            QString::fromUtf8(record.left(tab)).split(' ', Qt::SkipEmptyParts);
        if (meta.size() < 4)
            continue;
        const qint64 sz = meta.at(3).toLongLong();
        load.repoBytes += sz;
        const QString blob = QString::fromUtf8(record.mid(tab + 1));
        if (!prefix.isEmpty() && !blob.startsWith(prefix))
            continue;
        const QString rel = prefix.isEmpty() ? blob : blob.mid(prefix.size());
        const QString top = rel.section('/', 0, 0);
        childBytes[top] += sz;
        childFiles[top] += 1;
    }

    QHash<QString, qint64> childLoc;
    static const QByteArray kEmptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";
    load.locOut = cached ? request.cachedLocOut : QByteArray();
    if (load.locOut.isEmpty())
        runGitCapture(dir,
                      {"diff", "--numstat", "--no-renames", "-z",
                       QString::fromLatin1(kEmptyTree), ref},
                      &load.locOut, nullptr);
    for (const QByteArray &record : load.locOut.split('\0')) {
        if (record.isEmpty())
            continue;
        const int firstTab = record.indexOf('\t');
        const int secondTab = firstTab < 0 ? -1 : record.indexOf('\t', firstTab + 1);
        if (secondTab < 0)
            continue;
        const QByteArray added = record.left(firstTab);
        if (added == "-")
            continue;
        const qint64 lines = added.toLongLong();
        const QString blob = QString::fromUtf8(record.mid(secondTab + 1));
        if (!prefix.isEmpty() && !blob.startsWith(prefix))
            continue;
        const QString rel = prefix.isEmpty() ? blob : blob.mid(prefix.size());
        childLoc[rel.section('/', 0, 0)] += lines;
    }

    for (const QByteArray &record : out.split('\0')) {
        if (record.isEmpty())
            continue;
        const int tab = record.indexOf('\t');
        if (tab < 0)
            continue;
        const QStringList meta =
            QString::fromUtf8(record.left(tab)).split(' ', Qt::SkipEmptyParts);
        if (meta.size() < 2)
            continue;
        OverviewRow e;
        e.name = QString::fromUtf8(record.mid(tab + 1));
        e.isDir = meta.at(1) == "tree";
        e.path = path.isEmpty() ? e.name : path + "/" + e.name;
        e.size = childBytes.value(e.name, 0);
        e.loc = childLoc.value(e.name, 0);
        e.fileCount = childFiles.value(e.name, 0);
        const QByteArray logOut =
            cached ? request.cachedEntryLog.value(e.path) : QByteArray();
        if (logOut.trimmed().isEmpty()) {
            if (logOut.isEmpty())
                load.pendingEntryLogs.append(e.path);
        } else {
            const QStringList f = QString::fromUtf8(logOut).trimmed().split('\x1f');
            e.commitTs = f.value(0).toLongLong();
            e.whenText = f.value(1);
            e.subject = f.value(2);
        }
        load.rows.append(e);
        if (!e.isDir && e.name.compare("README.md", Qt::CaseInsensitive) == 0)
            load.readmePath = e.path;
    }

    if (!load.readmePath.isEmpty()) {
        QByteArray readme;
        if (runGitCapture(dir, {"show", ref + ":" + load.readmePath}, &readme,
                          nullptr) &&
            !readme.contains('\0'))
            load.readmeMarkdown = QString::fromUtf8(readme);
    }
    return load;
}

void MainWindow::applyRepoOverview(quint64 generation, const QString &request,
                                   const QString &path, const QString &dir,
                                   const QString &keyPrefix, OverviewLoad load)
{
    if (generation != m_overviewLoadGen)
        return;
    m_overviewLoading = false;
    const bool reloadPending = m_overviewReloadPending;
    m_overviewReloadPending = false;
    if (!m_overviewList)
        return;
    if (request != overviewRequestFor(m_overviewPath)) {
        loadRepoOverview(m_overviewPath);
        return;
    }
    if (load.refMissing && applyBrowsedRefRecovery(load.recovery)) {
        loadRepoOverview(path);
        return;
    }
    if (load.unchanged) {
        if (reloadPending)
            loadRepoOverview(path);
        return;
    }

    OverviewTreeCache &treeCache = overviewTreeCache();
    const QString treeKey =
        load.head.isEmpty() ? QString() : dir + QLatin1Char('|') + load.head;
    if (treeKey.isEmpty() || treeCache.key != treeKey)
        treeCache = OverviewTreeCache{treeKey, {}, {}, {}};
    if (!load.sizeOut.isEmpty())
        treeCache.sizeOut = load.sizeOut;
    if (!load.locOut.isEmpty())
        treeCache.locOut = load.locOut;

    QString commitBarText;
    QPixmap commitBarAvatar;
    if (m_commitBar) {
        if (load.hasCommit) {
            m_commitBarStatusHash = load.commitHash;
            const QString me = topBarUserName();
            const QByteArray avatarPng =
                (!me.isEmpty() && load.commitAuthor.compare(me, Qt::CaseInsensitive) == 0)
                    ? effectiveUserAvatar()
                    : forkMeshAvatarPng(load.commitAuthor.toLower());
            commitBarAvatar = roundedAvatar(avatarPng, kCommitBarAvatarPx, 0.5);
            if (m_overviewList->property("nextCodeTable").toBool())
                m_commitBarBodyHtml =
                    QStringLiteral("<span style='font-weight:600'>%1</span>&nbsp;&nbsp;"
                                   "<span style='color:%3'>%2</span>")
                        .arg(load.commitAuthor.toHtmlEscaped(),
                             load.commitSubject.toHtmlEscaped(),
                             Theme::hex(Theme::Role::FgMuted));
            else
                m_commitBarBodyHtml =
                    QStringLiteral("<b>%1</b>&nbsp;&nbsp;%2"
                                   "<span style='color:#8b949e'>&nbsp;&nbsp;&middot;"
                                   "&nbsp;committed %3</span>")
                        .arg(load.commitAuthor.toHtmlEscaped(),
                             load.commitSubject.toHtmlEscaped(),
                             load.commitWhen.toHtmlEscaped());
            commitBarText = m_commitBarBodyHtml + commitStatusGlyph(load.commitHash);
        } else {
            m_commitBarStatusHash.clear();
            m_commitBarBodyHtml.clear();
            commitBarText = "<span style='color:#8b949e'>No commits yet</span>";
        }
    }
    QString crumbText;
    {
        QString acc;
        for (const QString &part : path.split('/', Qt::SkipEmptyParts)) {
            acc = acc.isEmpty() ? part : acc + "/" + part;
            crumbText += (crumbText.isEmpty() ? QString() : QStringLiteral(" / ")) +
                         QStringLiteral("<a href=\"") + acc.toHtmlEscaped() +
                         QStringLiteral("\">") + part.toHtmlEscaped() +
                         QStringLiteral("</a>");
        }
    }

    QWidget *page = m_filesStack ? m_filesStack->widget(0) : nullptr;
    if (page)
        page->setUpdatesEnabled(false);
    if (m_commitBar)
        m_commitBar->setText(commitBarText);
    if (QLabel *commitMeta = m_commitBar && m_commitBar->parentWidget()
                                 ? m_commitBar->parentWidget()->findChild<QLabel *>(
                                       QStringLiteral("commitBarMeta"))
                                 : nullptr) {
        commitMeta->setText(
            load.hasCommit
                ? QString::fromUtf8("<span style=\"font-family:'Geist Mono','SF Mono',"
                                    "Menlo,Consolas,monospace\">%1</span>"
                                    "&nbsp;&nbsp;\xC2\xB7&nbsp;&nbsp;%2")
                      .arg(load.commitHash.left(7).toHtmlEscaped(),
                           load.commitWhen.toHtmlEscaped())
                : QString());
        commitMeta->setToolTip(load.hasCommit ? load.commitHash : QString());
    }
    if (m_commitBarAvatar) {
        if (commitBarAvatar.isNull())
            m_commitBarAvatar->clear();
        else
            m_commitBarAvatar->setPixmap(commitBarAvatar);
        m_commitBarAvatar->setVisible(!commitBarAvatar.isNull());
    }
    if (m_overviewCrumb) {
        m_overviewCrumb->setText(crumbText);
        m_overviewCrumb->setVisible(!crumbText.isEmpty());
    }
    m_overviewRows = load.rows;
    m_overviewRepoBytes = load.repoBytes;
    if (QWidget *about = nextAboutSidebarIn(m_filesStack)) {
        if (path.isEmpty() && load.messageRow.isEmpty()) {
            QString license;
            for (const OverviewRow &row : std::as_const(m_overviewRows)) {
                if (!row.isDir && nextIsLicenseName(row.name)) {
                    license = row.path;
                    break;
                }
            }
            updateNextAboutLicense(about, license);
        }
        if (treeKey.isEmpty() || treeCache.sizeOut.isEmpty()) {
            if (!load.messageRow.isEmpty()) {
                about->setProperty("languagesKey", QString());
                updateNextAboutLanguages(about, {});
            }
        } else if (about->property("languagesKey").toString() != treeKey) {
            about->setProperty("languagesKey", treeKey);
            const QByteArray sizeOut = treeCache.sizeOut;
            runOffThread<QList<QPair<QString, qint64>>>(
                [sizeOut] { return nextLanguageTotals(sizeOut); },
                [this, treeKey](QList<QPair<QString, qint64>> totals) {
                    QWidget *sidebar = nextAboutSidebarIn(m_filesStack);
                    if (sidebar && sidebar->property("languagesKey").toString() == treeKey)
                        updateNextAboutLanguages(sidebar, totals);
                });
        }
    }
    if (!load.messageRow.isEmpty()) {
        m_overviewLoadedKey.clear();
        m_overviewList->clear();
        new QTreeWidgetItem(m_overviewList, {load.messageRow});
    } else {
        populateOverviewTree();
        m_overviewLoadedKey = keyPrefix + load.head;
        fillOverviewEntryCommits(dir, currentRef(), treeKey, m_overviewLoadedKey,
                                 load.pendingEntryLogs);
    }
    if (m_readmeView) {
        const bool hasReadme = !load.readmeMarkdown.isEmpty();
        if (hasReadme) {
            m_readmeView->setProperty("markdownPath", load.readmePath);
            renderReadmeMarkdown(
                m_readmeView, load.readmeMarkdown,
                QFileInfo(QDir(dir).filePath(load.readmePath)).absolutePath());
        } else {
            m_readmeView->clear();
        }
        QWidget *readmeCard = m_readmeView->parentWidget();
        if (readmeCard &&
            readmeCard->objectName() == QLatin1String("readmeCard")) {
            const bool card = Theme::isNext() && hasReadme;
            const bool restyle = readmeCard->property("framed").toBool() != card;
            readmeCard->setProperty("framed", card);
            readmeCard->setVisible(hasReadme || !Theme::isNext());
            if (auto *title = readmeCard->findChild<QLabel *>(
                    QStringLiteral("readmeCardTitle")))
                title->setText(QFileInfo(load.readmePath).fileName());
            if (auto *header = readmeCard->findChild<QFrame *>(
                    QStringLiteral("readmeCardHeader")))
                header->setVisible(card);
            if (restyle) {
                QList<QWidget *> restyled = readmeCard->findChildren<QWidget *>();
                restyled.prepend(readmeCard);
                for (QWidget *widget : std::as_const(restyled)) {
                    widget->style()->unpolish(widget);
                    widget->style()->polish(widget);
                    QEvent styleChange(QEvent::StyleChange);
                    QCoreApplication::sendEvent(widget, &styleChange);
                }
            }
            if (QWidget *body = readmeCard->parentWidget(); body && body->layout())
                body->layout()->setContentsMargins(
                    Theme::isNext() ? QMargins(0, 0, 16, 0) : QMargins(6, 0, 0, 0));
        }
        QTimer::singleShot(0, m_readmeView, [this] {
            if (!m_readmeView)
                return;
            fitReadmeImages(m_readmeView, qMax(1, m_readmeView->viewport()->width()));
            styleReadmeDocument(m_readmeView);
            m_readmeView->document()->setTextWidth(
                qMax(1, m_readmeView->viewport()->width()));
            const QMargins chrome = m_readmeView->contentsMargins();
            const int height = qCeil(m_readmeView->document()->size().height()) +
                               4 + chrome.top() + chrome.bottom();
            m_readmeView->setFixedHeight(qMax(0, height));
        });
    }
    if (page)
        page->setUpdatesEnabled(true);
    if (reloadPending)
        loadRepoOverview(path);
}

// The commit line for each listed entry: one `git log -1` per entry, all of it
// on one worker, folded back into the rows already on screen. The listing is
// usable without it, so nothing waits here.
void MainWindow::fillOverviewEntryCommits(const QString &dir, const QString &ref,
                                          const QString &treeKey,
                                          const QString &loadedKey,
                                          const QStringList &paths)
{
    if (paths.isEmpty() || dir.isEmpty())
        return;
    runOffThread<QHash<QString, QByteArray>>(
        [dir, ref, paths] {
            QHash<QString, QByteArray> records;
            for (const QString &path : paths) {
                QByteArray record;
                if (!runGitCapture(dir,
                                   {QStringLiteral("log"), QStringLiteral("-1"),
                                    QStringLiteral("--format=%ct%x1f%cr%x1f%s"),
                                    ref, QStringLiteral("--"), path},
                                   &record, nullptr))
                    continue;
                records.insert(path, record);
            }
            return records;
        },
        [this, treeKey, loadedKey](QHash<QString, QByteArray> records) {
            // The browse moved to another directory, commit or repository while
            // the worker ran: those rows are gone and this answer with them.
            if (m_overviewLoadedKey != loadedKey)
                return;
            OverviewTreeCache &cache = overviewTreeCache();
            const bool sameTree = !treeKey.isEmpty() && cache.key == treeKey;
            bool changed = false;
            for (auto it = records.cbegin(); it != records.cend(); ++it) {
                if (sameTree)
                    cache.entryLog.insert(it.key(), it.value());
                const QString text = QString::fromUtf8(it.value()).trimmed();
                if (text.isEmpty())
                    continue;
                const QStringList f = text.split('\x1f');
                for (OverviewRow &row : m_overviewRows) {
                    if (row.path != it.key())
                        continue;
                    row.commitTs = f.value(0).toLongLong();
                    row.whenText = f.value(1);
                    row.subject = f.value(2);
                    changed = true;
                    break;
                }
            }
            if (changed)
                populateOverviewTree();
        });
}

static QWidget *makeOverviewMetricTracks(double sizeFraction, double locFraction,
                                         double fileFraction,
                                         const QStringList &toolTips)
{
    auto *metrics = new QWidget;
    metrics->setFixedSize(16, 16);
    metrics->setToolTip(toolTips.join(QLatin1Char('\n')));
    const QList<double> fractions{sizeFraction, locFraction, fileFraction};
    auto *layout = new QHBoxLayout(metrics);
    layout->setContentsMargins(1, 0, 1, 0);
    layout->setSpacing(2);
    for (double fraction : fractions) {
        auto *track = new QFrame(metrics);
        track->setObjectName("overviewMetricTrack");
        track->setFixedSize(3, 16);
        auto *fill = new QFrame(track);
        fill->setObjectName("overviewMetricFill");
        const int height = fraction <= 0.0 ? 0
            : qMax(2, qRound(16 * qMin(1.0, fraction)));
        fill->setGeometry(0, 16 - height, 3, height);
        for (QWidget *child : {track, fill})
            child->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(track);
    }
    return metrics;
}

static QWidget *makeOverviewNameCell(const QIcon &icon, const QString &name,
                                     const QString &updated, double sizeFraction,
                                     double locFraction, double fileFraction,
                                     qint64 size, qint64 loc, qint64 files)
{
    auto *cell = new QWidget;
    cell->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    cell->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *layout = new QHBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    auto *iconLabel = new QLabel;
    iconLabel->setFixedSize(16, 16);
    iconLabel->setPixmap(icon.pixmap(16, 16));
    auto *nameLabel = new QLabel(name);
    nameLabel->setObjectName("overviewFileName");
    auto *updatedLabel = new QLabel(updated);
    updatedLabel->setObjectName("overviewUpdatedTime");
    layout->addWidget(iconLabel);
    layout->addWidget(makeOverviewMetricTracks(
        sizeFraction, locFraction, fileFraction,
        {QStringLiteral("Size: %1").arg(formatByteSize(size)),
         loc > 0 ? QStringLiteral("Lines of code: %1").arg(QLocale().toString(loc))
                 : QStringLiteral("No source lines"),
         files > 0 ? QStringLiteral("Files: %1").arg(QLocale().toString(files))
                   : QStringLiteral("No files")}));
    layout->addSpacing(5);
    layout->addWidget(nameLabel);
    layout->addStretch(1);
    layout->addSpacing(12);
    layout->addWidget(updatedLabel);
    for (QWidget *child : {iconLabel, nameLabel, updatedLabel})
        child->setAttribute(Qt::WA_TransparentForMouseEvents);
    return cell;
}

static QWidget *makeNextOverviewNameCell(const QIcon &icon, const QString &name,
                                         double sizeFraction, double locFraction,
                                         double fileFraction, qint64 size, qint64 loc,
                                         qint64 files)
{
    auto *cell = new QWidget;
    cell->setObjectName(QStringLiteral("overviewNameCell"));
    cell->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *layout = new QHBoxLayout(cell);
    layout->setContentsMargins(16, 0, 8, 0);
    layout->setSpacing(10);
    auto *iconLabel = new QLabel;
    iconLabel->setFixedSize(16, 16);
    iconLabel->setPixmap(icon.pixmap(16, 16));
    auto *nameLabel = new QLabel(name);
    nameLabel->setObjectName("overviewFileName");
    nameLabel->setTextFormat(Qt::PlainText);
    nameLabel->setMinimumWidth(0);
    nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QWidget *tracks = makeOverviewMetricTracks(
        sizeFraction, locFraction, fileFraction,
        {QStringLiteral("Size: %1").arg(formatByteSize(size)),
         loc > 0 ? QStringLiteral("Lines of code: %1").arg(QLocale().toString(loc))
                 : QStringLiteral("No source lines"),
         files > 0 ? QStringLiteral("Files: %1").arg(QLocale().toString(files))
                   : QStringLiteral("No files")});
    layout->addWidget(iconLabel);
    layout->addWidget(nameLabel, 1);
    layout->addWidget(tracks);
    for (QWidget *child : {iconLabel, nameLabel})
        child->setAttribute(Qt::WA_TransparentForMouseEvents);
    return cell;
}

void MainWindow::showOverviewLoadingPlaceholders()
{
    if (!m_overviewList)
        return;
    m_overviewList->setUpdatesEnabled(false);
    m_overviewList->clear();
    constexpr int kRows = 9;
    const bool next = m_overviewList->property("nextCodeTable").toBool();
    const int rowHeight = next ? kNextCodeRowHeight : 28;
    const QList<int> nameWidths{96, 144, 78, 122, 108, 154, 86, 132, 112};
    for (int row = 0; row < kRows; ++row) {
        auto *item = new QTreeWidgetItem(m_overviewList);
        item->setSizeHint(0, QSize(next ? kNextCodeNameWidth : 360, rowHeight));
        auto *left = new QWidget(m_overviewList);
        auto *layout = new QHBoxLayout(left);
        layout->setContentsMargins(next ? QMargins(16, 0, 8, 0) : QMargins(0, 0, 0, 0));
        layout->setSpacing(next ? 10 : 5);
        auto placeholder = [left](int width, int height) {
            auto *bar = new QFrame(left);
            bar->setObjectName("overviewLoadingPlaceholder");
            bar->setFixedSize(width, height);
            return bar;
        };
        layout->addWidget(placeholder(16, 16));
        if (!next) {
            layout->addWidget(placeholder(16, 16));
            layout->addSpacing(5);
        }
        layout->addWidget(placeholder(nameWidths.at(row), 11));
        layout->addStretch(1);
        if (!next)
            layout->addWidget(placeholder(30, 10));
        m_overviewList->setItemWidget(item, 0, left);
        if (next) {
            auto *right = new QWidget(m_overviewList);
            auto *rightLayout = new QHBoxLayout(right);
            rightLayout->setContentsMargins(12, 0, 16, 0);
            auto *bar = new QFrame(right);
            bar->setObjectName("overviewLoadingPlaceholder");
            bar->setFixedSize(150 - (row % 3) * 28, 10);
            rightLayout->addWidget(bar);
            rightLayout->addStretch(1);
            auto *when = new QFrame(right);
            when->setObjectName("overviewLoadingPlaceholder");
            when->setFixedSize(36, 10);
            rightLayout->addWidget(when);
            m_overviewList->setItemWidget(item, 1, right);
        } else {
            auto *commit = placeholder(150 - (row % 3) * 28, 10);
            m_overviewList->setItemWidget(item, 1, commit);
        }
    }
    const int headerHeight =
        m_overviewList->header()->isHidden() ? 0 : m_overviewList->header()->sizeHint().height();
    const int listHeight = m_overviewList->frameWidth() * 2 + headerHeight + kRows * rowHeight;
    m_overviewList->setFixedHeight(listHeight);
    m_overviewList->setUpdatesEnabled(true);
    m_overviewList->viewport()->update();
}

void MainWindow::populateOverviewTree()
{
    if (!m_overviewList)
        return;
    m_overviewList->setUpdatesEnabled(false);
    m_overviewList->clear();
    const bool next = m_overviewList->property("nextCodeTable").toBool();
    const int rowHeight = next ? kNextCodeRowHeight : 28;

    if (!m_overviewPath.isEmpty()) {
        auto *up = new QTreeWidgetItem(m_overviewList);
        if (next)
            up->setSizeHint(0, QSize(kNextCodeNameWidth, rowHeight));
        up->setIcon(0, iconForDir(false));
        up->setText(0, "..");
        const int cut = m_overviewPath.lastIndexOf('/');
        up->setData(0, Qt::UserRole, cut < 0 ? QString() : m_overviewPath.left(cut));
        up->setData(0, Qt::UserRole + 1, 1); // kind 1 = directory / up
    }

    const QString key = m_overviewSortKey;
    const bool desc = m_overviewSortDesc;
    QList<OverviewRow> rows = m_overviewRows;
    std::sort(rows.begin(), rows.end(),
              [&key, desc](const OverviewRow &a, const OverviewRow &b) {
                  if (key == QLatin1String("size")) {
                      if (a.size != b.size)
                          return desc ? a.size > b.size : a.size < b.size;
                  } else if (key == QLatin1String("loc")) {
                      if (a.loc != b.loc)
                          return desc ? a.loc > b.loc : a.loc < b.loc;
                  } else if (key == QLatin1String("files")) {
                      if (a.fileCount != b.fileCount)
                          return desc ? a.fileCount > b.fileCount
                                      : a.fileCount < b.fileCount;
                  } else if (key == QLatin1String("updated")) {
                      if (a.commitTs != b.commitTs)
                          return desc ? a.commitTs > b.commitTs
                                      : a.commitTs < b.commitTs;
                  } else if (key == QLatin1String("subject")) {
                      const int c = a.subject.compare(b.subject, Qt::CaseInsensitive);
                      if (c != 0)
                          return desc ? c > 0 : c < 0;
                  } else { // name: folders first, then by name
                      if (a.isDir != b.isDir)
                          return a.isDir;
                      const int c = a.name.compare(b.name, Qt::CaseInsensitive);
                      return desc ? c > 0 : c < 0;
                  }
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });

    qint64 maxSize = 0;
    qint64 maxLoc = 0;
    qint64 maxFiles = 0;
    for (const OverviewRow &e : std::as_const(rows)) {
        maxSize = qMax(maxSize, e.size);
        maxLoc = qMax(maxLoc, e.loc);
        maxFiles = qMax(maxFiles, e.fileCount);
    }

    for (const OverviewRow &e : std::as_const(rows)) {
        auto *item = new QTreeWidgetItem(m_overviewList);
        const QIcon icon = e.isDir ? iconForDir(false) : iconForFile(e.name);
        const QString updated = formatShortRelativeTime(e.commitTs);
        item->setSizeHint(
            0, QSize(QFontMetrics(m_overviewList->font()).horizontalAdvance(e.name) +
                         QFontMetrics(m_overviewList->font()).horizontalAdvance(updated) +
                         16 + 16 + 5 * 5 + 12,
                     rowHeight));
        item->setData(0, Qt::UserRole, e.path);
        item->setData(0, Qt::UserRole + 1, e.isDir ? 1 : 0);
        item->setToolTip(0, QStringLiteral("Size: %1\nLines of code: %2\nFiles: %3")
                                .arg(formatByteSize(e.size), QLocale().toString(e.loc),
                                     QLocale().toString(e.fileCount)));
        const double sizeFraction = maxSize > 0 ? double(e.size) / double(maxSize) : 0.0;
        const double locFraction = maxLoc > 0 ? double(e.loc) / double(maxLoc) : 0.0;
        const double fileFraction =
            maxFiles > 0 ? double(e.fileCount) / double(maxFiles) : 0.0;
        m_overviewList->setItemWidget(
            item, 0,
            next ? makeNextOverviewNameCell(icon, e.name, sizeFraction, locFraction,
                                            fileFraction, e.size, e.loc, e.fileCount)
                 : makeOverviewNameCell(icon, e.name, updated, sizeFraction, locFraction,
                                        fileFraction, e.size, e.loc, e.fileCount));
        item->setText(1, e.subject);
        if (next)
            item->setData(1, kNextOverviewTimeRole, updated);
        item->setToolTip(1, e.subject);
        item->setToolTip(0, item->toolTip(0) + QStringLiteral("\nUpdated: %1").arg(e.whenText));
    }
    int listHeight = m_overviewList->frameWidth() * 2;
    if (!m_overviewList->header()->isHidden())
        listHeight += m_overviewList->header()->sizeHint().height();
    for (int row = 0; row < m_overviewList->topLevelItemCount(); ++row)
        listHeight += qMax(rowHeight, m_overviewList->sizeHintForRow(row));
    m_overviewList->setFixedHeight(qMax(1, listHeight));
    m_overviewList->setUpdatesEnabled(true);
}

QString MainWindow::currentRef() const
{
    return m_repoBranch.isEmpty() ? QStringLiteral("HEAD") : m_repoBranch;
}

// The browsed branch can be deleted while the repo screen still points at it:
// merging it deletes it, so do "Delete merged branches", removing an agent's
// worktree with its branch, deleting a pull request, or a `git branch -D` in a
// terminal. Every git pass then runs against a ref that is gone and the file
// list shows git's "fatal: Not a valid object name <branch>" where the files
// belong. Fall back to the repo's default branch (or HEAD) so the screen keeps
// browsing something that exists.
MainWindow::BrowsedRefRecovery
MainWindow::readBrowsedRefRecovery(const QString &dir, const QString &gone,
                                   const QString &base)
{
    BrowsedRefRecovery recovery;
    recovery.dir = dir;
    recovery.gone = gone;
    recovery.base = base;
    if (dir.isEmpty() || gone.isEmpty())
        return recovery;
    recovery.goneResolves = gitRefResolves(dir, gone);
    if (!recovery.goneResolves && !base.isEmpty() && base != gone)
        recovery.baseResolves = gitRefResolves(dir, base);
    return recovery;
}

QString MainWindow::browsedRefRecoveryBase() const
{
    const QString base = repoDefaultBranchFast();
    return base == m_repoBranch ? QString() : base;
}

bool MainWindow::applyBrowsedRefRecovery(const BrowsedRefRecovery &recovery)
{
    const QString &gone = recovery.gone;
    if (gone.isEmpty() || recovery.goneResolves || m_repoBranch != gone ||
        recovery.dir != repoGitDir())
        return false;
    m_repoBranch = recovery.baseResolves && recovery.base != gone
                       ? recovery.base
                       : QString();
    if (m_branchesCache.contains(gone))
        m_branchesCache.clear();
    m_overviewLoadedKey.clear();
    if (m_branchButton)
        m_branchButton->setText(currentRef());
    updateCommitsBranchButtonLabel();
    logSystem(QStringLiteral("Git: branch %1 is gone — browsing %2 instead.")
                  .arg(gone, currentRef()));
    return true;
}

QString MainWindow::openRepoMirrorPath() const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return QString();
    return m_repositories.at(m_repoDetailIndex).mirrorPath;
}

MainWindow::CommitMarkerProbe MainWindow::readCommitTips(const QString &dir,
                                                         const QString &ref,
                                                         const QString &mirrorPath,
                                                         const QString &mirrorBranch)
{
    CommitMarkerProbe probe;
    QByteArray out;
    if (runGitCapture(dir, {QStringLiteral("rev-parse"), ref}, &out, nullptr))
        probe.localTip = QString::fromUtf8(out).trimmed();
    if (!mirrorPath.isEmpty() && QDir(mirrorPath).exists())
        probe.mirrorTip = mirrorBranchCommit(
            mirrorPath,
            mirrorBranch.isEmpty() ? mirrorHeadBranch(mirrorPath) : mirrorBranch);
    return probe;
}

void MainWindow::refreshCommitsUnlessCurrent()
{
    const QString dir = repoGitDir();
    if (!m_commitsTable || m_commitsTable->rowCount() == 0 ||
        m_commitsLoadedTip.isEmpty() || m_commitsLoadedRef != currentRef() ||
        dir.isEmpty()) {
        loadCommits();
        return;
    }
    const QString ref = currentRef();
    const QString mirrorPath = openRepoMirrorPath();
    const QString mirrorBranch = m_repoBranch;
    const int index = m_repoDetailIndex;
    const int loadGen = m_commitsLoadGen;
    runOffThread<CommitMarkerProbe>(
        [dir, ref, mirrorPath, mirrorBranch] {
            return readCommitTips(dir, ref, mirrorPath, mirrorBranch);
        },
        [this, index, ref, loadGen](CommitMarkerProbe probe) {
            if (index != m_repoDetailIndex || ref != currentRef() ||
                loadGen != m_commitsLoadGen)
                return;
            if (!probe.localTip.isEmpty() && probe.localTip == m_commitsLoadedTip &&
                probe.mirrorTip == m_commitsLoadedMirrorTip)
                refreshSourceControl();
            else
                loadCommits();
        });
}

QString MainWindow::commitMarkerProbeSignature() const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return {};
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    auto stamp = [](const QString &base, const QString &leaf) -> QString {
        if (base.trimmed().isEmpty())
            return QString();
        const QFileInfo info(QDir(base).filePath(leaf));
        return QString::number(info.lastModified().toMSecsSinceEpoch()) +
               QLatin1Char(':') + QString::number(info.size());
    };
    QString signature = currentRef();
    for (const QString &leaf :
         {QStringLiteral("HEAD"), QStringLiteral("packed-refs"),
          QStringLiteral("refs/heads"), QStringLiteral("logs/HEAD")}) {
        signature += QLatin1Char('|') + stamp(repo.mirrorPath, leaf) +
                     QLatin1Char('|') +
                     stamp(repo.localPath, QStringLiteral(".git/") + leaf);
    }
    return signature;
}

void MainWindow::refreshCommitMarkersIfStale()
{
    if (!m_commitsListPage || !m_commitsListPage->isVisible())
        return;
    if (m_commitMarkerProbeInFlight)
        return;
    if (!m_commitsTable || m_commitsTable->rowCount() == 0 ||
        m_commitsLoadedTip.isEmpty() || m_commitsLoadedRef != currentRef()) {
        loadCommits();
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString signature = commitMarkerProbeSignature();
    constexpr qint64 kProbeCeilingMs = 10 * 1000;
    constexpr qint64 kProbeFloorMs = 750;
    if (now - m_commitMarkerProbedAtMs < kProbeFloorMs)
        return;
    if (!signature.isEmpty() && signature == m_commitMarkerProbeSig &&
        now - m_commitMarkerProbedAtMs < kProbeCeilingMs)
        return;
    m_commitMarkerProbeSig = signature;
    m_commitMarkerProbedAtMs = now;

    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return;
    const QString ref = currentRef();
    const QString mirrorBranch = m_repoBranch;
    const QString mirrorPath = openRepoMirrorPath();
    const int index = m_repoDetailIndex;
    m_commitMarkerProbeInFlight = true;
    runOffThread<CommitMarkerProbe>(
        [dir, ref, mirrorPath, mirrorBranch] {
            return readCommitTips(dir, ref, mirrorPath, mirrorBranch);
        },
        [this, index, ref](CommitMarkerProbe probe) {
            m_commitMarkerProbeInFlight = false;
            if (index != m_repoDetailIndex || ref != currentRef())
                return;
            if (!m_commitsListPage || !m_commitsListPage->isVisible())
                return;
            if (probe.localTip.isEmpty())
                return; // unreadable ref: leave the list alone
            if (probe.localTip == m_commitsLoadedTip &&
                probe.mirrorTip == m_commitsLoadedMirrorTip)
                return; // nothing moved
            loadCommits();
        });
}

constexpr int kCommitSearchDepth = 5000;

MainWindow::CommitsLoad MainWindow::readCommits(const QString &dir,
                                                const QString &localPath,
                                                const QString &mirrorPath,
                                                const QString &mirrorBranch,
                                                const QString &ref, int rowLimit,
                                                const QString &relayHost)
{
    CommitsLoad load;
    const QStringList logArgs{
        "log",
        "--topo-order",
        "--decorate=full",
        "--format=%x1e%H%x1f%h%x1f%an%x1f%ar%x1f%ct%x1f%s%x1f%P%x1f%D%x1f%b",
        "-n",
        QString::number(rowLimit + 1),
        ref};
    if (!runGitCapture(dir, logArgs, &load.log, nullptr))
        return load;
    load.ok = true;
    auto collect = [&load](const QByteArray &out) {
        for (const QByteArray &line : out.split('\n')) {
            const QString hash = QString::fromUtf8(line).trimmed();
            if (!hash.isEmpty())
                load.unpushed.insert(hash);
        }
    };
    if (!mirrorPath.isEmpty() && QDir(mirrorPath).exists())
        load.mirrorTip = mirrorBranchCommit(
            mirrorPath,
            mirrorBranch.isEmpty() ? mirrorHeadBranch(mirrorPath) : mirrorBranch);
    if (!load.mirrorTip.isEmpty() && !localPath.isEmpty() &&
        QDir(localPath).exists()) {
        QByteArray out;
        if (runGitCapture(localPath, {"rev-list", load.mirrorTip + ".." + ref},
                          &out, nullptr))
            collect(out);
    }
    if (load.unpushed.isEmpty() &&
        !upstreamPublishesThroughMirror(dir, mirrorPath, relayHost)) {
        QByteArray out;
        if (runGitCapture(dir,
                          {QStringLiteral("rev-list"),
                           QStringLiteral("@{upstream}..") + ref},
                          &out, nullptr))
            collect(out);
    }
    return load;
}

void MainWindow::loadCommits()
{
    refreshSourceControl();
    if (!m_commitsTable)
        return;
    const int loadGen = ++m_commitsLoadGen;
    {
        QSignalBlocker block(m_commitsTable);
        TableRepaintGuard repaintGuard(m_commitsTable);
        m_commitsTable->setRowCount(0);
        m_commitsTable->setProperty(kGraphLaneCountProperty, 0);
    }
    showCommitList();
    if (m_repoDetailStack && m_insightsTabIndex >= 0 &&
        m_repoDetailStack->currentIndex() == m_insightsTabIndex)
        loadRepoInsights();
    const QString dir = repoGitDir();
    if (dir.isEmpty()) {
        m_commitsLoading = false;
        m_commitsLoadingMore = false;
        return;
    }
    const QString ref = currentRef();
    if (ref != m_commitsLoadedRef)
        m_commitsLimit = 300;
    m_commitsHasMore = false;
    const int rowLimit = m_commitsShowingAll ? kCommitSearchDepth : m_commitsLimit;
    const int index = m_repoDetailIndex;
    const QString localPath =
        index >= 0 && index < m_repositories.size()
            ? m_repositories.at(index).localPath
            : QString();
    const QString mirrorPath = openRepoMirrorPath();
    const QString mirrorBranch = m_repoBranch;
    const QString relayHost = configuredRelayHost();
    m_commitsLoading = true;
    runOffThread<CommitsLoad>(
        [dir, localPath, mirrorPath, mirrorBranch, ref, rowLimit, relayHost] {
            return readCommits(dir, localPath, mirrorPath, mirrorBranch, ref,
                               rowLimit, relayHost);
        },
        [this, loadGen, index, ref, rowLimit](CommitsLoad load) {
            if (loadGen != m_commitsLoadGen)
                return;
            m_commitsLoading = false;
            if (!m_commitsTable)
                return;
            if (index != m_repoDetailIndex || ref != currentRef()) {
                loadCommits();
                return;
            }
            applyCommits(loadGen, load, rowLimit);
            if (!m_currentCommitHash.isEmpty() && m_commitsStack &&
                m_commitsStack->currentIndex() == kCommitWorkspaceCommitPage)
                syncCurrentCommitRow(m_currentCommitHash);
            if (m_commitsLoadingMore) {
                m_commitsTable->verticalScrollBar()->setValue(m_commitsLoadMoreScroll);
                m_commitsLoadingMore = false;
            }
        });
}

void MainWindow::applyCommits(int loadGen, const CommitsLoad &load, int rowLimit)
{
    if (!load.ok)
        return;
    QSignalBlocker block(m_commitsTable);
    TableRepaintGuard repaintGuard(m_commitsTable);
    const QByteArray &out = load.log;
    const QSet<QString> &unpushed = load.unpushed;
    m_commitsUnsyncedHashes.clear();
    QString loadedTip;
    QList<QString> activeLanes;
    int graphLaneCount = 0;
    if (!unpushed.isEmpty()) {
        const int row = m_commitsTable->rowCount();
        m_commitsTable->insertRow(row);
        auto *graphItem = new QTableWidgetItem;
        graphItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        graphItem->setData(kGraphNodeLaneRole, 0);
        graphItem->setData(kGraphBottomLanesRole, QVariantList{0});
        graphItem->setData(kGraphIsMergeRole, false);
        m_commitsTable->setItem(row, kCommitGraphCol, graphItem);

        auto *summary = new SortTableWidgetItem(QStringLiteral("Outgoing Changes"));
        summary->setData(kCommitRowKindRole, 2); // decorative graph-state row
        summary->setData(kCommitOutgoingRole, true);
        summary->setData(kCommitAuthorRole,
                         QStringLiteral("%1 pending sync")
                             .arg(unpushed.size()));
        summary->setData(kCommitRefsRole,
                         QStringList{QStringLiteral("%1 %2↑")
                                         .arg(currentRef())
                                         .arg(unpushed.size())});
        summary->setData(kCommitRefKindsRole, QStringList{QStringLiteral("local")});
        summary->setToolTip(
            QStringLiteral("%1 local commit%2 waiting to sync. Click to show files, "
                           "then Sync Changes to publish them.")
                .arg(unpushed.size())
                .arg(unpushed.size() == 1 ? QString() : QStringLiteral("s")));
        m_commitsTable->setItem(row, kCommitSummaryCol, summary);
    }
    const CommitStatusIndex commitStatusIndex = buildCommitStatusIndex();
    for (const QByteArray &record : out.split('\x1e')) {
        if (record.trimmed().isEmpty())
            continue;
        if (m_commitsTable->rowCount() >= rowLimit) {
            m_commitsHasMore = true;
            break;
        }
        const QStringList f =
            QString::fromUtf8(record).split(QLatin1Char('\x1f'));
        if (f.size() < 6)
            continue;
        if (loadedTip.isEmpty())
            loadedTip = f.at(0);
        const bool isUnpushed = unpushed.contains(f.at(0));

        const QString hash = f.at(0);
        const QStringList parents =
            f.value(6).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        int nodeLane = activeLanes.indexOf(hash);
        if (nodeLane < 0) {
            nodeLane = activeLanes.indexOf(QString()); // reuse a freed slot
            if (nodeLane < 0) {
                nodeLane = activeLanes.size();
                activeLanes.append(hash);
            } else {
                activeLanes[nodeLane] = hash;
            }
        }
        QVariantList laneCols;
        for (int i = 0; i < activeLanes.size(); ++i) {
            if (!activeLanes.at(i).isEmpty())
                laneCols.append(i);
        }
        for (int i = 0; i < activeLanes.size(); ++i)
            if (i != nodeLane && activeLanes.at(i) == hash)
                activeLanes[i].clear();
        if (parents.isEmpty()) {
            activeLanes[nodeLane].clear(); // root commit: the lane ends
        } else {
            activeLanes[nodeLane] = parents.at(0);
            for (int k = 1; k < parents.size(); ++k) {
                if (activeLanes.indexOf(parents.at(k)) >= 0)
                    continue; // that parent already has a lane
                int slot = activeLanes.indexOf(QString());
                if (slot < 0) {
                    slot = activeLanes.size();
                    activeLanes.append(parents.at(k));
                } else {
                    activeLanes[slot] = parents.at(k);
                }
            }
        }
        while (!activeLanes.isEmpty() && activeLanes.last().isEmpty())
            activeLanes.removeLast(); // keep the gutter as narrow as the history
        QVariantList botLaneCols;
        for (int i = 0; i < activeLanes.size(); ++i) {
            if (!activeLanes.at(i).isEmpty())
                botLaneCols.append(i);
        }
        graphLaneCount = std::max({graphLaneCount, nodeLane + 1,
                                   laneCols.isEmpty()
                                       ? 0
                                       : laneCols.last().toInt() + 1,
                                   botLaneCols.isEmpty()
                                       ? 0
                                       : botLaneCols.last().toInt() + 1});

        const int row = m_commitsTable->rowCount();
        m_commitsTable->insertRow(row);
        auto *graphItem = new QTableWidgetItem;
        graphItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        graphItem->setData(kGraphLanesRole, laneCols);
        graphItem->setData(kGraphNodeLaneRole, nodeLane);
        graphItem->setData(kGraphBottomLanesRole, botLaneCols);
        graphItem->setData(kGraphIsMergeRole, parents.size() > 1);
        m_commitsTable->setItem(row, kCommitGraphCol, graphItem);
        auto *summary = new SortTableWidgetItem(f.at(5));
        summary->setData(Qt::UserRole, f.at(0));
        summary->setData(kTableSortRole, f.at(5).toLower());
        summary->setData(kCommitRowKindRole, 0);
        summary->setData(kCommitExpandedRole, false);
        summary->setData(kCommitAuthorRole, f.at(2));
        summary->setData(kCommitUnsyncedRole, isUnpushed);
        const QString msgBody = f.value(8).trimmed();
        if (!msgBody.isEmpty())
            summary->setData(kCommitBodyRole, msgBody);
        {
            QStringList refs;
            QStringList refKinds;
            const QStringList rawRefs = f.value(7).split(
                QStringLiteral(", "), Qt::SkipEmptyParts);
            for (QString ref : rawRefs) {
                ref = ref.trimmed();
                QString kind = QStringLiteral("local");
                if (ref.startsWith(QLatin1String("HEAD -> "))) {
                    ref = ref.mid(8);
                } else if (ref == QLatin1String("HEAD")) {
                    continue; // detached HEAD marker, not a ref
                } else if (ref.startsWith(QLatin1String("tag: "))) {
                    ref = ref.mid(5);
                    kind = QStringLiteral("tag");
                }
                if (ref.startsWith(QLatin1String("refs/heads/"))) {
                    ref = ref.mid(11);
                    kind = QStringLiteral("local");
                } else if (ref.startsWith(QLatin1String("refs/remotes/"))) {
                    ref = ref.mid(13);
                    kind = QStringLiteral("remote");
                } else if (ref.startsWith(QLatin1String("refs/tags/"))) {
                    ref = ref.mid(10);
                    kind = QStringLiteral("tag");
                }
                refs << ref;
                refKinds << kind;
                if (refs.size() >= 3)
                    break;
            }
            if (!refs.isEmpty()) {
                summary->setData(kCommitRefsRole, refs);
                summary->setData(kCommitRefKindsRole, refKinds);
            }
        }
        const int statusCode = commitStatusCode(f.at(0), commitStatusIndex);
        switch (statusCode) {
        case 1:
            summary->setIcon(themedOcticon("check-circle", QColor("#3fb950"), 14));
            break;
        case 2:
            summary->setIcon(themedOcticon("x", QColor("#f85149"), 14));
            break;
        case 3:
            summary->setIcon(themedOcticon("sync", QColor("#58a6ff"), 14));
            break;
        default:
            break;
        }
        m_commitsTable->setItem(row, kCommitSummaryCol, summary);

        auto *author = new SortTableWidgetItem(f.at(2));
        author->setData(kTableSortRole, f.at(2).toLower());
        m_commitsTable->setItem(row, 0, author);
        const qint64 commitTs = f.at(4).toLongLong();
        auto *date = new SortTableWidgetItem(formatShortRelativeTime(commitTs));
        date->setData(kTableSortRole, commitTs);
        date->setToolTip(f.at(3)); // full "x ago" form on hover
        m_commitsTable->setItem(row, 1, date);
        auto *hashItem = new SortTableWidgetItem(
            isUnpushed ? QString::fromUtf8("\xE2\x96\xB2 ") + f.at(1) : f.at(1));
        hashItem->setData(kTableSortRole, f.at(1));
        if (isUnpushed) {
            m_commitsUnsyncedHashes << f.at(0);
            hashItem->setForeground(QColor("#d29922"));
            hashItem->setToolTip(
                QStringLiteral("%1 — not yet synced to the network mirror").arg(f.at(1)));
        } else {
            hashItem->setToolTip(f.at(1));
        }
        m_commitsTable->setItem(row, kCommitHashCol, hashItem);
        const QString pending = QString::fromUtf8("\xC2\xB7"); // "·"
        auto *fileItem = new SortTableWidgetItem(pending);
        fileItem->setData(kTableSortRole, 0);
        m_commitsTable->setItem(row, 3, fileItem);
        auto *addsItem = new SortTableWidgetItem(pending);
        addsItem->setForeground(QColor("#2ea043"));
        addsItem->setData(kTableSortRole, 0);
        m_commitsTable->setItem(row, 4, addsItem);
        auto *delsItem = new SortTableWidgetItem(pending);
        delsItem->setForeground(QColor("#f85149"));
        delsItem->setData(kTableSortRole, 0);
        m_commitsTable->setItem(row, 5, delsItem);
        updateCommitRowHover(row, statusCode);
    }
    m_commitsTable->setProperty(kGraphLaneCountProperty, graphLaneCount);

    updateCommitsUnsyncedFilesPanel();
    if (m_commitsPullButton) {
        const bool writable = repoHasWorkingTree();
        m_commitsPullButton->setEnabled(writable);
        m_commitsPullButton->setToolTip(
            writable ? QStringLiteral("Fast-forward the working tree to the "
                                      "latest fetched history (git pull --ff-only)")
                     : QString::fromUtf8("Read-only mirror \xE2\x80\x94 no working "
                                      "tree to pull into"));
    }

    if (m_commitSearch && !m_commitSearch->text().trimmed().isEmpty())
        filterCommits(m_commitSearch->text());

    // Record what's now on screen so a repeat tab click can skip this whole
    // rebuild while the branch and tip are unchanged. The mirror tip is part of
    // that signature: the markers go stale if it advances under an unchanged
    // local tip (see refreshCommitsUnlessCurrent).
    m_commitsLoadedRef = currentRef();
    m_commitsLoadedTip = loadedTip;
    m_commitsLoadedMirrorTip = load.mirrorTip;
    refreshCommitsBranchButton(); // keep the branch indicator + switcher in sync

    QTimer::singleShot(0, this, [this, loadGen] { fillCommitStats(loadGen); });

    QTimer::singleShot(0, this, [this] { applyCommitIssueClosures(); });
}

struct CommitStatTotals {
    int files = 0;
    int adds = 0;
    int dels = 0;
    QStringList preview;
};

void MainWindow::fillCommitStats(int loadGen)
{
    if (loadGen != m_commitsLoadGen || !m_commitsTable)
        return;
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return;
    QStringList args{"log", "--numstat", "--format=%x1e%H"};
    args << "--diff-merges=first-parent";
    args << "-n"
         << QString::number(m_commitsShowingAll ? kCommitSearchDepth
                                                : m_commitsLimit);
    args << currentRef();

    runOffThread<QHash<QString, CommitStatTotals>>(
        [dir, args] {
            QHash<QString, CommitStatTotals> stats;
            QByteArray out;
            if (!runGitCapture(dir, args, &out, nullptr)) {
                QStringList plain = args;
                plain.removeAll(QStringLiteral("--diff-merges=first-parent"));
                if (!runGitCapture(dir, plain, &out, nullptr))
                    return stats;
            }
            constexpr int kHoverFilePreview = 12;
            for (const QByteArray &record : out.split('\x1e')) {
                const QStringList lines = QString::fromUtf8(record).split(
                    QLatin1Char('\n'), Qt::SkipEmptyParts);
                if (lines.isEmpty())
                    continue;
                CommitStatTotals s;
                for (int i = 1; i < lines.size(); ++i) {
                    const QStringList cols = lines.at(i).split(QLatin1Char('\t'));
                    if (cols.size() < 3)
                        continue;
                    ++s.files;
                    bool ok = false;
                    const int addCount =
                        cols.at(0).toInt(&ok); // "-" for binary → skipped
                    if (ok)
                        s.adds += addCount;
                    const int delCount = cols.at(1).toInt(&ok);
                    if (ok)
                        s.dels += delCount;
                    if (s.preview.size() < kHoverFilePreview)
                        s.preview << QStringLiteral("%1\t%2\t%3")
                                         .arg(cols.mid(2).join(QLatin1Char('\t')),
                                              cols.at(0), cols.at(1));
                }
                stats.insert(lines.first(), s);
            }
            return stats;
        },
        [this, loadGen](QHash<QString, CommitStatTotals> stats) {
            if (loadGen != m_commitsLoadGen || !m_commitsTable)
                return;

            QSignalBlocker block(m_commitsTable);
            TableRepaintGuard repaintGuard(m_commitsTable);
            const CommitStatusIndex statusIndex = buildCommitStatusIndex();
            const bool wasSorting = m_commitsTable->isSortingEnabled();
            m_commitsTable->setSortingEnabled(false);
            for (int row = 0; row < m_commitsTable->rowCount(); ++row) {
                QTableWidgetItem *sum = m_commitsTable->item(row, kCommitSummaryCol);
                if (!sum || sum->data(kCommitRowKindRole).toInt() != 0)
                    continue; // expanded file rows share the hash — commits only
                const auto it = stats.constFind(sum->data(Qt::UserRole).toString());
                if (it == stats.constEnd())
                    continue;
                const CommitStatTotals &s = it.value();
                sum->setData(kCommitFilesRole, s.preview);
                if (QTableWidgetItem *f = m_commitsTable->item(row, 3)) {
                    f->setText(QString::number(s.files));
                    f->setData(kTableSortRole, s.files);
                }
                if (QTableWidgetItem *a = m_commitsTable->item(row, 4)) {
                    a->setText(QStringLiteral("+%1").arg(s.adds));
                    a->setData(kTableSortRole, s.adds);
                }
                if (QTableWidgetItem *d = m_commitsTable->item(row, 5)) {
                    d->setText(QString::fromUtf8("\xE2\x88\x92%1").arg(s.dels));
                    d->setData(kTableSortRole, s.dels);
                }
                updateCommitRowHover(
                    row, commitStatusCode(sum->data(Qt::UserRole).toString(),
                                          statusIndex)); // the hover box shows these counts
            }
            m_commitsTable->setSortingEnabled(wasSorting);
        });
}

void MainWindow::loadMoreCommits()
{
    if (m_commitsLoadingMore || !m_commitsHasMore || !m_commitsTable)
        return;
    m_commitsLoadingMore = true;
    const int scrollVal = m_commitsTable->verticalScrollBar()->value();
    m_commitsLimit += 300;
    m_commitsLoadMoreScroll = scrollVal;
    loadCommits();
}

struct CommitFileStat {
    QString path;
    int adds = 0;
    int dels = 0;
};
struct UnsyncedCommitFiles {
    QString hash;
    QString subject;
    QList<CommitFileStat> files;
};

static QList<CommitFileStat> commitFileStats(const QString &dir,
                                             const QString &hash)
{
    QList<CommitFileStat> files;
    QByteArray out;
    const QStringList base{QStringLiteral("diff-tree"), QStringLiteral("--root"),
                           QStringLiteral("--no-commit-id"),
                           QStringLiteral("--numstat"), QStringLiteral("-r"),
                           QStringLiteral("-M")};
    if (!runGitCapture(dir,
                       base + QStringList{
                                  QStringLiteral("--diff-merges=first-parent"),
                                  hash},
                       &out, nullptr) ||
        out.isEmpty()) {
        if (!runGitCapture(dir, base + QStringList{hash}, &out, nullptr))
            return files;
    }
    const QStringList lines =
        QString::fromUtf8(out).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList cols = line.split(QLatin1Char('\t'));
        if (cols.size() < 3)
            continue;
        CommitFileStat f;
        f.path = cols.mid(2).join(QLatin1Char('\t'));
        f.adds = cols.at(0).toInt(); // "-" for binary → 0
        f.dels = cols.at(1).toInt();
        files << f;
    }
    return files;
}

static void insertCommitFileRowsInto(QTableWidget *table, int row, const QString &hash,
                                     const QList<CommitFileStat> &files,
                                     const std::function<QIcon(const QString &)> &icon)
{
    QTableWidgetItem *sum = table->item(row, kCommitSummaryCol);
    QSignalBlocker block(table);
    const QTableWidgetItem *graphIt = table->item(row, kCommitGraphCol);
    const QVariantList lanes =
        graphIt ? graphIt->data(kGraphBottomLanesRole).toList() : QVariantList();
    const int lane = graphIt ? graphIt->data(kGraphNodeLaneRole).toInt() : 0;
    int at = row + 1;
    for (const CommitFileStat &f : files) {
        table->insertRow(at);
        auto *graph = new QTableWidgetItem;
        graph->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        graph->setData(kGraphLanesRole, lanes);
        graph->setData(kGraphBottomLanesRole, lanes);
        graph->setData(kGraphNodeLaneRole, -1); // no dot: lanes pass through
        table->setItem(at, kCommitGraphCol, graph);
        auto *item = new QTableWidgetItem(f.path);
        item->setIcon(icon(f.path.section(QLatin1Char('/'), -1)));
        item->setData(kCommitRowKindRole, 1);
        item->setData(Qt::UserRole, hash);
        item->setData(kCommitFilePathRole, f.path);
        item->setData(kCommitFileAddsRole, f.adds);
        item->setData(kCommitFileDelsRole, f.dels);
        item->setData(kGraphNodeLaneRole, lane); // indent under the commit
        item->setToolTip(
            QString::fromUtf8("%1 \xC2\xB7 +%2 \xE2\x88\x92%3<br>"
                              "Click to open this file's diff")
                .arg(f.path.toHtmlEscaped())
                .arg(f.adds)
                .arg(f.dels));
        table->setItem(at, kCommitSummaryCol, item);
        ++at;
    }
    sum->setData(kCommitExpandedRole, true);
}

void MainWindow::toggleCommitFilesRows(int row)
{
    if (!m_commitsTable)
        return;
    QTableWidgetItem *sum = m_commitsTable->item(row, kCommitSummaryCol);
    if (!sum || sum->data(kCommitRowKindRole).toInt() != 0)
        return;
    const QString hash = sum->data(Qt::UserRole).toString();
    QSignalBlocker block(m_commitsTable);
    if (sum->data(kCommitExpandedRole).toBool()) {
        while (row + 1 < m_commitsTable->rowCount()) {
            QTableWidgetItem *next =
                m_commitsTable->item(row + 1, kCommitSummaryCol);
            if (!next || next->data(kCommitRowKindRole).toInt() != 1)
                break;
            m_commitsTable->removeRow(row + 1);
        }
        sum->setData(kCommitExpandedRole, false);
        return;
    }
    const QString dir = repoGitDir();
    if (dir.isEmpty() || hash.isEmpty() || m_commitFilesLoading.contains(hash))
        return;
    m_commitFilesLoading.insert(hash);
    runOffThread<QList<CommitFileStat>>(
        [dir, hash] { return commitFileStats(dir, hash); },
        [this, dir, hash](QList<CommitFileStat> files) {
            m_commitFilesLoading.remove(hash);
            if (!m_commitsTable || dir != repoGitDir() || files.isEmpty())
                return;
            for (int row = 0; row < m_commitsTable->rowCount(); ++row) {
                QTableWidgetItem *sum = m_commitsTable->item(row, kCommitSummaryCol);
                if (!sum || sum->data(kCommitRowKindRole).toInt() != 0 ||
                    sum->data(Qt::UserRole).toString() != hash)
                    continue;
                if (!sum->data(kCommitExpandedRole).toBool())
                    insertCommitFileRowsInto(
                        m_commitsTable, row, hash, files,
                        [this](const QString &name) { return iconForFile(name); });
                return;
            }
        });
}


void MainWindow::collapseAllCommitFileRows()
{
    if (!m_commitsTable)
        return;
    QSignalBlocker block(m_commitsTable);
    for (int row = m_commitsTable->rowCount() - 1; row >= 0; --row) {
        QTableWidgetItem *sum = m_commitsTable->item(row, kCommitSummaryCol);
        if (!sum)
            continue;
        if (sum->data(kCommitRowKindRole).toInt() == 1)
            m_commitsTable->removeRow(row);
        else
            sum->setData(kCommitExpandedRole, false);
    }
}

void MainWindow::updateCommitRowHover(int row, int statusCode)
{
    if (!m_commitsTable)
        return;
    QTableWidgetItem *sum = m_commitsTable->item(row, kCommitSummaryCol);
    if (!sum || sum->data(kCommitRowKindRole).toInt() != 0)
        return;
    const QTableWidgetItem *author = m_commitsTable->item(row, 0);
    const QTableWidgetItem *date = m_commitsTable->item(row, 1);
    const QTableWidgetItem *hashIt = m_commitsTable->item(row, kCommitHashCol);
    const QTableWidgetItem *filesIt = m_commitsTable->item(row, 3);
    const QTableWidgetItem *addsIt = m_commitsTable->item(row, 4);
    const QTableWidgetItem *delsIt = m_commitsTable->item(row, 5);
    const QString hash = sum->data(Qt::UserRole).toString();
    const QString shortHash =
        hashIt ? hashIt->data(kTableSortRole).toString() : hash.left(8);
    const QString when = date ? date->toolTip() : QString();
    QString html = QStringLiteral("<b>%1</b>").arg(sum->text().toHtmlEscaped());
    QString msgBody = sum->data(kCommitBodyRole).toString();
    if (!msgBody.isEmpty()) {
        if (msgBody.size() > 1500) {
            msgBody.truncate(1500);
            msgBody += QChar(0x2026);
        }
        html += QStringLiteral("<br><span style='color:#8b949e; "
                               "white-space:pre-wrap'>%1</span>")
                    .arg(msgBody.toHtmlEscaped());
    }
    html += QStringLiteral("<br>%1 committed %2")
                .arg((author ? author->text() : QString()).toHtmlEscaped(),
                     when.toHtmlEscaped());
    html += QStringLiteral("<br><code>%1</code>").arg(shortHash.toHtmlEscaped());
    const QStringList refs = sum->data(kCommitRefsRole).toStringList();
    if (!refs.isEmpty())
        html += QStringLiteral("<br><span style='color:%1'>%2</span>")
                    .arg(Theme::nextOrHex(Theme::Role::AccentFg, QStringLiteral("#58a6ff")),
                         refs.join(QString::fromUtf8(" \xC2\xB7 ")).toHtmlEscaped());
    const QString filesTxt = filesIt ? filesIt->text() : QString();
    if (!filesTxt.isEmpty() && filesTxt != QString::fromUtf8("\xC2\xB7")) {
        html += QString::fromUtf8("<br>%1 file%2 changed \xC2\xB7 "
                                  "<span style='color:#3fb950'>%3</span> "
                                  "<span style='color:#f85149'>%4</span>")
                    .arg(filesTxt, filesTxt == QStringLiteral("1") ? "" : "s",
                         addsIt ? addsIt->text() : QString(),
                         delsIt ? delsIt->text() : QString());
        const QStringList preview = sum->data(kCommitFilesRole).toStringList();
        for (const QString &entry : preview) {
            const QStringList cols = entry.split(QLatin1Char('\t'));
            if (cols.isEmpty())
                continue;
            html += QString::fromUtf8(
                        "<br><span style='color:#8b949e'>%1</span> "
                        "<span style='color:#3fb950'>+%2</span> "
                        "<span style='color:#f85149'>\xE2\x88\x92%3</span>")
                        .arg(cols.first().toHtmlEscaped())
                        .arg(cols.value(1).toInt())  // "-" for binary → 0
                        .arg(cols.value(2).toInt());
        }
        const int total = filesTxt.toInt();
        if (total > preview.size() && !preview.isEmpty())
            html += QString::fromUtf8("<br><span style='color:#8b949e'>"
                                      "and %1 more\xE2\x80\xA6</span>")
                        .arg(total - preview.size());
    }
    switch (statusCode >= 0 ? statusCode : commitStatusCode(hash)) {
    case 1:
        html += QString::fromUtf8(
            "<br><span style='color:#3fb950'>Checks passed</span>");
        break;
    case 2:
        html += QString::fromUtf8(
            "<br><span style='color:#f85149'>Checks failed</span>");
        break;
    case 3:
        html += QString::fromUtf8(
            "<br><span style='color:#58a6ff'>Checks running</span>");
        break;
    default:
        break;
    }
    if (sum->data(kCommitUnsyncedRole).toBool())
        html += QString::fromUtf8("<br><span style='color:#d29922'>\xE2\x96\xB2 "
                                  "Not yet synced to the network mirror</span>");
    html += QString::fromUtf8("<br><span style='color:#8b949e'>Click to show "
                              "files \xC2\xB7 double-click for the full "
                              "diff</span>");
    sum->setToolTip(html);
}

void MainWindow::updateCommitsUnsyncedFilesPanel()
{
    if (!m_commitsUnsyncedBanner || !m_commitsUnsyncedFiles ||
        !m_commitsUnsyncedSyncButton)
        return;

    const int pendingCount = m_commitsUnsyncedHashes.size();
    if (pendingCount <= 0) {
        ++m_commitsUnsyncedFilesGen;
        m_commitsUnsyncedExpanded = false;
        if (m_commitsBannerFade)
            m_commitsBannerFade->stop();
        m_commitsUnsyncedBanner->hide();
        m_commitsUnsyncedSyncButton->hide();
        m_commitsUnsyncedFiles->hide();
        return;
    }

    if (m_commitsBannerFade)
        m_commitsBannerFade->stop();
    if (m_commitsBannerOpacity)
        m_commitsBannerOpacity->setOpacity(1.0);

    const bool syncing = m_pushingRepos.contains(m_repoDetailIndex) ||
                        m_syncingRepos.contains(m_repoDetailIndex);
    m_commitsUnsyncedSyncButton->setVisible(true);
    m_commitsUnsyncedSyncButton->setEnabled(!syncing);
    m_commitsUnsyncedSyncButton->setText(
        pendingCount > 0 ? QStringLiteral("Sync Changes %1↑").arg(pendingCount)
                         : QStringLiteral("Sync Changes"));
    if (syncing)
        startButtonSpin(m_commitsUnsyncedSyncButton);
    else
        stopButtonSpin(m_commitsUnsyncedSyncButton);
    setOcticon(m_commitsUnsyncedSyncButton, QStringLiteral("sync"), 14);
    m_commitsUnsyncedSyncButton->setToolTip(
        QStringLiteral("Publish outgoing commits to the network mirror or push "
                       "them to the configured upstream branch"));

    const QString filesLabel =
        m_commitsUnsyncedExpanded ? QStringLiteral("Hide files")
                                  : QStringLiteral("Show files");
    m_commitsUnsyncedBanner->setText(
        QStringLiteral("%1 local commit%2 waiting to sync. <a href=\"commits\">%3</a>")
            .arg(pendingCount)
            .arg(pendingCount == 1 ? QString() : QStringLiteral("s"))
            .arg(filesLabel));
    m_commitsUnsyncedBanner->show();

    if (!m_commitsUnsyncedExpanded) {
        ++m_commitsUnsyncedFilesGen;
        m_commitsUnsyncedFiles->hide();
        return;
    }

    const QString dir = repoGitDir();
    if (dir.isEmpty()) {
        m_commitsUnsyncedFiles->hide();
        return;
    }

    QHash<QString, QString> commitSubjects;
    if (m_commitsTable) {
        for (int row = 0; row < m_commitsTable->rowCount(); ++row) {
            QTableWidgetItem *sum = m_commitsTable->item(row, kCommitSummaryCol);
            if (!sum || sum->data(kCommitRowKindRole).toInt() != 0)
                continue;
            const QString hash = sum->data(Qt::UserRole).toString();
            if (!hash.isEmpty())
                commitSubjects.insert(hash, sum->text());
        }
    }

    const QStringList hashes = m_commitsUnsyncedHashes;
    const quint64 generation = ++m_commitsUnsyncedFilesGen;
    runOffThread<QList<UnsyncedCommitFiles>>(
        [dir, hashes, commitSubjects] {
            QList<UnsyncedCommitFiles> entries;
            for (const QString &hash : hashes) {
                QList<CommitFileStat> files = commitFileStats(dir, hash);
                if (files.isEmpty())
                    continue;
                UnsyncedCommitFiles entry;
                entry.hash = hash;
                entry.subject = commitSubjects.value(hash);
                entry.files = std::move(files);
                entries << entry;
            }
            return entries;
        },
        [this, generation, dir, hashes](QList<UnsyncedCommitFiles> entries) {
            if (generation != m_commitsUnsyncedFilesGen || !m_commitsUnsyncedFiles ||
                !m_commitsUnsyncedExpanded || dir != repoGitDir() ||
                hashes != m_commitsUnsyncedHashes)
                return;
            m_commitsUnsyncedFiles->clear();
            if (entries.isEmpty()) {
                auto *placeholder = new QTreeWidgetItem(m_commitsUnsyncedFiles);
                placeholder->setText(0, QStringLiteral("No file details available yet."));
                m_commitsUnsyncedFiles->show();
                return;
            }

            for (const UnsyncedCommitFiles &entry : entries) {
                auto *commitItem = new QTreeWidgetItem(m_commitsUnsyncedFiles);
                commitItem->setText(
                    0,
                    entry.subject.isEmpty()
                        ? entry.hash.left(8)
                        : QStringLiteral("%1 — %2").arg(entry.hash.left(8),
                                                       entry.subject));
                commitItem->setIcon(0, themedOcticon("git-commit", Theme::nextOr(Theme::Role::FgMuted, QColor("#58a6ff")), 14));
                commitItem->setData(0, Qt::UserRole, entry.hash);
                for (const CommitFileStat &file : entry.files) {
                    auto *fileItem = new QTreeWidgetItem(commitItem);
                    fileItem->setIcon(0, iconForFile(file.path.section(QLatin1Char('/'), -1)));
                    fileItem->setText(0, QString::fromUtf8("%1  +%2 \xE2\x88\x92%3")
                                               .arg(file.path)
                                               .arg(file.adds)
                                               .arg(file.dels));
                    fileItem->setData(0, Qt::UserRole, entry.hash);
                    fileItem->setData(0, Qt::UserRole + 1, file.path);
                    fileItem->setToolTip(
                        0,
                        QString::fromUtf8("%1 \xC2\xB7 +%2 \xE2\x88\x92%3").arg(
                            file.path, QString::number(file.adds), QString::number(file.dels)));
                }
                commitItem->setExpanded(true);
            }
            m_commitsUnsyncedFiles->show();
        });
}

namespace {

QString gitReason(const QString &gitOutput, const QString &fallback)
{
    const QString line = forkmesh::gitrefusal::firstFailureLine(gitOutput);
    return line.isEmpty() ? fallback : line.left(240);
}

QString fetchFailureText(const QString &gitOutput)
{
    const QString reason =
        gitReason(gitOutput, QStringLiteral("git fetch did not finish"));
    const QString explained = forkmesh::gitrefusal::explainFailure(gitOutput);
    if (!explained.isEmpty())
        return QStringLiteral("Fetch failed: %1 (%2)").arg(explained, reason);
    if (forkmesh::gitrefusal::remoteUnavailable(gitOutput))
        return QStringLiteral("Fetch failed: a remote could not be reached (%1). "
                              "Try again shortly.")
            .arg(reason);
    return QStringLiteral("Fetch failed: %1").arg(reason);
}

struct PullFailureNotice {
    QString text;
    bool guidance = false;
};

PullFailureNotice pullFailureNotice(const QString &gitOutput)
{
    if (forkmesh::gitrefusal::notFastForward(gitOutput))
        return {QStringLiteral("Pull stopped: this branch has commits its "
                               "upstream doesn't, so it can't fast-forward. "
                               "Merge or rebase it by hand."),
                true};
    if (forkmesh::gitrefusal::overLocalChanges(gitOutput))
        return {QStringLiteral("Pull stopped: it would overwrite uncommitted "
                               "changes. Commit or stash them first."),
                true};
    const QString reason =
        gitReason(gitOutput, QStringLiteral("git pull did not finish"));
    const QString explained = forkmesh::gitrefusal::explainFailure(gitOutput);
    if (!explained.isEmpty())
        return {QStringLiteral("Pull failed: %1 (%2)").arg(explained, reason),
                false};
    if (forkmesh::gitrefusal::remoteUnavailable(gitOutput))
        return {QStringLiteral("Pull failed: the remote could not be reached "
                               "(%1). Try again shortly.")
                    .arg(reason),
                false};
    return {QStringLiteral("Pull failed: %1").arg(reason), false};
}

}

void MainWindow::fetchCurrentRepo()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    syncRepository(m_repoDetailIndex, /*quiet=*/true);
    const bool hasWorkTree = !repo.localPath.isEmpty() &&
                             QDir(repo.localPath).exists(QStringLiteral(".git"));
    if (!hasWorkTree) {
        flashMessage(QString::fromUtf8("Fetching %1/%2 from the network\xE2\x80\xA6")
                         .arg(repo.owner, repo.name));
        return;
    }
    if (m_commitsFetchButton)
        m_commitsFetchButton->setEnabled(false);
    runGitDetached(repo.localPath,
                   {QStringLiteral("fetch"), QStringLiteral("--all"),
                    QStringLiteral("--prune"), QStringLiteral("--tags")},
                   [this](bool ok, const QByteArray &out) {
                       if (m_commitsFetchButton)
                           m_commitsFetchButton->setEnabled(true);
                       if (ok)
                           flashMessage(QStringLiteral("Fetched the latest history."));
                       else
                           flashMessage(fetchFailureText(QString::fromUtf8(out)),
                                        true);
                       if (ok)
                           loadCommits();
                   },
                   forkmesh::logSourceHere(), true);
}

void MainWindow::pullCurrentRepo()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    if (repo.localPath.isEmpty() ||
        !QDir(repo.localPath).exists(QStringLiteral(".git"))) {
        flashMessage(QString::fromUtf8("Read-only mirror \xE2\x80\x94 no working "
                                    "tree to pull into."),
                     true);
        return;
    }
    if (m_pullCurrentRepoRunning) {
        flashMessage(QStringLiteral("A pull is already running."));
        return;
    }
    m_pullCurrentRepoRunning = true;
    if (m_commitsPullButton)
        m_commitsPullButton->setEnabled(false);
    const QString localPath = repo.localPath;
    const QString mirrorPath =
        !repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists() ? repo.mirrorPath
                                                                     : QString();
    const auto finish = [this] {
        m_pullCurrentRepoRunning = false;
        if (m_commitsPullButton)
            m_commitsPullButton->setEnabled(true);
    };
    runOffThread<QStringList>(
        [localPath, mirrorPath] {
            QStringList args{QStringLiteral("pull"), QStringLiteral("--ff-only")};
            if (!runGitCapture(localPath,
                               {QStringLiteral("rev-parse"),
                                QStringLiteral("--abbrev-ref"),
                                QStringLiteral("--symbolic-full-name"),
                                QStringLiteral("@{upstream}")},
                               nullptr, nullptr)) {
                if (mirrorPath.isEmpty())
                    return QStringList();
                QByteArray branch;
                runGitCapture(localPath,
                              {QStringLiteral("rev-parse"),
                               QStringLiteral("--abbrev-ref"), QStringLiteral("HEAD")},
                              &branch, nullptr);
                args << mirrorPath << QString::fromUtf8(branch).trimmed();
            }
            return args;
        },
        [this, localPath, finish](QStringList args) {
            if (args.isEmpty()) {
                finish();
                flashMessage(QStringLiteral("No upstream branch or mirror to pull "
                                            "from."),
                             true);
                return;
            }
            runGitDetached(
                localPath, args,
                [this, finish](bool ok, const QByteArray &out) {
                    finish();
                    const QString text = QString::fromUtf8(out).trimmed();
                    if (ok) {
                        flashMessage(
                            text.contains(QLatin1String("Already up to date"))
                                ? QStringLiteral("Already up to date.")
                                : QStringLiteral("Pulled the latest history."));
                    } else {
                        const PullFailureNotice notice = pullFailureNotice(text);
                        flashMessage(notice.text, true, QString(),
                                     notice.guidance
                                         ? forkmesh::kLocalOperationNoticeKind
                                         : QString());
                    }
                    if (ok) {
                        refreshSourceControl(true);
                        loadCommits();
                    }
                },
                forkmesh::logSourceHere(), true);
        });
}

namespace {
constexpr int kGsKindRole = Qt::UserRole;     // GlobalSearchKind
constexpr int kGsStr1Role = Qt::UserRole + 1; // primary payload (id/hash/path/branch)
constexpr int kGsStr2Role = Qt::UserRole + 2; // secondary payload (e.g. node name)
constexpr int kGsNumRole = Qt::UserRole + 3;  // numeric payload (index / number)
enum GlobalSearchKind {
    GsHeader = 0, // a non-selectable category label
    GsSection,    // m_sectionStack index
    GsRepo,       // m_repositories index
    GsNode,       // roster member (str1 = id, str2 = name)
    GsRelay,      // m_servers index
    GsIssue,      // issue number in the open repo
    GsPull,       // pull-request number in the open repo
    GsDiscussion, // discussion number in the open repo
    GsProject,    // project number in the open repo
    GsAction,     // workflow-run id
    GsLog,        // exact retained network/runtime log line (str1)
    GsBranch,     // branch name (str1) in repository index (num)
    GsWorktree,   // worktree branch (str1) in repository index (num)
    GsTag,        // tag name (str1) in repository index (num)
    GsFile,       // path (str1) in the open repo
    GsCommit,     // full hash (str1) in the open repo
    GsAgent,      // agent session id (num) in the open repo
    GsTeammate,   // teammate id (str1), repository index (num)
    GsPackage,    // installed package id (str1), package-private hit id (str2)
    GsBuiltinPackage, // window-hosted package id (str1), record id (str2)
    GsDeepSearch, // open the streaming full-search page for the typed query
};

QListWidgetItem *makeGlobalSearchHeader(const QString &title)
{
    auto *header = new QListWidgetItem(title.toUpper());
    header->setData(kGsKindRole, GsHeader);
    header->setFlags(Qt::NoItemFlags); // not selectable, not hovered
    QFont font = header->font();
    font.setBold(true);
    header->setFont(font);
    header->setForeground(Theme::remapColor(QColor("#8b949e")));
    return header;
}

// After a worker splices rows into the dropdown: a "No matches" placeholder
// may be sitting there if nothing else matched, and the selection may still
// be on nothing selectable.
void settleGlobalSearchSplice(QListWidget *popup)
{
    for (int r = popup->count() - 1; r >= 0; --r) {
        QListWidgetItem *item = popup->item(r);
        if (item && item->flags() == Qt::NoItemFlags &&
            item->data(kGsKindRole).isNull() &&
            item->text() == QStringLiteral("No matches"))
            delete popup->takeItem(r);
    }
    if (popup->currentRow() >= 0 &&
        (popup->currentItem()->flags() & Qt::ItemIsSelectable))
        return;
    for (int r = 0; r < popup->count(); ++r) {
        if (popup->item(r)->flags() & Qt::ItemIsSelectable) {
            popup->setCurrentRow(r);
            break;
        }
    }
}
} // namespace

QWidget *MainWindow::createGlobalSearchBox()
{
    m_globalSearch = new QLineEdit;
    m_globalSearch->setObjectName("globalSearch");
    m_globalSearch->setPlaceholderText(
        QString::fromUtf8(Theme::isNext() ? "Search or jump to\xE2\x80\xA6"
                                          : "Search\xE2\x80\xA6"));
    m_globalSearch->setMinimumWidth(150);
    m_globalSearch->setMaximumWidth(360);
    // The magnifier is a way in, not decoration: with a query it runs the
    // full search, and with an empty box it opens the search page on its
    // saved searches.
    QAction *searchIcon = m_globalSearch->addAction(
        themedOcticon("search", QColor(Theme::kTextTertiary), 14),
        QLineEdit::LeadingPosition);
    searchIcon->setObjectName("globalSearchIcon");
    searchIcon->setToolTip(QStringLiteral("Open search and saved searches"));
    connect(searchIcon, &QAction::triggered, this, [this] {
        const QString query = m_globalSearch->text().trimmed();
        hideGlobalSearchPopup();
        if (query.isEmpty()) {
            openSavedSearchesPage();
            return;
        }
        {
            QSignalBlocker block(m_globalSearch); // clearing must not re-trigger a rebuild
            m_globalSearch->clear();
        }
        if (m_globalSearchClear)
            m_globalSearchClear->setVisible(false);
        syncAgentPageSearch();
        syncTeammatePageSearch();
        syncOrganizationTaskPageSearch();
        syncNativePackagePageSearch();
        openSearchResultsPage(query);
    });
    // Deliberately not setClearButtonEnabled: Qt's built-in ✕ takes its icon
    // from the desktop icon theme, which resolves to nothing here, so the box
    // reserved space for a button nobody could see. This is the same themed
    // octicon the leading magnifier uses, and it only appears once there is
    // something to clear.
    m_globalSearchClear =
        m_globalSearch->addAction(themedOcticon("x", QColor(Theme::kTextTertiary), 14),
                                  QLineEdit::TrailingPosition);
    m_globalSearchClear->setToolTip(QStringLiteral("Clear search"));
    m_globalSearchClear->setVisible(false);
    connect(m_globalSearchClear, &QAction::triggered, this, [this] {
        clearSearchFilters();
        m_globalSearch->setFocus(Qt::OtherFocusReason);
    });
    m_globalSearch->setToolTip(QString::fromUtf8(
        "Search everything \xE2\x80\x94 sections, relays, nodes, repositories, and "
        "the open repo's issues, pull requests, discussions, projects, agents, "
        "special agents, "
        "workflow runs, branches, files and commits, plus runtime logs"));
    m_globalSearch->installEventFilter(this);

    m_globalSearchPopup = new QListWidget(this);
    m_globalSearchPopup->setObjectName("globalSearchPopup");
    m_globalSearchPopup->setFocusPolicy(Qt::NoFocus);
    m_globalSearchPopup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_globalSearchPopup->setSelectionMode(QAbstractItemView::SingleSelection);
    m_globalSearchPopup->setMouseTracking(true);
    SearchHitDelegate::install(m_globalSearchPopup);
    m_globalSearchPopup->hide();
    connect(m_globalSearchPopup, &QListWidget::itemClicked, this,
            &MainWindow::activateGlobalSearchItem);
    connect(m_globalSearchPopup, &QListWidget::itemEntered, this,
            [this](QListWidgetItem *it) {
                if (it && (it->flags() & Qt::ItemIsSelectable))
                    m_globalSearchPopup->setCurrentItem(it);
            });

    m_globalSearchTimer = new QTimer(this);
    m_globalSearchTimer->setSingleShot(true);
    // The popup rebuild scans issues, pulls, discussions, projects, runs and
    // the newest 2000 log lines; a typing pause of ~200 ms is what a person
    // can notice, and it halves the rebuilds a quickly typed word triggers.
    m_globalSearchTimer->setInterval(200);
    connect(m_globalSearchTimer, &QTimer::timeout, this,
            &MainWindow::rebuildGlobalSearchResults);
    connect(m_globalSearch, &QLineEdit::textChanged, this, [this](const QString &t) {
        if (m_globalSearchClear)
            m_globalSearchClear->setVisible(!t.isEmpty());
        const bool onIssues = m_repoDetailStack &&
                              m_repoDetailStack->currentIndex() == 2;
        if (onIssues && m_issueSearch && m_issueSearch->text() != t)
            m_issueSearch->setText(t);
        syncAgentPageSearch();
        syncTeammatePageSearch();
        syncOrganizationTaskPageSearch();
        syncNativePackagePageSearch();
        if (t.trimmed().isEmpty()) {
            m_globalSearchTimer->stop();
            hideGlobalSearchPopup();
            syncGitCommitFilter();
        } else {
            m_globalSearchTimer->start();
        }
    });
    return m_globalSearch;
}

void MainWindow::rebuildGlobalSearchResults()
{
    if (!m_globalSearch || !m_globalSearchPopup)
        return;
    syncGitCommitFilter();
    // Every rebuild retires the git replies the previous one is still waiting on.
    const int searchGeneration = ++m_globalSearchGen;
    const QString needle = m_globalSearch->text().trimmed().toLower();
    if (needle.isEmpty()) {
        hideGlobalSearchPopup();
        return;
    }
    m_globalSearchPopup->clear();
    const QString query = m_globalSearch->text().trimmed();
    SearchHitDelegate::setQuery(m_globalSearchPopup, query);

    int total = 0;
    constexpr int kMaxTotal = 80;

    auto addHeader = [&](const QString &title) {
        m_globalSearchPopup->addItem(makeGlobalSearchHeader(title));
    };
    auto addResult = [&](const QString &icon, const QColor &iconColor,
                         const QString &text, int kind, const QString &s1,
                         const QString &s2, int num) -> bool {
        if (total >= kMaxTotal)
            return false;
        auto *it = new QListWidgetItem(text);
        if (!icon.isEmpty())
            it->setIcon(themedOcticon(icon, iconColor, 15));
        it->setData(kGsKindRole, kind);
        it->setData(kGsStr1Role, s1);
        it->setData(kGsStr2Role, s2);
        it->setData(kGsNumRole, num);
        m_globalSearchPopup->addItem(it);
        ++total;
        return true;
    };
    auto addResultWithIcon = [&](const QIcon &icon, const QString &text, int kind,
                                const QString &s1, const QString &s2, int num) -> bool {
        if (total >= kMaxTotal)
            return false;
        auto *it = new QListWidgetItem(text);
        if (!icon.isNull())
            it->setIcon(icon);
        it->setData(kGsKindRole, kind);
        it->setData(kGsStr1Role, s1);
        it->setData(kGsStr2Role, s2);
        it->setData(kGsNumRole, num);
        m_globalSearchPopup->addItem(it);
        ++total;
        return true;
    };

    auto addRecord = [&](const QString &kind, const QString &status,
                         const QString &label, const QString &where,
                         int gsKind, int number) -> bool {
        const StatusBadge badge = statusBadge(kind, status);
        const QString text =
            where.isEmpty() || where == QLatin1String("title")
                ? label
                : label + QStringLiteral("  ·  ") + where;
        const Category &category = categoryAt(categoryIndexOf(kind));
        if (!addResult(badge.icon.isEmpty() ? QString::fromLatin1(category.icon)
                                            : badge.icon,
                       QColor(badge.color.isEmpty()
                                  ? QString::fromLatin1(category.color)
                                  : badge.color),
                       text, gsKind, QString(), QString(), number))
            return false;
        QListWidgetItem *it =
            m_globalSearchPopup->item(m_globalSearchPopup->count() - 1);
        setStatusBadge(it, kind, status);
        it->setToolTip(text);
        return true;
    };

    addResult("search", Theme::nextOr(Theme::Role::AccentFg, QColor("#58a6ff")),
              QString::fromUtf8("Show all results for \xE2\x80\x9C%1\xE2\x80\x9D")
                  .arg(query),
              GsDeepSearch, query, QString(), 0);

    struct Sec { const char *label; const char *icon; int index; };
    static const Sec kSections[] = {
        {"Home / Repositories", "home", 0},
        {"Chat", "comment", 2},
        {"Pings", "bell", 3},
        {"Network log", "list-unordered", 4},
        {"Hosts", "server", 7},
        {"Relays", "broadcast", 8},
        {"Network", "workflow", kNetworkDiagnosticsSectionIndex},
        {"Users", "people", kUsersSectionIndex},
        {"Files", "file-directory", kFilesSectionIndex},
        {"Stats", "graph", kStatsSectionIndex},
        {"Health", "stopwatch", kComponentHealthSectionIndex},
        {"Memory", "database", kMemorySectionIndex},
        {"Agent governor", "shield-check", kGovernorSectionIndex},
        {"Sessions", "history", kSessionsSectionIndex},
        {"Home", "home", kHomeGridSectionIndex},
        {"Special Agents", "people", kTeamSectionIndex},
        {"Settings", "gear", 1},
    };
    bool header = false;
    for (const Sec &s : kSections) {
        if (s.index == kUsersSectionIndex && !m_isAdmin)
            continue;
        if (!QString::fromLatin1(s.label).toLower().contains(needle))
            continue;
        if (!header) { addHeader(QStringLiteral("Go to")); header = true; }
        addResult(QString::fromLatin1(s.icon), Theme::remapColor(QColor("#8b949e")),
                  QString::fromLatin1(s.label), GsSection, QString(), QString(),
                  s.index);
    }

    header = false;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &r = m_repositories.at(i);
        const QString full = r.owner.isEmpty() ? r.name : r.owner + "/" + r.name;
        if (!full.toLower().contains(needle) &&
            !r.description.toLower().contains(needle))
            continue;
        if (!header) { addHeader(QStringLiteral("Repositories")); header = true; }
        if (!addResult("repo", Theme::remapColor(QColor("#8b949e")), full, GsRepo, QString(),
                       QString(), i))
            break;
    }

    header = false;
    for (int repoIndex : teammateRepoChoices()) {
        const QString repository = teammateRepoLabel(repoIndex);
        const forkmesh::teammates::LoadResult loaded =
            forkmesh::teammates::Store(teammatesDirPath(repoIndex)).load();
        for (const forkmesh::teammates::Config &teammate : loaded.teammates) {
            const QString handle = forkmesh::teammates::mentionHandle(teammate);
            const QString haystack =
                QStringList{teammate.name, handle, teammate.specialty,
                            teammate.focus, teammate.description,
                            teammate.skills.join(QLatin1Char(' ')), repository}
                    .join(QLatin1Char(' '));
            if (!haystack.contains(query, Qt::CaseInsensitive))
                continue;
            if (!header) {
                addHeader(QStringLiteral("Special Agents"));
                header = true;
            }
            QString label = teammate.name.trimmed();
            if (label.isEmpty())
                label = QLatin1Char('@') + handle;
            else
                label += QStringLiteral("  @") + handle;
            if (!teammate.specialty.trimmed().isEmpty())
                label += QStringLiteral("  ·  ") + teammate.specialty.trimmed();
            label += QStringLiteral("  ·  ") + repository;
            if (!addResult("person", Theme::nextOr(Theme::Role::FgMuted, QColor("#a371f7")), label, GsTeammate,
                           teammate.id, QString(), repoIndex))
                break;
        }
        if (total >= kMaxTotal)
            break;
    }

    // Every installed marketplace package with a native page answers from its
    // own records under a heading carrying the package name. A page that has
    // not been opened this session starts loading now and redraws this list
    // when its records arrive.
    prepareNativePackageSearch();
    prepareBuiltinPackageSearch();
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (!page.widget || total >= kMaxTotal)
            continue;
        const QList<forkmesh::market::NativePackageSearchHit> hits =
            page.widget->searchHits(query, 8);
        if (hits.isEmpty())
            continue;
        const forkmesh::market::PackageManifest *manifest =
            m_packageRegistry ? m_packageRegistry->package(page.id) : nullptr;
        const forkmesh::market::NativePackageContribution *contribution =
            forkmesh::market::nativePackageContribution(
                page.entrypoint, manifest ? manifest->type : QString());
        addHeader(manifest && !manifest->name.isEmpty()
                      ? manifest->name
                      : contribution ? contribution->navigationLabel : page.id);
        for (const forkmesh::market::NativePackageSearchHit &hit : hits) {
            QString label = hit.title;
            if (!hit.detail.isEmpty())
                label += QString::fromUtf8("  \xC2\xB7  ") + hit.detail;
            if (!addResult(contribution ? contribution->navigationIcon
                                        : QStringLiteral("plug"),
                           Theme::nextOr(Theme::Role::FgMuted, QColor("#3fb950")), label, GsPackage, page.id, hit.id,
                           0))
                break;
        }
    }

    // Packages whose pages are part of the window (Notes, Tasks, Calendar,
    // Timers), then any other installed package by name.
    QString builtinGroup;
    for (const BuiltinPackageSearchHit &hit : builtinPackageSearchHits(query, 8)) {
        if (total >= kMaxTotal)
            break;
        if (hit.group != builtinGroup) {
            addHeader(hit.group);
            builtinGroup = hit.group;
        }
        addResult(hit.icon, Theme::nextOr(Theme::Role::FgMuted, QColor("#3fb950")), hit.label, GsBuiltinPackage,
                  hit.packageId, hit.hitId, 0);
    }

    header = false;
    for (const MemberInfo &m : std::as_const(m_homeRoster)) {
        if (!m.name.toLower().contains(needle) && !m.id.toLower().contains(needle))
            continue;
        if (!header) { addHeader(QStringLiteral("Nodes")); header = true; }
        QString label = m.name.isEmpty() ? m.id.left(12) : m.name;
        if (m.self)
            label += QStringLiteral("  (you)");
        else if (!m.note.isEmpty())
            label += QStringLiteral("  ") + m.note;
        if (!addResult("person", m.online ? QColor("#3fb950") : Theme::remapColor(QColor("#8b949e")),
                       label, GsNode, m.id, m.name, 0))
            break;
    }

    header = false;
    for (int i = 0; i < m_servers.size(); ++i) {
        const QString host = serverHost(m_servers.at(i).url);
        if (!host.toLower().contains(needle))
            continue;
        if (!header) { addHeader(QStringLiteral("Relays")); header = true; }
        const QString label =
            i == m_activeServer ? host + QStringLiteral("  (active)") : host;
        if (!addResult("server", Theme::remapColor(QColor("#8b949e")), label, GsRelay, QString(),
                       QString(), i))
            break;
    }

    const bool repoOpen =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size();
    if (repoOpen) {
        const QString repoName = m_repositories.at(m_repoDetailIndex).name;
        const QString suffix = QString::fromUtf8(" \xC2\xB7 ") + repoName;

        header = false;
        for (const Issue &iss : std::as_const(m_currentIssues)) {
            if (iss.isDeleted())
                continue;
            const QString hay =
                QStringLiteral("#%1 %2").arg(iss.number).arg(iss.title);
            QString where;
            if (!hay.toLower().contains(needle) &&
                !matchIssue(iss, query, &where))
                continue;
            if (!header) { addHeader(QStringLiteral("Issues") + suffix); header = true; }
            if (!addRecord(QStringLiteral("issue"), iss.status,
                           QStringLiteral("#%1  %2").arg(iss.number).arg(iss.title),
                           where, GsIssue, iss.number))
                break;
        }

        header = false;
        for (const PullRequest &pr : std::as_const(m_currentPulls)) {
            const QString hay =
                QStringLiteral("#%1 %2").arg(pr.number).arg(pr.title);
            QString where;
            if (!hay.toLower().contains(needle) &&
                !matchPull(pr, query, &where))
                continue;
            if (!header) {
                addHeader(QStringLiteral("Pull requests") + suffix);
                header = true;
            }
            if (!addRecord(QStringLiteral("pull"), pullDisplayStatus(pr),
                           QStringLiteral("#%1  %2").arg(pr.number).arg(pr.title),
                           where, GsPull, pr.number))
                break;
        }

        header = false;
        for (const Discussion &discussion :
             std::as_const(m_currentDiscussions)) {
            const QString hay = QStringLiteral("#%1 %2")
                                    .arg(discussion.number)
                                    .arg(discussion.title);
            QString where;
            if (!hay.toLower().contains(needle) &&
                !matchDiscussion(discussion, query, &where))
                continue;
            if (!header) {
                addHeader(QStringLiteral("Discussions") + suffix);
                header = true;
            }
            if (!addRecord(QStringLiteral("discussion"), discussion.status, hay,
                           where, GsDiscussion, discussion.number))
                break;
        }

        header = false;
        for (const Project &project : std::as_const(m_currentProjects)) {
            if (project.isDeleted())
                continue;
            const QString hay =
                QStringLiteral("#%1 %2").arg(project.number).arg(project.title);
            QString where;
            if (!hay.toLower().contains(needle) &&
                !matchProject(project, query, &where))
                continue;
            if (!header) {
                addHeader(QStringLiteral("Projects") + suffix);
                header = true;
            }
            if (!addRecord(QStringLiteral("project"), project.status, hay, where,
                           GsProject, project.number))
                break;
        }

        header = false;
        const QString repoOwner = m_repositories.at(m_repoDetailIndex).owner;
        for (const ActionRun &run : std::as_const(m_actionRuns)) {
            if (run.owner != repoOwner || run.name != repoName)
                continue;
            if (!containsFold(run.workflowName, query) &&
                !containsFold(run.workflowPath, query) &&
                !containsFold(run.workflowContent, query) &&
                !containsFold(run.ref, query) &&
                !containsFold(run.commit, query))
                continue;
            if (!header) {
                addHeader(QStringLiteral("Workflow runs") + suffix);
                header = true;
            }
            const QString label =
                QStringLiteral("%1  #%2")
                    .arg(run.workflowName.isEmpty() ? run.workflowPath
                                                    : run.workflowName)
                    .arg(run.id);
            if (!addResult("workflow", Theme::nextOr(Theme::Role::FgMuted, QColor("#3fb950")), label, GsAction,
                           QString(), QString(), run.id))
                break;
        }

        header = false;
        for (const AgentSession &session : std::as_const(m_agentSessions)) {
            if (session.owner != repoOwner || session.name != repoName)
                continue;
            const AgentTranscriptHit hit = agentTranscriptHit(session.id);
            const QString title =
                session.issueNumber > 0
                    ? QStringLiteral("#%1 %2").arg(session.issueNumber)
                          .arg(session.issueTitle)
                    : session.issueTitle;
            if (!title.toLower().contains(needle) && hit.count == 0 &&
                !containsFold(session.branchName, query) &&
                !containsFold(m_agentDiffStats.value(session.id).worktree, query))
                continue;
            if (!header) {
                addHeader(QStringLiteral("Agent sessions") + suffix);
                header = true;
            }
            QString label = title.simplified();
            if (label.size() > 80)
                label = label.left(79) + QString::fromUtf8("\xE2\x80\xA6");
            if (hit.count > 0)
                label += QString::fromUtf8("   \xC2\xB7  %1 in transcript")
                             .arg(hit.count);
            if (!addResultWithIcon(agentStatusOcticon(session, 15), label,
                                  GsAgent, QString(), QString(),
                                  session.id))
                break;
        }

        header = false;
        const QString ref = currentRef();
        for (const QString &b : repoBranches()) {
            if (!b.toLower().contains(needle))
                continue;
            if (!header) { addHeader(QStringLiteral("Branches") + suffix); header = true; }
            if (!addResult("repo-forked", Theme::remapColor(QColor("#8b949e")),
                           b == ref ? b + QStringLiteral("  (current)") : b,
                           GsBranch, b, QString(), m_repoDetailIndex))
                break;
        }

        if (m_fileCompleter && m_fileCompleter->model()) {
            QAbstractItemModel *fm = m_fileCompleter->model();
            header = false;
            int fileHits = 0;
            for (int r = 0; r < fm->rowCount() && fileHits < 12; ++r) {
                const QString path = fm->index(r, 0).data().toString();
                if (!path.toLower().contains(needle))
                    continue;
                if (!header) { addHeader(QStringLiteral("Files") + suffix); header = true; }
                if (!addResult("file", Theme::remapColor(QColor("#8b949e")), path, GsFile, path,
                               QString(), 0))
                    break;
                ++fileHits;
            }
        }

        startGlobalSearchCommitSplice(searchGeneration, needle, suffix, ref);
    }

    startGlobalSearchCatalogSplice(searchGeneration, needle, query);

    // The retained log holds up to kNetworkLogLimit lines, far more than this
    // keystroke-driven preview should fold-compare on the GUI thread. Scan the
    // newest slice only; the full results page searches all of it off-thread.
    header = false;
    constexpr int kPopupLogScanLines = 2000;
    const int logScanFrom = qMax(0, m_networkLog.size() - kPopupLogScanLines);
    for (int i = m_networkLog.size() - 1; i >= logScanFrom; --i) {
        const QString &line = m_networkLog.at(i);
        if (!line.contains(query, Qt::CaseInsensitive))
            continue;
        if (!header) {
            addHeader(QStringLiteral("Logs"));
            header = true;
        }
        if (!addResult("list-unordered", Theme::remapColor(QColor("#8b949e")),
                       snippetAround(line.trimmed(), query), GsLog, line,
                       QString(), 0))
            break;
    }

    finishGlobalSearchPopup(total);
}

// Splices the open repository's matching commits into the popup once the
// `git log --grep` worker returns.
void MainWindow::startGlobalSearchCommitSplice(int searchGeneration, const QString &needle,
                                               const QString &suffix, const QString &ref)
{
    if (needle.size() >= 2) {
        const QString dir = repoGitDir();
        // `git log --grep` walks the whole history until it has eight
        // hits — for a rare term that is the entire log, ~1 s on a big
        // repository — and it used to run synchronously on the GUI thread
        // per keystroke pause. Run it on a worker and splice the Commits
        // section in at the row it would have occupied, as long as the
        // popup still shows the same query.
        if (!dir.isEmpty()) {
            const int insertRow = m_globalSearchPopup->count();
            const int generation = searchGeneration;
            const QString sectionTitle = QStringLiteral("Commits") + suffix;
            const QStringList args =
                QStringList{QStringLiteral("log"),
                            QStringLiteral("--fixed-strings"),
                            QStringLiteral("-i"),
                            QStringLiteral("--grep=") + needle,
                            QStringLiteral("-n"), QStringLiteral("8"),
                            QStringLiteral("--format=%H%x1f%h%x1f%s%x1f%S")} +
                allBranchRevisionArgs(ref);
            runOffThread<QByteArray>(
                [dir, args] {
                    const GitCaptureResult result =
                        runGitCaptureDirect(dir, args);
                    return result.ok ? result.output : QByteArray();
                },
                [this, generation, insertRow, sectionTitle, needle](QByteArray out) {
                    if (generation != m_globalSearchGen || !m_globalSearchPopup ||
                        !m_globalSearch ||
                        m_globalSearch->text().trimmed().toLower() != needle ||
                        out.isEmpty())
                        return;
                    int row = qMin(insertRow, m_globalSearchPopup->count());
                    bool commitsHeader = false;
                    for (const QByteArray &line : out.split('\n')) {
                        if (line.trimmed().isEmpty())
                            continue;
                        const QStringList f =
                            QString::fromUtf8(line).split(QChar(0x1f));
                        if (f.size() < 3)
                            continue;
                        if (!commitsHeader) {
                            m_globalSearchPopup->insertItem(
                                row++, makeGlobalSearchHeader(sectionTitle));
                            commitsHeader = true;
                        }
                        QString label =
                            QStringLiteral("%1  %2").arg(f.at(1), f.at(2));
                        if (!f.value(3).isEmpty())
                            label += QStringLiteral("  ·  ") + f.at(3);
                        auto *it = new QListWidgetItem(label);
                        it->setIcon(themedOcticon("git-branch", Theme::remapColor(QColor("#8b949e")), 15));
                        it->setData(kGsKindRole, GsCommit);
                        it->setData(kGsStr1Role, f.at(0));
                        it->setData(kGsStr2Role, QString());
                        it->setData(kGsNumRole, 0);
                        m_globalSearchPopup->insertItem(row++, it);
                    }
                    if (!commitsHeader)
                        return;
                    settleGlobalSearchSplice(m_globalSearchPopup);
                    if (m_globalSearchPopup->isVisible())
                        positionGlobalSearchPopup(); // grow for the new rows
                });
        }
    }
}

void MainWindow::startGlobalSearchCatalogSplice(int searchGeneration, const QString &needle,
                                                const QString &query)
{
    // Worktrees and tags of the open repository, and branches, worktrees and
    // tags of every other repository on this node. Each costs git calls per
    // repository, so they are read on a worker and spliced in above the Logs
    // group once they arrive.
    if (needle.size() >= 2) {
        const QVector<GlobalSearchGitRepo> gitRepos = globalSearchGitRepos();
        const int openIndex = m_repoDetailIndex;
        runOffThread<QVector<GlobalSearchHit>>(
            [gitRepos, openIndex, query] {
                // Wider than the rows shown: the open repository's branches
                // lead the scan but are dropped below, and must not use up the
                // slots other repositories' branches need.
                constexpr int kPopupCatalogScanPerKind = 48;
                return scanAllGitCatalogHits(gitRepos, openIndex, QString(),
                                             query, kPopupCatalogScanPerKind);
            },
            [this, searchGeneration, needle, openIndex](
                QVector<GlobalSearchHit> hits) {
                if (searchGeneration != m_globalSearchGen ||
                    !m_globalSearchPopup || !m_globalSearch ||
                    m_globalSearch->text().trimmed().toLower() != needle)
                    return;
                int row = m_globalSearchPopup->count();
                for (int r = 0; r < m_globalSearchPopup->count(); ++r) {
                    QListWidgetItem *item = m_globalSearchPopup->item(r);
                    if (item->data(kGsKindRole).toInt() == GsHeader &&
                        item->flags() == Qt::NoItemFlags &&
                        item->text() == QStringLiteral("LOGS")) {
                        row = r;
                        break;
                    }
                }
                bool inserted = false;
                struct Group { const char *kind; int gsKind; const char *title; };
                static const Group kGroups[] = {
                    {"branch", GsBranch, "Branches in other repositories"},
                    {"worktree", GsWorktree, "Worktrees"},
                    {"tag", GsTag, "Tags"},
                };
                for (const Group &group : kGroups) {
                    const Category &category =
                        categoryAt(categoryIndexOf(QLatin1String(group.kind)));
                    constexpr int kPopupCatalogRowsPerGroup = 8;
                    bool groupHeader = false;
                    int groupRows = 0;
                    for (const GlobalSearchHit &hit : std::as_const(hits)) {
                        if (groupRows >= kPopupCatalogRowsPerGroup)
                            break;
                        if (hit.kind != QLatin1String(group.kind))
                            continue;
                        // The open repository's branches are already listed
                        // from the cached branch list above.
                        if (group.gsKind == GsBranch && hit.repoIndex == openIndex)
                            continue;
                        if (!groupHeader) {
                            m_globalSearchPopup->insertItem(
                                row++, makeGlobalSearchHeader(
                                           QString::fromLatin1(group.title)));
                            groupHeader = true;
                        }
                        auto *it = new QListWidgetItem(
                            hit.detail.isEmpty()
                                ? hit.primary
                                : hit.primary + QStringLiteral("  ·  ") + hit.detail);
                        it->setIcon(themedOcticon(
                            QString::fromLatin1(category.icon),
                            QColor(QString::fromLatin1(category.color)), 15));
                        it->setToolTip(it->text());
                        it->setData(kGsKindRole, group.gsKind);
                        it->setData(kGsStr1Role, hit.path);
                        it->setData(kGsStr2Role, QString());
                        it->setData(kGsNumRole, hit.repoIndex);
                        m_globalSearchPopup->insertItem(row++, it);
                        ++groupRows;
                        inserted = true;
                    }
                }
                if (!inserted)
                    return;
                settleGlobalSearchSplice(m_globalSearchPopup);
                if (m_globalSearchPopup->isVisible())
                    positionGlobalSearchPopup();
            });
    }
}

// Adds the "No matches" row when nothing matched, selects the first
// selectable row and shows the popup.
void MainWindow::finishGlobalSearchPopup(int total)
{
    if (total == 0) {
        auto *none = new QListWidgetItem(QStringLiteral("No matches"));
        none->setFlags(Qt::NoItemFlags);
        none->setForeground(Theme::remapColor(QColor("#8b949e")));
        m_globalSearchPopup->addItem(none);
    }

    for (int r = 0; r < m_globalSearchPopup->count(); ++r) {
        if (m_globalSearchPopup->item(r)->flags() & Qt::ItemIsSelectable) {
            m_globalSearchPopup->setCurrentRow(r);
            break;
        }
    }
    positionGlobalSearchPopup();
    m_globalSearchPopup->raise();
    m_globalSearchPopup->show();
}

void MainWindow::positionGlobalSearchPopup()
{
    if (!m_globalSearch || !m_globalSearchPopup)
        return;
    const QPoint topLeft =
        m_globalSearch->mapTo(this, QPoint(0, m_globalSearch->height() + 4));
    int w = std::max(m_globalSearch->width(), 380);
    w = std::min(w, width() - topLeft.x() - 12);
    w = std::max(w, 200);
    int rowsH = 8;
    for (int r = 0; r < m_globalSearchPopup->count(); ++r)
        rowsH += m_globalSearchPopup->sizeHintForRow(r);
    m_globalSearchPopup->setGeometry(topLeft.x(), topLeft.y(), w,
                                     std::min(rowsH, 440));
}

void MainWindow::moveGlobalSearchSelection(int delta)
{
    if (!m_globalSearchPopup || !m_globalSearchPopup->isVisible())
        return;
    const int count = m_globalSearchPopup->count();
    if (count == 0)
        return;
    int row = m_globalSearchPopup->currentRow();
    for (int step = 0; step < count; ++step) {
        row += delta;
        if (row < 0)
            row = count - 1;
        else if (row >= count)
            row = 0;
        QListWidgetItem *it = m_globalSearchPopup->item(row);
        if (it && (it->flags() & Qt::ItemIsSelectable)) {
            m_globalSearchPopup->setCurrentRow(row);
            m_globalSearchPopup->scrollToItem(it);
            return;
        }
    }
}

void MainWindow::activateGlobalSearchItem(QListWidgetItem *item)
{
    if (!item || !(item->flags() & Qt::ItemIsSelectable))
        return;
    const int kind = item->data(kGsKindRole).toInt();
    const QString s1 = item->data(kGsStr1Role).toString();
    const QString s2 = item->data(kGsStr2Role).toString();
    const int num = item->data(kGsNumRole).toInt();

    hideGlobalSearchPopup();
    if (m_globalSearch) {
        {
            QSignalBlocker block(m_globalSearch); // clearing must not re-trigger a rebuild
            m_globalSearch->clear();
        }
        syncAgentPageSearch();
        syncTeammatePageSearch();
        syncOrganizationTaskPageSearch();
        syncNativePackagePageSearch();
    }

    const bool repoOpen =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size();
    auto clickRepoTab = [this](int idx) {
        if (m_repoDetailTabs && m_repoDetailTabs->button(idx))
            m_repoDetailTabs->button(idx)->click(); // switches tab + loads its data
    };

    switch (kind) {
    case GsDeepSearch:
        openSearchResultsPage(s1); // s1 holds the (pre-clear) query text
        break;
    case GsSection:
        showSection(num);
        break;
    case GsRepo:
        openRepoDetailDeferred(num);
        break;
    case GsNode:
        showSection(0);
        showNodeProfile(s1, s2);
        break;
    case GsRelay:
        switchToServer(num);
        break;
    case GsIssue:
        if (repoOpen) { showSection(0); clickRepoTab(2); showIssue(num); }
        break;
    case GsPull:
        if (repoOpen) { showSection(0); clickRepoTab(4); showPull(num); }
        break;
    case GsDiscussion:
        if (repoOpen) { showSection(0); clickRepoTab(5); showDiscussion(num); }
        break;
    case GsProject:
        if (repoOpen) {
            showSection(0);
            clickRepoTab(m_projectsTabIndex);
            showProject(num);
        }
        break;
    case GsAction:
        openActionRunFromNotification(num);
        break;
    case GsLog:
        openFullLogAtFooterLine(s1);
        break;
    case GsBranch:
        if (num >= 0 && num != m_repoDetailIndex && num < m_repositories.size())
            openRepoDetail(num); // a branch found in another repository
        if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
            showSection(0);
            setRepoBranch(s1);
        }
        break;
    case GsWorktree:
    case GsTag: {
        GlobalSearchHit hit;
        hit.kind = kind == GsWorktree ? QStringLiteral("worktree")
                                      : QStringLiteral("tag");
        hit.path = s1;
        hit.repoIndex = num;
        activateGlobalSearchHit(hit); // opens the owning repository first
        break;
    }
    case GsFile:
        if (repoOpen) openRepoFile(s1);
        break;
    case GsCommit:
        if (repoOpen) { showSection(0); showOverviewCommits(); showCommit(s1); }
        break;
    case GsAgent:
        switchToAgentsTab(num); // binds the repo, opens the tab, selects the run
        break;
    case GsPackage:
        showNativePackage(s1);
        for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
            if (page.id == s1 && page.widget)
                page.widget->activateSearchHit(s2);
        }
        break;
    case GsBuiltinPackage:
        activateBuiltinPackageSearchHit(s1, s2);
        break;
    case GsTeammate:
        m_teammatesStatusFilter.clear();
        setActiveTeammateRepoIndex(num);
        showSection(kTeamSectionIndex);
        if (QPushButton *all = m_teammateMetricButtons.value(QString()))
            all->setChecked(true);
        showTeammateHome(s1);
        break;
    default:
        break;
    }
}

void MainWindow::hideGlobalSearchPopup()
{
    if (m_globalSearchPopup)
        m_globalSearchPopup->hide();
}

QWidget *MainWindow::createNavHistoryButtons()
{
    auto *holder = new QWidget;
    auto *row = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);

    // Holding either button opens that direction's slice of the trail, the way
    // a browser's back button does. One timer serves both: only one of them can
    // be under the mouse at a time.
    constexpr int kNavHistoryHoldMs = 450;
    m_navLongPressTimer = new QTimer(this);
    m_navLongPressTimer->setSingleShot(true);
    m_navLongPressTimer->setInterval(kNavHistoryHoldMs);
    connect(m_navLongPressTimer, &QTimer::timeout, this, [this] {
        m_navMenuFromLongPress = true; // the release that follows must not step
        showNavHistoryMenu(m_navLongPressForward);
    });

    auto makeButton = [this](const QString &icon, const QString &tip,
                             void (MainWindow::*slot)(), bool forward) {
        auto *b = new QPushButton;
        b->setObjectName("navHistoryButton");
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(30, 30);
        setOcticon(b, icon, 16);
        b->setToolTip(tip);
        b->setEnabled(false);
        connect(b, &QPushButton::pressed, this, [this, forward] {
            m_navMenuFromLongPress = false; // a fresh press starts clean
            m_navLongPressForward = forward;
            m_navLongPressTimer->start();
        });
        // Released also fires when the mouse is dragged off a held button, so
        // sliding away cancels the hold instead of popping the menu later.
        connect(b, &QPushButton::released, this,
                [this] { m_navLongPressTimer->stop(); });
        connect(b, &QPushButton::clicked, this, [this, slot] {
            if (m_navMenuFromLongPress)
                return; // the hold already offered the whole trail
            (this->*slot)();
        });
        return b;
    };
    m_navBackButton = makeButton("chevron-left", QStringLiteral("Back"),
                                 &MainWindow::navigateBack, false);
    m_navForwardButton = makeButton("chevron-right", QStringLiteral("Forward"),
                                    &MainWindow::navigateForward, true);
    row->addWidget(m_navBackButton);
    row->addWidget(m_navForwardButton);
    return holder;
}

void MainWindow::scheduleNavRecord()
{
    if (m_navRestoring || m_navRecordPending)
        return;
    m_navRecordPending = true;
    QTimer::singleShot(0, this, &MainWindow::recordNavLocation);
}

void MainWindow::recordNavLocation()
{
    m_navRecordPending = false;
    if (m_navRestoring)
        return;
    NavPlace here;
    here.section = m_sectionStack ? m_sectionStack->currentIndex() : 0;
    here.repoIndex = (here.section == 0) ? m_repoDetailIndex : -1;
    here.detailTab = (here.repoIndex >= 0 && m_repoDetailStack)
                         ? m_repoDetailStack->currentIndex()
                         : -1;
    if (here.detailTab == 0 && m_filesStack && m_filesStack->currentIndex() == 0 &&
        m_overviewBodyStack)
        here.overviewPage = m_overviewBodyStack->currentIndex();
    if (here.overviewPage == 1) {
        here.branch = m_repoBranch.isEmpty() ? repoHeadBranch() : m_repoBranch;
        if (m_commitsStack &&
            m_commitsStack->currentIndex() == kCommitWorkspaceCommitPage)
            here.commit = m_currentCommitHash;
    }
    captureNavSubPlace(here);

    if (here.detailTab >= 0) {
        // Keep this tiny preference buffered for the process lifetime.  A
        // temporary QSettings flushes (including fdatasync on Linux) from its
        // destructor; doing that during every navigation produced a measured
        // half-second GUI stall.  The persistent instance still flushes on a
        // normal application shutdown.
        static QSettings navigationSettings;
        if (navigationSettings.value(kLastRepoDetailTabSetting, -1).toInt() !=
            here.detailTab)
            navigationSettings.setValue(kLastRepoDetailTabSetting,
                                        here.detailTab);
    }

    if (m_navHistoryIndex >= 0 && m_navHistoryIndex < m_navHistory.size() &&
        m_navHistory.at(m_navHistoryIndex) == here)
        return; // already standing here — nothing moved

    while (m_navHistory.size() > m_navHistoryIndex + 1)
        m_navHistory.removeLast();
    m_navHistory.append(here);
    constexpr int kMaxNavHistory = 50;
    while (m_navHistory.size() > kMaxNavHistory)
        m_navHistory.removeFirst();
    m_navHistoryIndex = m_navHistory.size() - 1;
    updateNavHistoryButtons();
    notePageButtonNavigated();
}

QString MainWindow::openRepoFilePath() const
{
    if (!m_repoFileTabs)
        return QString();
    QWidget *current = m_repoFileTabs->currentWidget();
    if (!current)
        return QString();
    const QString coveKey = m_openCoveExplorerTabs.key(current);
    if (!coveKey.isEmpty())
        return QStringLiteral("cove") + QChar(0x1f) + coveKey;
    return m_openFileTabs.key(current);
}

void MainWindow::captureNavSubPlace(NavPlace &place) const
{
    if (place.section == 2) {
        place.conversation = m_currentConversation;
        return;
    }
    if (place.section == 1) {
        if (m_settingsSections)
            place.subTab = m_settingsSections->currentIndex();
        return;
    }
    if (place.section == kControlNodeSectionIndex) {
        if (m_controlNodeTabs)
            place.subTab = m_controlNodeTabs->currentIndex();
        return;
    }
    if (place.section == kNetworkDiagnosticsSectionIndex ||
        place.section == kNodesSectionIndex) {
        if (m_networkTabs)
            place.subTab = m_networkTabs->currentIndex();
        return;
    }
    if (place.section == kFilesSectionIndex) {
        place.filePath = openRepoFilePath();
        return;
    }
    if (place.section == kProjectSettingsSectionIndex) {
        place.itemId = m_projectSettingsSlug;
        return;
    }
    if (place.section == kTeamSectionIndex) {
        if (m_teammatesPageStack)
            place.subTab = m_teammatesPageStack->currentIndex();
        if (place.subTab == 0 && m_teammateDetailPanel &&
            m_teammateDetailPanel->isVisible())
            place.itemId = m_selectedTeammateId;
        else if (place.subTab == 2)
            place.itemId = m_selectedBriefingTeammateId;
        else if (place.subTab == 3)
            place.itemId = m_teammateHomeId;
        return;
    }
    if (place.repoIndex < 0)
        return;
    switch (place.detailTab) {
    case 0: // Code
        if (place.overviewPage <= 0)
            place.overviewDir = m_overviewPath;
        break;
    case 2: // Issues
        if (m_issueListStack)
            place.subTab = m_issueListStack->currentIndex();
        if (m_issueDetail && !m_issueDetail->isHidden())
            place.itemNumber = m_currentIssueNumber;
        break;
    case 3: // Agents — the selected session is what the pane shows
        place.itemNumber = m_selectedAgentSessionId;
        break;
    case 4: // Pull requests
        place.itemNumber = m_currentPullNumber;
        if (m_pullSubStack && m_currentPullNumber > 0)
            place.subTab = m_pullSubStack->currentIndex();
        break;
    case 5: // Discussions
        place.itemNumber = m_currentDiscussionNumber;
        break;
    default:
        if (place.detailTab == m_projectsTabIndex && m_projectViewStack)
            place.subTab = m_projectViewStack->currentIndex(); // List / Gantt
        break;
    }
}

void MainWindow::applyNavSubPlace(const NavPlace &place)
{
    if (place.section == kProjectSettingsSectionIndex) {
        if (place.itemId.isEmpty() || !loadProjectSettingsPage(place.itemId))
            showSection(0);
        return;
    }
    if (place.section == 2) {
        if (!place.conversation.isEmpty())
            switchConversation(place.conversation);
        return;
    }
    if (place.section == 1) {
        if (m_settingsSections && place.subTab >= 0 &&
            place.subTab < m_settingsSections->count())
            m_settingsSections->setCurrentIndex(place.subTab);
        return;
    }
    if (place.section == kControlNodeSectionIndex) {
        if (m_controlNodeTabs && place.subTab >= 0 &&
            place.subTab < m_controlNodeTabs->count())
            m_controlNodeTabs->setCurrentIndex(place.subTab);
        return;
    }
    if (place.section == kNetworkDiagnosticsSectionIndex ||
        place.section == kNodesSectionIndex) {
        if (m_networkTabs && place.subTab >= 0 &&
            place.subTab < m_networkTabs->count())
            showNetworkTab(place.subTab);
        return;
    }
    if (place.section == kFilesSectionIndex) {
        const QString covePrefix = QStringLiteral("cove") + QChar(0x1f);
        if (place.filePath.startsWith(covePrefix)) {
            const QStringList parts =
                place.filePath.split(QChar(0x1f), Qt::KeepEmptyParts);
            if (parts.size() == 3 && !parts.at(1).isEmpty() &&
                !parts.at(2).isEmpty()) {
                m_coveExplorerCurrentId = parts.at(1);
                showRepoCoveExplorer();
                openCoveExplorerDocument(parts.at(2));
            }
        } else if (!place.filePath.isEmpty()) {
            openRepoFile(place.filePath);
        }
        return;
    }
    if (place.section == kTeamSectionIndex) {
        switch (place.subTab) {
        case 1:
            if (m_teammatesPageStack)
                m_teammatesPageStack->setCurrentIndex(1);
            if (m_teammateStatsButton)
                m_teammateStatsButton->setChecked(true);
            loadTeammatesPanel();
            break;
        case 2:
            showTeammateBriefing(place.itemId);
            break;
        case 3:
            if (!place.itemId.isEmpty()) {
                showTeammateHome(place.itemId);
                break;
            }
            showTeammateGrid();
            break;
        default:
            if (place.itemId.isEmpty())
                showTeammateGrid();
            else
                showTeammateDetail(place.itemId);
            break;
        }
        return;
    }
    if (place.repoIndex < 0 || place.repoIndex != m_repoDetailIndex)
        return;
    switch (place.detailTab) {
    case 0: // Code
        if (place.overviewPage <= 0) {
            if (m_filesStack && m_filesStack->currentIndex() != 0)
                m_filesStack->setCurrentIndex(0);
            if (m_overviewPath != place.overviewDir)
                loadRepoOverview(place.overviewDir);
        }
        break;
    case 2: // Issues
        if (place.subTab >= 0)
            selectIssueListTab(place.subTab);
        if (place.itemNumber > 0) {
            showIssue(place.itemNumber);
        } else if (m_issueDetail) {
            m_issueDetail->show();
        }
        break;
    case 3: // Agents
        if (place.itemNumber > 0 && m_selectedAgentSessionId != place.itemNumber &&
            findAgentSession(place.itemNumber))
            showAgentSession(place.itemNumber);
        break;
    case 4: // Pull requests
        if (place.itemNumber > 0) {
            if (m_currentPullNumber != place.itemNumber)
                showPull(place.itemNumber);
            if (place.subTab >= 0 && m_pullSubTabs &&
                m_pullSubTabs->button(place.subTab) &&
                m_pullSubStack->currentIndex() != place.subTab)
                m_pullSubTabs->button(place.subTab)->click();
        }
        break;
    case 5: // Discussions
        if (place.itemNumber > 0 && m_currentDiscussionNumber != place.itemNumber)
            showDiscussion(place.itemNumber);
        break;
    default:
        if (place.detailTab == m_projectsTabIndex && place.subTab >= 0 &&
            m_projectViewStack &&
            m_projectViewStack->currentIndex() != place.subTab && m_projectViewTabs &&
            m_projectViewTabs->button(place.subTab))
            m_projectViewTabs->button(place.subTab)->click();
        break;
    }
}

void MainWindow::restoreNavEntry(int index)
{
    if (index < 0 || index >= m_navHistory.size())
        return;
    const NavPlace place = m_navHistory.at(index);
    m_navHistoryIndex = index;
    m_navRestoring = true;
    if (place.section == 0 && place.repoIndex >= 0 &&
        place.repoIndex < m_repositories.size() &&
        place.repoIndex != m_repoDetailIndex) {
        openRepoDetailDeferred(place.repoIndex); // lands on the Code section itself
        const NavPlace target = place;
        QTimer::singleShot(0, this, [this, target] {
            applyNavDetailTab(target);
            applyNavSubPlace(target);
            m_navRestoring = false;
            updateNavHistoryButtons();
        });
        return;
    }
    showSection(place.section);
    applyNavDetailTab(place);
    applyNavSubPlace(place);
    m_navRestoring = false;
    updateNavHistoryButtons();
}

void MainWindow::applyNavDetailTab(const NavPlace &place)
{
    if (place.section != 0 || place.repoIndex < 0 || place.detailTab < 0)
        return;
    if (place.detailTab == kRetiredMirrorNodesDetailTab) {
        showMirrorNodesOnNetwork(place.repoIndex);
        return;
    }
    if (!m_repoDetailTabs || !m_repoDetailStack)
        return;
    if (m_repoDetailStack->currentIndex() != place.detailTab) {
        if (QAbstractButton *b = m_repoDetailTabs->button(place.detailTab))
            b->click(); // switches the tab and loads its data, like a real click
    }
    if (place.detailTab == 0) {
        if (place.overviewPage == 2) {
            showOverviewBranches();
            loadBranchesPanel();
            return;
        }
        if (place.overviewPage == 3) {
            showOverviewWorktrees();
            loadWorktreesPanel();
            return;
        }
        if (place.overviewPage != 1) {
            showOverviewFiles();
            return;
        }
        showOverviewCommits();
        const QString base = repoDefaultBranchFast();
        if (!place.branch.isEmpty() && place.branch != base) {
            switchToBranch(place.branch);
        } else {
            closeBranchCompareView();
            showOverviewCommits();
            refreshCommitsUnlessCurrent();
        }
        if (!place.commit.isEmpty())
            showCommit(place.commit);
        else if (m_commitsStack &&
                 m_commitsStack->currentIndex() == kCommitWorkspaceCommitPage)
            setCommitWorkspacePage(kCommitWorkspaceChangesPage);
        return;
    }
    if (m_repoDetailStack->currentIndex() == place.detailTab)
        return;
    if (place.detailTab == 3) {
        m_repoDetailStack->setCurrentIndex(place.detailTab);
        reloadAgents(AgentReloadScan::FromCache);
    }
}

void MainWindow::navigateBack()
{
    if (m_navHistoryIndex > 0)
        restoreNavEntry(m_navHistoryIndex - 1);
}

void MainWindow::navigateForward()
{
    if (m_navHistoryIndex >= 0 && m_navHistoryIndex < m_navHistory.size() - 1)
        restoreNavEntry(m_navHistoryIndex + 1);
}

QList<int> MainWindow::navHistoryMenuIndices(bool forward) const
{
    constexpr int kNavMenuRows = 10; // as much trail as is still recognisable
    QList<int> rows;
    if (m_navHistoryIndex < 0)
        return rows;
    if (forward) {
        for (int i = m_navHistoryIndex + 1;
             i < m_navHistory.size() && rows.size() < kNavMenuRows; ++i)
            rows.append(i);
    } else {
        for (int i = m_navHistoryIndex - 1; i >= 0 && rows.size() < kNavMenuRows;
             --i)
            rows.append(i);
    }
    return rows; // nearest place first, either way
}

QMenu *MainWindow::buildNavHistoryMenu(bool forward)
{
    const QList<int> rows = navHistoryMenuIndices(forward);
    if (rows.isEmpty())
        return nullptr;

    constexpr int kLabelRoom = 420; // a long file path must not stretch the popup
    auto *menu = new QMenu(this);
    menu->setToolTipsVisible(true);
    for (int index : rows) {
        const QString label = navPlaceLabel(m_navHistory.at(index));
        QAction *action = menu->addAction(
            menu->fontMetrics().elidedText(label, Qt::ElideMiddle, kLabelRoom));
        action->setData(index); // where in the trail this row stands
        if (action->text() != label)
            action->setToolTip(label);
    }
    return menu;
}

void MainWindow::showNavHistoryMenu(bool forward)
{
    QPushButton *button = forward ? m_navForwardButton : m_navBackButton;
    if (!button)
        return;
    QMenu *menu = buildNavHistoryMenu(forward);
    if (!menu) {
        button->setDown(false);
        return;
    }

    // The popup owns the mouse grab until it closes, so the button never sees
    // the release that ended the hold: undo the pressed look by hand. And read
    // the choice out as a plain index before navigating — restoreNavEntry
    // re-enters the repo detail machinery, which must not run underneath the
    // menu that is still standing on this stack frame.
    QAction *chosen =
        menu->exec(button->mapToGlobal(QPoint(0, button->height())));
    const int target = chosen ? chosen->data().toInt() : -1;
    button->setDown(false);
    menu->deleteLater();
    if (target >= 0)
        restoreNavEntry(target);
}

QString MainWindow::navPlaceLabel(const NavPlace &place) const
{
    QString repo;
    if (place.repoIndex >= 0 && place.repoIndex < m_repositories.size()) {
        const RepositoryRecord &record = m_repositories.at(place.repoIndex);
        repo = record.owner + QLatin1Char('/') + record.name;
    }

    QString destination;
    if (place.section == 0 && place.repoIndex >= 0) {
        if (place.detailTab == 0) {
            switch (place.overviewPage) {
            case 1:
                destination = QStringLiteral("Git · %1")
                                  .arg(place.branch.isEmpty()
                                           ? QStringLiteral("main")
                                           : place.branch);
                if (!place.commit.isEmpty())
                    destination = QStringLiteral("%1 · %2")
                                      .arg(destination, place.commit.left(7));
                break;
            case 2:
                destination = QStringLiteral("Branches");
                break;
            case 3:
                destination = QStringLiteral("Worktrees");
                break;
            default:
                destination = QStringLiteral("Code");
                if (!place.filePath.isEmpty())
                    destination = QStringLiteral("Code · %1").arg(place.filePath);
                else if (!place.overviewDir.isEmpty())
                    destination =
                        QStringLiteral("Code · %1/").arg(place.overviewDir);
                break;
            }
        } else if (place.detailTab == 3) {
            destination = place.itemNumber > 0
                              ? QStringLiteral("Agent session %1").arg(place.itemNumber)
                              : QStringLiteral("Agents");
        } else if (m_repoDetailTabs && m_repoDetailTabs->button(place.detailTab)) {
            destination = m_repoDetailTabs->button(place.detailTab)->text();
            if (place.detailTab == 2) { // Issues
                if (place.subTab > 0 && m_issueTabGroup &&
                    m_issueTabGroup->button(place.subTab))
                    destination = m_issueTabGroup->button(place.subTab)->text();
                if (place.itemNumber > 0)
                    destination = QStringLiteral("Issue #%1").arg(place.itemNumber);
            } else if (place.detailTab == 5 && place.itemNumber > 0) { // Discussions
                destination =
                    QStringLiteral("Discussion #%1").arg(place.itemNumber);
            } else if (place.detailTab == 4 && place.itemNumber > 0) { // Pulls
                destination = QStringLiteral("PR #%1").arg(place.itemNumber);
                if (place.subTab > 0 && m_pullSubTabs &&
                    m_pullSubTabs->button(place.subTab))
                    destination =
                        QStringLiteral("%1 · %2")
                            .arg(destination,
                                 m_pullSubTabs->button(place.subTab)->text());
            }
        } else {
            destination = QStringLiteral("Repository");
        }
    } else {
        switch (place.section) {
        case 0:
            destination = QStringLiteral("Home");
            break;
        case 1:
            destination = QStringLiteral("Settings");
            if (m_settingsSections && place.subTab >= 0 &&
                place.subTab < m_settingsSections->count())
                destination = QStringLiteral("Settings · %1")
                                  .arg(m_settingsSections->tabText(place.subTab));
            break;
        case 2: {
            destination = QStringLiteral("Chat");
            if (!place.conversation.isEmpty()) {
                const QString room =
                    isDirectConversation(place.conversation)
                        ? kDmPrefix + m_dmNames.value(dmPeerId(place.conversation),
                                                      QStringLiteral("unknown"))
                        : place.conversation;
                destination = QStringLiteral("Chat · %1").arg(room);
            }
            break;
        }
        case 3:
            destination = QStringLiteral("Pings");
            break;
        case 4:
            destination = QStringLiteral("Log");
            break;
        case 6:
            destination = QStringLiteral("Search results");
            break;
        case 10:
            destination = QStringLiteral("Node profile");
            break;
        case kNetworkReposSectionIndex:
            destination = QStringLiteral("Repos");
            break;
        case kControlNodeSectionIndex:
            destination = QStringLiteral("Control node");
            if (m_controlNodeTabs && place.subTab >= 0 &&
                place.subTab < m_controlNodeTabs->count())
                destination = QStringLiteral("Control node · %1")
                                  .arg(m_controlNodeTabs->tabText(place.subTab));
            break;
        case kNetworkDiagnosticsSectionIndex:
        case kNodesSectionIndex:
            destination = QStringLiteral("Network");
            if (m_networkTabs && place.subTab >= 0 &&
                place.subTab < m_networkTabs->count())
                destination = QStringLiteral("Network · %1")
                                  .arg(m_networkTabs->tabText(place.subTab));
            break;
        case kUsersSectionIndex:
            destination = QStringLiteral("Users");
            break;
        case kOrganizationTasksSectionIndex:
            destination = QStringLiteral("Tasks");
            break;
        case kNotesSectionIndex:
            destination = QStringLiteral("Notes");
            break;
        case kFilesSectionIndex:
            destination = QStringLiteral("Files");
            break;
        case kStatsSectionIndex:
            destination = QStringLiteral("Stats");
            break;
        case kComponentHealthSectionIndex:
            destination = QStringLiteral("Component health");
            break;
        case kMemorySectionIndex:
            destination = QStringLiteral("Memory");
            break;
        case kHomeGridSectionIndex:
            destination = QStringLiteral("Home");
            break;
        case kGovernorSectionIndex:
            destination = QStringLiteral("Memory · Agent governor");
            break;
        case kSessionsSectionIndex:
            destination = QStringLiteral("Sessions");
            break;
        case kProjectSettingsSectionIndex:
            destination = place.itemId.isEmpty()
                              ? QStringLiteral("Project settings")
                              : QStringLiteral("Project settings · %1").arg(place.itemId);
            break;
        case kCalendarSectionIndex:
            destination = QStringLiteral("Calendar");
            break;
        case kMarketSectionIndex:
            destination = QStringLiteral("Market");
            break;
        case kLedgerSectionIndex:
            destination = QStringLiteral("Ledger");
            break;
        case kTeamSectionIndex:
            destination = QStringLiteral("Special Agents");
            if (place.subTab == 1)
                destination += QStringLiteral(" · Stats");
            else if (place.subTab == 2)
                destination += place.itemId.isEmpty()
                                   ? QStringLiteral(" · Briefing")
                                   : QStringLiteral(" · Briefing · @%1")
                                         .arg(place.itemId);
            else if (!place.itemId.isEmpty())
                destination += QStringLiteral(" · @%1").arg(place.itemId);
            break;
        default:
            destination = QStringLiteral("Section %1").arg(place.section);
            break;
        }
    }

    return repo.isEmpty() ? destination
                          : QStringLiteral("%1 · %2").arg(repo, destination);
}

void MainWindow::updateNavHistoryButtons()
{
    const bool canBack = m_navHistoryIndex > 0;
    const bool canForward = m_navHistoryIndex >= 0 &&
                            m_navHistoryIndex < m_navHistory.size() - 1;
    if (m_navBackButton) {
        m_navBackButton->setEnabled(canBack);
        m_navBackButton->setToolTip(
            canBack ? QStringLiteral("Back to %1\nHold for recent history")
                          .arg(navPlaceLabel(m_navHistory.at(m_navHistoryIndex - 1)))
                    : QStringLiteral("Back"));
    }
    if (m_navForwardButton) {
        m_navForwardButton->setEnabled(canForward);
        m_navForwardButton->setToolTip(
            canForward ? QStringLiteral("Forward to %1\nHold for recent history")
                             .arg(navPlaceLabel(m_navHistory.at(m_navHistoryIndex + 1)))
                       : QStringLiteral("Forward"));
    }
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testNavBackToolTip() const
{
    return m_navBackButton ? m_navBackButton->toolTip() : QString();
}

QString MainWindow::testNavForwardToolTip() const
{
    return m_navForwardButton ? m_navForwardButton->toolTip() : QString();
}

QStringList MainWindow::testNavHistoryMenuEntries(bool forward)
{
    QStringList entries;
    if (QMenu *menu = buildNavHistoryMenu(forward)) {
        const QList<QAction *> rows = menu->actions();
        for (const QAction *row : rows)
            entries << row->text(); // exactly the rows a hold would show
        delete menu;
    }
    return entries;
}

void MainWindow::testNavHistoryMenuJump(bool forward, int row)
{
    QMenu *menu = buildNavHistoryMenu(forward);
    if (!menu)
        return;
    const QList<QAction *> rows = menu->actions();
    const int target =
        (row >= 0 && row < rows.size()) ? rows.at(row)->data().toInt() : -1;
    delete menu;
    if (target >= 0)
        restoreNavEntry(target);
}

void MainWindow::testClickNavButton(bool forward, bool afterLongPress)
{
    QPushButton *b = forward ? m_navForwardButton : m_navBackButton;
    if (!b || !b->isEnabled())
        return;
    if (!afterLongPress) {
        b->click(); // press, release, click — the ordinary tap
        return;
    }
    // A press-and-hold, stopping short of the popup itself: the menu owns a
    // nested event loop no test can click through. What is worth proving is
    // that the click the release still delivers afterwards goes nowhere.
    emit b->pressed();
    m_navLongPressTimer->stop();
    m_navMenuFromLongPress = true; // the hold's timeout, minus its menu
    emit b->released();
    emit b->clicked();
}

int MainWindow::testFilesStackPage() const
{
    return m_filesStack ? m_filesStack->currentIndex() : -1;
}

bool MainWindow::testFilesSectionShowing() const
{
    return m_sectionStack &&
           m_sectionStack->currentIndex() == kFilesSectionIndex;
}

QString MainWindow::testOpenCommitHash() const
{
    return (m_commitsStack &&
            m_commitsStack->currentIndex() == kCommitWorkspaceCommitPage)
               ? m_currentCommitHash
               : QString();
}
#endif

namespace {
constexpr int kSrKindRole = Qt::UserRole;     // SearchResultKind
constexpr int kSrPathRole = Qt::UserRole + 1; // file path or commit hash
constexpr int kSrBaseRole = Qt::UserRole + 2; // a bucket's base title (for its count)
constexpr int kSrLineRole = Qt::UserRole + 3; // file match's line number
constexpr int kSrNumberRole = Qt::UserRole + 4; // collaboration record/run id
constexpr int kSrRepoRole = Qt::UserRole + 5;   // owning repository index
constexpr int kSrHitKindRole = Qt::UserRole + 6; // GlobalSearchHit::kind
constexpr int kSrLoadingRole = Qt::UserRole + 7; // bucket still searching
constexpr int kSrIconRole = Qt::UserRole + 8; // bucket's idle icon name
constexpr int kSrKeyRole = Qt::UserRole + 9;  // saved-search result key (empty = never new)
constexpr int kSrMatchTextRole = Qt::UserRole + 10;
enum SearchResultKind { SrBucket = 0, SrFile, SrCommit, SrGlobal };
enum SearchCat { ScFile = 0, ScMessage, ScHistory };
constexpr int kMaxSearchPageRecords = 500;

// The slice of the catalog the results-page worker needs. RepositoryRecord
// itself stays on the GUI thread.
struct SearchRepoRow {
    int index = 0;
    QString slug;
    QString description;
};

void setSearchBucketIcon(QTreeWidgetItem *bucket, bool loading, int angle)
{
    if (!bucket)
        return;
    const QString icon = bucket->data(0, kSrIconRole).toString();
    bucket->setIcon(
        0, loading
               ? QIcon(rotatedTintedOcticonPixmap(
                     QStringLiteral("sync"), Theme::remapColor(QColor("#8b949e")), 15, angle))
               : themedOcticon(icon, Theme::remapColor(QColor("#8b949e")), 15));
}

QString savedSearchKindTitle(const QString &kind)
{
    if (kind == QLatin1String("file-content"))
        return QStringLiteral("Code");
    if (kind == QLatin1String("commit-message"))
        return QStringLiteral("Commit messages");
    if (kind == QLatin1String("history"))
        return QStringLiteral("History");
    return QString::fromLatin1(categoryAt(categoryIndexOf(kind)).title);
}

// What stays the same about one result from run to run, hashed. Log lines get
// no key and so never count as new: filing a ping for a saved search writes a
// log line that matches it, and the next run would ping about its own ping.
QString savedSearchHitKey(const GlobalSearchHit &hit)
{
    if (hit.kind.isEmpty() || hit.kind == QLatin1String("log"))
        return {};
    QString identity;
    if (hit.kind == QLatin1String("issue") || hit.kind == QLatin1String("pull") ||
        hit.kind == QLatin1String("discussion") ||
        hit.kind == QLatin1String("project"))
        identity = hit.path.isEmpty()
                       ? QString::number(hit.number)
                       : QString::number(hit.number) + QLatin1Char('\n') + hit.path;
    else if (hit.kind == QLatin1String("agent") || hit.kind == QLatin1String("action"))
        identity = QString::number(hit.number); // status and title may change
    else if (hit.kind == QLatin1String("repo"))
        identity = hit.primary; // the slug; the row index is not stable
    else if (hit.kind == QLatin1String("file-content") ||
             hit.kind == QLatin1String("worktree-code") ||
             hit.kind == QLatin1String("project-file"))
        identity = hit.path + QLatin1Char('\n') + hit.detail; // lines shift
    else if (hit.kind == QLatin1String("commit-message") ||
             hit.kind == QLatin1String("history"))
        identity = hit.path; // the commit hash
    else
        identity = hit.path + QLatin1Char('\n') + hit.primary;
    return SavedSearchStore::resultKey(hit.kind, identity);
}

QString savedSearchHitText(const GlobalSearchHit &hit, const QString &repoLabel)
{
    return QStringList{repoLabel, hit.primary, hit.path, hit.detail}.join(
        QLatin1Char('\n'));
}
} // namespace

// Everything the category searches read, copied on the GUI thread so the page
// workers and a background saved-search run can match off it.
struct MainWindow::SearchPageInputs {
    QString query;
    int repoIndex = -1;
    bool repoOpen = false;
    QString dir; // Git directory of the searched repository
    QString ref;
    QString localPath;
    QString mirrorPath;
    QString repoOwner;
    QString repoName;
    const ForkMeshIdentity *identity = nullptr;
    QString userName;
    QList<AgentSession> sessions;
    QHash<int, AgentDiffStat> diffStats;
    QList<ActionRun> runs;
    struct LogSnapshot {
        QMutex mutex;
        QStringList lines;
    };
    std::shared_ptr<LogSnapshot> networkLog = std::make_shared<LogSnapshot>();
    QVector<GlobalSearchGitRepo> gitRepos;
    QVector<SearchRepoRow> repoRows;
    QVector<GlobalSearchGitRepo> recordRepos;
    bool singleRepo = false;
    std::optional<AgentStore> agentStore;
};

std::shared_ptr<const MainWindow::SearchPageInputs>
MainWindow::searchPageInputs(int repoIndex, const QString &query) const
{
    auto in = std::make_shared<SearchPageInputs>();
    in->query = query;
    in->repoIndex = repoIndex;
    in->repoOpen = repoIndex >= 0 && repoIndex < m_repositories.size();
    if (in->repoOpen) {
        const RepositoryRecord &repo = m_repositories.at(repoIndex);
        // Same rule as repoGitDir(): the working clone, else the mirror.
        if (!repo.localPath.isEmpty() && QDir(repo.localPath).exists())
            in->dir = repo.localPath;
        else if (!repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists())
            in->dir = repo.mirrorPath;
        const RepositoryRecord record = writableRecordFor(repo);
        in->localPath = record.localPath;
        in->mirrorPath = record.mirrorPath;
        in->repoOwner = record.owner;
        in->repoName = record.name;
    }
    if (!in->dir.isEmpty())
        in->ref = repoIndex == m_repoDetailIndex ? currentRef() : QStringLiteral("HEAD");
    in->identity = &m_profileIdentity;
    in->userName = m_userName;
    in->sessions = m_agentSessions;
    in->diffStats = m_agentDiffStats;
    in->runs = m_actionRuns;
    in->networkLog->lines = m_networkLog;
    in->gitRepos = globalSearchGitRepos();
    // The searched repository leads, so its hits survive every per-kind cap.
    std::stable_partition(in->gitRepos.begin(), in->gitRepos.end(),
                          [repoIndex](const GlobalSearchGitRepo &repo) {
                              return repo.index == repoIndex;
                          });
    in->repoRows.reserve(m_repositories.size());
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &record = m_repositories.at(i);
        if (forkmesh::hiddenterms::hides(
                QStringLiteral("%1/%2\n%3\n%4\n%5")
                    .arg(record.owner, record.name, record.description,
                         record.localPath, record.mirrorPath)))
            continue;
        in->repoRows.append({i, QStringLiteral("%1/%2").arg(record.owner, record.name),
                             record.description});
    }
    if (in->repoOpen && (!in->localPath.isEmpty() || !in->mirrorPath.isEmpty()))
        in->recordRepos.append({repoIndex, searchScopeLabel(repoIndex), in->localPath,
                                in->mirrorPath});
    for (int i = 0; i < m_repositories.size(); ++i) {
        if (i == repoIndex)
            continue;
        const RepositoryRecord record = writableRecordFor(m_repositories.at(i));
        if (record.localPath.isEmpty() && record.mirrorPath.isEmpty())
            continue;
        const QString label = QStringLiteral("%1/%2").arg(record.owner, record.name);
        if (forkmesh::hiddenterms::hides(label + QLatin1Char('\n') + record.localPath +
                                         QLatin1Char('\n') + record.mirrorPath))
            continue;
        in->recordRepos.append({i, label, record.localPath, record.mirrorPath});
    }
    if (m_agentStore)
        in->agentStore.emplace(*m_agentStore);
    return in;
}

QVector<GlobalSearchHit> MainWindow::searchPageKindHits(const SearchPageInputs &in,
                                                        const QString &kind)
{
    const auto recordHit = [&in](const QString &hitKind, int number,
                                 const QString &path, const QString &primary,
                                 const QString &detail,
                                 const QString &status = QString()) {
        GlobalSearchHit hit;
        hit.kind = hitKind;
        hit.number = number;
        hit.path = path;
        hit.primary = primary;
        hit.detail = detail;
        hit.status = status;
        hit.repoIndex = in.repoIndex;
        return hit;
    };

    if (!in.singleRepo &&
        (kind == QLatin1String("issue") || kind == QLatin1String("pull") ||
         kind == QLatin1String("discussion") || kind == QLatin1String("project") ||
         kind == QLatin1String("milestone"))) {
        QVector<GlobalSearchHit> hits;
        for (const GlobalSearchGitRepo &repo : in.recordRepos) {
            SearchPageInputs one = in;
            one.singleRepo = true;
            one.repoIndex = repo.index;
            one.localPath = repo.localPath;
            one.mirrorPath = repo.mirrorPath;
            const bool other = repo.index != in.repoIndex;
            for (GlobalSearchHit &hit : searchPageKindHits(one, kind)) {
                if (other) {
                    if (kind == QLatin1String("milestone"))
                        hit.primary = repo.label + QStringLiteral(" \u00b7 ") + hit.primary;
                    else
                        hit.path = repo.label;
                    hit.detail = repo.label + QStringLiteral(" \u00b7 ") + hit.detail;
                }
                hits.append(std::move(hit));
                if (hits.size() >= kMaxSearchPageRecords)
                    return hits;
            }
        }
        return hits;
    }

    if (kind == QLatin1String("repo")) {
        QVector<GlobalSearchHit> hits;
        for (const SearchRepoRow &repo : in.repoRows) {
            QString where;
            if (containsFold(repo.slug, in.query))
                where = QStringLiteral("repository");
            else if (containsFold(repo.description, in.query))
                where = snippetAround(repo.description, in.query);
            else
                continue;
            GlobalSearchHit hit = recordHit(QStringLiteral("repo"), repo.index,
                                            QString(), repo.slug,
                                            repo.index == in.repoIndex
                                                ? where + QStringLiteral(" · open")
                                                : where);
            hit.repoIndex = repo.index;
            hits.append(std::move(hit));
            if (hits.size() >= kMaxSearchPageRecords)
                break;
        }
        return hits;
    }
    if (kind == QLatin1String("issue")) {
        QVector<GlobalSearchHit> hits;
        if (in.localPath.isEmpty() && in.mirrorPath.isEmpty())
            return hits;
        const IssueStore store(in.localPath, in.mirrorPath, in.identity, in.userName);
        for (const Issue &issue : store.loadAll()) {
            if (hits.size() >= kMaxSearchPageRecords)
                break;
            if (issue.isDeleted())
                continue;
            QString where;
            const QString identityText =
                QStringLiteral("#%1 %2").arg(issue.number).arg(issue.title);
            if (!matchIssue(issue, in.query, &where) &&
                !containsFold(identityText, in.query))
                continue;
            if (where.isEmpty())
                where = QStringLiteral("issue number");
            hits.append(recordHit(QStringLiteral("issue"), issue.number,
                                  QString(), identityText, where, issue.status));
        }
        return hits;
    }
    if (kind == QLatin1String("milestone")) {
        QVector<GlobalSearchHit> hits;
        if (in.localPath.isEmpty() && in.mirrorPath.isEmpty())
            return hits;
        const IssueStore store(in.localPath, in.mirrorPath, in.identity, in.userName);
        for (const IssueMilestone &milestone : store.loadMilestones()) {
            if (hits.size() >= kMaxSearchPageRecords)
                break;
            QString where;
            if (!matchTitleBody(in.query, milestone.title, milestone.description,
                                &where))
                continue;
            hits.append(recordHit(QStringLiteral("milestone"), 0,
                                  milestone.title, milestone.title, where,
                                  milestone.status));
        }
        return hits;
    }
    if (kind == QLatin1String("pull")) {
        QVector<GlobalSearchHit> hits;
        if (in.localPath.isEmpty() && in.mirrorPath.isEmpty())
            return hits;
        const PullStore store(in.localPath, in.mirrorPath, in.identity, in.userName);
        for (const PullRequest &pull : store.loadAll()) {
            if (hits.size() >= kMaxSearchPageRecords)
                break;
            QString where;
            const QString identityText =
                QStringLiteral("#%1 %2").arg(pull.number).arg(pull.title);
            if (!matchPull(pull, in.query, &where) &&
                !containsFold(identityText, in.query))
                continue;
            if (where.isEmpty())
                where = QStringLiteral("pull request number");
            hits.append(recordHit(QStringLiteral("pull"), pull.number,
                                  QString(), identityText, where,
                                  pullDisplayStatus(pull)));
        }
        return hits;
    }
    if (kind == QLatin1String("discussion")) {
        QVector<GlobalSearchHit> hits;
        if (in.localPath.isEmpty() && in.mirrorPath.isEmpty())
            return hits;
        const DiscussionStore store(in.localPath, in.mirrorPath, in.identity, in.userName);
        for (const Discussion &discussion : store.loadAll()) {
            if (hits.size() >= kMaxSearchPageRecords)
                break;
            QString where;
            const QString identityText = QStringLiteral("#%1 %2")
                                             .arg(discussion.number)
                                             .arg(discussion.title);
            if (!matchDiscussion(discussion, in.query, &where) &&
                !containsFold(identityText, in.query))
                continue;
            if (where.isEmpty())
                where = QStringLiteral("discussion number");
            const QString detail =
                discussion.category.isEmpty()
                    ? where
                    : discussion.category + QStringLiteral(" · ") + where;
            hits.append(recordHit(
                QStringLiteral("discussion"), discussion.number, QString(),
                identityText, detail, discussion.status));
        }
        return hits;
    }
    if (kind == QLatin1String("project")) {
        QVector<GlobalSearchHit> hits;
        if (in.localPath.isEmpty() && in.mirrorPath.isEmpty())
            return hits;
        const ProjectStore store(in.localPath, in.mirrorPath, in.identity, in.userName);
        for (const Project &project : store.loadAll()) {
            if (hits.size() >= kMaxSearchPageRecords)
                break;
            if (project.isDeleted())
                continue;
            QString where;
            const QString identityText =
                QStringLiteral("#%1 %2").arg(project.number).arg(project.title);
            if (!matchProject(project, in.query, &where) &&
                !containsFold(identityText, in.query))
                continue;
            if (where.isEmpty())
                where = QStringLiteral("project number");
            hits.append(recordHit(QStringLiteral("project"), project.number,
                                  QString(), identityText, where,
                                  project.status));
        }
        return hits;
    }
    if (kind == QLatin1String("agent")) {
        QVector<GlobalSearchHit> hits;
        if (!in.repoOpen)
            return hits;
        for (const AgentSession &session : in.sessions) {
            if (hits.size() >= kMaxSearchPageRecords ||
                session.owner != in.repoOwner || session.name != in.repoName)
                continue;
            QString where;
            if (containsFold(session.issueTitle, in.query))
                where = QStringLiteral("task");
            else if (containsFold(session.prompt, in.query))
                where = snippetAround(session.prompt, in.query);
            else if (containsFold(session.branchName, in.query))
                where = QStringLiteral("branch ") + session.branchName;
            else if (containsFold(in.diffStats.value(session.id).worktree, in.query))
                where = QStringLiteral("worktree ") +
                        in.diffStats.value(session.id).worktree;
            QString transcriptSnippet;
            const int transcriptHits =
                in.agentStore ? in.agentStore->searchTranscript(
                                    session, in.query, &transcriptSnippet)
                              : 0;
            if (where.isEmpty() && transcriptHits == 0)
                continue;
            if (transcriptHits > 0) {
                const QString transcriptWhere =
                    QStringLiteral("%1 transcript match%2: %3")
                        .arg(transcriptHits)
                        .arg(transcriptHits == 1 ? QString() : QStringLiteral("es"))
                        .arg(transcriptSnippet);
                where = where.isEmpty()
                            ? transcriptWhere
                            : where + QStringLiteral(" · ") + transcriptWhere;
            }
            const QString title = session.issueTitle.isEmpty()
                                      ? session.prompt.simplified().left(100)
                                      : session.issueTitle;
            hits.append(recordHit(
                QStringLiteral("agent"), session.id, QString(),
                title.isEmpty() ? QStringLiteral("Session %1").arg(session.id)
                                : title,
                session.status + QStringLiteral(" · ") + where));
        }
        return hits;
    }
    if (kind == QLatin1String("action")) {
        QVector<GlobalSearchHit> hits;
        if (!in.repoOpen)
            return hits;
        for (const ActionRun &run : in.runs) {
            if (hits.size() >= kMaxSearchPageRecords || run.owner != in.repoOwner ||
                run.name != in.repoName)
                continue;
            QString where;
            if (containsFold(run.workflowName, in.query))
                where = QStringLiteral("workflow");
            else if (containsFold(run.workflowPath, in.query))
                where = run.workflowPath;
            else if (containsFold(run.workflowContent, in.query))
                where = snippetAround(run.workflowContent, in.query);
            else if (containsFold(run.ref, in.query))
                where = run.ref;
            else if (containsFold(run.commit, in.query))
                where = run.commit.left(12);
            else
                continue;
            hits.append(recordHit(
                QStringLiteral("action"), run.id, QString(),
                QStringLiteral("%1  #%2")
                    .arg(run.workflowName.isEmpty() ? run.workflowPath
                                                     : run.workflowName)
                    .arg(run.id),
                run.status + QStringLiteral(" · ") + where));
        }
        return hits;
    }
    if (kind == QLatin1String("log")) {
        QStringList networkLog;
        {
            const QMutexLocker locker(&in.networkLog->mutex);
            networkLog.swap(in.networkLog->lines);
        }
        networkLog.detach();
        QVector<GlobalSearchHit> hits;
        for (auto it = networkLog.crbegin();
             it != networkLog.crend() && hits.size() < kMaxSearchPageRecords;
             ++it) {
            if (!containsFold(*it, in.query))
                continue;
            hits.append(recordHit(QStringLiteral("log"), 0, *it,
                                  snippetAround(it->trimmed(), in.query),
                                  QStringLiteral("network and runtime log")));
        }
        return hits;
    }
    if (kind == QLatin1String("git-catalog"))
        return scanAllGitCatalogHits(in.gitRepos, in.repoIndex, in.ref, in.query,
                                     kMaxSearchPageRecords);
    if (kind == QLatin1String("worktree-code"))
        return scanWorktreeCodeHits(in.dir, in.ref, in.query, kMaxSearchPageRecords);
    if (kind == QLatin1String("project-file"))
        return scanProjectFileHits(in.dir, in.query, kMaxSearchPageRecords);
    return {};
}

static QList<QTextEdit::ExtraSelection> termSelections(QTextDocument *doc,
                                                       const QString &term)
{
    QList<QTextEdit::ExtraSelection> sels;
    if (!doc || term.isEmpty())
        return sels;
    QTextCharFormat fmt;
    fmt.setBackground(QColor("#e3b341"));
    fmt.setForeground(Theme::remapColor(QColor("#0d1117")));
    QTextCursor c = doc->find(term, 0);
    while (!c.isNull()) {
        QTextEdit::ExtraSelection sel;
        sel.cursor = c;
        sel.format = fmt;
        sels.append(sel);
        if (sels.size() >= 5000)
            break; // safety cap on pathological match counts
        c = doc->find(term, c);
    }
    return sels;
}

QWidget *MainWindow::buildSearchResultsSection()
{
    auto *page = new QWidget;
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(16, 12, 16, 16);
    outer->setSpacing(16);

    auto *results = new QWidget;
    auto *layout = new QVBoxLayout(results);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *headerRow = new QHBoxLayout;
    headerRow->setSpacing(8);
    auto *back = new QPushButton(QStringLiteral("Back"));
    back->setObjectName("ghostButton");
    back->setCursor(Qt::PointingHandCursor);
    setOcticon(back, "arrow-left", 16);
    connect(back, &QPushButton::clicked, this, [this] { showSection(0); });
    m_searchResultsTitle = new QLabel;
    m_searchResultsTitle->setObjectName("searchResultsTitle");
    m_searchResultsTitle->setTextFormat(Qt::RichText);
    m_searchResultsSave = new QPushButton(QStringLiteral("Save search"));
    m_searchResultsSave->setObjectName("ghostButton");
    m_searchResultsSave->setCursor(Qt::PointingHandCursor);
    setOcticon(m_searchResultsSave, "star", 14);
    m_searchResultsSave->hide();
    connect(m_searchResultsSave, &QPushButton::clicked, this,
            [this] { showSaveSearchPrompt(); });
    m_searchResultsStop = new QPushButton(QStringLiteral("Stop"));
    m_searchResultsStop->setObjectName("ghostButton");
    m_searchResultsStop->setCursor(Qt::PointingHandCursor);
    setOcticon(m_searchResultsStop, "x", 14);
    m_searchResultsStop->hide();
    connect(m_searchResultsStop, &QPushButton::clicked, this, [this] { stopSearch(); });
    m_searchResultsScrub = new QPushButton(QStringLiteral("Scrub\u2026"));
    m_searchResultsScrub->setObjectName("ghostButton");
    m_searchResultsScrub->setCursor(Qt::PointingHandCursor);
    setOcticon(m_searchResultsScrub, "trash", 14);
    m_searchResultsScrub->setToolTip(QStringLiteral(
        "Replace this term in the repository's source and rewrite it out of every "
        "branch's history, with the Scrub a term shortcut"));
    m_searchResultsScrub->hide();
    connect(m_searchResultsScrub, &QPushButton::clicked, this, [this] {
        const QString term = m_searchPageQuery;
        const QString scope = m_searchRunScope;
        QTimer::singleShot(0, this, [this, term, scope] { scrubSearchTerm(term, scope); });
    });
    headerRow->addWidget(back);
    headerRow->addWidget(m_searchResultsTitle, 1);
    headerRow->addWidget(m_searchResultsScrub);
    headerRow->addWidget(m_searchResultsSave);
    headerRow->addWidget(m_searchResultsStop);
    layout->addLayout(headerRow);

    // Asked inline rather than in a dialog: the results stay readable while
    // deciding, and nothing modal can park a headless node.
    m_saveSearchPrompt = new QFrame;
    m_saveSearchPrompt->setObjectName("saveSearchPrompt");
    auto *promptLayout = new QHBoxLayout(m_saveSearchPrompt);
    promptLayout->setContentsMargins(12, 8, 8, 8);
    promptLayout->setSpacing(10);
    m_saveSearchPromptLabel = new QLabel;
    m_saveSearchPromptLabel->setObjectName("saveSearchPromptLabel");
    m_saveSearchPromptLabel->setTextFormat(Qt::PlainText);
    m_saveSearchPromptLabel->setWordWrap(true);
    m_saveSearchAlertsCheck = new QCheckBox(QStringLiteral("Ping me about new results"));
    m_saveSearchAlertsCheck->setObjectName("saveSearchAlerts");
    m_saveSearchSecurityCheck = new QCheckBox(QStringLiteral("Security pertinent"));
    m_saveSearchSecurityCheck->setObjectName("saveSearchSecurity");
    m_saveSearchSecurityCheck->setToolTip(QStringLiteral(
        "Sound a continual alarm whenever this search has any results, until "
        "you acknowledge them"));
    auto *confirm = new QPushButton(QStringLiteral("Save"));
    confirm->setObjectName("primaryButton");
    confirm->setCursor(Qt::PointingHandCursor);
    connect(confirm, &QPushButton::clicked, this, [this] { saveCurrentSearch(); });
    auto *notNow = new QPushButton(QStringLiteral("Not now"));
    notNow->setObjectName("ghostButton");
    notNow->setCursor(Qt::PointingHandCursor);
    connect(notNow, &QPushButton::clicked, this, [this] { dismissSaveSearchPrompt(); });
    promptLayout->addWidget(m_saveSearchPromptLabel, 1);
    promptLayout->addWidget(m_saveSearchAlertsCheck);
    promptLayout->addWidget(m_saveSearchSecurityCheck);
    promptLayout->addWidget(confirm);
    promptLayout->addWidget(notNow);
    m_saveSearchPrompt->hide();
    layout->addWidget(m_saveSearchPrompt);

    m_searchResultsStatus = new QLabel;
    m_searchResultsStatus->setObjectName("navCaption");
    layout->addWidget(m_searchResultsStatus);

    m_searchResultsTree = new QTreeWidget;
    m_searchResultsTree->setObjectName("searchResultsTree");
    m_searchResultsTree->setHeaderHidden(true);
    // Column 1 only holds the Delete all button on a Logs or Workflow runs
    // group header; result rows span both columns.
    m_searchResultsTree->setColumnCount(2);
    m_searchResultsTree->header()->setStretchLastSection(false);
    m_searchResultsTree->header()->setMinimumSectionSize(0);
    m_searchResultsTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_searchResultsTree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_searchResultsTree->header()->resizeSection(1, 0);
    m_searchResultsTree->setUniformRowHeights(true);
    SearchHitDelegate::install(m_searchResultsTree);
    m_searchResultsTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_searchResultsTree->setExpandsOnDoubleClick(false);
    m_searchResultsTree->setCursor(Qt::PointingHandCursor);
    connect(m_searchResultsTree, &QTreeWidget::itemClicked, this,
            &MainWindow::onSearchResultActivated);
    connect(m_searchResultsTree, &QTreeWidget::itemActivated, this,
            &MainWindow::onSearchResultActivated);
    // Log lines can be deleted one at a time, from the right-click menu or
    // with Delete on the selected row.
    m_searchResultsTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_searchResultsTree, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
                if (!m_searchResultsTree)
                    return;
                QTreeWidgetItem *item = m_searchResultsTree->itemAt(position);
                if (!isSearchResultLogRow(item))
                    return;
                m_searchResultsTree->setCurrentItem(item);
                QMenu menu(m_searchResultsTree);
                QAction *open = menu.addAction(QStringLiteral("Show in log"));
                open->setIcon(themedOcticon(QStringLiteral("link-external"),
                                            QColor(QStringLiteral("#8b949e")), 14));
                QAction *remove = menu.addAction(QStringLiteral("Delete log entry"));
                remove->setIcon(themedOcticon(QStringLiteral("trash"),
                                              QColor(QStringLiteral("#f85149")), 14));
                const QAction *chosen = menu.exec(
                    m_searchResultsTree->viewport()->mapToGlobal(position));
                if (chosen == open)
                    onSearchResultActivated(item, 0);
                else if (chosen == remove)
                    deleteSearchResultLogRow(item);
            });
    auto *deleteRow = new QShortcut(QKeySequence::Delete, m_searchResultsTree);
    deleteRow->setContext(Qt::WidgetShortcut);
    connect(deleteRow, &QShortcut::activated, this, [this] {
        if (m_searchResultsTree &&
            isSearchResultLogRow(m_searchResultsTree->currentItem()))
            deleteSearchResultLogRow(m_searchResultsTree->currentItem());
    });
    layout->addWidget(m_searchResultsTree, 1);
    outer->addWidget(results, 1);

    auto *panel = new QFrame;
    panel->setObjectName("savedSearchPanel");
    panel->setFixedWidth(300);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 8, 0, 0);
    panelLayout->setSpacing(6);
    auto *heading = new QLabel(QStringLiteral("Saved searches"));
    heading->setObjectName("issueSidebarHeading");
    m_savedSearchCaption = new QLabel;
    m_savedSearchCaption->setObjectName("navCaption");
    m_savedSearchList = new QListWidget;
    m_savedSearchList->setObjectName("savedSearchList");
    m_savedSearchList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_savedSearchList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_savedSearchList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_savedSearchList->setCursor(Qt::PointingHandCursor);
    connect(m_savedSearchList, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
                if (!item)
                    return;
                // Running it rebuilds this list, deleting the clicked item.
                const QString id = item->data(Qt::UserRole).toString();
                QTimer::singleShot(0, this, [this, id] { openSavedSearch(id); });
            });
    m_savedSearchEmpty = new QLabel(QStringLiteral(
        "Save a search to run it again from here. Turn on its bell to get a "
        "ping when new results show up."));
    m_savedSearchEmpty->setObjectName("searchEmptyNotice");
    m_savedSearchEmpty->setWordWrap(true);
    m_savedSearchEmpty->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto *headingRow = new QHBoxLayout;
    headingRow->setContentsMargins(0, 0, 8, 0);
    headingRow->addWidget(heading, 1);
    m_savedSearchRefreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_savedSearchRefreshButton->setObjectName("savedSearchRefreshButton");
    m_savedSearchRefreshButton->setCursor(Qt::PointingHandCursor);
    m_savedSearchRefreshButton->setIcon(
        themedOcticon(QStringLiteral("sync"), QColor("#8b949e"), 14));
    m_savedSearchRefreshButton->setToolTip(QStringLiteral(
        "Run every saved search again now and update its counts"));
    connect(m_savedSearchRefreshButton, &QPushButton::clicked, this,
            [this] { refreshAllSavedSearches(); });
    headingRow->addWidget(m_savedSearchRefreshButton);
    panelLayout->addLayout(headingRow);
    panelLayout->addWidget(m_savedSearchCaption);
    auto *hiddenRow = new QHBoxLayout;
    hiddenRow->setContentsMargins(0, 0, 8, 0);
    m_searchHiddenCaption = new QLabel;
    m_searchHiddenCaption->setObjectName("searchHiddenCaption");
    m_searchHiddenCaption->setWordWrap(true);
    m_searchHiddenCaption->setTextFormat(Qt::PlainText);
    hiddenRow->addWidget(m_searchHiddenCaption, 1);
    auto *editHidden = new QPushButton(QStringLiteral("Edit hidden\u2026"));
    editHidden->setObjectName("searchHiddenEditButton");
    editHidden->setCursor(Qt::PointingHandCursor);
    editHidden->setToolTip(QStringLiteral(
        "Words, repository names or paths that never appear in any search "
        "(repositories, worktrees, code, records, logs) and are replaced with "
        "[hidden] in the log"));
    connect(editHidden, &QPushButton::clicked, this, [this] {
        QTimer::singleShot(0, this, [this] { editSearchHiddenTerms(); });
    });
    hiddenRow->addWidget(editHidden);
    panelLayout->addLayout(hiddenRow);
    m_searchHideVariablesCheck =
        new QCheckBox(QStringLiteral("Also hide every Actions variable value"));
    m_searchHideVariablesCheck->setObjectName("searchHideVariablesCheck");
    m_searchHideVariablesCheck->setCursor(Qt::PointingHandCursor);
    m_searchHideVariablesCheck->setToolTip(QStringLiteral(
        "Treat the value of every variable and secret in Settings > Secrets & Coves "
        "as hidden: it never appears in any search and is replaced with [hidden] in "
        "the log, telemetry and pings. Values shorter than 8 characters are skipped."));
    m_searchHideVariablesCheck->setChecked(forkmesh::hiddenterms::hidesVariables());
    connect(m_searchHideVariablesCheck, &QCheckBox::toggled, this,
            [this](bool checked) { setSearchHidesVariables(checked); });
    panelLayout->addWidget(m_searchHideVariablesCheck);
    panelLayout->addWidget(m_savedSearchList, 1);
    panelLayout->addWidget(m_savedSearchEmpty, 1);
    outer->addWidget(panel);
    refreshSavedSearchList();
    return page;
}

void MainWindow::openSavedSearchesPage()
{
    showSection(6);
    stopSearch(); // cancel any run still in flight
    m_searchPageQuery.clear();
    m_searchRunKeys.clear();
    m_searchRunKindCounts.clear();
    if (m_searchResultsTitle)
        m_searchResultsTitle->setText(QStringLiteral("<b>Search</b>"));
    if (m_searchResultsTree)
        m_searchResultsTree->clear();
    if (m_searchResultsStatus)
        m_searchResultsStatus->setText(QString::fromUtf8(
            "Type in the search box and press Enter to search everything, or run "
            "one of your saved searches."));
    if (m_saveSearchPrompt)
        m_saveSearchPrompt->hide();
    updateSaveSearchButton();
    refreshSavedSearchList();
}

void MainWindow::openSearchResultsPage(const QString &query)
{
    const QString q = query.trimmed();
    if (q.isEmpty())
        return;
    m_searchPageQuery = q;
    showSection(6);
    if (m_searchResultsTitle)
        m_searchResultsTitle->setText(
            QString::fromUtf8("<b>Results for \xE2\x80\x9C%1\xE2\x80\x9D</b>")
                .arg(q.toHtmlEscaped()));

    stopSearch(); // cancel any run still in flight
    m_searchRunGeneration = m_searchPageGeneration;
    m_searchRunRecorded = false;
    m_searchRunScope = searchScopeLabel(m_repoDetailIndex);
    m_searchRunKeys.clear();
    m_searchRunKindCounts.clear();
    if (m_saveSearchPrompt)
        m_saveSearchPrompt->hide();
    updateSaveSearchButton();
    refreshSavedSearchList();
    if (m_searchResultsTree)
        m_searchResultsTree->clear();
    SearchHitDelegate::setQuery(m_searchResultsTree, q);

    const QString dir = repoGitDir();
    const QString ref = dir.isEmpty() ? QString() : currentRef();

    auto makeBucket = [this](const QString &base, const QString &icon,
                             const QString &key) {
        auto *b = new QTreeWidgetItem(m_searchResultsTree);
        b->setData(0, kSrKindRole, SrBucket);
        b->setData(0, kSrBaseRole, base);
        b->setData(0, kSrHitKindRole, key);
        b->setData(0, kSrIconRole, icon);
        b->setData(0, kSrLoadingRole, true);
        setSearchBucketIcon(b, true, 0);
        QFont f = b->font(0);
        f.setBold(true);
        b->setFont(0, f);
        b->setText(0, base + QString::fromUtf8(" \xE2\x80\x94 0"));
        b->setExpanded(true);
        return b;
    };

    // One bucket per searchable kind, titled and iconed from the same category
    // table the Ctrl+K overlay groups by. Each bucket gets its own read-only
    // worker (or Git process below), so it can stop spinning as soon as its
    // source is complete instead of waiting for the slowest category.
    QHash<QString, QTreeWidgetItem *> recordBuckets;
    for (const QString &kind :
         {QStringLiteral("repo"), QStringLiteral("issue"),
          QStringLiteral("pull"), QStringLiteral("discussion"),
          QStringLiteral("project"), QStringLiteral("milestone"),
          QStringLiteral("agent"), QStringLiteral("branch"),
          QStringLiteral("worktree"), QStringLiteral("tag"),
          QStringLiteral("action"), QStringLiteral("log"),
          QStringLiteral("file")}) {
        const Category &category = categoryAt(categoryIndexOf(kind));
        recordBuckets.insert(
            kind, makeBucket(QString::fromLatin1(category.title),
                             QString::fromLatin1(category.icon), kind));
    }
    QTreeWidgetItem *codeBucket =
        makeBucket(QStringLiteral("Code (current tree)"), "code",
                   QStringLiteral("file-content"));
    QTreeWidgetItem *msgBucket =
        makeBucket(QStringLiteral("Commit messages (all branches)"), "git-branch",
                   QStringLiteral("commit-message"));
    QTreeWidgetItem *histBucket = makeBucket(
        QString::fromUtf8("History \xE2\x80\x94 diffs that touched it"),
        "git-pull-request", QStringLiteral("history"));
    const Category &worktreeCodeCategory =
        categoryAt(categoryIndexOf(QStringLiteral("worktree-code")));
    QTreeWidgetItem *worktreeCodeBucket = makeBucket(
        QString::fromLatin1(worktreeCodeCategory.title),
        QString::fromLatin1(worktreeCodeCategory.icon),
        QStringLiteral("worktree-code"));
    const Category &projectFileCategory =
        categoryAt(categoryIndexOf(QStringLiteral("project-file")));
    QTreeWidgetItem *projectFileBucket = makeBucket(
        QString::fromLatin1(projectFileCategory.title),
        QString::fromLatin1(projectFileCategory.icon),
        QStringLiteral("project-file"));

    m_searchPending = 0;
    if (!dir.isEmpty()) {
        startSearchProcess(
            dir, {"grep", "-n", "-I", "-i", "-F", "-e", q, ref},
            codeBucket, ScFile);
        startSearchProcess(dir,
                           QStringList{"log", "-i", "--fixed-strings",
                                       "--grep=" + q,
                                       "--format=%H%x1f%h%x1f%ar%x1f%s%x1f%S"} +
                               allBranchRevisionArgs(ref),
                           msgBucket, ScMessage);
        startSearchProcess(dir,
                           {"log", "-i", "-G" + QRegularExpression::escape(q),
                            "--format=%H%x1f%h%x1f%ar%x1f%s", ref},
                           histBucket, ScHistory);
    }

    const int generation = m_searchPageGeneration;
    const std::shared_ptr<const SearchPageInputs> inputs =
        searchPageInputs(m_repoDetailIndex, q);

    auto appendHitsToBuckets = [this](const QList<QTreeWidgetItem *> &buckets,
                                  const QVector<GlobalSearchHit> &hits) {
        for (const GlobalSearchHit &hit : hits) {
            QTreeWidgetItem *bucket = nullptr;
            for (QTreeWidgetItem *candidate : buckets) {
                if (candidate && candidate->data(0, kSrHitKindRole).toString() ==
                                      hit.kind) {
                    bucket = candidate;
                    break;
                }
            }
            if (!bucket || bucket->childCount() >= kMaxSearchPageRecords)
                continue;
            const QString matchText = savedSearchHitText(
                hit, hit.repoIndex >= 0 ? searchScopeLabel(hit.repoIndex)
                                        : m_searchRunScope);
            if (forkmesh::hiddenterms::hides(matchText))
                continue;
            const Category &category = categoryAt(categoryIndexOf(hit.kind));
            auto *child = new QTreeWidgetItem(bucket);
            child->setFirstColumnSpanned(true);
            child->setData(0, kSrKindRole, SrGlobal);
            child->setData(0, kSrPathRole, hit.path);
            child->setData(0, kSrNumberRole, hit.number);
            child->setData(0, kSrRepoRole, hit.repoIndex);
            child->setData(0, kSrHitKindRole, hit.kind);
            child->setData(0, kSrLineRole, hit.line);
            child->setData(0, kSrKeyRole, savedSearchHitKey(hit));
            child->setData(0, kSrMatchTextRole, matchText);
            const StatusBadge badge = setStatusBadge(child, hit.kind, hit.status);
            child->setIcon(
                0, badge.icon.isEmpty()
                       ? themedOcticon(QString::fromLatin1(category.icon),
                                       QColor(QString::fromLatin1(category.color)),
                                       15)
                       : themedOcticon(badge.icon, QColor(badge.color), 15));
            const QString label =
                hit.detail.isEmpty()
                    ? hit.primary
                    : hit.primary + QStringLiteral("  ·  ") + hit.detail;
            child->setText(0, label);
            child->setToolTip(0, label); // rows elide; the tooltip does not
            bucket->setText(
                0, bucket->data(0, kSrBaseRole).toString() +
                       QString::fromUtf8(" \xE2\x80\x94 %1")
                           .arg(bucket->childCount()));
        }
    };

    auto startWorker = [this, generation, appendHitsToBuckets](
                           const QList<QTreeWidgetItem *> &buckets,
                           std::function<QVector<GlobalSearchHit>()> work) {
        ++m_searchPending;
        runOffThread<QVector<GlobalSearchHit>>(
            std::move(work),
            [this, generation, buckets,
             appendHitsToBuckets](QVector<GlobalSearchHit> hits) {
                if (generation != m_searchPageGeneration ||
                    !m_searchResultsTree)
                    return;
                appendHitsToBuckets(buckets, hits);
                for (QTreeWidgetItem *bucket : buckets) {
                    if (!bucket)
                        continue;
                    bucket->setData(0, kSrLoadingRole, false);
                    setSearchBucketIcon(bucket, false, 0);
                }
                if (m_searchPending > 0)
                    --m_searchPending;
                updateSearchStatus();
            });
    };

    for (const QString &kind :
         {QStringLiteral("repo"), QStringLiteral("issue"),
          QStringLiteral("milestone"), QStringLiteral("pull"),
          QStringLiteral("discussion"), QStringLiteral("project"),
          QStringLiteral("agent"), QStringLiteral("action"),
          QStringLiteral("log")}) {
        startWorker({recordBuckets.value(kind)},
                    [inputs, kind] { return searchPageKindHits(*inputs, kind); });
    }
    startWorker({recordBuckets.value(QStringLiteral("branch")),
                 recordBuckets.value(QStringLiteral("worktree")),
                 recordBuckets.value(QStringLiteral("tag")),
                 recordBuckets.value(QStringLiteral("file"))},
                [inputs] {
                    return searchPageKindHits(*inputs, QStringLiteral("git-catalog"));
                });
    startWorker({worktreeCodeBucket}, [inputs] {
        return searchPageKindHits(*inputs, QStringLiteral("worktree-code"));
    });
    startWorker({projectFileBucket}, [inputs] {
        return searchPageKindHits(*inputs, QStringLiteral("project-file"));
    });
    updateSearchStatus();
}

void MainWindow::startSearchProcess(const QString &dir, const QStringList &gitArgs,
                                    QTreeWidgetItem *bucket, int cat)
{
    if (!bucket)
        return;
    auto *process = new QProcess(this);
    process->setWorkingDirectory(dir);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    auto buffer = std::make_shared<QByteArray>();
    const QString ref = currentRef();

    auto addLine = [this, bucket, cat, ref, process](const QByteArray &raw) {
        if (bucket->childCount() >= 500) { // cap the live list; stop this search
            process->kill();
            return;
        }
        const QString line = QString::fromUtf8(raw);
        if (line.trimmed().isEmpty() ||
            forkmesh::hiddenterms::hides(m_searchRunScope + QLatin1Char('\n') + line))
            return;
        if (cat == ScFile) {
            QString rest = line;
            const QString prefix = ref + QLatin1Char(':');
            if (rest.startsWith(prefix))
                rest = rest.mid(prefix.size());
            const int c1 = rest.indexOf(QLatin1Char(':'));
            const int c2 = c1 < 0 ? -1 : rest.indexOf(QLatin1Char(':'), c1 + 1);
            if (c1 < 0 || c2 < 0)
                return;
            const QString path = rest.left(c1);
            const QString lineNo = rest.mid(c1 + 1, c2 - c1 - 1);
            const QString text = rest.mid(c2 + 1).trimmed();
            auto *child = new QTreeWidgetItem(bucket);
            child->setFirstColumnSpanned(true);
            child->setData(0, kSrKindRole, SrFile);
            child->setData(0, kSrPathRole, path);
            child->setData(0, kSrLineRole, lineNo.toInt());
            GlobalSearchHit keyed;
            keyed.kind = QStringLiteral("file-content");
            keyed.path = path;
            keyed.detail = text;
            child->setData(0, kSrKeyRole, savedSearchHitKey(keyed));
            child->setData(0, kSrMatchTextRole,
                           savedSearchHitText(keyed, m_searchRunScope));
            const QString shown = text.length() > 160
                                      ? text.left(157) + QString::fromUtf8("\xE2\x80\xA6")
                                      : text;
            child->setText(0, QStringLiteral("%1:%2  %3").arg(path, lineNo, shown));
            child->setToolTip(0, QStringLiteral("%1:%2").arg(path, lineNo));
        } else {
            const QStringList f = line.split(QChar(0x1f));
            if (f.size() < 4)
                return;
            auto *child = new QTreeWidgetItem(bucket);
            child->setFirstColumnSpanned(true);
            child->setData(0, kSrKindRole, SrCommit);
            child->setData(0, kSrPathRole, f.at(0));
            GlobalSearchHit keyed;
            keyed.kind = bucket->data(0, kSrHitKindRole).toString();
            keyed.path = f.at(0);
            child->setData(0, kSrKeyRole, savedSearchHitKey(keyed));
            QString label = QString::fromUtf8("%1  %2  \xC2\xB7  %3")
                                .arg(f.at(1), f.at(3), f.at(2));
            if (!f.value(4).isEmpty()) // --source: the branch it was found on
                label += QString::fromUtf8("  \xC2\xB7  ") + f.at(4);
            keyed.primary = label;
            child->setData(0, kSrMatchTextRole,
                           savedSearchHitText(keyed, m_searchRunScope));
            child->setText(0, label);
            child->setToolTip(0, f.at(3));
        }
        bucket->setText(0, bucket->data(0, kSrBaseRole).toString() +
                               QString::fromUtf8(" \xE2\x80\x94 %1")
                                   .arg(bucket->childCount()));
    };

    connect(process, &QProcess::readyReadStandardOutput, this,
            [process, buffer, addLine] {
                buffer->append(process->readAllStandardOutput());
                int nl;
                while ((nl = buffer->indexOf('\n')) >= 0) {
                    addLine(buffer->left(nl));
                    buffer->remove(0, nl + 1);
                }
            });
    connect(process, &QProcess::finished, this,
            [this, process, buffer, addLine, bucket](int, QProcess::ExitStatus) {
                if (!buffer->isEmpty())
                    addLine(*buffer); // trailing line with no newline
                m_searchProcs.removeOne(process);
                process->deleteLater();
                bucket->setData(0, kSrLoadingRole, false);
                setSearchBucketIcon(bucket, false, 0);
                if (m_searchPending > 0)
                    --m_searchPending;
                updateSearchStatus();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, bucket](QProcess::ProcessError err) {
                if (err != QProcess::FailedToStart)
                    return; // a crash still emits finished(); let that path clean up
                m_searchProcs.removeOne(process);
                process->deleteLater();
                bucket->setData(0, kSrLoadingRole, false);
                setSearchBucketIcon(bucket, false, 0);
                if (m_searchPending > 0)
                    --m_searchPending;
                updateSearchStatus();
            });

    bucket->setData(0, kSrLoadingRole, true);
    setSearchBucketIcon(bucket, true, m_searchSpinAngle);
    m_searchProcs.append(process);
    ++m_searchPending;
    process->start(QStringLiteral("git"), gitArgs);
}

void MainWindow::onSearchResultActivated(QTreeWidgetItem *item, int)
{
    if (!item)
        return;
    const int kind = item->data(0, kSrKindRole).toInt();
    if (kind == SrBucket) {
        item->setExpanded(!item->isExpanded()); // click a header to fold / unfold
        return;
    }
    const QString payload = item->data(0, kSrPathRole).toString();
    if (kind == SrGlobal) {
        GlobalSearchHit hit;
        hit.kind = item->data(0, kSrHitKindRole).toString();
        hit.number = item->data(0, kSrNumberRole).toInt();
        hit.path = payload;
        hit.line = item->data(0, kSrLineRole).toInt();
        hit.repoIndex = item->data(0, kSrRepoRole).toInt();
        activateGlobalSearchHit(hit);
        return;
    }
    const bool repoOpen =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size();
    if (!repoOpen || payload.isEmpty())
        return;
    showSection(0);
    const QString term = m_searchPageQuery;
    if (kind == SrFile) {
        const int line = item->data(0, kSrLineRole).toInt();
        openRepoFile(payload, [this, payload, line, term] {
            auto *edit = qobject_cast<QPlainTextEdit *>(m_openFileTabs.value(payload));
            if (!edit)
                return;
            QList<QTextEdit::ExtraSelection> sels =
                termSelections(edit->document(), term);
            QTextCursor lc(edit->document());
            lc.movePosition(QTextCursor::Start);
            if (line > 1)
                lc.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, line - 1);
            QTextEdit::ExtraSelection lineSel;
            lineSel.cursor = lc;
            lineSel.format.setBackground(Theme::nextOr(Theme::Role::AccentSubtle, QColor(31, 111, 235, 60)));
            lineSel.format.setProperty(QTextFormat::FullWidthSelection, true);
            sels.prepend(lineSel);
            edit->setExtraSelections(sels);
            edit->setTextCursor(lc);
            edit->centerCursor();
            edit->setFocus();
        });
    } else if (kind == SrCommit) {
        showOverviewCommits();
        showCommit(payload);
        QTimer::singleShot(0, this, [this, term] {
            if (!m_commitDiffView)
                return;
            QList<QTextEdit::ExtraSelection> sels =
                termSelections(m_commitDiffView->document(), term);
            m_commitDiffView->setExtraSelections(sels);
            if (!sels.isEmpty()) {
                m_commitDiffView->setTextCursor(sels.first().cursor);
                m_commitDiffView->ensureCursorVisible();
            }
        });
    }
}

bool MainWindow::isSearchResultLogRow(QTreeWidgetItem *item) const
{
    return item && item->data(0, kSrKindRole).toInt() == SrGlobal &&
           item->data(0, kSrHitKindRole).toString() == QLatin1String("log");
}

void MainWindow::deleteSearchResultLogRow(QTreeWidgetItem *item)
{
    if (!m_searchResultsTree || !isSearchResultLogRow(item))
        return;
    const QString line = item->data(0, kSrPathRole).toString();
    if (!deleteNetworkLogLine(line)) {
        flashMessage(QStringLiteral("That log entry is no longer in the log."), true);
        return;
    }
    QTreeWidgetItem *bucket = item->parent();
    delete item;
    if (bucket) {
        bucket->setText(0, bucket->data(0, kSrBaseRole).toString() +
                               QString::fromUtf8(" \xE2\x80\x94 %1")
                                   .arg(bucket->childCount()));
    }
    updateSearchStatus(); // recount, and fold the bucket away once it is empty
}

void MainWindow::updateSearchBucketDeleteButton(QTreeWidgetItem *bucket)
{
    if (!m_searchResultsTree || !bucket)
        return;
    // Only local, disposable records get a bulk delete. Agent sessions are left
    // out on purpose: deleting one also removes its unmerged pull request.
    const QString kind = bucket->data(0, kSrHitKindRole).toString();
    const bool logs = kind == QLatin1String("log");
    const bool wanted = (logs || kind == QLatin1String("action")) &&
                        bucket->childCount() > 0 &&
                        !bucket->data(0, kSrLoadingRole).toBool();
    const bool present = m_searchResultsTree->itemWidget(bucket, 1) != nullptr;
    if (!wanted) {
        if (present)
            m_searchResultsTree->removeItemWidget(bucket, 1); // deleteLater
        return;
    }
    if (present)
        return;
    auto *button = new QToolButton;
    button->setObjectName(QStringLiteral("searchBucketDeleteAll"));
    button->setText(QStringLiteral("Delete all"));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIcon(themedOcticon(QStringLiteral("trash"),
                                  QColor(QStringLiteral("#f85149")), 14));
    button->setIconSize(QSize(14, 14));
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(logs ? QStringLiteral("Delete every log entry in this group")
                            : QStringLiteral("Delete every finished workflow run in "
                                             "this group, with its logs"));
    connect(button, &QToolButton::clicked, this,
            [this, bucket] { deleteSearchResultBucket(bucket); });
    m_searchResultsTree->setItemWidget(bucket, 1, button);
    QHeaderView *header = m_searchResultsTree->header();
    const int width = button->sizeHint().width() + 8;
    if (header->sectionSize(1) < width)
        header->resizeSection(1, width);
}

void MainWindow::deleteSearchResultBucket(QTreeWidgetItem *bucket)
{
    if (!m_searchResultsTree || !bucket ||
        m_searchResultsTree->indexOfTopLevelItem(bucket) < 0)
        return;
    const QString kind = bucket->data(0, kSrHitKindRole).toString();
    const bool logs = kind == QLatin1String("log");
    if (!logs && kind != QLatin1String("action"))
        return;

    QList<QTreeWidgetItem *> rows;
    QStringList lines;
    QList<ActionRun> runs;
    for (int i = 0; i < bucket->childCount(); ++i) {
        QTreeWidgetItem *row = bucket->child(i);
        if (logs) {
            lines << row->data(0, kSrPathRole).toString();
            rows << row;
            continue;
        }
        const ActionRun *run = findRun(row->data(0, kSrNumberRole).toInt());
        if (run && (run->status == ActionStatus::Running ||
                    run->status == ActionStatus::Queued))
            continue; // still going; Stop it first
        if (run)
            runs << *run;
        rows << row; // a run that is already gone just leaves the list
    }
    const auto noun = [logs](qsizetype n) {
        if (logs)
            return n == 1 ? QStringLiteral("log entry") : QStringLiteral("log entries");
        return n == 1 ? QStringLiteral("workflow run") : QStringLiteral("workflow runs");
    };
    const qsizetype count = logs ? lines.size() : runs.size();
    if (count == 0) {
        flashMessage(logs ? QStringLiteral("No log entries to delete.")
                          : QStringLiteral("No finished runs to delete; running "
                                           "and queued runs are kept."));
        return;
    }

    const int generation = m_searchPageGeneration;
    if (QMessageBox::question(
            this, QStringLiteral("Delete all"),
            QString::fromUtf8("Delete %1 %2 matching \xE2\x80\x9C%3\xE2\x80\x9D%4? "
                              "This can't be undone.")
                .arg(count)
                .arg(noun(count), m_searchPageQuery,
                     logs ? QString() : QStringLiteral(", including their logs")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    // A new search while the question was up has already replaced these rows.
    if (generation != m_searchPageGeneration || !m_searchResultsTree)
        return;

    qsizetype removed = 0;
    if (logs) {
        removed = deleteNetworkLogLines(lines);
    } else {
        removeActionRuns(runs);
        removed = runs.size();
    }
    qDeleteAll(rows);
    bucket->setText(0, bucket->data(0, kSrBaseRole).toString() +
                           QString::fromUtf8(" \xE2\x80\x94 %1").arg(bucket->childCount()));
    updateSearchStatus(); // recount, and fold the bucket away once it is empty
    flashMessage(QStringLiteral("Deleted %1 %2.").arg(removed).arg(noun(removed)));
}

void MainWindow::stopSearch()
{
    ++m_searchPageGeneration; // discard the read-only record/log worker too
    for (QProcess *p : std::as_const(m_searchProcs)) {
        if (!p)
            continue;
        disconnect(p, nullptr, this, nullptr); // silence callbacks before killing
        p->kill();
        p->deleteLater();
    }
    m_searchProcs.clear();
    m_searchPending = 0;
    updateSearchStatus();
}

void MainWindow::animateSearchBucketSpinners()
{
    if (!m_searchResultsTree)
        return;
    m_searchSpinAngle = (m_searchSpinAngle + 30) % 360;
    for (int i = 0; i < m_searchResultsTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *bucket = m_searchResultsTree->topLevelItem(i);
        if (bucket && bucket->data(0, kSrLoadingRole).toBool())
            setSearchBucketIcon(bucket, true, m_searchSpinAngle);
    }
}

void MainWindow::updateSearchStatus()
{
    if (m_searchResultsStop)
        m_searchResultsStop->setVisible(m_searchPending > 0);
    const bool done = m_searchPending == 0;
    if (done) {
        if (m_searchSpinTimer)
            m_searchSpinTimer->stop();
    } else if (!m_searchSpinTimer) {
        m_searchSpinTimer = new QTimer(this);
        m_searchSpinTimer->setInterval(kAgentSpinTickMs);
        connect(m_searchSpinTimer, &QTimer::timeout, this,
                &MainWindow::animateSearchBucketSpinners);
        m_searchSpinTimer->start();
    } else if (!m_searchSpinTimer->isActive()) {
        m_searchSpinTimer->start();
    }
    int hits = 0;
    if (m_searchResultsTree)
        for (int i = 0; i < m_searchResultsTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *bucket = m_searchResultsTree->topLevelItem(i);
            hits += bucket->childCount();
            if (done && bucket->data(0, kSrLoadingRole).toBool()) {
                bucket->setData(0, kSrLoadingRole, false);
                setSearchBucketIcon(bucket, false, 0);
            }
            // Searching everything means a dozen buckets, most of them empty for
            // any one query. Keep them visible while their producer may still
            // report, then drop the zeros once nothing is left to arrive.
            if (done && bucket->childCount() == 0)
                bucket->setHidden(true);
            updateSearchBucketDeleteButton(bucket);
        }
    if (!m_searchResultsStatus)
        return;
    if (!done) {
        m_searchResultsStatus->setText(
            QString::fromUtf8(
                "Searching\xE2\x80\xA6  %1 match%2 so far  \xC2\xB7  %3 still running")
                .arg(hits)
                .arg(hits == 1 ? QString() : QStringLiteral("es"))
                .arg(m_searchPending));
        return;
    }
    QString text = QString::fromUtf8("Done \xE2\x80\x94 %1 match%2")
                       .arg(hits)
                       .arg(hits == 1 ? QString() : QStringLiteral("es"));
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        text += QString::fromUtf8(
            "  \xC2\xB7  open a repository to also search its issues, pull "
            "requests, discussions, files and history");
    m_searchResultsStatus->setText(text);
    if (!m_searchRunRecorded && m_searchRunGeneration == m_searchPageGeneration) {
        m_searchRunRecorded = true;
        finishSearchPageRun();
    }
}

SavedSearchStore &MainWindow::savedSearchStore()
{
    if (!m_savedSearchStore)
        m_savedSearchStore = std::make_unique<SavedSearchStore>();
    return *m_savedSearchStore;
}

QString MainWindow::searchScopeLabel(int repoIndex) const
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return {};
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    return QStringLiteral("%1/%2").arg(repo.owner, repo.name);
}

void MainWindow::finishSearchPageRun()
{
    if (!m_searchResultsTree || m_searchPageQuery.isEmpty())
        return;
    SavedSearchStore &store = savedSearchStore();
    const SavedSearch *saved = store.findQuery(m_searchPageQuery, m_searchRunScope);
    const QStringList excluded = (saved ? saved->excluded : QStringList()) +
                                 forkmesh::hiddenterms::activeTerms();
    QStringList keys;
    QMap<QString, int> counts;
    QHash<QString, QTreeWidgetItem *> rowsByKey;
    int excludedRows = 0;
    for (int i = 0; i < m_searchResultsTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *bucket = m_searchResultsTree->topLevelItem(i);
        const QString kind = bucket->data(0, kSrHitKindRole).toString();
        int hiddenInBucket = 0;
        for (int c = 0; c < bucket->childCount(); ++c) {
            QTreeWidgetItem *row = bucket->child(c);
            const bool hide = SavedSearchStore::excludes(
                excluded, row->data(0, kSrMatchTextRole).toString());
            row->setHidden(hide);
            if (hide) {
                ++hiddenInBucket;
                continue;
            }
            const QString key = row->data(0, kSrKeyRole).toString();
            if (key.isEmpty())
                continue;
            counts[kind] += 1;
            keys.append(key);
            rowsByKey.insert(key, row);
        }
        if (hiddenInBucket > 0 && bucket->data(0, kSrBaseRole).isValid()) {
            excludedRows += hiddenInBucket;
            bucket->setText(0, bucket->data(0, kSrBaseRole).toString() +
                                   QString::fromUtf8(" \xE2\x80\x94 %1")
                                       .arg(bucket->childCount() - hiddenInBucket));
        }
    }
    m_searchRunKeys = keys;
    m_searchRunKindCounts = counts;
    if (excludedRows > 0 && m_searchResultsStatus)
        m_searchResultsStatus->setText(
            m_searchResultsStatus->text() +
            QString::fromUtf8("  \xC2\xB7  %1 excluded").arg(excludedRows));

    if (!saved) {
        const QString declined =
            m_searchRunScope + QLatin1Char('\n') + m_searchPageQuery.toCaseFolded();
        if (!keys.isEmpty() && !m_saveSearchPromptDismissed.contains(declined))
            showSaveSearchPrompt();
        updateSaveSearchButton();
        return;
    }
    const SavedSearchRun run = store.recordRun(saved->id, keys, counts,
                                               QDateTime::currentMSecsSinceEpoch(),
                                               true);
    store.save();
    evaluateSecurityAlarms();
    for (const QString &key : run.newKeys) {
        QTreeWidgetItem *row = rowsByKey.value(key);
        if (!row)
            continue;
        QFont font = row->font(0);
        font.setBold(true);
        row->setFont(0, font);
        row->setText(0, QString::fromUtf8("New  \xC2\xB7  ") + row->text(0));
    }
    if (!run.newKeys.isEmpty() && m_searchResultsStatus)
        m_searchResultsStatus->setText(
            m_searchResultsStatus->text() +
            QString::fromUtf8("  \xC2\xB7  %1 new since this search last ran")
                .arg(run.newKeys.size()));
    refreshSavedSearchList();
    updateSaveSearchButton();
}

void MainWindow::showSaveSearchPrompt()
{
    if (!m_saveSearchPrompt || m_searchPageQuery.isEmpty())
        return;
    if (savedSearchStore().findQuery(m_searchPageQuery, m_searchRunScope)) {
        refreshSavedSearchList(); // already saved: just point at its row
        return;
    }
    const QString where = m_searchRunScope.isEmpty()
                              ? QStringLiteral("all repositories")
                              : m_searchRunScope;
    m_saveSearchPromptLabel->setText(
        QString::fromUtf8("Save this search? \xE2\x80\x9C%1\xE2\x80\x9D in %2 "
                          "stays in Saved searches so you can run it again.")
            .arg(m_searchPageQuery, where));
    m_saveSearchPrompt->show();
}

void MainWindow::saveCurrentSearch()
{
    if (m_searchPageQuery.isEmpty())
        return;
    const bool alerts = m_saveSearchAlertsCheck && m_saveSearchAlertsCheck->isChecked();
    const bool pertinent =
        m_saveSearchSecurityCheck && m_saveSearchSecurityCheck->isChecked();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    SavedSearchStore &store = savedSearchStore();
    const SavedSearch saved =
        store.add(m_searchPageQuery, m_searchRunScope, alerts, now);
    if (alerts)
        store.setAlerts(saved.id, true);
    if (pertinent)
        store.setSecurityPertinent(saved.id, true);
    // A finished run is the baseline right away; one still running records
    // itself when it completes.
    if (saved.runs == 0 && m_searchRunRecorded &&
        m_searchRunGeneration == m_searchPageGeneration)
        store.recordRun(saved.id, m_searchRunKeys, m_searchRunKindCounts, now, true);
    // The "search" kind keeps the confirmation out of the log.
    if (!store.save())
        flashMessage(QStringLiteral("Couldn't write saved searches to disk."), true);
    else
        flashMessage(pertinent ? QStringLiteral("Search saved as security pertinent. "
                                                "Any result sounds an alarm.")
                     : alerts  ? QStringLiteral("Search saved. You'll get a ping when "
                                                "new results show up.")
                               : QStringLiteral("Search saved."),
                     false, QString(), QStringLiteral("search"));
    if (m_saveSearchPrompt)
        m_saveSearchPrompt->hide();
    if (m_saveSearchAlertsCheck)
        m_saveSearchAlertsCheck->setChecked(false);
    if (m_saveSearchSecurityCheck)
        m_saveSearchSecurityCheck->setChecked(false);
    refreshSavedSearchList();
    updateSaveSearchButton();
    scheduleSavedSearchAlerts();
    evaluateSecurityAlarms();
}

void MainWindow::dismissSaveSearchPrompt()
{
    m_saveSearchPromptDismissed.insert(m_searchRunScope + QLatin1Char('\n') +
                                       m_searchPageQuery.toCaseFolded());
    if (m_saveSearchPrompt)
        m_saveSearchPrompt->hide();
}

void MainWindow::updateSaveSearchButton()
{
    if (m_searchResultsScrub)
        m_searchResultsScrub->setVisible(!m_searchPageQuery.isEmpty() &&
                                         scrubTargetRepoIndex(m_searchRunScope) >= 0);
    if (!m_searchResultsSave)
        return;
    const bool saved =
        !m_searchPageQuery.isEmpty() &&
        savedSearchStore().findQuery(m_searchPageQuery, m_searchRunScope);
    m_searchResultsSave->setVisible(!m_searchPageQuery.isEmpty());
    m_searchResultsSave->setText(saved ? QStringLiteral("Saved")
                                       : QStringLiteral("Save search"));
    m_searchResultsSave->setToolTip(
        saved ? QStringLiteral("This search is in Saved searches")
              : QStringLiteral("Keep this search, and optionally get a ping when "
                               "it finds new results"));
}

void MainWindow::refreshSavedSearchList()
{
    if (!m_savedSearchList)
        return;
    const SavedSearchStore &store = savedSearchStore();
    const SavedSearch *current =
        m_searchPageQuery.isEmpty()
            ? nullptr
            : store.findQuery(m_searchPageQuery, m_searchRunScope);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");

    QSignalBlocker block(m_savedSearchList);
    m_savedSearchList->clear();
    int pinging = 0;
    int pertinentCount = 0;
    for (const SavedSearch &search : store.searches()) {
        if (search.alerts)
            ++pinging;
        if (search.securityPertinent)
            ++pertinentCount;
        const QString id = search.id;
        auto *item = new QListWidgetItem(m_savedSearchList);
        item->setData(Qt::UserRole, id);
        item->setToolTip(search.query);

        auto *row = new QWidget;
        row->setObjectName("savedSearchRow");
        auto *rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(10, 8, 4, 8);
        rowLayout->setSpacing(2);
        // Labels let clicks through to the list, which runs the search; only
        // the bell and remove buttons keep their own clicks.
        const auto rowLabel = [rowLayout](const QString &text, const char *name) {
            auto *label = new QLabel(text);
            label->setObjectName(name);
            label->setTextFormat(Qt::PlainText);
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            return label;
        };

        auto *top = new QHBoxLayout;
        top->setSpacing(4);
        top->addWidget(rowLabel(search.query, "savedSearchQuery"), 1);
        if (search.newSinceViewed > 0) {
            auto *badge = new QLabel(QStringLiteral("%1 new").arg(search.newSinceViewed));
            badge->setObjectName("savedSearchNewBadge");
            badge->setAttribute(Qt::WA_TransparentForMouseEvents);
            top->addWidget(badge);
        }
        if (search.securityPertinent) {
            const bool alarming = SecurityAlarmState::alarming(search);
            auto *shield = new QLabel(alarming ? QStringLiteral("Alarm")
                                               : QStringLiteral("Security"));
            shield->setObjectName("savedSearchSecurityBadge");
            shield->setProperty("alarming", alarming);
            shield->setAttribute(Qt::WA_TransparentForMouseEvents);
            top->addWidget(shield);
        }
        auto *bell = new QToolButton;
        bell->setObjectName("savedSearchAlertToggle");
        bell->setCheckable(true);
        bell->setChecked(search.alerts);
        bell->setAutoRaise(true);
        bell->setCursor(Qt::PointingHandCursor);
        bell->setIcon(themedOcticon(QStringLiteral("bell"),
                                    Theme::remapColor(QColor(search.alerts ? "#e3b341" : "#6e7681")), 14));
        bell->setToolTip(search.alerts
                             ? QStringLiteral("Pings on: new results for this search "
                                              "ping you. Click to turn them off.")
                             : QStringLiteral("Get a ping when this search finds new "
                                              "results"));
        connect(bell, &QToolButton::clicked, this, [this, id](bool on) {
            // Rebuilding the list deletes this button; leave its signal first.
            QTimer::singleShot(0, this, [this, id, on] { setSavedSearchAlerts(id, on); });
        });
        auto *remove = new QToolButton;
        remove->setObjectName("savedSearchRemove");
        remove->setAutoRaise(true);
        remove->setCursor(Qt::PointingHandCursor);
        remove->setIcon(themedOcticon(QStringLiteral("x"), Theme::remapColor(QColor("#6e7681")), 14));
        remove->setToolTip(QStringLiteral("Remove this saved search"));
        connect(remove, &QToolButton::clicked, this, [this, id] {
            QTimer::singleShot(0, this, [this, id] { removeSavedSearch(id); });
        });
        top->addWidget(bell);
        top->addWidget(remove);
        rowLayout->addLayout(top);

        QString results;
        if (search.runs == 0) {
            results = QStringLiteral("Not run yet");
        } else {
            results = QStringLiteral("%1 result%2")
                          .arg(search.lastTotal)
                          .arg(search.lastTotal == 1 ? QString() : QStringLiteral("s"));
            const int delta = search.lastTotal - search.previousTotal;
            if (search.runs > 1 && delta != 0)
                results += QStringLiteral(" (%1%2 on the run before)")
                               .arg(delta > 0 ? QStringLiteral("+") : QString())
                               .arg(delta);
            if (search.lastNew > 0)
                results += dot + QStringLiteral("%1 new last run").arg(search.lastNew);
        }
        rowLayout->addWidget(rowLabel(results, "savedSearchStats"));

        // Where the results are: the biggest kinds first.
        QVector<QPair<int, QString>> kinds;
        for (auto it = search.kindCounts.cbegin(); it != search.kindCounts.cend(); ++it)
            if (it.value() > 0)
                kinds.append({it.value(), it.key()});
        std::sort(kinds.begin(), kinds.end(), [](const auto &a, const auto &b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        QStringList parts;
        for (int i = 0; i < kinds.size() && i < 3; ++i)
            parts << QStringLiteral("%1 %2").arg(kinds.at(i).first).arg(
                savedSearchKindTitle(kinds.at(i).second));
        if (kinds.size() > 3)
            parts << QStringLiteral("+%1 more").arg(kinds.size() - 3);
        if (!parts.isEmpty())
            rowLayout->addWidget(rowLabel(parts.join(dot), "savedSearchStats"));

        QStringList meta{search.scope.isEmpty() ? QStringLiteral("All repositories")
                                                : search.scope};
        meta << (search.lastRunMs > 0
                     ? QStringLiteral("ran ") + formatIssueRelativeTime(search.lastRunMs)
                     : QStringLiteral("saved ") + formatIssueRelativeTime(search.createdMs));
        if (search.runs > 0)
            meta << QStringLiteral("%1 run%2").arg(search.runs).arg(
                search.runs == 1 ? QString() : QStringLiteral("s"));
        rowLayout->addWidget(rowLabel(meta.join(dot), "savedSearchStats"));
        if (!search.excluded.isEmpty())
            rowLayout->addWidget(rowLabel(
                QStringLiteral("Excluding ") + search.excluded.join(QStringLiteral(", ")),
                "savedSearchExcluded"));

        auto *security = new QCheckBox(QStringLiteral("Security pertinent"));
        security->setObjectName("savedSearchSecurityCheck");
        security->setChecked(search.securityPertinent);
        security->setCursor(Qt::PointingHandCursor);
        security->setToolTip(QStringLiteral(
            "Sound a continual alarm whenever this search has any results, until "
            "you acknowledge them"));
        connect(security, &QCheckBox::toggled, this, [this, id](bool on) {
            QTimer::singleShot(0, this,
                               [this, id, on] { setSavedSearchSecurityPertinent(id, on); });
        });
        auto *exclude = new QToolButton;
        exclude->setObjectName("savedSearchExcludeButton");
        exclude->setAutoRaise(true);
        exclude->setCursor(Qt::PointingHandCursor);
        exclude->setText(search.excluded.isEmpty() ? QStringLiteral("Exclude\u2026")
                                                   : QStringLiteral("Edit exclusions\u2026"));
        exclude->setToolTip(QStringLiteral(
            "Leave out results whose repository name, title, path or text contains "
            "any of these words. Excluded results never count, ping or alarm."));
        connect(exclude, &QToolButton::clicked, this, [this, id] {
            QTimer::singleShot(0, this, [this, id] { editSavedSearchExclusions(id); });
        });
        auto *scrub = new QToolButton;
        scrub->setObjectName("savedSearchScrubButton");
        scrub->setAutoRaise(true);
        scrub->setCursor(Qt::PointingHandCursor);
        scrub->setText(QStringLiteral("Scrub\u2026"));
        const bool scrubbable = scrubTargetRepoIndex(search.scope) >= 0;
        scrub->setEnabled(scrubbable);
        scrub->setToolTip(
            scrubbable
                ? QStringLiteral("Replace this term in the repository's source and rewrite "
                                 "it out of every branch's history")
                : QStringLiteral("Open a repository with a local checkout to scrub this "
                                 "term from it"));
        connect(scrub, &QToolButton::clicked, this, [this, id] {
            QTimer::singleShot(0, this, [this, id] {
                if (const SavedSearch *saved = savedSearchStore().find(id))
                    scrubSearchTerm(saved->query, saved->scope);
            });
        });
        auto *options = new QHBoxLayout;
        options->setSpacing(4);
        options->addWidget(security);
        options->addStretch(1);
        options->addWidget(scrub);
        options->addWidget(exclude);
        rowLayout->addLayout(options);

        item->setSizeHint(QSize(0, row->sizeHint().height()));
        m_savedSearchList->setItemWidget(item, row);
        if (current && current->id == id)
            m_savedSearchList->setCurrentItem(item);
    }
    if (m_searchHiddenCaption) {
        const QStringList hidden = forkmesh::hiddenterms::terms();
        const qsizetype values = forkmesh::hiddenterms::variableValueCount();
        QStringList parts = hidden;
        if (values > 0)
            parts.append(values == 1 ? QStringLiteral("1 Actions variable value")
                                     : QStringLiteral("%1 Actions variable values").arg(values));
        m_searchHiddenCaption->setText(
            parts.isEmpty() ? QStringLiteral("Nothing hidden from search")
                            : QStringLiteral("Hidden from search and logs: ") +
                                  parts.join(QStringLiteral(", ")));
    }
    if (m_searchHideVariablesCheck) {
        const QSignalBlocker blocker(m_searchHideVariablesCheck);
        m_searchHideVariablesCheck->setChecked(forkmesh::hiddenterms::hidesVariables());
    }
    updateSavedSearchRefreshButton();
    const int count = int(store.searches().size());
    if (m_savedSearchCaption) {
        m_savedSearchCaption->setText(
            QStringLiteral("%1 saved").arg(count) + dot +
            QStringLiteral("%1 with pings on").arg(pinging) +
            (pertinentCount > 0
                 ? dot + QStringLiteral("%1 security pertinent").arg(pertinentCount)
                 : QString()));
        m_savedSearchCaption->setVisible(count > 0);
    }
    m_savedSearchList->setVisible(count > 0);
    if (m_savedSearchEmpty)
        m_savedSearchEmpty->setVisible(count == 0);
}

void MainWindow::openSavedSearch(const QString &id)
{
    const SavedSearch *search = savedSearchStore().find(id);
    if (!search) {
        flashMessage(QStringLiteral("That saved search was removed."), true);
        return;
    }
    const QString query = search->query;
    const QString scope = search->scope;
    if (!scope.isEmpty()) {
        const int slash = scope.indexOf(QLatin1Char('/'));
        const int index = repoIndexFor(scope.left(slash), scope.mid(slash + 1));
        if (index < 0) {
            flashMessage(QStringLiteral("That saved search's repository isn't on "
                                        "this node anymore."),
                         true);
            return;
        }
        if (index != m_repoDetailIndex)
            openRepoDetail(index);
    }
    openSearchResultsPage(query);
}

void MainWindow::editSearchHiddenTerms()
{
    bool ok = false;
    const QString text = QInputDialog::getText(
        this, QStringLiteral("Hidden from search and logs"),
        QStringLiteral("Words, repository names or paths that never show up in any "
                       "search and are replaced with [hidden] in the log "
                       "(comma-separated, not case-sensitive):"),
        QLineEdit::Normal, forkmesh::hiddenterms::terms().join(QStringLiteral(", ")),
        &ok);
    if (!ok)
        return;
    setSearchHiddenTerms(SavedSearchStore::parseExcluded(text));
}

void MainWindow::setSearchHidesVariables(bool hide)
{
    QSettings().setValue(forkmesh::hiddenterms::hideVariablesKey(), hide);
    forkmesh::hiddenterms::refreshVariableValues(ActionStore::variables());
    const qsizetype values = forkmesh::hiddenterms::variableValueCount();
    flashMessage(hide ? QStringLiteral("Hiding %1 Actions variable value%2 from search and logs.")
                            .arg(values)
                            .arg(values == 1 ? QString() : QStringLiteral("s"))
                      : QStringLiteral("Actions variable values are no longer hidden."),
                 false, QString(), QStringLiteral("search"));
    refreshSavedSearchList();
    if (!m_searchPageQuery.isEmpty())
        openSearchResultsPage(m_searchPageQuery);
    else if (savedSearchStore().hasAlerts() || savedSearchStore().hasSecurityPertinent())
        checkSavedSearchAlerts();
}

void MainWindow::setSearchHiddenTerms(const QStringList &terms)
{
    const QStringList clean = SavedSearchStore::parseExcluded(terms.join(QLatin1Char('\n')));
    if (!SavedSearchStore::saveHiddenTerms(SavedSearchStore::hiddenTermsPath(), clean)) {
        flashMessage(QStringLiteral("Couldn't write the hidden search list to disk."), true);
        return;
    }
    forkmesh::hiddenterms::setTerms(clean);
    flashMessage(clean.isEmpty()
                     ? QStringLiteral("Nothing is hidden from search now.")
                     : QStringLiteral("Hidden from search and logs: %1 term%2.")
                           .arg(clean.size())
                           .arg(clean.size() == 1 ? QString() : QStringLiteral("s")),
                 false, QString(), QStringLiteral("search"));
    refreshSavedSearchList();
    if (!m_searchPageQuery.isEmpty())
        openSearchResultsPage(m_searchPageQuery);
    else if (savedSearchStore().hasAlerts() || savedSearchStore().hasSecurityPertinent())
        checkSavedSearchAlerts();
}

void MainWindow::refreshAllSavedSearches()
{
    if (savedSearchStore().searches().isEmpty())
        return;
    m_savedSearchRefreshAll = true;
    for (const SavedSearch &search : savedSearchStore().searches())
        if (!m_savedSearchCheckQueue.contains(search.id))
            m_savedSearchCheckQueue.append(search.id);
    updateSavedSearchRefreshButton();
    if (!m_savedSearchCheckRunning)
        runNextSavedSearchAlertCheck();
}

void MainWindow::updateSavedSearchRefreshButton()
{
    if (!m_savedSearchRefreshButton)
        return;
    const bool busy = m_savedSearchRefreshAll &&
                      (m_savedSearchCheckRunning || !m_savedSearchCheckQueue.isEmpty());
    m_savedSearchRefreshButton->setEnabled(!busy &&
                                           !savedSearchStore().searches().isEmpty());
    m_savedSearchRefreshButton->setText(busy ? QStringLiteral("Refreshing\u2026")
                                             : QStringLiteral("Refresh"));
}

void MainWindow::editSavedSearchExclusions(const QString &id)
{
    const SavedSearch *search = savedSearchStore().find(id);
    if (!search)
        return;
    bool ok = false;
    const QString text = QInputDialog::getText(
        this, QStringLiteral("Exclude from saved search"),
        QString::fromUtf8("Leave out results of \xE2\x80\x9C%1\xE2\x80\x9D whose "
                          "repository name, title, path or text contains any of "
                          "these (comma-separated, not case-sensitive):")
            .arg(search->query),
        QLineEdit::Normal, search->excluded.join(QStringLiteral(", ")), &ok);
    if (!ok)
        return;
    setSavedSearchExclusions(id, SavedSearchStore::parseExcluded(text));
}

void MainWindow::setSavedSearchExclusions(const QString &id, const QStringList &excluded)
{
    SavedSearchStore &store = savedSearchStore();
    if (!store.setExcluded(id, excluded))
        return;
    const SavedSearch *search = store.find(id);
    if (!store.save()) {
        flashMessage(QStringLiteral("Couldn't write saved searches to disk."), true);
    } else {
        flashMessage(search->excluded.isEmpty()
                         ? QStringLiteral("That saved search no longer excludes anything.")
                         : QStringLiteral("Saved search now excludes: %1")
                               .arg(search->excluded.join(QStringLiteral(", "))),
                     false, QString(), QStringLiteral("search"));
    }
    const bool shown = !m_searchPageQuery.isEmpty() &&
                       store.findQuery(m_searchPageQuery, m_searchRunScope) == search;
    const bool checked = search->alerts || search->securityPertinent;
    refreshSavedSearchList();
    if (shown)
        openSavedSearch(id);
    else if (checked)
        checkSavedSearchAlerts();
}

void MainWindow::setSavedSearchAlerts(const QString &id, bool alerts)
{
    SavedSearchStore &store = savedSearchStore();
    if (store.setAlerts(id, alerts)) {
        store.save();
        flashMessage(alerts ? QStringLiteral("Pings on: new results for that saved "
                                             "search will ping you.")
                            : QStringLiteral("Pings off for that saved search."),
                     false, QString(), QStringLiteral("search"));
    }
    refreshSavedSearchList();
    scheduleSavedSearchAlerts();
}

void MainWindow::removeSavedSearch(const QString &id)
{
    SavedSearchStore &store = savedSearchStore();
    if (!store.remove(id))
        return;
    store.save();
    refreshSavedSearchList();
    updateSaveSearchButton();
    scheduleSavedSearchAlerts();
    evaluateSecurityAlarms();
}

void MainWindow::scheduleSavedSearchAlerts()
{
    constexpr int kSavedSearchAlertIntervalMs = 15 * 60 * 1000;
    const SavedSearchStore &store = savedSearchStore();
    if (!m_securityAlarmStartupCheckQueued) {
        m_securityAlarmStartupCheckQueued = true;
        if (store.hasSecurityPertinent())
            QTimer::singleShot(60 * 1000, this, [this] { checkSavedSearchAlerts(); });
    }
    if (!store.hasAlerts() && !store.hasSecurityPertinent()) {
        if (m_savedSearchAlertTimer)
            m_savedSearchAlertTimer->stop();
        return;
    }
    if (!m_savedSearchAlertTimer) {
        m_savedSearchAlertTimer = new QTimer(this);
        m_savedSearchAlertTimer->setInterval(kSavedSearchAlertIntervalMs);
        connect(m_savedSearchAlertTimer, &QTimer::timeout, this,
                &MainWindow::checkSavedSearchAlerts);
    }
    if (!m_savedSearchAlertTimer->isActive())
        m_savedSearchAlertTimer->start();
}

void MainWindow::checkSavedSearchAlerts()
{
    if (m_savedSearchCheckRunning || !m_savedSearchCheckQueue.isEmpty())
        return;
    for (const SavedSearch &search : savedSearchStore().searches())
        if (search.alerts || search.securityPertinent || m_savedSearchRefreshAll)
            m_savedSearchCheckQueue.append(search.id);
    runNextSavedSearchAlertCheck();
}

void MainWindow::runNextSavedSearchAlertCheck()
{
    while (!m_savedSearchCheckQueue.isEmpty()) {
        const QString id = m_savedSearchCheckQueue.takeFirst();
        const SavedSearch *search = savedSearchStore().find(id);
        if (!search ||
            !(search->alerts || search->securityPertinent || m_savedSearchRefreshAll))
            continue;
        int repoIndex = -1;
        if (!search->scope.isEmpty()) {
            const int slash = search->scope.indexOf(QLatin1Char('/'));
            repoIndex = repoIndexFor(search->scope.left(slash),
                                     search->scope.mid(slash + 1));
            if (repoIndex < 0)
                continue; // the repository left this node; nothing to search
        }
        const std::shared_ptr<const SearchPageInputs> inputs =
            searchPageInputs(repoIndex, search->query);
        m_savedSearchCheckRunning = true;
        runOffThread<QVector<GlobalSearchHit>>(
            [inputs] {
                {
                    QStringList unused;
                    const QMutexLocker locker(&inputs->networkLog->mutex);
                    unused.swap(inputs->networkLog->lines);
                }
                QVector<GlobalSearchHit> hits;
                for (const QString &kind :
                     {QStringLiteral("repo"), QStringLiteral("issue"),
                      QStringLiteral("milestone"), QStringLiteral("pull"),
                      QStringLiteral("discussion"), QStringLiteral("project"),
                      QStringLiteral("agent"), QStringLiteral("action"),
                      QStringLiteral("git-catalog"), QStringLiteral("worktree-code"),
                      QStringLiteral("project-file")})
                    hits += searchPageKindHits(*inputs, kind);
                hits += scanSavedSearchGitHits(inputs->dir, inputs->ref,
                                               inputs->query, kMaxSearchPageRecords);
                return hits;
            },
            [this, id](QVector<GlobalSearchHit> hits) {
                m_savedSearchCheckRunning = false;
                QStringList keys;
                QMap<QString, int> counts;
                QHash<QString, QString> kindByKey;
                const SavedSearch *running = savedSearchStore().find(id);
                const QStringList excluded =
                    (running ? running->excluded : QStringList()) +
                    forkmesh::hiddenterms::activeTerms();
                const QString runScope = running ? running->scope : QString();
                for (const GlobalSearchHit &hit : std::as_const(hits)) {
                    const QString key = savedSearchHitKey(hit);
                    if (key.isEmpty())
                        continue;
                    if (SavedSearchStore::excludes(
                            excluded,
                            savedSearchHitText(hit, hit.repoIndex >= 0
                                                        ? searchScopeLabel(hit.repoIndex)
                                                        : runScope)))
                        continue;
                    counts[hit.kind] += 1;
                    keys.append(key);
                    kindByKey.insert(key, hit.kind);
                }
                SavedSearchStore &store = savedSearchStore();
                const SavedSearch *search = store.find(id);
                if (search && (search->alerts || search->securityPertinent ||
                               m_savedSearchRefreshAll)) {
                    const QString query = search->query;
                    const QString scope = search->scope;
                    const bool alerts = search->alerts;
                    const bool pertinent = search->securityPertinent;
                    const SavedSearchRun run = store.recordRun(
                        id, keys, counts, QDateTime::currentMSecsSinceEpoch(), false);
                    store.save();
                    evaluateSecurityAlarms();
                    if (alerts && !run.baseline && !run.newKeys.isEmpty()) {
                        QMap<QString, int> newByKind;
                        for (const QString &key : run.newKeys)
                            newByKind[kindByKey.value(key)] += 1;
                        QStringList parts;
                        for (auto it = newByKind.cbegin(); it != newByKind.cend(); ++it)
                            parts << QStringLiteral("%1 %2").arg(it.value()).arg(
                                savedSearchKindTitle(it.key()));
                        const int count = int(run.newKeys.size());
                        NotificationLink link;
                        link.kind = QStringLiteral("search");
                        if (!scope.isEmpty()) {
                            const int slash = scope.indexOf(QLatin1Char('/'));
                            link.owner = scope.left(slash);
                            link.name = scope.mid(slash + 1);
                        }
                        link.ref = id;
                        const QString title =
                            pertinent
                                ? QStringLiteral("A security-pertinent saved search "
                                                 "found %1 new result%2")
                                      .arg(count)
                                      .arg(count == 1 ? QString() : QStringLiteral("s"))
                                : QString::fromUtf8("Saved search \xE2\x80\x9C%1\xE2\x80\x9D "
                                                    "found %2 new result%3")
                                      .arg(query)
                                      .arg(count)
                                      .arg(count == 1 ? QString() : QStringLiteral("s"));
                        addNotification(
                            forkmesh::hiddenterms::redact(title),
                            forkmesh::hiddenterms::redact(
                                QStringLiteral("New in %1: %2")
                                    .arg(scope.isEmpty() ? QStringLiteral("all repositories")
                                                         : scope,
                                         parts.join(QStringLiteral(", ")))),
                            false, link);
                    }
                    refreshSavedSearchList();
                }
                runNextSavedSearchAlertCheck();
            });
        return;
    }
    if (m_savedSearchRefreshAll) {
        m_savedSearchRefreshAll = false;
        refreshSavedSearchList();
        flashMessage(QStringLiteral("Saved searches refreshed."), false, QString(),
                     QStringLiteral("search"));
    }
    updateSavedSearchRefreshButton();
}


void MainWindow::setCommitWorkspacePage(int page)
{
    if (!m_commitsStack)
        return;
    if (page == kCommitWorkspaceCommitPage ||
        page == kCommitWorkspaceRangePage)
        showOverviewCommits();
    // Leaving the commit page abandons any commit still being fetched for
    // it: showCommit() reads the metadata and the patch with detached git,
    // and a render landing after Back/Forward (or a list reload) flipped the
    // page back to the commit and grew the navigation trail by a phantom
    // entry — a race the window suite hits whenever the box is busy.
    if (page != kCommitWorkspaceCommitPage &&
        m_commitsStack->currentIndex() == kCommitWorkspaceCommitPage) {
        ++m_commitLoadGen;
        stopCommitDiffSpin();
    }
    m_commitsStack->setCurrentIndex(page);
    if (m_gitFilesSlot)
        m_gitFilesSlot->setCurrentIndex(0);
    if (m_gitHistorySlot)
        m_gitHistorySlot->setCurrentIndex(0);
    updateCommitsCompareIndicator();
    updateRepoActivityRail();
    // Leaving or entering the range page changes whether an agent's diff is on
    // screen, and with it who the prompt bar's "add" is talking to.
    updateQuickAddEnterTarget();
    scheduleNavRecord();
}

void MainWindow::updateCommitsCompareIndicator()
{
    if (!m_commitsCompareBaseButton || !m_commitsCompareArrow)
        return;
    const bool comparing =
        m_commitsStack &&
        m_commitsStack->currentIndex() == kCommitWorkspaceRangePage &&
        !m_branchDiffBranch.isEmpty();
    if (!comparing) {
        if (m_commitsBranchButton) {
            const QString browsed =
                m_repoBranch.isEmpty() ? repoHeadBranch() : m_repoBranch;
            const QString headLabel = browsed.isEmpty()
                                          ? QStringLiteral("(detached)")
                                          : browsed;
            if (auto *elider =
                    dynamic_cast<ElidingPushButton *>(m_commitsBranchButton))
                elider->setFullText(headLabel);
            else
                m_commitsBranchButton->setText(headLabel);
        }
        m_commitsCompareArrow->hide();
        m_commitsCompareBaseButton->hide();
        return;
    }
    const QString base = branchCompareBase();
    const QString label = base.isEmpty() ? QStringLiteral("(no base)") : base;
    if (m_commitsBranchButton) {
        if (auto *elider =
                dynamic_cast<ElidingPushButton *>(m_commitsBranchButton))
            elider->setFullText(m_branchDiffBranch);
        else
            m_commitsBranchButton->setText(m_branchDiffBranch);
    }
    if (auto *elider =
            dynamic_cast<ElidingPushButton *>(m_commitsCompareBaseButton))
        elider->setFullText(label);
    else
        m_commitsCompareBaseButton->setText(label);
    m_commitsCompareBaseButton->setToolTip(
        QString::fromUtf8("Comparing %1 against %2 \xE2\x80\x94 the diff on the "
                          "right is everything %1 adds over %2. Click to compare "
                          "against a different branch.")
            .arg(m_branchDiffBranch, label));

    auto *menu = new QMenu(m_commitsCompareBaseButton);
    const QStringList branches = repoBranches();
    for (const QString &b : branches) {
        if (b == m_branchDiffBranch)
            continue; // a branch can't be compared against itself
        QAction *a = menu->addAction(b);
        a->setCheckable(true);
        a->setChecked(b == base);
        connect(a, &QAction::triggered, this,
                [this, b] { setBranchCompareBase(b); });
    }
    if (menu->isEmpty())
        menu->addAction(QStringLiteral("No other branches"))->setEnabled(false);
    QMenu *old = m_commitsCompareBaseButton->menu();
    m_commitsCompareBaseButton->setMenu(menu);
    if (old)
        old->deleteLater();

    m_commitsCompareArrow->show();
    m_commitsCompareBaseButton->show();
}

void MainWindow::showCommitList()
{
    if (!m_commitsStack)
        return;
    if (m_commitsStack->currentIndex() == kCommitWorkspaceRangePage)
        return;
    setCommitWorkspacePage(kCommitWorkspaceChangesPage);
}

void MainWindow::openMostRecentCommit()
{
    if (!m_commitsTable || m_commitsTable->rowCount() == 0) {
        showCommitList();
        return;
    }
    QTableWidgetItem *item = m_commitsTable->item(0, kCommitSummaryCol);
    if (!item) {
        showCommitList();
        return;
    }
    {
        QSignalBlocker block(m_commitsTable);
        m_commitsTable->setCurrentCell(0, kCommitSummaryCol);
    }
    showCommit(item->data(Qt::UserRole).toString());
}

namespace {

bool isInboxDataPath(const QString &rel)
{
    return rel.startsWith(QLatin1String(".forkmesh/issues/"))
        || rel.startsWith(QLatin1String(".forkmesh/pulls/"))
        || rel.startsWith(QLatin1String(".forkmesh/commits/"))
        || rel.startsWith(QLatin1String(".forkmesh/"));
}

QString rebaseMergeDir(const QString &dir)
{
    QByteArray out;
    if (!runGitCapture(dir, {"rev-parse", "--git-path", "rebase-merge"}, &out,
                       nullptr))
        return QString();
    QString path = QString::fromUtf8(out).trimmed();
    if (path.isEmpty())
        return QString();
    if (!QDir::isAbsolutePath(path))
        path = dir + "/" + path;
    return QFileInfo::exists(path) ? path : QString();
}

QStringList rebaseUnmergedFiles(const QString &dir)
{
    const QByteArray out =
        gitCaptureStdout(dir, {"diff", "--name-only", "--diff-filter=U"});
    return QString::fromUtf8(out).split('\n', Qt::SkipEmptyParts);
}

bool driveDeleteCommitRebase(const QString &dir, QString *err)
{
    for (int step = 0; step < 100000; ++step) {
        if (rebaseMergeDir(dir).isEmpty())
            return true; // rebase finished cleanly

        const QStringList unmerged = rebaseUnmergedFiles(dir);
        QStringList blockers;
        for (const QString &f : unmerged)
            if (!isInboxDataPath(f))
                blockers << f;
        if (!blockers.isEmpty()) {
            if (err)
                *err = QStringLiteral(
                           "Can't remove this commit automatically — replaying a "
                           "later commit conflicts in %1. Resolve those files by "
                           "hand, or revert the commit instead. History was left "
                           "unchanged.")
                           .arg(blockers.join(QStringLiteral(", ")));
            return false;
        }

        for (const QString &f : unmerged) {
            if (runGitCapture(dir, {"checkout", "--theirs", "--", f}, nullptr,
                              nullptr))
                runGitCapture(dir, {"add", "--", f}, nullptr, nullptr);
            else
                runGitCapture(dir, {"rm", "--force", "--", f}, nullptr, nullptr);
        }

        const bool nothingToCommit =
            runGitCapture(dir, {"diff", "--cached", "--quiet"}, nullptr, nullptr);
        const QStringList advance =
            nothingToCommit
                ? QStringList{"rebase", "--skip"}
                : QStringList{"-c", "core.editor=true", "rebase", "--continue"};
        runGitCapture(dir, advance, nullptr, nullptr);
    }

    if (err)
        *err = QStringLiteral(
            "Gave up removing the commit after too many conflict steps; history "
            "was left unchanged.");
    return false;
}

} // namespace

void MainWindow::deleteCommit(const QString &hash)
{
    if (hash.isEmpty() || !repoHasWorkingTree() || m_commitRewriteRunning)
        return;
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return;
    const QString branch = currentRef();
    m_commitRewriteRunning = true;
    runOffThread<QString>(
        [dir, hash] {
            QByteArray subjOut;
            runGitCapture(dir, {"log", "-1", "--format=%h %s", hash}, &subjOut, nullptr);
            return QString::fromUtf8(subjOut).trimmed();
        },
        [this, dir, hash, branch](QString label) {
            m_commitRewriteRunning = false;
            if (dir != repoGitDir())
                return;
            if (QMessageBox::warning(
                    this, "Delete commit",
                    QStringLiteral(
                        "Remove commit \"%1\" from history?\n\n"
                        "This rewrites %2 and replays every later commit onto the one "
                        "before it. It can't be undone, and you'll need to publish again "
                        "to update the network mirror.")
                        .arg(label.isEmpty() ? hash.left(8) : label, branch),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes)
                return;
            if (m_commitRewriteRunning)
                return;
            m_commitRewriteRunning = true;
            runOffThread<std::pair<bool, QString>>(
                [dir, hash, branch] { return removeCommitFromHistory(dir, hash, branch); },
                [this, dir, hash, branch](std::pair<bool, QString> removed) {
                    m_commitRewriteRunning = false;
                    const bool current = dir == repoGitDir();
                    if (!removed.first) {
                        if (current)
                            setRepoDetailNotice(removed.second, true);
                        else
                            flashMessage(removed.second, true);
                        return;
                    }
                    logSystem(QStringLiteral("Git: removed commit %1 from %2.")
                                  .arg(hash.left(8), branch));
                    if (!current)
                        return;
                    setRepoDetailNotice(
                        QStringLiteral("Removed commit %1 — gone from history.")
                            .arg(hash.left(8)));
                    loadCommits();
                    refreshRepoSyncIndicators();
                });
        });
}

std::pair<bool, QString> MainWindow::removeCommitFromHistory(const QString &dir,
                                                            const QString &hash,
                                                            const QString &branch)
{
    QByteArray parentsOut;
    if (!runGitCapture(dir, {"rev-list", "--parents", "-n", "1", hash},
                       &parentsOut, nullptr))
        return {false, QStringLiteral("Could not inspect that commit.")};
    const QStringList fields = QString::fromUtf8(parentsOut).trimmed().split(
        QLatin1Char(' '), Qt::SkipEmptyParts);
    if (fields.size() < 2)
        return {false, QStringLiteral("Can't delete the first commit — it has no "
                                      "parent to replay onto.")};
    if (fields.size() > 2)
        return {false, QStringLiteral("Can't drop a merge commit from history this "
                                      "way (it has two parents).")};

    QString err;
    if (!runGitCapture(dir, {"rebase", "--onto", hash + "^", hash, branch},
                       nullptr, &err)) {
        if (rebaseMergeDir(dir).isEmpty())
            return {false,
                    err.trimmed().isEmpty()
                        ? QStringLiteral("Could not remove the commit — the rebase "
                                         "failed. Make sure the working tree is "
                                         "clean and try again.")
                        : err.trimmed()};
        QString driveErr;
        if (!driveDeleteCommitRebase(dir, &driveErr)) {
            runGitCapture(dir, {"rebase", "--abort"}, nullptr, nullptr);
            return {false, driveErr};
        }
    }

    return {true, QString()};
}

void MainWindow::revertCommit(const QString &hash)
{
    if (hash.isEmpty() || !repoHasWorkingTree() || m_commitRewriteRunning)
        return;
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return;
    const QString branch = currentRef();
    m_commitRewriteRunning = true;
    runOffThread<QString>(
        [dir, hash] {
            QByteArray subjOut;
            runGitCapture(dir, {"log", "-1", "--format=%h %s", hash}, &subjOut, nullptr);
            return QString::fromUtf8(subjOut).trimmed();
        },
        [this, dir, hash, branch](QString label) {
            m_commitRewriteRunning = false;
            if (dir != repoGitDir())
                return;
            if (QMessageBox::question(
                    this, "Restore commit",
                    QStringLiteral(
                        "Undo commit \"%1\"?\n\n"
                        "This adds a new commit to %2 that reverses its changes. "
                        "History is kept, so the original commit stays in the log — "
                        "you'll need to publish again to update the network mirror.")
                        .arg(label.isEmpty() ? hash.left(8) : label, branch),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes)
                return;
            if (m_commitRewriteRunning)
                return;
            m_commitRewriteRunning = true;
            runOffThread<std::pair<bool, QString>>(
                [dir, hash] { return revertCommitInHistory(dir, hash); },
                [this, dir, hash, branch](std::pair<bool, QString> reverted) {
                    m_commitRewriteRunning = false;
                    const bool current = dir == repoGitDir();
                    if (!reverted.first) {
                        if (current)
                            setRepoDetailNotice(reverted.second, true);
                        else
                            flashMessage(reverted.second, true);
                        return;
                    }
                    logSystem(QStringLiteral("Git: reverted commit %1 on %2 with a "
                                             "new commit.")
                                  .arg(hash.left(8), branch));
                    if (!current)
                        return;
                    setRepoDetailNotice(
                        QStringLiteral("Reverted commit %1 — added a commit that "
                                       "undoes it.")
                            .arg(hash.left(8)));
                    loadCommits();
                    refreshRepoSyncIndicators();
                });
        });
}

std::pair<bool, QString> MainWindow::revertCommitInHistory(const QString &dir,
                                                          const QString &hash)
{
    QByteArray parentsOut;
    if (!runGitCapture(dir, {"rev-list", "--parents", "-n", "1", hash},
                       &parentsOut, nullptr))
        return {false, QStringLiteral("Could not inspect that commit.")};
    const QStringList fields = QString::fromUtf8(parentsOut).trimmed().split(
        QLatin1Char(' '), Qt::SkipEmptyParts);
    if (fields.size() > 2)
        return {false, QStringLiteral("Can't revert a merge commit this way (it has "
                                      "two parents).")};

    QString err;
    if (runGitCapture(dir, {"revert", "--no-edit", hash}, nullptr, &err))
        return {true, QString()};
    if (QFileInfo::exists(dir + QStringLiteral("/.git/REVERT_HEAD")) ||
        QFileInfo::exists(dir + QStringLiteral("/.git/sequencer"))) {
        runGitCapture(dir, {"revert", "--abort"}, nullptr, nullptr);
        return {false, QStringLiteral("Couldn't revert cleanly — a later commit "
                                      "changed the same lines. History was left "
                                      "unchanged.")};
    }
    return {false, err.trimmed().isEmpty()
                       ? QStringLiteral("Could not revert the commit. Make sure the "
                                        "working tree is clean and try again.")
                       : err.trimmed()};
}

void MainWindow::showCommitsBanner(const QString &html)
{
    if (!m_commitsUnsyncedBanner)
        return;
    if (m_commitsBannerFade)
        m_commitsBannerFade->stop(); // cancel any in-flight fade-out
    if (m_commitsBannerOpacity)
        m_commitsBannerOpacity->setOpacity(1.0);
    m_commitsUnsyncedBanner->setText(html);
    m_commitsUnsyncedBanner->show();
}

void MainWindow::hideCommitsBanner()
{
    if (m_commitsUnsyncedFiles)
        m_commitsUnsyncedFiles->hide();
    if (!m_commitsUnsyncedBanner || m_commitsUnsyncedBanner->isHidden())
        return;
    if (!m_commitsBannerFade || !m_commitsBannerOpacity) {
        m_commitsUnsyncedBanner->hide();
        return;
    }
    m_commitsBannerFade->stop();
    m_commitsBannerFade->setStartValue(m_commitsBannerOpacity->opacity());
    m_commitsBannerFade->setEndValue(0.0);
    m_commitsBannerFade->start();
}

void MainWindow::filterCommits(const QString &query)
{
    if (!m_commitsTable)
        return;
    collapseAllCommitFileRows();
    const QString needle = query.trimmed().toLower();
    if (!needle.isEmpty() && !m_commitsShowingAll && m_commitsHasMore) {
        m_commitsShowingAll = true;
        loadCommits();
        return;
    }
    if (needle.isEmpty() && m_commitsShowingAll) {
        m_commitsShowingAll = false;
        m_commitsLimit = 300;
        // Retire the notice here rather than below: loadCommits only re-enters
        // this function while the box still has text, so on this path nothing
        // else would put the table back on screen.
        updateSearchEmptyNotice(m_commitSearchEmpty, m_commitsTable, QString(),
                                false);
        loadCommits();
        return;
    }
    int shown = 0;
    for (int row = 0; row < m_commitsTable->rowCount(); ++row) {
        bool match = needle.isEmpty();
        if (!match) {
            const QTableWidgetItem *summary =
                m_commitsTable->item(row, kCommitSummaryCol);
            const QTableWidgetItem *hash =
                m_commitsTable->item(row, kCommitHashCol);
            const QTableWidgetItem *author = m_commitsTable->item(row, 0);
            if (summary &&
                (summary->text().toLower().contains(needle) ||
                 summary->data(Qt::UserRole).toString().toLower().contains(needle)))
                match = true;
            else if (hash && hash->text().toLower().contains(needle))
                match = true;
            else if (author && author->text().toLower().contains(needle))
                match = true;
        }
        m_commitsTable->setRowHidden(row, !match);
        if (match)
            ++shown;
    }
    // Every row hidden is the same blank pane as an empty table, so say which
    // term did it rather than leaving the history looking unloaded.
    updateSearchEmptyNotice(m_commitSearchEmpty, m_commitsTable, query,
                            shown == 0);
}

void MainWindow::openCommitReference(int number)
{
    if (number <= 0)
        return;
    // Issue and pull ledgers have independent number sequences. A bare #N is
    // the legacy shorthand, so prefer a matching pull and fall back to the
    // issue; typed links bypass this lookup and remain unambiguous.
    reloadPulls();
    for (const PullRequest &pull : std::as_const(m_currentPulls)) {
        if (pull.number == number) {
            openPullReference(number);
            return;
        }
    }
    openIssueReference(number);
}

void MainWindow::openIssueReference(int number)
{
    if (number <= 0)
        return;
    if (m_repoDetailTabs && m_repoDetailTabs->button(2))
        m_repoDetailTabs->button(2)->click();
    reloadIssues();
    showIssue(number);
}

void MainWindow::openPullReference(int number)
{
    if (number <= 0)
        return;
    switchToPullTab(number);
}

void MainWindow::openCommitHashReference(const QString &hash)
{
    const QString ref = hash.trimmed();
    if (ref.isEmpty())
        return;
    showOverviewCommits();
    if (m_globalSearch)
        m_globalSearch->setText(ref);
    if (m_commitSearch)
        m_commitSearch->setText(ref);
    showCommit(ref);
}

void MainWindow::openReferenceLink(const QString &href)
{
    if (openNoteLink(href))
        return;
    if (href.startsWith(QStringLiteral("fm-issue:"))) {
        openIssueReference(href.mid(9).toInt());
        return;
    }
    if (href.startsWith(QStringLiteral("fm-pull:"))) {
        openPullReference(href.mid(8).toInt());
        return;
    }
    if (href.startsWith(QStringLiteral("fm-commit:"))) {
        openCommitHashReference(href.mid(10));
        return;
    }

    const QUrl url(href);
    if (url.scheme() == QLatin1String("forkmesh")) {
        const QString kind = url.host();
        const QStringList parts =
            url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        auto switchLinkedRepo = [&]() {
            if (parts.size() < 2)
                return;
            const int repoIndex = repoIndexFor(parts.at(0), parts.at(1));
            if (repoIndex >= 0 && repoIndex != m_repoDetailIndex)
                openRepoDetail(repoIndex);
        };
        if ((kind == QLatin1String("issue") || kind == QLatin1String("pull")) &&
            parts.size() >= 3) {
            switchLinkedRepo();
            const int number = parts.last().toInt();
            if (kind == QLatin1String("issue"))
                openIssueReference(number);
            else
                openPullReference(number);
            return;
        }
        if (kind == QLatin1String("commit") && parts.size() >= 3) {
            switchLinkedRepo();
            openCommitHashReference(parts.last());
            return;
        }
        if (kind == QLatin1String("cove") && parts.size() == 3) {
            switchLinkedRepo();
            m_coveExplorerCurrentId = parts.last();
            showRepoCoveExplorer();
            return;
        }
    }

    QDesktopServices::openUrl(url.isValid() ? url : QUrl::fromUserInput(href));
}

QString MainWindow::autolinkReferences(
    const QString &markdown,
    const std::function<QString(const QString &kind, int number)> &statusGlyph)
{
    if (markdown.isEmpty())
        return markdown;
    QString out;
    out.reserve(markdown.size() + 32);
    bool inFence = false;
    QString fenceMarker;
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (int li = 0; li < lines.size(); ++li) {
        if (li > 0)
            out += QLatin1Char('\n');
        const QString &line = lines.at(li);
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
            out += linkifyReferenceLine(line, statusGlyph);
        }
    }
    return out;
}

void MainWindow::openBodyReference(const QString &href)
{
    if (openNoteLink(href))
        return;
    if (href.startsWith(QStringLiteral("forkmesh-issue:"))) {
        openIssueReference(href.mid(15).toInt());
        return;
    }
    if (href.startsWith(QStringLiteral("forkmesh-pull:"))) {
        openPullReference(href.mid(14).toInt());
        return;
    }
    if (href.startsWith(QStringLiteral("forkmesh-ref:"))) {
        openCommitReference(href.mid(13).toInt());
        return;
    }
    if (href.startsWith(kTaskLinkScheme)) {
        openOrganizationTask(href.mid(kTaskLinkScheme.size()));
        return;
    }
    if (href.startsWith(kUserLinkScheme)) {
        openUserProfile(href.mid(kUserLinkScheme.size()));
        return;
    }
    if (href.startsWith(QStringLiteral("forkmesh-commit:"))) {
        const QString sha = href.mid(16);
        if (sha.isEmpty())
            return;
        showOverviewCommits();
        showCommit(sha);
        return;
    }
    if (href.startsWith(QStringLiteral("forkmesh://"))) {
        const QString rest = href.mid(QStringLiteral("forkmesh://").size());
        const QString kind = rest.section(QLatin1Char('/'), 0, 0);
        const QString id =
            rest.section(QLatin1Char('/'), -1).section(QLatin1Char('#'), 0, 0);
        if (kind == QLatin1String("issue")) {
            if (m_repoDetailTabs && m_repoDetailTabs->button(2))
                m_repoDetailTabs->button(2)->click();
            reloadIssuesInBackground();
            showIssue(id.toInt());
        } else if (kind == QLatin1String("pull")) {
            reloadPulls();
            if (m_repoDetailTabs && m_repoDetailTabs->button(4))
                m_repoDetailTabs->button(4)->click();
            showPull(id.toInt());
        } else if (kind == QLatin1String("commit")) {
            openBodyReference(QStringLiteral("forkmesh-commit:%1").arg(id));
        } else if (kind == QLatin1String("cove")) {
            openReferenceLink(href);
        }
        return;
    }
    QDesktopServices::openUrl(QUrl(href));
}

void MainWindow::copyReferenceLink(const QString &kind, const QString &id)
{
    if (id.isEmpty())
        return;
    QString owner = QStringLiteral("repo");
    QString repo = kind;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        repo = m_repositories.at(m_repoDetailIndex).name;
    }
    QApplication::clipboard()->setText(
        QStringLiteral("forkmesh://%1/%2/%3/%4").arg(kind, owner, repo, id));
    setRepoDetailNotice(QString::fromUtf8("Link copied \xE2\x80\x94 paste it into a "
                                       "comment to link back here."));
}

void MainWindow::downloadCommitPatch()
{
    const QString dir = repoGitDir();
    if (dir.isEmpty() || m_currentCommitHash.isEmpty() || m_commitPatchRunning)
        return;
    const QString hash = m_currentCommitHash;
    m_commitPatchRunning = true;
    runOffThread<GitCaptureResult>(
        [dir, hash] { return readGit(dir, {"format-patch", "-1", "--stdout", hash}); },
        [this, hash](GitCaptureResult read) {
            m_commitPatchRunning = false;
            saveCommitPatch(hash, read);
        });
}

void MainWindow::saveCommitPatch(const QString &hash, const GitCaptureResult &read)
{
    const QByteArray &patch = read.output;
    if (!read.ok || patch.trimmed().isEmpty()) {
        setRepoDetailNotice(read.error.isEmpty() ? "Could not generate the patch."
                                                 : read.error,
                            true);
        return;
    }
    const QString shortHash = hash.left(8);
    const QString suggested =
        QDir::homePath() + "/" + shortHash + QStringLiteral(".patch");
    const QString path = QFileDialog::getSaveFileName(
        this, "Download patch", suggested, "Patch files (*.patch *.diff)");
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(patch) < 0) {
        setRepoDetailNotice("Could not write the patch file.", true);
        return;
    }
    file.close();
    setRepoDetailNotice(QStringLiteral("Saved patch to %1.").arg(path));
    logSystem(QStringLiteral("Saved commit %1 as patch %2.").arg(shortHash, path));
}

void MainWindow::applyCommitIssueClosures()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite())
        return;
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return;
    if (m_commitClosureScanRunning) {
        m_commitClosureScanPending = true;
        return;
    }
    const QString ref = currentRef();
    const QString issuesSignature = store.contentSignature();
    const QString previousKey = m_commitClosureScanKey;
    m_commitClosureScanRunning = true;
    m_commitClosureScanPending = false;
    runOffThread<CommitClosureScan>(
        [dir, ref, issuesSignature, previousKey] {
            return readCommitClosureScan(dir, ref, issuesSignature, previousKey);
        },
        [this, dir, ref](CommitClosureScan scan) {
            m_commitClosureScanRunning = false;
            if (std::exchange(m_commitClosureScanPending, false)) {
                QTimer::singleShot(0, this, [this] { applyCommitIssueClosures(); });
                return;
            }
            if (!scan.ok || scan.unchanged || dir != repoGitDir() ||
                ref != currentRef())
                return;
            m_commitClosureScanKey = scan.key;
            if (!scan.namesIssue)
                return;
            IssueStore current = issueStoreForCurrentRepo();
            if (!current.canWrite())
                return;
            closeIssuesNamedByCommits(std::move(current), scan.log);
        });
}

static const QRegularExpression &commitClosesIssuePattern()
{
    static const QRegularExpression closeRe(
        QStringLiteral("\\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)\\b\\s*:?\\s*"
                       "#(\\d+)"),
        QRegularExpression::CaseInsensitiveOption);
    return closeRe;
}

MainWindow::CommitClosureScan
MainWindow::readCommitClosureScan(const QString &dir, const QString &ref,
                                  const QString &issuesSignature,
                                  const QString &previousKey)
{
    CommitClosureScan scan;
    QByteArray tipOut;
    if (!runGitCapture(dir, {"rev-parse", ref}, &tipOut, nullptr))
        return scan;
    scan.key = dir + QLatin1Char('\x1f') + ref + QLatin1Char('\x1f') +
               QString::fromUtf8(tipOut).trimmed() + QLatin1Char('\x1f') +
               issuesSignature;
    if (!issuesSignature.isEmpty() && scan.key == previousKey) {
        scan.ok = true;
        scan.unchanged = true;
        return scan;
    }
    if (!runGitCapture(dir,
                       {"log", "--format=%h%x1f%s%x1f%B%x1e", "-n", "500", ref},
                       &scan.log, nullptr))
        return scan;
    scan.ok = true;
    scan.namesIssue =
        commitClosesIssuePattern().match(QString::fromUtf8(scan.log)).hasMatch();
    return scan;
}

// code-health: allow(deep-nested-loop) ≤500 commits, regex-gated; the innermost event scan runs only for an open issue a commit message names
void MainWindow::closeIssuesNamedByCommits(IssueStore store, const QByteArray &out)
{
    const QRegularExpression &closeRe = commitClosesIssuePattern();
    QList<Issue> issues = store.loadAll();
    QHash<int, const Issue *> byNumber;
    for (const Issue &issue : issues)
        byNumber.insert(issue.number, &issue);

    int closedCount = 0;
    QString lastClosed;
    for (const QByteArray &record : out.split('\x1e')) {
        if (record.trimmed().isEmpty())
            continue;
        const QStringList f = QString::fromUtf8(record).split('\x1f');
        if (f.size() < 3)
            continue;
        const QString shortHash = f.at(0).trimmed();
        const QString subject = f.at(1).trimmed();
        const QString message = f.at(2);

        auto it = closeRe.globalMatch(message);
        QSet<int> seenInThisCommit; // a commit may name the same issue twice
        while (it.hasNext()) {
            const int number = it.next().captured(1).toInt();
            if (number <= 0 || seenInThisCommit.contains(number))
                continue;
            seenInThisCommit.insert(number);

            const Issue *issue = byNumber.value(number, nullptr);
            if (!issue || issue->status == QLatin1String("closed"))
                continue;

            bool alreadyLinked = false;
            for (const IssueEvent &ev : issue->events) {
                if (ev.type == QLatin1String("comment") &&
                    ev.body.contains(shortHash)) {
                    alreadyLinked = true;
                    break;
                }
            }
            if (alreadyLinked)
                continue;

            QString err;
            const QString note =
                QString::fromUtf8("Closed by commit `%1` \xE2\x80\x94 %2")
                    .arg(shortHash, subject);
            if (!store.addComment(number, note, {}, &err)) {
                logSystem(QStringLiteral("Issue #%1: could not link commit %2: %3")
                              .arg(number)
                              .arg(shortHash, err));
                continue;
            }
            if (!store.setStatus(number, QStringLiteral("closed"), &err)) {
                logSystem(QStringLiteral("Issue #%1: could not close: %2")
                              .arg(number)
                              .arg(err));
                continue;
            }
            logSystem(QStringLiteral("Closed issue #%1 via commit %2.")
                          .arg(number)
                          .arg(shortHash));
            ++closedCount;
            lastClosed = QStringLiteral("#%1").arg(number);
            issues = store.loadAll();
            byNumber.clear();
            for (const Issue &i : issues)
                byNumber.insert(i.number, &i);
        }
    }

    if (closedCount > 0) {
        flashMessage(closedCount == 1
                         ? QStringLiteral("Closed issue %1 from commit message.")
                               .arg(lastClosed)
                         : QStringLiteral("Closed %1 issues from commit messages.")
                               .arg(closedCount));
        reloadIssues();
        updateRepoIssueCount();
    }
}


void MainWindow::updateDiffSplitButton(QPushButton *button)
{
    if (!button)
        return;
    const bool split = diffSplitPref();
    button->setText(split ? QStringLiteral("Side-by-side")
                          : QStringLiteral("Unified"));
    button->setToolTip(split
                           ? QString::fromUtf8("Showing a side-by-side diff \xE2\x80\x94 "
                                            "click for a unified diff")
                           : QString::fromUtf8("Showing a unified diff \xE2\x80\x94 "
                                            "click for a side-by-side diff"));
}

void MainWindow::syncCurrentCommitRow(const QString &hash)
{
    m_currentCommitRow = -1;
    if (m_commitsTable)
        for (int i = 0; i < m_commitsTable->rowCount(); ++i)
            if (m_commitsTable->item(i, kCommitSummaryCol)
                    ->data(Qt::UserRole)
                    .toString() == hash) {
                m_currentCommitRow = i;
                break;
            }
    if (m_commitPrevButton)
        m_commitPrevButton->setEnabled(m_currentCommitRow > 0);
    if (m_commitNextButton)
        m_commitNextButton->setEnabled(m_commitsTable &&
                                       m_currentCommitRow >= 0 &&
                                       m_currentCommitRow < m_commitsTable->rowCount() - 1);
    if (m_commitsTable && m_currentCommitRow >= 0 &&
        m_pendingCommitFileScroll.isEmpty()) {
        QSignalBlocker blk(m_commitsTable);
        m_commitsTable->selectRow(m_currentCommitRow);
    }
}

void MainWindow::showCommit(const QString &hash)
{
    const QString dir = repoGitDir();
    if (dir.isEmpty() || hash.isEmpty() || !m_commitsStack)
        return;

    syncCurrentCommitRow(hash);

    const int gen = ++m_commitLoadGen;

    setCommitWorkspacePage(kCommitWorkspaceCommitPage);
    startCommitDiffSpin();

    m_currentCommitHash = hash; // refined to the full hash when metadata lands
    if (m_commitDownloadButton)
        m_commitDownloadButton->setEnabled(true);
    if (m_commitDeleteButton || m_commitRevertButton) {
        const bool writable = repoHasWorkingTree();
        if (m_commitDeleteButton) {
            m_commitDeleteButton->setEnabled(writable);
            m_commitDeleteButton->setToolTip(
                writable ? QStringLiteral(
                               "Remove this commit from history (rewrites the branch "
                               "and replays the later commits onto its parent)")
                         : QString::fromUtf8(
                               "Read-only mirror \xE2\x80\x94 no working tree to "
                               "rewrite history in"));
        }
        if (m_commitRevertButton) {
            m_commitRevertButton->setEnabled(writable);
            m_commitRevertButton->setToolTip(
                writable ? QStringLiteral(
                               "Undo this commit by committing the reverse of its "
                               "changes (history is kept)")
                         : QString::fromUtf8(
                               "Read-only mirror \xE2\x80\x94 no working tree to "
                               "commit a revert into"));
        }
    }
    runGitDetached(
        dir,
        {QStringLiteral("show"), QStringLiteral("-s"),
         QStringLiteral("--date=format:%b %e, %Y"),
         QStringLiteral("--format=%H%x1f%an%x1f%ad%x1f%P%x1f%s%x1f%b"), hash},
        [this, gen, dir, hash](bool, const QByteArray &meta) {
            if (gen != m_commitLoadGen)
                return;
            const QStringList mf =
                QString::fromUtf8(meta).split(QLatin1Char('\x1f'));
            const QString full = mf.value(0).trimmed();
            const QString fullHash = full.isEmpty() ? hash : full;
            const QString emptyTree =
                QStringLiteral("4b825dc642cb6eb9a060e54bf8d69288fbee4904");
            const QStringList parents = mf.value(3).trimmed().split(
                QLatin1Char(' '), Qt::SkipEmptyParts);
            const QString base =
                parents.isEmpty() ? emptyTree : parents.first();
            runGitDetached(
                dir,
                {QStringLiteral("diff"), QStringLiteral("-M"), base, fullHash},
                [this, gen, dir, hash, mf, base,
                 fullHash](bool, const QByteArray &patch) {
                    if (gen != m_commitLoadGen)
                        return;
                    runOffThread<bool>(
                        [patch, dir, base, fullHash] {
                            prefetchDiffImageBlobs(QString::fromUtf8(patch), dir, base,
                                                   fullHash);
                            return true;
                        },
                        [this, gen, dir, hash, mf, patch](bool) {
                            if (gen != m_commitLoadGen)
                                return;
                            renderCommitDetail(dir, hash, mf, patch);
                        });
                });
        });
}

static void refreshCommitMessageLabel(QLabel *label)
{
    if (!label)
        return;
    const QString subject = label->property("subjectHtml").toString();
    const QString body = label->property("bodyHtml").toString();
    const bool expanded = label->property("expanded").toBool();
    QString msg;
    if (!body.isEmpty())
        msg += QStringLiteral("<a href='toggle-msg' "
                              "style='color:#8b949e;text-decoration:none'>%1</a> ")
                   .arg(expanded ? QString::fromUtf8("\xE2\x96\xBE")
                                 : QString::fromUtf8("\xE2\x96\xB8"));
    msg += QStringLiteral("<b>%1</b>").arg(subject);
    if (expanded && !body.isEmpty())
        msg += QStringLiteral(
                   "<br><span style='color:#8b949e; white-space:pre-wrap'>%1</span>")
                   .arg(body);
    label->setText(msg);
}

void MainWindow::renderCommitDetail(const QString &dir, const QString &hash,
                                    const QStringList &metaFields,
                                    const QByteArray &patchRaw)
{
    const QString full = metaFields.value(0).trimmed();
    const QString author = metaFields.value(1).trimmed();
    const QString date = metaFields.value(2).trimmed();
    const QStringList parents =
        metaFields.value(3).trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString subject = metaFields.value(4).trimmed();
    const QString body = metaFields.value(5).trimmed();
    const QString fullHash = full.isEmpty() ? hash : full;
    const QString emptyTree =
        QStringLiteral("4b825dc642cb6eb9a060e54bf8d69288fbee4904");
    const QString base = parents.isEmpty() ? emptyTree : parents.first();
    m_currentCommitHash = fullHash;

    if (m_commitTitle) {
        QString breakableHash;
        breakableHash.reserve(fullHash.size() + fullHash.size() / 8 * 7);
        for (int i = 0; i < fullHash.size(); ++i) {
            breakableHash += fullHash.mid(i, 1).toHtmlEscaped();
            if ((i + 1) % 8 == 0 && i + 1 < fullHash.size())
                breakableHash += QStringLiteral("&#8203;");
        }
        m_commitTitle->setText(
            QStringLiteral("Commit <code>%1</code>").arg(breakableHash));
    }
    if (m_commitMessage) {
        m_commitMessage->setProperty("subjectHtml",
                                     linkifyIssueRefs(subject.toHtmlEscaped()));
        m_commitMessage->setProperty("bodyHtml",
                                     linkifyIssueRefs(body.toHtmlEscaped()));
        m_commitMessage->setProperty("expanded", false);
        m_commitMessage->setToolTip(
            body.isEmpty() ? subject
                           : subject + QStringLiteral("\n\n") + body);
        refreshCommitMessageLabel(m_commitMessage);
    }

    QList<DiffFileEntry> files;
    const QString diffHtml =
        renderDiffHtml(QString::fromUtf8(patchRaw), files, dir, base, fullHash);
    int totalAdds = 0, totalDels = 0;
    for (const DiffFileEntry &f : files) {
        totalAdds += f.adds;
        totalDels += f.dels;
    }

    if (m_commitMeta)
        m_commitMeta->setText(
            QString::fromUtf8("%1 committed on %2 \xC2\xB7 %3 parent%4 \xC2\xB7 "
                           "<b>%5</b> file%6 changed "
                           "<span style='color:#3fb950'>+%7</span> "
                           "<span style='color:#f85149'>\xE2\x88\x92%8</span>")
                .arg(author.toHtmlEscaped(), date.toHtmlEscaped(),
                     QString::number(qMax(1, parents.size())),
                     parents.size() == 1 ? "" : "s", QString::number(files.size()),
                     files.size() == 1 ? "" : "s", QString::number(totalAdds),
                     QString::number(totalDels)));

    if (m_commitFilesSummary)
        m_commitFilesSummary->setText(
            QStringLiteral("%1 file%2 changed")
                .arg(files.size())
                .arg(files.size() == 1 ? "" : "s"));

    if (m_commitDiffView) {
        setDiffHtml(m_commitDiffView,
                    diffHtml.isEmpty()
                        ? QStringLiteral("<p style='color:#8b949e'>"
                                         "No changes in this commit.</p>")
                        : diffHtml);
    }
    if (m_commitDiffView && !m_pendingCommitFileScroll.isEmpty()) {
        for (const DiffFileEntry &f : files) {
            if (f.path == m_pendingCommitFileScroll) {
                scrollDiffToAnchor(m_commitDiffView, f.anchor);
                break;
            }
        }
        m_pendingCommitFileScroll.clear();
    }

    stopCommitDiffSpin();
    if (gitWorkspaceIsVisible())
        setCommitWorkspacePage(kCommitWorkspaceCommitPage);
}

void MainWindow::loadRepoInsights()
{
    if (!m_insightsSummary)
        return;
    const QString dir = repoGitDir();
    const bool haveRepo =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size();
    if (!haveRepo || dir.isEmpty()) {
        ++m_insightsLoadGen;
        m_insightsLoading = false;
        m_insightsReloadPending = false;
        renderRepoInsights(InsightsGitRead{});
        return;
    }
    if (m_insightsLoading) {
        m_insightsReloadPending = true;
        return;
    }
    InsightsGitRead request;
    request.dir = dir;
    request.ref = currentRef();
    request.windowDays =
        m_insightsRangeCombo ? m_insightsRangeCombo->currentData().toInt() : 0;
    const quint64 generation = ++m_insightsLoadGen;
    m_insightsLoading = true;
    m_insightsReloadPending = false;
    runOffThread<InsightsGitRead>(
        [request] { return readRepoInsightsGit(request); },
        [this, generation](InsightsGitRead read) {
            if (generation != m_insightsLoadGen)
                return;
            m_insightsLoading = false;
            const int windowDays =
                m_insightsRangeCombo ? m_insightsRangeCombo->currentData().toInt()
                                     : 0;
            if (std::exchange(m_insightsReloadPending, false) ||
                read.dir != repoGitDir() || read.ref != currentRef() ||
                read.windowDays != windowDays) {
                loadRepoInsights();
                return;
            }
            renderRepoInsights(read);
        });
}

MainWindow::InsightsGitRead MainWindow::readRepoInsightsGit(InsightsGitRead read)
{
    read.treeOk = runGitCapture(read.dir, {"ls-tree", "-r", "-l", read.ref},
                                &read.treeOut, &read.treeErr);
    QStringList logArgs{"log", "--format=%an%x1f%ae%x1f%ct", "-n", "50000"};
    if (read.windowDays > 0)
        logArgs << QStringLiteral("--since=%1.days.ago").arg(read.windowDays);
    logArgs << read.ref;
    read.logOk = runGitCapture(read.dir, logArgs, &read.logOut, &read.logErr);
    return read;
}

void MainWindow::renderRepoInsights(const InsightsGitRead &git)
{
    if (!m_insightsSummary)
        return;

    TableRepaintGuard repaintGuard(m_insightsContributors);
    if (m_insightsContributors)
        m_insightsContributors->setRowCount(0);
    if (m_insightsLanguageBar)
        m_insightsLanguageBar->clear();
    if (m_insightsLanguageLegend)
        m_insightsLanguageLegend->clear();
    if (m_insightsActivity)
        m_insightsActivity->clear();
    if (m_insightsActivityAxis)
        m_insightsActivityAxis->clear();

    auto setNoRepo = [this] {
        m_insightsSummary->setText(
            "<b>Repository summary</b><br><span style='color:#8b949e'>Select a "
            "repository with a local checkout or mirror to see insights.</span>");
        if (m_insightsTraffic)
            m_insightsTraffic->setText(
                "<b>ForkMesh traffic</b><br><span style='color:#8b949e'>No "
                "repository selected.</span>");
        if (m_insightsLanguageLegend)
            m_insightsLanguageLegend->setText(
                "<span style='color:#8b949e'>No language data available.</span>");
    };

    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        setNoRepo();
        return;
    }

    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QString dir = git.dir;
    const QString ref = git.dir.isEmpty() ? currentRef() : git.ref;
    QStringList notes;

    QString sourceKind = QStringLiteral("unavailable");
    if (!repo.localPath.isEmpty() && QDir(repo.localPath).exists())
        sourceKind = QStringLiteral("local worktree");
    else if (repo.previewOnly && !repo.mirrorPath.isEmpty() &&
             QDir(repo.mirrorPath).exists())
        sourceKind = QStringLiteral("preview cache");
    else if (!repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists())
        sourceKind = QStringLiteral("bare mirror");

    notes << QStringLiteral("Data source: %1. Selected ref: %2.")
                 .arg(sourceKind, ref);
    if (!repositorySource(repo).isEmpty())
        notes << QStringLiteral("Source: %1.").arg(repositorySource(repo));
    if (repo.lastSyncMs > 0)
        notes << QStringLiteral("Last sync: %1.").arg(formatRepoDate(repo.lastSyncMs));

    qint64 fileCount = 0;
    qint64 totalBytes = 0;
    QHash<QString, qint64> bytesByLanguage;
    qint64 recognizedBytes = 0;

    if (dir.isEmpty()) {
        notes << QStringLiteral("No local checkout or mirror is available for Git history.");
    } else {
        const QByteArray &treeOut = git.treeOut;
        const QString &treeErr = git.treeErr;
        if (git.treeOk) {
            for (const QByteArray &record : treeOut.split('\n')) {
                if (record.trimmed().isEmpty())
                    continue;
                const int tab = record.indexOf('\t');
                if (tab < 0)
                    continue;
                const QList<QByteArray> meta = record.left(tab).simplified().split(' ');
                if (meta.size() < 4 || meta.at(1) != "blob")
                    continue;
                ++fileCount;
                bool ok = false;
                const qint64 size = QString::fromUtf8(meta.at(3)).toLongLong(&ok);
                if (!ok)
                    continue;
                totalBytes += qMax<qint64>(0, size);
                const QString name = QString::fromUtf8(record.mid(tab + 1));
                const QString language = languageForFile(name);
                if (!language.isEmpty() && size > 0) {
                    bytesByLanguage[language] += size;
                    recognizedBytes += size;
                }
            }
        } else {
            notes << QStringLiteral("File composition unavailable: %1.")
                         .arg(treeErr.isEmpty() ? QStringLiteral("git failed") : treeErr.left(160));
        }
    }

    int openIssues = 0;
    int closedIssues = 0;
    for (const Issue &issue : std::as_const(m_currentIssues)) {
        if (issue.isDeleted())
            continue;
        if (issue.status == QStringLiteral("closed"))
            ++closedIssues;
        else
            ++openIssues;
    }

    QString pullError;
    const QList<PullRequest> pulls = pullStoreForCurrentRepo().loadAll(&pullError);
    int openPulls = 0;
    int mergedPulls = 0;
    int closedPulls = 0;
    for (const PullRequest &pr : pulls) {
        if (pr.status == QStringLiteral("merged"))
            ++mergedPulls;
        else if (pr.status == QStringLiteral("closed"))
            ++closedPulls;
        else
            ++openPulls;
    }
    if (!pullError.isEmpty())
        notes << QStringLiteral("Pull request data unavailable: %1.").arg(pullError.left(160));

    const int totalIssues = openIssues + closedIssues;
    const int totalPulls = openPulls + mergedPulls + closedPulls;
    m_insightsSummary->setText(
        "<b>Repository summary</b>" +
        insightMetricsTable({
            insightMetricCell("Contributors", QStringLiteral("0"), "current branch"),
            insightMetricCell("Files", QString::number(fileCount), "tracked blobs"),
            insightMetricCell("Code size", formatInsightBytes(totalBytes), "tracked bytes"),
            insightMetricCell("Issues", QString::number(totalIssues),
                              QStringLiteral("%1 open / %2 closed")
                                  .arg(openIssues)
                                  .arg(closedIssues)),
            insightMetricCell("Pull requests", QString::number(totalPulls),
                              QStringLiteral("%1 open / %2 merged / %3 closed")
                                  .arg(openPulls)
                                  .arg(mergedPulls)
                                  .arg(closedPulls)),
        }));

    if (m_insightsTraffic) {
        m_insightsTraffic->setText(
            QStringLiteral(
                "<b>ForkMesh traffic</b><br><br>"
                "<span style='color:#8b949e'>Serving and clone traffic is reported "
                "by active managed mirror nodes. Open the Mirror Nodes tab for "
                "per-node counters.</span>"));
    }

    QList<QPair<QString, qint64>> languages;
    for (auto it = bytesByLanguage.constBegin(); it != bytesByLanguage.constEnd(); ++it)
        languages.append({it.key(), it.value()});
    std::sort(languages.begin(), languages.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    QString languageLegend;
    const int shownLanguages = qMin(5, int(languages.size()));
    for (int i = 0; i < shownLanguages && recognizedBytes > 0; ++i) {
        const double pct = 100.0 * languages.at(i).second / recognizedBytes;
        const QString color = languageColor(languages.at(i).first);
        languageLegend += QString::fromUtf8(
                              "<span style='color:%1'>\xE2\x97\x8F</span> %2 %3% "
                              "<span style='color:#8b949e'>(%4)</span>&nbsp;&nbsp;")
                              .arg(color,
                                   languages.at(i).first.toHtmlEscaped(),
                                   QString::number(pct, 'f', 1),
                                   formatInsightBytes(languages.at(i).second));
    }
    if (m_insightsLanguageBar) {
        m_insightsLanguageBar->setScaledContents(true);
        m_insightsLanguageBar->setPixmap(languageBarPixmap(
            languages, recognizedBytes, shownLanguages, 600, 10));
    }
    if (m_insightsLanguageLegend)
        m_insightsLanguageLegend->setText(
            languageLegend.isEmpty()
                ? "<span style='color:#8b949e'>No recognized code files yet.</span>"
                : languageLegend);

    const int windowDays =
        git.dir.isEmpty()
            ? (m_insightsRangeCombo ? m_insightsRangeCombo->currentData().toInt() : 0)
            : git.windowDays;
    constexpr int kBuckets = 32;

    struct Contributor {
        QString name;
        int commits = 0;
        QVector<int> buckets;
        QHash<QString, int> commitsByEmail;
        qint64 firstTs = 0;
        qint64 lastTs = 0;
    };
    QHash<QString, int> indexByName; // author name -> contributors[] index
    QList<Contributor> contributors;
    int contributorCommitTotal = 0;
    qint64 minTs = 0, maxTs = 0;
    int sharedMax = 1;

    if (!dir.isEmpty()) {
        const QByteArray &logOut = git.logOut;
        const QString &logErr = git.logErr;
        if (git.logOk) {
            struct Stamp {
                QString author;
                QString email;
                qint64 ts;
            };
            QList<Stamp> stamps;
            for (const QByteArray &line : logOut.split('\n')) {
                if (line.trimmed().isEmpty())
                    continue;
                const QStringList f = QString::fromUtf8(line).split(QLatin1Char('\x1f'));
                if (f.size() < 3)
                    continue;
                const QString author = f.at(0).trimmed();
                const QString email = f.at(1).trimmed();
                const qint64 ts = f.at(2).toLongLong();
                if (author.isEmpty() || ts <= 0)
                    continue;
                stamps.append({author, email, ts});
                if (minTs == 0 || ts < minTs)
                    minTs = ts;
                if (ts > maxTs)
                    maxTs = ts;
            }
            const qint64 span = qMax<qint64>(1, maxTs - minTs);
            for (const Stamp &s : std::as_const(stamps)) {
                int idx = indexByName.value(s.author, -1);
                if (idx < 0) {
                    idx = int(contributors.size());
                    indexByName.insert(s.author, idx);
                    Contributor c;
                    c.name = s.author;
                    c.buckets.resize(kBuckets);
                    contributors.append(c);
                }
                Contributor &c = contributors[idx];
                ++c.commits;
                if (!s.email.isEmpty())
                    ++c.commitsByEmail[s.email];
                if (c.firstTs == 0 || s.ts < c.firstTs)
                    c.firstTs = s.ts;
                if (s.ts > c.lastTs)
                    c.lastTs = s.ts;
                ++contributorCommitTotal;
                if (maxTs > minTs) {
                    const int b = qBound<int>(
                        0, int((s.ts - minTs) * kBuckets / (span + 1)), kBuckets - 1);
                    ++c.buckets[b];
                    sharedMax = qMax(sharedMax, c.buckets.at(b));
                }
            }
        } else {
            notes << QStringLiteral("Contributor data unavailable: %1.")
                         .arg(logErr.isEmpty() ? QStringLiteral("git failed")
                                               : logErr.left(160));
        }
    }

    std::sort(contributors.begin(), contributors.end(),
              [](const Contributor &a, const Contributor &b) {
                  if (a.commits != b.commits)
                      return a.commits > b.commits;
                  return a.name.localeAwareCompare(b.name) < 0;
              });

    if (m_insightsContributors) {
        for (const Contributor &c : std::as_const(contributors)) {
            const int row = m_insightsContributors->rowCount();
            m_insightsContributors->insertRow(row);

            const QString openHint =
                QStringLiteral("Click to view %1's commits").arg(c.name);
            auto *nameItem = new QTableWidgetItem(c.name);
            nameItem->setData(Qt::UserRole, c.name); // read by the reassign menu
            nameItem->setToolTip(openHint);
            m_insightsContributors->setItem(row, 0, nameItem);

            QList<QPair<QString, int>> emails;
            for (auto it = c.commitsByEmail.constBegin(); it != c.commitsByEmail.constEnd();
                 ++it)
                emails.append({it.key(), it.value()});
            std::sort(emails.begin(), emails.end(), [](const auto &a, const auto &b) {
                if (a.second != b.second)
                    return a.second > b.second;
                return a.first < b.first;
            });
            QStringList emailList;
            QStringList emailTips;
            for (const auto &email : std::as_const(emails)) {
                emailList << email.first;
                emailTips << QStringLiteral("%1 (%2 commits)").arg(email.first).arg(email.second);
            }
            auto *emailItem = new QTableWidgetItem(emailList.join(QStringLiteral(", ")));
            emailItem->setData(Qt::UserRole, emailList);
            emailItem->setToolTip(emailTips.join(QLatin1Char('\n')));
            m_insightsContributors->setItem(row, 1, emailItem);

            auto *countItem = new QTableWidgetItem;
            countItem->setData(Qt::DisplayRole, c.commits);
            countItem->setToolTip(openHint);
            m_insightsContributors->setItem(row, 2, countItem);

            const double pct = contributorCommitTotal > 0
                                   ? 100.0 * c.commits / contributorCommitTotal
                                   : 0.0;
            m_insightsContributors->setItem(
                row, 3, new QTableWidgetItem(QStringLiteral("%1%").arg(pct, 0, 'f', 1)));

            const auto commitDateItem = [](qint64 ts) {
                const QDateTime when = QDateTime::fromSecsSinceEpoch(ts);
                auto *item = new QTableWidgetItem(when.date().toString("MMM d, yyyy"));
                item->setToolTip(QLocale().toString(when, QLocale::LongFormat));
                return item;
            };
            m_insightsContributors->setItem(row, 4, commitDateItem(c.firstTs));
            m_insightsContributors->setItem(row, 5, commitDateItem(c.lastTs));

            if (maxTs > minTs) {
                auto *chart = new CommitBarChart(c.buckets, sharedMax,
                                                 insightContributorColor(c.name));
                chart->setToolTip(
                    QString::fromUtf8("%1 \xE2\x80\x94 %2 commits over time")
                        .arg(c.name)
                        .arg(c.commits));
                m_insightsContributors->setCellWidget(row, 6, chart);
            }
        }
        if (contributors.isEmpty()) {
            const int row = m_insightsContributors->rowCount();
            m_insightsContributors->insertRow(row);
            auto *empty = new QTableWidgetItem(
                windowDays > 0
                    ? QStringLiteral("No commits in the selected range.")
                    : QStringLiteral("No commit history available."));
            empty->setForeground(QColor(0x8b, 0x94, 0x9e));
            m_insightsContributors->setItem(row, 0, empty);
            m_insightsContributors->setSpan(row, 0, 1, 7);
        }
        fitInsightsContributorsHeight();
    }

    if (m_insightsActivityAxis) {
        if (maxTs > minTs) {
            m_insightsActivityAxis->setText(
                QString::fromUtf8(
                    "<span style='color:#8b949e'>%1 &nbsp;&nbsp;\xE2\x86\x90 oldest "
                    "&nbsp;&nbsp;\xC2\xB7&nbsp;&nbsp; newest \xE2\x86\x92&nbsp;&nbsp; "
                    "%2 &nbsp;&nbsp;\xC2\xB7&nbsp;&nbsp; click a contributor to view "
                    "their commits, right-click to reassign attribution or copy their email</span>")
                    .arg(QDateTime::fromSecsSinceEpoch(minTs).date().toString(
                             "MMM d, yyyy"),
                         QDateTime::fromSecsSinceEpoch(maxTs).date().toString(
                             "MMM d, yyyy")));
        } else {
            m_insightsActivityAxis->clear();
        }
    }

    m_insightsSummary->setText(
        "<b>Repository summary</b>" +
        insightMetricsTable({
            insightMetricCell("Contributors", QString::number(contributors.size()),
                              windowDays > 0 ? QStringLiteral("in selected range")
                                             : QStringLiteral("current branch")),
            insightMetricCell("Files", QString::number(fileCount), "tracked blobs"),
            insightMetricCell("Code size", formatInsightBytes(totalBytes), "tracked bytes"),
            insightMetricCell("Issues", QString::number(totalIssues),
                              QStringLiteral("%1 open / %2 closed")
                                  .arg(openIssues)
                                  .arg(closedIssues)),
            insightMetricCell("Pull requests", QString::number(totalPulls),
                              QStringLiteral("%1 open / %2 merged / %3 closed")
                                  .arg(openPulls)
                                  .arg(mergedPulls)
                                  .arg(closedPulls)),
        }));

    if (m_insightsActivity) {
        QStringList escapedNotes;
        for (const QString &note : std::as_const(notes))
            escapedNotes << note.toHtmlEscaped();
        m_insightsActivity->setText(escapedNotes.join("<br>"));
    }
}

void MainWindow::showInsightsContributorMenu(const QPoint &pos)
{
    if (!m_insightsContributors)
        return;
    const QModelIndex idx = m_insightsContributors->indexAt(pos);
    if (!idx.isValid())
        return;
    QTableWidgetItem *nameItem = m_insightsContributors->item(idx.row(), 0);
    if (!nameItem)
        return;
    const QString name = nameItem->data(Qt::UserRole).toString().isEmpty()
                             ? nameItem->text()
                             : nameItem->data(Qt::UserRole).toString();
    if (name.isEmpty())
        return;

    QTableWidgetItem *emailItem = m_insightsContributors->item(idx.row(), 1);
    const QStringList emails =
        emailItem ? emailItem->data(Qt::UserRole).toStringList() : QStringList();

    QMenu menu(this);
    QAction *reassign =
        menu.addAction(QString::fromUtf8("Reassign attribution for \"%1\"\xE2\x80\xA6").arg(name));
    QAction *copyEmail = nullptr;
    if (!emails.isEmpty())
        copyEmail = menu.addAction(emails.size() == 1 ? QStringLiteral("Copy email")
                                                      : QStringLiteral("Copy emails"));
    QAction *chosen = menu.exec(m_insightsContributors->viewport()->mapToGlobal(pos));
    if (chosen == reassign)
        reassignContributorIdentity(name);
    else if (chosen && chosen == copyEmail)
        QApplication::clipboard()->setText(emails.join(QStringLiteral(", ")));
}

void MainWindow::fitInsightsContributorsHeight()
{
    if (!m_insightsContributors)
        return;
    QTableWidget *table = m_insightsContributors;
    QHeaderView *header = table->horizontalHeader();
    int height = 2 * table->frameWidth();
    if (!header->isHidden())
        height += qMax(header->height(), header->sizeHint().height());
    for (int row = 0; row < table->rowCount(); ++row)
        height += table->rowHeight(row);
    if (table->horizontalScrollBar()->maximum() > 0)
        height += table->horizontalScrollBar()->sizeHint().height();
    table->setFixedHeight(height);
}

void MainWindow::openCommitsForContributor(const QString &author)
{
    const QString name = author.trimmed();
    if (name.isEmpty())
        return;
    showOverviewCommits();
    if (m_globalSearch) {
        m_globalSearch->setText(name);
        m_globalSearch->setFocus();
    }
    if (m_commitSearch)
        m_commitSearch->setText(name);
}

void MainWindow::reassignContributorIdentity(const QString &oldName)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QString dir = repo.localPath;
    if (dir.isEmpty() || !QDir(dir).exists()) {
        QMessageBox::warning(
            this, QStringLiteral("Reassign attribution"),
            QStringLiteral("Rewriting author identity needs a local checkout of this "
                           "repository. This node only has a mirror or preview cache."));
        return;
    }

    if (m_reassignIdentityRunning)
        return;
    m_reassignIdentityRunning = true;
    const int repoIndex = m_repoDetailIndex;
    runOffThread<ContributorIdentityRead>(
        [dir, oldName] { return readContributorIdentities(dir, oldName); },
        [this, dir, oldName, repoIndex](ContributorIdentityRead read) {
            m_reassignIdentityRunning = false;
            if (repoIndex != m_repoDetailIndex || repoIndex < 0 ||
                repoIndex >= m_repositories.size() ||
                m_repositories.at(repoIndex).localPath != dir)
                return;
            if (read.dirty) {
                QMessageBox::warning(
                    this, QStringLiteral("Reassign attribution"),
                    QString::fromUtf8("Commit or stash your local changes first "
                                      "\xE2\x80\x94 rewriting history requires a "
                                      "clean working tree."));
                return;
            }
            continueReassignContributorIdentity(dir, oldName, read.identities,
                                                read.primaryEmail);
        });
}

MainWindow::ContributorIdentityRead
MainWindow::readContributorIdentities(const QString &dir, const QString &oldName)
{
    ContributorIdentityRead read;
    QByteArray statusOut;
    if (runGitCapture(dir, {"status", "--porcelain"}, &statusOut, nullptr) &&
        !QString::fromUtf8(statusOut).trimmed().isEmpty()) {
        read.dirty = true;
        return read;
    }
    QByteArray out;
    if (runGitCapture(dir, {"log", "--all", "--format=%an%x1f%ae"}, &out, nullptr)) {
        QSet<QString> seen;
        QHash<QString, int> emailHits;
        for (const QByteArray &line : out.split('\n')) {
            const QStringList f = QString::fromUtf8(line).split(QLatin1Char('\x1f'));
            if (f.size() < 2)
                continue;
            const QString an = f.at(0).trimmed();
            const QString ae = f.at(1).trimmed();
            if (an.isEmpty())
                continue;
            const QString display = QStringLiteral("%1 <%2>").arg(an, ae);
            if (!seen.contains(display)) {
                seen.insert(display);
                read.identities << display;
            }
            if (an == oldName && !ae.isEmpty())
                ++emailHits[ae];
        }
        int best = 0;
        for (auto it = emailHits.constBegin(); it != emailHits.constEnd(); ++it) {
            if (it.value() > best) {
                best = it.value();
                read.primaryEmail = it.key();
            }
        }
    }
    read.identities.sort(Qt::CaseInsensitive);
    return read;
}

void MainWindow::continueReassignContributorIdentity(const QString &dir,
                                                     const QString &oldName,
                                                     const QStringList &identities,
                                                     const QString &primaryEmail)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Reassign attribution"));
    auto *form = new QVBoxLayout(&dialog);

    auto *intro = new QLabel(
        QStringLiteral("Reassign every commit authored by <b>%1</b> to a different "
                       "name and email.")
            .arg(oldName.toHtmlEscaped()));
    intro->setWordWrap(true);
    form->addWidget(intro);

    auto *pick = new QComboBox;
    pick->addItem(QString::fromUtf8("\xE2\x80\x94 copy from an existing identity \xE2\x80\x94"));
    for (const QString &id : std::as_const(identities))
        pick->addItem(id);
    form->addWidget(pick);

    auto *grid = new QFormLayout;
    auto *nameEdit = new QLineEdit(oldName);
    auto *emailEdit = new QLineEdit(primaryEmail);
    grid->addRow(QStringLiteral("New name"), nameEdit);
    grid->addRow(QStringLiteral("New email"), emailEdit);
    form->addLayout(grid);

    connect(pick, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [pick, nameEdit, emailEdit](int i) {
                if (i <= 0)
                    return;
                const QString text = pick->itemText(i);
                const int lt = text.lastIndexOf(QLatin1Char('<'));
                const int gt = text.lastIndexOf(QLatin1Char('>'));
                if (lt > 0 && gt > lt) {
                    nameEdit->setText(text.left(lt).trimmed());
                    emailEdit->setText(text.mid(lt + 1, gt - lt - 1).trimmed());
                }
            });

    auto *warn = new QLabel(
        QStringLiteral("<span style='color:#d29922'>This rewrites history on all "
                       "branches and changes commit hashes. It cannot be undone.</span>"));
    warn->setWordWrap(true);
    warn->setTextFormat(Qt::RichText);
    form->addWidget(warn);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Reassign"));
    form->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString newName = nameEdit->text().trimmed();
    const QString newEmail = emailEdit->text().trimmed();
    if (newName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Reassign attribution"),
                             QStringLiteral("The new name cannot be empty."));
        return;
    }
    if (newName == oldName && newEmail == primaryEmail) {
        QMessageBox::information(this, QStringLiteral("Reassign attribution"),
                                 QStringLiteral("Nothing to change."));
        return;
    }

    if (QMessageBox::question(
            this, QStringLiteral("Reassign attribution"),
            QStringLiteral("Re-attribute all commits by \"%1\" to \"%2 <%3>\"?\n\n"
                           "This rewrites every branch and changes commit hashes "
                           "from the first affected commit onward.")
                .arg(oldName, newName, newEmail),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
        return;

    auto shq = [](const QString &s) {
        QString e = s;
        e.replace(QLatin1Char('\''), QLatin1String("'\\''"));
        return QLatin1Char('\'') + e + QLatin1Char('\'');
    };
    const QString script =
        QStringLiteral(
            "OLD=%1\n"
            "if [ \"$GIT_AUTHOR_NAME\" = \"$OLD\" ]; then "
            "export GIT_AUTHOR_NAME=%2; export GIT_AUTHOR_EMAIL=%3; fi\n"
            "if [ \"$GIT_COMMITTER_NAME\" = \"$OLD\" ]; then "
            "export GIT_COMMITTER_NAME=%2; export GIT_COMMITTER_EMAIL=%3; fi\n")
            .arg(shq(oldName), shq(newName), shq(newEmail));

    if (m_reassignIdentityRunning)
        return;
    m_reassignIdentityRunning = true;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    runOffThread<std::pair<bool, QString>>(
        [dir, script] { return rewriteContributorIdentity(dir, script); },
        [this, oldName, newName, newEmail](std::pair<bool, QString> rewritten) {
            m_reassignIdentityRunning = false;
            QApplication::restoreOverrideCursor();
            if (!rewritten.first) {
                QMessageBox::critical(
                    this, QStringLiteral("Reassign attribution"),
                    QStringLiteral("History rewrite failed: %1")
                        .arg(rewritten.second.isEmpty() ? QStringLiteral("git failed")
                                                        : rewritten.second.left(400)));
                return;
            }
            QMessageBox::information(
                this, QStringLiteral("Reassign attribution"),
                QStringLiteral("Re-attributed commits by \"%1\" to \"%2 <%3>\".\n\n"
                               "Commit hashes changed; re-publish or re-sync mirrors "
                               "so peers pick up the rewrite.")
                    .arg(oldName, newName, newEmail));
            loadCommits();
        });
}

// code-health: allow(blocking-wait) runs only on a runOffThread worker behind the confirmed rewrite; 300 s cap per git step
std::pair<bool, QString> MainWindow::rewriteContributorIdentity(const QString &dir,
                                                                const QString &script)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("FILTER_BRANCH_SQUELCH_WARNING"), QStringLiteral("1"));
    const auto runLong = [&env, &dir](const QStringList &args, QString *err) {
        QProcess p;
        p.setProcessEnvironment(env);
        p.start("git", QStringList{"-C", dir} + args);
        if (!p.waitForFinished(300000)) {
            p.kill();
            p.waitForFinished(1000);
            if (err)
                *err = QStringLiteral("git timed out");
            return false;
        }
        if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
            if (err)
                *err = QString::fromUtf8(p.readAllStandardError()).trimmed();
            return false;
        }
        return true;
    };
    QString err;
    if (!runLong({"filter-branch", "--force", "--env-filter", script,
                  "--tag-name-filter", "cat", "--", "--all"},
                 &err))
        return {false, err};
    QByteArray origRefs;
    if (runGitCapture(dir, {"for-each-ref", "--format=%(refname)", "refs/original/"},
                      &origRefs, nullptr)) {
        for (const QByteArray &ref : origRefs.split('\n')) {
            const QString r = QString::fromUtf8(ref).trimmed();
            if (!r.isEmpty())
                runGitCapture(dir, {"update-ref", "-d", r}, nullptr, nullptr);
        }
    }
    return {true, QString()};
}

static QString repoInfoJsonWritePath(const QString &localPath)
{
    return QDir(localPath).filePath(QStringLiteral(".forkmesh/info.json"));
}

static QString repoInfoJsonReadPath(const QString &localPath)
{
    const QString preferred = repoInfoJsonWritePath(localPath);
    if (QFileInfo::exists(preferred))
        return preferred;
    const QString legacy = QDir(localPath).filePath(QStringLiteral("info.json"));
    return QFileInfo::exists(legacy) ? legacy : preferred;
}

void MainWindow::loadRepoInfo()
{
    m_repoInfo = RepoInfo();
    ++m_repoInfoLoadGen;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);

    const QString local = QDir(repo.localPath).filePath(kRepoInfoPath);
    if (!repo.localPath.isEmpty() && QFileInfo::exists(local)) {
        QFile file(local);
        if (file.open(QIODevice::ReadOnly))
            applyRepoInfoJson(file.readAll());
        return;
    }
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return;
    const QString ref = currentRef();
    const quint64 generation = m_repoInfoLoadGen;
    const int repoIndex = m_repoDetailIndex;
    const QString fallback = repoDefaultBranchFast();
    runOffThread<QByteArray>(
        [dir, ref] {
            QByteArray raw;
            runGitCapture(dir, {"show", ref + ":" + kRepoInfoPath}, &raw, nullptr);
            return raw;
        },
        [this, generation, repoIndex, dir, fallback](QByteArray raw) {
            if (generation != m_repoInfoLoadGen || repoIndex != m_repoDetailIndex ||
                dir != repoGitDir() || raw.isEmpty())
                return;
            applyRepoInfoJson(raw);
            const QString wanted = m_repoInfo.defaultBranch.trimmed();
            if (!wanted.isEmpty() && wanted != fallback &&
                (m_repoBranch == fallback || m_repoBranch.isEmpty()))
                setRepoBranch(wanted);
            refreshRepoInfoViews();
        });
}

void MainWindow::applyRepoInfoJson(const QByteArray &raw)
{
    if (raw.isEmpty())
        return;
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    m_repoInfo.about = obj.value("about").toString();
    m_repoInfo.website = obj.value("website").toString();
    m_repoInfo.language = obj.value("language").toString();
    m_repoInfo.defaultBranch = obj.value("defaultBranch").toString();
    m_repoInfo.forks = obj.value("forks").toInt(0);
    m_repoInfo.stars = obj.value("stars").toInt(0);
    m_repoInfo.mirrors = obj.value("mirrors").toInt(1);
    for (const QJsonValue &v : obj.value("topics").toArray())
        m_repoInfo.topics << v.toString();
    for (const QJsonValue &v : obj.value("contributors").toArray()) {
        const QJsonObject c = v.toObject();
        if (!c.value("name").toString().isEmpty())
            m_repoInfo.contributorAvatars.insert(c.value("name").toString(),
                                                 c.value("avatar").toString());
    }
}

void MainWindow::refreshRepoInfoViews()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    if (auto *fork = dynamic_cast<VerticalIconButton *>(m_forkButton))
        fork->setBadgeCount(m_repoInfo.forks);
    if (auto *mirror = dynamic_cast<VerticalIconButton *>(m_mirrorButton);
        mirror && !m_repositories.at(m_repoDetailIndex).previewOnly)
        mirror->setBadgeCount(qMax(1, m_repoInfo.mirrors));
    m_defaultBranchFastCache.clear();
    updateRepoDetailStatus();
    updateNextAboutInfo(nextAboutSidebarIn(m_filesStack),
                        m_repositories.at(m_repoDetailIndex), m_repoInfo);
    updateRepoActionMenus();
    refreshRepoSettings();
    loadBranchesAndTags();
}

void MainWindow::editRepoAbout()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    if (!repoHasWorkingTree()) {
        QMessageBox::information(
            this, "Edit repository details",
            "Open a repository with a local working copy to edit its About details.");
        return;
    }

    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    QDialog dialog(this);
    dialog.setWindowTitle("Edit repository details");

    auto *descriptionEdit = new QPlainTextEdit(&dialog);
    descriptionEdit->setPlaceholderText("Short repository description");
    descriptionEdit->setPlainText(m_repoInfo.about.isEmpty() ? repo.description
                                                             : m_repoInfo.about);
    descriptionEdit->setMaximumHeight(96);

    auto *websiteEdit = new QLineEdit(&dialog);
    websiteEdit->setPlaceholderText("https://example.com");
    websiteEdit->setText(m_repoInfo.website);

    auto *form = new QFormLayout;
    form->addRow("Description", descriptionEdit);
    form->addRow("Website", websiteEdit);

    auto *fediGroup = new QGroupBox("ActivityPub federation", &dialog);
    auto *fediLayout = new QVBoxLayout(fediGroup);
    auto *federateBox = new QCheckBox(
        "Federate this repository (fediverse actor and handle)", fediGroup);
    auto *broadcastBox = new QCheckBox(
        "Post new issues, pull requests, discussions and releases to followers",
        fediGroup);
    auto *commentsBox = new QCheckBox(
        "Accept fediverse replies as federated comments", fediGroup);
    for (QCheckBox *box : {federateBox, broadcastBox, commentsBox}) {
        box->setChecked(true);
        box->setEnabled(false);
        fediLayout->addWidget(box);
    }
    auto fediLoaded = std::make_shared<bool>(false);
    if (repo.publishToNetwork) {
        const QString owner = repoSegment(repo.owner, QStringLiteral("owner"));
        const QString name =
            repoSegment(repo.name, QStringLiteral("repository"));
        QNetworkReply *reply = m_networkAccess->get(
            QNetworkRequest(repoAboutApiUrl(owner, name)));
        QPointer<QCheckBox> fedPtr(federateBox);
        QPointer<QCheckBox> broadPtr(broadcastBox);
        QPointer<QCheckBox> comPtr(commentsBox);
        connect(reply, &QNetworkReply::finished, this,
                [reply, fedPtr, broadPtr, comPtr, fediLoaded] {
                    reply->deleteLater();
                    if (!fedPtr || !broadPtr || !comPtr ||
                        reply->error() != QNetworkReply::NoError)
                        return;
                    const QJsonObject settings =
                        QJsonDocument::fromJson(reply->readAll())
                            .object()
                            .value(QStringLiteral("fediverse"))
                            .toObject()
                            .value(QStringLiteral("settings"))
                            .toObject();
                    if (settings.isEmpty())
                        return;
                    fedPtr->setChecked(
                        settings.value(QStringLiteral("federate")).toBool(true));
                    broadPtr->setChecked(
                        settings.value(QStringLiteral("broadcastEvents"))
                            .toBool(true));
                    comPtr->setChecked(
                        settings.value(QStringLiteral("acceptComments"))
                            .toBool(true));
                    for (QCheckBox *box :
                         {fedPtr.data(), broadPtr.data(), comPtr.data()})
                        box->setEnabled(true);
                    *fediLoaded = true;
                });
    } else {
        fediGroup->setToolTip(
            "Publish this repository to the network to give it a fediverse "
            "presence.");
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save |
                                         QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, &dialog,
                                                         descriptionEdit,
                                                         websiteEdit,
                                                         federateBox,
                                                         broadcastBox,
                                                         commentsBox,
                                                         fediLoaded] {
        QString error;
        if (!saveRepoAboutMetadata(descriptionEdit->toPlainText(),
                                   websiteEdit->text(), &error)) {
            QMessageBox::warning(&dialog, "Edit repository details",
                                 error.isEmpty()
                                     ? QStringLiteral("Could not save details.")
                                     : error);
            return;
        }
        if (*fediLoaded && m_repoDetailIndex >= 0 &&
            m_repoDetailIndex < m_repositories.size()) {
            const RepositoryRecord &current =
                m_repositories.at(m_repoDetailIndex);
            saveRepoFediverseSettings(
                repoSegment(current.owner, QStringLiteral("owner")),
                repoSegment(current.name, QStringLiteral("repository")),
                federateBox->isChecked(), broadcastBox->isChecked(),
                commentsBox->isChecked());
        }
        dialog.accept();
    });

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);
    layout->addLayout(form);
    layout->addWidget(fediGroup);
    layout->addWidget(buttons);
    dialog.resize(520, 340);
    dialog.exec();
}

QUrl MainWindow::repoAboutApiUrl(const QString &owner, const QString &name) const
{
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/repo/%1/%2/about").arg(owner, name));
    return url;
}

void MainWindow::saveRepoFediverseSettings(const QString &owner,
                                           const QString &name,
                                           bool federate, bool broadcastEvents,
                                           bool acceptComments)
{
    if (owner.isEmpty() || name.isEmpty())
        return;
    if (!hasOwnerSigningCapability(owner))
        return;
    if (!m_profileIdentity.isValid() && !m_profileIdentity.load()) {
        logSystem("Fediverse: could not load identity to save federation "
                  "settings.");
        return;
    }
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-repo-about-v1\n" + owner + "\n" + name + "\n" + ts).toUtf8();
    QJsonObject settings;
    settings.insert(QStringLiteral("federate"), federate);
    settings.insert(QStringLiteral("broadcastEvents"), broadcastEvents);
    settings.insert(QStringLiteral("acceptComments"), acceptComments);
    QJsonObject body;
    body.insert(QStringLiteral("ts"), ts);
    body.insert(QStringLiteral("ownerSig"),
                m_profileIdentity.signData(canonical));
    body.insert(QStringLiteral("fediverse"), settings);
    QNetworkRequest request(repoAboutApiUrl(owner, name));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, owner, name] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
            logSystem(QStringLiteral(
                          "Fediverse: saved federation settings for %1/%2.")
                          .arg(owner, name));
        else
            logSystem(QStringLiteral("Fediverse: could not save federation "
                                     "settings for %1/%2 (%3).")
                          .arg(owner, name, reply->errorString()));
    });
}

bool MainWindow::saveRepoAboutMetadata(const QString &about,
                                       const QString &websiteInput,
                                       QString *error)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        if (error)
            *error = QStringLiteral("No repository is open.");
        return false;
    }
    if (!repoHasWorkingTree()) {
        if (error)
            *error = QStringLiteral(
                "Open a repository with a local working copy to edit its About details.");
        return false;
    }
    if (!applyRepoAboutMetadataAt(m_repoDetailIndex, about, websiteInput, error))
        return false;
    setRepoDetailNotice(QStringLiteral("Updated repository details."));
    return true;
}

bool MainWindow::applyRepoAboutMetadataAt(int index, const QString &about,
                                          const QString &websiteInput,
                                          QString *error)
{
    if (index < 0 || index >= m_repositories.size()) {
        if (error)
            *error = QStringLiteral("Unknown repository.");
        return false;
    }
    if (m_repositories.at(index).localPath.isEmpty() ||
        !QDir(m_repositories.at(index).localPath)
             .exists(QStringLiteral(".git"))) {
        if (error)
            *error = QStringLiteral(
                "This repository has no local working copy on this node.");
        return false;
    }

    const QString website = normalizedRepoWebsite(websiteInput, error);
    if (!websiteInput.trimmed().isEmpty() && website.isEmpty())
        return false;

    RepositoryRecord &repo = m_repositories[index];
    const QDir repoDir(repo.localPath);
    const QString infoPath = repoDir.filePath(kRepoInfoPath);
    QJsonObject obj;
    if (QFileInfo::exists(infoPath)) {
        QFile file(infoPath);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("Could not read .forkmesh/info.json.");
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error)
                *error = QStringLiteral(".forkmesh/info.json is not valid JSON.");
            return false;
        }
        obj = doc.object();
    }

    const QString aboutText = about.trimmed();
    if (aboutText.isEmpty())
        obj.remove(QStringLiteral("about"));
    else
        obj.insert(QStringLiteral("about"), aboutText);
    if (website.isEmpty())
        obj.remove(QStringLiteral("website"));
    else
        obj.insert(QStringLiteral("website"), website);

    const QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (!repoDir.mkpath(QStringLiteral(".forkmesh"))) {
        if (error)
            *error = QStringLiteral("Could not create .forkmesh directory.");
        return false;
    }
    QSaveFile file(infoPath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Could not write .forkmesh/info.json.");
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("Could not save .forkmesh/info.json.");
        return false;
    }

    const QString legacyPath = QDir(repo.localPath).filePath("info.json");
    if (legacyPath != infoPath && QFileInfo::exists(legacyPath))
        QFile::remove(legacyPath);

    repo.description = aboutText;
    saveRepositories();
    if (index == m_repoDetailIndex) {
        m_repoInfo.about = aboutText;
        m_repoInfo.website = website;
        updateNextAboutInfo(nextAboutSidebarIn(m_filesStack), repo, m_repoInfo);
    }
    refreshRepositoryList();
    if (repo.publishToNetwork)
        refreshRepositoryMirror(index);
    logSystem(QStringLiteral("Updated About details for %1/%2.")
                  .arg(repo.owner, repo.name));
    return true;
}

void MainWindow::setRepoBranch(const QString &branch, bool loadContent)
{
    m_repoBranch = branch;
    if (m_branchButton)
        m_branchButton->setText(branch);
    updateCommitsBranchButtonLabel();
    updateFooterCommitInfo(); // the strip's commit line follows the browsed branch
    if (!loadContent)
        return;
    loadRepoOverview(QString());
    loadCommits(); // also refreshes the Insights counts when that tab is on screen
}

QString MainWindow::repoHeadBranch() const
{
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return QString();
    return headBranchFromFile(dir);
}

void MainWindow::updateCommitsBranchButtonLabel()
{
    if (!m_commitsBranchButton)
        return;
    const QString browsed = m_repoBranch.isEmpty() ? repoHeadBranch() : m_repoBranch;
    const QString label =
        browsed.isEmpty() ? QStringLiteral("(detached)") : browsed;
    const QString dir = repoGitDir();
    QString worktreeName;
    if (repoHasWorkingTree()) {
        worktreeName = QFileInfo(QDir(dir).absolutePath()).fileName();
        if (worktreeName.isEmpty())
            worktreeName = QDir::toNativeSeparators(dir);
    }
    // While a range is on screen the button is its left-hand side, which
    // updateCommitsCompareIndicator() names; a branch-list reload refreshing
    // this label must not turn "<branch> -> main" back into the browsed ref.
    const bool comparing = m_commitsStack &&
                           m_commitsStack->currentIndex() == kCommitWorkspaceRangePage &&
                           !m_branchDiffBranch.isEmpty();
    const QString displayLabel =
        comparing ? m_branchDiffBranch
        : worktreeName.isEmpty()
            ? label
            : QString::fromUtf8("%1 \xC2\xB7 %2").arg(label, worktreeName);
    if (auto *elider = dynamic_cast<ElidingPushButton *>(m_commitsBranchButton))
        elider->setFullText(displayLabel);
    else
        m_commitsBranchButton->setText(displayLabel);
    QString toolTip =
        QString::fromUtf8("%1 \xE2\x80\x94 click to open another branch (its "
                          "history here, its diff against the base on the "
                          "right) or create one")
            .arg(label);
    if (!worktreeName.isEmpty())
        toolTip += QStringLiteral("\nCurrent worktree: %1")
                       .arg(QDir::toNativeSeparators(dir));
    m_commitsBranchButton->setToolTip(toolTip);
}

void MainWindow::refreshCommitsBranchButton()
{
    if (!m_commitsBranchButton)
        return;
    updateCommitsBranchButtonLabel();
    const QString browsed = m_repoBranch.isEmpty() ? repoHeadBranch() : m_repoBranch;
    updateCommitsCompareIndicator();

    auto *menu = new QMenu(m_commitsBranchButton);
    QAction *create =
        menu->addAction(QString::fromUtf8("\xEF\xBC\x8B  Create new branch\xE2\x80\xA6"));
    connect(create, &QAction::triggered, this, [this] { createAndCheckoutBranch(); });
    menu->addSeparator();
    const QStringList branches = repoBranches();
    for (const QString &b : branches) {
        QAction *a = menu->addAction(b);
        a->setCheckable(true);
        a->setChecked(b == browsed);
        connect(a, &QAction::triggered, this, [this, b] { switchToBranch(b); });
    }
    if (branches.isEmpty())
        menu->addAction(QStringLiteral("No branches"))->setEnabled(false);
    QMenu *old = m_commitsBranchButton->menu();
    m_commitsBranchButton->setMenu(menu);
    if (old)
        old->deleteLater();
}

void MainWindow::createAndCheckoutBranch()
{
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
                             this, QStringLiteral("Create new branch"),
                             QStringLiteral("New branch name:"), QLineEdit::Normal,
                             QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (m_createBranchRunning)
        return;
    m_createBranchRunning = true;
    runOffThread<std::pair<bool, GitCaptureResult>>(
        [dir, name] {
            QByteArray st;
            runGitCapture(dir, {"status", "--porcelain"}, &st, nullptr);
            return std::pair<bool, GitCaptureResult>{
                !st.trimmed().isEmpty(), readGit(dir, {"checkout", "-b", name})};
        },
        [this, dir, name](std::pair<bool, GitCaptureResult> created) {
            m_createBranchRunning = false;
            const bool carryingChanges = created.first;
            if (!created.second.ok) {
                const QString message = QStringLiteral("Could not create %1: %2")
                                            .arg(name, created.second.error.left(200));
                if (dir == repoGitDir())
                    setRepoDetailNotice(message, true);
                else
                    flashMessage(message, true);
                return;
            }
            logSystem(QStringLiteral("Git: created and checked out %1.").arg(name));
            m_branchesCache.clear();
            if (dir != repoGitDir())
                return;
            setRepoDetailNotice(
                carryingChanges
                    ? QStringLiteral("Created and switched to %1 with your current changes.")
                          .arg(name)
                    : QStringLiteral("Created and switched to %1.").arg(name));
            m_branchesCache.clear(); // new branch — bust the cache so it appears in the menu
            setRepoBranch(name);
            refreshCommitsBranchButton();
        });
}

static bool isPullMetadataBranch(const QString &branch)
{
    return branch == QLatin1String("forkmesh/pulls") ||
           branch.startsWith(QLatin1String("forkmesh/pull/"));
}

static QString repoBranchListStamp(const QString &dir)
{
    const QString gitDir = resolveGitDirPath(dir);
    const QString commonDir = gitCommonDirPath(dir);
    if (gitDir.isEmpty() || commonDir.isEmpty() ||
        QFileInfo::exists(commonDir + QStringLiteral("/reftable")))
        return QString();
    const QString commonStamp = gitRefsDirSignature(commonDir);
    if (QDir(gitDir).absolutePath() == QDir(commonDir).absolutePath())
        return commonStamp;
    return gitRefsDirSignature(gitDir) + QLatin1Char('|') + commonStamp;
}

static bool cachedRepoBranchListing(const QString &dir, QByteArray *out)
{
    struct Entry {
        QString stamp;
        QByteArray listing;
    };
    static QHash<QString, Entry> cache;
    static QMutex mutex;
    const QString key = QDir(dir).absolutePath();
    const QString stamp = repoBranchListStamp(dir);
    if (!stamp.isEmpty()) {
        QMutexLocker lock(&mutex);
        const auto cached = cache.constFind(key);
        if (cached != cache.constEnd() && cached->stamp == stamp) {
            *out = cached->listing;
            return true;
        }
    }
    QByteArray listing;
    if (!runGitCapture(dir,
                       {"branch", "--sort=-committerdate",
                        "--format=%(refname:short)"},
                       &listing, nullptr))
        return false;
    *out = listing;
    if (!stamp.isEmpty()) {
        QMutexLocker lock(&mutex);
        if (cache.size() > 64)
            cache.clear();
        cache.insert(key, Entry{stamp, listing});
    }
    return true;
}

QStringList MainWindow::listRepoBranches(const QString &dir)
{
    QStringList branches;
    QByteArray out;
    if (!dir.isEmpty() && cachedRepoBranchListing(dir, &out)) {
        for (const QString &line : QString::fromUtf8(out).split('\n')) {
            const QString branch = line.trimmed();
            if (!branch.isEmpty() && !isPullMetadataBranch(branch) &&
                !branches.contains(branch))
                branches.append(branch);
        }
    }
    return branches;
}

MainWindow::BranchRefFiles MainWindow::readBranchRefFiles(const QString &dir)
{
    BranchRefFiles files;
    const QString common = dir.isEmpty() ? QString() : gitCommonDirPath(dir);
    if (common.isEmpty() ||
        QFileInfo(common + QStringLiteral("/reftable")).isDir())
        return files;
    struct LooseHead {
        QString name;
        qint64 modifiedMs = 0;
    };
    QList<LooseHead> loose;
    QSet<QString> seen;
    qint64 newest = 0;
    const QString headsRoot = common + QStringLiteral("/refs/heads");
    const QDir heads(headsRoot);
    QDirIterator it(headsRoot, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString name = heads.relativeFilePath(it.filePath());
        const qint64 modified = it.fileInfo().lastModified().toMSecsSinceEpoch();
        newest = qMax(newest, modified);
        if (name.endsWith(QLatin1String(".lock")) || seen.contains(name))
            continue;
        seen.insert(name);
        loose.append({name, modified});
    }
    std::stable_sort(loose.begin(), loose.end(),
                     [](const LooseHead &a, const LooseHead &b) {
                         return a.modifiedMs > b.modifiedMs;
                     });
    for (const LooseHead &head : std::as_const(loose))
        files.branches.append(head.name);
    const QString packedPath = common + QStringLiteral("/packed-refs");
    QFile packed(packedPath);
    if (packed.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!packed.atEnd()) {
            const QByteArray line = packed.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#') || line.startsWith('^'))
                continue;
            const int space = line.indexOf(' ');
            if (space < 0)
                continue;
            const QByteArray ref = line.mid(space + 1).trimmed();
            if (!ref.startsWith("refs/heads/"))
                continue;
            const QString name = QString::fromUtf8(ref.mid(11));
            if (name.isEmpty() || seen.contains(name))
                continue;
            seen.insert(name);
            files.branches.append(name);
        }
    }
    files.branches.removeIf(isPullMetadataBranch);
    files.signature =
        QString::number(newest) + QLatin1Char(':') +
        QString::number(seen.size()) + QLatin1Char(':') +
        QString::number(
            QFileInfo(packedPath).lastModified().toMSecsSinceEpoch()) +
        QLatin1Char(':') +
        QString::number(QFileInfo(headsRoot).lastModified().toMSecsSinceEpoch());
    files.readable = true;
    return files;
}

QStringList MainWindow::orderBranchesLike(const QStringList &onDisk,
                                          const QStringList &gitOrder)
{
    if (gitOrder.isEmpty())
        return onDisk;
    const QSet<QString> present(onDisk.cbegin(), onDisk.cend());
    const QSet<QString> known(gitOrder.cbegin(), gitOrder.cend());
    QStringList ordered;
    ordered.reserve(onDisk.size());
    for (const QString &branch : onDisk) {
        if (!known.contains(branch))
            ordered.append(branch);
    }
    for (const QString &branch : gitOrder) {
        if (present.contains(branch))
            ordered.append(branch);
    }
    return ordered;
}

QStringList MainWindow::repoBranches() const
{
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return QStringList();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (dir == m_branchesCacheDir && !m_branchesCache.isEmpty() &&
        now - m_branchesCacheTime < 5) {
        return m_branchesCache;
    }
    const BranchRefFiles files = readBranchRefFiles(dir);
    const bool orderKnown = dir == m_branchesGitOrderDir;
    QStringList branches;
    if (files.readable)
        branches = orderBranchesLike(
            files.branches, orderKnown ? m_branchesGitOrder : QStringList());
    else if (orderKnown)
        branches = m_branchesGitOrder;
    else if (dir == m_branchesCacheDir)
        branches = m_branchesCache;
    if (!orderKnown || !files.readable ||
        files.signature != m_branchesGitOrderSig)
        requestRepoBranchOrder(dir);
    if (branches.isEmpty())
        return branches;
    m_branchesCacheDir = dir;
    m_branchesCache = branches;
    m_branchesCacheTime = now;
    return branches;
}

void MainWindow::requestRepoBranchOrder(const QString &dir) const
{
    if (dir.isEmpty())
        return;
    if (m_branchOrderLoading) {
        m_branchOrderPendingDir = dir;
        return;
    }
    m_branchOrderLoading = true;
    m_branchOrderPendingDir.clear();
    auto *self = const_cast<MainWindow *>(this);
    self->runOffThread<BranchOrderLoad>(
        [dir] {
            BranchOrderLoad load;
            load.dir = dir;
            load.signature = readBranchRefFiles(dir).signature;
            load.branches = listRepoBranches(dir);
            return load;
        },
        [self](BranchOrderLoad load) {
            self->m_branchOrderLoading = false;
            self->applyRepoBranchOrder(load);
            const QString pending = self->m_branchOrderPendingDir;
            self->m_branchOrderPendingDir.clear();
            if (!pending.isEmpty() && pending == self->repoGitDir() &&
                (pending != load.dir ||
                 readBranchRefFiles(pending).signature != load.signature))
                self->requestRepoBranchOrder(pending);
        });
}

void MainWindow::applyRepoBranchOrder(const BranchOrderLoad &load)
{
    if (load.dir.isEmpty() || load.dir != repoGitDir())
        return;
    m_branchesGitOrderDir = load.dir;
    m_branchesGitOrder = load.branches;
    m_branchesGitOrderSig = load.signature;
    const BranchRefFiles files = readBranchRefFiles(load.dir);
    const QStringList branches =
        files.readable ? orderBranchesLike(files.branches, load.branches)
                       : load.branches;
    if (branches.isEmpty())
        return;
    const bool callersSawNothing =
        m_branchesCacheDir != load.dir || m_branchesCache.isEmpty();
    m_branchesCacheDir = load.dir;
    m_branchesCache = branches;
    m_branchesCacheTime = QDateTime::currentSecsSinceEpoch();
    if (!callersSawNothing || files.readable)
        return;
    m_defaultBranchFastCache.clear();
    refreshCommitsBranchButton();
}

QString MainWindow::chooseDefaultBranch(const QStringList &branches,
                                        const QString &configured,
                                        const QString &dir,
                                        const QString &checkedOut,
                                        bool askGit)
{
    if (!configured.isEmpty() && branches.contains(configured))
        return configured;
    if (branches.contains(QStringLiteral("main")))
        return QStringLiteral("main");
    if (branches.contains(QStringLiteral("master")))
        return QStringLiteral("master");

    if (!dir.isEmpty()) {
        QString branch = headBranchFromFile(dir);
        if (branch.isEmpty() && askGit) {
            QByteArray head;
            if (runGitCapture(dir, {"symbolic-ref", "--short", "HEAD"}, &head,
                              nullptr))
                branch = QString::fromUtf8(head).trimmed();
        }
        if (!branch.isEmpty() && branches.contains(branch))
            return branch;
    }
    if (!checkedOut.isEmpty() && branches.contains(checkedOut))
        return checkedOut;
    return branches.isEmpty() ? QString() : branches.first();
}

QString MainWindow::repoDefaultBranch(const QStringList &branches) const
{
    return chooseDefaultBranch(branches, m_repoInfo.defaultBranch.trimmed(),
                               repoGitDir(), m_repoBranch, false);
}

QString MainWindow::repoDefaultBranchFast() const
{
    const QString configured = m_repoInfo.defaultBranch.trimmed();
    if (!configured.isEmpty())
        return configured;
    const QString dir = repoGitDir();
    if (dir.isEmpty())
        return QString();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    auto stamp = [&dir](const QString &leaf) {
        return QString::number(QFileInfo(QDir(dir).filePath(leaf))
                                   .lastModified()
                                   .toMSecsSinceEpoch());
    };
    const QString fingerprint = stamp(QStringLiteral("refs/heads")) +
                                QLatin1Char('|') +
                                stamp(QStringLiteral("packed-refs")) +
                                QLatin1Char('|') + stamp(QStringLiteral("HEAD"));
    if (dir == m_defaultBranchFastCacheDir &&
        !m_defaultBranchFastCache.isEmpty() &&
        (now - m_defaultBranchFastCacheTime < 5 ||
         fingerprint == m_defaultBranchFastCacheSig)) {
        return m_defaultBranchFastCache;
    }
    const BranchRefFiles files = readBranchRefFiles(dir);
    QStringList branches = files.readable ? files.branches : repoBranches();
    std::sort(branches.begin(), branches.end());
    const QString base = repoDefaultBranch(branches);
    m_defaultBranchFastCacheDir = dir;
    m_defaultBranchFastCache = base;
    m_defaultBranchFastCacheTime = now;
    m_defaultBranchFastCacheSig = fingerprint;
    return base;
}

void MainWindow::loadBranchesAndTags()
{
    if (deferUiRefresh(UiRefreshBranches))
        return;
    if (m_repoBranch.isEmpty())
        m_repoBranch = repoDefaultBranchFast();
    if (m_branchButton) {
        m_branchButton->setText(
            m_repoBranch.isEmpty() ? QStringLiteral("HEAD") : m_repoBranch);
        m_branchButton->setToolTip(QStringLiteral("Switch branch"));
    }
    updateCommitsBranchButtonLabel();

    if (m_repoDetailStack) {
        const int current = m_repoDetailStack->currentIndex();
        if (current == 0 && m_overviewBodyStack &&
            m_overviewBodyStack->currentIndex() == 2)
            loadBranchesPanel();
        else if (current == 0 && m_overviewBodyStack &&
                 m_overviewBodyStack->currentIndex() == 3)
            loadWorktreesPanel();
        else if (current == m_releasesTabIndex)
            loadReleasesPanel();
    }

    if (m_branchesTagsLoading) {
        m_branchesTagsReloadQueued = true;
        return;
    }
    m_branchesTagsLoading = true;

    BranchesTagsSnapshot snap;
    snap.dir = repoGitDir();
    snap.localPath =
        (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size())
            ? m_repositories.at(m_repoDetailIndex).localPath
            : QString();
    snap.configuredDefault = m_repoInfo.defaultBranch.trimmed();
    snap.checkedOut = m_repoBranch;

    runOffThread<BranchesTagsSnapshot>(
        [snap] { return readBranchesTagsGit(snap); },
        [this](BranchesTagsSnapshot loaded) {
            m_branchesTagsLoading = false;
            if (loaded.dir == repoGitDir())
                applyBranchesTags(loaded);
            if (m_branchesTagsReloadQueued) {
                m_branchesTagsReloadQueued = false;
                loadBranchesAndTags();
            }
        });
}

MainWindow::BranchesTagsSnapshot
MainWindow::readBranchesTagsGit(BranchesTagsSnapshot snap)
{
    const QString &dir = snap.dir;
    if (dir.isEmpty())
        return snap;

    snap.branchRefsSig = readBranchRefFiles(dir).signature;
    snap.branches = listRepoBranches(dir);
    snap.base = chooseDefaultBranch(snap.branches, snap.configuredDefault, dir,
                                    snap.checkedOut);
    // A browsed ref missing from the branch list is usually one just deleted,
    // but tags and raw commits are browsable too — ask git before saying gone.
    snap.checkedOutGone = !snap.checkedOut.isEmpty() &&
                          !snap.branches.contains(snap.checkedOut) &&
                          !gitRefResolves(dir, snap.checkedOut);
    if (snap.checkedOutGone) {
        snap.recovery.dir = dir;
        snap.recovery.gone = snap.checkedOut;
        snap.recovery.goneResolves = false;
        snap.recovery.base = snap.base;
        snap.recovery.baseResolves = !snap.base.isEmpty() &&
                                     snap.base != snap.checkedOut &&
                                     gitRefResolves(dir, snap.base);
    }

    QByteArray head;
    if (runGitCapture(dir, {"rev-parse", "--abbrev-ref", "HEAD"}, &head, nullptr)) {
        const QString b = QString::fromUtf8(head).trimmed();
        if (b != QLatin1String("HEAD")) // "HEAD" => detached
            snap.head = b;
    }

    QByteArray wtOut;
    if (!snap.localPath.isEmpty() &&
        cachedWorktreeListPorcelain(snap.localPath, &wtOut)) {
        QString curBranch;
        auto flush = [&] {
            if (curBranch != QLatin1String("forkmesh/pulls"))
                ++snap.worktreeCount;
        };
        bool inEntry = false;
        for (const QString &raw : QString::fromUtf8(wtOut).split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed();
            if (line.isEmpty()) {
                if (inEntry) flush();
                inEntry = false;
                curBranch.clear();
                continue;
            }
            if (line.startsWith(QLatin1String("worktree ")))
                inEntry = true;
            else if (line.startsWith(QLatin1String("branch ")))
                curBranch = line.mid(7).replace(QLatin1String("refs/heads/"),
                                                QString());
        }
        if (inEntry) flush();
    }

    QByteArray remoteOut;
    if (cachedRepoRemotesRaw(dir, &remoteOut)) {
        for (const QString &raw :
             QString::fromUtf8(remoteOut).split('\n', Qt::SkipEmptyParts)) {
            const int tab = raw.indexOf('\t');
            if (tab <= 0)
                continue;
            const QString name = raw.left(tab).trimmed();
            QString rest = raw.mid(tab + 1).trimmed();
            const bool isPush = rest.endsWith(QLatin1String("(push)"));
            rest.remove(QLatin1String("(fetch)"));
            rest.remove(QLatin1String("(push)"));
            rest = rest.trimmed();
            auto it = std::find_if(snap.remotes.begin(), snap.remotes.end(),
                                   [&name](const BranchesTagsSnapshot::Remote &e) {
                                       return e.name == name;
                                   });
            if (it == snap.remotes.end()) {
                snap.remotes.append({name, QString(), QString()});
                it = snap.remotes.end() - 1;
            }
            if (isPush)
                it->pushUrl = rest;
            else
                it->fetchUrl = rest;
        }
    }

    QByteArray tags;
    if (runGitCapture(dir, {"tag", "--sort=-creatordate"}, &tags, nullptr)) {
        for (const QString &line : QString::fromUtf8(tags).split('\n'))
            if (!line.trimmed().isEmpty())
                ++snap.tagCount;
    }
    return snap;
}

void MainWindow::applyBranchesTags(const BranchesTagsSnapshot &snap)
{
    applyRepoBranchOrder({snap.dir, snap.branchRefsSig, snap.branches});

    if (!snap.base.isEmpty() && m_repoBranch == snap.checkedOut &&
        snap.checkedOut == snap.head && m_repoBranch != snap.base)
        m_repoBranch = snap.base;
    // Every branch deletion refreshes this list, so this is where the screen
    // learns its branch is gone — before the next git pass renders the file
    // list as "fatal: Not a valid object name <branch>". Reload the tree the
    // deleted branch left on screen once the browse has moved.
    if (snap.checkedOutGone && m_repoBranch == snap.checkedOut &&
        applyBrowsedRefRecovery(snap.recovery))
        QTimer::singleShot(0, this, [this] { loadRepoOverview(QString()); });
    if (m_branchButton) {
        const QString label =
            m_repoBranch.isEmpty() ? QStringLiteral("HEAD") : m_repoBranch;
        m_branchButton->setText(label);
        m_branchButton->setToolTip(
            snap.head.isEmpty() || snap.head == m_repoBranch
                ? QStringLiteral("Switch branch")
                : QStringLiteral("Browsing %1 — the checkout is on %2")
                      .arg(label, snap.head));
    }

    if (m_branchesButton) {
        if (auto *b = dynamic_cast<VerticalIconButton *>(m_branchesButton))
            b->setBadgeCount(snap.branches.size());
        m_branchesButton->setEnabled(!snap.branches.isEmpty());
    }
    if (m_repoBranchesTab)
        m_repoBranchesTab->setText(
            QStringLiteral("Branches (%1)").arg(formatCount(snap.branches.size())));

    if (auto *b = dynamic_cast<VerticalIconButton *>(m_worktreesButton))
        b->setBadgeCount(snap.worktreeCount);

    if (m_remotesButton) {
        if (auto *b = dynamic_cast<VerticalIconButton *>(m_remotesButton))
            b->setBadgeCount(snap.remotes.size());
        auto *menu = new QMenu(m_remotesButton);
        menu->setToolTipsVisible(true); // fetch/push URLs hover per row
        for (const BranchesTagsSnapshot::Remote &remote : snap.remotes) {
            const QString url =
                remote.fetchUrl.isEmpty() ? remote.pushUrl : remote.fetchUrl;
            QAction *action = menu->addAction(
                url.isEmpty() ? remote.name
                              : QString::fromUtf8("%1 \xE2\x80\x94 %2")
                                    .arg(remote.name, url),
                this, [this, name = remote.name, url] {
                    if (url.isEmpty())
                        return;
                    QApplication::clipboard()->setText(url);
                    setRepoDetailNotice(
                        QStringLiteral("Copied %1 URL: %2").arg(name, url));
                });
            if (remote.pushUrl.isEmpty() || remote.pushUrl == remote.fetchUrl)
                action->setToolTip(url);
            else
                action->setToolTip(QStringLiteral("fetch %1\npush %2")
                                       .arg(remote.fetchUrl, remote.pushUrl));
        }
        if (snap.remotes.isEmpty())
            menu->addAction("No remotes")->setEnabled(false);
        menu->addSeparator();
        menu->addAction("Manage remotes\xE2\x80\xA6", this, [this] {
            if (m_settingsTabIndex >= 0 && m_repoDetailTabs &&
                m_repoDetailTabs->button(m_settingsTabIndex)) {
                m_repoDetailTabs->button(m_settingsTabIndex)->setChecked(true);
                m_repoDetailStack->setCurrentIndex(m_settingsTabIndex);
                refreshRepoSettings();
            }
        });
        QMenu *old = m_remotesButton->menu();
        m_remotesButton->setMenu(menu);
        if (old)
            old->deleteLater();
    }

    if (m_branchButton) {
        auto *menu = new QMenu(m_branchButton);
        for (const QString &branchName : snap.branches)
            menu->addAction(branchName, this,
                            [this, branchName] { setRepoBranch(branchName); });
        if (menu->isEmpty())
            menu->addAction("No branches")->setEnabled(false);
        QMenu *old = m_branchButton->menu();
        m_branchButton->setMenu(menu);
        if (old)
            old->deleteLater();
    }

    if (auto *b = dynamic_cast<VerticalIconButton *>(m_tagsButton))
        b->setBadgeCount(snap.tagCount);
    if (auto *b = dynamic_cast<VerticalIconButton *>(m_repoReleasesTab))
        b->setBadgeCount(snap.tagCount);
    updateNextAboutReleases(nextAboutSidebarIn(m_filesStack), snap.tagCount);
}

bool MainWindow::repoHasWorkingTree() const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return false;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    return !repo.previewOnly && !repo.localPath.isEmpty() &&
           QDir(repo.localPath).exists(".git");
}





void MainWindow::ensureRepoDetailSectionBuilt()
{
    if (m_repoDetailStack)
        return;

    QWidget *placeholder = m_repoDetailSection;
    QWidget *home = placeholder ? placeholder->parentWidget() : nullptr;
    QLayout *homeLayout = home ? home->layout() : nullptr;
    QWidget *detail = buildRepoDetailSection();
    if (homeLayout && placeholder)
        homeLayout->replaceWidget(placeholder, detail);
    else if (homeLayout)
        homeLayout->addWidget(detail);
    m_repoDetailSection = detail;
    if (placeholder)
        placeholder->deleteLater();
}

void MainWindow::ensureRepoDetailTabBuilt(int index)
{
    if (!m_repoDetailStack || index < 0 ||
        index >= m_repoDetailStack->count())
        return;
    QWidget *placeholder = m_repoDetailStack->widget(index);
    if (!placeholder ||
        !placeholder->property("forkmeshDeferredRepoTab").toBool())
        return;
    QElapsedTimer buildTimer;
    buildTimer.start();
    const forkmesh::BackgroundScope buildAction(
        QStringLiteral("uibuild"),
        QStringLiteral("build repository tab %1").arg(index),
        forkmesh::ActionTelemetry::Execution::UiDeferred);

    QWidget *page = nullptr;
    if (index == 0)
        page = buildRepoFilesPanel();
    else if (index == 2)
        page = buildIssuesSection();
    else if (index == 3)
        page = buildAgentsTab();
    else if (index == 4)
        page = buildPullsTab();
    else if (index == 5)
        page = buildDiscussionsTab();
    else if (index == 6)
        page = buildRepoActionsTab();
    else if (index == 7)
        page = buildRepoSecurityTab();
    else if (index == 8)
        page = buildRepoQualityTab();
    else if (index == m_insightsTabIndex)
        page = buildInsightsTab();
    else if (index == m_releasesTabIndex)
        page = buildReleasesTab();
    else if (index == m_artifactsTabIndex)
        page = buildArtifactsTab();
    else if (index == m_shortcutsTabIndex)
        page = buildShortcutsTab();
    else if (index == m_settingsTabIndex)
        page = buildRepoSettingsTab();
    else if (index == m_projectsTabIndex)
        page = buildProjectsSection();
    else if (index == m_sizeMapTabIndex)
        page = buildSizeMapTab();
    if (!page)
        return;

    const bool wasCurrent = m_repoDetailStack->currentIndex() == index;
    {
        // Inserting immediately before the current placeholder temporarily
        // increments currentIndex. Without blocking that implementation-detail
        // signal, currentChanged recursively built every following deferred
        // tab (Agents through Insights); the August 30 trace spent 3.9 seconds
        // in that cascade and its regex/text layout. The logical tab never
        // changed, so publish no intermediate index.
        QSignalBlocker stackSignals(m_repoDetailStack);
        m_repoDetailStack->insertWidget(index, page);
        m_repoDetailStack->removeWidget(placeholder);
        if (wasCurrent)
            m_repoDetailStack->setCurrentIndex(index);
    }
    placeholder->deleteLater();

    if (index == 0)
        applyRailOrder();
    syncPackageSurfaces();
    if (index == 2) {
        refreshIssuesRepoCombo();
        // Reporters and assignees wear the pictures from the user directory.
        // Nothing on this tab used to ask for it, so the faces stayed seeded
        // stand-ins until an unrelated fetch happened to land (issue #699).
        // Throttled: a directory pulled in the last five minutes is reused.
        refreshChatUserDirectory();
    }
    forkmesh::PageSwitchTrace::instance().addBuild(buildTimer.elapsed());
    logStartup(QStringLiteral("  repo tab %1 built in %2ms")
                   .arg(index)
                   .arg(buildTimer.elapsed()));
}

void MainWindow::ensureRepoOverviewBodyBuilt(int index)
{
    if (!m_overviewBodyStack || index < 1 || index > 3 ||
        index >= m_overviewBodyStack->count())
        return;
    QWidget *placeholder = m_overviewBodyStack->widget(index);
    if (!placeholder ||
        !placeholder->property("forkmeshDeferredOverviewBody").toBool())
        return;

    QWidget *page = nullptr;
    if (index == 1)
        page = buildRepoCommitsTab();
    else if (index == 2)
        page = buildBranchesTab();
    else
        page = buildWorktreesTab();
    if (!page)
        return;

    const bool wasCurrent = m_overviewBodyStack->currentIndex() == index;
    m_overviewBodyStack->insertWidget(index, page);
    m_overviewBodyStack->removeWidget(placeholder);
    placeholder->deleteLater();
    if (wasCurrent)
        m_overviewBodyStack->setCurrentIndex(index);
}

QWidget *MainWindow::buildRepoDetailSection()
{
    auto *page = new QWidget;

    m_repoHeaderTitle = new QLabel("Repository");
    m_repoHeaderTitle->setObjectName("repoHeaderTitle");
    m_repoHeaderTitle->setTextFormat(Qt::RichText);
    m_repoHeaderTitle->hide();

    auto *notifyButton =
        new VerticalIconButton("Notify", VerticalIconButton::Action);
    m_notifyButton = notifyButton;
    notifyButton->setToolTip("Pings");
    setOcticon(notifyButton, "bell", 16);
    m_forkButton = new VerticalIconButton("Fork", VerticalIconButton::Action);
    m_mirrorButton =
        new VerticalIconButton("Mirror", VerticalIconButton::Action);
    m_sourceButton =
        new VerticalIconButton("Source", VerticalIconButton::Action);
    m_repoOpenButton =
        new VerticalIconButton("Open", VerticalIconButton::Action);
    for (QPushButton *b :
         {static_cast<QPushButton *>(notifyButton), m_forkButton,
          m_mirrorButton, m_sourceButton, m_repoOpenButton}) {
        b->setObjectName("repoActionStack");
        b->setCursor(Qt::PointingHandCursor);
    }
    setOcticon(m_forkButton, "repo-forked", 16);
    setOcticon(m_mirrorButton, "sync", 16);
    setOcticon(m_sourceButton, "code", 16);
    setOcticon(m_repoOpenButton, "link", 16);
    m_repoOpenButton->setToolTip("Open this repository on the web");
    connect(m_repoOpenButton, &QPushButton::clicked, this,
            &MainWindow::openRepositoryWebsite);
    m_mirrorButton->setToolTip("Mirror status and actions");
    m_forkButton->setToolTip("Fork this repository into a local folder");
    m_sourceButton->setToolTip("Download or use this repository's local remote");
    m_mirrorMenu = new QMenu(m_mirrorButton);
    m_sourceMenu = new QMenu(m_sourceButton);
    m_mirrorButton->setMenu(m_mirrorMenu);
    m_sourceButton->setMenu(m_sourceMenu);
    connect(m_forkButton, &QPushButton::clicked, this,
            &MainWindow::forkCurrentRepo);
    connect(m_mirrorMenu, &QMenu::aboutToShow, this,
            &MainWindow::updateRepoActionMenus);
    connect(m_sourceMenu, &QMenu::aboutToShow, this,
            &MainWindow::updateRepoActionMenus);


    m_repoDetailNotice = new QLabel;
    m_repoDetailNotice->setObjectName("repoInlineNotice");
    m_repoDetailNotice->setWordWrap(true);
    m_repoDetailNotice->hide();

    auto *metaBand = new QWidget;
    metaBand->setObjectName("repoDetailMeta");
    m_repoDetailStatus = new QLabel;
    m_repoDetailStatus->setObjectName("statusLine");
    m_repoDetailStatus->setWordWrap(true);
    m_repoDetailStatus->setTextFormat(Qt::RichText);

    auto *metaLayout = new QVBoxLayout(metaBand);
    metaLayout->setContentsMargins(16, 4, 16, 8);
    metaLayout->setSpacing(6);
    metaLayout->addWidget(m_repoDetailStatus);
    metaBand->hide();

    struct TabDef {
        const char *label;
        const char *icon;
    };
    const QList<TabDef> tabs = {{"Code", "code"},
                                {"Commits", "git-branch"},
                                {"Issues", "issue-opened"},
                                {"Agents", "terminal"},
                                {"Pulls", "git-pull-request"},
                                {"Discussions", "comment"},
                                {"Actions", "workflow"},
                                {"Security", "shield-check"},
                                {"Quality", "check-circle"},
                                {"Insights", "graph"},
                                {"Branches", "repo-forked"},
                                {"Worktrees", "worktree"},
                                {"Releases", "tag"},
                                {"", ""},
                                {"Artifacts", "package"},
                                {"Shortcuts", "rocket"},
                                {"Settings", "gear"},
                                {"Projects", "list-unordered"},
                                {"Size map", "pie-chart"}};
    const bool next = Theme::isNext();
    m_repoDetailTabs = new QButtonGroup(this);
    m_repoDetailTabs->setExclusive(true);
    auto *tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(next ? 10 : 12, kRepoTabRowTopInset, next ? 16 : 12, 0);
    tabRow->setSpacing(next ? 0 : 10);
    for (int i = 0; i < tabs.size(); ++i) {
        if (i == 1 || i == 3 || i == 10 || i == 11 ||
            i == kRetiredMirrorNodesDetailTab)
            continue;
        const TabDef tab = tabs.at(i);
        QPushButton *b = new VerticalIconButton(QString::fromLatin1(tab.label),
                                                VerticalIconButton::Tab);
        b->setObjectName("repoTab");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        setOcticon(b, QString::fromLatin1(tab.icon), 16);
        if (i == 0)
            b->setChecked(true);
        if (i == 0)
            m_repoCodeTab = b; // also shows the on-disk repo size
        if (i == 2)
            m_repoIssuesTab = b; // keep a handle for the Issues (N) badge; the
        if (i == 4)
            m_repoPullsTab = b;
        if (i == 5)
            m_repoDiscussionsTab = b;
        if (i == 6)
            m_repoActionsTab = b; // handle for the Actions (N) badge
        if (i == 12)
            m_repoReleasesTab = b; // handle for the Releases (N) badge
        if (i == 17)
            m_repoProjectsTab = b; // handle for the Projects (N) badge
        m_repoDetailTabs->addButton(b, i);
        b->setProperty("forkmeshButtonKey",
                       QStringLiteral("repoTab:%1")
                           .arg(QString::fromLatin1(tab.label)
                                    .toLower()
                                    .replace(QLatin1Char(' '),
                                             QLatin1Char('-'))));
        connect(b, &QPushButton::clicked, this, [this, b] {
            if (isInRail(b))
                showSection(0);
        });
        if (i == 0) {
            b->setParent(this);
            b->hide();
        } else if (i == 12)
            (void)b; // added to the toolbar layout below, not the tab row
        else if (i == 17)
            tabRow->insertWidget(1, b, 0, Qt::AlignTop);
        else if (i == 18)
            // Size map sits right after Insights. By this point the row holds
            // Issues, Projects, Pulls, Discussions, Actions, Security, Quality,
            // Insights (indices 0-7), so index 8 drops it between Insights and
            // Artifacts; its positional id stays 18.
            tabRow->insertWidget(8, b, 0, Qt::AlignTop);
        else
            tabRow->addWidget(b, 0, Qt::AlignTop);
    }
    tabRow->addStretch();
    tabRow->addWidget(notifyButton, 0, Qt::AlignTop);
    tabRow->addWidget(m_forkButton, 0, Qt::AlignTop);
    tabRow->addWidget(m_mirrorButton, 0, Qt::AlignTop);
    tabRow->addWidget(m_sourceButton, 0, Qt::AlignTop);
    tabRow->addWidget(m_repoOpenButton, 0, Qt::AlignTop);
    m_repoTabRowLayout = tabRow;
    m_repoTabRowTabs.clear();
    for (int i = 0; i < tabRow->count(); ++i) {
        QWidget *widget = tabRow->itemAt(i)->widget();
        if (!widget)
            break;
        m_repoTabRowTabs.append(
            {widget->property("forkmeshButtonKey").toString(), widget});
    }
    m_repoTabRowActions = {
        {QStringLiteral("repoAction:notify"), notifyButton},
        {QStringLiteral("repoAction:fork"), m_forkButton},
        {QStringLiteral("repoAction:mirror"), m_mirrorButton},
        {QStringLiteral("repoAction:source"), m_sourceButton},
        {QStringLiteral("repoAction:open"), m_repoOpenButton},
    };
    {
        NextRepoButtonStyler *styler = NextRepoButtonStyler::forWindow(this);
        for (const auto &entry : std::as_const(m_repoTabRowTabs))
            styler->adopt(entry.second.data(), NextRepoTab);
        for (const auto &entry : std::as_const(m_repoTabRowActions))
            styler->adopt(entry.second.data(), NextRepoAction);
    }
    auto *tabBar = new QWidget;
    tabBar->setObjectName("repoTabBar");
    tabBar->setLayout(tabRow);
    tabBar->setMinimumWidth(0);
    tabBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *tabBarScroll = new QScrollArea;
    tabBarScroll->setObjectName("repoTabBarScroll");
    tabBarScroll->setWidget(tabBar);
    tabBarScroll->setWidgetResizable(true);
    tabBarScroll->setFrameShape(QFrame::NoFrame);
    tabBarScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    tabBarScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tabBarScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    tabBarScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tabBarScroll->setMinimumWidth(0);
    auto adjustTabBarHeight = [tabBarScroll] {
        const int scrollbarHeight = tabBarScroll->horizontalScrollBar()->maximum() > 0
                                       ? tabBarScroll->horizontalScrollBar()
                                             ->sizeHint()
                                             .height()
                                       : 0;
        tabBarScroll->setFixedHeight(44 + kRepoTabRowTopInset + scrollbarHeight);
    };
    connect(tabBarScroll->horizontalScrollBar(), &QScrollBar::rangeChanged,
            tabBarScroll, [adjustTabBarHeight](int, int) { adjustTabBarHeight(); });
    QTimer::singleShot(0, tabBarScroll, adjustTabBarHeight);



    auto *looperToggle = new LooperToggle(this);
    looperToggle->setOnClick([this] { toggleIssueLooper(); });
    looperToggle->setOnNumberClick([this] {
        int sessionId = m_looperSessionId;
        if (sessionId <= 0 && m_looperCurrentIssue > 0)
            if (const AgentSession *s =
                    latestAgentSessionForIssue(m_looperCurrentIssue))
                sessionId = s->id;
        if (sessionId > 0)
            switchToAgentsTab(sessionId);
    });
    m_looperToggle = looperToggle;


    m_repoDetailStack = new CurrentPageStack;
    connect(m_repoDetailStack, &QStackedWidget::currentChanged, this,
            [this](int) {
                updateQuickAddEnterTarget();
                renderPageNotices();
            });
    auto addDeferredRepoTab = [this] {
        auto *placeholder = new QWidget;
        placeholder->setProperty("forkmeshDeferredRepoTab", true);
        m_repoDetailStack->addWidget(placeholder);
    };
    addDeferredRepoTab();                                                // 0 Code
    m_repoDetailStack->addWidget(new QWidget);
    addDeferredRepoTab();                                                // 2 Issues
    addDeferredRepoTab();                                                // 3 Agents
    addDeferredRepoTab();                                                // 4 Pull requests
    addDeferredRepoTab();                                                // 5 Discussions
    addDeferredRepoTab();                                                // 6 Actions
    addDeferredRepoTab();                                                // 7 Security
    addDeferredRepoTab();                                                // 8 Quality
    m_insightsTabIndex = m_repoDetailStack->count();
    addDeferredRepoTab();                                                // 9 Insights
    m_branchesTabIndex = m_repoDetailStack->count();
    m_repoDetailStack->addWidget(new QWidget);                           // 10 Branches (moved)
    m_worktreesTabIndex = m_repoDetailStack->count();
    m_repoDetailStack->addWidget(new QWidget);                           // 11 Worktrees (moved)
    m_releasesTabIndex = m_repoDetailStack->count();
    addDeferredRepoTab();                                                // 12 Releases
    m_repoDetailStack->addWidget(new QWidget);
    m_artifactsTabIndex = m_repoDetailStack->count();
    addDeferredRepoTab();                                                // 14 Artifacts
    m_shortcutsTabIndex = m_repoDetailStack->count();
    addDeferredRepoTab();                                                // 15 Shortcuts
    m_settingsTabIndex = m_repoDetailStack->count();
    addDeferredRepoTab();                                                // 16 Settings
    m_projectsTabIndex = m_repoDetailStack->count();
    addDeferredRepoTab();                                                // 17 Projects
    m_sizeMapTabIndex = m_repoDetailStack->count();
    addDeferredRepoTab();                                                // 18 Size map
    connect(m_repoDetailStack, &QStackedWidget::currentChanged, this,
            [this](int index) { ensureRepoDetailTabBuilt(index); });
    m_chatStackIndex = -1;
    connect(m_repoDetailStack, &QStackedWidget::currentChanged, this,
            [this](int) { scheduleNavRecord(); });
    connect(m_repoDetailTabs, &QButtonGroup::idClicked, this, [this](int id) {
        const QAbstractButton *tabButton = m_repoDetailTabs->button(id);
        const forkmesh::PageSwitchScope switchTrace(
            QStringLiteral("repo tab %1 %2")
                .arg(id)
                .arg(tabButton ? tabButton->text().simplified() : QString()),
            [this, id]() -> QWidget * {
                return m_repoDetailStack ? m_repoDetailStack->widget(id) : nullptr;
            });
        ensureRepoDetailTabBuilt(id);
        m_repoDetailStack->setCurrentIndex(id);
        if (m_agentsNavButton)
            m_agentsNavButton->setChecked(id == 3);
        if (id == 0) {
            showOverviewFiles();
            if (m_overviewLoadedKey.isEmpty())
                QTimer::singleShot(0, this,
                                   [this] { loadRepoOverview(QString()); });
        }
        if (id == 2) {
            resetIssueFilters();
            reloadIssuesInBackground();
            refreshChatUserDirectory(); // faces for the reporters and assignees
            // Next turn: the sync marks the repo as syncing and refreshes the
            // repository list, which can rebuild widgets this handler still
            // touches on the way out (focusRepoDetailTable below).
            QTimer::singleShot(0, this, [this] {
                syncIssuesFromMeshOnOpen(m_repoDetailIndex);
            });
        }
        else if (id == 3) {
            QTimer::singleShot(0, this, [this] {
                reloadPullsInBackground();
                // Opening the page repaints the last scan's numbers. Only
                // "Update all" (or an action that moved a branch) rescans.
                reloadAgents(AgentReloadScan::FromCache);
            });
        }
        else if (id == 4) {
            reloadPullsInBackground();
            if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
                drainPullsInboxFor(m_repositories.at(m_repoDetailIndex), false);
                reconcilePullLedgerFor(m_repositories.at(m_repoDetailIndex),
                                       /*force=*/true);
            }
        }
        else if (id == 5) {
            reloadDiscussions();
            if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size())
                drainDiscussionsInboxFor(m_repositories.at(m_repoDetailIndex), false);
        }
        else if (id == 6)
            refreshRepoActions();
        else if (id == 7)
            refreshRepoSecurity();
        else if (id == 8)
            refreshRepoQuality();
        else if (id == m_insightsTabIndex)
            loadRepoInsights();
        else if (id == m_worktreesTabIndex)
            loadWorktreesPanel();
        else if (id == m_releasesTabIndex)
            loadReleasesPanel();
        else if (id == m_artifactsTabIndex)
            loadArtifactsPanel();
        else if (id == m_shortcutsTabIndex)
            loadShortcutsPanel();
        else if (id == m_settingsTabIndex)
            refreshRepoSettings();
        else if (id == m_projectsTabIndex)
            reloadProjects();
        else if (id == m_sizeMapTabIndex)
            refreshSizeMapTab(false);
        focusRepoDetailTable(id);
    });

    m_repoDetailStack->setCurrentIndex(0);

    m_repoDetailStack->setMinimumHeight(0);
    m_repoDetailStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);

    m_railCodeButton = new ActivityRailButton(QStringLiteral("code"),
                                              QStringLiteral("Code"));
    m_railCodeButton->setToolTip(QStringLiteral("Browse the repository files"));
    connect(m_railCodeButton, &QPushButton::clicked, this, [this] {
        showSection(0);
        if (m_repoDetailTabs && m_repoDetailTabs->button(0))
            m_repoDetailTabs->button(0)->click();
        updateRepoActivityRail();
        scheduleNavRecord();
    });
    m_railGitButton = new ActivityRailButton(QStringLiteral("git-branch"),
                                             QStringLiteral("Git"));
    m_railGitButton->setToolTip(
        QString::fromUtf8("Source control \xE2\x80\x94 view the current changes"));
    connect(m_railGitButton, &QPushButton::clicked, this, [this] {
        showSection(0);
        m_branchCompareBase.clear();
        showOverviewCommits();
        setCommitWorkspacePage(kCommitWorkspaceChangesPage);
        const QString base = repoDefaultBranchFast();
        showSourceControlLoading(base);
        QTimer::singleShot(0, this, [this] {
            const QString base = repoDefaultBranchFast();
            if (!base.isEmpty() && m_repoBranch != base) {
                setRepoBranch(base);
                return; // setRepoBranch already refreshes source control/history
            }
            refreshCommitsUnlessCurrent();
        });
        updateRepoActivityRail();
        scheduleNavRecord();
    });
    if (m_appNavigationRailLayout) {
        QWidget *firstAgentButton =
            m_sessionsNavButton ? m_sessionsNavButton : m_agentsNavButton;
        const int codeAt =
            firstAgentButton
                ? qMax(0, m_appNavigationRailLayout->indexOf(firstAgentButton))
                : 0;
        m_appNavigationRailLayout->insertWidget(
            codeAt, m_railCodeButton, 0, Qt::AlignLeft);
        const int gitAt = m_agentsNavButton
                              ? m_appNavigationRailLayout->indexOf(
                                    m_agentsNavButton) + 1
                              : codeAt + 1;
        m_appNavigationRailLayout->insertWidget(
            qMax(codeAt + 1, gitAt), m_railGitButton, 0, Qt::AlignLeft);
        applyRailCaptionPreference();
        applyRailOrder();
    }
    connect(m_repoDetailStack, &QStackedWidget::currentChanged, this,
            [this](int) { updateRepoActivityRail(); });
    if (m_overviewBodyStack)
        connect(m_overviewBodyStack, &QStackedWidget::currentChanged, this,
                [this](int) {
                    updateRepoActivityRail();
                    scheduleNavRecord();
                });
    if (m_filesStack)
        connect(m_filesStack, &QStackedWidget::currentChanged, this,
                [this](int) {
                    updateRepoActivityRail();
                    scheduleNavRecord();
                });
    updateRepoActivityRail();

    m_repoDetailChrome = new QWidget;
    auto *chromeLayout = new QVBoxLayout(m_repoDetailChrome);
    chromeLayout->setContentsMargins(0, 0, 0, 0);
    chromeLayout->setSpacing(6);
    if (next) {
        auto *header = new QWidget;
        header->setObjectName(QStringLiteral("repoNextHeader"));
        auto *headerRow = new QHBoxLayout(header);
        headerRow->setContentsMargins(22, 14, 16, 4);
        headerRow->setSpacing(10);
        auto *tile = new QLabel;
        tile->setObjectName(QStringLiteral("repoNextHeaderTile"));
        tile->setFixedSize(28, 28);
        tile->setAlignment(Qt::AlignCenter);
        tile->setPixmap(tintedOcticonPixmap(QStringLiteral("repo"),
                                            Theme::color(Theme::Role::FgMuted), 16));
        headerRow->addWidget(tile, 0, Qt::AlignVCenter);
        m_repoHeaderTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_repoHeaderTitle->setTextFormat(Qt::RichText);
        headerRow->addWidget(m_repoHeaderTitle, 0, Qt::AlignVCenter);
        m_repoHeaderTitle->setVisible(true);
        auto *pill = new QLabel;
        pill->setObjectName(QStringLiteral("repoVisibilityPill"));
        pill->setTextFormat(Qt::PlainText);
        pill->hide();
        headerRow->addWidget(pill, 0, Qt::AlignVCenter);
        headerRow->addStretch(1);
        chromeLayout->addWidget(header);
    }
    chromeLayout->addWidget(m_repoDetailNotice);
    chromeLayout->addWidget(metaBand);
    chromeLayout->addWidget(tabBarScroll);
    syncPackageSurfaces();

    auto *content = new QVBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(6);
    content->addWidget(m_repoDetailChrome);
    content->addWidget(m_repoDetailStack, 1);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(content, 1);
    return page;
}

void MainWindow::updateRepoActivityRail()
{
    if (!m_railCodeButton || !m_railGitButton)
        return;
    const bool onHome =
        !m_sectionStack || m_sectionStack->currentIndex() == 0;
    const bool onCode =
        onHome && m_repoDetailStack && m_repoDetailStack->currentIndex() == 0;
    const bool onChanges =
        onCode && m_filesStack && m_filesStack->currentIndex() == 0 &&
        m_overviewBodyStack && m_overviewBodyStack->currentIndex() == 1;
    const bool onBranches =
        onCode && m_filesStack && m_filesStack->currentIndex() == 0 &&
        m_overviewBodyStack && m_overviewBodyStack->currentIndex() == 2;
    const bool onAgents = onHome && m_repoDetailStack &&
                          m_repoDetailStack->currentIndex() == kRepoAgentsTab;
    const bool onIssues = onHome && m_repoDetailStack &&
                          m_repoDetailStack->currentIndex() == 2;
    const bool onTeam = m_sectionStack &&
                        m_sectionStack->currentIndex() == kTeamSectionIndex;
    const bool onTasks =
        m_sectionStack &&
        m_sectionStack->currentIndex() == kOrganizationTasksSectionIndex;
    if (m_repoDetailChrome)
        m_repoDetailChrome->setVisible(!onChanges && !onAgents);
    if (m_repoFilesModeBar)
        m_repoFilesModeBar->setVisible(!onChanges);
    if (m_repoOverviewChrome)
        m_repoOverviewChrome->setVisible(!onChanges && !onBranches);
    setGitPromptOverlay(onChanges);
    m_railCodeButton->setChecked(onCode && !onChanges);
    m_railGitButton->setChecked(onChanges);
    if (m_filesModeOverviewButton)
        m_filesModeOverviewButton->setChecked(
            !isInRail(m_filesModeOverviewButton) ||
            (onCode && m_filesStack && m_filesStack->currentIndex() == 0 &&
             m_overviewBodyStack && m_overviewBodyStack->currentIndex() == 0));
    if (m_agentsNavButton)
        m_agentsNavButton->setChecked(onAgents);
    if (m_globalSearch)
        m_globalSearch->setPlaceholderText(
            onTeam
                ? QString::fromUtf8(
                      "Search special agents, specialties or skills\xE2\x80\xA6")
            : onTasks
                ? QString::fromUtf8("Search tasks\xE2\x80\xA6")
            : onChanges ? QString::fromUtf8("Search commits\xE2\x80\xA6")
            : onAgents
                ? QString::fromUtf8("Search agents & transcripts\xE2\x80\xA6")
            : onIssues
                ? QString::fromUtf8("Search issues\xE2\x80\xA6  is:open label:bug author:me")
            : Theme::isNext() ? QString::fromUtf8("Search or jump to\xE2\x80\xA6")
                : QString::fromUtf8("Search\xE2\x80\xA6"));
    if (m_globalSearch && onTeam) {
        m_globalSearch->setToolTip(
            QStringLiteral("Filter these special agents by name, specialty, focus, skill, "
                           "shortcut or latest output."));
    } else if (m_globalSearch && onTasks) {
        m_globalSearch->setToolTip(
            QStringLiteral("Filter the task list by title, details, assignee, "
                           "repository, department, status, how to test or "
                           "task ID."));
    } else if (m_globalSearch && onIssues) {
        m_globalSearch->setToolTip(
            "Search issues with GitHub-style operators: is:open, is:closed, "
            "label:name, milestone:name, author:name, assignee:name, "
            "priority:number, and no:label/milestone/assignee.");
        if (m_issueSearch && m_issueSearch->text() != m_globalSearch->text())
            m_issueSearch->setText(m_globalSearch->text());
    } else if (m_globalSearch) {
        m_globalSearch->setToolTip(QString::fromUtf8(
            "Search everything \xE2\x80\x94 sections, relays, nodes, repositories, and "
            "the open repo's issues, pull requests, discussions, projects, agents, "
            "special agents, "
            "workflow runs, branches, files and commits, plus runtime logs"));
    }
    syncGitCommitFilter();
    syncAgentPageSearch();
    syncTeammatePageSearch();
    syncOrganizationTaskPageSearch();
    syncNativePackagePageSearch();
    // The rail stays visible while the nested Git view is hidden. Coalesce the
    // stack signals from one navigation, then refresh its count for whichever
    // page is now on screen.
    scheduleRepoChangeBadgeRefresh();
}

void MainWindow::setGitPromptOverlay(bool enabled)
{
    Q_UNUSED(enabled);
    positionGlobalFooterOverlays();
}

void MainWindow::positionGitPromptOverlay()
{
    positionGlobalFooterOverlays();
}

void MainWindow::syncGitCommitFilter()
{
    if (!m_commitSearch)
        return;
    const bool onGit = m_railGitButton && m_railGitButton->isChecked();
    const QString query =
        onGit && m_globalSearch ? m_globalSearch->text().trimmed() : QString();
    if (m_commitSearch->text() == query)
        return;
    m_commitSearch->setText(query); // textChanged -> filterCommits
}

void MainWindow::syncAgentPageSearch()
{
    const bool onHome = !m_sectionStack || m_sectionStack->currentIndex() == 0;
    const bool onAgents = onHome && m_repoDetailStack &&
                          m_repoDetailStack->currentIndex() == kRepoAgentsTab;
    const QString query =
        onAgents && m_globalSearch ? m_globalSearch->text().trimmed() : QString();
    auto mirror = [&](QLineEdit *box) {
        if (!box || box->text() == query)
            return;
        if (query.isEmpty() && box->text() != m_agentPageSearchMirror)
            return;
        box->setText(query); // textChanged -> transcript highlight
    };
    mirror(m_transcriptSearch);
    const bool listQueryChanged = m_agentPageSearchMirror != query;
    m_agentPageSearchMirror = query;
    if (listQueryChanged)
        refreshAgentTable();
    scheduleAgentTranscriptSearch();
}

void MainWindow::clearSearchFilters()
{
    // Mirrors first: the top box only pushes its text into whichever mirror the
    // current page owns, so clearing it alone would leave a box the user typed
    // into directly (the issue filter, the commit filter) still narrowing its
    // list — and the notice offering this link names that term too.
    for (QLineEdit *box : {m_issueSearch, m_commitSearch, m_transcriptSearch}) {
        if (box && !box->text().isEmpty())
            box->clear();
    }
    if (m_globalSearch && !m_globalSearch->text().isEmpty())
        m_globalSearch->clear(); // textChanged -> popup, mirrors, list refreshes
}

QLabel *MainWindow::createSearchEmptyNotice()
{
    QLabel *notice = makeSearchEmptyNotice();
    connect(notice, &QLabel::linkActivated, this, [this](const QString &href) {
        if (href == kClearSearchLink)
            clearSearchFilters();
    });
    return notice;
}

void MainWindow::updateSearchEmptyNotice(QLabel *notice, QWidget *list,
                                         const QString &query, bool listIsEmpty)
{
    if (!notice)
        return;
    // An unfiltered list that happens to be empty is not a search result: the
    // page's own "nothing here yet" state is the honest thing to leave showing.
    const bool show = listIsEmpty && !query.trimmed().isEmpty();
    if (show)
        notice->setText(searchEmptyNoticeHtml(query));
    notice->setVisible(show);
    if (list)
        list->setVisible(!show);
}


QWidget *MainWindow::buildRepoCommitsTab()
{
    m_commitsStack = new QStackedWidget;
    m_commitsStack->installEventFilter(this);

    auto *listPage = new QWidget;
    listPage->setObjectName(QStringLiteral("gitHistoryListPage"));
    m_commitsTable = new QTableWidget(0, 9);
    m_commitsTable->setObjectName("commitsList");
    enableHoverRowHighlight(m_commitsTable);
    m_commitsTable->setFrameShape(QFrame::NoFrame);
    m_commitsTable->setStyleSheet(Theme::remapSheet(m_commitsTable->styleSheet() +
                                  QStringLiteral("#commitsList{border:none;}")));
    m_commitsTable->horizontalHeader()->setVisible(false);
    m_commitsTable->verticalHeader()->setVisible(false);
    m_commitsTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_commitsTable->verticalHeader()->setDefaultSectionSize(24);
    m_commitsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_commitsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_commitsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_commitsTable->setShowGrid(false);
    m_commitsTable->setWordWrap(false);
    m_commitsTable->setSortingEnabled(false);
    m_commitsTable->setTextElideMode(Qt::ElideRight);
    m_commitsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_commitsTable, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
                if (!m_commitsTable)
                    return;
                QTableWidgetItem *hit = m_commitsTable->itemAt(position);
                if (!hit)
                    return;
                QTableWidgetItem *summary =
                    m_commitsTable->item(hit->row(), kCommitSummaryCol);
                if (!summary)
                    return;
                const int kind = summary->data(kCommitRowKindRole).toInt();
                const bool heading = kind == 2;
                const bool outgoingCommit =
                    kind == 0 && summary->data(kCommitUnsyncedRole).toBool();
                if (!heading && !outgoingCommit)
                    return;

                QMenu menu(m_commitsTable);
                QAction *undo = menu.addAction(
                    heading ? QStringLiteral("Uncommit all outgoing changes")
                            : QStringLiteral("Uncommit this and newer changes"));
                undo->setIcon(themedOcticon(QStringLiteral("arrow-left"),
                                            QColor(QStringLiteral("#8b949e")), 14));
                if (menu.exec(m_commitsTable->viewport()->mapToGlobal(position)) !=
                    undo)
                    return;

                QString target;
                QString description;
                if (heading) {
                    if (m_commitsUnsyncedHashes.isEmpty())
                        return;
                    target = m_commitsUnsyncedHashes.last() + QLatin1Char('^');
                    description = QStringLiteral("all %1 outgoing commits")
                                      .arg(m_commitsUnsyncedHashes.size());
                } else {
                    const QString hash = summary->data(Qt::UserRole).toString();
                    if (hash.isEmpty())
                        return;
                    target = hash + QLatin1Char('^');
                    const int index = m_commitsUnsyncedHashes.indexOf(hash);
                    description = QStringLiteral("%1 outgoing commit%2")
                                      .arg(index < 0 ? 1 : index + 1)
                                      .arg(index == 0 ? QString()
                                                      : QStringLiteral("s"));
                }
                if (QMessageBox::question(
                        this, QStringLiteral("Uncommit outgoing changes"),
                        QStringLiteral("Uncommit %1?\n\nTheir file changes will return "
                                       "to the Changes panel and will not be lost.")
                            .arg(description),
                        QMessageBox::Yes | QMessageBox::Cancel) !=
                    QMessageBox::Yes)
                    return;
                const QString dir = repoGitDir();
                if (dir.isEmpty()) {
                    QMessageBox::warning(this,
                                         QStringLiteral("Uncommit outgoing changes"),
                                         QStringLiteral("git reset failed."));
                    return;
                }
                if (m_uncommitRunning)
                    return;
                m_uncommitRunning = true;
                runOffThread<GitCaptureResult>(
                    [dir, target] {
                        return readGit(dir, {QStringLiteral("reset"),
                                             QStringLiteral("--mixed"), target});
                    },
                    [this, dir, description](GitCaptureResult reset) {
                        m_uncommitRunning = false;
                        if (!reset.ok) {
                            QMessageBox::warning(
                                this, QStringLiteral("Uncommit outgoing changes"),
                                reset.error.isEmpty()
                                    ? QStringLiteral("git reset failed.")
                                    : reset.error.left(300));
                            return;
                        }
                        logSystem(QStringLiteral("Git: uncommitted %1 in %2; the "
                                                 "file changes returned to Changes.")
                                      .arg(description, dir));
                        if (dir == repoGitDir())
                            loadCommits();
                        refreshSourceControl(true);
                    });
            });
    QHeaderView *commitHeader = m_commitsTable->horizontalHeader();
    commitHeader->setHighlightSections(false);
    for (int i = 0; i < kCommitSummaryCol; ++i)
        m_commitsTable->setColumnHidden(i, true);
    m_commitsTable->setColumnHidden(kCommitActionCol, true);
    commitHeader->setSectionResizeMode(kCommitSummaryCol, QHeaderView::Stretch);
    m_commitsTable->setColumnHidden(kCommitGraphCol, true);
    m_commitsTable->setItemDelegateForColumn(
        kCommitSummaryCol, new CommitSummaryDelegate(m_commitsTable));
    connect(m_commitsTable, &QTableWidget::cellClicked, this,
            [this](int row, int) {
                QTableWidgetItem *item = m_commitsTable->item(row, kCommitSummaryCol);
                if (!item)
                    return;
                const int kind = item->data(kCommitRowKindRole).toInt();
                if (kind == 1) {
                    m_pendingCommitFileScroll =
                        item->data(kCommitFilePathRole).toString();
                    showCommit(item->data(Qt::UserRole).toString());
                    return;
                }
                if (kind == 0)
                    toggleCommitFilesRows(row);
                if (kind == 2) {
                    m_commitsUnsyncedExpanded = !m_commitsUnsyncedExpanded;
                    updateCommitsUnsyncedFilesPanel();
                }
            });
    connect(m_commitsTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) {
                QTableWidgetItem *item = m_commitsTable->item(row, kCommitSummaryCol);
                if (item && item->data(kCommitRowKindRole).toInt() == 0)
                    showCommit(item->data(Qt::UserRole).toString());
            });
    connect(m_commitsTable, &QTableWidget::itemActivated, this,
            [this](QTableWidgetItem *it) {
                QTableWidgetItem *item =
                    it ? m_commitsTable->item(it->row(), kCommitSummaryCol) : nullptr;
                if (item && item->data(kCommitRowKindRole).toInt() == 0)
                    showCommit(item->data(Qt::UserRole).toString());
            });
    m_commitsListPage = listPage;
    buildCommitsUnsyncedBanner(listPage);

    m_commitSearch = new QLineEdit(listPage);
    m_commitSearch->hide();
    // The first character typed flips the list to its 5000-commit search
    // depth (a full reload), and every further keystroke re-filters all of
    // it. Debounce so a typed word costs one reload + one filter pass, not
    // one per key.
    auto *commitSearchDebounce = new QTimer(this);
    commitSearchDebounce->setSingleShot(true);
    commitSearchDebounce->setInterval(250);
    connect(commitSearchDebounce, &QTimer::timeout, this, [this] {
        if (m_commitSearch)
            filterCommits(m_commitSearch->text());
    });
    connect(m_commitSearch, &QLineEdit::textChanged, this,
            [commitSearchDebounce](const QString &) {
                commitSearchDebounce->start();
            });

    m_commitsFetchButton = new QPushButton("Fetch");
    m_commitsFetchButton->setObjectName("ghostButton");
    m_commitsFetchButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_commitsFetchButton, "cloud", 14);
    m_commitsFetchButton->setToolTip(
        "Fetch the latest history from the network without changing your "
        "working tree, then reload this list");
    connect(m_commitsFetchButton, &QPushButton::clicked, this,
            &MainWindow::fetchCurrentRepo);

    m_commitsLeaderboardButton = new QPushButton("Leaderboard");
    m_commitsLeaderboardButton->setObjectName("ghostButton");
    m_commitsLeaderboardButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_commitsLeaderboardButton, "graph", 14);
    m_commitsLeaderboardButton->setToolTip(
        "See the page a merge shows: the newest merge's certificate, merge "
        "velocity, merges per agent and today's activity");
    connect(m_commitsLeaderboardButton, &QPushButton::clicked, this,
            &MainWindow::showGitLeaderboard);

    m_commitsPullButton = new QPushButton("Pull");
    m_commitsPullButton->setObjectName("ghostButton");
    m_commitsPullButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_commitsPullButton, "download", 14);
    m_commitsPullButton->setToolTip(
        "Fast-forward the working tree to the latest fetched history "
        "(git pull --ff-only)");
    connect(m_commitsPullButton, &QPushButton::clicked, this,
            &MainWindow::pullCurrentRepo);

    auto *branchButton = new ElidingPushButton;
    branchButton->setFullText(QStringLiteral("main"));
    m_commitsBranchButton = branchButton;
    // A button's default policy makes its layout minimum its full sizeHint,
    // which is exactly what these two elide their text to avoid. Let them
    // shrink to the eliding minimum they already advertise.
    m_commitsBranchButton->setSizePolicy(QSizePolicy::Preferred,
                                         QSizePolicy::Fixed);
    m_commitsBranchButton->setObjectName("ghostButton");
    m_commitsBranchButton->setCursor(Qt::PointingHandCursor);
    m_commitsBranchButton->setToolTip(
        "Branch shown below — click to browse another branch's history or create one");
    setOcticon(m_commitsBranchButton, "git-branch", 14);

    m_commitsCompareArrow = new QLabel(QString::fromUtf8("\xE2\x86\x92"));
    m_commitsCompareArrow->setObjectName("hintLabel");
    m_commitsCompareArrow->setToolTip(
        QStringLiteral("The branch on the left is being compared against the "
                       "branch on the right"));
    m_commitsCompareArrow->hide();

    auto *compareBaseButton = new ElidingPushButton;
    m_commitsCompareBaseButton = compareBaseButton;
    m_commitsCompareBaseButton->setSizePolicy(QSizePolicy::Preferred,
                                              QSizePolicy::Fixed);
    m_commitsCompareBaseButton->setObjectName("ghostButton");
    m_commitsCompareBaseButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_commitsCompareBaseButton, "git-merge", 14);
    m_commitsCompareBaseButton->hide();

    // The branch pair plus Fetch/Pull/refresh on one rigid line was the widest
    // thing in the Git file pane, and so the limit on how far in the splitter
    // could be dragged. The pair keeps the line and the buttons drop below it.
    auto *searchRowHost = new QWidget(listPage);
    searchRowHost->setObjectName(QStringLiteral("gitHistoryHeader"));
    searchRowHost->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto *searchRow = new HeaderWrapLayout(searchRowHost, 8, 4);
    auto *branchPair = new QWidget(searchRowHost);
    auto *branchPairRow = new QHBoxLayout(branchPair);
    branchPairRow->setContentsMargins(0, 0, 0, 0);
    branchPairRow->setSpacing(8);
    branchPairRow->addWidget(m_commitsBranchButton, 1);
    branchPairRow->addWidget(m_commitsCompareArrow);
    branchPairRow->addWidget(m_commitsCompareBaseButton, 1);
    searchRow->setTitle(branchPair);
    searchRow->addAction(m_commitsFetchButton);
    searchRow->addAction(m_commitsPullButton);
    searchRow->addAction(m_commitsLeaderboardButton);

    connect(m_commitsTable->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int value) {
                if (!m_commitsTable)
                    return;
                QScrollBar *sb = m_commitsTable->verticalScrollBar();
                if (m_commitsHasMore && !m_commitsLoadingMore && sb->maximum() > 0 &&
                    value >= sb->maximum() - 2)
                    QTimer::singleShot(0, this, [this] { loadMoreCommits(); });
            });

    auto *listLayout = new QVBoxLayout(listPage);
    listLayout->setContentsMargins(6, 4, 4, 4);
    listLayout->addWidget(searchRowHost);
    auto *outgoingRow = new QWidget(listPage);
    auto *outgoingRowLayout = new QHBoxLayout(outgoingRow);
    outgoingRowLayout->setContentsMargins(0, 0, 0, 0);
    outgoingRowLayout->setSpacing(8);
    outgoingRowLayout->addWidget(m_commitsUnsyncedSyncButton);
    outgoingRowLayout->addWidget(m_commitsUnsyncedBanner, 1);
    listLayout->addWidget(outgoingRow);
    m_commitsUnsyncedFiles = new QTreeWidget(listPage);
    m_commitsUnsyncedFiles->setObjectName("commitsUnsyncedFiles");
    m_commitsUnsyncedFiles->setHeaderHidden(true);
    m_commitsUnsyncedFiles->setRootIsDecorated(true);
    m_commitsUnsyncedFiles->setMaximumHeight(180);
    enableHoverRowHighlight(m_commitsUnsyncedFiles);
    connect(m_commitsUnsyncedFiles, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!item)
                    return;
                const QString hash = item->data(0, Qt::UserRole).toString();
                if (hash.isEmpty())
                    return;
                m_pendingCommitFileScroll =
                    item->data(0, Qt::UserRole + 1).toString();
                showCommit(hash);
            });
    m_commitsUnsyncedFiles->hide();
    listLayout->addWidget(m_commitsUnsyncedFiles);
    listLayout->addWidget(m_commitsTable);
    m_commitSearchEmpty = createSearchEmptyNotice();
    listLayout->addWidget(m_commitSearchEmpty);

    auto *detailPage = buildCommitDetailPage();

    auto *scmPanel = buildSourceControlPanel();
    searchRow->addAction(m_scmRefreshButton);
    m_gitFilesSlot = new QStackedWidget;
    m_gitFilesSlot->addWidget(scmPanel); // 0: working-tree CHANGES
    m_gitHistorySlot = new QStackedWidget;
    m_gitHistorySlot->addWidget(listPage); // 0: commit history
    auto *leftSplit = new QSplitter(Qt::Vertical);
    leftSplit->setChildrenCollapsible(false);
    leftSplit->addWidget(m_gitFilesSlot);
    leftSplit->addWidget(m_gitHistorySlot);
    constexpr int kGitFilesSlotMinHeight = 130;
    m_gitFilesSlot->setMinimumHeight(kGitFilesSlotMinHeight);
    leftSplit->setStretchFactor(0, 2);
    leftSplit->setStretchFactor(1, 3);
    leftSplit->setSizes({320, 520});

    auto *changesPage = new QWidget;
    auto *changesLayout = new QVBoxLayout(changesPage);
    changesLayout->setContentsMargins(0, 0, 0, 0);
    changesLayout->setSpacing(0);
    m_scmDiff = new QTextBrowser;
    m_scmDiff->setObjectName("diffView");
    registerDiffView(m_scmDiff);
    m_scmDiff->setFrameShape(QFrame::NoFrame);
    m_scmDiff->setStyleSheet(Theme::remapSheet(m_scmDiff->styleSheet() +
                             QStringLiteral("#diffView{border:none;}")));
    setupScmDiffPane();
    m_scmDiff->setHtml(QStringLiteral(
        "<p style='color:#8b949e'>No working-tree changes to review.</p>"));
    m_scmSecretBanner = new forkmesh::ui::SecretDiffBanner(changesPage);
    m_scmSecretBanner->setOuterMargins(8, 8, 8, 8);
    m_scmSecretBanner->onIgnore =
        [this](const QList<RepoSecurityFinding> &findings) {
            ignoreScmDiffSecrets(findings);
        };
    changesLayout->addWidget(m_scmSecretBanner);
    changesLayout->addWidget(m_scmDiff, 1);

    m_commitsStack->addWidget(changesPage); // kCommitWorkspaceChangesPage
    m_commitsStack->addWidget(detailPage);  // kCommitWorkspaceCommitPage
    m_commitsStack->addWidget(buildBranchRangePane()); // kCommitWorkspaceRangePage
    m_commitsStack->setCurrentIndex(kCommitWorkspaceChangesPage);

    auto *workspaceSplit = new QSplitter(Qt::Horizontal);
    workspaceSplit->setObjectName(QStringLiteral("gitWorkspaceSplit"));
    workspaceSplit->setChildrenCollapsible(true);
    workspaceSplit->addWidget(leftSplit);
    workspaceSplit->addWidget(m_commitsStack);
    workspaceSplit->setCollapsible(0, false);
    workspaceSplit->setCollapsible(1, true);
    workspaceSplit->setStretchFactor(0, 0);
    workspaceSplit->setStretchFactor(1, 1);
    workspaceSplit->setSizes({430, 950});

    m_gitWorkspaceStack = new QStackedWidget;
    m_gitWorkspaceStack->addWidget(workspaceSplit);
    m_gitWorkspaceStack->addWidget(buildGitMergedPage());
    m_gitWorkspaceStack->setCurrentIndex(0);

    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_gitWorkspaceStack);
    return page;
}

QWidget *MainWindow::buildGitMergedPage()
{
    m_gitMergedPage = new QWidget;
    m_gitMergedPage->setObjectName(QStringLiteral("gitMergedPage"));
    m_gitMergedPage->installEventFilter(this);

    m_gitMergedLabel = new QLabel;
    m_gitMergedLabel->setObjectName("hintLabel");
    m_gitMergedLabel->setTextFormat(Qt::RichText);
    m_gitMergedLabel->setWordWrap(true);

    auto *agentsButton = new QPushButton(QStringLiteral("Back to agents"));
    agentsButton->setObjectName("ghostButton");
    agentsButton->setCursor(Qt::PointingHandCursor);
    setOcticon(agentsButton, "smart-toy", 14);
    connect(agentsButton, &QPushButton::clicked, this, [this] {
        openRepoAgentsList();
        clearGitMergedCover();
    });

    auto *branchesButton = new QPushButton(QStringLiteral("Back to branches"));
    branchesButton->setObjectName("ghostButton");
    branchesButton->setCursor(Qt::PointingHandCursor);
    setOcticon(branchesButton, "git-branch", 14);
    connect(branchesButton, &QPushButton::clicked, this, [this] {
        clearGitMergedCover();
        showOverviewBranches();
        loadBranchesPanel();
    });

    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    header->addWidget(m_gitMergedLabel, 1);
    header->addWidget(agentsButton);
    header->addWidget(branchesButton);

    m_gitMergedStats = new QTextBrowser;
    m_gitMergedStats->setObjectName("diffView");
    m_gitMergedStats->setFrameShape(QFrame::NoFrame);
    m_gitMergedStats->setStyleSheet(Theme::remapSheet(m_gitMergedStats->styleSheet() +
                                    QStringLiteral("#diffView{border:none;}")));
    m_gitMergedStats->setOpenExternalLinks(false);
    m_gitMergedStats->setOpenLinks(false);
    connect(m_gitMergedStats, &QTextBrowser::anchorClicked, this,
            &MainWindow::onBranchDiffAnchorClicked);

    auto *layout = new QVBoxLayout(m_gitMergedPage);
    layout->setContentsMargins(12, 10, 0, 0);
    layout->setSpacing(8);
    layout->addLayout(header);
    layout->addWidget(m_gitMergedStats, 1);
    return m_gitMergedPage;
}

bool MainWindow::gitWorkspaceShowsBranchRange(const QString &branch) const
{
    return !branch.isEmpty() && branch == m_branchDiffBranch && m_commitsStack &&
           m_commitsStack->currentIndex() == kCommitWorkspaceRangePage &&
           m_commitsStack->isVisible();
}

void MainWindow::clearGitMergedCover()
{
    if (m_gitWorkspaceStack && m_gitWorkspaceStack->currentIndex() != 0)
        m_gitWorkspaceStack->setCurrentIndex(0);
}

bool MainWindow::gitMergedCoverShown() const
{
    return m_gitWorkspaceStack && m_gitMergedPage &&
           m_gitWorkspaceStack->currentWidget() == m_gitMergedPage;
}

// The outgoing-commits banner above the history list and its Sync button.
void MainWindow::buildCommitsUnsyncedBanner(QWidget *listPage)
{
    m_commitsUnsyncedBanner = new QLabel(listPage);
    m_commitsUnsyncedBanner->setObjectName("statusLine");
    m_commitsUnsyncedBanner->setTextFormat(Qt::RichText);
    m_commitsUnsyncedBanner->setWordWrap(true);
    m_commitsUnsyncedBanner->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(m_commitsUnsyncedBanner, &QLabel::linkActivated, this,
            [this](const QString &) {
                m_commitsUnsyncedExpanded = !m_commitsUnsyncedExpanded;
                updateCommitsUnsyncedFilesPanel();
            });
    m_commitsUnsyncedBanner->setStyleSheet(Theme::remapSheet(
        "#statusLine {"
        "  background-color: rgba(210,153,34,0.16);"
        "  border: 1px solid rgba(210,153,34,0.55);"
        "  border-radius: 6px;"
        "  padding: 6px 10px;"
        "}"));
    m_commitsBannerOpacity = new QGraphicsOpacityEffect(m_commitsUnsyncedBanner);
    m_commitsBannerOpacity->setOpacity(1.0);
    m_commitsUnsyncedBanner->setGraphicsEffect(m_commitsBannerOpacity);
    m_commitsBannerFade = new QPropertyAnimation(m_commitsBannerOpacity,
                                                 "opacity", this);
    m_commitsBannerFade->setDuration(1500);
    m_commitsBannerFade->setEasingCurve(QEasingCurve::InCubic);
    connect(m_commitsBannerFade, &QPropertyAnimation::finished, this, [this] {
        if (m_commitsUnsyncedBanner)
            m_commitsUnsyncedBanner->hide();
    });
    m_commitsUnsyncedBanner->hide();
    m_commitsUnsyncedSyncButton = new QPushButton(QStringLiteral("Sync Changes"));
    m_commitsUnsyncedSyncButton->setObjectName("ghostButton");
    m_commitsUnsyncedSyncButton->setCursor(Qt::PointingHandCursor);
    m_commitsUnsyncedSyncButton->setToolTip(
        QStringLiteral("Publish outgoing commits to the network mirror or push "
                       "them to the configured upstream branch"));
    setOcticon(m_commitsUnsyncedSyncButton, QStringLiteral("sync"), 14);
    connect(m_commitsUnsyncedSyncButton, &QPushButton::clicked, this,
            &MainWindow::pushCurrentRepoUpstream);
    m_commitsUnsyncedSyncButton->hide();
}

// The single-commit page of the Git workspace: navigation and history
// actions, title, message, metadata and the commit's diff.
QWidget *MainWindow::buildCommitDetailPage()
{
    auto *detailPage = new QWidget;

    m_commitPrevButton = new QPushButton("Prev");
    m_commitNextButton = new QPushButton("Next");
    for (QPushButton *b : {m_commitPrevButton, m_commitNextButton}) {
        b->setObjectName("ghostButton");
        b->setCursor(Qt::PointingHandCursor);
    }
    setOcticon(m_commitPrevButton, "chevron-down", 16);
    setOcticon(m_commitNextButton, "chevron-right", 16);
    m_commitPrevButton->setToolTip("Show the previous (newer) commit");
    m_commitNextButton->setToolTip("Show the next (older) commit");
    auto goToCommitRow = [this](int row, int dir) {
        if (!m_commitsTable)
            return;
        while (row >= 0 && row < m_commitsTable->rowCount()) {
            QTableWidgetItem *it = m_commitsTable->item(row, kCommitSummaryCol);
            if (it && it->data(kCommitRowKindRole).toInt() == 0) {
                showCommit(it->data(Qt::UserRole).toString());
                return;
            }
            row += dir;
        }
    };
    connect(m_commitPrevButton, &QPushButton::clicked, this,
            [this, goToCommitRow] { goToCommitRow(m_currentCommitRow - 1, -1); });
    connect(m_commitNextButton, &QPushButton::clicked, this,
            [this, goToCommitRow] { goToCommitRow(m_currentCommitRow + 1, +1); });

    m_commitTitle = new QLabel;
    m_commitTitle->setObjectName("repoHeaderTitle");
    m_commitTitle->setTextFormat(Qt::RichText);
    m_commitTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_commitTitle->setWordWrap(true);
    m_commitTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto *commitCopyLinkButton = new QPushButton("Copy link");
    commitCopyLinkButton->setObjectName("ghostButton");
    commitCopyLinkButton->setCursor(Qt::PointingHandCursor);
    commitCopyLinkButton->setToolTip(
        "Copy a link to this commit you can paste into an issue or PR comment");
    setOcticon(commitCopyLinkButton, "copy", 16);
    connect(commitCopyLinkButton, &QPushButton::clicked, this, [this] {
        copyReferenceLink(QStringLiteral("commit"), m_currentCommitHash);
    });

    m_commitDownloadButton = new QPushButton("Download patch");
    m_commitDownloadButton->setObjectName("ghostButton");
    m_commitDownloadButton->setCursor(Qt::PointingHandCursor);
    m_commitDownloadButton->setToolTip(
        "Save this commit as a .patch file you can re-import as a pull request");
    setOcticon(m_commitDownloadButton, "download", 16);
    connect(m_commitDownloadButton, &QPushButton::clicked, this,
            &MainWindow::downloadCommitPatch);

    m_commitDeleteButton = new QPushButton("Delete commit");
    m_commitDeleteButton->setObjectName("ghostButton");
    m_commitDeleteButton->setCursor(Qt::PointingHandCursor);
    m_commitDeleteButton->setToolTip(
        "Remove this commit from history (rewrites the branch and replays the "
        "later commits onto its parent)");
    setOcticon(m_commitDeleteButton, "trash", 16);
    connect(m_commitDeleteButton, &QPushButton::clicked, this,
            [this] { deleteCommit(m_currentCommitHash); });

    m_commitRevertButton = new QPushButton("Restore commit");
    m_commitRevertButton->setObjectName("ghostButton");
    m_commitRevertButton->setCursor(Qt::PointingHandCursor);
    m_commitRevertButton->setToolTip(
        "Undo this commit by committing the reverse of its changes (history is "
        "kept)");
    setOcticon(m_commitRevertButton, "history", 16);
    connect(m_commitRevertButton, &QPushButton::clicked, this,
            [this] { revertCommit(m_currentCommitHash); });

    m_commitSplitButton = new QPushButton;
    m_commitSplitButton->setObjectName("ghostButton");
    m_commitSplitButton->setCursor(Qt::PointingHandCursor);
    m_commitSplitButton->setCheckable(true);
    m_commitSplitButton->setChecked(diffSplitPref());
    setOcticon(m_commitSplitButton, "diff", 16);
    updateDiffSplitButton(m_commitSplitButton);
    connect(m_commitSplitButton, &QPushButton::clicked, this, [this](bool on) {
        setDiffSplitPref(on);
        updateDiffSplitButton(m_commitSplitButton);
        for (QPushButton *b : {m_pullSplitButton, m_branchSplitButton}) {
            if (b) {
                b->setChecked(on);
                updateDiffSplitButton(b);
            }
        }
        if (!m_currentCommitHash.isEmpty())
            showCommit(m_currentCommitHash);
    });
    for (QPushButton *b :
         {commitCopyLinkButton, m_commitDownloadButton, m_commitDeleteButton,
          m_commitRevertButton, m_commitPrevButton, m_commitNextButton}) {
        b->setProperty("buttonSize", "sm");
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        b->setFixedWidth(qMax(72, b->sizeHint().width()));
    }

    auto *navCol = new QVBoxLayout;
    navCol->setContentsMargins(0, 0, 0, 0);
    navCol->setSpacing(4);

    // Both rows flow: seven fixed-width buttons in two rigid lines set the
    // floor for the whole right-hand pane -- a QStackedLayout takes its
    // minimum from every page it holds, shown or not -- so the commit view
    // used to stop the splitter being dragged in even while a diff was up.
    QSizePolicy commitRowPolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    commitRowPolicy.setHeightForWidth(true);

    auto *historyActions = new QWidget;
    historyActions->setSizePolicy(commitRowPolicy);
    auto *historyActionRow = new FlowLayout(historyActions, 0, 4, 4);
    historyActionRow->addWidget(m_commitDeleteButton);
    historyActionRow->addWidget(m_commitRevertButton);
    navCol->addWidget(historyActions);

    auto *commitTools = new QWidget;
    commitTools->setSizePolicy(commitRowPolicy);
    auto *commitToolRow = new FlowLayout(commitTools, 0, 4, 4);
    commitToolRow->addWidget(m_commitSplitButton);
    commitToolRow->addWidget(commitCopyLinkButton);
    commitToolRow->addWidget(m_commitDownloadButton);
    commitToolRow->addWidget(m_commitPrevButton);
    commitToolRow->addWidget(m_commitNextButton);
    navCol->addWidget(commitTools);

    auto *headerRow = new QVBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->addLayout(navCol);
    headerRow->addWidget(m_commitTitle, 0, Qt::AlignTop);

    m_commitMessage = new QLabel;
    m_commitMessage->setObjectName("commitMessage");
    m_commitMessage->setWordWrap(true);
    m_commitMessage->setTextFormat(Qt::RichText);
    m_commitMessage->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                             Qt::LinksAccessibleByMouse);
    connect(m_commitMessage, &QLabel::linkActivated, this,
            [this](const QString &href) {
                if (href == QLatin1String("toggle-msg")) {
                    m_commitMessage->setProperty(
                        "expanded",
                        !m_commitMessage->property("expanded").toBool());
                    refreshCommitMessageLabel(m_commitMessage);
                } else if (href.startsWith(QStringLiteral("pull:")))
                    openPullReference(href.mid(5).toInt());
                else if (href.startsWith(QStringLiteral("issue:")))
                    openIssueReference(href.mid(6).toInt());
                else if (href.startsWith(QStringLiteral("ref:")))
                    openCommitReference(href.mid(4).toInt());
                else if (href.startsWith(QStringLiteral("commit:")))
                    openCommitHashReference(href.mid(7));
            });

    m_commitMeta = new QLabel;
    m_commitMeta->setObjectName("statusLine");
    m_commitMeta->setTextFormat(Qt::RichText);
    m_commitMeta->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Author, date and churn on one unbreakable line was the last thing
    // holding the right-hand pane open; wrapped, it reads the same and gives
    // the splitter the rest of its travel, as the message above it already did.
    m_commitMeta->setWordWrap(true);

    m_commitFilesSummary = nullptr;

    m_commitFileList = nullptr;

    m_commitDiffView = new QTextBrowser;
    m_commitDiffView->setObjectName("commitDiffView");
    m_commitDiffView->setOpenExternalLinks(false);
    registerDiffView(m_commitDiffView);

    m_commitDiffSpinner = new BusySpinner;
    m_commitDiffSpinner->setToolTip(QString::fromUtf8("Loading diff\xE2\x80\xA6"));
    m_commitDiffSpinner->hide();
    auto *diffSpinRow = new QHBoxLayout;
    diffSpinRow->setContentsMargins(0, 0, 0, 0);
    diffSpinRow->addStretch();
    diffSpinRow->addWidget(m_commitDiffSpinner);

    auto *split = new QWidget;
    auto *splitLayout = new QVBoxLayout(split);
    splitLayout->setContentsMargins(0, 0, 0, 0);
    splitLayout->setSpacing(4);
    splitLayout->addLayout(diffSpinRow);
    splitLayout->addWidget(m_commitDiffView, 1);

    auto *detailLayout = new QVBoxLayout(detailPage);
    detailLayout->setContentsMargins(16, 12, 16, 16);
    detailLayout->setSpacing(8);
    detailLayout->addLayout(headerRow);
    detailLayout->addWidget(m_commitMessage);
    detailLayout->addWidget(m_commitMeta);
    detailLayout->addWidget(split, 1);
    return detailPage;
}




void MainWindow::loadFileSearchIndex()
{
    if (!m_fileCompleter)
        return;
    const QString dir = repoGitDir();
    if (dir.isEmpty()) {
        m_fileCompleter->setModel(new QStringListModel(QStringList(), m_fileCompleter));
        return;
    }
    const int forIndex = m_repoDetailIndex;
    runGitDetached(dir, {"ls-tree", "-r", "--name-only", "-z", currentRef()},
                   [this, forIndex](bool ok, const QByteArray &out) {
                       if (forIndex != m_repoDetailIndex || !m_fileCompleter)
                           return;
                       QStringList paths;
                       if (ok) {
                           for (const QByteArray &record : out.split('\0'))
                               if (!record.isEmpty())
                                   paths << QString::fromUtf8(record);
                       }
                       m_fileCompleter->setModel(
                           new QStringListModel(paths, m_fileCompleter));
                   });
}

void MainWindow::spinRefreshButton(QPushButton *button)
{
    if (!button || button->property("fmSpinning").toBool())
        return;
    button->setProperty("fmSpinning", true);
    const int size = button->iconSize().width() > 0 ? button->iconSize().width() : 16;
    const QIcon original = button->icon();
    auto *timer = new QTimer(button);
    auto angle = std::make_shared<int>(0);
    connect(timer, &QTimer::timeout, button, [button, angle, size] {
        *angle = (*angle + 30) % 360;
        button->setIcon(
            QIcon(refreshPixmap(QColor(Theme::kRunning), *angle, size)));
    });
    timer->start(60);
    QTimer::singleShot(650, button, [button, timer, original] {
        timer->stop();
        timer->deleteLater();
        button->setIcon(original);
        button->setProperty("fmSpinning", false);
    });
}

void MainWindow::addRefreshSpin(QPushButton *button)
{
    if (!button)
        return;
    connect(button, &QPushButton::clicked, this,
            [this, button] { spinRefreshButton(button); });
}

void MainWindow::startButtonSpin(QPushButton *button)
{
    if (!button || button->property("fmSpinning").toBool())
        return;
    button->setProperty("fmSpinning", true);
    button->setProperty("fmSpinIcon", QVariant::fromValue(button->icon()));
    const int size = button->iconSize().width() > 0 ? button->iconSize().width() : 16;
    auto *timer = new QTimer(button);
    timer->setObjectName(QStringLiteral("fmSpinTimer"));
    auto angle = std::make_shared<int>(0);
    connect(timer, &QTimer::timeout, button, [button, angle, size] {
        if (!button->isVisible())
            return;
        *angle = (*angle + 30) % 360;
        const QVariant restartProgress = button->property("fmRestartProgress");
        const QPixmap frame = restartProgress.isValid()
                                  ? restartProgressPixmap(
                                        QColor(Theme::kRunning), *angle, size,
                                        restartProgress.toInt(),
                                        button->property("fmSpinHourglass").toBool())
                                  : (button->property("fmSpinHourglass").toBool()
                                         ? hourglassPixmap(QColor(Theme::kRunning), *angle,
                                                           size)
                                         : refreshPixmap(QColor(Theme::kRunning), *angle,
                                                         size));
        button->setIcon(QIcon(frame));
    });
    timer->start(60);
}

void MainWindow::stopButtonSpin(QPushButton *button)
{
    if (!button || !button->property("fmSpinning").toBool())
        return;
    if (auto *timer = button->findChild<QTimer *>(QStringLiteral("fmSpinTimer"))) {
        timer->stop();
        timer->deleteLater();
    }
    button->setIcon(button->property("fmSpinIcon").value<QIcon>());
    button->setProperty("fmSpinning", false);
    button->setProperty("fmSpinHourglass", false);
    button->setProperty("fmRestartProgress", QVariant());
}

QString MainWindow::restartTimingHistoryKey(RestartKind kind) const
{
    return kind == RestartKind::Update
               ? QStringLiteral("restartTiming/updateRuns")
               : QStringLiteral("restartTiming/rebuildRuns");
}

QList<forkmesh::restarttiming::RestartRun>
MainWindow::restartRunHistory(RestartKind kind) const
{
    QSettings settings;
    const QString key = restartTimingHistoryKey(kind);
    if (settings.contains(key))
        return forkmesh::restarttiming::parseRuns(settings.value(key).toStringList());
    // Seed from the duration-only totals that older builds kept.
    return forkmesh::restarttiming::parseRuns(
        settings
            .value(kind == RestartKind::Update
                       ? QStringLiteral("restartTiming/updateDurationsMs")
                       : QStringLiteral("restartTiming/rebuildDurationsMs"))
            .toStringList());
}

void MainWindow::appendRestartRun(RestartKind kind,
                                  const forkmesh::restarttiming::RestartRun &run)
{
    QList<forkmesh::restarttiming::RestartRun> runs = restartRunHistory(kind);
    runs.append(run);
    QSettings().setValue(restartTimingHistoryKey(kind),
                         forkmesh::restarttiming::serializeRuns(runs));
}

void MainWindow::recordRestartDuration()
{
    if (m_restartDurationRecorded || !m_restartElapsedTimer.isValid())
        return;
    using namespace forkmesh::restarttiming;
    RestartRun run;
    run.totalMs = m_restartElapsedTimer.elapsed();
    run.milestones = m_restartMilestones;
    run.finishedAtMs = QDateTime::currentMSecsSinceEpoch();
    QSettings settings;
    if (m_updateAsUser.isEmpty()) {
        // The relaunched app reads these same settings and finishes the record
        // at its first paint, so the total includes the startup users wait on.
        settings.setValue(QStringLiteral("restartTiming/pendingKind"),
                          m_restartKind == RestartKind::Update
                              ? QStringLiteral("update")
                              : QStringLiteral("rebuild"));
        settings.setValue(QStringLiteral("restartTiming/pendingRun"),
                          serializeRun(run));
    } else if (isPlausibleDuration(run.totalMs)) {
        // A relaunch as another user starts with that user's settings, so it
        // cannot finish this record; keep the time up to the handoff.
        appendRestartRun(m_restartKind, run);
    }
    settings.sync(); // the old process exits immediately after a successful relaunch
    m_restartDurationRecorded = true;
}

void MainWindow::finishPendingRestartTiming()
{
    using namespace forkmesh::restarttiming;
    const QString runKey = QStringLiteral("restartTiming/pendingRun");
    const QString kindKey = QStringLiteral("restartTiming/pendingKind");
    QSettings settings;
    if (!settings.contains(runKey))
        return;
    const QString stored = settings.value(runKey).toString();
    const bool update =
        settings.value(kindKey).toString() == QLatin1String("update");
    settings.remove(runKey);
    settings.remove(kindKey);
    RestartRun run;
    if (!parseRun(stored, &run) || run.finishedAtMs <= 0 ||
        !m_debugSessionUptime.isValid())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 handoffMs =
        (now - m_debugSessionUptime.elapsed()) - run.finishedAtMs;
    if (handoffMs < -5'000 || handoffMs > kRelaunchHandoffWindowMs)
        return; // left behind by a relaunch that never painted; not this launch
    run.totalMs += qMax<qint64>(0, now - run.finishedAtMs);
    run.finishedAtMs = now;
    if (isPlausibleDuration(run.totalMs))
        appendRestartRun(update ? RestartKind::Update : RestartKind::Rebuild, run);
}

static QBoxLayout *containingButtonRow(QLayout *layout, QWidget *button)
{
    if (!layout)
        return nullptr;
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem *item = layout->itemAt(index);
        if (item->widget() == button)
            return qobject_cast<QBoxLayout *>(layout);
        if (QBoxLayout *row = containingButtonRow(item->layout(), button))
            return row;
    }
    return nullptr;
}

void MainWindow::addRestartCountdown(QPushButton *button)
{
    if (!button || !button->parentWidget())
        return;
    auto *row = containingButtonRow(button->parentWidget()->layout(), button);
    if (!row)
        return;
    const int index = row->indexOf(button);
    if (index < 0)
        return;

    auto *label = new QLabel(button->parentWidget());
    label->setObjectName(QStringLiteral("restartCountdown"));
    label->setAccessibleName(QStringLiteral("Restart countdown"));
    label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    row->insertWidget(index + 1, label, 0, Qt::AlignVCenter);
    m_restartCountdownLabel = label;
}

QString MainWindow::restartCountdownText() const
{
    if (!m_restartSpinButton || !m_restartElapsedTimer.isValid())
        return QString();
    const qint64 elapsed = m_restartElapsedTimer.elapsed();
    const qint64 remaining = forkmesh::restarttiming::remainingNowMs(
        m_restartMilestoneRemainingMs, m_restartMilestoneElapsedMs, elapsed);
    QString activity = m_restartPhase;
    if (!m_restartBuildStep.isEmpty())
        activity = m_restartBuildPass > 1
                       ? QStringLiteral("%1 (pass %2)")
                             .arg(m_restartBuildStep)
                             .arg(m_restartBuildPass)
                       : m_restartBuildStep;
    return forkmesh::restarttiming::countdownText(
        m_restartProgress, activity, remaining, elapsed);
}

void MainWindow::updateRestartCountdown()
{
    if (!m_restartSpinButton || !m_restartElapsedTimer.isValid())
        return;
    const qint64 elapsed = m_restartElapsedTimer.elapsed();
    const QString text = restartCountdownText();
    if (m_restartCountdownLabel) {
        m_restartCountdownLabel->setText(text);
        const QString source = m_restartHistoryCount > 0
                                   ? QStringLiteral("based on %1 previous restart%2")
                                         .arg(m_restartHistoryCount)
                                         .arg(m_restartHistoryCount == 1 ? QString()
                                                                           : QStringLiteral("s"))
                                   : QStringLiteral("using the initial estimate");
        QString tip = QStringLiteral("%1; the estimate is %2.\nRunning for %3.")
                          .arg(text, source,
                               forkmesh::restarttiming::durationText(elapsed));
        if (!m_restartBuildLine.isEmpty())
            tip += QStringLiteral("\nNow: %1").arg(m_restartBuildLine);
        m_restartCountdownLabel->setToolTip(tip);
    }
    m_restartSpinButton->setToolTip(
        QStringLiteral("Restarting… %1. Click again to cancel.").arg(text));
}

void MainWindow::startRestartSpin(QPushButton *button, RestartKind kind)
{
    if (!button)
        return;
    stopRestartSpin();
    m_restartSpinButton = button;
    m_restartKind = kind;
    m_restartProgress = 0;
    m_restartDurationRecorded = false;
    m_restartPhase.clear();
    m_restartBuildPass = 0;
    m_restartBuildStep.clear();
    m_restartBuildLine.clear();
    m_restartOutputCarry.clear();
    const QList<forkmesh::restarttiming::RestartRun> history =
        restartRunHistory(kind);
    m_restartHistoryCount =
        qMin(history.size(), forkmesh::restarttiming::kHistoryLimit);
    m_restartElapsedTimer.start();
    m_restartMilestones.clear();
    m_restartMilestones.insert(0, 0);
    m_restartMilestoneElapsedMs = 0;
    m_restartMilestoneRemainingMs =
        forkmesh::restarttiming::remainingAfterMilestoneMs(history, 0, 0);
    button->setProperty("fmRestartProgress", 0);
    button->setProperty("fmRestartTooltip", button->toolTip());
    button->setToolTip(
        QString::fromUtf8("Restarting\xE2\x80\xA6 click again to cancel"));
    addRestartCountdown(button);
    if (!m_restartCountdownTimer) {
        m_restartCountdownTimer = new QTimer(this);
        m_restartCountdownTimer->setInterval(1000);
        connect(m_restartCountdownTimer, &QTimer::timeout, this,
                &MainWindow::updateRestartCountdown);
    }
    m_restartCountdownTimer->start();
    updateRestartCountdown();
    startButtonSpin(button);
}

void MainWindow::stopRestartSpin()
{
    stopRestartCautionFlash();
    if (m_restartCountdownTimer)
        m_restartCountdownTimer->stop();
    if (m_restartCountdownLabel) {
        delete m_restartCountdownLabel;
        m_restartCountdownLabel = nullptr;
    }
    m_restartElapsedTimer.invalidate();
    m_restartMilestoneRemainingMs = 0;
    m_restartMilestoneElapsedMs = 0;
    m_restartMilestones.clear();
    m_restartProgress = 0;
    m_restartHistoryCount = 0;
    m_restartPhase.clear();
    m_restartBuildPass = 0;
    m_restartBuildStep.clear();
    m_restartBuildLine.clear();
    m_restartOutputCarry.clear();
    if (!m_restartSpinButton)
        return;
    if (const QVariant tip = m_restartSpinButton->property("fmRestartTooltip");
        tip.isValid()) {
        m_restartSpinButton->setToolTip(tip.toString());
        m_restartSpinButton->setProperty("fmRestartTooltip", QVariant());
    }
    stopButtonSpin(m_restartSpinButton);
    m_restartSpinButton = nullptr;
}

void MainWindow::setRestartSpinProgress(int percent)
{
    if (!m_restartSpinButton)
        return;
    m_restartProgress = qBound(0, percent, 100);
    m_restartSpinButton->setProperty("fmRestartProgress", m_restartProgress);
    // A rebuild pass that repeats a milestone keeps the first arrival time.
    if (m_restartElapsedTimer.isValid() &&
        !m_restartMilestones.contains(m_restartProgress)) {
        const qint64 elapsed = m_restartElapsedTimer.elapsed();
        m_restartMilestones.insert(m_restartProgress, elapsed);
        m_restartMilestoneElapsedMs = elapsed;
        m_restartMilestoneRemainingMs =
            forkmesh::restarttiming::remainingAfterMilestoneMs(
                restartRunHistory(m_restartKind), m_restartProgress, elapsed);
    }
    updateRestartCountdown();
}

void MainWindow::setRestartPhase(const QString &phase, int buildPass)
{
    m_restartPhase = phase;
    m_restartBuildPass = buildPass;
    m_restartBuildStep.clear();
    m_restartBuildLine.clear();
    m_restartOutputCarry.clear();
    if (m_restartSpinButton && m_restartElapsedTimer.isValid())
        updateRestartCountdown();
}

// Build output arrives in arbitrary chunks; only whole lines are read, and only
// while a build pass runs, so git or go output can never pose as a step count.
void MainWindow::noteRestartStepOutput(const QString &chunk)
{
    if (!m_restartSpinButton || m_restartBuildPass <= 0)
        return;
    const QString text = m_restartOutputCarry + chunk;
    const qsizetype end = qMax(text.lastIndexOf(QLatin1Char('\n')),
                               text.lastIndexOf(QLatin1Char('\r')));
    m_restartOutputCarry = text.mid(end + 1).right(4096);
    if (end < 0)
        return;
    const forkmesh::restarttiming::BuildProgress progress =
        forkmesh::restarttiming::lastBuildProgress(text.left(end));
    if (!progress.isValid())
        return;
    m_restartBuildStep = progress.text();
    m_restartBuildLine = progress.line;
    if (m_restartElapsedTimer.isValid())
        updateRestartCountdown();
}

void MainWindow::setRestartSpinHourglass(bool hourglass)
{
    if (m_restartSpinButton)
        m_restartSpinButton->setProperty("fmSpinHourglass", hourglass);
}

void MainWindow::startRefreshSpin()
{
    if (!m_refreshButton)
        return;
    if (!m_refreshSpinTimer) {
        m_refreshSpinTimer = new QTimer(this);
        connect(m_refreshSpinTimer, &QTimer::timeout, this, [this] {
            m_refreshAngle = (m_refreshAngle + 30) % 360;
            m_refreshButton->setIcon(
                QIcon(refreshPixmap(QColor(Theme::kRunning), m_refreshAngle, 22)));
        });
    }
    m_refreshSpinTimer->start(60);
}

void MainWindow::stopRefreshSpin()
{
    if (m_refreshSpinTimer)
        m_refreshSpinTimer->stop();
    if (m_refreshButton)
        m_refreshButton->setIcon(
            QIcon(refreshPixmap(QColor(Theme::kTextTertiary), 0, 22)));
}

void MainWindow::startCommitDiffSpin()
{
    if (m_commitDiffSpinner)
        m_commitDiffSpinner->show();
}

void MainWindow::stopCommitDiffSpin()
{
    if (m_commitDiffSpinner)
        m_commitDiffSpinner->hide();
}

void MainWindow::startNodeSwitchSpin()
{
    if (!m_nodeMenuButton)
        return;
    if (!m_nodeSwitchSpinTimer) {
        m_nodeSwitchSpinTimer = new QTimer(this);
        connect(m_nodeSwitchSpinTimer, &QTimer::timeout, this, [this] {
            m_nodeSwitchAngle = (m_nodeSwitchAngle + 30) % 360;
            m_nodeMenuButton->setIcon(
                QIcon(refreshPixmap(QColor(Theme::kRunning),
                                    m_nodeSwitchAngle, 16)));
        });
    }
    m_nodeSwitchSpinTimer->start(60);

    if (!m_nodeSwitchProgress) {
        m_nodeSwitchProgress =
            new QProgressBar(m_nodeMenuButton->parentWidget());
        m_nodeSwitchProgress->setObjectName("nodeSwitchProgress");
        m_nodeSwitchProgress->setRange(0, 0); // busy / indeterminate
        m_nodeSwitchProgress->setTextVisible(false);
        m_nodeSwitchProgress->setFixedHeight(3);
        m_nodeSwitchProgress->hide();
    }
    positionNodeSwitchProgress();
    m_nodeSwitchProgress->show();
    m_nodeSwitchProgress->raise();
}

void MainWindow::positionNodeSwitchProgress()
{
    if (!m_nodeSwitchProgress || !m_nodeMenuButton)
        return;
    QWidget *parent = m_nodeSwitchProgress->parentWidget();
    if (!parent)
        return;
    const QPoint topLeft = m_nodeMenuButton->mapTo(
        parent, QPoint(0, m_nodeMenuButton->height() + 1));
    m_nodeSwitchProgress->setGeometry(topLeft.x(), topLeft.y(),
                                      m_nodeMenuButton->width(), 3);
}

void MainWindow::stopNodeSwitchSpin()
{
    if (m_nodeSwitchSpinTimer)
        m_nodeSwitchSpinTimer->stop();
    if (m_nodeSwitchProgress)
        m_nodeSwitchProgress->hide();
    updateNodeSwitcher();
}

void MainWindow::startRepoSwitchSpin()
{
    if (!m_repoMenuButton)
        return;
    if (!m_repoSwitchSpinTimer) {
        m_repoSwitchSpinTimer = new QTimer(this);
        connect(m_repoSwitchSpinTimer, &QTimer::timeout, this, [this] {
            m_repoSwitchAngle = (m_repoSwitchAngle + 30) % 360;
            m_repoMenuButton->setIcon(
                QIcon(refreshPixmap(QColor(Theme::kRunning),
                                    m_repoSwitchAngle, 16)));
        });
    }
    m_repoMenuButton->setIcon(
        QIcon(refreshPixmap(QColor(Theme::kRunning), 0, 16)));
    m_repoSwitchSpinTimer->start(60);
}

void MainWindow::stopRepoSwitchSpin()
{
    if (m_repoSwitchSpinTimer)
        m_repoSwitchSpinTimer->stop();
    if (m_repoMenuButton)
        m_repoMenuButton->setIcon(QIcon());
    updateRepoSwitcher();
}

void MainWindow::openRepoDetailDeferred(int repoIndex, bool preservePage)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    if (repoIndex == m_repoDetailIndex && !m_repoDetailLoading) {
        showSection(0);
        return;
    }
    if (m_repoOpenPending == repoIndex)
        return;
    const bool restoreGit = preservePage && m_railGitButton &&
                            m_railGitButton->isChecked();
    const int restoreDetailTab =
        preservePage && m_repoDetailStack ? m_repoDetailStack->currentIndex() : 0;
    m_repoOpenPending = repoIndex;
    startRepoSwitchSpin();
    showLoadStatus(QStringLiteral("Opening repository…"));
    QApplication::setOverrideCursor(Qt::BusyCursor);
    QTimer::singleShot(0, this,
                       [this, repoIndex, restoreGit, restoreDetailTab] {
        m_repoOpenPending = -1;
        QElapsedTimer timer;
        timer.start();
        m_repoLoadActive = true;
        openRepoDetail(repoIndex);
        m_repoLoadActive = false;
        if (restoreGit && m_railGitButton) {
            m_railGitButton->click();
        } else if (restoreDetailTab > 0 && m_repoDetailTabs &&
                   m_repoDetailTabs->button(restoreDetailTab)) {
            m_repoDetailTabs->button(restoreDetailTab)->click();
        }
        finishLoadStepTiming(); // log the final step's duration
        stopRepoSwitchSpin();
        QApplication::restoreOverrideCursor();
        const qint64 ms = timer.elapsed();
        if (ms > 500 && repoIndex >= 0 && repoIndex < m_repositories.size()) {
            const RepositoryRecord &r = m_repositories.at(repoIndex);
            flashMessage(QStringLiteral("Opened %1/%2 in %3 ms.")
                             .arg(r.owner, r.name)
                             .arg(ms));
        } else {
            dismissLoadStatus();
        }
    });
}

// code-health: allow(blocking-wait) progress flush between the repo-open load steps so the status line paints; processEvents does not wait on anything
void MainWindow::nodeSwitchStep(const QString &what)
{
    if (!m_nodeSwitching && !m_repoLoadActive)
        return;
    if (!m_loadStepName.isEmpty() && m_loadStepTimer.isValid())
        logSystem(QStringLiteral("  - %1 (%2 ms)")
                      .arg(m_loadStepName)
                      .arg(m_loadStepTimer.elapsed()));
    showLoadStatus(what);
    QString plain = what;
    plain.replace(QChar(0x2026), QStringLiteral("..."));
    m_loadStepName = plain;
    m_loadStepTimer.restart();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

void MainWindow::finishLoadStepTiming()
{
    if (!m_loadStepName.isEmpty() && m_loadStepTimer.isValid())
        logSystem(QStringLiteral("  - %1 (%2 ms)")
                      .arg(m_loadStepName)
                      .arg(m_loadStepTimer.elapsed()));
    m_loadStepName.clear();
    m_loadStepTimer.invalidate();
}

void MainWindow::showLoadStatus(const QString &what)
{
    if (!m_loadStatusNotice || !m_pageNoticeArea || what.isEmpty())
        return;
    m_loadStatusNotice->showMessage(what, kit::Tone::Accent);
    m_loadStatusShowing = true;
    m_pageNoticeArea->show();
}
