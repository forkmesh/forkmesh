#include "AwsSettingsPanel.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <memory>

namespace forkmesh::aws {

namespace {

constexpr int kTimeoutMs = 20000;

struct PanelState {
    PanelOptions options;
    QPointer<QLineEdit> accessKey;
    QPointer<QLineEdit> secret;
    QPointer<QComboBox> region;
    QPointer<QLabel> status;
    QPointer<QLabel> cheapest;
    QPointer<QPushButton> test;
    CredentialStore store;
    int testSerial = 0;

    explicit PanelState(const PanelOptions &panelOptions)
        : options(panelOptions), store(panelOptions.keyPath)
    {
    }

    Credentials entered(QString *error) const
    {
        Credentials credentials;
        credentials.accessKeyId = accessKey->text().trimmed();
        credentials.secretAccessKey = secret->text().trimmed();
        credentials.region = region->currentData().toString();
        if (credentials.secretAccessKey.isEmpty() && store.hasStored()) {
            const Credentials stored = store.load(error);
            if (stored.accessKeyId == credentials.accessKeyId)
                credentials.secretAccessKey = stored.secretAccessKey;
        }
        return credentials;
    }

    void say(const QString &text, bool problem = false)
    {
        status->setText(text);
        status->setStyleSheet(problem ? QStringLiteral("color:#f85149;") : QString());
    }

    void refreshCheapest()
    {
        const QString type = cheapestInstanceType();
        cheapest->setText(
            QStringLiteral("Smallest fleet-ready instance: %1 (x86_64, 1 GB): about $%2/month "
                           "on demand in us-east-1, including its public IPv4 and a 20 GB "
                           "encrypted gp3 disk. Other regions cost more.")
                .arg(type, QString::number(monthlyEstimate(type), 'f', 2)));
    }
};

} // namespace

Transport networkTransport()
{
    return [](const Request &request, QObject *context, ReplyHandler done) {
        auto *manager = new QNetworkAccessManager(context);
        QNetworkRequest networkRequest(request.url);
        networkRequest.setTransferTimeout(kTimeoutMs);
        for (const auto &header : request.headers) {
            if (header.first.compare("host", Qt::CaseInsensitive) != 0)
                networkRequest.setRawHeader(header.first, header.second);
        }
        QNetworkReply *reply =
            manager->sendCustomRequest(networkRequest, request.method, request.body);
        QObject::connect(reply, &QNetworkReply::finished, manager, [reply, manager, done] {
            HttpReply result;
            result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            result.body = reply->readAll();
            if (reply->error() != QNetworkReply::NoError && result.status == 0)
                result.error = reply->errorString();
            reply->deleteLater();
            manager->deleteLater();
            done(result);
        });
    };
}

QWidget *buildSettingsPanel(const PanelOptions &options, QWidget *parent)
{
    auto *panel = new QWidget(parent);
    panel->setObjectName(QStringLiteral("awsSettingsPanel"));
    auto state = std::make_shared<PanelState>(options);
    if (!state->options.transport)
        state->options.transport = networkTransport();
    if (!state->options.clock)
        state->options.clock = [] { return QDateTime::currentDateTimeUtc(); };

    auto *column = new QVBoxLayout(panel);
    column->setContentsMargins(2, 12, 2, 12);
    column->setSpacing(8);
    auto *title = new QLabel(QStringLiteral("Amazon Web Services"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    column->addWidget(title);
    auto *intro = new QLabel(QStringLiteral(
        "An IAM user's access key lets ForkMesh create fleet nodes on AWS (Add core node, "
        "or next_fleet.py create --provider aws). The key is stored encrypted on this "
        "device; the secret is never shown again. Give the user only the ForkMesh fleet "
        "policy from docs/deploy/README.md, not administrator access."));
    intro->setWordWrap(true);
    intro->setObjectName(QStringLiteral("mutedLabel"));
    column->addWidget(intro);

    auto *form = new QFormLayout;
    state->accessKey = new QLineEdit;
    state->accessKey->setObjectName(QStringLiteral("awsAccessKeyId"));
    state->accessKey->setPlaceholderText(QStringLiteral("AKIA…"));
    state->accessKey->setMaxLength(20);
    state->secret = new QLineEdit;
    state->secret->setObjectName(QStringLiteral("awsSecretAccessKey"));
    state->secret->setEchoMode(QLineEdit::Password);
    state->secret->setMaxLength(128);
    state->region = new QComboBox;
    state->region->setObjectName(QStringLiteral("awsRegion"));
    for (const QString &name : regions())
        state->region->addItem(name, name);
    form->addRow(QStringLiteral("Access key ID"), state->accessKey);
    form->addRow(QStringLiteral("Secret access key"), state->secret);
    form->addRow(QStringLiteral("Region"), state->region);
    column->addLayout(form);

    auto *buttons = new QHBoxLayout;
    auto *save = new QPushButton(QStringLiteral("Save"));
    save->setObjectName(QStringLiteral("awsSave"));
    state->test = new QPushButton(QStringLiteral("Test"));
    state->test->setObjectName(QStringLiteral("awsTest"));
    state->test->setToolTip(QStringLiteral(
        "Calls sts:GetCallerIdentity, which reads nothing and changes nothing."));
    auto *exportButton = new QPushButton(QStringLiteral("Write aws.env for the fleet tools"));
    exportButton->setObjectName(QStringLiteral("awsExport"));
    exportButton->setToolTip(QStringLiteral(
        "Writes AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY and AWS_REGION to aws.env in the "
        "fleet config folder (mode 0600, like vultr.env) for next_fleet.py and Add core "
        "node. That file is plain text on disk."));
    auto *forget = new QPushButton(QStringLiteral("Forget"));
    forget->setObjectName(QStringLiteral("awsForget"));
    buttons->addWidget(save);
    buttons->addWidget(state->test);
    buttons->addWidget(exportButton);
    buttons->addWidget(forget);
    buttons->addStretch(1);
    column->addLayout(buttons);

    state->status = new QLabel;
    state->status->setObjectName(QStringLiteral("awsStatus"));
    state->status->setWordWrap(true);
    state->status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    column->addWidget(state->status);
    state->cheapest = new QLabel;
    state->cheapest->setObjectName(QStringLiteral("awsCheapest"));
    state->cheapest->setWordWrap(true);
    column->addWidget(state->cheapest);
    column->addStretch(1);

    QString loadError;
    const Credentials stored = state->store.load(&loadError);
    state->accessKey->setText(stored.accessKeyId);
    state->region->setCurrentIndex(qMax(0, state->region->findData(stored.region)));
    if (!stored.secretAccessKey.isEmpty())
        state->secret->setPlaceholderText(QStringLiteral("stored (encrypted); type to replace"));
    if (!loadError.isEmpty())
        state->say(loadError, true);
    else if (!stored.secretAccessKey.isEmpty())
        state->say(QStringLiteral("Credentials stored for %1.").arg(stored.region));
    state->refreshCheapest();

    QObject::connect(save, &QPushButton::clicked, panel, [state] {
        QString error;
        const Credentials credentials = state->entered(&error);
        if (!state->store.save(credentials, &error)) {
            state->say(QStringLiteral("Not saved: %1").arg(error), true);
            return;
        }
        state->secret->clear();
        state->secret->setPlaceholderText(QStringLiteral("stored (encrypted); type to replace"));
        state->say(QStringLiteral("Saved (encrypted) for %1.").arg(credentials.region));
    });
    QObject::connect(forget, &QPushButton::clicked, panel, [state] {
        state->store.clear();
        state->secret->clear();
        state->secret->setPlaceholderText(QString());
        state->say(QStringLiteral("The stored AWS secret was removed from this device."));
    });
    QObject::connect(exportButton, &QPushButton::clicked, panel, [state] {
        QString error;
        const Credentials credentials = state->entered(&error);
        const QString directory = state->options.fleetConfigDirectory
                                      ? state->options.fleetConfigDirectory()
                                      : QString();
        if (!writeFleetEnv(directory, credentials, &error)) {
            state->say(QStringLiteral("aws.env not written: %1").arg(error), true);
            return;
        }
        state->say(QStringLiteral("Wrote %1/aws.env (mode 0600).").arg(directory));
    });
    QObject::connect(state->test, &QPushButton::clicked, panel, [state, panel] {
        QString error;
        const Credentials credentials = state->entered(&error);
        const QString problem = validationError(credentials);
        if (!problem.isEmpty()) {
            state->say(problem, true);
            return;
        }
        const int serial = ++state->testSerial;
        state->test->setEnabled(false);
        state->say(QStringLiteral("Asking AWS who this key belongs to…"));
        const Request request = callerIdentityRequest(credentials, state->options.clock());
        QPointer<QWidget> guard(panel);
        state->options.transport(request, panel, [state, guard, serial](const HttpReply &reply) {
            if (!guard || serial != state->testSerial)
                return;
            state->test->setEnabled(true);
            const CallerIdentity identity =
                parseCallerIdentity(reply.status, reply.body);
            if (identity.ok) {
                state->say(QStringLiteral("Connected: AWS account %1 (%2).")
                               .arg(identity.account, identity.arn));
                return;
            }
            state->say(QStringLiteral("AWS refused the key: %1")
                           .arg(reply.error.isEmpty() ? identity.error : reply.error),
                       true);
        });
    });
    return panel;
}

} // namespace forkmesh::aws
