
#include "Tooltips.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowNetworkInternal.h"
#include "AgentCliLocator.h"
#include "AgentLoginPty.h"
#include "AgentWorktree.h"
#include "ActionStore.h"
#include "ControlNode.h"
#include "CurrentPageStack.h"
#include "MirrorSyncVerdict.h"
#include "DatabaseManagerWidget.h"
#include "KebabHeaderView.h"
#include "NetworkReplyError.h"
#include "NodeEventSocket.h"
#include "PromptDraftStore.h"
#include "QtRuntimeDiagnostics.h"
#include "RewardPoolSigner.h"
#include "TerminalWidget.h"
#include "WorldSpeechBridge.h"
#include "FontSizing.h"

#include "DesignTokens.h"
#include "UiKit.h"

#include <QAction>
#include <QComboBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QTabWidget>

#include <functional>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <signal.h>
#endif

using namespace forkmesh::ui;

namespace {
constexpr int kUsersResizeContentsPrecision = 64;

enum UsersColumn {
    kUsersColName = 0,
    kUsersColDiscordUsername,
    kUsersColDiscordDisplayName,
    kUsersColDiscordId,
    kUsersColProfileVisibility,
    kUsersColAdmin,
    kUsersColOutreach,
    kUsersColEmailNotifications,
    kUsersColSignupIp,
    kUsersColSignupAgent,
    kUsersColWorldActivity,
    kUsersColWorldSteps,
    kUsersColOfficeVisits,
    kUsersColActivityRecency,
    kUsersColJoined,
    kUsersColAvatarUpdated,
    kUsersColEmail,
    kUsersColReferredBy,
    kUsersColReferralStatus,
    kUsersColReferralClickId,
    kUsersColReferralRisk,
    kUsersColReferralCreated,
    kUsersColReferralVerified,
    kUsersColReferralCredited,
    kUsersColReferralClicks,
    kUsersColReferralAuditedUniqueClicks,
    kUsersColReferralDuplicateClicks,
    kUsersColReferralRejectedClicks,
    kUsersColReferralLegacyClicks,
    kUsersColLegacyUnattributed,
    kUsersColCreditedReferrals,
    kUsersColReferredUsers,
    kUsersColReferralLastActivity,
    kUsersColSignupIpFingerprint,
    kUsersColSignupAgentFingerprint,
    kUsersColSignupClientFingerprint,
    kUsersColReferralNetworkFingerprint,
    kUsersColUserFingerprint,
    kUsersColEmailFingerprint,
    kUsersColDiscordFingerprint,
    kUsersColSignupCountry,
    kUsersColSignupClient,
    kUsersColSolana,
    kUsersColMastodon,
    kUsersColVerified,
    kUsersColStatus,
    kUsersColPulls,
    kUsersColIssues,
    kUsersColCommits,
    kUsersColDiscussions,
    kUsersColLastEmail,
    kUsersColEmailDelivery,
    kUsersColEmailOpens,
    kUsersColEmailClicks,
    kUsersColLastEmailOpened,
    kUsersColLastEmailClicked,
    kUsersColCountry,
    kUsersColBrowser,
    kUsersColOs,
    kUsersColNodes,
    kUsersColCount,
};

enum UsersQuarantineColumn {
    kUsersQuarantineColName = 0,
    kUsersQuarantineColDiscordUsername,
    kUsersQuarantineColDiscordDisplayName,
    kUsersQuarantineColDiscordId,
    kUsersQuarantineColProfileVisibility,
    kUsersQuarantineColAdmin,
    kUsersQuarantineColOutreach,
    kUsersQuarantineColAccountStatus,
    kUsersQuarantineColEmailNotifications,
    kUsersQuarantineColSignupIp,
    kUsersQuarantineColSignupAgent,
    kUsersQuarantineColEmail,
    kUsersQuarantineColVerified,
    kUsersQuarantineColJoined,
    kUsersQuarantineColAvatarUpdated,
    kUsersQuarantineColSolana,
    kUsersQuarantineColMastodon,
    kUsersQuarantineColStatus,
    kUsersQuarantineColReasons,
    kUsersQuarantineColQuarantined,
    kUsersQuarantineColReviewAfter,
    kUsersQuarantineColReviewed,
    kUsersQuarantineColReviewedBy,
    kUsersQuarantineColReferredBy,
    kUsersQuarantineColReferralStatus,
    kUsersQuarantineColReferralClickId,
    kUsersQuarantineColRisk,
    kUsersQuarantineColReferralCreated,
    kUsersQuarantineColReferralVerified,
    kUsersQuarantineColReferralCredited,
    kUsersQuarantineColIpFingerprint,
    kUsersQuarantineColAgentFingerprint,
    kUsersQuarantineColClientFingerprint,
    kUsersQuarantineColNetworkFingerprint,
    kUsersQuarantineColUserFingerprint,
    kUsersQuarantineColEmailFingerprint,
    kUsersQuarantineColDiscordFingerprint,
    kUsersQuarantineColReferrerFingerprint,
    kUsersQuarantineColCountry,
    kUsersQuarantineColClient,
    kUsersQuarantineColWorldCountry,
    kUsersQuarantineColBrowser,
    kUsersQuarantineColOs,
    kUsersQuarantineColNodes,
    kUsersQuarantineColCount,
};

QStringList usersTableHeaders()
{
    return {QStringLiteral("User"), QStringLiteral("Discord username"),
            QStringLiteral("Discord display name"), QStringLiteral("Discord ID"),
            QStringLiteral("Profile visibility"), QStringLiteral("Admin"),
            QStringLiteral("Outreach"), QStringLiteral("Email notifications"),
            QStringLiteral("Signup IP"),
            QStringLiteral("Signup agent"), QStringLiteral("World activity"),
            QStringLiteral("World steps"), QStringLiteral("Office visits"),
            QStringLiteral("Activity recency"), QStringLiteral("Joined"),
            QStringLiteral("Avatar updated"),
            QStringLiteral("Email"),
            QStringLiteral("Referred by"), QStringLiteral("Referral status"),
            QStringLiteral("Referral click ID"),
            QStringLiteral("Referral risk"), QStringLiteral("Referral created"),
            QStringLiteral("Referral verified"),
            QStringLiteral("Referral credited"),
            QStringLiteral("Referral clicks"),
            QStringLiteral("Audited unique clicks"),
            QStringLiteral("Duplicate click attempts"),
            QStringLiteral("Rejected click attempts"),
            QStringLiteral("Legacy unaudited clicks"),
            QStringLiteral("Legacy unattributed signups"),
            QStringLiteral("Credited referrals"),
            QStringLiteral("Referred users"),
            QStringLiteral("Referral activity"),
            QStringLiteral("IP fingerprint"),
            QStringLiteral("Agent fingerprint"),
            QStringLiteral("Client fingerprint"),
            QStringLiteral("Referral network fingerprint"),
            QStringLiteral("User fingerprint"),
            QStringLiteral("Email fingerprint"),
            QStringLiteral("Discord fingerprint"),
            QStringLiteral("Signup country"), QStringLiteral("Signup client"),
            QStringLiteral("Solana"), QStringLiteral("Mastodon"),
            QStringLiteral("Email verified"),
            QStringLiteral("Status"),
            QStringLiteral("PRs"),
            QStringLiteral("Issues"), QStringLiteral("Commits"),
            QStringLiteral("Discussions"), QStringLiteral("Last email"),
            QStringLiteral("Email delivery"), QStringLiteral("Email opens"),
            QStringLiteral("Email clicks"), QStringLiteral("Last email open"),
            QStringLiteral("Last email click"), QStringLiteral("Country"),
            QStringLiteral("Browser"), QStringLiteral("OS"),
            QStringLiteral("Nodes")};
}

QStringList usersQuarantineTableHeaders()
{
    return {QStringLiteral("User"), QStringLiteral("Discord username"),
            QStringLiteral("Discord display name"), QStringLiteral("Discord ID"),
            QStringLiteral("Profile visibility"), QStringLiteral("Admin"),
            QStringLiteral("Outreach"), QStringLiteral("Account status"),
            QStringLiteral("Email notifications"), QStringLiteral("Signup IP"),
            QStringLiteral("Signup agent"), QStringLiteral("Email"),
            QStringLiteral("Email verified"), QStringLiteral("Joined"),
            QStringLiteral("Avatar updated"), QStringLiteral("Solana"),
            QStringLiteral("Mastodon"),
            QStringLiteral("Quarantine status"), QStringLiteral("Reasons"),
            QStringLiteral("Quarantined"), QStringLiteral("Review after"),
            QStringLiteral("Reviewed"), QStringLiteral("Reviewed by"),
            QStringLiteral("Referred by"), QStringLiteral("Referral status"),
            QStringLiteral("Referral click ID"),
            QStringLiteral("Referral risk"),
            QStringLiteral("Referral created"),
            QStringLiteral("Referral verified"),
            QStringLiteral("Referral credited"),
            QStringLiteral("IP fingerprint"),
            QStringLiteral("Agent fingerprint"),
            QStringLiteral("Client fingerprint"),
            QStringLiteral("Referral network fingerprint"),
            QStringLiteral("User fingerprint"),
            QStringLiteral("Email fingerprint"),
            QStringLiteral("Discord fingerprint"),
            QStringLiteral("Referrer fingerprint"),
            QStringLiteral("Signup country"), QStringLiteral("Signup client"),
            QStringLiteral("Country"), QStringLiteral("Browser"),
            QStringLiteral("OS"),
            QStringLiteral("Nodes")};
}

SortTableWidgetItem *usersTableItem(const QString &text,
                                    const QVariant &sortValue)
{
    auto *item = new SortTableWidgetItem(text);
    item->setData(kTableSortRole, sortValue);
    return item;
}

QString usersTimestamp(qint64 timestampMs)
{
    if (timestampMs <= 0)
        return QStringLiteral("Not reported");
    return QDateTime::fromMSecsSinceEpoch(timestampMs)
        .toLocalTime()
        .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QPair<QString, int> usersActivityRecency(const QString &bucket)
{
    if (bucket == QLatin1String("hour"))
        return {QStringLiteral("Within 1 hour"), 0};
    if (bucket == QLatin1String("5h"))
        return {QStringLiteral("Within 5 hours"), 1};
    if (bucket == QLatin1String("24h"))
        return {QStringLiteral("Within 24 hours"), 2};
    if (bucket == QLatin1String("3d"))
        return {QStringLiteral("Within 3 days"), 3};
    if (bucket == QLatin1String("5d"))
        return {QStringLiteral("Within 5 days"), 4};
    if (bucket == QLatin1String("10d"))
        return {QStringLiteral("Within 10 days"), 5};
    if (bucket == QLatin1String("stale"))
        return {QStringLiteral("More than 10 days"), 6};
    return {QStringLiteral("Not reported"), 7};
}

constexpr int kUsersNextRowHeight = 48;

QString usersNextInitials(const QString &text)
{
    static const QRegularExpression separators(QStringLiteral("[\\s._-]+"));
    QString initials;
    const QStringList parts = text.split(separators, Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        initials += part.at(0).toUpper();
        if (initials.size() == 2)
            break;
    }
    if (initials.isEmpty() && !text.isEmpty())
        initials = text.left(1).toUpper();
    return initials;
}

void usersNextRowHover(QStyleOptionViewItem *option, const QModelIndex &index)
{
    const auto *view = qobject_cast<const QAbstractItemView *>(option->widget);
    if (!view)
        return;
    option->state &= ~QStyle::State_MouseOver;
    QWidget *viewport = view->viewport();
    if (!viewport || !viewport->underMouse())
        return;
    const QModelIndex hovered =
        view->indexAt(viewport->mapFromGlobal(QCursor::pos()));
    if (hovered.isValid() && hovered.row() == index.row())
        option->state |= QStyle::State_MouseOver;
}

void usersNextDrawBackground(QStyleOptionViewItem *option,
                             QPainter *painter)
{
    option->text.clear();
    option->icon = QIcon();
    option->features &= ~QStyleOptionViewItem::HasDecoration;
    QStyle *style =
        option->widget ? option->widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, option, painter,
                       option->widget);
}

class UsersNextPersonDelegate final : public QStyledItemDelegate
{
public:
    UsersNextPersonDelegate(int displayNameColumn,
                            std::function<QString()> self, QObject *parent)
        : QStyledItemDelegate(parent),
          m_displayNameColumn(displayNameColumn), m_self(std::move(self))
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        usersNextRowHover(&opt, index);
        const QString handle = opt.text.trimmed();
        const QIcon avatar = opt.icon;
        const QFont base = opt.font;
        usersNextDrawBackground(&opt, painter);

        const QString title = titleFor(index, handle);
        const bool self = isSelf(handle);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect rect = opt.rect.adjusted(12, 0, -8, 0);
        constexpr int side = 28;
        const QRect circle(rect.left(), rect.center().y() - side / 2 + 1, side,
                           side);
        if (!avatar.isNull()) {
            QPainterPath clip;
            clip.addEllipse(QRectF(circle));
            painter->setClipPath(clip);
            avatar.paint(painter, circle);
            painter->setClipping(false);
        } else {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(Theme::Role::SurfaceActive));
            painter->drawEllipse(circle);
            QFont initialsFont = base;
            initialsFont.setPixelSize(10);
            initialsFont.setWeight(QFont::DemiBold);
            painter->setFont(initialsFont);
            painter->setPen(Theme::color(Theme::Role::FgMuted));
            painter->drawText(circle, Qt::AlignCenter, usersNextInitials(title));
        }

        const int textLeft = circle.right() + 11;
        const int textWidth = qMax(0, rect.right() - textLeft);
        const QFont nameFont = font(base, 13, QFont::Medium);
        const QFont handleFont = font(base, 12, QFont::Normal);
        const QFont badgeFont = font(base, 11, QFont::Medium);
        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics handleMetrics(handleFont);
        const QFontMetrics badgeMetrics(badgeFont);
        const QString badge = self ? QStringLiteral("You") : QString();
        const int badgeWidth =
            badge.isEmpty() ? 0 : badgeMetrics.horizontalAdvance(badge) + 12;
        const int block = nameMetrics.height() + handleMetrics.height();
        const int top = rect.center().y() - block / 2 + 1;
        const QString name = nameMetrics.elidedText(
            title, Qt::ElideRight,
            qMax(0, textWidth - (badgeWidth > 0 ? badgeWidth + 6 : 0)));
        const QRect nameRect(textLeft, top, textWidth, nameMetrics.height());
        painter->setFont(nameFont);
        painter->setPen(Theme::color(Theme::Role::Fg));
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, name);
        if (badgeWidth > 0) {
            const int x = textLeft + nameMetrics.horizontalAdvance(name) + 6;
            const QRect pill(x, nameRect.center().y() - 8, badgeWidth, 16);
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(Theme::Role::AccentSubtle));
            painter->drawRoundedRect(pill, 5, 5);
            painter->setFont(badgeFont);
            painter->setPen(Theme::color(Theme::Role::AccentFg));
            painter->drawText(pill, Qt::AlignCenter, badge);
        }
        painter->setFont(handleFont);
        painter->setPen(Theme::color(Theme::Role::FgMuted));
        painter->drawText(
            QRect(textLeft, nameRect.bottom() + 1, textWidth,
                  handleMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            handleMetrics.elidedText(QStringLiteral("@") + handle,
                                     Qt::ElideRight, textWidth));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        const QString handle = index.data(Qt::DisplayRole).toString().trimmed();
        const QString title = titleFor(index, handle);
        const QFontMetrics nameMetrics(font(option.font, 13, QFont::Medium));
        const QFontMetrics handleMetrics(font(option.font, 12, QFont::Normal));
        const QFontMetrics badgeMetrics(font(option.font, 11, QFont::Medium));
        int nameWidth = nameMetrics.horizontalAdvance(title);
        if (isSelf(handle))
            nameWidth += badgeMetrics.horizontalAdvance(QStringLiteral("You")) + 18;
        const int textWidth = qMax(
            nameWidth,
            handleMetrics.horizontalAdvance(QStringLiteral("@") + handle));
        return {qMin(12 + 28 + 11 + textWidth + 16, 360), kUsersNextRowHeight};
    }

private:
    static QFont font(const QFont &base, int pixels, QFont::Weight weight)
    {
        QFont result = base;
        result.setPixelSize(pixels);
        result.setWeight(weight);
        return result;
    }

    QString titleFor(const QModelIndex &index, const QString &handle) const
    {
        const QModelIndex display =
            index.sibling(index.row(), m_displayNameColumn);
        if (!display.isValid() ||
            display.data(kTableSortRole).toString().isEmpty())
            return handle;
        const QString text = display.data(Qt::DisplayRole).toString().trimmed();
        return text.isEmpty() ? handle : text;
    }

    bool isSelf(const QString &handle) const
    {
        if (!m_self || handle.isEmpty())
            return false;
        const QString self = m_self().trimmed();
        return !self.isEmpty() &&
               handle.compare(self, Qt::CaseInsensitive) == 0;
    }

    int m_displayNameColumn = 0;
    std::function<QString()> m_self;
};

class UsersNextDotDelegate final : public QStyledItemDelegate
{
public:
    UsersNextDotDelegate(std::function<Theme::Role(const QModelIndex &)> tone,
                         QObject *parent)
        : QStyledItemDelegate(parent), m_tone(std::move(tone))
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        usersNextRowHover(&opt, index);
        const QString text = opt.text;
        const QFont base = opt.font;
        usersNextDrawBackground(&opt, painter);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect rect = opt.rect.adjusted(12, 0, -8, 0);
        const QRect dot(rect.left(), rect.center().y() - 2, 6, 6);
        painter->setPen(Qt::NoPen);
        painter->setBrush(Theme::color(m_tone ? m_tone(index)
                                              : Theme::Role::FgSubtle));
        painter->drawEllipse(dot);
        painter->setFont(base);
        painter->setPen(Theme::color(Theme::Role::FgSecondary));
        const QRect textRect = rect.adjusted(12, 0, 0, 0);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(base).elidedText(text, Qt::ElideRight,
                                                        textRect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setWidth(size.width() + 12);
        return size;
    }

private:
    std::function<Theme::Role(const QModelIndex &)> m_tone;
};

class UsersNextRowHeight final : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show && Theme::isNext()) {
            if (auto *table = qobject_cast<QTableView *>(watched)) {
                QHeaderView *vertical = table->verticalHeader();
                if (vertical &&
                    vertical->defaultSectionSize() != kUsersNextRowHeight)
                    vertical->setDefaultSectionSize(kUsersNextRowHeight);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

void usersNextDropHiddenSelection(QTableWidget *table)
{
    if (!table || !table->selectionModel())
        return;
    QItemSelectionModel *selection = table->selectionModel();
    QSet<int> rows;
    const QModelIndexList selected = selection->selectedIndexes();
    for (const QModelIndex &index : selected)
        rows.insert(index.row());
    for (int row : std::as_const(rows)) {
        if (table->isRowHidden(row))
            selection->select(table->model()->index(row, 0),
                              QItemSelectionModel::Deselect |
                                  QItemSelectionModel::Rows);
    }
}

void usersNextFilterTable(QTableWidget *table, const QString &needle,
                          int role, const QList<int> &columns,
                          int adminColumn)
{
    if (!table)
        return;
    for (int row = 0; row < table->rowCount(); ++row) {
        bool match = true;
        if (role > 0) {
            const QTableWidgetItem *admin = table->item(row, adminColumn);
            const bool isAdmin =
                admin && admin->data(kTableSortRole).toInt() == 1;
            match = role == 1 ? isAdmin : !isAdmin;
        }
        if (match && !needle.isEmpty()) {
            match = false;
            for (int column : columns) {
                const QTableWidgetItem *cell = table->item(row, column);
                if (cell && cell->text().contains(needle, Qt::CaseInsensitive)) {
                    match = true;
                    break;
                }
            }
        }
        if (table->isRowHidden(row) == match)
            table->setRowHidden(row, !match);
    }
    usersNextDropHiddenSelection(table);
}

void usersNextRefreshFilter(QTabWidget *tabs, QTableWidget *users,
                            QTableWidget *quarantine)
{
    if (!tabs || !Theme::isNext())
        return;
    QWidget *bar = tabs->cornerWidget(Qt::TopRightCorner);
    if (!bar)
        return;
    auto *search = bar->findChild<QLineEdit *>(QStringLiteral("usersNextSearch"));
    auto *role = bar->findChild<QComboBox *>(QStringLiteral("usersNextRoleFilter"));
    auto *shown = bar->findChild<QLabel *>(QStringLiteral("usersNextShown"));
    const QString needle = search ? search->text().trimmed() : QString();
    const int roleIndex = role ? role->currentIndex() : 0;
    usersNextFilterTable(users, needle, roleIndex,
                         {kUsersColName, kUsersColDiscordUsername,
                          kUsersColDiscordDisplayName, kUsersColEmail},
                         kUsersColAdmin);
    usersNextFilterTable(quarantine, needle, roleIndex,
                         {kUsersQuarantineColName,
                          kUsersQuarantineColDiscordUsername,
                          kUsersQuarantineColDiscordDisplayName,
                          kUsersQuarantineColEmail},
                         kUsersQuarantineColAdmin);
    QTableWidget *current = tabs->currentIndex() == 1 ? quarantine : users;
    if (!shown || !current)
        return;
    int count = 0;
    for (int row = 0; row < current->rowCount(); ++row)
        if (!current->isRowHidden(row))
            ++count;
    shown->setText(QStringLiteral("%1 shown").arg(count));
}

void usersNextMoveColumns(QTableWidget *table, const QList<int> &order)
{
    QHeaderView *header = table ? table->horizontalHeader() : nullptr;
    if (!header)
        return;
    for (int target = 0; target < order.size(); ++target) {
        const int from = header->visualIndex(order.at(target));
        if (from >= 0 && from != target)
            header->moveSection(from, target);
    }
}
} // namespace

QWidget *MainWindow::buildUsersSection()
{
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(24, 20, 24, 24);
    outer->setSpacing(12);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(10);

    const bool next = Theme::isNext();
    if (next) {
        page->setProperty("usersNextPage", true);
        outer->setContentsMargins(24, 16, 24, 24);
        header->setSpacing(6);
    }
    QBoxLayout *titleRow = header;
    QVBoxLayout *titleColumn = nullptr;
    if (next) {
        titleColumn = new QVBoxLayout;
        titleColumn->setContentsMargins(0, 0, 0, 0);
        titleColumn->setSpacing(2);
        auto *nameRow = new QHBoxLayout;
        nameRow->setContentsMargins(0, 0, 0, 0);
        nameRow->setSpacing(6);
        titleColumn->addLayout(nameRow);
        header->addLayout(titleColumn);
        titleRow = nameRow;
    }

    auto *title = new QLabel(QStringLiteral("Users"));
    title->setObjectName(QStringLiteral("sectionTitle"));
    if (!next) {
        QFont titleFont = title->font();
        titleFont = forkmesh::resizedFont(titleFont, 4);
        titleFont.setBold(true);
        title->setFont(titleFont);
    }
    titleRow->addWidget(title);
    titleRow->addWidget(makeInlineHelpButton(
        QStringLiteral("About user statistics"),
        QStringLiteral(
            "Administrator-only account directory containing every users-table "
            "record, including private and non-active accounts. Public activity "
            "facts match World avatar chests; protected identity, email, and "
            "bounded signup evidence stay on this screen for account review. "
            "Quarantine shows every preserved account and investigation field; "
            "select one record there to restore the account and linked nodes. "
            "Hold Shift and use Up/Down to select a range for quarantine or "
            "deletion.")));

    m_usersStatus = new QLabel(QStringLiteral("Loading users..."));
    m_usersStatus->setObjectName(QStringLiteral("mutedLabel"));
    if (titleColumn) {
        titleRow->addStretch(1);
        titleColumn->addWidget(m_usersStatus);
        header->addStretch(1);
    } else {
        header->addWidget(m_usersStatus, 1);
    }

    m_usersQuarantineButton =
        new QPushButton(QString::fromUtf8("Quarantine\xE2\x80\xA6"));
    m_usersQuarantineButton->setObjectName(QStringLiteral("dangerButton"));
    m_usersQuarantineButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_usersQuarantineButton, "alert", 14);
    m_usersQuarantineButton->setEnabled(false);
    connect(m_usersQuarantineButton, &QPushButton::clicked, this,
            &MainWindow::quarantineSelectedUsers);
    header->addWidget(m_usersQuarantineButton);

    m_usersUnquarantineButton =
        new QPushButton(QStringLiteral("Unquarantine…"));
    m_usersUnquarantineButton->setObjectName(QStringLiteral("repoAction"));
    m_usersUnquarantineButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_usersUnquarantineButton, "history", 14);
    m_usersUnquarantineButton->setEnabled(false);
    connect(m_usersUnquarantineButton, &QPushButton::clicked, this,
            &MainWindow::unquarantineSelectedUser);
    header->addWidget(m_usersUnquarantineButton);

    // The irreversible neighbour of Quarantine: that one preserves the record
    // for review, this one erases the member and everything attached to it.
    m_usersDeleteButton = new QPushButton(QStringLiteral("Delete…"));
    m_usersDeleteButton->setObjectName(QStringLiteral("dangerButton"));
    m_usersDeleteButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_usersDeleteButton, "trash", 14);
    m_usersDeleteButton->setEnabled(false);
    connect(m_usersDeleteButton, &QPushButton::clicked, this,
            &MainWindow::deleteSelectedUsers);
    header->addWidget(m_usersDeleteButton);

    // Tipping lives here as well as on a member's profile: this directory is
    // where members are actually found. It acts on the selected row and stays
    // disabled until that row publishes a payout address. See MainWindowTips.cpp.
    m_usersTipButton = new QPushButton(QString::fromUtf8("Send tip\xE2\x80\xA6"));
    m_usersTipButton->setObjectName(QStringLiteral("repoAction"));
    m_usersTipButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_usersTipButton, "credit-card", 14);
    m_usersTipButton->setEnabled(false);
    connect(m_usersTipButton, &QPushButton::clicked, this,
            &MainWindow::sendTipToSelectedUser);
    header->addWidget(m_usersTipButton);

    m_usersRefreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_usersRefreshButton->setObjectName(QStringLiteral("repoAction"));
    m_usersRefreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_usersRefreshButton, "sync", 14);
    connect(m_usersRefreshButton, &QPushButton::clicked, this,
            [this] { refreshUsersPage(/*force=*/true); });
    header->addWidget(m_usersRefreshButton);
    outer->addLayout(header);

    m_usersSignupReferralCodePanel = new QWidget;
    m_usersSignupReferralCodePanel->setObjectName(
        QStringLiteral("usersSignupReferralCodePanel"));
    auto *codeLayout = new QHBoxLayout(m_usersSignupReferralCodePanel);
    codeLayout->setContentsMargins(12, 10, 12, 10);
    codeLayout->setSpacing(10);
    auto *codeTitle = new QLabel(QStringLiteral("Signup referral code"));
    QFont codeTitleFont = codeTitle->font();
    codeTitleFont.setBold(true);
    codeTitle->setFont(codeTitleFont);
    codeLayout->addWidget(codeTitle);
    m_usersSignupReferralCodeStatus = new QLabel(QStringLiteral("Loading..."));
    m_usersSignupReferralCodeStatus->setObjectName(QStringLiteral("mutedLabel"));
    codeLayout->addWidget(m_usersSignupReferralCodeStatus, 1);
    m_usersSignupReferralCodeEdit = new QLineEdit;
    m_usersSignupReferralCodeEdit->setObjectName(
        QStringLiteral("usersSignupReferralCode"));
    m_usersSignupReferralCodeEdit->setReadOnly(true);
    m_usersSignupReferralCodeEdit->setMaximumWidth(260);
    m_usersSignupReferralCodeEdit->setPlaceholderText(
        QStringLiteral("Waiting for the server"));
    m_usersSignupReferralCodeEdit->setFont(
        QFontDatabase::systemFont(QFontDatabase::FixedFont));
    codeLayout->addWidget(m_usersSignupReferralCodeEdit);
    m_usersSignupReferralCodeCopyButton =
        new QPushButton(QStringLiteral("Copy code"));
    m_usersSignupReferralCodeCopyButton->setObjectName(
        QStringLiteral("repoAction"));
    m_usersSignupReferralCodeCopyButton->setEnabled(false);
    setOcticon(m_usersSignupReferralCodeCopyButton, "copy", 14);
    connect(m_usersSignupReferralCodeCopyButton, &QPushButton::clicked, this,
            [this] {
                const QString code = m_usersSignupReferralCode;
                if (!m_isAdmin || code.isEmpty())
                    return;
                QApplication::clipboard()->setText(code);
                m_usersSignupReferralCodeCopyButton->setText(
                    QStringLiteral("Copied"));
                QTimer::singleShot(60000, QApplication::clipboard(), [code] {
                    if (QApplication::clipboard()->text() == code)
                        QApplication::clipboard()->clear();
                });
                QTimer::singleShot(1800, this, [this] {
                    if (m_usersSignupReferralCodeCopyButton)
                        m_usersSignupReferralCodeCopyButton->setText(
                            QStringLiteral("Copy code"));
                });
            });
    codeLayout->addWidget(m_usersSignupReferralCodeCopyButton);
    if (next)
        m_usersSignupReferralCodePanel->setAttribute(Qt::WA_StyledBackground,
                                                     true);
    outer->addWidget(m_usersSignupReferralCodePanel);

    m_usersTabs = new QTabWidget;
    m_usersTabs->setObjectName(QStringLiteral("usersTabs"));
    m_usersTabs->setDocumentMode(true);

    m_usersTable = new QTableWidget(0, kUsersColCount);
    installColumnHeaderMenu(m_usersTable);
    m_usersTable->setObjectName(QStringLiteral("usersTable"));
    m_usersTable->setProperty("dataTable", true);
    m_usersTable->setHorizontalHeaderLabels(usersTableHeaders());
    m_usersTable->verticalHeader()->setVisible(false);
    m_usersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // ExtendedSelection gives the directory the desktop conventions people
    // already know: Shift+Up/Down extends from the current row, Shift-click
    // selects a range, and Ctrl/Cmd-click toggles individual rows.
    m_usersTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_usersTable->setToolTip(QStringLiteral(
        "Select one or more users. Hold Shift and use Up/Down to extend the "
        "selection."));
    m_usersTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_usersTable->setAlternatingRowColors(true);
    m_usersTable->setShowGrid(false);
    m_usersTable->setSortingEnabled(true);
    m_usersTable->horizontalHeader()->setSortIndicator(
        kUsersColJoined, Qt::DescendingOrder);
    networkPrepareFullTable(m_usersTable);
    m_usersTable->horizontalHeader()->setResizeContentsPrecision(
        kUsersResizeContentsPrecision);
    for (int column = 0; column < m_usersTable->columnCount(); ++column)
        m_usersTable->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_usersTable);
    connect(m_usersTable, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::refreshUsersTipButton);
    connect(m_usersTable, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::refreshUsersQuarantineButton);
    connect(m_usersTable, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::refreshUsersDeleteButton);
    m_usersTabs->addTab(m_usersTable, QStringLiteral("All users"));

    m_usersQuarantineTable =
        new QTableWidget(0, kUsersQuarantineColCount);
    installColumnHeaderMenu(m_usersQuarantineTable);
    m_usersQuarantineTable->setObjectName(
        QStringLiteral("usersQuarantineTable"));
    m_usersQuarantineTable->setProperty("dataTable", true);
    m_usersQuarantineTable->setHorizontalHeaderLabels(
        usersQuarantineTableHeaders());
    m_usersQuarantineTable->verticalHeader()->setVisible(false);
    m_usersQuarantineTable->setSelectionBehavior(
        QAbstractItemView::SelectRows);
    m_usersQuarantineTable->setSelectionMode(
        QAbstractItemView::SingleSelection);
    m_usersQuarantineTable->setEditTriggers(
        QAbstractItemView::NoEditTriggers);
    m_usersQuarantineTable->setAlternatingRowColors(true);
    m_usersQuarantineTable->setShowGrid(false);
    m_usersQuarantineTable->setSortingEnabled(true);
    m_usersQuarantineTable->horizontalHeader()->setSortIndicator(
        kUsersQuarantineColQuarantined, Qt::DescendingOrder);
    networkPrepareFullTable(m_usersQuarantineTable);
    m_usersQuarantineTable->horizontalHeader()->setResizeContentsPrecision(
        kUsersResizeContentsPrecision);
    for (int column = 0;
         column < m_usersQuarantineTable->columnCount(); ++column) {
        m_usersQuarantineTable->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    makeColumnsResizable(m_usersQuarantineTable);
    connect(m_usersQuarantineTable,
            &QTableWidget::itemSelectionChanged, this,
            &MainWindow::refreshUsersUnquarantineButton);
    m_usersTabs->addTab(
        m_usersQuarantineTable, QStringLiteral("Quarantine"));
    connect(m_usersTabs, &QTabWidget::currentChanged, this,
            [this](int index) {
                if (index != 0 && m_usersTable)
                    m_usersTable->clearSelection();
                refreshUsersTipButton();
                refreshUsersQuarantineButton();
                refreshUsersUnquarantineButton();
                refreshUsersDeleteButton();
                updateUsersStatus();
            });
    if (next) {
        auto *bar = new QWidget;
        bar->setObjectName(QStringLiteral("usersNextToolbar"));
        auto *barLayout = new QHBoxLayout(bar);
        barLayout->setContentsMargins(8, 0, 0, 4);
        barLayout->setSpacing(8);
        auto *search = new QLineEdit(bar);
        search->setObjectName(QStringLiteral("usersNextSearch"));
        search->setPlaceholderText(QStringLiteral("Search people"));
        search->setClearButtonEnabled(true);
        search->setFixedWidth(240);
        search->addAction(QIcon(kit::tintedIcon(
                              QStringLiteral("search"),
                              Theme::color(Theme::Role::FgSubtle), 14,
                              search->devicePixelRatioF())),
                          QLineEdit::LeadingPosition);
        auto *role = new QComboBox(bar);
        role->setObjectName(QStringLiteral("usersNextRoleFilter"));
        role->addItems({QStringLiteral("All roles"), QStringLiteral("Admins"),
                        QStringLiteral("Not admins")});
        role->setToolTip(QStringLiteral("Show everyone, only admins, or only "
                                        "members who are not admins"));
        auto *shown = new QLabel(bar);
        shown->setObjectName(QStringLiteral("usersNextShown"));
        barLayout->addWidget(search);
        barLayout->addWidget(role);
        barLayout->addWidget(shown);
        m_usersTabs->setCornerWidget(bar, Qt::TopRightCorner);
        connect(search, &QLineEdit::textChanged, this, [this] {
            usersNextRefreshFilter(m_usersTabs, m_usersTable,
                                   m_usersQuarantineTable);
        });
        connect(role, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] {
                    usersNextRefreshFilter(m_usersTabs, m_usersTable,
                                           m_usersQuarantineTable);
                });

        const auto self = [this] { return accountOwner(); };
        auto *rowHeight = new UsersNextRowHeight(page);
        const auto dressTable = [this, self, rowHeight](
                                    QTableWidget *table, int displayColumn,
                                    const QList<QPointer<QPushButton>> &actions) {
            table->setItemDelegateForColumn(
                0, new UsersNextPersonDelegate(displayColumn, self, table));
            table->verticalHeader()->setDefaultSectionSize(kUsersNextRowHeight);
            table->installEventFilter(rowHeight);
            connect(table->selectionModel(),
                    &QItemSelectionModel::selectionChanged, table,
                    [table] { usersNextDropHiddenSelection(table); });
            table->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(table, &QWidget::customContextMenuRequested, this,
                    [this, table, actions](const QPoint &pos) {
                        QTableWidgetItem *item = table->itemAt(pos);
                        if (item && !item->isSelected())
                            table->selectRow(item->row());
                        QMenu menu(this);
                        QHash<QAction *, QPointer<QPushButton>> mapped;
                        for (const QPointer<QPushButton> &button : actions) {
                            if (!button) {
                                if (!menu.isEmpty())
                                    menu.addSeparator();
                                continue;
                            }
                            if (button->isHidden())
                                continue;
                            QAction *action =
                                menu.addAction(button->icon(), button->text());
                            action->setEnabled(button->isEnabled());
                            action->setToolTip(button->toolTip());
                            mapped.insert(action, button);
                        }
                        QAction *chosen =
                            menu.exec(table->viewport()->mapToGlobal(pos));
                        const QPointer<QPushButton> button = mapped.value(chosen);
                        if (button)
                            button->click();
                    });
        };
        dressTable(m_usersTable, kUsersColDiscordDisplayName,
                   {m_usersTipButton, m_usersQuarantineButton,
                    m_usersDeleteButton, nullptr,
                    m_usersRefreshButton});
        dressTable(m_usersQuarantineTable,
                   kUsersQuarantineColDiscordDisplayName,
                   {m_usersUnquarantineButton, nullptr,
                    m_usersRefreshButton});
        const auto tone = [](Theme::Role on, Theme::Role off) {
            return [on, off](const QModelIndex &index) {
                return index.data(kTableSortRole).toInt() == 1 ? on : off;
            };
        };
        m_usersTable->setItemDelegateForColumn(
            kUsersColVerified,
            new UsersNextDotDelegate(
                tone(Theme::Role::Success, Theme::Role::Warning), m_usersTable));
        m_usersTable->setItemDelegateForColumn(
            kUsersColStatus,
            new UsersNextDotDelegate(
                [](const QModelIndex &index) {
                    return index.data(Qt::DisplayRole)
                                       .toString()
                                       .compare(QStringLiteral("active"),
                                                Qt::CaseInsensitive) == 0
                               ? Theme::Role::Success
                               : Theme::Role::FgSubtle;
                },
                m_usersTable));
        m_usersQuarantineTable->setItemDelegateForColumn(
            kUsersQuarantineColVerified,
            new UsersNextDotDelegate(
                tone(Theme::Role::Success, Theme::Role::Warning),
                m_usersQuarantineTable));
        usersNextMoveColumns(m_usersTable,
                             {kUsersColName, kUsersColAdmin, kUsersColNodes,
                              kUsersColVerified, kUsersColStatus,
                              kUsersColActivityRecency});
        usersNextMoveColumns(m_usersQuarantineTable,
                             {kUsersQuarantineColName,
                              kUsersQuarantineColAdmin,
                              kUsersQuarantineColNodes,
                              kUsersQuarantineColStatus,
                              kUsersQuarantineColReasons,
                              kUsersQuarantineColQuarantined});
    }
    outer->addWidget(m_usersTabs, 1);

    renderUsersPage(m_usersDirectoryPayload);
    renderUsersQuarantinePage(m_usersQuarantinePayload);
    renderUsersSignupReferralCode(QJsonObject());
    refreshUsersTipButton();
    refreshUsersQuarantineButton();
    refreshUsersUnquarantineButton();
    refreshUsersDeleteButton();
    return page;
}

void MainWindow::renderUsersSignupReferralCode(const QJsonObject &state)
{
    if (!m_usersSignupReferralCodeEdit ||
        !m_usersSignupReferralCodeStatus ||
        !m_usersSignupReferralCodeCopyButton)
        return;
    const QString previousCode = m_usersSignupReferralCode;
    m_usersSignupReferralCode.clear();
    m_usersSignupReferralCodeEdit->clear();
    m_usersSignupReferralCodeEdit->setToolTip(QString());
    m_usersSignupReferralCodeCopyButton->setText(QStringLiteral("Copy code"));
    m_usersSignupReferralCodeCopyButton->setEnabled(false);
    if (!m_isAdmin) {
        if (!previousCode.isEmpty() &&
            QApplication::clipboard()->text() == previousCode)
            QApplication::clipboard()->clear();
        m_usersSignupReferralCodeStatus->setText(QStringLiteral("Admin only"));
        m_usersSignupReferralCodeEdit->setPlaceholderText(QString());
        return;
    }
    if (state.isEmpty()) {
        m_usersSignupReferralCodeStatus->setText(QStringLiteral("Loading..."));
        m_usersSignupReferralCodeEdit->setPlaceholderText(
            QStringLiteral("Waiting for the server"));
        return;
    }
    if (!state.value(QStringLiteral("enabled")).toBool()) {
        m_usersSignupReferralCodeStatus->setText(
            QStringLiteral("Off - /signup does not require a code"));
        m_usersSignupReferralCodeEdit->setPlaceholderText(
            QStringLiteral("Code mode is off"));
        return;
    }
    const QString code =
        state.value(QStringLiteral("code")).toString().trimmed();
    const int generation = qMax(
        1, static_cast<int>(
               state.value(QStringLiteral("generation")).toDouble(1)));
    if (code.isEmpty()) {
        m_usersSignupReferralCodeStatus->setText(
            QStringLiteral("Required - current code unavailable"));
        m_usersSignupReferralCodeEdit->setPlaceholderText(
            QStringLiteral("Refresh to try again"));
        return;
    }
    m_usersSignupReferralCode = code;
    m_usersSignupReferralCodeEdit->setText(code);
    m_usersSignupReferralCodeEdit->setPlaceholderText(QString());
    m_usersSignupReferralCodeCopyButton->setEnabled(true);
    m_usersSignupReferralCodeStatus->setText(
        QStringLiteral("Required - one use - generation %1").arg(generation));
    m_usersSignupReferralCodeEdit->setToolTip(
        QStringLiteral(
            "This code is replaced immediately after a successful signup."));
}

void MainWindow::refreshUsersPage(bool force)
{
    if (!m_isAdmin || !m_usersTable || !m_usersStatus)
        return;
    const bool needsAdminPayload =
        m_isAdmin && !m_usersDirectoryAdminPayload;
    if (m_chatDirectoryLoaded)
        renderUsersPage(m_usersDirectoryPayload);
    renderUsersQuarantinePage(m_usersQuarantinePayload);
    if (force || needsAdminPayload)
        m_chatDirectoryFetchedMs = 0;
    if (force || needsAdminPayload || !m_chatDirectoryLoaded)
        m_usersStatus->setText(QStringLiteral("Loading users..."));
    refreshChatUserDirectory();
}

void MainWindow::renderUsersPage(const QJsonArray &users)
{
    if (!m_usersTable)
        return;

    const int sortColumn = qBound(
        0, m_usersTable->horizontalHeader()->sortIndicatorSection(),
        m_usersTable->columnCount() - 1);
    const Qt::SortOrder sortOrder =
        m_usersTable->horizontalHeader()->sortIndicatorOrder();
    TableRepaintGuard repaintGuard(m_usersTable);
    m_usersTable->setSortingEnabled(false);
    m_usersTable->setRowCount(0);

    for (const QJsonValue &value : users) {
        const QJsonObject user = value.toObject();
        if (user.value(QStringLiteral("kind"))
                .toString(QStringLiteral("user")) != QLatin1String("user"))
            continue;
        const QString name =
            user.value(QStringLiteral("name"))
                .toString(user.value(QStringLiteral("nodeName")).toString())
                .trimmed();
        if (name.isEmpty())
            continue;

        const int row = m_usersTable->rowCount();
        m_usersTable->insertRow(row);

        fillUsersIdentityCells(row, user, name);
        fillUsersReferralCells(row, user);
        fillUsersSignupCells(row, user);
        fillUsersActivityCells(row, user);
    }

    m_usersTable->setSortingEnabled(true);
    m_usersTable->sortItems(sortColumn, sortOrder);
    updateUsersStatus();
    // Rebuilding the rows drops the selection, so the send button has to be
    // re-evaluated against whatever (if anything) is selected now.
    refreshUsersTipButton();
    refreshUsersQuarantineButton();
    refreshUsersDeleteButton();
}

// Users table: name, Discord, profile flags and email columns for one row.
void MainWindow::fillUsersIdentityCells(int row, const QJsonObject &user,
                                        const QString &name)
{
    auto *nameItem = usersTableItem(name, name.toLower());
    const auto avatar = m_avatars.constFind(
        QStringLiteral("user:") + name.toLower());
    if (avatar != m_avatars.constEnd())
        nameItem->setIcon(QIcon(*avatar));
    m_usersTable->setItem(row, kUsersColName, nameItem);

    const QString discordUsername =
        user.value(QStringLiteral("discordUsername")).toString().trimmed();
    const QString discordDisplayName =
        user.value(QStringLiteral("discordDisplayName")).toString().trimmed();
    const bool discordLinked =
        user.value(QStringLiteral("discordLinked")).toBool();
    const QString discordText =
        !discordUsername.isEmpty()
            ? discordUsername
            : (discordLinked
                   ? QStringLiteral("Linked - username unavailable")
                   : QStringLiteral("Not linked"));
    auto *discordItem = usersTableItem(
        discordText, discordUsername.isEmpty()
                         ? discordText.toLower()
                         : discordUsername.toLower());
    if (!discordDisplayName.isEmpty())
        discordItem->setToolTip(
            QStringLiteral("Discord display name: %1")
                .arg(discordDisplayName));
    m_usersTable->setItem(
        row, kUsersColDiscordUsername, discordItem);
    const QString discordId =
        user.value(QStringLiteral("discordId")).toString().trimmed();
    m_usersTable->setItem(
        row, kUsersColDiscordDisplayName,
        usersTableItem(discordDisplayName.isEmpty()
                           ? QStringLiteral("Unavailable")
                           : discordDisplayName,
                       discordDisplayName.toLower()));
    m_usersTable->setItem(
        row, kUsersColDiscordId,
        usersTableItem(discordId.isEmpty() ? QStringLiteral("Unavailable")
                                            : discordId,
                       discordId));

    const bool profilePrivate =
        user.value(QStringLiteral("profilePrivate")).toBool();
    m_usersTable->setItem(
        row, kUsersColProfileVisibility,
        usersTableItem(profilePrivate ? QStringLiteral("Private")
                                      : QStringLiteral("Public"),
                       profilePrivate ? 1 : 0));
    const bool administrator =
        user.value(QStringLiteral("isAdmin")).toBool();
    m_usersTable->setItem(
        row, kUsersColAdmin,
        usersTableItem(administrator ? QStringLiteral("Yes")
                                     : QStringLiteral("No"),
                       administrator ? 1 : 0));
    const bool outreach =
        user.value(QStringLiteral("outreachEnabled")).toBool();
    m_usersTable->setItem(
        row, kUsersColOutreach,
        usersTableItem(outreach ? QStringLiteral("Enabled")
                                : QStringLiteral("Disabled"),
                       outreach ? 1 : 0));
    const bool emailNotifications =
        user.value(QStringLiteral("emailNotifications")).toBool(true);
    m_usersTable->setItem(
        row, kUsersColEmailNotifications,
        usersTableItem(emailNotifications ? QStringLiteral("Enabled")
                                           : QStringLiteral("Disabled"),
                       emailNotifications ? 1 : 0));

    const QString email =
        user.value(QStringLiteral("email")).toString().trimmed();
    m_usersTable->setItem(
        row, kUsersColEmail,
        usersTableItem(
            email.isEmpty() ? QStringLiteral("Unavailable") : email,
            email.toLower()));
}

// Users table: referral attribution, risk, click counters and referred users.
void MainWindow::fillUsersReferralCells(int row, const QJsonObject &user)
{
    const QString referralStatus =
        user.value(QStringLiteral("referralStatus")).toString().trimmed();
    const QString referredBy =
        user.value(QStringLiteral("referredBy")).toString().trimmed();
    QString referredByText = referredBy;
    if (referredByText.isEmpty()) {
        if (referralStatus == QLatin1String("legacy-unattributed"))
            referredByText = QStringLiteral("Not recorded (legacy)");
        else if (referralStatus == QLatin1String("capture-failed"))
            referredByText = QStringLiteral("Unavailable");
        else
            referredByText = QStringLiteral("Direct");
    }
    m_usersTable->setItem(
        row, kUsersColReferredBy,
        usersTableItem(referredByText, referredByText.toLower()));

    m_usersTable->setItem(
        row, kUsersColReferralStatus,
        usersTableItem(
            referralStatus.isEmpty() ? QStringLiteral("Direct")
                                     : referralStatus,
            referralStatus.toLower()));
    const QString clickId = user.value(
        QStringLiteral("referralClickId")).toString().trimmed();
    m_usersTable->setItem(
        row, kUsersColReferralClickId,
        usersTableItem(clickId.isEmpty() ? QStringLiteral("Unavailable")
                                          : clickId,
                       clickId.toLower()));

    QStringList referralRisk;
    for (const QJsonValue &flag :
         user.value(QStringLiteral("referralRiskFlags")).toArray()) {
        const QString clean = flag.toString().trimmed();
        if (!clean.isEmpty())
            referralRisk.append(clean);
    }
    const int referralRiskScore = static_cast<int>(
        user.value(QStringLiteral("referralRiskScore")).toDouble());
    const QString riskText = QStringLiteral("%1 · %2")
                                 .arg(referralRiskScore)
                                 .arg(referralRisk.isEmpty()
                                          ? QStringLiteral("None")
                                          : referralRisk.join(
                                                QStringLiteral(", ")));
    auto *riskItem = usersTableItem(riskText, referralRiskScore);
    const QString clientFingerprint = user.value(
        QStringLiteral("signupClientFingerprint")).toString().trimmed();
    if (!clientFingerprint.isEmpty() || !clickId.isEmpty())
        riskItem->setToolTip(
            QStringLiteral("Client fingerprint: %1\nClick claim: %2")
                .arg(clientFingerprint.isEmpty()
                         ? QStringLiteral("Unavailable")
                         : clientFingerprint)
                .arg(clickId.isEmpty() ? QStringLiteral("Unavailable")
                                       : clickId));
    m_usersTable->setItem(row, kUsersColReferralRisk, riskItem);

    const auto referralTimestamp = [&](const char *field, int column) {
        const qint64 value = static_cast<qint64>(
            user.value(QLatin1String(field)).toDouble());
        m_usersTable->setItem(
            row, column,
            usersTableItem(value > 0 ? usersTimestamp(value)
                                     : QStringLiteral("Not set"),
                           value));
    };
    referralTimestamp("referralCreatedAt", kUsersColReferralCreated);
    referralTimestamp("referralVerifiedAt", kUsersColReferralVerified);
    referralTimestamp("referralCreditedAt", kUsersColReferralCredited);

    const int referralClicks = static_cast<int>(
        user.value(QStringLiteral("referralClicks")).toDouble());
    const int referralAuditedUniqueClicks = static_cast<int>(
        user.value(
            QStringLiteral("referralAuditedUniqueClicks")).toDouble());
    const int referralDuplicateClicks = static_cast<int>(
        user.value(QStringLiteral(
            "referralDuplicateClickAttempts")).toDouble());
    const int referralRejectedClicks = static_cast<int>(
        user.value(QStringLiteral(
            "referralRejectedClickAttempts")).toDouble());
    const int referralLegacyClicks = static_cast<int>(
        user.value(QStringLiteral("referralLegacyClickCount")).toDouble());
    const int legacyUnattributed = static_cast<int>(
        user.value(QStringLiteral("legacyUnattributed")).toDouble());
    const int creditedReferrals = static_cast<int>(
        user.value(QStringLiteral("creditedReferrals")).toDouble());
    m_usersTable->setItem(
        row, kUsersColReferralClicks,
        usersTableItem(QString::number(referralClicks), referralClicks));
    m_usersTable->setItem(
        row, kUsersColReferralAuditedUniqueClicks,
        usersTableItem(QString::number(referralAuditedUniqueClicks),
                       referralAuditedUniqueClicks));
    m_usersTable->setItem(
        row, kUsersColReferralDuplicateClicks,
        usersTableItem(QString::number(referralDuplicateClicks),
                       referralDuplicateClicks));
    m_usersTable->setItem(
        row, kUsersColReferralRejectedClicks,
        usersTableItem(QString::number(referralRejectedClicks),
                       referralRejectedClicks));
    m_usersTable->setItem(
        row, kUsersColReferralLegacyClicks,
        usersTableItem(QString::number(referralLegacyClicks),
                       referralLegacyClicks));
    m_usersTable->setItem(
        row, kUsersColLegacyUnattributed,
        usersTableItem(QString::number(legacyUnattributed),
                       legacyUnattributed));
    m_usersTable->setItem(
        row, kUsersColCreditedReferrals,
        usersTableItem(QString::number(creditedReferrals),
                       creditedReferrals));

    QStringList referredUsers;
    for (const QJsonValue &referred :
         user.value(QStringLiteral("referredUsers")).toArray()) {
        const QString clean = referred.toString().trimmed();
        if (!clean.isEmpty() &&
            !referredUsers.contains(clean, Qt::CaseInsensitive))
            referredUsers.append(clean);
    }
    QString referredUsersText = referredUsers.join(QStringLiteral(", "));
    if (referredUsersText.isEmpty())
        referredUsersText = QStringLiteral("None");
    auto *referredUsersItem = usersTableItem(
        referredUsersText, referredUsersText.toLower());
    if (!referredUsers.isEmpty())
        referredUsersItem->setToolTip(referredUsersText);
    m_usersTable->setItem(
        row, kUsersColReferredUsers, referredUsersItem);
    referralTimestamp("referralLastAt", kUsersColReferralLastActivity);
}

// Users table: signup network, agent and fingerprint columns, wallets, status.
void MainWindow::fillUsersSignupCells(int row, const QJsonObject &user)
{
    const QString signupIp =
        user.value(QStringLiteral("signupIp")).toString().trimmed();
    m_usersTable->setItem(
        row, kUsersColSignupIp,
        usersTableItem(
            signupIp.isEmpty() ? QStringLiteral("Unavailable") : signupIp,
            signupIp));
    const auto fingerprintItem = [&](const char *field, int column) {
        const QString fingerprint =
            user.value(QLatin1String(field)).toString().trimmed();
        const QString visible = fingerprint.size() > 18
                                    ? fingerprint.left(18) +
                                          QString::fromUtf8("\xE2\x80\xA6")
                                    : fingerprint;
        auto *item = usersTableItem(
            visible.isEmpty() ? QStringLiteral("Unavailable") : visible,
            fingerprint.toLower());
        if (!fingerprint.isEmpty())
            item->setToolTip(fingerprint);
        m_usersTable->setItem(row, column, item);
    };
    fingerprintItem("signupIpFingerprint", kUsersColSignupIpFingerprint);

    const QString signupAgent =
        user.value(QStringLiteral("signupUserAgent")).toString().trimmed();
    const QString visibleAgent = signupAgent.size() > 80
                                     ? signupAgent.left(80) +
                                           QString::fromUtf8("\xE2\x80\xA6")
                                     : signupAgent;
    auto *agentItem = usersTableItem(
        visibleAgent.isEmpty() ? QStringLiteral("Unavailable")
                               : visibleAgent,
        signupAgent.toLower());
    if (!signupAgent.isEmpty())
        agentItem->setToolTip(signupAgent);
    m_usersTable->setItem(row, kUsersColSignupAgent, agentItem);
    fingerprintItem(
        "signupUserAgentFingerprint", kUsersColSignupAgentFingerprint);
    fingerprintItem(
        "signupClientFingerprint", kUsersColSignupClientFingerprint);
    fingerprintItem(
        "referralNetworkFingerprint",
        kUsersColReferralNetworkFingerprint);
    fingerprintItem("userFingerprint", kUsersColUserFingerprint);
    fingerprintItem("emailFingerprint", kUsersColEmailFingerprint);
    fingerprintItem("discordFingerprint", kUsersColDiscordFingerprint);

    const QString signupCountry =
        user.value(QStringLiteral("signupCountry")).toString().trimmed();
    const QString signupClient =
        user.value(QStringLiteral("signupClientCategory")).toString().trimmed();
    m_usersTable->setItem(
        row, kUsersColSignupCountry,
        usersTableItem(
            signupCountry.isEmpty() ? QStringLiteral("Unavailable")
                                    : signupCountry,
            signupCountry));
    m_usersTable->setItem(
        row, kUsersColSignupClient,
        usersTableItem(
            signupClient.isEmpty() ? QStringLiteral("Unavailable")
                                   : signupClient,
            signupClient.toLower()));

    const QString solana =
        user.value(QStringLiteral("solana")).toString().trimmed();
    m_usersTable->setItem(
        row, kUsersColSolana,
        usersTableItem(solana.isEmpty() ? QStringLiteral("Not set")
                                         : solana,
                       solana.toLower()));
    const QString mastodon =
        user.value(QStringLiteral("mastodon")).toString().trimmed();
    m_usersTable->setItem(
        row, kUsersColMastodon,
        usersTableItem(mastodon.isEmpty() ? QStringLiteral("Not set")
                                           : mastodon,
                       mastodon.toLower()));

    const bool verified =
        user.value(QStringLiteral("emailVerified")).toBool();
    m_usersTable->setItem(
        row, kUsersColVerified,
        usersTableItem(verified ? QStringLiteral("Yes")
                                : QStringLiteral("No"),
                       verified ? 1 : 0));

    const QString status =
        user.value(QStringLiteral("status")).toString(QStringLiteral("active"));
    m_usersTable->setItem(
        row, kUsersColStatus, usersTableItem(status, status.toLower()));
}

// Users table: join/avatar dates, World activity, contributions, email
// engagement, locale and linked nodes.
void MainWindow::fillUsersActivityCells(int row, const QJsonObject &user)
{
    const qint64 joined = static_cast<qint64>(
        user.value(QStringLiteral("createdAt")).toDouble());
    m_usersTable->setItem(
        row, kUsersColJoined, usersTableItem(usersTimestamp(joined), joined));
    const qint64 avatarUpdated = static_cast<qint64>(
        user.value(QStringLiteral("avatarUpdatedAt")).toDouble());
    m_usersTable->setItem(
        row, kUsersColAvatarUpdated,
        usersTableItem(avatarUpdated > 0 ? usersTimestamp(avatarUpdated)
                                         : QStringLiteral("Never"),
                       avatarUpdated));

    const qint64 totalActiveMs = static_cast<qint64>(
        user.value(QStringLiteral("totalActiveMs")).toDouble());
    m_usersTable->setItem(
        row, kUsersColWorldActivity,
        usersTableItem(formatDuration(totalActiveMs), totalActiveMs));
    const int totalSteps = static_cast<int>(
        user.value(QStringLiteral("totalSteps")).toDouble());
    const int officeVisits = static_cast<int>(
        user.value(QStringLiteral("officeVisits")).toDouble());
    m_usersTable->setItem(
        row, kUsersColWorldSteps,
        usersTableItem(QString::number(totalSteps), totalSteps));
    m_usersTable->setItem(
        row, kUsersColOfficeVisits,
        usersTableItem(QString::number(officeVisits), officeVisits));

    const auto recency = usersActivityRecency(
        user.value(QStringLiteral("activityBucket")).toString());
    m_usersTable->setItem(
        row, kUsersColActivityRecency,
        usersTableItem(recency.first, recency.second));

    const int pulls =
        static_cast<int>(user.value(QStringLiteral("pulls")).toDouble());
    m_usersTable->setItem(
        row, kUsersColPulls,
        usersTableItem(QString::number(pulls), pulls));

    const int issues =
        static_cast<int>(user.value(QStringLiteral("issues")).toDouble());
    m_usersTable->setItem(
        row, kUsersColIssues,
        usersTableItem(QString::number(issues), issues));

    const int commits =
        static_cast<int>(user.value(QStringLiteral("commits")).toDouble());
    m_usersTable->setItem(
        row, kUsersColCommits,
        usersTableItem(QString::number(commits), commits));

    const int discussions =
        static_cast<int>(user.value(QStringLiteral("discussions")).toDouble());
    m_usersTable->setItem(
        row, kUsersColDiscussions,
        usersTableItem(QString::number(discussions), discussions));

    const qint64 lastEmail = static_cast<qint64>(
        user.value(QStringLiteral("lastEmailAt")).toDouble());
    m_usersTable->setItem(
        row, kUsersColLastEmail,
        usersTableItem(lastEmail > 0 ? usersTimestamp(lastEmail)
                                     : QStringLiteral("Never"),
                       lastEmail));

    const QString delivery =
        user.value(QStringLiteral("lastEmailStatus")).toString();
    m_usersTable->setItem(
        row, kUsersColEmailDelivery,
        usersTableItem(delivery.isEmpty() ? QStringLiteral("Not sent")
                                          : delivery,
                       delivery.toLower()));

    const int emailOpens = static_cast<int>(
        user.value(QStringLiteral("emailOpens")).toDouble());
    const int emailClicks = static_cast<int>(
        user.value(QStringLiteral("emailClicks")).toDouble());
    m_usersTable->setItem(
        row, kUsersColEmailOpens,
        usersTableItem(QString::number(emailOpens), emailOpens));
    m_usersTable->setItem(
        row, kUsersColEmailClicks,
        usersTableItem(QString::number(emailClicks), emailClicks));
    const qint64 lastEmailOpened = static_cast<qint64>(
        user.value(QStringLiteral("lastEmailOpenedAt")).toDouble());
    const qint64 lastEmailClicked = static_cast<qint64>(
        user.value(QStringLiteral("lastEmailClickedAt")).toDouble());
    m_usersTable->setItem(
        row, kUsersColLastEmailOpened,
        usersTableItem(
            lastEmailOpened > 0 ? usersTimestamp(lastEmailOpened)
                                : QStringLiteral("Never"),
            lastEmailOpened));
    m_usersTable->setItem(
        row, kUsersColLastEmailClicked,
        usersTableItem(
            lastEmailClicked > 0 ? usersTimestamp(lastEmailClicked)
                                 : QStringLiteral("Never"),
            lastEmailClicked));

    const QString country =
        user.value(QStringLiteral("countryCode")).toString();
    const QString browser = user.value(QStringLiteral("browser")).toString();
    const QString os = user.value(QStringLiteral("os")).toString();
    m_usersTable->setItem(
        row, kUsersColCountry,
        usersTableItem(country.isEmpty() ? QStringLiteral("Not shared")
                                         : country,
                       country));
    m_usersTable->setItem(
        row, kUsersColBrowser,
        usersTableItem(browser.isEmpty() ? QStringLiteral("Not shared")
                                         : browser,
                       browser));
    m_usersTable->setItem(
        row, kUsersColOs,
        usersTableItem(os.isEmpty() ? QStringLiteral("Not shared") : os,
                       os));

    QStringList nodes;
    for (const QJsonValue &nodeValue :
         user.value(QStringLiteral("nodes")).toArray()) {
        const QString node = nodeValue.toString().trimmed();
        if (!node.isEmpty())
            nodes.append(node);
    }
    const QString nodeText =
        nodes.isEmpty()
            ? QStringLiteral("0")
            : QStringLiteral("%1 - %2").arg(nodes.size()).arg(
                  nodes.join(QStringLiteral(", ")));
    auto *nodesItem = usersTableItem(nodeText, nodes.size());
    if (!nodes.isEmpty())
        nodesItem->setToolTip(nodes.join(QLatin1Char('\n')));
    m_usersTable->setItem(row, kUsersColNodes, nodesItem);
}

void MainWindow::renderUsersQuarantinePage(const QJsonArray &users)
{
    if (!m_usersQuarantineTable)
        return;

    const int sortColumn = qBound(
        0,
        m_usersQuarantineTable->horizontalHeader()->sortIndicatorSection(),
        m_usersQuarantineTable->columnCount() - 1);
    const Qt::SortOrder sortOrder =
        m_usersQuarantineTable->horizontalHeader()->sortIndicatorOrder();
    TableRepaintGuard repaintGuard(m_usersQuarantineTable);
    m_usersQuarantineTable->setSortingEnabled(false);
    m_usersQuarantineTable->setRowCount(0);

    for (const QJsonValue &value : users) {
        const QJsonObject user = value.toObject();
        const QString name =
            user.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty())
            continue;
        const int row = m_usersQuarantineTable->rowCount();
        m_usersQuarantineTable->insertRow(row);
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColName,
            usersTableItem(name, name.toLower()));

        const bool discordLinked =
            user.value(QStringLiteral("discordLinked")).toBool();
        const QString discordUsername = user.value(
            QStringLiteral("discordUsername")).toString().trimmed();
        const QString discordDisplayName = user.value(
            QStringLiteral("discordDisplayName")).toString().trimmed();
        const QString discordId = user.value(
            QStringLiteral("discordId")).toString().trimmed();
        const QString discordUsernameText =
            !discordUsername.isEmpty()
                ? discordUsername
                : discordLinked
                      ? QStringLiteral("Linked - username unavailable")
                      : QStringLiteral("Not linked");
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColDiscordUsername,
            usersTableItem(discordUsernameText,
                           discordUsernameText.toLower()));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColDiscordDisplayName,
            usersTableItem(discordDisplayName.isEmpty()
                               ? QStringLiteral("Unavailable")
                               : discordDisplayName,
                           discordDisplayName.toLower()));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColDiscordId,
            usersTableItem(discordId.isEmpty() ? QStringLiteral("Unavailable")
                                                : discordId,
                           discordId));
        const bool profilePrivate =
            user.value(QStringLiteral("profilePrivate")).toBool();
        const bool isAdmin = user.value(QStringLiteral("isAdmin")).toBool();
        const bool outreach =
            user.value(QStringLiteral("outreachEnabled")).toBool();
        const bool emailNotifications =
            user.value(QStringLiteral("emailNotifications")).toBool(true);
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColProfileVisibility,
            usersTableItem(profilePrivate ? QStringLiteral("Private")
                                          : QStringLiteral("Public"),
                           profilePrivate ? 1 : 0));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColAdmin,
            usersTableItem(isAdmin ? QStringLiteral("Yes")
                                   : QStringLiteral("No"),
                           isAdmin ? 1 : 0));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColOutreach,
            usersTableItem(outreach ? QStringLiteral("Enabled")
                                    : QStringLiteral("Disabled"),
                           outreach ? 1 : 0));
        const QString accountStatus = user.value(
            QStringLiteral("accountStatus")).toString().trimmed();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColAccountStatus,
            usersTableItem(accountStatus.isEmpty() ? QStringLiteral("Unknown")
                                                    : accountStatus,
                           accountStatus.toLower()));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColEmailNotifications,
            usersTableItem(emailNotifications ? QStringLiteral("Enabled")
                                               : QStringLiteral("Disabled"),
                           emailNotifications ? 1 : 0));

        const QString email =
            user.value(QStringLiteral("email")).toString().trimmed();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColEmail,
            usersTableItem(email.isEmpty() ? QStringLiteral("Unavailable")
                                            : email,
                           email.toLower()));
        const bool verified =
            user.value(QStringLiteral("emailVerified")).toBool();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColVerified,
            usersTableItem(verified ? QStringLiteral("Yes")
                                    : QStringLiteral("No"),
                           verified ? 1 : 0));

        const auto timestampItem = [&](const char *field, int column,
                                       const QString &empty) {
            const qint64 timestamp = static_cast<qint64>(
                user.value(QLatin1String(field)).toDouble());
            m_usersQuarantineTable->setItem(
                row, column,
                usersTableItem(timestamp > 0 ? usersTimestamp(timestamp)
                                             : empty,
                               timestamp));
        };
        timestampItem("createdAt", kUsersQuarantineColJoined,
                      QStringLiteral("Not reported"));
        timestampItem("avatarUpdatedAt", kUsersQuarantineColAvatarUpdated,
                      QStringLiteral("Never"));

        const QString solana =
            user.value(QStringLiteral("solana")).toString().trimmed();
        const QString mastodon =
            user.value(QStringLiteral("mastodon")).toString().trimmed();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColSolana,
            usersTableItem(solana.isEmpty() ? QStringLiteral("Not set")
                                             : solana,
                           solana));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColMastodon,
            usersTableItem(mastodon.isEmpty() ? QStringLiteral("Not set")
                                               : mastodon,
                           mastodon.toLower()));

        const QString status = user.value(
            QStringLiteral("quarantineStatus")).toString().trimmed();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColStatus,
            usersTableItem(
                status.isEmpty() ? QStringLiteral("pending_verification")
                                 : status,
                status.toLower()));

        QStringList reasons;
        for (const QJsonValue &reason :
             user.value(QStringLiteral("quarantineReasons")).toArray()) {
            const QString clean = reason.toString().trimmed();
            if (!clean.isEmpty())
                reasons.append(clean);
        }
        const QString reasonsText =
            reasons.isEmpty() ? QStringLiteral("None")
                              : reasons.join(QStringLiteral(", "));
        auto *reasonsItem = usersTableItem(
            reasonsText, reasonsText.toLower());
        if (!reasons.isEmpty())
            reasonsItem->setToolTip(reasons.join(QLatin1Char('\n')));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColReasons, reasonsItem);

        timestampItem("quarantinedAt", kUsersQuarantineColQuarantined,
                      QStringLiteral("Not reported"));
        timestampItem("reviewAfter", kUsersQuarantineColReviewAfter,
                      QStringLiteral("Not set"));
        timestampItem("reviewedAt", kUsersQuarantineColReviewed,
                      QStringLiteral("Not reviewed"));
        const QString reviewedBy = user.value(
            QStringLiteral("reviewedBy")).toString().trimmed();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColReviewedBy,
            usersTableItem(
                reviewedBy.isEmpty() ? QStringLiteral("Not reviewed")
                                     : reviewedBy,
                reviewedBy.toLower()));

        const QString referredBy = user.value(
            QStringLiteral("referredBy")).toString().trimmed();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColReferredBy,
            usersTableItem(referredBy.isEmpty() ? QStringLiteral("Direct")
                                                : referredBy,
                           referredBy.toLower()));
        const QString referralStatus = user.value(
            QStringLiteral("referralStatus")).toString().trimmed();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColReferralStatus,
            usersTableItem(
                referralStatus.isEmpty() ? QStringLiteral("Not attributed")
                                         : referralStatus,
                referralStatus.toLower()));
        const QString referralClickId = user.value(
            QStringLiteral("referralClickId")).toString().trimmed();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColReferralClickId,
            usersTableItem(
                referralClickId.isEmpty() ? QStringLiteral("Unavailable")
                                          : referralClickId,
                referralClickId.toLower()));

        QStringList riskFlags;
        for (const QJsonValue &flag :
             user.value(QStringLiteral("referralRiskFlags")).toArray()) {
            const QString clean = flag.toString().trimmed();
            if (!clean.isEmpty())
                riskFlags.append(clean);
        }
        const int riskScore = static_cast<int>(
            user.value(QStringLiteral("referralRiskScore")).toDouble());
        const QString riskText = QStringLiteral("%1 - %2")
                                     .arg(riskScore)
                                     .arg(riskFlags.isEmpty()
                                              ? QStringLiteral("None")
                                              : riskFlags.join(
                                                    QStringLiteral(", ")));
        auto *riskItem = usersTableItem(riskText, riskScore);
        if (!riskFlags.isEmpty())
            riskItem->setToolTip(riskFlags.join(QLatin1Char('\n')));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColRisk, riskItem);
        timestampItem("referralCreatedAt", kUsersQuarantineColReferralCreated,
                      QStringLiteral("Not set"));
        timestampItem("referralVerifiedAt",
                      kUsersQuarantineColReferralVerified,
                      QStringLiteral("Not set"));
        timestampItem("referralCreditedAt",
                      kUsersQuarantineColReferralCredited,
                      QStringLiteral("Not set"));

        const QString signupIp = user.value(
            QStringLiteral("signupIp")).toString().trimmed();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColSignupIp,
            usersTableItem(signupIp.isEmpty() ? QStringLiteral("Unavailable")
                                              : signupIp,
                           signupIp));
        const auto fingerprintItem = [&](const char *field, int column) {
            const QString fingerprint = user.value(
                QLatin1String(field)).toString().trimmed();
            const QString visible =
                fingerprint.size() > 18
                    ? fingerprint.left(18) + QString::fromUtf8("\xE2\x80\xA6")
                    : fingerprint;
            auto *item = usersTableItem(
                visible.isEmpty() ? QStringLiteral("Unavailable") : visible,
                fingerprint.toLower());
            if (!fingerprint.isEmpty())
                item->setToolTip(fingerprint);
            m_usersQuarantineTable->setItem(row, column, item);
        };
        fingerprintItem("signupIpFingerprint",
                        kUsersQuarantineColIpFingerprint);

        const QString signupAgent = user.value(
            QStringLiteral("signupUserAgent")).toString().trimmed();
        const QString visibleAgent =
            signupAgent.size() > 80
                ? signupAgent.left(80) + QString::fromUtf8("\xE2\x80\xA6")
                : signupAgent;
        auto *agentItem = usersTableItem(
            visibleAgent.isEmpty() ? QStringLiteral("Unavailable")
                                   : visibleAgent,
            signupAgent.toLower());
        if (!signupAgent.isEmpty())
            agentItem->setToolTip(signupAgent);
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColSignupAgent, agentItem);
        fingerprintItem("signupUserAgentFingerprint",
                        kUsersQuarantineColAgentFingerprint);
        fingerprintItem("signupClientFingerprint",
                        kUsersQuarantineColClientFingerprint);
        fingerprintItem("referralNetworkFingerprint",
                        kUsersQuarantineColNetworkFingerprint);
        fingerprintItem("userFingerprint",
                        kUsersQuarantineColUserFingerprint);
        fingerprintItem("emailFingerprint",
                        kUsersQuarantineColEmailFingerprint);
        fingerprintItem("discordFingerprint",
                        kUsersQuarantineColDiscordFingerprint);
        fingerprintItem("referrerFingerprint",
                        kUsersQuarantineColReferrerFingerprint);

        const QString country = user.value(
            QStringLiteral("signupCountry")).toString().trimmed();
        const QString client = user.value(
            QStringLiteral("signupClientCategory")).toString().trimmed();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColCountry,
            usersTableItem(country.isEmpty() ? QStringLiteral("Unavailable")
                                             : country,
                           country));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColClient,
            usersTableItem(client.isEmpty() ? QStringLiteral("Unavailable")
                                            : client,
                           client.toLower()));
        const QString worldCountry = user.value(
            QStringLiteral("countryCode")).toString().trimmed();
        const QString browser = user.value(
            QStringLiteral("browser")).toString().trimmed();
        const QString os =
            user.value(QStringLiteral("os")).toString().trimmed();
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColWorldCountry,
            usersTableItem(worldCountry.isEmpty()
                               ? QStringLiteral("Not shared")
                               : worldCountry,
                           worldCountry));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColBrowser,
            usersTableItem(browser.isEmpty() ? QStringLiteral("Not shared")
                                              : browser,
                           browser.toLower()));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColOs,
            usersTableItem(os.isEmpty() ? QStringLiteral("Not shared") : os,
                           os.toLower()));
        const int nodes = static_cast<int>(
            user.value(QStringLiteral("nodeCount")).toDouble());
        QStringList nodeNames;
        for (const QJsonValue &nodeValue :
             user.value(QStringLiteral("nodes")).toArray()) {
            const QString nodeName = nodeValue.toString().trimmed();
            if (!nodeName.isEmpty())
                nodeNames.append(nodeName);
        }
        const QString nodeText = nodeNames.isEmpty()
                                     ? QString::number(nodes)
                                     : QStringLiteral("%1 - %2")
                                           .arg(nodes)
                                           .arg(nodeNames.join(", "));
        auto *nodeItem = usersTableItem(nodeText, nodes);
        if (!nodeNames.isEmpty())
            nodeItem->setToolTip(nodeNames.join(QLatin1Char('\n')));
        m_usersQuarantineTable->setItem(
            row, kUsersQuarantineColNodes,
            nodeItem);
    }

    m_usersQuarantineTable->setSortingEnabled(true);
    m_usersQuarantineTable->sortItems(sortColumn, sortOrder);
    updateUsersStatus();
    refreshUsersUnquarantineButton();
}

void MainWindow::updateUsersStatus()
{
    if (!m_usersStatus)
        return;
    usersNextRefreshFilter(m_usersTabs, m_usersTable, m_usersQuarantineTable);
    if (m_usersTabs && m_usersTabs->currentIndex() == 1) {
        const int count = m_usersQuarantineTable
                              ? m_usersQuarantineTable->rowCount()
                              : 0;
        m_usersStatus->setText(QStringLiteral(
            "%1 quarantine %2 - complete preserved admin-only records")
                                   .arg(count)
                                   .arg(count == 1 ? QStringLiteral("record")
                                                   : QStringLiteral("records")));
        return;
    }
    const int count = m_usersTable ? m_usersTable->rowCount() : 0;
    m_usersStatus->setText(
        QStringLiteral(
            "%1 total user %2 - complete administrator directory")
            .arg(count)
            .arg(count == 1 ? QStringLiteral("record")
                            : QStringLiteral("records")));
}

QStringList MainWindow::selectedUsersActionTargets() const
{
    QStringList targets;
    if (!m_usersTable || !m_usersTable->selectionModel())
        return targets;

    QModelIndexList rows =
        m_usersTable->selectionModel()->selectedRows(kUsersColName);
    std::sort(rows.begin(), rows.end(), [](const QModelIndex &left,
                                           const QModelIndex &right) {
        return left.row() < right.row();
    });
    for (const QModelIndex &index : std::as_const(rows)) {
        const QTableWidgetItem *item =
            m_usersTable->item(index.row(), kUsersColName);
        const QString target =
            item ? item->text().trimmed().toLower() : QString();
        if (!target.isEmpty() && !targets.contains(target))
            targets.append(target);
    }
    return targets;
}

void MainWindow::removeUsersFromActiveDirectory(const QStringList &targets)
{
    if (targets.isEmpty())
        return;
    const auto withoutTargets = [&targets](const QJsonArray &users) {
        QJsonArray remaining;
        for (const QJsonValue &value : users) {
            const QJsonObject user = value.toObject();
            const QString name =
                user.value(QStringLiteral("name"))
                    .toString(
                        user.value(QStringLiteral("nodeName")).toString())
                    .trimmed();
            if (!targets.contains(name, Qt::CaseInsensitive))
                remaining.append(user);
        }
        return remaining;
    };
    const bool hadAdminPayload = m_usersDirectoryAdminPayload;
    const QJsonArray remainingAdmin =
        withoutTargets(m_usersDirectoryPayload);
    mergeChatUserDirectory(
        withoutTargets(m_usersPublicDirectoryPayload));
    if (hadAdminPayload) {
        m_usersDirectoryPayload = remainingAdmin;
        m_usersDirectoryAdminPayload = true;
        renderUsersPage(m_usersDirectoryPayload);
    }
}

void MainWindow::refreshUsersUnquarantineButton()
{
    if (!m_usersUnquarantineButton)
        return;
    QString target;
    QString status;
    if (m_usersQuarantineTable &&
        m_usersQuarantineTable->selectionModel()) {
        const QModelIndexList rows =
            m_usersQuarantineTable->selectionModel()->selectedRows(
                kUsersQuarantineColName);
        if (rows.size() == 1) {
            const int row = rows.constFirst().row();
            if (const QTableWidgetItem *item =
                    m_usersQuarantineTable->item(
                        row, kUsersQuarantineColName))
                target = item->text().trimmed().toLower();
            if (const QTableWidgetItem *item =
                    m_usersQuarantineTable->item(
                        row, kUsersQuarantineColStatus))
                status = item->text().trimmed().toLower();
        }
    }
    const bool quarantineTab =
        m_usersTabs && m_usersTabs->currentIndex() == 1;
    const bool restorable =
        status == QLatin1String("pending_verification") ||
        status == QLatin1String("rejected");
    const bool actionInFlight =
        m_usersQuarantineInFlight || m_usersUnquarantineInFlight ||
        m_usersDeleteInFlight;
    m_usersUnquarantineButton->setEnabled(
        m_isAdmin && quarantineTab && !target.isEmpty() && restorable &&
        !actionInFlight);
    if (target.isEmpty()) {
        m_usersUnquarantineButton->setToolTip(QStringLiteral(
            "Select a quarantined user to restore their preserved account "
            "and linked nodes."));
    } else if (!restorable) {
        m_usersUnquarantineButton->setToolTip(QStringLiteral(
            "%1 has already been restored.").arg(target));
    } else {
        m_usersUnquarantineButton->setToolTip(QStringLiteral(
            "Restore %1 and their linked nodes to the active directory. "
            "Revoked sessions and devices stay revoked.").arg(target));
    }
}

void MainWindow::unquarantineSelectedUser()
{
    if (!m_usersQuarantineTable || !m_usersTabs ||
        m_usersTabs->currentIndex() != 1 ||
        !m_usersQuarantineTable->selectionModel())
        return;
    const QModelIndexList rows =
        m_usersQuarantineTable->selectionModel()->selectedRows(
            kUsersQuarantineColName);
    if (rows.size() != 1)
        return;
    const int row = rows.constFirst().row();
    const QTableWidgetItem *nameItem =
        m_usersQuarantineTable->item(row, kUsersQuarantineColName);
    const QTableWidgetItem *statusItem =
        m_usersQuarantineTable->item(row, kUsersQuarantineColStatus);
    const QString target =
        nameItem ? nameItem->text().trimmed().toLower() : QString();
    const QString status =
        statusItem ? statusItem->text().trimmed().toLower() : QString();
    const bool restorable =
        status == QLatin1String("pending_verification") ||
        status == QLatin1String("rejected");
    if (!m_isAdmin || target.isEmpty() || !restorable ||
        m_usersQuarantineInFlight || m_usersUnquarantineInFlight ||
        m_usersDeleteInFlight)
        return;

    const int confirmed = QMessageBox::question(
        this, QStringLiteral("Unquarantine user"),
        QStringLiteral(
            "Restore %1 and all preserved linked nodes to the active "
            "directory?\n\nTheir old sessions and devices remain revoked; "
            "they must sign in again. Referral credit remains subject to "
            "separate review.")
            .arg(target),
        QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);
    if (confirmed != QMessageBox::Ok || !m_isAdmin ||
        m_usersQuarantineInFlight || m_usersUnquarantineInFlight ||
        m_usersDeleteInFlight)
        return;

    m_usersUnquarantineInFlight = true;
    refreshUsersUnquarantineButton();
    refreshUsersQuarantineButton();
    refreshUsersDeleteButton();
    refreshUsersTipButton();
    if (m_usersStatus)
        m_usersStatus->setText(
            QStringLiteral("Restoring %1 from quarantine...").arg(target));

    QUrl url = accountsApiUrl(QStringLiteral("admin-unquarantine"));
    QNetworkRequest request;
    if (!authenticateOrgTaskRequest(
            url, request, kAccountUserUnquarantineProof, target)) {
        m_usersUnquarantineInFlight = false;
        refreshUsersUnquarantineButton();
        refreshUsersQuarantineButton();
        refreshUsersDeleteButton();
        updateUsersStatus();
        QMessageBox::warning(
            this, QStringLiteral("Unquarantine failed"),
            QStringLiteral("This desktop could not authenticate the "
                           "administrator request."));
        return;
    }

    QNetworkReply *reply = m_networkAccess->post(
        request,
        QJsonDocument(QJsonObject{{QStringLiteral("target"), target}})
            .toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, target] {
        const QByteArray body = reply->readAll();
        const int httpStatus = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        const QJsonObject response = QJsonDocument::fromJson(body).object();
        const bool restored =
            networkOk && response.value(QStringLiteral("ok")).toBool();
        m_usersUnquarantineInFlight = false;

        if (restored) {
            QJsonArray updated;
            for (const QJsonValue &value : m_usersQuarantinePayload) {
                QJsonObject record = value.toObject();
                if (record.value(QStringLiteral("name"))
                        .toString().compare(target, Qt::CaseInsensitive) == 0) {
                    record.insert(QStringLiteral("quarantineStatus"),
                                  QStringLiteral("restored"));
                    record.insert(
                        QStringLiteral("reviewedAt"),
                        response.value(QStringLiteral("reviewedAt")));
                    record.insert(
                        QStringLiteral("reviewedBy"),
                        response.value(QStringLiteral("reviewedBy")));
                }
                updated.append(record);
            }
            m_usersQuarantinePayload = updated;
            renderUsersQuarantinePage(m_usersQuarantinePayload);
            m_chatDirectoryFetchedMs = 0;
            refreshChatUserDirectory();
            if (m_usersStatus)
                m_usersStatus->setText(
                    QStringLiteral("Restored %1. Refreshing...").arg(target));
        } else {
            const QString code =
                response.value(QStringLiteral("error")).toString();
            QString detail = QStringLiteral("The account could not be restored.");
            if (code == QLatin1String("already_restored"))
                detail = QStringLiteral("The account was already restored.");
            else if (code == QLatin1String("user_already_active"))
                detail = QStringLiteral("The account is already active.");
            else if (code == QLatin1String("restore_conflict"))
                detail = QStringLiteral(
                    "An account identity or preserved node conflicts with "
                    "an active record.");
            else if (code == QLatin1String("no_such_quarantined_user"))
                detail = QStringLiteral(
                    "The quarantine record no longer exists.");
            else if (httpStatus == 401)
                detail = QStringLiteral(
                    "Administrator authorization was rejected.");
            updateUsersStatus();
            QMessageBox::warning(
                this, QStringLiteral("Unquarantine failed"), detail);
        }
        refreshUsersUnquarantineButton();
        refreshUsersQuarantineButton();
        refreshUsersDeleteButton();
        refreshUsersTipButton();
    });
}

void MainWindow::refreshUsersQuarantineButton()
{
    if (!m_usersQuarantineButton)
        return;
    const QStringList targets = selectedUsersActionTargets();
    const QString owner = accountOwner().trimmed();
    const bool includesSelf = std::any_of(
        targets.cbegin(), targets.cend(), [&owner](const QString &target) {
            return target.compare(owner, Qt::CaseInsensitive) == 0;
        });
    const bool activeTab = !m_usersTabs || m_usersTabs->currentIndex() == 0;
    const bool actionInFlight =
        m_usersQuarantineInFlight || m_usersUnquarantineInFlight ||
        m_usersDeleteInFlight;
    m_usersQuarantineButton->setText(
        targets.size() > 1
            ? QStringLiteral("Quarantine %1…").arg(targets.size())
            : QStringLiteral("Quarantine…"));
    m_usersQuarantineButton->setEnabled(
        m_isAdmin && activeTab && !targets.isEmpty() && !includesSelf &&
        !actionInFlight);
    if (targets.isEmpty()) {
        m_usersQuarantineButton->setToolTip(QStringLiteral(
            "Select one or more users to move them into quarantine."));
    } else if (includesSelf) {
        m_usersQuarantineButton->setToolTip(QStringLiteral(
            "The selection includes your own account, which cannot be "
            "quarantined."));
    } else if (targets.size() == 1) {
        m_usersQuarantineButton->setToolTip(QStringLiteral(
            "Move %1 out of the active user directory, revoke their sessions, "
            "and quarantine their linked nodes.").arg(targets.constFirst()));
    } else {
        m_usersQuarantineButton->setToolTip(QStringLiteral(
            "Move all %1 selected users out of the active directory, revoke "
            "their sessions, and quarantine their linked nodes.")
                                                .arg(targets.size()));
    }
}

void MainWindow::quarantineSelectedUsers()
{
    const QStringList targets = selectedUsersActionTargets();
    const QString owner = accountOwner().trimmed();
    const bool includesSelf = std::any_of(
        targets.cbegin(), targets.cend(), [&owner](const QString &target) {
            return target.compare(owner, Qt::CaseInsensitive) == 0;
        });
    const bool activeTab = !m_usersTabs || m_usersTabs->currentIndex() == 0;
    if (!m_isAdmin || !activeTab || targets.isEmpty() ||
        m_usersQuarantineInFlight || m_usersUnquarantineInFlight ||
        m_usersDeleteInFlight)
        return;
    if (includesSelf) {
        QMessageBox::information(
            this, QStringLiteral("Quarantine users"),
            QStringLiteral(
                "Your own account is selected. Remove it from the selection "
                "before quarantining these users."));
        return;
    }

    const bool multiple = targets.size() > 1;
    QString selection = targets.mid(0, 8).join(QStringLiteral(", "));
    if (targets.size() > 8)
        selection += QStringLiteral(", and %1 more").arg(targets.size() - 8);
    const QString prompt =
        multiple
            ? QStringLiteral(
                  "Move all %1 selected users into quarantine?\n\n%2\n\nThis "
                  "immediately removes their accounts and linked nodes from "
                  "active directories and revokes all of their devices and "
                  "sessions.")
                  .arg(targets.size())
                  .arg(selection)
            : QStringLiteral(
                  "Move %1 into quarantine?\n\nThis immediately removes the "
                  "account and its linked nodes from active directories and "
                  "revokes all of its devices and sessions.")
                  .arg(targets.constFirst());
    const int confirmed = QMessageBox::warning(
        this,
        multiple ? QStringLiteral("Quarantine users")
                 : QStringLiteral("Quarantine user"),
        prompt, QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);
    if (confirmed != QMessageBox::Ok || !m_isAdmin ||
        m_usersQuarantineInFlight || m_usersUnquarantineInFlight ||
        m_usersDeleteInFlight)
        return;

    m_usersQuarantineBatch = {};
    m_usersQuarantineBatch.targets = targets;
    m_usersQuarantineInFlight = true;
    refreshUsersQuarantineButton();
    refreshUsersUnquarantineButton();
    refreshUsersDeleteButton();
    refreshUsersTipButton();
    quarantineNextSelectedUser();
}

void MainWindow::quarantinePendingUsers(const QStringList &targets)
{
    if (!m_isAdmin || targets.isEmpty() || m_usersQuarantineInFlight ||
        m_usersUnquarantineInFlight || m_usersDeleteInFlight)
        return;

    QStringList normalizedTargets;
    for (const QString &target : targets) {
        const QString normalized = target.trimmed().toLower();
        if (!normalized.isEmpty() &&
            !normalizedTargets.contains(normalized))
            normalizedTargets.append(normalized);
    }
    if (normalizedTargets.isEmpty())
        return;

    const QString owner = accountOwner().trimmed().toLower();
    const bool includesSelf = std::any_of(
        normalizedTargets.cbegin(), normalizedTargets.cend(),
        [&owner](const QString &target) { return target == owner; });
    if (includesSelf) {
        QMessageBox::information(
            this, QStringLiteral("Quarantine users"),
            QStringLiteral(
                "Your own account is among the new users. Remove it before "
                "quarantining this group."));
        return;
    }

    m_usersQuarantineBatch = {};
    m_usersQuarantineBatch.targets = normalizedTargets;
    m_usersQuarantineInFlight = true;
    refreshUsersQuarantineButton();
    refreshUsersUnquarantineButton();
    refreshUsersDeleteButton();
    refreshUsersTipButton();
    quarantineNextSelectedUser();
}

void MainWindow::quarantineNextSelectedUser()
{
    if (!m_usersQuarantineInFlight)
        return;
    if (!m_isAdmin) {
        while (m_usersQuarantineBatch.nextIndex <
               m_usersQuarantineBatch.targets.size()) {
            const QString target = m_usersQuarantineBatch.targets.at(
                m_usersQuarantineBatch.nextIndex++);
            m_usersQuarantineBatch.failures.append(
                QStringLiteral("%1 — administrator access is no longer "
                               "available.")
                    .arg(target));
        }
        finishUsersQuarantineBatch();
        return;
    }
    if (m_usersQuarantineBatch.nextIndex >=
        m_usersQuarantineBatch.targets.size()) {
        finishUsersQuarantineBatch();
        return;
    }

    const int targetIndex = m_usersQuarantineBatch.nextIndex;
    const QString target = m_usersQuarantineBatch.targets.at(targetIndex);
    if (m_usersStatus) {
        m_usersStatus->setText(
            QStringLiteral("Quarantining %1 of %2: %3...")
                .arg(targetIndex + 1)
                .arg(m_usersQuarantineBatch.targets.size())
                .arg(target));
    }
    QUrl url = accountsApiUrl(QStringLiteral("admin-quarantine"));
    QNetworkRequest request;
    if (!authenticateOrgTaskRequest(
            url, request, kAccountUserQuarantineProof, target)) {
        m_usersQuarantineBatch.failures.append(QStringLiteral(
            "%1 — this desktop could not authenticate the administrator "
            "request.")
                                                   .arg(target));
        ++m_usersQuarantineBatch.nextIndex;
        QTimer::singleShot(0, this,
                           &MainWindow::quarantineNextSelectedUser);
        return;
    }

    QNetworkReply *reply = m_networkAccess->post(
        request,
        QJsonDocument(QJsonObject{{QStringLiteral("target"), target}})
            .toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, target] {
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        const QJsonObject response = QJsonDocument::fromJson(body).object();
        if (networkOk && response.value(QStringLiteral("ok")).toBool()) {
            m_usersQuarantineBatch.succeeded.append(target);
        } else {
            const QString code =
                response.value(QStringLiteral("error")).toString();
            QString detail = QStringLiteral("could not be quarantined");
            if (code == QLatin1String("target_is_admin"))
                detail = QStringLiteral("is an administrator");
            else if (code == QLatin1String("self_quarantine_forbidden"))
                detail = QStringLiteral("is your own account");
            else if (code == QLatin1String("no_such_user"))
                detail = QStringLiteral("is no longer active");
            else if (status == 401)
                detail = QStringLiteral(
                    "was rejected by administrator authorization");
            m_usersQuarantineBatch.failures.append(
                QStringLiteral("%1 — %2.").arg(target, detail));
        }
        ++m_usersQuarantineBatch.nextIndex;
        QTimer::singleShot(0, this,
                           &MainWindow::quarantineNextSelectedUser);
    });
}

void MainWindow::finishUsersQuarantineBatch()
{
    const UsersModerationBatch result = m_usersQuarantineBatch;
    m_usersQuarantineBatch = {};
    m_usersQuarantineInFlight = false;
    removeUsersFromActiveDirectory(result.succeeded);
    m_chatDirectoryFetchedMs = 0;
    refreshChatUserDirectory();
    refreshUsersQuarantineButton();
    refreshUsersUnquarantineButton();
    refreshUsersDeleteButton();
    refreshUsersTipButton();

    if (m_usersStatus) {
        m_usersStatus->setText(
            QStringLiteral("Quarantined %1 of %2 selected users. Refreshing...")
                .arg(result.succeeded.size())
                .arg(result.targets.size()));
    }
    if (!result.failures.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("Quarantine incomplete"),
            QStringLiteral("Quarantined %1 of %2 selected users.\n\n%3")
                .arg(result.succeeded.size())
                .arg(result.targets.size())
                .arg(result.failures.join(QLatin1Char('\n'))));
    }
}

void MainWindow::refreshUsersDeleteButton()
{
    if (!m_usersDeleteButton)
        return;
    const QStringList targets = selectedUsersActionTargets();
    const QString owner = accountOwner().trimmed();
    const bool includesSelf = std::any_of(
        targets.cbegin(), targets.cend(), [&owner](const QString &target) {
            return target.compare(owner, Qt::CaseInsensitive) == 0;
        });
    const bool activeTab = !m_usersTabs || m_usersTabs->currentIndex() == 0;
    const bool actionInFlight =
        m_usersQuarantineInFlight || m_usersUnquarantineInFlight ||
        m_usersDeleteInFlight;
    m_usersDeleteButton->setText(
        targets.size() > 1
            ? QStringLiteral("Delete %1…").arg(targets.size())
            : QStringLiteral("Delete…"));
    m_usersDeleteButton->setEnabled(
        m_isAdmin && activeTab && !targets.isEmpty() && !includesSelf &&
        !actionInFlight);
    if (targets.isEmpty()) {
        m_usersDeleteButton->setToolTip(QStringLiteral(
            "Select one or more users to delete them permanently."));
    } else if (includesSelf) {
        m_usersDeleteButton->setToolTip(QStringLiteral(
            "The selection includes your own account, which cannot be "
            "deleted."));
    } else if (targets.size() == 1) {
        m_usersDeleteButton->setToolTip(QStringLiteral(
            "Permanently erase %1 and everything attached to it: linked "
            "nodes and their repositories, sessions, devices, SSH keys, "
            "referrals, follows, chat memberships, and World records. "
            "Unlike quarantine, this keeps no record and cannot be undone.")
                                            .arg(targets.constFirst()));
    } else {
        m_usersDeleteButton->setToolTip(QStringLiteral(
            "Permanently erase all %1 selected users and everything attached "
            "to them. Unlike quarantine, this keeps no records and cannot be "
            "undone.")
                                            .arg(targets.size()));
    }
}

void MainWindow::deleteSelectedUsers()
{
    const QStringList targets = selectedUsersActionTargets();
    const QString owner = accountOwner().trimmed();
    const bool includesSelf = std::any_of(
        targets.cbegin(), targets.cend(), [&owner](const QString &target) {
            return target.compare(owner, Qt::CaseInsensitive) == 0;
        });
    const bool activeTab = !m_usersTabs || m_usersTabs->currentIndex() == 0;
    if (!m_isAdmin || !activeTab || targets.isEmpty() ||
        m_usersQuarantineInFlight || m_usersUnquarantineInFlight ||
        m_usersDeleteInFlight)
        return;
    if (includesSelf) {
        QMessageBox::information(
            this, QStringLiteral("Delete users"),
            QStringLiteral(
                "Your own account is selected. Remove it from the selection "
                "before deleting these users."));
        return;
    }

    const bool multiple = targets.size() > 1;
    QString selection = targets.mid(0, 8).join(QStringLiteral(", "));
    if (targets.size() > 8)
        selection += QStringLiteral(", and %1 more").arg(targets.size() - 8);
    const QString prompt =
        multiple
            ? QStringLiteral(
                  "Permanently delete all %1 selected users?\n\n%2\n\nThis "
                  "erases every account and everything attached to it: linked "
                  "nodes and their repositories, sessions, devices, SSH keys, "
                  "referrals, follows, chat memberships, and World records."
                  "\n\nQuarantine preserves records for review — deletion "
                  "does not, and it cannot be undone.")
                  .arg(targets.size())
                  .arg(selection)
            : QStringLiteral(
                  "Permanently delete %1?\n\nThis erases the account and "
                  "everything attached to it: its linked nodes and their "
                  "repositories, sessions, devices, SSH keys, referrals, "
                  "follows, chat memberships, and World records.\n\nQuarantine "
                  "preserves the record for review — this does not, and it "
                  "cannot be undone.")
                  .arg(targets.constFirst());
    QMessageBox box(QMessageBox::Warning,
                    multiple ? QStringLiteral("Delete users")
                             : QStringLiteral("Delete user"),
                    prompt, QMessageBox::Cancel, this);
    QPushButton *go = box.addButton(
        multiple ? QStringLiteral("Delete %1 users…").arg(targets.size())
                 : QStringLiteral("Delete user…"),
        QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != go)
        return;

    // A batch gets one explicit typed phrase, while the single-user path keeps
    // the account-name confirmation people already use.
    const QString confirmation =
        multiple ? QStringLiteral("DELETE %1 USERS").arg(targets.size())
                 : targets.constFirst();
    bool typedOk = false;
    const QString typed = QInputDialog::getText(
        this, QStringLiteral("Confirm delete"),
        QStringLiteral("Type %1 to confirm this deletion:").arg(confirmation),
        QLineEdit::Normal, QString(), &typedOk);
    if (!typedOk ||
        typed.trimmed().compare(confirmation, Qt::CaseInsensitive) != 0)
        return;
    // Both dialogs pump the event loop. Re-check authorization and action state,
    // but keep the exact target list the administrator just confirmed.
    if (!m_isAdmin || m_usersQuarantineInFlight ||
        m_usersUnquarantineInFlight || m_usersDeleteInFlight)
        return;

    m_usersDeleteBatch = {};
    m_usersDeleteBatch.targets = targets;
    m_usersDeleteInFlight = true;
    refreshUsersQuarantineButton();
    refreshUsersUnquarantineButton();
    refreshUsersDeleteButton();
    refreshUsersTipButton();
    deleteNextSelectedUser();
}

void MainWindow::deleteNextSelectedUser()
{
    if (!m_usersDeleteInFlight)
        return;
    if (!m_isAdmin) {
        while (m_usersDeleteBatch.nextIndex <
               m_usersDeleteBatch.targets.size()) {
            const QString target =
                m_usersDeleteBatch.targets.at(m_usersDeleteBatch.nextIndex++);
            m_usersDeleteBatch.failures.append(
                QStringLiteral("%1 — administrator access is no longer "
                               "available.")
                    .arg(target));
        }
        finishUsersDeleteBatch();
        return;
    }
    if (m_usersDeleteBatch.nextIndex >= m_usersDeleteBatch.targets.size()) {
        finishUsersDeleteBatch();
        return;
    }

    const int targetIndex = m_usersDeleteBatch.nextIndex;
    const QString target = m_usersDeleteBatch.targets.at(targetIndex);
    if (m_usersStatus) {
        m_usersStatus->setText(QStringLiteral("Deleting %1 of %2: %3...")
                                   .arg(targetIndex + 1)
                                   .arg(m_usersDeleteBatch.targets.size())
                                   .arg(target));
    }
    QUrl url = accountsApiUrl(QStringLiteral("admin-delete"));
    QNetworkRequest request;
    if (!authenticateOrgTaskRequest(
            url, request, kAccountUserDeleteProof, target)) {
        m_usersDeleteBatch.failures.append(QStringLiteral(
            "%1 — this desktop could not authenticate the administrator "
            "request.")
                                               .arg(target));
        ++m_usersDeleteBatch.nextIndex;
        m_usersDeleteBatch.lastRemaining = -1;
        QTimer::singleShot(0, this, &MainWindow::deleteNextSelectedUser);
        return;
    }

    QNetworkReply *reply = m_networkAccess->post(
        request,
        QJsonDocument(QJsonObject{{QStringLiteral("target"), target}})
            .toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, target] {
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        const QJsonObject response = QJsonDocument::fromJson(body).object();
        if (!networkOk || !response.value(QStringLiteral("ok")).toBool()) {
            const QString code =
                response.value(QStringLiteral("error")).toString();
            QString detail = QStringLiteral("could not be deleted");
            if (code == QLatin1String("target_is_admin"))
                detail = QStringLiteral("is an administrator");
            else if (code == QLatin1String("self_delete_forbidden"))
                detail = QStringLiteral("is your own account");
            else if (code == QLatin1String("no_such_user"))
                detail = QStringLiteral("is no longer active");
            else if (status == 401)
                detail = QStringLiteral(
                    "was rejected by administrator authorization");
            m_usersDeleteBatch.failures.append(
                QStringLiteral("%1 — %2.").arg(target, detail));
            ++m_usersDeleteBatch.nextIndex;
            m_usersDeleteBatch.lastRemaining = -1;
            QTimer::singleShot(0, this, &MainWindow::deleteNextSelectedUser);
            return;
        }

        if (response.value(QStringLiteral("accountDeleted")).toBool()) {
            m_usersDeleteBatch.succeeded.append(target);
            ++m_usersDeleteBatch.nextIndex;
            m_usersDeleteBatch.lastRemaining = -1;
            QTimer::singleShot(0, this, &MainWindow::deleteNextSelectedUser);
            return;
        }

        // One server request intentionally removes only a bounded linked-node
        // batch. Keep going under the one confirmation until this selected
        // account is actually gone, while refusing to loop if the relay makes
        // no forward progress.
        const int remaining = static_cast<int>(
            response.value(QStringLiteral("nodesRemaining")).toDouble());
        const int deleted =
            response.value(QStringLiteral("nodesDeleted")).toArray().size();
        const bool stalled =
            remaining <= 0 || deleted <= 0 ||
            (m_usersDeleteBatch.lastRemaining >= 0 &&
             remaining >= m_usersDeleteBatch.lastRemaining);
        if (stalled) {
            m_usersDeleteBatch.failures.append(QStringLiteral(
                "%1 — linked-node deletion stopped making progress.")
                                                   .arg(target));
            ++m_usersDeleteBatch.nextIndex;
            m_usersDeleteBatch.lastRemaining = -1;
        } else {
            m_usersDeleteBatch.lastRemaining = remaining;
            if (m_usersStatus) {
                m_usersStatus->setText(
                    QStringLiteral("Deleting %1: %2 linked nodes remain...")
                        .arg(target)
                        .arg(remaining));
            }
        }
        QTimer::singleShot(0, this, &MainWindow::deleteNextSelectedUser);
    });
}

void MainWindow::finishUsersDeleteBatch()
{
    const UsersModerationBatch result = m_usersDeleteBatch;
    m_usersDeleteBatch = {};
    m_usersDeleteInFlight = false;
    removeUsersFromActiveDirectory(result.succeeded);
    m_chatDirectoryFetchedMs = 0;
    refreshChatUserDirectory();
    refreshUsersQuarantineButton();
    refreshUsersUnquarantineButton();
    refreshUsersDeleteButton();
    refreshUsersTipButton();

    if (m_usersStatus) {
        m_usersStatus->setText(
            QStringLiteral("Deleted %1 of %2 selected users. Refreshing...")
                .arg(result.succeeded.size())
                .arg(result.targets.size()));
    }
    if (!result.failures.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("Delete incomplete"),
            QStringLiteral("Deleted %1 of %2 selected users.\n\n%3")
                .arg(result.succeeded.size())
                .arg(result.targets.size())
                .arg(result.failures.join(QLatin1Char('\n'))));
    }
}
