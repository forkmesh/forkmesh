#include "MainWindow.h"

#include "MainWindowInternal.h"

#include <QDesktopServices>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

namespace {

const QUrl kPatreonJoinUrl(
    QStringLiteral("https://www.patreon.com/16434219/join"));
const QUrl kPatreonFreeUrl(QStringLiteral(
    "https://www.patreon.com/checkout/mdkaifansari04?rid=29122373"));
const QUrl kPatreonSupporterUrl(QStringLiteral(
    "https://www.patreon.com/checkout/mdkaifansari04?rid=29122413"));
const QUrl kPatreonProUrl(QStringLiteral(
    "https://www.patreon.com/checkout/mdkaifansari04?rid=29122568"));
const QUrl kPatreonBuilderUrl(QStringLiteral(
    "https://www.patreon.com/checkout/mdkaifansari04?rid=29122566"));
const QUrl kPatreonStudioUrl(QStringLiteral(
    "https://www.patreon.com/checkout/mdkaifansari04?rid=29260767"));

QNetworkRequest membershipRequest(const QUrl &url, const QString &token = {})
{
    QNetworkRequest request(url);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    request.setTransferTimeout(8000);
    if (!token.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("Authorization"),
                             QByteArrayLiteral("Bearer ") + token.toUtf8());
    }
    return request;
}

QJsonObject responseObject(QNetworkReply *reply)
{
    if (!reply || reply->error() != QNetworkReply::NoError)
        return {};
    const QByteArray body = reply->read(64 * 1024);
    return QJsonDocument::fromJson(body).object();
}

} // namespace

// Show the membership options the user just asked for. There is deliberately
// no dismissal cooldown and no "already a member" suppression here: both only
// ever made sense for a prompt that opened itself, and a current member still
// needs this panel to change or compare tiers.
void MainWindow::openMembershipOptions()
{
    if (!isVisible())
        return;
    if (QDialog *existing =
            findChild<QDialog *>(QStringLiteral("membershipSupportPrompt"))) {
        existing->raise();
        existing->activateWindow();
        return;
    }

    // Live tier counts are a nicety; the panel opens either way.
    if (!m_networkAccess) {
        showMembershipPrompt({});
        return;
    }
    QUrl activityUrl = catalogApiUrl();
    activityUrl.setPath(QStringLiteral("/api/chat/activity"));
    activityUrl.setQuery(QString());
    QNetworkReply *reply = m_networkAccess->get(membershipRequest(activityUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QJsonObject activity = responseObject(reply);
        reply->deleteLater();
        showMembershipPrompt(activity);
    });
}

void MainWindow::showMembershipPrompt(const QJsonObject &activity)
{
    if (!isVisible() ||
        findChild<QDialog *>(QStringLiteral("membershipSupportPrompt"))) {
        return;
    }

    const QJsonObject membership =
        activity.value(QStringLiteral("membership")).toObject();
    const int supporterCount =
        qMax(0, membership.value(QStringLiteral("supporterCount")).toInt());
    const int proCount =
        qMax(0, membership.value(QStringLiteral("proCount")).toInt());
    const int builderCount =
        qMax(0, membership.value(QStringLiteral("builderCount")).toInt());
    const int studioCount =
        qMax(0, membership.value(QStringLiteral("studioCount")).toInt());
    const int totalCount = qMax(
        supporterCount + proCount + builderCount + studioCount,
        membership.value(QStringLiteral("totalCount")).toInt());
    const int communityCount =
        qMax(0, activity.value(QStringLiteral("userCount")).toInt());

    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("membershipSupportPrompt"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Support ForkMesh development"));
    dialog->setWindowModality(Qt::NonModal);
    dialog->setModal(false);
    dialog->resize(1180, 720);
    dialog->setMinimumSize(760, 580);
    dialog->setStyleSheet(Theme::remapSheet(QStringLiteral(R"CSS(
        QDialog#membershipSupportPrompt { background: %1; color: %2; }
        QDialog#membershipSupportPrompt QLabel { background: transparent;
            border: none; color: %2; }
        QScrollArea, QScrollArea > QWidget > QWidget,
        QWidget#membershipPage { background: transparent; border: none; }
        QFrame#membershipPlan { background: %3; border: 1px solid %4;
            border-radius: 14px; }
        QFrame#membershipPlan[featured="true"] { border-color: %5; }
        QLabel#membershipKicker { color: %6; font-weight: 700;
            letter-spacing: 1px; }
        QLabel#membershipTitle { color: %7; font-size: 24px; font-weight: 800; }
        QLabel#membershipMuted { color: %8; }
        QLabel#membershipPrice { color: %9; font-weight: 800; }
        QProgressBar { min-height: 9px; max-height: 9px; border: 1px solid %10;
            border-radius: 5px; background: %11; text-align: center; }
        QProgressBar::chunk { border-radius: 4px; background: %6; }
        QPushButton { min-height: 30px; border: 1px solid %12;
            border-radius: 7px; background: %13; color: %2;
            padding: 0 9px; font-weight: 700; font-size: 10px; }
        QPushButton:hover { border-color: %5; }
        QPushButton#membershipPrimary { background: %14; color: %15;
            border-color: %14; }
    )CSS")
        .arg(Theme::nextOrHex(Theme::Role::Bg, QStringLiteral("#0b100d")),
             Theme::nextOrHex(Theme::Role::Fg, QStringLiteral("#f1fff5")),
             Theme::nextOrHex(Theme::Role::Surface, QStringLiteral("#111914")),
             Theme::nextOrHex(Theme::Role::Border, QStringLiteral("#33483a")),
             Theme::nextOrHex(Theme::Role::BorderStrong, QStringLiteral("#68e89b")),
             Theme::nextOrHex(Theme::Role::FgMuted, QStringLiteral("#68e89b")),
             Theme::nextOrHex(Theme::Role::Fg, QStringLiteral("#ffffff")),
             Theme::nextOrHex(Theme::Role::FgMuted, QStringLiteral("#a9b8ad")),
             Theme::nextOrHex(Theme::Role::Fg, QStringLiteral("#b7ffd1")),
             Theme::nextOrHex(Theme::Role::Border, QStringLiteral("#26352b")),
             Theme::nextOrHex(Theme::Role::SurfaceRaised, QStringLiteral("#050705")),
             Theme::nextOrHex(Theme::Role::Border, QStringLiteral("#3c4c40")),
             Theme::nextOrHex(Theme::Role::SurfaceRaised, QStringLiteral("#19221b")),
             Theme::nextOrHex(Theme::Role::Accent, QStringLiteral("#68e89b")),
             Theme::nextOrHex(Theme::Role::OnAccent, QStringLiteral("#06200f")))));

    auto *outer = new QVBoxLayout(dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("membershipPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 13, 16, 12);
    layout->setSpacing(6);

    auto *kicker = new QLabel(QStringLiteral("COMMUNITY-FUNDED INFRASTRUCTURE"));
    kicker->setObjectName(QStringLiteral("membershipKicker"));
    layout->addWidget(kicker);
    auto *title = new QLabel(QStringLiteral("Become a ForkMesh member."));
    title->setObjectName(QStringLiteral("membershipTitle"));
    layout->addWidget(title);
    auto *intro = new QLabel(QStringLiteral(
        "Memberships fund public relays, signed releases, security work and "
        "maintenance. Core stays free; members keep that promise sustainable."));
    intro->setObjectName(QStringLiteral("membershipMuted"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *counts = new QLabel(
        totalCount > 0
            ? QStringLiteral("%1 paying members  ·  %2 people in the community")
                  .arg(totalCount)
                  .arg(communityCount)
            : QStringLiteral("Join the founding members sustaining ForkMesh"));
    counts->setObjectName(QStringLiteral("membershipMuted"));
    layout->addWidget(counts);

    auto *progressLabel = new QLabel(
        totalCount > 0
            ? QStringLiteral("Founding 100  ·  %1 of 100 memberships").arg(totalCount)
            : QStringLiteral("Founding 100 membership drive"));
    progressLabel->setStyleSheet(Theme::remapSheet(QStringLiteral("font-weight:700;")));
    layout->addWidget(progressLabel);
    auto *progress = new QProgressBar;
    progress->setRange(0, 100);
    progress->setValue(qMin(100, totalCount));
    progress->setTextVisible(false);
    layout->addWidget(progress);

    auto plan = [dialog](const QString &name, const QString &price,
                         const QString &members, const QStringList &perks,
                         const QString &artPath, const QString &artDescription,
                         const QString &buttonText, const QUrl &checkoutUrl,
                         bool featured) {
        auto *card = new QFrame(dialog);
        card->setObjectName(QStringLiteral("membershipPlan"));
        card->setProperty("featured", featured);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(7, 7, 7, 8);
        cardLayout->setSpacing(4);

        auto *art = new QLabel(card);
        art->setAccessibleName(artDescription);
        art->setFixedHeight(92);
        art->setMinimumWidth(1);
        art->setScaledContents(true);
        QPixmap cover(artPath);
        if (!cover.isNull()) {
            const QSize target(400, 92);
            QPixmap crop = cover.scaled(target, Qt::KeepAspectRatioByExpanding,
                                        Qt::SmoothTransformation);
            const int x = qMax(0, (crop.width() - target.width()) / 2);
            const int y = qMax(0, (crop.height() - target.height()) / 2);
            art->setPixmap(crop.copy(x, y, qMin(target.width(), crop.width()),
                                     qMin(target.height(), crop.height())));
        }
        cardLayout->addWidget(art);

        auto *heading = new QLabel(QStringLiteral("<b>%1</b>").arg(name), card);
        heading->setTextFormat(Qt::RichText);
        heading->setStyleSheet(Theme::remapSheet(QStringLiteral("font-size:14px;")));
        cardLayout->addWidget(heading);
        auto *priceLabel = new QLabel(price, card);
        priceLabel->setObjectName(QStringLiteral("membershipPrice"));
        cardLayout->addWidget(priceLabel);
        auto *memberLabel = new QLabel(members, card);
        memberLabel->setObjectName(QStringLiteral("membershipMuted"));
        cardLayout->addWidget(memberLabel);
        auto *perkLabel = new QLabel(card);
        perkLabel->setWordWrap(true);
        perkLabel->setTextFormat(Qt::RichText);
        perkLabel->setStyleSheet(Theme::remapSheet(QStringLiteral("font-size:9px;")));
        QStringList lines;
        for (const QString &perk : perks)
            lines.append(QStringLiteral("<div style='margin:1px 0'>"
                                        "<span style='color:%2'>✓</span> %1</div>")
                             .arg(perk.toHtmlEscaped(),
                                  Theme::nextOrHex(Theme::Role::FgMuted,
                                                   QStringLiteral("#68e89b"))));
        perkLabel->setText(lines.join(QString()));
        cardLayout->addWidget(perkLabel, 1);
        auto *join = new QPushButton(buttonText, card);
        if (featured)
            join->setObjectName(QStringLiteral("membershipPrimary"));
        cardLayout->addWidget(join);
        QObject::connect(join, &QPushButton::clicked, dialog,
                         [dialog, checkoutUrl] {
            QDesktopServices::openUrl(checkoutUrl);
            dialog->close();
        });
        return card;
    };

    auto *freePlan = new QFrame(dialog);
    freePlan->setObjectName(QStringLiteral("membershipPlan"));
    auto *freeLayout = new QHBoxLayout(freePlan);
    freeLayout->setContentsMargins(9, 4, 9, 4);
    auto *freeText = new QLabel(
        QStringLiteral("<b>Free · $0 / month</b>  ·  Follow public updates; "
                       "ForkMesh Core remains free."), freePlan);
    freeText->setTextFormat(Qt::RichText);
    auto *freeJoin = new QPushButton(QStringLiteral("Join free ↗"), freePlan);
    freeLayout->addWidget(freeText);
    freeLayout->addStretch(1);
    freeLayout->addWidget(freeJoin);
    connect(freeJoin, &QPushButton::clicked, dialog, [dialog] {
        QDesktopServices::openUrl(kPatreonFreeUrl);
        dialog->close();
    });
    layout->addWidget(freePlan);

    auto *plans = new QGridLayout;
    plans->setSpacing(6);
    plans->addWidget(
        plan(QStringLiteral("Supporter"), QStringLiteral("$5 / month"),
             supporterCount == 1
                 ? QStringLiteral("1 active Supporter")
                 : QStringLiteral("%1 active Supporters").arg(supporterCount),
             {QStringLiteral("Supporter badge on your ForkMesh profile"),
              QStringLiteral("Unlock World outfit colors and cuts"),
              QStringLiteral("Supporters wall / public credits listing"),
              QStringLiteral("Help fund public relays, mirrors and releases")},
             QStringLiteral(":/app/membership/patreon-supporter.png"),
             QStringLiteral("Supporter artwork from the ForkMesh Patreon campaign"),
             QStringLiteral("Choose Supporter ↗"), kPatreonSupporterUrl,
             false),
        0, 0);
    plans->addWidget(
        plan(QStringLiteral("Pro"), QStringLiteral("$10 / month"),
             proCount == 1 ? QStringLiteral("1 active Pro member")
                           : QStringLiteral("%1 active Pro members").arg(proCount),
             {QStringLiteral("Everything in Supporter"),
              QStringLiteral("Pro badge on your ForkMesh profile"),
              QStringLiteral("Hosted issue-to-agent coding workflows"),
              QStringLiteral("Automatic AI PR reviews and quick fixes"),
              QStringLiteral("AI conflict resolution and review loops"),
              QStringLiteral("Parallel agents with resumable sessions"),
              QStringLiteral("Mobile editor, terminal, files and dev tools"),
              QStringLiteral("Everything in Free Core (Git hosting, signed collaboration, mirrors, encrypted rooms)")},
             QStringLiteral(":/app/membership/patreon-pro.png"),
             QStringLiteral("Pro artwork from the ForkMesh Patreon campaign"),
             QStringLiteral("Choose Pro ↗"), kPatreonProUrl,
             true),
        0, 1);
    plans->addWidget(
        plan(QStringLiteral("Builder"), QStringLiteral("$50 / month"),
             builderCount == 1
                 ? QStringLiteral("1 active Builder")
                 : QStringLiteral("%1 active Builders").arg(builderCount),
             {QStringLiteral("Everything in Pro"),
              QStringLiteral("Builder badge on your ForkMesh profile"),
              QStringLiteral("Exclusive Builder World outfit and office prop set"),
              QStringLiteral("Priority queue for custom office and World requests"),
              QStringLiteral("Access to supporters office hours and group AMAs"),
              QStringLiteral("Early access to selected experimental features"),
              QStringLiteral("Private supporters community role")},
             QStringLiteral(":/app/membership/patreon-builder.png"),
             QStringLiteral("Builder artwork from the ForkMesh Patreon campaign"),
             QStringLiteral("Choose Builder ↗"), kPatreonBuilderUrl,
             false),
        0, 2);
    plans->addWidget(
        plan(QStringLiteral("Studio"), QStringLiteral("$100 / month"),
             studioCount == 1
                 ? QStringLiteral("1 active Studio")
                 : QStringLiteral("%1 active Studios").arg(studioCount),
             {QStringLiteral("Everything in Builder"),
              QStringLiteral("Studio badge on your ForkMesh profile"),
              QStringLiteral("Kickoff onboarding call"),
              QStringLiteral("Personal ForkMesh World and office setup assistance"),
              QStringLiteral("Guided subdomain and deployment pipeline setup"),
              QStringLiteral("Hands-on help throughout the entire setup process"),
              QStringLiteral("Weekly syncs during onboarding and active Studio engagement"),
              QStringLiteral("Priority support channel (business-hours priority, not 24/7)"),
              QStringLiteral("Direct roadmap input for your use case")},
             QStringLiteral(":/app/membership/patreon-studio.png"),
             QStringLiteral("Studio artwork from the ForkMesh Patreon campaign"),
             QStringLiteral("Choose Studio ↗"), kPatreonStudioUrl,
             false),
        0, 3);
    layout->addLayout(plans);

    auto *privacy = new QLabel(QStringLiteral(
        "Financially transparent—<a style='color:%1' "
        "href='https://forkmesh.com/ledger'>review our public ledger</a>. "
        "Patreon handles payments; every Core feature stays enabled whether or "
        "not you join.")
        .arg(Theme::nextOrHex(Theme::Role::AccentFg, QStringLiteral("#b7ffd1"))));
    privacy->setObjectName(QStringLiteral("membershipMuted"));
    privacy->setTextFormat(Qt::RichText);
    privacy->setOpenExternalLinks(true);
    privacy->setWordWrap(true);
    layout->addWidget(privacy);

    auto *actions = new QHBoxLayout;
    auto *later = new QPushButton(QStringLiteral("Close"), dialog);
    auto *compare = new QPushButton(QStringLiteral("View on Patreon ↗"), dialog);
    actions->addWidget(later);
    actions->addStretch(1);
    actions->addWidget(compare);
    layout->addLayout(actions);
    connect(later, &QPushButton::clicked, dialog, &QDialog::close);
    connect(compare, &QPushButton::clicked, dialog, [] {
        QDesktopServices::openUrl(kPatreonJoinUrl);
    });
    scroll->setWidget(page);
    outer->addWidget(scroll);
    dialog->show();
    dialog->raise();
}
