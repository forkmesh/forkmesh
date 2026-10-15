#include "PackageFeedback.h"

#include "Theme.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSysInfo>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace forkmesh::market {

namespace {

QString clipped(const QString &value, int maximum)
{
    const QString trimmed = value.trimmed();
    return trimmed.size() > maximum ? trimmed.left(maximum).trimmed() : trimmed;
}

} // namespace

QString desktopFeedbackPlatform()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#else
    return QStringLiteral("linux");
#endif
}

PackageFeedbackReport packageFeedbackReport(const QString &packageId,
                                            const QString &packageName,
                                            const QString &packageVersion,
                                            const QString &appVersion)
{
    PackageFeedbackReport report;
    report.packageId = packageId;
    report.packageName = packageName;
    report.packageVersion = packageVersion;
    report.appVersion = appVersion;
    report.platform = desktopFeedbackPlatform();
    report.osVersion = QSysInfo::prettyProductName();
    report.device = QSysInfo::currentCpuArchitecture();
    return report;
}

QJsonObject packageFeedbackPayload(const PackageFeedbackReport &report)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("message"),
                   clipped(report.message, kPackageFeedbackMaxMessage));
    const QString contact =
        clipped(report.contact, kPackageFeedbackMaxContact);
    if (!contact.isEmpty())
        payload.insert(QStringLiteral("contact"), contact);
    const QString name = report.packageName.trimmed().isEmpty()
                             ? report.packageId.trimmed()
                             : report.packageName.trimmed();
    payload.insert(
        QStringLiteral("screen"),
        clipped(QStringLiteral("Plugin: %1 %2")
                    .arg(name, report.packageVersion.trimmed()),
                120));
    payload.insert(QStringLiteral("severity"), QStringLiteral("normal"));
    payload.insert(QStringLiteral("source"), QStringLiteral("desktop"));
    payload.insert(QStringLiteral("platform"), clipped(report.platform, 40));
    payload.insert(QStringLiteral("appVersion"),
                   clipped(report.appVersion, 60));
    payload.insert(QStringLiteral("osVersion"), clipped(report.osVersion, 60));
    payload.insert(QStringLiteral("device"), clipped(report.device, 120));
    payload.insert(QStringLiteral("package"),
                   report.packageId.trimmed().toLower());
    return payload;
}

PackageFeedbackOutcome packageFeedbackOutcome(int httpStatus,
                                              const QByteArray &body,
                                              const QString &networkError)
{
    const QJsonObject reply = QJsonDocument::fromJson(body).object();
    if ((httpStatus == 200 || httpStatus == 201) &&
        reply.value(QStringLiteral("ok")).toBool()) {
        const int issue = reply.value(QStringLiteral("issueNumber")).toInt();
        return {true,
                issue > 0
                    ? QStringLiteral("Thanks. Your report was sent and filed "
                                     "as issue #%1.")
                          .arg(issue)
                    : QStringLiteral("Thanks. Your report was sent.")};
    }
    if (httpStatus == 429)
        return {false, QStringLiteral("Too many reports were sent from here "
                                      "in the last hour. Try again later.")};
    if (httpStatus == 413)
        return {false, QStringLiteral("This report is too long to send. "
                                      "Shorten it and try again.")};
    if (httpStatus == 400 &&
        reply.value(QStringLiteral("error")).toString() ==
            QLatin1String("empty_report"))
        return {false, QStringLiteral("Describe the problem in a few more "
                                      "words, then send it again.")};
    if (httpStatus <= 0)
        return {false,
                networkError.trimmed().isEmpty()
                    ? QStringLiteral("ForkMesh could not be reached. Check "
                                     "your connection and try again.")
                    : QStringLiteral("ForkMesh could not be reached (%1). "
                                     "Try again.")
                          .arg(networkError.trimmed())};
    return {false, QStringLiteral("The report was not sent (HTTP %1). Try "
                                  "again.")
                       .arg(httpStatus)};
}

QUrl packageRatingUrl(const QString &origin, const QString &marketplaceId,
                      const QString &packageId)
{
    QUrl url(origin.trimmed());
    const QString id = packageId.trimmed().toLower();
    if (!url.isValid() || url.host().isEmpty() || id.isEmpty() ||
        (url.scheme() != QLatin1String("http") &&
         url.scheme() != QLatin1String("https")))
        return {};
    url.setUserInfo(QString());
    url.setPath(QStringLiteral("/market-reviews"));
    QUrlQuery query;
    const QString market = marketplaceId.trimmed().toLower();
    query.addQueryItem(QStringLiteral("market"),
                       market.isEmpty() ? QStringLiteral("forkmesh") : market);
    query.addQueryItem(QStringLiteral("package"), id);
    url.setQuery(query);
    url.setFragment(QString());
    return url;
}

PackageFeedbackDialog::PackageFeedbackDialog(const PackageFeedbackReport &base,
                                             PackageFeedbackSender sender,
                                             QWidget *parent)
    : QDialog(parent), m_base(base), m_sender(std::move(sender))
{
    const QString name =
        base.packageName.isEmpty() ? base.packageId : base.packageName;
    setObjectName(QStringLiteral("packageFeedbackDialog"));
    setProperty("forkmeshPackageId", base.packageId);
    setWindowTitle(QStringLiteral("Feedback for %1").arg(name));
    setMinimumWidth(440);

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        QStringLiteral("Tell us what works, what does not, or what went wrong "
                       "in %1. Your report becomes a public issue, so leave "
                       "out anything private.")
            .arg(name),
        this);
    intro->setObjectName(QStringLiteral("packageFeedbackIntro"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_message = new QPlainTextEdit(this);
    m_message->setObjectName(QStringLiteral("packageFeedbackMessage"));
    m_message->setPlaceholderText(
        QStringLiteral("What happened, or what would make %1 better?")
            .arg(name));
    m_message->setTabChangesFocus(true);
    m_message->setMinimumHeight(140);
    layout->addWidget(m_message, 1);

    m_contact = new QLineEdit(this);
    m_contact->setObjectName(QStringLiteral("packageFeedbackContact"));
    m_contact->setMaxLength(kPackageFeedbackMaxContact);
    m_contact->setPlaceholderText(
        QStringLiteral("Email or ForkMesh name for a reply (optional)"));
    layout->addWidget(m_contact);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("packageFeedbackStatus"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->hide();
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(this);
    m_cancel = buttons->addButton(QDialogButtonBox::Cancel);
    m_cancel->setObjectName(QStringLiteral("packageFeedbackCancel"));
    m_send = buttons->addButton(QStringLiteral("Send"),
                                QDialogButtonBox::AcceptRole);
    m_send->setObjectName(QStringLiteral("packageFeedbackSend"));
    m_send->setDefault(true);
    layout->addWidget(buttons);

    connect(m_message, &QPlainTextEdit::textChanged, this,
            [this] { updateSendEnabled(); });
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_send, &QPushButton::clicked, this, [this] { send(); });
    updateSendEnabled();
    m_message->setFocus();
}

QJsonObject PackageFeedbackDialog::payload() const
{
    PackageFeedbackReport report = m_base;
    report.message = m_message->toPlainText();
    report.contact = m_contact->text();
    return packageFeedbackPayload(report);
}

void PackageFeedbackDialog::updateSendEnabled()
{
    m_send->setEnabled(!m_sending && !m_sent &&
                       m_message->toPlainText().trimmed().size() >=
                           kPackageFeedbackMinMessage);
}

void PackageFeedbackDialog::send()
{
    if (m_sending || m_sent || !m_send->isEnabled())
        return;
    if (!m_sender) {
        finish(packageFeedbackOutcome(0, {}, {}));
        return;
    }
    m_sending = true;
    m_message->setReadOnly(true);
    m_contact->setReadOnly(true);
    m_status->setStyleSheet(QString());
    m_status->setText(QString::fromUtf8("Sending\xE2\x80\xA6"));
    m_status->show();
    updateSendEnabled();
    QPointer<PackageFeedbackDialog> self(this);
    m_sender(payload(), [self](int status, const QByteArray &body,
                               const QString &error) {
        if (self)
            self->finish(packageFeedbackOutcome(status, body, error));
    });
}

void PackageFeedbackDialog::finish(const PackageFeedbackOutcome &outcome)
{
    m_sending = false;
    m_sent = outcome.sent;
    m_status->setProperty("sent", outcome.sent);
    m_status->setStyleSheet(
        outcome.sent ? QString()
                     : Theme::remapSheet(QStringLiteral("color:#f85149;")));
    m_status->setText(outcome.text);
    m_status->show();
    if (outcome.sent) {
        m_cancel->setText(QStringLiteral("Close"));
        m_cancel->setFocus();
    } else {
        m_message->setReadOnly(false);
        m_contact->setReadOnly(false);
    }
    updateSendEnabled();
}

} // namespace forkmesh::market
