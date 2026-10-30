#include "MastodonFeature.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QCursor>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QStyle>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace forkmesh::mastodon {

namespace {

constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kArgRole = Qt::UserRole + 2;
constexpr int kCountRole = Qt::UserRole + 3;
constexpr int kHeaderRole = Qt::UserRole + 4;
constexpr int kIconRole = Qt::UserRole + 5;
constexpr int kMaxSeenStatuses = 600;

class NavDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.data(kHeaderRole).toBool())
            return QSize(option.rect.width(), index.data(Qt::DisplayRole).toString().isEmpty() ? 10 : 30);
        return QSize(option.rect.width(), 32);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QWidget *view = option.widget;
        const QColor text = view ? view->palette().color(QPalette::WindowText) : Qt::black;
        const QColor muted = mutedColor(view);
        const QColor accent = accentColor(view);
        const QRect rect = option.rect.adjusted(4, 1, -4, -1);
        if (index.data(kHeaderRole).toBool()) {
            QFont font = option.font;
            font.setPixelSize(11);
            font.setBold(true);
            font.setCapitalization(QFont::AllUppercase);
            font.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
            painter->setFont(font);
            painter->setPen(muted);
            painter->drawText(rect.adjusted(10, 0, 0, -4), Qt::AlignLeft | Qt::AlignBottom, index.data(Qt::DisplayRole).toString());
            painter->restore();
            return;
        }
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            QColor fill = selected ? accent : text;
            fill.setAlphaF(selected ? 0.16 : 0.05);
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRoundedRect(rect, 7, 7);
        }
        const QString iconName = index.data(kIconRole).toString();
        const QIcon itemIcon = icon(iconName, selected ? accent : muted, 16);
        itemIcon.paint(painter, QRect(rect.left() + 10, rect.center().y() - 8, 16, 16));
        QFont font = option.font;
        font.setPixelSize(13);
        font.setBold(selected);
        painter->setFont(font);
        painter->setPen(text);
        const int count = index.data(kCountRole).toInt();
        int right = rect.right() - 8;
        if (count > 0) {
            QFont badgeFont = font;
            badgeFont.setPixelSize(11);
            badgeFont.setBold(true);
            const QString label = count >= 40 ? QStringLiteral("40+") : QString::number(count);
            const int width = qMax(20, QFontMetrics(badgeFont).horizontalAdvance(label) + 12);
            const QRect badge(right - width, rect.center().y() - 9, width, 18);
            painter->setPen(Qt::NoPen);
            painter->setBrush(accent);
            painter->drawRoundedRect(badge, 9, 9);
            painter->setFont(badgeFont);
            painter->setPen(Qt::white);
            painter->drawText(badge, Qt::AlignCenter, label);
            painter->setFont(font);
            painter->setPen(text);
            right = badge.left() - 6;
        }
        const QRect textRect(rect.left() + 36, rect.top(), right - rect.left() - 36, rect.height());
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(font).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRect.width()));
        painter->restore();
    }
};

QString callbackPage(bool ok)
{
    const QString title = ok ? QStringLiteral("You are signed in") : QStringLiteral("Sign-in was not completed");
    const QString body = ok ? QStringLiteral("You can close this tab and go back to ForkMesh.")
                            : QStringLiteral("Go back to ForkMesh and try again.");
    return QStringLiteral("<!doctype html><html><head><meta charset=\"utf-8\"><title>%1</title></head>"
                          "<body style=\"font-family: system-ui, sans-serif; display:flex; align-items:center; "
                          "justify-content:center; height:90vh; color:#24292f;\"><div style=\"text-align:center\">"
                          "<h2>%1</h2><p>%2</p></div></body></html>")
        .arg(title, body);
}

}

MastodonFeature::MastodonFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_host(host), m_client(std::make_unique<Client>(host.network))
{
    setObjectName(QStringLiteral("mastodonSection"));
    m_openExternal = [](const QUrl &url) { QDesktopServices::openUrl(url); };
    m_confirm = [this](const QString &title, const QString &text) {
        return QMessageBox::question(this, title, text, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) ==
               QMessageBox::Yes;
    };
    m_client->setUnauthorizedHandler([this] { sessionExpired(); });

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    m_stack = new QStackedWidget;
    m_welcome = buildWelcome();
    m_stack->addWidget(m_welcome);
    m_app = new QWidget;
    auto *appLayout = new QHBoxLayout(m_app);
    appLayout->setContentsMargins(0, 0, 0, 0);
    appLayout->setSpacing(0);
    appLayout->addWidget(buildSidebar());
    auto *divider = new QWidget;
    divider->setObjectName(QStringLiteral("mastodonDivider"));
    divider->setAttribute(Qt::WA_StyledBackground);
    divider->setStyleSheet(QStringLiteral("QWidget#mastodonDivider { background: rgba(128, 128, 128, 0.28); }"));
    divider->setFixedWidth(1);
    appLayout->addWidget(divider);
    appLayout->addWidget(buildMain(), 1);
    m_stack->addWidget(m_app);
    outer->addWidget(m_stack);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(450);
    connect(m_searchTimer, &QTimer::timeout, this, [this] {
        if (!signedIn())
            return;
        if (m_hostQuery.size() >= 2)
            openPage({QStringLiteral("search"), m_hostQuery, QString()}, m_page.kind != QLatin1String("search"));
        else if (m_hostQuery.isEmpty() && m_page.kind == QLatin1String("search"))
            goBack();
    });

    loadAccounts();
    const SavedAccount account = activeAccount();
    if (account.valid())
        activate(account);
    else
        showSignIn();
}

MastodonFeature::~MastodonFeature()
{
    stopLoopback();
    m_client->cancelAll();
}

bool MastodonFeature::signedIn() const
{
    return m_client->signedIn() && m_stack->currentWidget() == m_app;
}

SavedAccount MastodonFeature::activeAccount() const
{
    for (const SavedAccount &account : m_accounts) {
        if (account.key() == m_activeKey)
            return account;
    }
    return m_accounts.isEmpty() ? SavedAccount() : m_accounts.first();
}

QString MastodonFeature::notice() const
{
    if (m_stack->currentWidget() == m_welcome)
        return m_welcomeNotice->text();
    return m_notice->isHidden() ? QString() : m_notice->text();
}

QList<PostCard *> MastodonFeature::postCards() const
{
    QList<PostCard *> cards;
    for (QFrame *frame : m_column->findChildren<QFrame *>(QStringLiteral("mastodonPost"))) {
        auto *card = dynamic_cast<PostCard *>(frame);
        if (card && !card->isHidden())
            cards.append(card);
    }
    return cards;
}

QList<AccountRow *> MastodonFeature::accountRows() const
{
    QList<AccountRow *> rows;
    for (QFrame *frame : m_column->findChildren<QFrame *>(QStringLiteral("mastodonAccountRow"))) {
        auto *row = dynamic_cast<AccountRow *>(frame);
        if (row && !row->isHidden())
            rows.append(row);
    }
    return rows;
}

void MastodonFeature::refresh()
{
    if (signedIn())
        reload();
}

QWidget *MastodonFeature::buildWelcome()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("mastodonWelcome"));
    auto *outer = new QVBoxLayout(page);
    outer->addStretch(1);
    auto *center = new QHBoxLayout;
    center->addStretch(1);
    auto *card = new QWidget;
    card->setMaximumWidth(500);
    card->setMinimumWidth(380);
    auto *layout = new QVBoxLayout(card);
    layout->setSpacing(12);
    auto *logo = new QLabel;
    logo->setPixmap(icon(QStringLiteral("mastodon"), QColor(0x63, 0x64, 0xff), 44).pixmap(44, 44));
    logo->setAlignment(Qt::AlignCenter);
    layout->addWidget(logo);
    auto *title = new QLabel(QStringLiteral("Mastodon"));
    title->setObjectName(QStringLiteral("pageTitle"));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 600;"));
    layout->addWidget(title);
    auto *intro = new QLabel(QStringLiteral("Sign in to your Mastodon account to read your timelines, post, and manage "
                                            "your profile, followers, lists and settings from here."));
    intro->setWordWrap(true);
    intro->setAlignment(Qt::AlignCenter);
    intro->setObjectName(QStringLiteral("mutedLabel"));
    layout->addWidget(intro);
    layout->addSpacing(6);

    auto *label = new QLabel(QStringLiteral("Your server"));
    label->setStyleSheet(QStringLiteral("font-weight: 600;"));
    layout->addWidget(label);
    auto *row = new QHBoxLayout;
    m_instanceField = new QLineEdit;
    m_instanceField->setObjectName(QStringLiteral("mastodonInstance"));
    m_instanceField->setPlaceholderText(QStringLiteral("mastodon.social"));
    m_instanceField->setClearButtonEnabled(true);
    row->addWidget(m_instanceField, 1);
    m_signInButton = new QPushButton(QStringLiteral("Sign in with browser"));
    m_signInButton->setObjectName(QStringLiteral("primaryButton"));
    m_signInButton->setCursor(Qt::PointingHandCursor);
    row->addWidget(m_signInButton);
    layout->addLayout(row);
    connect(m_signInButton, &QPushButton::clicked, this, [this] { beginBrowserSignIn(m_instanceField->text()); });
    connect(m_instanceField, &QLineEdit::returnPressed, this, [this] { beginBrowserSignIn(m_instanceField->text()); });

    QStringList servers;
    for (const QString &server : {QStringLiteral("mastodon.social"), QStringLiteral("mastodon.online"),
                                  QStringLiteral("fosstodon.org"), QStringLiteral("hachyderm.io")})
        servers << QStringLiteral("<a href=\"%1\" style=\"text-decoration:none;\">%1</a>").arg(server);
    auto *suggestions = richLabel(QStringLiteral("Popular servers: ") + servers.join(QStringLiteral(" &nbsp;&middot;&nbsp; ")));
    suggestions->setObjectName(QStringLiteral("mastodonServerSuggestions"));
    suggestions->setStyleSheet(QStringLiteral("font-size: 12px;"));
    connect(suggestions, &QLabel::linkActivated, this, [this](const QString &server) {
        m_instanceField->setText(server);
        m_instanceField->setFocus();
    });
    layout->addWidget(suggestions);

    m_welcomeNotice = new QLabel;
    m_welcomeNotice->setObjectName(QStringLiteral("mastodonWelcomeNotice"));
    m_welcomeNotice->setWordWrap(true);
    m_welcomeNotice->hide();
    layout->addWidget(m_welcomeNotice);

    m_waitingBox = new QWidget;
    auto *waiting = new QVBoxLayout(m_waitingBox);
    waiting->setContentsMargins(0, 4, 0, 4);
    waiting->setSpacing(8);
    m_waitingText = new QLabel;
    m_waitingText->setWordWrap(true);
    waiting->addWidget(m_waitingText);
    auto *waitingButtons = new QHBoxLayout;
    auto *again = new QPushButton(QStringLiteral("Open the page again"));
    again->setObjectName(QStringLiteral("ghostButton"));
    connect(again, &QPushButton::clicked, this, [this] {
        if (!m_pending.clientId.isEmpty())
            openExternal(authorizeUrl(m_pending.instance, m_pending.clientId, m_pending.redirect, m_pending.state,
                                      pkceChallenge(m_pending.verifier)));
    });
    waitingButtons->addWidget(again);
    auto *codeInstead = new QPushButton(QStringLiteral("Show me a code instead"));
    codeInstead->setObjectName(QStringLiteral("ghostButton"));
    codeInstead->setToolTip(QStringLiteral("Use this if the browser cannot reach ForkMesh on this computer"));
    connect(codeInstead, &QPushButton::clicked, this, [this] { useCodeInstead(); });
    waitingButtons->addWidget(codeInstead);
    auto *cancel = new QPushButton(QStringLiteral("Cancel"));
    cancel->setObjectName(QStringLiteral("ghostButton"));
    connect(cancel, &QPushButton::clicked, this, [this] {
        stopLoopback();
        m_pending = {};
        setWelcomeState(QStringLiteral("idle"));
    });
    waitingButtons->addWidget(cancel);
    waitingButtons->addStretch(1);
    waiting->addLayout(waitingButtons);
    auto *codeRow = new QHBoxLayout;
    m_codeField = new QLineEdit;
    m_codeField->setObjectName(QStringLiteral("mastodonCode"));
    m_codeField->setPlaceholderText(QStringLiteral("Paste the authorization code"));
    codeRow->addWidget(m_codeField, 1);
    auto *finish = new QPushButton(QStringLiteral("Continue"));
    finish->setObjectName(QStringLiteral("primaryButton"));
    connect(finish, &QPushButton::clicked, this, [this] { finishSignIn(m_codeField->text()); });
    connect(m_codeField, &QLineEdit::returnPressed, this, [this] { finishSignIn(m_codeField->text()); });
    codeRow->addWidget(finish);
    waiting->addLayout(codeRow);
    m_waitingBox->hide();
    layout->addWidget(m_waitingBox);

    auto *tokenToggle = new QPushButton(QStringLiteral("Use an access token instead"));
    tokenToggle->setObjectName(QStringLiteral("ghostButton"));
    tokenToggle->setCheckable(true);
    layout->addWidget(tokenToggle, 0, Qt::AlignLeft);
    m_tokenBox = new QWidget;
    auto *tokenLayout = new QVBoxLayout(m_tokenBox);
    tokenLayout->setContentsMargins(0, 0, 0, 0);
    auto *tokenHelp = mutedLabel(QStringLiteral("Create one on your server under Preferences → Development → New "
                                                "application with the read, write and follow scopes, then paste its access token."),
                                 card);
    tokenHelp->setWordWrap(true);
    tokenLayout->addWidget(tokenHelp);
    auto *tokenRow = new QHBoxLayout;
    m_tokenField = new QLineEdit;
    m_tokenField->setObjectName(QStringLiteral("mastodonToken"));
    m_tokenField->setEchoMode(QLineEdit::Password);
    m_tokenField->setPlaceholderText(QStringLiteral("Access token"));
    tokenRow->addWidget(m_tokenField, 1);
    auto *tokenButton = new QPushButton(QStringLiteral("Sign in"));
    tokenButton->setObjectName(QStringLiteral("ghostButton"));
    connect(tokenButton, &QPushButton::clicked, this, [this] { signInWithToken(m_instanceField->text(), m_tokenField->text()); });
    tokenRow->addWidget(tokenButton);
    tokenLayout->addLayout(tokenRow);
    m_tokenBox->hide();
    connect(tokenToggle, &QPushButton::toggled, m_tokenBox, &QWidget::setVisible);
    layout->addWidget(m_tokenBox);

    auto *privacy = mutedLabel(QStringLiteral("ForkMesh talks to your server directly. Your sign-in stays on this computer "
                                              "until you sign out or remove the package."),
                               card);
    privacy->setWordWrap(true);
    layout->addSpacing(6);
    layout->addWidget(privacy);
    m_welcomeBack = new QPushButton;
    m_welcomeBack->setObjectName(QStringLiteral("ghostButton"));
    connect(m_welcomeBack, &QPushButton::clicked, this, [this] {
        stopLoopback();
        m_pending = {};
        const SavedAccount account = activeAccount();
        if (account.valid())
            activate(account);
    });
    m_welcomeBack->hide();
    layout->addWidget(m_welcomeBack, 0, Qt::AlignLeft);

    center->addWidget(card);
    center->addStretch(1);
    outer->addLayout(center);
    outer->addStretch(2);
    return page;
}

QWidget *MastodonFeature::buildSidebar()
{
    auto *sidebar = new QWidget;
    sidebar->setObjectName(QStringLiteral("mastodonSidebar"));
    sidebar->setFixedWidth(248);
    auto *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(10, 12, 8, 10);
    layout->setSpacing(10);

    auto *accountRow = new QHBoxLayout;
    accountRow->setSpacing(10);
    m_accountAvatar = new AvatarLabel(38);
    m_accountAvatar->setCursor(Qt::PointingHandCursor);
    m_accountAvatar->clicked = [this] { openPage({QStringLiteral("profile"), QString(), QString()}); };
    accountRow->addWidget(m_accountAvatar);
    auto *names = new QVBoxLayout;
    names->setSpacing(0);
    m_accountName = new QLabel;
    m_accountName->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600;"));
    m_accountName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    names->addWidget(m_accountName);
    m_accountHandle = mutedLabel(QString(), sidebar);
    m_accountHandle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    names->addWidget(m_accountHandle);
    accountRow->addLayout(names, 1);
    m_accountButton = iconButton(QStringLiteral("chevron-down"), QStringLiteral("Accounts"), sidebar);
    m_accountButton->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(m_accountButton);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        for (const SavedAccount &account : m_accounts) {
            QAction *action = menu->addAction(account.displayName.isEmpty() ? account.handle()
                                                                            : account.displayName + QStringLiteral("  ") + account.handle());
            action->setCheckable(true);
            action->setChecked(account.key() == m_activeKey);
            const QString key = account.key();
            connect(action, &QAction::triggered, this, [this, key] { switchAccount(key); });
        }
        menu->addSeparator();
        menu->addAction(QStringLiteral("Add another account…"), this, [this] { showSignIn(); });
        menu->addAction(QStringLiteral("Edit profile"), this, [this] { openPage({QStringLiteral("profile"), QString(), QString()}); });
        menu->addAction(QStringLiteral("Open my profile on the server"), this, [this] {
            const QUrl url(m_self.value(QStringLiteral("url")).toString());
            if (safeWebUrl(url))
                openExternal(url);
        });
        menu->addSeparator();
        menu->addAction(QStringLiteral("Sign out of this account"), this, [this] {
            const SavedAccount account = activeAccount();
            if (confirm(QStringLiteral("Sign out"), QStringLiteral("Sign out of %1 on this computer?").arg(account.handle())))
                signOut(account.key());
        });
    });
    m_accountButton->setMenu(menu);
    m_accountButton->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0; }"));
    accountRow->addWidget(m_accountButton);
    layout->addLayout(accountRow);

    auto *newPost = new QPushButton(QStringLiteral("New post"));
    newPost->setObjectName(QStringLiteral("primaryButton"));
    newPost->setCursor(Qt::PointingHandCursor);
    newPost->setIcon(icon(QStringLiteral("pencil"), Qt::white));
    connect(newPost, &QPushButton::clicked, this, [this] { compose(); });
    layout->addWidget(newPost);

    m_searchField = new QLineEdit;
    m_searchField->setObjectName(QStringLiteral("mastodonSearch"));
    m_searchField->setPlaceholderText(QStringLiteral("Search people, hashtags, posts"));
    m_searchField->setClearButtonEnabled(true);
    m_searchField->addAction(icon(QStringLiteral("search"), mutedColor(this)), QLineEdit::LeadingPosition);
    connect(m_searchField, &QLineEdit::returnPressed, this, [this] {
        const QString query = m_searchField->text().trimmed();
        if (!query.isEmpty())
            openPage({QStringLiteral("search"), query, QString()});
    });
    layout->addWidget(m_searchField);

    m_nav = new QListWidget;
    m_nav->setObjectName(QStringLiteral("mastodonNav"));
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setMouseTracking(true);
    m_nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nav->setItemDelegate(new NavDelegate(m_nav));
    m_nav->setStyleSheet(QStringLiteral("QListWidget#mastodonNav { background: transparent; border: none; outline: none; }"
                                        "QListWidget#mastodonNav::item { background: transparent; border: none; }"));
    connect(m_nav, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (m_navUpdating || !item || item->data(kHeaderRole).toBool())
            return;
        m_navJustChanged = true;
        openPage({item->data(kKindRole).toString(), item->data(kArgRole).toString(), QString()});
    });
    connect(m_nav, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const bool changed = m_navJustChanged;
        m_navJustChanged = false;
        if (changed || !item || item->data(kHeaderRole).toBool())
            return;
        const Page page{item->data(kKindRole).toString(), item->data(kArgRole).toString(), QString()};
        if (m_page.kind == page.kind && m_page.arg == page.arg)
            reload();
    });
    layout->addWidget(m_nav, 1);
    return sidebar;
}

QWidget *MastodonFeature::buildMain()
{
    auto *main = new QWidget;
    main->setObjectName(QStringLiteral("mastodonMain"));
    auto *layout = new QVBoxLayout(main);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new QWidget;
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(14, 12, 14, 6);
    headerLayout->setSpacing(8);
    m_backButton = iconButton(QStringLiteral("arrow-left"), QStringLiteral("Back"), header);
    connect(m_backButton, &QToolButton::clicked, this, [this] { goBack(); });
    headerLayout->addWidget(m_backButton);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(0);
    m_title = new QLabel;
    m_title->setObjectName(QStringLiteral("pageTitle"));
    m_title->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));
    titles->addWidget(m_title);
    m_subtitle = mutedLabel(QString(), header);
    titles->addWidget(m_subtitle);
    headerLayout->addLayout(titles, 1);
    m_headerActions = new QHBoxLayout;
    m_headerActions->setSpacing(6);
    headerLayout->addLayout(m_headerActions);
    auto *refreshButton = iconButton(QStringLiteral("sync"), QStringLiteral("Refresh"), header);
    connect(refreshButton, &QToolButton::clicked, this, [this] { reload(); });
    headerLayout->addWidget(refreshButton);
    layout->addWidget(header);

    m_tabBar = new QWidget;
    m_tabLayout = new QHBoxLayout(m_tabBar);
    m_tabLayout->setContentsMargins(14, 0, 14, 6);
    m_tabLayout->setSpacing(4);
    m_tabBar->hide();
    layout->addWidget(m_tabBar);

    m_notice = new QLabel;
    m_notice->setObjectName(QStringLiteral("mastodonNotice"));
    m_notice->setWordWrap(true);
    m_notice->setContentsMargins(16, 4, 16, 6);
    m_notice->hide();
    layout->addWidget(m_notice);

    m_scroll = new QScrollArea;
    m_scroll->setObjectName(QStringLiteral("mastodonScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *canvas = new QWidget;
    canvas->setObjectName(QStringLiteral("mastodonCanvas"));
    auto *canvasLayout = new QHBoxLayout(canvas);
    canvasLayout->setContentsMargins(0, 0, 0, 0);
    canvasLayout->addStretch(1);
    m_column = new QWidget;
    m_column->setObjectName(QStringLiteral("mastodonColumn"));
    m_column->setMaximumWidth(720);
    m_column->setMinimumWidth(360);
    m_column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *columnLayout = new QVBoxLayout(m_column);
    columnLayout->setContentsMargins(0, 0, 0, 16);
    columnLayout->setSpacing(0);
    m_list = new QVBoxLayout;
    m_list->setSpacing(0);
    columnLayout->addLayout(m_list);
    m_loading = mutedLabel(QStringLiteral("Loading…"), m_column);
    m_loading->setAlignment(Qt::AlignCenter);
    m_loading->setContentsMargins(0, 18, 0, 18);
    m_loading->hide();
    columnLayout->addWidget(m_loading);
    m_loadMoreButton = new QPushButton(QStringLiteral("Load more"));
    m_loadMoreButton->setObjectName(QStringLiteral("ghostButton"));
    m_loadMoreButton->setCursor(Qt::PointingHandCursor);
    connect(m_loadMoreButton, &QPushButton::clicked, this, [this] { loadMore(); });
    m_loadMoreButton->hide();
    auto *moreRow = new QHBoxLayout;
    moreRow->setContentsMargins(0, 12, 0, 0);
    moreRow->addStretch(1);
    moreRow->addWidget(m_loadMoreButton);
    moreRow->addStretch(1);
    columnLayout->addLayout(moreRow);
    columnLayout->addStretch(1);
    canvasLayout->addWidget(m_column, 4);
    canvasLayout->addStretch(1);
    m_scroll->setWidget(canvas);
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        QScrollBar *bar = m_scroll->verticalScrollBar();
        if (m_loadMore && !m_loading->isVisible() && bar->maximum() > 0 && value >= bar->maximum() - 300)
            loadMore();
    });
    layout->addWidget(m_scroll, 1);
    return main;
}

void MastodonFeature::loadAccounts()
{
    if (!m_host.setting)
        return;
    m_accounts = accountsFromJson(m_host.setting(QStringLiteral("accounts"), QString()).toString().toUtf8());
    m_activeKey = m_host.setting(QStringLiteral("active"), QString()).toString();
}

void MastodonFeature::saveAccounts()
{
    if (!m_host.setSetting)
        return;
    m_host.setSetting(QStringLiteral("accounts"), QString::fromUtf8(accountsToJson(m_accounts)));
    m_host.setSetting(QStringLiteral("active"), m_activeKey);
}

void MastodonFeature::activate(const SavedAccount &account)
{
    stopLoopback();
    m_pending = {};
    m_activeKey = account.key();
    saveAccounts();
    m_client->setAccount(account.instance, account.token);
    m_self = {};
    m_lists = {};
    m_cardContext.reset();
    m_history.clear();
    m_page = {};
    m_seenStatuses.clear();
    m_followRequests = 0;
    setUnread(0);
    m_badgeCheckedMs = 0;
    m_stack->setCurrentWidget(m_app);
    refreshAccountHeader();
    rebuildNav();
    beginPage(QStringLiteral("Home"));
    setLoading(true);
    const QString key = account.key();
    m_client->get(QStringLiteral("/api/v1/accounts/verify_credentials"), {}, [this, key](const Result &result) {
        if (key != m_activeKey)
            return;
        if (!result.ok) {
            setLoading(false);
            m_page = {QStringLiteral("home"), QString(), QString()};
            if (result.status != 401)
                setNotice(QStringLiteral("%1 Press refresh to try again.").arg(result.error), true);
            return;
        }
        m_self = result.object();
        for (SavedAccount &saved : m_accounts) {
            if (saved.key() != key)
                continue;
            saved.id = m_self.value(QStringLiteral("id")).toString();
            saved.acct = m_self.value(QStringLiteral("acct")).toString();
            saved.displayName = displayName(m_self);
            saved.avatar = m_self.value(QStringLiteral("avatar_static")).toString();
            m_activeKey = saved.key();
        }
        saveAccounts();
        m_client->trustImageHost(QUrl(m_self.value(QStringLiteral("avatar_static")).toString()).host());
        m_client->trustImageHost(QUrl(m_self.value(QStringLiteral("header_static")).toString()).host());
        m_cardContext.reset();
        refreshAccountHeader();
        loadInstance();
        loadLists();
        loadSidebarCounts();
        m_badgeCheckedMs = 0;
        prepareNavigationBadge();
        openPage({QStringLiteral("home"), QString(), QString()}, false);
    });
}

void MastodonFeature::switchAccount(const QString &key)
{
    for (const SavedAccount &account : m_accounts) {
        if (account.key() == key) {
            activate(account);
            return;
        }
    }
}

void MastodonFeature::signOut(const QString &key)
{
    SavedAccount removed;
    for (int index = 0; index < m_accounts.size(); ++index) {
        if (m_accounts.at(index).key() == key) {
            removed = m_accounts.takeAt(index);
            break;
        }
    }
    if (!removed.valid())
        return;
    if (!removed.clientId.isEmpty() && !removed.clientSecret.isEmpty()) {
        m_client->anonymous("POST", QUrl(removed.instance + QStringLiteral("/oauth/revoke")),
                            {{QStringLiteral("client_id"), removed.clientId},
                             {QStringLiteral("client_secret"), removed.clientSecret},
                             {QStringLiteral("token"), removed.token}},
                            {});
    }
    const bool wasActive = key == m_activeKey;
    if (wasActive)
        m_activeKey.clear();
    saveAccounts();
    if (!wasActive)
        return;
    if (!m_accounts.isEmpty()) {
        activate(m_accounts.first());
        return;
    }
    m_client->setAccount(QString(), QString());
    m_self = {};
    setUnread(0);
    showSignIn(removed.instance, QStringLiteral("Signed out of %1.").arg(removed.handle()));
}

void MastodonFeature::sessionExpired()
{
    const SavedAccount account = activeAccount();
    if (!account.valid() || m_stack->currentWidget() != m_app)
        return;
    m_accounts.removeIf([&](const SavedAccount &saved) { return saved.key() == account.key(); });
    m_activeKey.clear();
    saveAccounts();
    m_client->setAccount(QString(), QString());
    m_self = {};
    setUnread(0);
    showSignIn(account.instance, QStringLiteral("Your sign-in to %1 has expired or was revoked. Sign in again to continue.")
                                     .arg(instanceHost(account.instance)));
}

void MastodonFeature::showSignIn(const QString &instance, const QString &message)
{
    m_stack->setCurrentWidget(m_welcome);
    if (!instance.isEmpty())
        m_instanceField->setText(instanceHost(instance));
    m_codeField->clear();
    m_tokenField->clear();
    const SavedAccount account = activeAccount();
    m_welcomeBack->setVisible(account.valid() && m_client->signedIn());
    m_welcomeBack->setText(QStringLiteral("Back to %1").arg(account.handle()));
    setWelcomeState(QStringLiteral("idle"), message, false);
    m_instanceField->setFocus();
}

void MastodonFeature::setWelcomeState(const QString &state, const QString &message, bool error)
{
    const QString host = instanceHost(m_pending.instance);
    const bool busy = state == QLatin1String("registering") || state == QLatin1String("exchanging");
    m_signInButton->setEnabled(!busy);
    m_instanceField->setEnabled(!busy);
    m_waitingBox->setVisible(state == QLatin1String("waiting") || state == QLatin1String("code"));
    m_codeField->setVisible(state == QLatin1String("code") || m_pending.loopback.isEmpty());
    if (state == QLatin1String("waiting")) {
        m_waitingText->setText(m_pending.loopback.isEmpty()
                                   ? QStringLiteral("Approve ForkMesh in the browser page that opened for %1, then paste "
                                                    "the code it shows below.").arg(host)
                                   : QStringLiteral("Approve ForkMesh in the browser page that opened for %1. This page "
                                                    "continues on its own once you do.").arg(host));
    } else if (state == QLatin1String("code")) {
        m_waitingText->setText(QStringLiteral("Approve ForkMesh in the browser page that opened for %1, then paste the "
                                              "code it shows below.").arg(host));
        m_codeField->setFocus();
    }
    QString text = message;
    if (text.isEmpty() && state == QLatin1String("registering"))
        text = QStringLiteral("Contacting %1…").arg(host);
    if (text.isEmpty() && state == QLatin1String("exchanging"))
        text = QStringLiteral("Finishing sign-in…");
    m_welcomeNotice->setText(text);
    m_welcomeNotice->setStyleSheet(error ? QStringLiteral("color: #d1242f;") : QString());
    m_welcomeNotice->setVisible(!text.isEmpty());
}

void MastodonFeature::startLoopback()
{
    stopLoopback();
    auto *server = new QTcpServer(this);
    if (!server->listen(QHostAddress::LocalHost, 0)) {
        delete server;
        return;
    }
    m_loopback = server;
    m_pending.loopback = QStringLiteral("http://127.0.0.1:%1/callback").arg(server->serverPort());
    connect(server, &QTcpServer::newConnection, this, [this, server] {
        while (QTcpSocket *socket = server->nextPendingConnection()) {
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                QByteArray buffer = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", buffer);
                if (!buffer.contains("\r\n\r\n") && buffer.size() < 16384)
                    return;
                const QUrlQuery query = callbackQuery(buffer);
                const QString code = query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
                const QString state = query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
                const bool matched = !code.isEmpty() && !m_pending.state.isEmpty() && state == m_pending.state;
                const bool denied = !query.queryItemValue(QStringLiteral("error")).isEmpty() && state == m_pending.state;
                const QByteArray page = callbackPage(matched).toUtf8();
                QByteArray response = (matched || denied || !query.isEmpty() ? QByteArray("HTTP/1.1 200 OK\r\n")
                                                                             : QByteArray("HTTP/1.1 404 Not Found\r\n"));
                response += "Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n";
                response += "Content-Length: " + QByteArray::number(page.size()) + "\r\n\r\n" + page;
                socket->write(response);
                socket->disconnectFromHost();
                if (matched)
                    QTimer::singleShot(0, this, [this, code] { exchangeCode(code); });
                else if (denied)
                    setWelcomeState(QStringLiteral("idle"), QStringLiteral("Sign-in was cancelled on the server."), true);
            });
        }
    });
}

void MastodonFeature::stopLoopback()
{
    if (m_loopback) {
        m_loopback->close();
        m_loopback->deleteLater();
    }
    m_loopback = nullptr;
}

quint16 MastodonFeature::callbackPort() const
{
    return m_loopback ? m_loopback->serverPort() : 0;
}

void MastodonFeature::beginBrowserSignIn(const QString &raw)
{
    const QString instance = normalizeInstance(raw);
    if (instance.isEmpty()) {
        setWelcomeState(QStringLiteral("idle"), QStringLiteral("Enter your server's address, such as mastodon.social."), true);
        return;
    }
    m_pending = {};
    m_pending.instance = instance;
    m_pending.verifier = randomToken(48);
    m_pending.state = randomToken(18);
    startLoopback();
    QString redirects = kOutOfBand;
    if (!m_pending.loopback.isEmpty())
        redirects = m_pending.loopback + QLatin1Char('\n') + kOutOfBand;
    setWelcomeState(QStringLiteral("registering"));
    const QString state = m_pending.state;
    m_client->anonymous("POST", QUrl(instance + QStringLiteral("/api/v1/apps")),
                        {{QStringLiteral("client_name"), kClientName},
                         {QStringLiteral("redirect_uris"), redirects},
                         {QStringLiteral("scopes"), kScopes},
                         {QStringLiteral("website"), kClientWebsite}},
                        [this, state](const Result &result) {
                            if (m_pending.state != state)
                                return;
                            const QString clientId = result.object().value(QStringLiteral("client_id")).toString();
                            if (!result.ok || clientId.isEmpty()) {
                                stopLoopback();
                                setWelcomeState(QStringLiteral("idle"),
                                                result.ok ? QStringLiteral("That server did not accept ForkMesh as an app.")
                                                          : QStringLiteral("Could not reach that Mastodon server: %1").arg(result.error),
                                                true);
                                return;
                            }
                            m_pending.clientId = clientId;
                            m_pending.clientSecret = result.object().value(QStringLiteral("client_secret")).toString();
                            m_pending.redirect = m_pending.loopback.isEmpty() ? kOutOfBand : m_pending.loopback;
                            openExternal(authorizeUrl(m_pending.instance, m_pending.clientId, m_pending.redirect,
                                                      m_pending.state, pkceChallenge(m_pending.verifier)));
                            setWelcomeState(m_pending.loopback.isEmpty() ? QStringLiteral("code") : QStringLiteral("waiting"));
                        });
}

void MastodonFeature::useCodeInstead()
{
    if (m_pending.clientId.isEmpty())
        return;
    m_pending.redirect = kOutOfBand;
    openExternal(authorizeUrl(m_pending.instance, m_pending.clientId, m_pending.redirect, m_pending.state,
                              pkceChallenge(m_pending.verifier)));
    setWelcomeState(QStringLiteral("code"));
}

void MastodonFeature::finishSignIn(const QString &codeOrUrl)
{
    if (m_pending.clientId.isEmpty()) {
        setWelcomeState(QStringLiteral("idle"), QStringLiteral("Start by choosing your server."), true);
        return;
    }
    if (codeOrUrl.contains(QLatin1String("state="))) {
        const QString query = codeOrUrl.section(QLatin1Char('?'), 1);
        if (QUrlQuery(query).queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded) != m_pending.state) {
            setWelcomeState(QStringLiteral("code"), QStringLiteral("That link belongs to a different sign-in attempt."), true);
            return;
        }
    }
    const QString code = codeFromCallback(codeOrUrl);
    if (code.isEmpty()) {
        setWelcomeState(QStringLiteral("code"), QStringLiteral("That does not look like an authorization code."), true);
        return;
    }
    exchangeCode(code);
}

void MastodonFeature::exchangeCode(const QString &code)
{
    if (m_pending.clientId.isEmpty())
        return;
    setWelcomeState(QStringLiteral("exchanging"));
    const PendingSignIn pending = m_pending;
    m_client->anonymous("POST", QUrl(pending.instance + QStringLiteral("/oauth/token")),
                        {{QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
                         {QStringLiteral("code"), code},
                         {QStringLiteral("client_id"), pending.clientId},
                         {QStringLiteral("client_secret"), pending.clientSecret},
                         {QStringLiteral("redirect_uri"), pending.redirect},
                         {QStringLiteral("code_verifier"), pending.verifier},
                         {QStringLiteral("scope"), kScopes}},
                        [this, pending](const Result &result) {
                            if (m_pending.state != pending.state)
                                return;
                            const QString token = result.object().value(QStringLiteral("access_token")).toString();
                            if (!result.ok || token.isEmpty()) {
                                setWelcomeState(QStringLiteral("code"),
                                                QStringLiteral("The server did not finish the sign-in: %1")
                                                    .arg(result.ok ? QStringLiteral("no access token was returned.") : result.error),
                                                true);
                                return;
                            }
                            stopLoopback();
                            afterSignIn(pending.instance, token, pending.clientId, pending.clientSecret);
                        });
}

void MastodonFeature::signInWithToken(const QString &raw, const QString &token)
{
    const QString instance = normalizeInstance(raw);
    if (instance.isEmpty()) {
        setWelcomeState(QStringLiteral("idle"), QStringLiteral("Enter your server's address, such as mastodon.social."), true);
        return;
    }
    if (token.trimmed().isEmpty()) {
        setWelcomeState(QStringLiteral("idle"), QStringLiteral("Paste an access token first."), true);
        return;
    }
    stopLoopback();
    m_pending = {};
    m_pending.instance = instance;
    m_pending.state = randomToken(18);
    setWelcomeState(QStringLiteral("exchanging"));
    afterSignIn(instance, token.trimmed(), QString(), QString());
}

void MastodonFeature::afterSignIn(const QString &instance, const QString &token, const QString &clientId,
                                  const QString &clientSecret)
{
    const SavedAccount previous = activeAccount();
    const bool hadSession = m_client->signedIn();
    m_client->setAccount(instance, token);
    const QString state = m_pending.state;
    m_client->get(QStringLiteral("/api/v1/accounts/verify_credentials"), {},
                  [this, instance, token, clientId, clientSecret, previous, hadSession, state](const Result &result) {
                      if (m_pending.state != state)
                          return;
                      if (!result.ok) {
                          if (hadSession && previous.valid())
                              m_client->setAccount(previous.instance, previous.token);
                          else
                              m_client->setAccount(QString(), QString());
                          setWelcomeState(QStringLiteral("idle"),
                                          QStringLiteral("Could not sign in: %1").arg(result.error), true);
                          return;
                      }
                      const QJsonObject self = result.object();
                      SavedAccount account;
                      account.instance = instance;
                      account.token = token;
                      account.clientId = clientId;
                      account.clientSecret = clientSecret;
                      account.id = self.value(QStringLiteral("id")).toString();
                      account.acct = self.value(QStringLiteral("acct")).toString();
                      account.displayName = displayName(self);
                      account.avatar = self.value(QStringLiteral("avatar_static")).toString();
                      m_accounts.removeIf([&](const SavedAccount &saved) { return saved.key() == account.key(); });
                      m_accounts.append(account);
                      activate(account);
                      setNotice(QStringLiteral("Signed in as %1.").arg(account.handle()));
                  });
}

void MastodonFeature::refreshAccountHeader()
{
    const SavedAccount account = activeAccount();
    const QString name = m_self.isEmpty() ? account.displayName : displayName(m_self);
    m_accountName->setText(name.isEmpty() ? account.handle() : name);
    m_accountHandle->setText(account.handle());
    m_accountHandle->setToolTip(account.handle());
    m_accountAvatar->setSource(m_client.get(),
                               m_self.isEmpty() ? account.avatar : m_self.value(QStringLiteral("avatar_static")).toString(),
                               name);
    m_accountAvatar->setToolTip(QStringLiteral("Edit profile"));
}

void MastodonFeature::rebuildNav()
{
    m_navUpdating = true;
    m_nav->clear();
    const auto add = [this](const QString &label, const QString &iconName, const QString &kind, const QString &arg = QString()) {
        auto *item = new QListWidgetItem(label, m_nav);
        item->setData(kKindRole, kind);
        item->setData(kArgRole, arg);
        item->setData(kIconRole, iconName);
        item->setToolTip(label);
        return item;
    };
    const auto header = [this](const QString &label) {
        auto *item = new QListWidgetItem(label, m_nav);
        item->setData(kHeaderRole, true);
        item->setFlags(Qt::NoItemFlags);
    };
    add(QStringLiteral("Home"), QStringLiteral("home"), QStringLiteral("home"));
    QListWidgetItem *notifications = add(QStringLiteral("Notifications"), QStringLiteral("bell"), QStringLiteral("notifications"));
    notifications->setData(kCountRole, m_unread);
    add(QStringLiteral("Explore"), QStringLiteral("sparkle"), QStringLiteral("explore"));
    add(QStringLiteral("Local"), QStringLiteral("people"), QStringLiteral("local"));
    add(QStringLiteral("Federated"), QStringLiteral("globe"), QStringLiteral("federated"));
    add(QStringLiteral("Private mentions"), QStringLiteral("at"), QStringLiteral("conversations"));
    add(QStringLiteral("Bookmarks"), QStringLiteral("bookmark"), QStringLiteral("bookmarks"));
    add(QStringLiteral("Favourites"), QStringLiteral("star"), QStringLiteral("favourites"));
    header(QStringLiteral("Lists"));
    for (const QJsonValue &value : m_lists) {
        const QJsonObject list = value.toObject();
        add(list.value(QStringLiteral("title")).toString(), QStringLiteral("list-unordered"), QStringLiteral("list"),
            list.value(QStringLiteral("id")).toString());
    }
    add(QStringLiteral("Manage lists"), QStringLiteral("gear"), QStringLiteral("lists"));
    header(QStringLiteral("Account"));
    add(QStringLiteral("Profile"), QStringLiteral("person"), QStringLiteral("profile"));
    QListWidgetItem *follows = add(QStringLiteral("Followers & following"), QStringLiteral("people"), QStringLiteral("follows"));
    follows->setData(kCountRole, m_followRequests);
    add(QStringLiteral("Blocks & mutes"), QStringLiteral("circle-slash"), QStringLiteral("moderation"));
    add(QStringLiteral("Hashtags"), QStringLiteral("hash"), QStringLiteral("hashtags"));
    add(QStringLiteral("Filters"), QStringLiteral("filter"), QStringLiteral("filters"));
    add(QStringLiteral("Scheduled posts"), QStringLiteral("clock"), QStringLiteral("scheduled"));
    add(QStringLiteral("Preferences"), QStringLiteral("gear"), QStringLiteral("preferences"));
    m_navUpdating = false;
    selectNav();
}

void MastodonFeature::selectNav()
{
    m_navUpdating = true;
    QListWidgetItem *match = nullptr;
    for (int row = 0; row < m_nav->count(); ++row) {
        QListWidgetItem *item = m_nav->item(row);
        if (item->data(kHeaderRole).toBool())
            continue;
        const QString kind = item->data(kKindRole).toString();
        const QString arg = item->data(kArgRole).toString();
        if (kind == m_page.kind && arg == m_page.arg) {
            match = item;
            break;
        }
        if (kind == QLatin1String("lists") && m_page.kind == QLatin1String("list-members"))
            match = item;
        if (kind == QLatin1String("follows") && m_page.kind == QLatin1String("follows"))
            match = item;
    }
    if (match) {
        m_nav->setCurrentItem(match);
    } else {
        m_nav->clearSelection();
        m_nav->setCurrentRow(-1);
    }
    m_navUpdating = false;
}

void MastodonFeature::loadInstance()
{
    const quint64 generation = m_generation;
    Q_UNUSED(generation);
    m_client->get(QStringLiteral("/api/v2/instance"), {}, [this](const Result &result) {
        const auto apply = [this](const QJsonObject &instance) {
            const QJsonObject configuration = instance.value(QStringLiteral("configuration")).toObject();
            const QJsonObject statuses = configuration.value(QStringLiteral("statuses")).toObject();
            const QJsonObject polls = configuration.value(QStringLiteral("polls")).toObject();
            m_limits.maxCharacters = statuses.value(QStringLiteral("max_characters")).toInt(
                instance.value(QStringLiteral("max_toot_chars")).toInt(500));
            m_limits.maxMedia = statuses.value(QStringLiteral("max_media_attachments")).toInt(4);
            m_limits.maxOptions = polls.value(QStringLiteral("max_options")).toInt(4);
            m_limits.maxOptionCharacters = polls.value(QStringLiteral("max_characters_per_option")).toInt(50);
            const QString mediaHost = QUrl(instance.value(QStringLiteral("thumbnail")).toObject().value(QStringLiteral("url")).toString()).host();
            m_client->trustImageHost(mediaHost);
        };
        if (result.ok) {
            apply(result.object());
            return;
        }
        m_client->get(QStringLiteral("/api/v1/instance"), {}, [apply](const Result &fallback) {
            if (fallback.ok)
                apply(fallback.object());
        });
    });
}

void MastodonFeature::loadLists()
{
    m_client->get(QStringLiteral("/api/v1/lists"), {}, [this](const Result &result) {
        if (!result.ok)
            return;
        m_lists = result.array();
        rebuildNav();
    });
}

void MastodonFeature::loadSidebarCounts()
{
    m_client->get(QStringLiteral("/api/v1/follow_requests"), QUrlQuery(QStringLiteral("limit=40")), [this](const Result &result) {
        if (!result.ok)
            return;
        m_followRequests = static_cast<int>(result.array().size());
        for (int row = 0; row < m_nav->count(); ++row) {
            if (m_nav->item(row)->data(kKindRole).toString() == QLatin1String("follows"))
                m_nav->item(row)->setData(kCountRole, m_followRequests);
        }
    });
}

void MastodonFeature::prepareNavigationBadge()
{
    if (!m_client->signedIn())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_badgeCheckedMs && now - m_badgeCheckedMs < 60000)
        return;
    m_badgeCheckedMs = now;
    QUrlQuery markers;
    markers.addQueryItem(QStringLiteral("timeline[]"), QStringLiteral("notifications"));
    const QString key = m_activeKey;
    m_client->get(QStringLiteral("/api/v1/markers"), markers, [this, key](const Result &result) {
        if (key != m_activeKey || !result.ok)
            return;
        const QString lastRead = result.object().value(QStringLiteral("notifications")).toObject()
                                     .value(QStringLiteral("last_read_id")).toString();
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("limit"), QStringLiteral("40"));
        if (!lastRead.isEmpty())
            query.addQueryItem(QStringLiteral("since_id"), lastRead);
        m_client->get(QStringLiteral("/api/v1/notifications"), query, [this, key, lastRead](const Result &list) {
            if (key != m_activeKey || !list.ok)
                return;
            setUnread(lastRead.isEmpty() ? 0 : static_cast<int>(list.array().size()));
        });
    });
}

void MastodonFeature::setUnread(int count)
{
    if (m_nav) {
        for (int row = 0; row < m_nav->count(); ++row) {
            if (m_nav->item(row)->data(kKindRole).toString() == QLatin1String("notifications"))
                m_nav->item(row)->setData(kCountRole, count);
        }
    }
    if (count == m_unread)
        return;
    m_unread = count;
    if (m_host.navigationBadgeChanged)
        m_host.navigationBadgeChanged();
}

void MastodonFeature::markNotificationsRead(const QString &newestId)
{
    if (newestId.isEmpty())
        return;
    m_client->send("POST", QStringLiteral("/api/v1/markers"),
                   {{QStringLiteral("notifications"), QJsonObject{{QStringLiteral("last_read_id"), newestId}}}},
                   [this](const Result &result) {
                       if (result.ok)
                           setUnread(0);
                   });
}

void MastodonFeature::setHostSearchQuery(const QString &query)
{
    m_hostQuery = query.trimmed();
    if (!signedIn())
        return;
    if (m_searchField->text() != m_hostQuery)
        m_searchField->setText(m_hostQuery);
    m_searchTimer->start();
}

QList<forkmesh::market::NativePackageSearchHit> MastodonFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty() || limit <= 0)
        return hits;
    if (!m_client->signedIn()) {
        if (QStringLiteral("mastodon sign in").contains(needle, Qt::CaseInsensitive))
            hits.append({QStringLiteral("nav:signin:"), QStringLiteral("Sign in to Mastodon"), QStringLiteral("Mastodon")});
        return hits;
    }
    for (int row = 0; row < m_nav->count() && hits.size() < limit; ++row) {
        const QListWidgetItem *item = m_nav->item(row);
        if (item->data(kHeaderRole).toBool() || !item->text().contains(needle, Qt::CaseInsensitive))
            continue;
        hits.append({QStringLiteral("nav:%1:%2").arg(item->data(kKindRole).toString(), item->data(kArgRole).toString()),
                     item->text(), QStringLiteral("Mastodon")});
    }
    for (auto it = m_seenStatuses.constBegin(); it != m_seenStatuses.constEnd() && hits.size() < limit; ++it) {
        const QJsonObject status = it.value();
        const QString text = plainText(status.value(QStringLiteral("content")).toString());
        const QJsonObject account = status.value(QStringLiteral("account")).toObject();
        if (!text.contains(needle, Qt::CaseInsensitive) && !displayName(account).contains(needle, Qt::CaseInsensitive))
            continue;
        hits.append({QStringLiteral("status:") + it.key(), elide(text, 90),
                     QStringLiteral("Mastodon · %1").arg(displayName(account))});
    }
    if (hits.size() < limit && needle.size() >= 2)
        hits.append({QStringLiteral("search:") + needle, QStringLiteral("Search Mastodon for “%1”").arg(needle),
                     QStringLiteral("Mastodon")});
    return hits;
}

void MastodonFeature::activateSearchHit(const QString &id)
{
    if (m_host.showPage)
        m_host.showPage();
    if (id.startsWith(QLatin1String("nav:"))) {
        const QString kind = id.section(QLatin1Char(':'), 1, 1);
        if (kind == QLatin1String("signin")) {
            showSignIn();
            return;
        }
        openPage({kind, id.section(QLatin1Char(':'), 2), QString()});
    } else if (id.startsWith(QLatin1String("status:"))) {
        openPage({QStringLiteral("thread"), id.mid(7), QString()});
    } else if (id.startsWith(QLatin1String("search:"))) {
        openPage({QStringLiteral("search"), id.mid(7), QString()});
    }
}

void MastodonFeature::themeChanged()
{
    restyle();
}

void MastodonFeature::changeEvent(QEvent *event)
{
    forkmesh::market::NativePackageWidget::changeEvent(event);
    if (event->type() != QEvent::PaletteChange || m_restylePending)
        return;
    m_restylePending = true;
    QTimer::singleShot(0, this, [this] {
        m_restylePending = false;
        restyle();
    });
}

void MastodonFeature::restyle()
{
    for (QToolButton *button : findChildren<QToolButton *>(QStringLiteral("mastodonIconButton"))) {
        const QString name = button->property("mastodonIcon").toString();
        if (!name.isEmpty())
            button->setIcon(icon(name, button->isCheckable() && button->isChecked() ? accentColor(this) : mutedColor(this)));
    }
    for (QLabel *label : findChildren<QLabel *>(QStringLiteral("mutedLabel")))
        label->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; background: transparent;").arg(mutedColor(this).name()));
    m_nav->viewport()->update();
    if (signedIn() && !m_page.kind.isEmpty())
        reload();
}

CardContextPtr MastodonFeature::cardContext()
{
    if (m_cardContext)
        return m_cardContext;
    auto context = std::make_shared<CardContext>();
    context->client = m_client.get();
    context->instance = m_client->instance();
    context->selfId = m_self.value(QStringLiteral("id")).toString();
    context->reply = [this](const QJsonObject &status) { compose(status); };
    context->act = [this](const QJsonObject &status, const QString &action) { act(status, action); };
    context->more = [this](const QJsonObject &status, QToolButton *anchor) { openStatusMenu(status, anchor); };
    context->openThread = [this](const QString &id) { openPage({QStringLiteral("thread"), id, QString()}); };
    context->openAccount = [this](const QJsonObject &account) { openAccountObject(account); };
    context->openLink = [this](const QString &link) { openLink(link); };
    context->openMedia = [this](const QJsonArray &media, int index) { openMedia(media, index); };
    context->vote = [this](const QJsonObject &poll, const QList<int> &choices) { vote(poll, choices); };
    m_cardContext = context;
    return context;
}

void MastodonFeature::openPage(const Page &page, bool remember)
{
    if (!m_client->signedIn())
        return;
    if (remember && !m_page.kind.isEmpty() && !(m_page.kind == page.kind && m_page.arg == page.arg)) {
        m_history.append(m_page);
        while (m_history.size() > 60)
            m_history.removeFirst();
    }
    m_page = page;
    ++m_generation;
    m_stack->setCurrentWidget(m_app);
    selectNav();
    m_scroll->verticalScrollBar()->setValue(0);
    const QString kind = page.kind;
    if (cardContext())
        m_cardContext->filterContext = QStringLiteral("public");
    if (kind == QLatin1String("home")) {
        showHome();
    } else if (kind == QLatin1String("local") || kind == QLatin1String("federated")) {
        const bool local = kind == QLatin1String("local");
        beginPage(local ? QStringLiteral("Local") : QStringLiteral("Federated"),
                  local ? QStringLiteral("Public posts from people on %1").arg(instanceHost(m_client->instance()))
                        : QStringLiteral("Public posts from across the fediverse that %1 knows about")
                              .arg(instanceHost(m_client->instance())));
        setTabs({{QString(), QStringLiteral("All")}, {QStringLiteral("media"), QStringLiteral("Media only")}});
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("limit"), QStringLiteral("30"));
        if (local)
            query.addQueryItem(QStringLiteral("local"), QStringLiteral("true"));
        if (page.tab == QLatin1String("media"))
            query.addQueryItem(QStringLiteral("only_media"), QStringLiteral("true"));
        loadStatuses(QStringLiteral("/api/v1/timelines/public"), query, QStringLiteral("public"),
                     QStringLiteral("Nothing here yet."));
    } else if (kind == QLatin1String("bookmarks")) {
        beginPage(QStringLiteral("Bookmarks"), QStringLiteral("Posts you saved for later. Only you can see them."));
        loadStatuses(QStringLiteral("/api/v1/bookmarks"), QUrlQuery(QStringLiteral("limit=30")), QString(),
                     QStringLiteral("You have not bookmarked any posts yet."));
    } else if (kind == QLatin1String("favourites")) {
        beginPage(QStringLiteral("Favourites"), QStringLiteral("Posts you favourited"));
        loadStatuses(QStringLiteral("/api/v1/favourites"), QUrlQuery(QStringLiteral("limit=30")), QString(),
                     QStringLiteral("You have not favourited any posts yet."));
    } else if (kind == QLatin1String("list")) {
        QString title = QStringLiteral("List");
        for (const QJsonValue &value : m_lists) {
            if (value.toObject().value(QStringLiteral("id")).toString() == page.arg)
                title = value.toObject().value(QStringLiteral("title")).toString();
        }
        beginPage(title, QStringLiteral("List timeline"));
        auto *members = new QPushButton(QStringLiteral("Members"));
        members->setObjectName(QStringLiteral("ghostButton"));
        const QString id = page.arg;
        connect(members, &QPushButton::clicked, this, [this, id] { openPage({QStringLiteral("list-members"), id, QString()}); });
        addHeaderAction(members);
        m_cardContext->filterContext = QStringLiteral("home");
        loadStatuses(QStringLiteral("/api/v1/timelines/list/") + page.arg, QUrlQuery(QStringLiteral("limit=30")),
                     QStringLiteral("home"), QStringLiteral("No posts from this list's members yet."));
    } else if (kind == QLatin1String("notifications")) {
        showNotifications();
    } else if (kind == QLatin1String("explore")) {
        showExplore();
    } else if (kind == QLatin1String("conversations")) {
        showConversations();
    } else if (kind == QLatin1String("thread")) {
        showThread();
    } else if (kind == QLatin1String("account")) {
        showAccount();
    } else if (kind == QLatin1String("connections")) {
        showConnections();
    } else if (kind == QLatin1String("follows")) {
        showFollows();
    } else if (kind == QLatin1String("moderation")) {
        showModeration();
    } else if (kind == QLatin1String("lists")) {
        showLists();
    } else if (kind == QLatin1String("list-members")) {
        showListMembers();
    } else if (kind == QLatin1String("hashtags")) {
        showHashtags();
    } else if (kind == QLatin1String("tag")) {
        showTag();
    } else if (kind == QLatin1String("profile")) {
        showProfileEditor();
    } else if (kind == QLatin1String("filters")) {
        showFilters();
    } else if (kind == QLatin1String("scheduled")) {
        showScheduled();
    } else if (kind == QLatin1String("preferences")) {
        showPreferences();
    } else if (kind == QLatin1String("search")) {
        showSearch();
    } else {
        m_page = {QStringLiteral("home"), QString(), QString()};
        showHome();
    }
}

void MastodonFeature::goBack()
{
    if (m_history.isEmpty())
        return;
    openPage(m_history.takeLast(), false);
}

void MastodonFeature::reload()
{
    if (m_self.isEmpty() && m_client->signedIn() && m_stack->currentWidget() == m_app) {
        const SavedAccount account = activeAccount();
        if (account.valid()) {
            activate(account);
            return;
        }
    }
    if (!m_page.kind.isEmpty())
        openPage(m_page, false);
}

void MastodonFeature::loadMore()
{
    if (!m_loadMore)
        return;
    const auto loader = m_loadMore;
    setLoadMore({});
    loader();
}

void MastodonFeature::beginPage(const QString &title, const QString &subtitle)
{
    m_title->setText(title);
    m_subtitle->setText(subtitle);
    m_subtitle->setVisible(!subtitle.isEmpty());
    m_backButton->setVisible(!m_history.isEmpty());
    while (QLayoutItem *item = m_headerActions->takeAt(0)) {
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_tabBar->hide();
    clearList();
    setNotice(QString());
    setLoadMore({});
    setLoading(false);
}

void MastodonFeature::setTabs(const QList<QPair<QString, QString>> &tabs)
{
    while (QLayoutItem *item = m_tabLayout->takeAt(0)) {
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    for (const auto &tab : tabs) {
        auto *button = new QPushButton(QString(tab.second).replace(QLatin1Char('&'), QStringLiteral("&&")));
        button->setObjectName(QStringLiteral("repoTab"));
        button->setCheckable(true);
        button->setChecked(tab.first == m_page.tab);
        button->setCursor(Qt::PointingHandCursor);
        const QString value = tab.first;
        connect(button, &QPushButton::clicked, this, [this, value] {
            Page page = m_page;
            page.tab = value;
            openPage(page, false);
        });
        m_tabLayout->addWidget(button);
    }
    m_tabLayout->addStretch(1);
    m_tabBar->setVisible(!tabs.isEmpty());
}

void MastodonFeature::addHeaderAction(QWidget *widget)
{
    m_headerActions->addWidget(widget);
}

void MastodonFeature::clearList()
{
    while (QLayoutItem *item = m_list->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        } else if (QLayout *layout = item->layout()) {
            while (QLayoutItem *child = layout->takeAt(0)) {
                if (QWidget *gone = child->widget()) {
                    gone->hide();
                    gone->deleteLater();
                }
                delete child;
            }
        }
        delete item;
    }
}

void MastodonFeature::addRow(QWidget *widget)
{
    m_list->addWidget(widget);
}

void MastodonFeature::addSection(const QString &title)
{
    auto *label = new QLabel(title);
    label->setObjectName(QStringLiteral("sectionTitle"));
    label->setContentsMargins(16, 18, 16, 6);
    label->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 600;"));
    addRow(label);
}

void MastodonFeature::setEmpty(const QString &text)
{
    auto *label = mutedLabel(text, m_column);
    label->setObjectName(QStringLiteral("emptyStateBody"));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setContentsMargins(24, 40, 24, 40);
    label->setStyleSheet(label->styleSheet() + QStringLiteral(" font-size: 14px;"));
    addRow(label);
}

void MastodonFeature::setLoading(bool loading)
{
    m_loading->setVisible(loading);
    if (loading)
        m_loadMoreButton->hide();
    else
        m_loadMoreButton->setVisible(static_cast<bool>(m_loadMore));
}

void MastodonFeature::setNotice(const QString &text, bool error)
{
    m_notice->setText(text);
    m_notice->setStyleSheet(error ? QStringLiteral("color: #d1242f;") : QStringLiteral("color: %1;").arg(mutedColor(this).name()));
    m_notice->setVisible(!text.isEmpty());
}

void MastodonFeature::setLoadMore(std::function<void()> loader)
{
    m_loadMore = std::move(loader);
    m_loadMoreButton->setVisible(static_cast<bool>(m_loadMore) && !m_loading->isVisible());
}

void MastodonFeature::loadStatuses(const QString &path, const QUrlQuery &query, const QString &context, const QString &empty)
{
    const quint64 generation = m_generation;
    setLoading(true);
    m_client->get(path, query, [this, generation, context, empty](const Result &result) {
        if (current(generation))
            showStatuses(result, generation, context, empty, false);
    });
}

void MastodonFeature::showStatuses(const Result &result, quint64 generation, const QString &context,
                                   const QString &empty, bool append)
{
    setLoading(false);
    if (!result.ok) {
        setNotice(result.error, true);
        return;
    }
    const QJsonArray statuses = result.array();
    if (statuses.isEmpty() && !append) {
        setEmpty(empty);
        return;
    }
    CardContextPtr context2 = cardContext();
    for (const QJsonValue &value : statuses) {
        const QJsonObject status = value.toObject();
        bool hide = false;
        filterHideReason(shownStatus(status), context, &hide);
        if (hide)
            continue;
        addRow(new PostCard(status, context2));
        if (m_seenStatuses.size() >= kMaxSeenStatuses)
            m_seenStatuses.clear();
        m_seenStatuses.insert(shownStatus(status).value(QStringLiteral("id")).toString(), shownStatus(status));
    }
    if (result.next.isValid() && !statuses.isEmpty()) {
        const QUrl next = result.next;
        setLoadMore([this, next, context, empty] {
            const quint64 page = m_generation;
            setLoading(true);
            m_client->getUrl(next, [this, page, context, empty](const Result &more) {
                if (current(page))
                    showStatuses(more, page, context, empty, true);
            });
        });
    }
    Q_UNUSED(generation);
}

void MastodonFeature::loadAccountList(const QString &path, const QUrlQuery &query, const QString &empty, const QString &mode)
{
    const quint64 generation = m_generation;
    setLoading(true);
    m_client->get(path, query, [this, generation, empty, mode](const Result &result) {
        if (current(generation))
            showAccounts(result, generation, empty, mode, false);
    });
}

void MastodonFeature::showAccounts(const Result &result, quint64 generation, const QString &empty, const QString &mode,
                                   bool append)
{
    setLoading(false);
    if (!result.ok) {
        setNotice(result.error, true);
        return;
    }
    QJsonArray accounts = result.array();
    if (accounts.isEmpty() && !append) {
        setEmpty(empty);
        return;
    }
    QList<AccountRow *> rows;
    QStringList ids;
    for (const QJsonValue &value : accounts) {
        QJsonObject account = value.toObject();
        if (account.contains(QStringLiteral("account")) && account.value(QStringLiteral("account")).isObject())
            account = account.value(QStringLiteral("account")).toObject();
        auto *row = new AccountRow(account, cardContext());
        addRow(row);
        rows.append(row);
        ids.append(account.value(QStringLiteral("id")).toString());
    }
    const bool needsRelationships = mode == QLatin1String("followers") || mode == QLatin1String("plain") ||
                                    mode == QLatin1String("suggestions");
    if (needsRelationships) {
        QList<QPointer<AccountRow>> guarded;
        for (AccountRow *row : rows)
            guarded.append(row);
        fetchRelationships(ids, [this, guarded, mode, generation](const QHash<QString, QJsonObject> &relationships) {
            if (!current(generation))
                return;
            for (const QPointer<AccountRow> &row : guarded) {
                if (row)
                    decorateAccount(row, mode, relationships.value(row->account().value(QStringLiteral("id")).toString()));
            }
        });
    } else {
        for (AccountRow *row : rows)
            decorateAccount(row, mode, {});
    }
    if (result.next.isValid() && !accounts.isEmpty()) {
        const QUrl next = result.next;
        setLoadMore([this, next, empty, mode] {
            const quint64 page = m_generation;
            setLoading(true);
            m_client->getUrl(next, [this, page, empty, mode](const Result &more) {
                if (current(page))
                    showAccounts(more, page, empty, mode, true);
            });
        });
    }
}

void MastodonFeature::fetchRelationships(const QStringList &ids,
                                         std::function<void(const QHash<QString, QJsonObject> &)> done)
{
    if (ids.isEmpty()) {
        done({});
        return;
    }
    QUrlQuery query;
    for (const QString &id : ids)
        query.addQueryItem(QStringLiteral("id[]"), id);
    query.addQueryItem(QStringLiteral("with_suspended"), QStringLiteral("true"));
    m_client->get(QStringLiteral("/api/v1/accounts/relationships"), query, [done](const Result &result) {
        QHash<QString, QJsonObject> relationships;
        for (const QJsonValue &value : result.array())
            relationships.insert(value.toObject().value(QStringLiteral("id")).toString(), value.toObject());
        done(relationships);
    });
}

QPushButton *MastodonFeature::followButton(const QJsonObject &account, const QJsonObject &relationship)
{
    auto *button = new QPushButton;
    button->setCursor(Qt::PointingHandCursor);
    QPointer<QPushButton> guard(button);
    const auto paint = [guard](const QJsonObject &state) {
        if (!guard)
            return;
        QString text = QStringLiteral("Follow");
        bool primary = true;
        if (state.value(QStringLiteral("following")).toBool()) {
            text = QStringLiteral("Unfollow");
            primary = false;
        } else if (state.value(QStringLiteral("requested")).toBool()) {
            text = QStringLiteral("Requested");
            primary = false;
        } else if (state.value(QStringLiteral("followed_by")).toBool()) {
            text = QStringLiteral("Follow back");
        } else if (state.value(QStringLiteral("blocking")).toBool()) {
            text = QStringLiteral("Blocked");
            primary = false;
        }
        guard->setText(text);
        guard->setObjectName(primary ? QStringLiteral("primaryButton") : QStringLiteral("ghostButton"));
        guard->setProperty("relationship", state);
        guard->style()->unpolish(guard);
        guard->style()->polish(guard);
    };
    paint(relationship);
    const QString id = account.value(QStringLiteral("id")).toString();
    if (id == m_self.value(QStringLiteral("id")).toString())
        button->hide();
    connect(button, &QPushButton::clicked, this, [this, guard, paint, id, account] {
        if (!guard)
            return;
        const QJsonObject state = guard->property("relationship").toJsonObject();
        if (state.value(QStringLiteral("blocking")).toBool())
            return;
        const bool unfollow = state.value(QStringLiteral("following")).toBool() || state.value(QStringLiteral("requested")).toBool();
        if (unfollow && !confirm(QStringLiteral("Unfollow"), QStringLiteral("Unfollow %1?").arg(displayName(account))))
            return;
        guard->setEnabled(false);
        m_client->post(QStringLiteral("/api/v1/accounts/%1/%2").arg(id, unfollow ? QStringLiteral("unfollow") : QStringLiteral("follow")),
                       [this, guard, paint](const Result &result) {
                           if (!guard)
                               return;
                           guard->setEnabled(true);
                           if (!result.ok) {
                               setNotice(result.error, true);
                               return;
                           }
                           paint(result.object());
                       });
    });
    return button;
}

void MastodonFeature::decorateAccount(AccountRow *row, const QString &mode, const QJsonObject &relationship)
{
    const QJsonObject account = row->account();
    const QString id = account.value(QStringLiteral("id")).toString();
    QPointer<AccountRow> guard(row);
    const auto simple = [this, guard, id](const QString &label, const QString &path, const QJsonObject &body,
                                          const QString &done, bool danger, const QString &question) {
        auto *button = new QPushButton(label);
        button->setObjectName(danger ? QStringLiteral("dangerButton") : QStringLiteral("ghostButton"));
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this, [this, guard, path, body, done, question, button] {
            if (!question.isEmpty() && !confirm(QStringLiteral("Confirm"), question))
                return;
            button->setEnabled(false);
            m_client->send("POST", path, body, [this, guard, done](const Result &result) {
                if (!result.ok) {
                    setNotice(result.error, true);
                    if (guard)
                        guard->setEnabled(true);
                    return;
                }
                setNotice(done);
                if (guard)
                    guard->hide();
            });
        });
        Q_UNUSED(id);
        return button;
    };
    const QString name = displayName(account);
    if (mode == QLatin1String("following")) {
        row->addTrailing(simple(QStringLiteral("Unfollow"), QStringLiteral("/api/v1/accounts/%1/unfollow").arg(id), {},
                                QStringLiteral("Unfollowed %1.").arg(name), false, QStringLiteral("Unfollow %1?").arg(name)));
    } else if (mode == QLatin1String("followers")) {
        if (!relationship.isEmpty() && !relationship.value(QStringLiteral("following")).toBool())
            row->addTrailing(followButton(account, relationship));
        row->addTrailing(simple(QStringLiteral("Remove"), QStringLiteral("/api/v1/accounts/%1/remove_from_followers").arg(id), {},
                                QStringLiteral("%1 no longer follows you.").arg(name), false,
                                QStringLiteral("Remove %1 from your followers? They are not told.").arg(name)));
    } else if (mode == QLatin1String("requests")) {
        auto *accept = simple(QStringLiteral("Accept"), QStringLiteral("/api/v1/follow_requests/%1/authorize").arg(id), {},
                              QStringLiteral("%1 now follows you.").arg(name), false, QString());
        accept->setObjectName(QStringLiteral("primaryButton"));
        row->addTrailing(accept);
        row->addTrailing(simple(QStringLiteral("Reject"), QStringLiteral("/api/v1/follow_requests/%1/reject").arg(id), {},
                                QStringLiteral("Rejected %1's request.").arg(name), false, QString()));
    } else if (mode == QLatin1String("blocked")) {
        row->addTrailing(simple(QStringLiteral("Unblock"), QStringLiteral("/api/v1/accounts/%1/unblock").arg(id), {},
                                QStringLiteral("Unblocked %1.").arg(name), false, QString()));
    } else if (mode == QLatin1String("muted")) {
        row->addTrailing(simple(QStringLiteral("Unmute"), QStringLiteral("/api/v1/accounts/%1/unmute").arg(id), {},
                                QStringLiteral("Unmuted %1.").arg(name), false, QString()));
    } else if (mode == QLatin1String("endorsed")) {
        row->addTrailing(simple(QStringLiteral("Stop featuring"), QStringLiteral("/api/v1/accounts/%1/unpin").arg(id), {},
                                QStringLiteral("%1 is no longer featured on your profile.").arg(name), false, QString()));
    } else if (mode.startsWith(QLatin1String("members:"))) {
        const QString listId = mode.mid(8);
        auto *remove = new QPushButton(QStringLiteral("Remove"));
        remove->setObjectName(QStringLiteral("ghostButton"));
        connect(remove, &QPushButton::clicked, this, [this, listId, id, guard] {
            listMember(listId, id, false);
            if (guard)
                guard->hide();
        });
        row->addTrailing(remove);
    } else if (mode.startsWith(QLatin1String("add:"))) {
        const QString listId = mode.mid(4);
        auto *add = new QPushButton(QStringLiteral("Add"));
        add->setObjectName(QStringLiteral("primaryButton"));
        connect(add, &QPushButton::clicked, this, [this, listId, id, add] {
            listMember(listId, id, true);
            add->setEnabled(false);
            add->setText(QStringLiteral("Added"));
        });
        row->addTrailing(add);
    } else if (mode == QLatin1String("suggestions")) {
        row->addTrailing(followButton(account, relationship));
        auto *dismiss = iconButton(QStringLiteral("x"), QStringLiteral("Don't suggest again"), row);
        connect(dismiss, &QToolButton::clicked, this, [this, id, guard] {
            m_client->remove(QStringLiteral("/api/v1/suggestions/") + id, {});
            if (guard)
                guard->hide();
        });
        row->addTrailing(dismiss);
    } else if (mode == QLatin1String("plain")) {
        row->addTrailing(followButton(account, relationship));
    }
}

void MastodonFeature::run(const QByteArray &method, const QString &path, const QJsonObject &body, const QString &done,
                          std::function<void(const Result &)> after)
{
    m_client->send(method, path, body, [this, done, after](const Result &result) {
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        if (!done.isEmpty())
            setNotice(done);
        if (after)
            after(result);
    });
}

void MastodonFeature::updateCards(const QJsonObject &status)
{
    const QString id = status.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return;
    for (PostCard *card : postCards()) {
        if (card->shownId() == id)
            card->setStatus(status);
    }
    if (m_seenStatuses.contains(id))
        m_seenStatuses.insert(id, status);
}

void MastodonFeature::removeCards(const QString &id)
{
    for (PostCard *card : postCards()) {
        if (card->shownId() == id) {
            card->hide();
            card->deleteLater();
        }
    }
    m_seenStatuses.remove(id);
}

void MastodonFeature::act(const QJsonObject &status, const QString &action)
{
    const QString id = status.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return;
    m_client->post(QStringLiteral("/api/v1/statuses/%1/%2").arg(id, action), [this, action](const Result &result) {
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        QJsonObject updated = result.object();
        if (action == QLatin1String("reblog") && updated.value(QStringLiteral("reblog")).isObject())
            updated = updated.value(QStringLiteral("reblog")).toObject();
        updateCards(updated);
        static const QHash<QString, QString> messages{
            {QStringLiteral("pin"), QStringLiteral("Pinned to your profile.")},
            {QStringLiteral("unpin"), QStringLiteral("Unpinned from your profile.")},
            {QStringLiteral("mute"), QStringLiteral("You will no longer be notified about this conversation.")},
            {QStringLiteral("unmute"), QStringLiteral("Notifications for this conversation are back on.")},
        };
        if (messages.contains(action))
            setNotice(messages.value(action));
    });
}

void MastodonFeature::vote(const QJsonObject &poll, const QList<int> &choices)
{
    const QString pollId = poll.value(QStringLiteral("id")).toString();
    QJsonArray picked;
    for (int choice : choices)
        picked.append(choice);
    run("POST", QStringLiteral("/api/v1/polls/%1/votes").arg(pollId), {{QStringLiteral("choices"), picked}},
        QStringLiteral("Vote counted."), [this, pollId](const Result &result) {
            const QJsonObject updated = result.object();
            for (PostCard *card : postCards()) {
                QJsonObject shown = card->shown();
                if (shown.value(QStringLiteral("poll")).toObject().value(QStringLiteral("id")).toString() != pollId)
                    continue;
                shown.insert(QStringLiteral("poll"), updated);
                card->setStatus(shown);
            }
        });
}

void MastodonFeature::deleteStatus(const QJsonObject &status, bool redraft)
{
    const QString id = status.value(QStringLiteral("id")).toString();
    m_client->send("DELETE", QStringLiteral("/api/v1/statuses/") + id + (redraft ? QStringLiteral("?delete_media=false") : QString()),
                   {}, [this, id, redraft, status](const Result &result) {
                       if (!result.ok) {
                           setNotice(result.error, true);
                           return;
                       }
                       removeCards(id);
                       setNotice(redraft ? QStringLiteral("Deleted. Edit the draft and post it again.") : QStringLiteral("Post deleted."));
                       if (!redraft)
                           return;
                       const QJsonObject source = result.object();
                       ComposeDialog *dialog = compose({}, source.value(QStringLiteral("text")).toString(),
                                                       status.value(QStringLiteral("visibility")).toString());
                       dialog->setSpoiler(source.value(QStringLiteral("spoiler_text")).toString());
                       for (const QJsonValue &media : status.value(QStringLiteral("media_attachments")).toArray())
                           dialog->addExisting(media.toObject());
                       const QJsonObject poll = status.value(QStringLiteral("poll")).toObject();
                       if (!poll.isEmpty()) {
                           QStringList options;
                           for (const QJsonValue &option : poll.value(QStringLiteral("options")).toArray())
                               options << option.toObject().value(QStringLiteral("title")).toString();
                           dialog->setPoll(options, 86400, poll.value(QStringLiteral("multiple")).toBool());
                       }
                   });
}

ComposeDialog *MastodonFeature::compose(const QJsonObject &replyTo, const QString &text, const QString &visibility)
{
    auto *dialog = new ComposeDialog(m_client.get(), m_limits, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const QJsonObject source = m_self.value(QStringLiteral("source")).toObject();
    dialog->setDefaults(source.value(QStringLiteral("privacy")).toString(QStringLiteral("public")),
                        source.value(QStringLiteral("language")).toString(), source.value(QStringLiteral("sensitive")).toBool());
    if (!replyTo.isEmpty())
        dialog->setReplyTo(replyTo, m_self.value(QStringLiteral("acct")).toString());
    if (!text.isEmpty())
        dialog->setText(text);
    if (!visibility.isEmpty())
        dialog->setVisibility(visibility);
    dialog->published = [this](const QJsonObject &status, bool scheduled) {
        if (scheduled) {
            setNotice(QStringLiteral("Post scheduled. Find it under Scheduled posts."));
            if (m_page.kind == QLatin1String("scheduled"))
                reload();
            return;
        }
        setNotice(QStringLiteral("Posted."));
        if (m_page.kind == QLatin1String("account") && m_page.arg == m_self.value(QStringLiteral("id")).toString()) {
            reload();
        } else if (m_page.kind == QLatin1String("home")) {
            for (QLabel *empty : m_column->findChildren<QLabel *>(QStringLiteral("emptyStateBody")))
                empty->hide();
            QLayoutItem *first = m_list->count() > 0 ? m_list->itemAt(0) : nullptr;
            const bool prompt = first && first->widget() &&
                                first->widget()->objectName() == QLatin1String("mastodonComposePrompt");
            m_list->insertWidget(prompt ? 1 : 0, new PostCard(status, cardContext()));
        } else if (m_page.kind == QLatin1String("thread")) {
            reload();
        }
    };
    dialog->open();
    return dialog;
}

void MastodonFeature::editStatus(const QJsonObject &status)
{
    const QString id = status.value(QStringLiteral("id")).toString();
    m_client->get(QStringLiteral("/api/v1/statuses/%1/source").arg(id), {}, [this, status](const Result &result) {
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        auto *dialog = new ComposeDialog(m_client.get(), m_limits, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setEditing(status, result.object().value(QStringLiteral("text")).toString(),
                           result.object().value(QStringLiteral("spoiler_text")).toString());
        dialog->published = [this](const QJsonObject &updated, bool) {
            updateCards(updated);
            setNotice(QStringLiteral("Post updated."));
        };
        dialog->open();
    });
}

void MastodonFeature::follow(const QString &accountId, bool on, const QJsonObject &options)
{
    run("POST", QStringLiteral("/api/v1/accounts/%1/%2").arg(accountId, on ? QStringLiteral("follow") : QStringLiteral("unfollow")),
        options, QString(), [this, accountId, on](const Result &result) {
            const QJsonObject relationship = result.object();
            setNotice(!on ? QStringLiteral("Unfollowed.")
                          : relationship.value(QStringLiteral("requested")).toBool() ? QStringLiteral("Follow request sent.")
                                                                                     : QStringLiteral("Following."));
            if (m_page.kind == QLatin1String("account") && m_page.arg == accountId)
                reload();
        });
}

void MastodonFeature::relationshipAction(const QString &accountId, const QString &action, const QJsonObject &body)
{
    static const QHash<QString, QString> messages{
        {QStringLiteral("block"), QStringLiteral("Blocked.")},
        {QStringLiteral("unblock"), QStringLiteral("Unblocked.")},
        {QStringLiteral("mute"), QStringLiteral("Muted.")},
        {QStringLiteral("unmute"), QStringLiteral("Unmuted.")},
        {QStringLiteral("remove_from_followers"), QStringLiteral("Removed from your followers.")},
        {QStringLiteral("pin"), QStringLiteral("Featured on your profile.")},
        {QStringLiteral("unpin"), QStringLiteral("No longer featured on your profile.")},
        {QStringLiteral("note"), QStringLiteral("Personal note saved.")},
    };
    run("POST", QStringLiteral("/api/v1/accounts/%1/%2").arg(accountId, action), body, messages.value(action),
        [this, accountId](const Result &) {
            if (m_page.kind == QLatin1String("account") && m_page.arg == accountId)
                reload();
        });
}

void MastodonFeature::blockDomain(const QString &domain, bool on)
{
    const QString clean = domain.trimmed().toLower();
    if (clean.isEmpty())
        return;
    run(on ? QByteArray("POST") : QByteArray("DELETE"), QStringLiteral("/api/v1/domain_blocks"),
        {{QStringLiteral("domain"), clean}},
        on ? QStringLiteral("Blocked everything from %1.").arg(clean) : QStringLiteral("Unblocked %1.").arg(clean),
        [this](const Result &) {
            if (m_page.kind == QLatin1String("moderation"))
                reload();
        });
}

void MastodonFeature::saveProfile(const QJsonObject &fields, const QString &avatarPath, const QString &headerPath)
{
    QList<UploadPart> parts;
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        const QJsonValue value = it.value();
        const QByteArray data = value.isBool() ? QByteArray(value.toBool() ? "true" : "false") : value.toString().toUtf8();
        parts.append({it.key(), data, QString(), QString()});
    }
    for (const auto &file : {qMakePair(QStringLiteral("avatar"), avatarPath), qMakePair(QStringLiteral("header"), headerPath)}) {
        if (file.second.isEmpty())
            continue;
        QFile handle(file.second);
        if (!handle.open(QIODevice::ReadOnly)) {
            setNotice(QStringLiteral("Could not read %1.").arg(file.second), true);
            return;
        }
        parts.append({file.first, handle.readAll(), QFileInfo(file.second).fileName(), QString()});
    }
    setNotice(QStringLiteral("Saving profile…"));
    m_client->upload("PATCH", QStringLiteral("/api/v1/accounts/update_credentials"), parts, [this](const Result &result) {
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        m_self = result.object();
        for (SavedAccount &saved : m_accounts) {
            if (saved.key() == m_activeKey) {
                saved.displayName = displayName(m_self);
                saved.avatar = m_self.value(QStringLiteral("avatar_static")).toString();
            }
        }
        saveAccounts();
        m_cardContext.reset();
        refreshAccountHeader();
        if (m_page.kind == QLatin1String("profile"))
            reload();
        setNotice(QStringLiteral("Profile saved."));
    });
}

void MastodonFeature::savePostingDefaults(const QString &visibility, bool sensitive, const QString &language)
{
    QJsonObject source{{QStringLiteral("privacy"), visibility}, {QStringLiteral("sensitive"), sensitive}};
    if (!language.isEmpty())
        source.insert(QStringLiteral("language"), language);
    run("PATCH", QStringLiteral("/api/v1/accounts/update_credentials"), {{QStringLiteral("source"), source}},
        QStringLiteral("Posting defaults saved."), [this](const Result &result) { m_self = result.object(); });
}

void MastodonFeature::createList(const QString &title)
{
    if (title.trimmed().isEmpty())
        return;
    run("POST", QStringLiteral("/api/v1/lists"), {{QStringLiteral("title"), title.trimmed()}},
        QStringLiteral("List created."), [this](const Result &) {
            loadLists();
            if (m_page.kind == QLatin1String("lists"))
                reload();
        });
}

void MastodonFeature::updateList(const QString &id, const QJsonObject &body)
{
    run("PUT", QStringLiteral("/api/v1/lists/") + id, body, QStringLiteral("List saved."), [this](const Result &) {
        loadLists();
        if (m_page.kind == QLatin1String("lists"))
            reload();
    });
}

void MastodonFeature::deleteList(const QString &id)
{
    run("DELETE", QStringLiteral("/api/v1/lists/") + id, {}, QStringLiteral("List deleted."), [this](const Result &) {
        loadLists();
        if (m_page.kind == QLatin1String("lists"))
            reload();
    });
}

void MastodonFeature::listMember(const QString &listId, const QString &accountId, bool add)
{
    run(add ? QByteArray("POST") : QByteArray("DELETE"), QStringLiteral("/api/v1/lists/%1/accounts").arg(listId),
        {{QStringLiteral("account_ids"), QJsonArray{accountId}}},
        add ? QStringLiteral("Added to the list.") : QStringLiteral("Removed from the list."));
}

void MastodonFeature::followTag(const QString &name, bool on)
{
    QString tag = name.trimmed();
    if (tag.startsWith(QLatin1Char('#')))
        tag.remove(0, 1);
    if (tag.isEmpty())
        return;
    run("POST", QStringLiteral("/api/v1/tags/%1/%2").arg(QString::fromUtf8(QUrl::toPercentEncoding(tag)),
                                                          on ? QStringLiteral("follow") : QStringLiteral("unfollow")),
        {}, on ? QStringLiteral("Following #%1.").arg(tag) : QStringLiteral("Unfollowed #%1.").arg(tag), [this](const Result &) {
            if (m_page.kind == QLatin1String("hashtags") || m_page.kind == QLatin1String("tag"))
                reload();
        });
}

void MastodonFeature::featureTag(const QString &name, const QString &featuredId)
{
    QString tag = name.trimmed();
    if (tag.startsWith(QLatin1Char('#')))
        tag.remove(0, 1);
    if (featuredId.isEmpty() && tag.isEmpty())
        return;
    const auto after = [this](const Result &) {
        if (m_page.kind == QLatin1String("profile") || m_page.kind == QLatin1String("tag") || m_page.kind == QLatin1String("hashtags"))
            reload();
    };
    if (featuredId.isEmpty())
        run("POST", QStringLiteral("/api/v1/featured_tags"), {{QStringLiteral("name"), tag}},
            QStringLiteral("#%1 is featured on your profile.").arg(tag), after);
    else
        run("DELETE", QStringLiteral("/api/v1/featured_tags/") + featuredId, {},
            QStringLiteral("Hashtag removed from your profile."), after);
}

void MastodonFeature::saveFilter(const QString &id, const QJsonObject &body)
{
    run(id.isEmpty() ? QByteArray("POST") : QByteArray("PUT"),
        id.isEmpty() ? QStringLiteral("/api/v2/filters") : QStringLiteral("/api/v2/filters/") + id, body,
        QStringLiteral("Filter saved."), [this](const Result &) {
            if (m_page.kind == QLatin1String("filters"))
                reload();
        });
}

void MastodonFeature::deleteFilter(const QString &id)
{
    run("DELETE", QStringLiteral("/api/v2/filters/") + id, {}, QStringLiteral("Filter deleted."), [this](const Result &) {
        if (m_page.kind == QLatin1String("filters"))
            reload();
    });
}

void MastodonFeature::cancelScheduled(const QString &id)
{
    run("DELETE", QStringLiteral("/api/v1/scheduled_statuses/") + id, {}, QStringLiteral("Scheduled post cancelled."),
        [this](const Result &) {
            if (m_page.kind == QLatin1String("scheduled"))
                reload();
        });
}

void MastodonFeature::reschedule(const QString &id, const QDateTime &when)
{
    run("PUT", QStringLiteral("/api/v1/scheduled_statuses/") + id,
        {{QStringLiteral("scheduled_at"), when.toUTC().toString(Qt::ISODate)}}, QStringLiteral("Rescheduled."),
        [this](const Result &) {
            if (m_page.kind == QLatin1String("scheduled"))
                reload();
        });
}

void MastodonFeature::answerFollowRequest(const QString &accountId, bool accept)
{
    run("POST", QStringLiteral("/api/v1/follow_requests/%1/%2").arg(accountId, accept ? QStringLiteral("authorize") : QStringLiteral("reject")),
        {}, accept ? QStringLiteral("Request accepted.") : QStringLiteral("Request rejected."), [this](const Result &) {
            loadSidebarCounts();
            if (m_page.kind == QLatin1String("notifications") || m_page.kind == QLatin1String("follows"))
                reload();
        });
}

void MastodonFeature::dismissNotification(const QString &id)
{
    run("POST", QStringLiteral("/api/v1/notifications/%1/dismiss").arg(id), {}, QString());
}

void MastodonFeature::clearNotifications()
{
    run("POST", QStringLiteral("/api/v1/notifications/clear"), {}, QStringLiteral("Notifications cleared."),
        [this](const Result &) {
            setUnread(0);
            if (m_page.kind == QLatin1String("notifications"))
                reload();
        });
}

void MastodonFeature::report(const QJsonObject &body)
{
    run("POST", QStringLiteral("/api/v1/reports"), body, QStringLiteral("Report sent to your server's moderators."));
}

void MastodonFeature::savePersonalNote(const QString &accountId, const QString &note)
{
    relationshipAction(accountId, QStringLiteral("note"), {{QStringLiteral("comment"), note}});
}

void MastodonFeature::openExternal(const QUrl &url)
{
    if (m_openExternal && url.isValid())
        m_openExternal(url);
}

bool MastodonFeature::confirm(const QString &title, const QString &text)
{
    return !m_confirm || m_confirm(title, text);
}

void MastodonFeature::openOnInstance(const QString &path)
{
    openExternal(QUrl(m_client->instance() + path));
}

void MastodonFeature::openLink(const QString &link)
{
    const QUrl url(link);
    if (url.scheme() == kTagScheme) {
        openPage({QStringLiteral("tag"), url.path(), QString()});
    } else if (url.scheme() == kAccountScheme) {
        openAccountId(url.path());
    } else if (url.scheme() == QLatin1String("fm-thread")) {
        openPage({QStringLiteral("thread"), url.path(), QString()});
    } else if (safeWebUrl(url)) {
        openExternal(url);
    }
}

void MastodonFeature::openAccountObject(const QJsonObject &account)
{
    openAccountId(account.value(QStringLiteral("id")).toString());
}

void MastodonFeature::openAccountId(const QString &id)
{
    if (!id.isEmpty())
        openPage({QStringLiteral("account"), id, QString()});
}

void MastodonFeature::openMedia(const QJsonArray &media, int index)
{
    auto *viewer = new MediaViewer(m_client.get(), media, index, this);
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    viewer->open();
}

void MastodonFeature::translate(const QJsonObject &status)
{
    const QString id = status.value(QStringLiteral("id")).toString();
    setNotice(QStringLiteral("Translating…"));
    m_client->post(QStringLiteral("/api/v1/statuses/%1/translate").arg(id), [this, status](const Result &result) {
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        const QJsonObject translation = result.object();
        QJsonObject shown = status;
        shown.insert(QStringLiteral("content"), translation.value(QStringLiteral("content")).toString());
        if (!translation.value(QStringLiteral("spoiler_text")).toString().isEmpty())
            shown.insert(QStringLiteral("spoiler_text"), translation.value(QStringLiteral("spoiler_text")).toString());
        updateCards(shown);
        setNotice(QStringLiteral("Translated from %1 by %2.")
                      .arg(languageName(translation.value(QStringLiteral("detected_source_language")).toString()),
                           translation.value(QStringLiteral("provider")).toString(QStringLiteral("your server"))));
    });
}

void MastodonFeature::reportAccount(const QJsonObject &account, const QJsonObject &status)
{
    const bool remote = account.value(QStringLiteral("acct")).toString().contains(QLatin1Char('@'));
    auto *dialog = new ReportDialog(account, status, remote, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog] { report(dialog->body()); });
    dialog->open();
}

void MastodonFeature::editListMembership(const QJsonObject &account)
{
    const QString id = account.value(QStringLiteral("id")).toString();
    m_client->get(QStringLiteral("/api/v1/accounts/%1/lists").arg(id), {}, [this, id, account](const Result &result) {
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        if (m_lists.isEmpty()) {
            setNotice(QStringLiteral("Create a list first under Manage lists."));
            return;
        }
        QSet<QString> current;
        for (const QJsonValue &value : result.array())
            current.insert(value.toObject().value(QStringLiteral("id")).toString());
        QMenu menu(this);
        menu.setToolTipsVisible(true);
        for (const QJsonValue &value : std::as_const(m_lists)) {
            const QJsonObject list = value.toObject();
            const QString listId = list.value(QStringLiteral("id")).toString();
            QAction *action = menu.addAction(list.value(QStringLiteral("title")).toString());
            action->setCheckable(true);
            action->setChecked(current.contains(listId));
            connect(action, &QAction::toggled, this, [this, listId, id](bool on) { listMember(listId, id, on); });
        }
        Q_UNUSED(account);
        menu.exec(QCursor::pos());
    });
}

void MastodonFeature::openStatusMenu(const QJsonObject &status, QToolButton *anchor)
{
    const QJsonObject account = status.value(QStringLiteral("account")).toObject();
    const bool own = account.value(QStringLiteral("id")).toString() == m_self.value(QStringLiteral("id")).toString();
    const QString name = displayName(account);
    const QString acct = account.value(QStringLiteral("acct")).toString();
    QMenu menu(this);
    const QUrl url(status.value(QStringLiteral("url")).toString());
    menu.addAction(QStringLiteral("Open thread"), this, [this, status] {
        openPage({QStringLiteral("thread"), status.value(QStringLiteral("id")).toString(), QString()});
    });
    if (safeWebUrl(url)) {
        menu.addAction(QStringLiteral("Open in browser"), this, [this, url] { openExternal(url); });
        menu.addAction(QStringLiteral("Copy link"), this, [url] { QApplication::clipboard()->setText(url.toString()); });
    }
    const QString language = status.value(QStringLiteral("language")).toString();
    if (!language.isEmpty() && language != QLocale().name().section(QLatin1Char('_'), 0, 0))
        menu.addAction(QStringLiteral("Translate"), this, [this, status] { translate(status); });
    menu.addSeparator();
    if (own) {
        menu.addAction(QStringLiteral("Edit"), this, [this, status] { editStatus(status); });
        const bool pinned = status.value(QStringLiteral("pinned")).toBool();
        const QString visibility = status.value(QStringLiteral("visibility")).toString();
        if (visibility == QLatin1String("public") || visibility == QLatin1String("unlisted"))
            menu.addAction(pinned ? QStringLiteral("Unpin from profile") : QStringLiteral("Pin on profile"), this,
                           [this, status, pinned] { act(status, pinned ? QStringLiteral("unpin") : QStringLiteral("pin")); });
    }
    const bool muted = status.value(QStringLiteral("muted")).toBool();
    menu.addAction(muted ? QStringLiteral("Unmute conversation") : QStringLiteral("Mute conversation"), this,
                   [this, status, muted] { act(status, muted ? QStringLiteral("unmute") : QStringLiteral("mute")); });
    if (own) {
        menu.addSeparator();
        menu.addAction(QStringLiteral("Delete and redraft"), this, [this, status] {
            if (confirm(QStringLiteral("Delete and redraft"),
                        QStringLiteral("Delete this post and open it as a draft? Favourites and boosts are lost, and replies lose their parent.")))
                deleteStatus(status, true);
        });
        menu.addAction(QStringLiteral("Delete"), this, [this, status] {
            if (confirm(QStringLiteral("Delete post"), QStringLiteral("Delete this post? This cannot be undone.")))
                deleteStatus(status, false);
        });
    } else {
        menu.addAction(QStringLiteral("Mention %1").arg(name), this, [this, acct] { compose({}, QLatin1Char('@') + acct + QLatin1Char(' ')); });
        menu.addAction(QStringLiteral("Open %1's profile").arg(name), this, [this, account] { openAccountObject(account); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Mute %1").arg(name), this, [this, account, name] {
            if (confirm(QStringLiteral("Mute"), QStringLiteral("Mute %1? You will no longer see their posts.").arg(name)))
                relationshipAction(account.value(QStringLiteral("id")).toString(), QStringLiteral("mute"));
        });
        menu.addAction(QStringLiteral("Block %1").arg(name), this, [this, account, name] {
            if (confirm(QStringLiteral("Block"), QStringLiteral("Block %1? They will no longer be able to follow you or see your posts.").arg(name)))
                relationshipAction(account.value(QStringLiteral("id")).toString(), QStringLiteral("block"));
        });
        if (acct.contains(QLatin1Char('@'))) {
            const QString domain = acct.section(QLatin1Char('@'), 1);
            menu.addAction(QStringLiteral("Block the whole server %1").arg(domain), this, [this, domain] {
                if (confirm(QStringLiteral("Block server"),
                            QStringLiteral("Block everything from %1? You will lose followers from there.").arg(domain)))
                    blockDomain(domain, true);
            });
        }
        menu.addAction(QStringLiteral("Report %1…").arg(name), this, [this, account, status] { reportAccount(account, status); });
    }
    menu.exec(anchor ? anchor->mapToGlobal(QPoint(0, anchor->height())) : QCursor::pos());
}

void MastodonFeature::openAccountMenu(const QJsonObject &account, const QJsonObject &relationship, QWidget *anchor)
{
    const QString id = account.value(QStringLiteral("id")).toString();
    const QString name = displayName(account);
    const QString acct = account.value(QStringLiteral("acct")).toString();
    QMenu menu(this);
    menu.addAction(QStringLiteral("Mention %1").arg(name), this, [this, acct] { compose({}, QLatin1Char('@') + acct + QLatin1Char(' ')); });
    menu.addAction(QStringLiteral("Send a private mention"), this, [this, acct] {
        compose({}, QLatin1Char('@') + acct + QLatin1Char(' '), QStringLiteral("direct"));
    });
    const QUrl url(account.value(QStringLiteral("url")).toString());
    if (safeWebUrl(url)) {
        menu.addAction(QStringLiteral("Open original page"), this, [this, url] { openExternal(url); });
        menu.addAction(QStringLiteral("Copy link to profile"), this, [url] { QApplication::clipboard()->setText(url.toString()); });
    }
    menu.addSeparator();
    if (relationship.value(QStringLiteral("following")).toBool()) {
        const bool notifying = relationship.value(QStringLiteral("notifying")).toBool();
        menu.addAction(notifying ? QStringLiteral("Stop notifying me when %1 posts").arg(name)
                                 : QStringLiteral("Notify me when %1 posts").arg(name),
                       this, [this, id, notifying] { follow(id, true, {{QStringLiteral("notify"), !notifying}}); });
        const bool boosts = relationship.value(QStringLiteral("showing_reblogs")).toBool(true);
        menu.addAction(boosts ? QStringLiteral("Hide boosts from %1").arg(name) : QStringLiteral("Show boosts from %1").arg(name),
                       this, [this, id, boosts] { follow(id, true, {{QStringLiteral("reblogs"), !boosts}}); });
        const bool endorsed = relationship.value(QStringLiteral("endorsed")).toBool();
        menu.addAction(endorsed ? QStringLiteral("Stop featuring on profile") : QStringLiteral("Feature on profile"), this,
                       [this, id, endorsed] { relationshipAction(id, endorsed ? QStringLiteral("unpin") : QStringLiteral("pin")); });
        menu.addAction(QStringLiteral("Add to or remove from lists…"), this, [this, account] { editListMembership(account); });
    }
    menu.addAction(QStringLiteral("Personal note…"), this, [this, id, relationship, name] {
        bool ok = false;
        const QString note = QInputDialog::getMultiLineText(this, QStringLiteral("Personal note"),
                                                            QStringLiteral("A note about %1 only you can see:").arg(name),
                                                            relationship.value(QStringLiteral("note")).toString(), &ok);
        if (ok)
            savePersonalNote(id, note);
    });
    if (relationship.value(QStringLiteral("followed_by")).toBool())
        menu.addAction(QStringLiteral("Remove from your followers"), this, [this, id, name] {
            if (confirm(QStringLiteral("Remove follower"), QStringLiteral("Remove %1 from your followers?").arg(name)))
                relationshipAction(id, QStringLiteral("remove_from_followers"));
        });
    menu.addSeparator();
    const bool muting = relationship.value(QStringLiteral("muting")).toBool();
    menu.addAction(muting ? QStringLiteral("Unmute %1").arg(name) : QStringLiteral("Mute %1").arg(name), this, [this, id, muting] {
        relationshipAction(id, muting ? QStringLiteral("unmute") : QStringLiteral("mute"));
    });
    const bool blocking = relationship.value(QStringLiteral("blocking")).toBool();
    menu.addAction(blocking ? QStringLiteral("Unblock %1").arg(name) : QStringLiteral("Block %1").arg(name), this,
                   [this, id, blocking, name] {
                       if (blocking || confirm(QStringLiteral("Block"), QStringLiteral("Block %1?").arg(name)))
                           relationshipAction(id, blocking ? QStringLiteral("unblock") : QStringLiteral("block"));
                   });
    if (acct.contains(QLatin1Char('@'))) {
        const QString domain = acct.section(QLatin1Char('@'), 1);
        const bool domainBlocked = relationship.value(QStringLiteral("domain_blocking")).toBool();
        menu.addAction(domainBlocked ? QStringLiteral("Unblock server %1").arg(domain) : QStringLiteral("Block server %1").arg(domain),
                       this, [this, domain, domainBlocked] {
                           if (domainBlocked || confirm(QStringLiteral("Block server"), QStringLiteral("Block everything from %1?").arg(domain)))
                               blockDomain(domain, !domainBlocked);
                       });
    }
    menu.addAction(QStringLiteral("Report %1…").arg(name), this, [this, account] { reportAccount(account, {}); });
    menu.exec(anchor ? anchor->mapToGlobal(QPoint(0, anchor->height())) : QCursor::pos());
}

}

namespace {

const forkmesh::market::NativePackageRegistration mastodonRegistration({
    QStringLiteral("builtin:mastodon"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Mastodon"),
    QStringLiteral("mastodon"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::mastodon::MastodonFeature(host, parent);
    },
    QString(),
});

}
