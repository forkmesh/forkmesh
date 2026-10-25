// Local control-node operations: mirror lifecycle/sync/health, repository
// permissions, local identity and public wallet connection, Cloudflare relay
// bootstrap, remote hosts, and logs.

#include "ActionStore.h"
#include "ControlNode.h"
#include "FleetAutoscaleModel.h"
#include "ForkMeshVersion.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "PrivateMirrorStore.h"
#include "PublicMirrorRuntime.h"
#include "RunnerSetupHandoff.h"
#include "UiKit.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLocale>
#include <QMenu>
#include <QNetworkReply>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStandardPaths>
#include <QSaveFile>
#include <QTabBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QtMath>

#include <functional>

using namespace forkmesh::ui;

namespace {

QFrame *controlCard(const QString &heading, QVBoxLayout **bodyOut)
{
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("controlTabPane"));
    card->setFrameShape(QFrame::NoFrame);
    card->setAccessibleName(heading);
    auto *body = new QVBoxLayout(card);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(10);
    if (bodyOut)
        *bodyOut = body;
    return card;
}
QLabel *controlHint(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    label->setProperty("fmControlHelp", true);
    return label;
}

QLabel *cardSubheading(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    if (Theme::isNext())
        label->setProperty("controlNextRole", QStringLiteral("subheading"));
    return label;
}

const char *const kControlTabIcon = "controlNextIcon";
const char *const kControlTabFitted = "controlNextFitted";
const char *const kControlTabClassicWidth = "controlClassicWidth";
constexpr int kControlTabHeight = 36;
constexpr int kControlTabIconPx = 14;
constexpr int kControlTabPad = 10;
constexpr int kControlBodyMaxWidth = 1100;

QString controlNextTabIcon(const QString &icon)
{
    if (icon == QLatin1String("broadcast"))
        return QStringLiteral("sync");
    if (icon == QLatin1String("cloud"))
        return QStringLiteral("broadcast");
    return icon;
}

class ControlTabStyler : public QObject
{
public:
    explicit ControlTabStyler(QObject *parent) : QObject(parent) {}

    void adopt(VerticalIconButton *tile, const QString &icon)
    {
        if (!tile)
            return;
        tile->setProperty(kControlTabIcon, icon);
        tile->installEventFilter(this);
        fit(tile);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        if (type != QEvent::Paint && type != QEvent::Show && type != QEvent::FontChange &&
            type != QEvent::Polish && type != QEvent::StyleChange)
            return false;
        auto *tile = dynamic_cast<VerticalIconButton *>(watched);
        if (!tile)
            return false;
        if (type == QEvent::Polish || type == QEvent::StyleChange) {
            QPointer<VerticalIconButton> guarded(tile);
            QTimer::singleShot(0, tile, [guarded] {
                if (guarded)
                    fit(guarded);
            });
            return false;
        }
        if (type != QEvent::Paint) {
            fit(tile);
            return false;
        }
        if (!active(tile)) {
            release(tile);
            return false;
        }
        if (tile->size() != preferred(tile)) {
            QPointer<VerticalIconButton> guarded(tile);
            QTimer::singleShot(0, tile, [guarded] {
                if (guarded)
                    fit(guarded);
            });
        }
        paint(tile);
        return true;
    }

private:
    static bool active(const VerticalIconButton *tile)
    {
        return Theme::isNext() && !tile->inRail();
    }

    static QFont labelFont(const QWidget *tile)
    {
        QFont font = tile->font();
        font.setPixelSize(13);
        font.setWeight(QFont::Medium);
        return font;
    }

    static QSize preferred(const VerticalIconButton *tile)
    {
        const int label =
            qCeil(QFontMetricsF(labelFont(tile)).horizontalAdvance(tile->text())) + 2;
        return QSize(kControlTabPad + kControlTabIconPx + 6 + label + kControlTabPad,
                     kControlTabHeight);
    }

    static void fit(VerticalIconButton *tile)
    {
        if (!active(tile)) {
            release(tile);
            return;
        }
        if (!tile->property(kControlTabFitted).toBool()) {
            tile->setProperty(kControlTabClassicWidth,
                              tile->minimumWidth() == tile->maximumWidth()
                                  ? tile->minimumWidth()
                                  : -1);
        }
        const QSize want = preferred(tile);
        if (tile->minimumSize() != want || tile->maximumSize() != want)
            tile->setFixedSize(want);
        tile->setProperty(kControlTabFitted, true);
    }

    static void release(VerticalIconButton *tile)
    {
        if (!tile->property(kControlTabFitted).toBool())
            return;
        tile->setProperty(kControlTabFitted, false);
        const int width = tile->property(kControlTabClassicWidth).toInt();
        if (width > 0) {
            tile->setFixedWidth(width);
        } else {
            tile->setMinimumWidth(0);
            tile->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        tile->setMinimumHeight(0);
        tile->setMaximumHeight(QWIDGETSIZE_MAX);
    }

    static void paint(VerticalIconButton *tile)
    {
        QPainter painter(tile);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool enabled = tile->isEnabled();
        const bool hovered = enabled && tile->underMouse();
        const bool checked = tile->isChecked();
        const bool toggle =
            tile->objectName() == QLatin1String("controlNodeHelpToggle");
        const int width = tile->width();
        const int height = tile->height();
        QColor fg = Theme::color(checked || hovered ? Theme::Role::Fg
                                                    : Theme::Role::FgMuted);
        if (!enabled)
            fg = Theme::color(Theme::Role::FgSubtle);
        if (toggle && checked) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::color(Theme::Role::SurfaceRaised));
            painter.drawRoundedRect(QRectF(2, 5, width - 4, height - 10), 6, 6);
        }
        const QRect iconRect(kControlTabPad, (height - kControlTabIconPx) / 2,
                             kControlTabIconPx, kControlTabIconPx);
        const QString icon = tile->property(kControlTabIcon).toString();
        if (!icon.isEmpty())
            painter.drawPixmap(iconRect.topLeft(),
                               tintedOcticonPixmap(icon, fg, kControlTabIconPx));
        const QFont font = labelFont(tile);
        const int x = iconRect.right() + 1 + 6;
        const int room = qMax(0, width - x - kControlTabPad + 2);
        painter.setFont(font);
        painter.setPen(fg);
        painter.drawText(QRect(x, 0, room, height), Qt::AlignVCenter | Qt::AlignLeft,
                         QFontMetrics(font).elidedText(tile->text(), Qt::ElideRight, room));
        if (checked && !toggle) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::color(Theme::Role::AccentFg));
            painter.drawRoundedRect(QRectF(8, height - 2, qMax(0, width - 16), 2), 1, 1);
        }
    }
};

class ControlStatusDot : public QWidget
{
public:
    ControlStatusDot(QLabel *status, QWidget *spinner, QWidget *parent = nullptr)
        : QWidget(parent), m_status(status), m_spinner(spinner)
    {
        setObjectName(QStringLiteral("controlNextStatusDot"));
        setFixedSize(6, 16);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        if (status)
            status->installEventFilter(this);
        if (spinner)
            spinner->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        Q_UNUSED(watched);
        const QEvent::Type type = event->type();
        if (type == QEvent::Paint || type == QEvent::Show || type == QEvent::Hide)
            update();
        return false;
    }

    void paintEvent(QPaintEvent *) override
    {
        if (!m_status || (m_spinner && m_spinner->isVisible()))
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Theme::color(tone()));
        painter.drawEllipse(QRectF(0, 5, 6, 6));
    }

private:
    Theme::Role tone() const
    {
        const QString text = m_status->text();
        if (!m_status->styleSheet().isEmpty() ||
            text.contains(QLatin1String("failed"), Qt::CaseInsensitive) ||
            text.contains(QLatin1String("not found"), Qt::CaseInsensitive) ||
            text.contains(QLatin1String("could not start"), Qt::CaseInsensitive))
            return Theme::Role::Danger;
        if (text.startsWith(QLatin1String("Force is armed")))
            return Theme::Role::Warning;
        if (text.startsWith(QLatin1String("Ready")) ||
            text.contains(QLatin1String("succeeded"), Qt::CaseInsensitive) ||
            text.contains(QLatin1String("finished"), Qt::CaseInsensitive))
            return Theme::Role::Success;
        return Theme::Role::FgSubtle;
    }

    QPointer<QLabel> m_status;
    QPointer<QWidget> m_spinner;
};

class ControlShowWhenEnabled : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::EnabledChange && Theme::isNext()) {
            if (auto *widget = qobject_cast<QWidget *>(watched))
                widget->setVisible(widget->isEnabled());
        }
        return false;
    }
};

void arrangeNextDeployCard(QFrame *card, QVBoxLayout *col, const QString &icon,
                           const QString &title, QWidget *spinner, QLabel *status,
                           const QList<QPushButton *> &actions,
                           const QList<QWidget *> &hidden)
{
    if (!card || !col)
        return;
    QList<QWidget *> ordered;
    std::function<void(QLayout *)> collect = [&](QLayout *layout) {
        for (int index = 0; index < layout->count(); ++index) {
            QLayoutItem *item = layout->itemAt(index);
            if (QWidget *widget = item->widget())
                ordered.append(widget);
            else if (QLayout *nested = item->layout())
                collect(nested);
        }
    };
    collect(col);
    while (QLayoutItem *item = col->takeAt(0))
        delete item;

    card->setProperty("controlNextCard", true);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto *head = new QHBoxLayout;
    head->setContentsMargins(16, 14, 16, 14);
    head->setSpacing(12);
    auto *glyph = new QLabel;
    glyph->setObjectName(QStringLiteral("controlNextCardIcon"));
    glyph->setFixedSize(16, 18);
    glyph->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
    glyph->setPixmap(tintedOcticonPixmap(icon, Theme::color(Theme::Role::FgMuted), 16));
    head->addWidget(glyph, 0, Qt::AlignTop);
    auto *titleCol = new QVBoxLayout;
    titleCol->setContentsMargins(0, 0, 0, 0);
    titleCol->setSpacing(2);
    auto *heading = new QLabel(title);
    heading->setObjectName(QStringLiteral("controlNextCardTitle"));
    titleCol->addWidget(heading);
    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(6);
    if (status) {
        status->setProperty("controlNextRole", QStringLiteral("status"));
        statusRow->addWidget(new ControlStatusDot(status, spinner), 0, Qt::AlignTop);
    }
    if (spinner)
        statusRow->addWidget(spinner, 0, Qt::AlignTop);
    if (status)
        statusRow->addWidget(status, 1);
    titleCol->addLayout(statusRow);
    head->addLayout(titleCol, 1);
    for (QPushButton *button : actions) {
        if (button)
            head->addWidget(button, 0, Qt::AlignTop);
    }
    col->addLayout(head);

    auto *rule = new QFrame;
    rule->setObjectName(QStringLiteral("controlNextCardRule"));
    rule->setFrameShape(QFrame::NoFrame);
    rule->setFixedHeight(1);
    col->addWidget(rule);

    auto *body = new QVBoxLayout;
    body->setContentsMargins(16, 16, 16, 16);
    body->setSpacing(10);
    for (QWidget *widget : std::as_const(ordered)) {
        auto *asButton = qobject_cast<QPushButton *>(widget);
        if (widget == spinner || widget == status ||
            (asButton && actions.contains(asButton)))
            continue;
        if (hidden.contains(widget)) {
            widget->setVisible(false);
            continue;
        }
        body->addWidget(widget);
    }
    col->addLayout(body);
}

void polishControlNextChrome(QWidget *page)
{
    if (!page)
        return;
    const auto buttons = page->findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (dynamic_cast<VerticalIconButton *>(button) ||
            button->property("controlNextButton").isValid())
            continue;
        const bool primary =
            button->property("buttonSize").toString() == QLatin1String("primary");
        button->setProperty("controlNextButton",
                            primary ? QStringLiteral("primary") : QStringLiteral("outline"));
        if (!button->property("nextIconRole").isValid())
            button->setProperty("nextIconRole",
                                int(primary ? Theme::Role::Bg : Theme::Role::Fg));
        applyStoredOcticon(button);
    }
    for (const char *name :
         {"controlPermissionsTable", "exportValuesTable", "exportDataTable",
          "importValuesTable", "contributorRewardsTable", "rewardPoolIntentsTable"}) {
        auto *table = page->findChild<QTableWidget *>(QLatin1String(name));
        if (!table || table->property("dataTable").toBool())
            continue;
        table->setProperty("dataTable", true);
        if (table->testAttribute(Qt::WA_WState_Polished)) {
            QEvent polish(QEvent::Polish);
            QCoreApplication::sendEvent(table, &polish);
        }
    }
    const auto forms = page->findChildren<QFormLayout *>();
    for (QFormLayout *form : forms) {
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(8);
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem *item = form->itemAt(row, QFormLayout::LabelRole);
            if (QWidget *label = item ? item->widget() : nullptr)
                label->setProperty("controlNextRole", QStringLiteral("fieldLabel"));
        }
    }
}

// The export and import pages list secrets by name. Masking is a screen
// courtesy only — the JSON itself is documented as clear text — so err towards
// masking anything whose name reads like a credential.
bool looksLikeSecretName(const QString &name)
{
    const QString upper = name.toUpper();
    for (const QString &marker :
         {QStringLiteral("TOKEN"), QStringLiteral("SECRET"),
          QStringLiteral("PASSWORD"), QStringLiteral("PASSPHRASE"),
          QStringLiteral("CREDENTIAL"), QStringLiteral("PRIVATE"),
          QStringLiteral("KEY"), QStringLiteral("SIGNATURE"),
          QStringLiteral("SEED"), QStringLiteral("AUTH")}) {
        if (upper.contains(marker))
            return true;
    }
    return false;
}

QString maskedExportValue(const QString &name, const QString &value, bool reveal)
{
    if (value.isEmpty())
        return QStringLiteral("(empty)");
    if (reveal || !looksLikeSecretName(name)) {
        return value.size() <= 400 ? value
                                   : value.left(400) + QStringLiteral("…");
    }
    return QStringLiteral("•••••••• ends %1 · %2 characters")
        .arg(value.right(4))
        .arg(value.size());
}

QString exportSettingText(const QVariant &value)
{
    switch (value.typeId()) {
    case QMetaType::QStringList:
        return value.toStringList().join(QStringLiteral(", "));
    case QMetaType::QByteArray:
        return QStringLiteral("%1 binary bytes")
            .arg(value.toByteArray().size());
    case QMetaType::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    default:
        break;
    }
    const QString text = value.toString();
    if (!text.isEmpty() || !value.isValid())
        return text;
    return QStringLiteral("(%1)")
        .arg(QString::fromLatin1(value.typeName() ? value.typeName() : "empty"));
}

QTableWidget *systemValuesTable(const QString &objectName)
{
    auto *table = new QTableWidget(0, 3);
    table->setObjectName(objectName);
    table->setHorizontalHeaderLabels({QStringLiteral("Section"),
                                      QStringLiteral("Name"),
                                      QStringLiteral("Value")});
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->setMinimumHeight(320);
    table->horizontalHeader()->setSectionResizeMode(0,
                                                    QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1,
                                                    QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    return table;
}

constexpr int kValueTooltipChars = 2000;

void appendValueRow(QTableWidget *table, const QString &section,
                    const QString &name, const QString &value)
{
    if (!table)
        return;
    const int row = table->rowCount();
    table->insertRow(row);
    table->setItem(row, 0, new QTableWidgetItem(section));
    table->setItem(row, 1, new QTableWidgetItem(name));
    auto *valueItem = new QTableWidgetItem(forkmesh::ui::kit::cellDisplayText(value));
    valueItem->setToolTip(forkmesh::ui::kit::cellDisplayText(value, kValueTooltipChars));
    table->setItem(row, 2, valueItem);
}

void appendDeploymentValueRows(QTableWidget *table, const QString &prefix,
                               const QJsonObject &object, bool reveal)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString name = prefix.isEmpty()
                                 ? it.key()
                                 : prefix + QStringLiteral(" · ") + it.key();
        const QJsonValue value = it.value();
        if (value.isObject()) {
            const QJsonObject child = value.toObject();
            if (child.isEmpty()) {
                appendValueRow(table, QStringLiteral("Deployment"), name,
                               QStringLiteral("(empty)"));
                continue;
            }
            appendDeploymentValueRows(table, name, child, reveal);
            continue;
        }
        QString text;
        if (value.isBool())
            text = value.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
        else if (value.isDouble())
            text = QString::number(value.toDouble());
        else
            text = value.toString();
        appendValueRow(table, QStringLiteral("Deployment"), name,
                       maskedExportValue(name, text, reveal));
    }
}

QString publicRelayOrigin(const QString &configured)
{
    QUrl url(configured.trimmed());
    if (!url.isValid() || url.host().isEmpty())
        return QStringLiteral("https://forkmesh.com");
    if (url.scheme() == QLatin1String("wss"))
        url.setScheme(QStringLiteral("https"));
    else if (url.scheme() == QLatin1String("ws"))
        url.setScheme(QStringLiteral("http"));
    url.setUserInfo(QString());
    url.setPath(QString());
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString(QUrl::RemoveUserInfo | QUrl::StripTrailingSlash);
}

QString repoControlKey(const RepositoryRecord &repo)
{
    return repo.owner + QLatin1Char('/') + repo.name;
}

QString directGatewayRoot()
{
    return forkmesh::paths::dataRoot() +
           QStringLiteral("/mirror-gateway");
}

QString directGatewayConfigPath()
{
    return QDir(directGatewayRoot())
        .filePath(QStringLiteral("config.json"));
}

QString directGatewayManifestPath()
{
    return QDir(directGatewayRoot())
        .filePath(QStringLiteral("forkmesh-mirror.json"));
}

QString directGatewayConnectorTokenPath()
{
    return QDir(directGatewayRoot())
        .filePath(QStringLiteral("connector.token"));
}

bool prepareOwnerDirectory(const QString &path, QString *error)
{
    QDir directory(path);
    if ((!directory.exists() &&
         !QDir().mkpath(directory.absolutePath())) ||
        QFileInfo(directory.absolutePath()).isSymLink() ||
        !QFileInfo(directory.absolutePath()).isDir()) {
        if (error)
            *error = QStringLiteral(
                "Could not create the owner-only mirror-gateway directory.");
        return false;
    }
    QFile permissions(directory.absolutePath());
    if (!permissions.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner)) {
        if (error)
            *error = QStringLiteral(
                "Could not restrict the mirror-gateway directory.");
        return false;
    }
    return true;
}

bool writeOwnerJson(const QString &path, const QJsonObject &object,
                    QString *error)
{
    if (!prepareOwnerDirectory(QFileInfo(path).absolutePath(), error))
        return false;
    const QFileInfo existing(path);
    if (existing.exists() &&
        (existing.isSymLink() || !existing.isFile())) {
        if (error)
            *error = QStringLiteral(
                "The mirror-gateway configuration path is unsafe.");
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner |
                             QFileDevice::WriteOwner)) {
        file.cancelWriting();
        if (error)
            *error = QStringLiteral(
                "Could not create the mirror-gateway configuration.");
        return false;
    }
    const QByteArray bytes =
        QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral(
                "Could not commit the mirror-gateway configuration.");
        return false;
    }
    return true;
}

// app/.env.production is an owner-only credential store and must never be
// created through a symlink.
bool writeEnvAssignments(const QString &path,
                         const QMap<QString, QString> &values,
                         QString *error, bool *changed = nullptr)
{
    if (changed)
        *changed = false;
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const QFileInfo info(path);
    QString contents;
    if (info.exists()) {
        if (info.isSymLink() || !info.isFile())
            return fail(QStringLiteral("%1 is not a regular file.").arg(path));
        // A.env is a handful of lines; anything larger is not the file this
        // rotation should be rewriting, and truncating it would lose secrets.
        if (info.size() > 512 * 1024)
            return fail(QStringLiteral("%1 is unexpectedly large.").arg(path));
        QFile existing(path);
        if (!existing.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("Could not read %1.").arg(path));
        contents = QString::fromUtf8(existing.readAll());
    }
    const QString before = contents;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        contents = forkmesh::control::updatedEnvAssignment(contents, it.key(),
                                                           it.value());
    }
    if (info.exists() && contents == before)
        return true;
    if (changed)
        *changed = true;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner |
                             QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return fail(QStringLiteral("Could not write %1.").arg(path));
    }
    const QByteArray bytes = contents.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit())
        return fail(QStringLiteral("Could not commit %1.").arg(path));
    return true;
}

QString normalizedHttpsOrigin(const QString &hostname)
{
    const QString host = hostname.trimmed().toLower();
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(host);
    return url.isValid() && !url.host().isEmpty()
               ? url.toString(QUrl::StripTrailingSlash)
               : QString();
}

bool controlMirrorReady(const RepositoryRecord &repo)
{
    if (!repo.isPrivate) {
        return QFileInfo(repo.mirrorPath).isDir() &&
               QFileInfo(QDir(repo.mirrorPath)
                             .filePath(QStringLiteral("HEAD")))
                   .isFile() &&
               !PublicMirrorRuntime::repositoryRefsSha256(repo.mirrorPath)
                    .isEmpty();
    }
    if (!PrivateMirrorStore::isOpaqueId(repo.privateReplicaId))
        return false;
    const QString root =
        forkmesh::paths::dataRoot() +
        QStringLiteral("/private-replicas");
    PrivateMirrorStore::Metadata metadata;
    return PrivateMirrorStore::inspectReplica(
        root, repo.privateReplicaId, &metadata, nullptr);
}

QString controlMirrorReadyKey(const RepositoryRecord &repo)
{
    if (repo.isPrivate) {
        return PrivateMirrorStore::isOpaqueId(repo.privateReplicaId)
                   ? QStringLiteral("private:") + repo.privateReplicaId
                   : QString();
    }
    return repo.mirrorPath.trimmed().isEmpty()
               ? QString()
               : QStringLiteral("public:") + repo.mirrorPath +
                     QLatin1Char(':') + QString::number(repo.lastSyncMs);
}

} // namespace

QWidget *MainWindow::buildControlNodeSection()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("controlNodeSection"));
    const bool next = Theme::isNext();
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(24, kRepoTabRowTopInset, 24, 24);
    outer->setSpacing(8);
    if (next) {
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
    }

    auto *tabs = new QTabWidget;
    tabs->setObjectName(QStringLiteral("controlNodeTabs"));
    tabs->setDocumentMode(true);
    m_controlNodeTabs = tabs;

    auto *tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(2);
    auto *tabGroup = new QButtonGroup(page);
    tabGroup->setExclusive(true);
    QVector<VerticalIconButton *> tabTiles;

    connect(tabs, &QTabWidget::currentChanged, this,
            [this, tabGroup](int index) {
                if (QAbstractButton *tile = tabGroup->button(index))
                    tile->setChecked(true);
                if (index == m_controlExportTabIndex)
                    refreshSystemExportValues();
                scheduleNavRecord();
            });
    const auto addTab = [&](QWidget *card, const QString &name,
                            const QString &caption, const QString &icon) {
        auto *body = new QWidget;
        auto *bodyCol = new QVBoxLayout(body);
        bodyCol->setContentsMargins(2, 6, 2, 12);
        bodyCol->setSpacing(14);
        if (next) {
            bodyCol->setContentsMargins(24, 20, 24, 24);
            bodyCol->setSpacing(12);
            const auto addCentred = [bodyCol](QWidget *widget) {
                auto *row = new QHBoxLayout;
                row->setContentsMargins(0, 0, 0, 0);
                row->setSpacing(0);
                row->addStretch(0);
                row->addWidget(widget, 1);
                row->addStretch(0);
                bodyCol->addLayout(row);
            };
            if (!card->property("controlNextBare").toBool()) {
                auto *heading = new QLabel(name);
                heading->setObjectName(QStringLiteral("controlNextSectionTitle"));
                heading->setMaximumWidth(kControlBodyMaxWidth);
                heading->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                addCentred(heading);
            }
            card->setMaximumWidth(kControlBodyMaxWidth);
            card->setSizePolicy(QSizePolicy::Expanding, card->sizePolicy().verticalPolicy());
            addCentred(card);
        } else {
            bodyCol->addWidget(card);
        }
        bodyCol->addStretch();
        auto *scroll = new QScrollArea;
        scroll->setObjectName(QStringLiteral("controlNodeTabScroll"));
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(body);
        const int index = tabs->count();
        tabs->addTab(scroll, name);

        auto *tile = new VerticalIconButton(caption, VerticalIconButton::Tab);
        tile->setObjectName(QStringLiteral("controlNodeTab"));
        tile->setCheckable(true);
        tile->setChecked(index == 0);
        tile->setToolTip(name);
        tile->setOcticonName(icon);
        if (next)
            tile->setProperty(kControlTabIcon, controlNextTabIcon(icon));
        tabGroup->addButton(tile, index);
        tabRow->addWidget(tile);
        tabTiles.append(tile);
    };

    auto *deployCards = new QWidget;
    auto *deployCol = new QVBoxLayout(deployCards);
    deployCol->setContentsMargins(0, 0, 0, 0);
    deployCol->setSpacing(14);
    deployCol->addWidget(buildNodeDeployCard());
    deployCol->addWidget(buildSiteDeployCard());
    if (next) {
        deployCol->setSpacing(20);
        deployCards->setProperty("controlNextBare", true);
    }
    addTab(deployCards, QStringLiteral("Site deployment"),
           QStringLiteral("Deploy"), QStringLiteral("rocket"));

    m_controlExportTabIndex = tabs->count();
    addTab(buildSystemExportCard(), QStringLiteral("Export system"),
           QStringLiteral("Export"), QStringLiteral("download"));
    m_controlImportTabIndex = tabs->count();
    addTab(buildSystemImportCard(), QStringLiteral("Import and deploy"),
           QStringLiteral("Import"), QStringLiteral("upload"));

    QVBoxLayout *serviceCol = nullptr;
    QFrame *serviceCard =
        controlCard(QStringLiteral("Mirror client"), &serviceCol);
    m_controlNodeStatus = new QLabel;
    m_controlNodeStatus->setObjectName(QStringLiteral("controlNodeStatus"));
    m_controlNodeStatus->setWordWrap(true);
    serviceCol->addWidget(m_controlNodeStatus);
    m_controlNodeHealth = new QLabel;
    m_controlNodeHealth->setObjectName(QStringLiteral("mutedLabel"));
    m_controlNodeHealth->setWordWrap(true);
    serviceCol->addWidget(m_controlNodeHealth);
    auto *serviceButtons = new QHBoxLayout;
    auto *syncButton = new QPushButton(QStringLiteral("Sync all mirrors"));
    syncButton->setObjectName(QStringLiteral("controlSyncMirrorsButton"));
    syncButton->setCursor(Qt::PointingHandCursor);
    setOcticon(syncButton, QStringLiteral("sync"), 14);
    connect(syncButton, &QPushButton::clicked, this,
            &MainWindow::syncControlNodeMirrors);
    serviceButtons->addWidget(syncButton);
    auto *healthButton = new QPushButton(QStringLiteral("Run health check"));
    healthButton->setObjectName(QStringLiteral("controlHealthButton"));
    healthButton->setCursor(Qt::PointingHandCursor);
    setOcticon(healthButton, QStringLiteral("check-circle"), 14);
    connect(healthButton, &QPushButton::clicked, this,
            &MainWindow::runControlNodeHealthCheck);
    serviceButtons->addWidget(healthButton);
    auto *logsButton = new QPushButton(QStringLiteral("Open full local log"));
    logsButton->setObjectName(QStringLiteral("controlLogsButton"));
    logsButton->setCursor(Qt::PointingHandCursor);
    setOcticon(logsButton, QStringLiteral("terminal"), 14);
    connect(logsButton, &QPushButton::clicked, this,
            [this] { showSection(4); });
    serviceButtons->addWidget(logsButton);
    serviceButtons->addStretch();
    serviceCol->addLayout(serviceButtons);
    if (next) {
        serviceCard->setProperty("controlNextCard", true);
        serviceCol->setContentsMargins(16, 16, 16, 16);
    }
    const int mirrorsTabIndex = tabs->count();
    addTab(serviceCard, QStringLiteral("Mirror client"),
           QStringLiteral("Mirrors"), QStringLiteral("broadcast"));

    QVBoxLayout *permissionCol = nullptr;
    QFrame *permissionCard =
        controlCard(QStringLiteral("Repository permissions"), &permissionCol);
    permissionCol->addWidget(controlHint(
        QStringLiteral(
            "Control what this machine serves and which local workflows may run. "
            "Only an owner-capable identity can change network serving or "
            "visibility. Private repositories remain absent from public discovery.")));
    m_controlPermissionsTable = new QTableWidget(0, 7);
    m_controlPermissionsTable->setObjectName(
        QStringLiteral("controlPermissionsTable"));
    m_controlPermissionsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Repository"), QStringLiteral("Mirror"),
         QStringLiteral("Private"), QStringLiteral("Serve"),
         QStringLiteral("Run actions"), QStringLiteral("Secret scan"),
         QStringLiteral("Last sync")});
    m_controlPermissionsTable->verticalHeader()->setVisible(false);
    m_controlPermissionsTable->setSelectionBehavior(
        QAbstractItemView::SelectRows);
    m_controlPermissionsTable->setShowGrid(false);
    m_controlPermissionsTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    for (int column = 1; column < 7; ++column) {
        m_controlPermissionsTable->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    connect(m_controlPermissionsTable, &QTableWidget::itemChanged, this,
            &MainWindow::updateControlRepositoryPermission);
    permissionCol->addWidget(m_controlPermissionsTable);
    addTab(permissionCard, QStringLiteral("Repository permissions"),
           QStringLiteral("Permissions"), QStringLiteral("lock"));

    QVBoxLayout *identityCol = nullptr;
    QFrame *identityCard =
        controlCard(QStringLiteral("Local keys and wallet connection"), &identityCol);
    identityCol->addWidget(controlHint(
        QStringLiteral(
            "The Ed25519 identity key signs node and mirror manifests locally. "
            "Backups are passphrase-encrypted. ForkMesh never asks for a wallet "
            "private key, seed phrase, or recovery phrase: only a public Solana "
            "payout address can be connected.")));
    m_controlIdentityStatus = new QLabel;
    m_controlIdentityStatus->setObjectName(
        QStringLiteral("controlIdentityStatus"));
    m_controlIdentityStatus->setWordWrap(true);
    m_controlIdentityStatus->setTextInteractionFlags(
        Qt::TextSelectableByMouse);
    identityCol->addWidget(m_controlIdentityStatus);
    auto *backupButton =
        new QPushButton(QStringLiteral("Back up or import identity key"));
    backupButton->setObjectName(QStringLiteral("controlIdentityBackupButton"));
    backupButton->setCursor(Qt::PointingHandCursor);
    setOcticon(backupButton, QStringLiteral("key"), 14);
    connect(backupButton, &QPushButton::clicked, this,
            &MainWindow::backUpIdentityKey);
    identityCol->addWidget(backupButton, 0, Qt::AlignLeft);

    auto *walletRow = new QHBoxLayout;
    m_controlWalletEdit = new QLineEdit;
    m_controlWalletEdit->setObjectName(QStringLiteral("controlWalletAddress"));
    m_controlWalletEdit->setPlaceholderText(
        QStringLiteral("Solana public address (never a private key)"));
    m_controlWalletEdit->setMaxLength(64);
    walletRow->addWidget(m_controlWalletEdit, 1);
    auto *walletSave = new QPushButton(QStringLiteral("Connect address"));
    walletSave->setObjectName(QStringLiteral("controlWalletConnectButton"));
    walletSave->setCursor(Qt::PointingHandCursor);
    connect(walletSave, &QPushButton::clicked, this,
            &MainWindow::saveControlWalletAddress);
    walletRow->addWidget(walletSave);
    auto *walletClear = new QPushButton(QStringLiteral("Disconnect"));
    walletClear->setObjectName(QStringLiteral("controlWalletDisconnectButton"));
    walletClear->setCursor(Qt::PointingHandCursor);
    connect(walletClear, &QPushButton::clicked, this, [this] {
        if (m_controlWalletEdit)
            m_controlWalletEdit->clear();
        saveControlWalletAddress();
    });
    walletRow->addWidget(walletClear);
    identityCol->addLayout(walletRow);
    m_controlWalletStatus = new QLabel;
    m_controlWalletStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_controlWalletStatus->setWordWrap(true);
    identityCol->addWidget(m_controlWalletStatus);
    identityCol->addWidget(controlHint(
        QStringLiteral(
            "Fund states stay separate: User-owned funds remain in the "
            "self-custodial wallet; the Community-funded reward pool is a "
            "distinct public on-chain address; Pending rewards are uncompleted "
            "ledger allocations; Completed on-chain transfers include a "
            "confirmed transaction signature. ForkMesh never stores a user's "
            "wallet private key.")));
    if (next) {
        identityCard->setProperty("controlNextCard", true);
        identityCol->setContentsMargins(16, 16, 16, 16);
    }
    addTab(identityCard, QStringLiteral("Keys and wallet"),
           QStringLiteral("Keys"), QStringLiteral("key"));

    addTab(buildRewardPoolControlCard(), QStringLiteral("Reward pool"),
           QStringLiteral("Rewards"), QStringLiteral("credit-card"));

    QVBoxLayout *cloudflareCol = nullptr;
    QFrame *cloudflareCard =
        controlCard(QStringLiteral("Cloudflare relay deployment"), &cloudflareCol);
    cloudflareCol->addWidget(controlHint(
        QStringLiteral(
            "For a token scoped to exactly one account and active zone, leave the "
            "advanced topology fields empty: one click discovers the zone, derives "
            "separate relay and mirror hostnames, creates/reuses D1, deploys the "
            "Worker, configures proxied DNS and a Tunnel, and starts the local "
            "gateway. Broader tokens require an explicit account and zone so "
            "ForkMesh never guesses. Public repositories are served from "
            "integrity-pinned bare mirrors; private repositories retain their "
            "encrypted storage boundary. The API "
            "token is held in memory for this run, passed through the child "
            "environment, removed from displayed output, and never saved in "
            "settings, argv, logs, generated Worker configuration, D1, or Worker "
            "secrets. A Tunnel connector credential is stored owner-only on this "
            "device for service restarts; the local identity signs the public "
            "mirror manifest.")));
    auto *cloudflareForm = new QFormLayout;
    cloudflareForm->setLabelAlignment(Qt::AlignRight);
    QSettings settings;
    m_cloudflareHostnameEdit = new QLineEdit(
        settings.value(QStringLiteral("control/cloudflareHostname")).toString());
    m_cloudflareHostnameEdit->setObjectName(
        QStringLiteral("cloudflareHostname"));
    m_cloudflareHostnameEdit->setPlaceholderText(
        QStringLiteral("optional: auto-derive forkmesh.<zone>"));
    cloudflareForm->addRow(QStringLiteral("Relay hostname"),
                           m_cloudflareHostnameEdit);
    m_cloudflareMirrorHostnameEdit = new QLineEdit(
        settings
            .value(QStringLiteral(
                "control/cloudflareMirrorHostname"))
            .toString());
    m_cloudflareMirrorHostnameEdit->setObjectName(
        QStringLiteral("cloudflareMirrorHostname"));
    m_cloudflareMirrorHostnameEdit->setPlaceholderText(
        QStringLiteral("optional: auto-derive mirror.<zone>"));
    cloudflareForm->addRow(
        QStringLiteral("Mirror endpoint"),
        m_cloudflareMirrorHostnameEdit);
    m_cloudflareZoneEdit = new QLineEdit(
        settings.value(QStringLiteral("control/cloudflareZone")).toString());
    m_cloudflareZoneEdit->setObjectName(QStringLiteral("cloudflareZone"));
    m_cloudflareZoneEdit->setPlaceholderText(
        QStringLiteral("optional when token sees one active zone"));
    cloudflareForm->addRow(QStringLiteral("Cloudflare zone"),
                           m_cloudflareZoneEdit);
    m_cloudflareAccountEdit = new QLineEdit(
        settings.value(QStringLiteral("control/cloudflareAccount")).toString());
    m_cloudflareAccountEdit->setObjectName(
        QStringLiteral("cloudflareAccount"));
    m_cloudflareAccountEdit->setPlaceholderText(
        QStringLiteral("optional when the token has one account"));
    cloudflareForm->addRow(QStringLiteral("Account ID"),
                           m_cloudflareAccountEdit);
    m_cloudflareNodeNameEdit = new QLineEdit(
        settings.value(QStringLiteral("control/cloudflareNodeName")).toString());
    m_cloudflareNodeNameEdit->setObjectName(
        QStringLiteral("cloudflareNodeName"));
    m_cloudflareNodeNameEdit->setPlaceholderText(
        QStringLiteral("derived from hostname"));
    cloudflareForm->addRow(QStringLiteral("Node name"),
                           m_cloudflareNodeNameEdit);
    m_cloudflareRelayLabelEdit = new QLineEdit(
        settings.value(QStringLiteral("control/cloudflareRelayLabel")).toString());
    m_cloudflareRelayLabelEdit->setObjectName(
        QStringLiteral("cloudflareRelayLabel"));
    m_cloudflareRelayLabelEdit->setPlaceholderText(
        QStringLiteral("optional display label"));
    cloudflareForm->addRow(QStringLiteral("Relay label"),
                           m_cloudflareRelayLabelEdit);
    QString upstream =
        settings.value(QStringLiteral("control/cloudflareMainRelay")).toString();
    if (upstream.isEmpty() && !m_servers.isEmpty())
        upstream = publicRelayOrigin(m_servers.at(m_activeServer).url);
    m_cloudflareMainRelayEdit = new QLineEdit(upstream);
    m_cloudflareMainRelayEdit->setObjectName(
        QStringLiteral("cloudflareMainRelay"));
    m_cloudflareMainRelayEdit->setPlaceholderText(
        QStringLiteral("empty for a standalone main relay"));
    cloudflareForm->addRow(QStringLiteral("Upstream relay"),
                           m_cloudflareMainRelayEdit);
    m_cloudflareTokenEdit = new QLineEdit;
    m_cloudflareTokenEdit->setObjectName(QStringLiteral("cloudflareApiToken"));
    m_cloudflareTokenEdit->setEchoMode(QLineEdit::Password);
    m_cloudflareTokenEdit->setPlaceholderText(
        QStringLiteral("session-only Cloudflare API token"));
    m_cloudflareTokenEdit->setClearButtonEnabled(true);
    cloudflareForm->addRow(QStringLiteral("API token"),
                           m_cloudflareTokenEdit);
    m_cloudflareVpsHostEdit = new QLineEdit;
    m_cloudflareVpsHostEdit->setObjectName(
        QStringLiteral("cloudflareFirstMirrorHost"));
    m_cloudflareVpsHostEdit->setPlaceholderText(
        QStringLiteral("optional VPS hostname or address"));
    cloudflareForm->addRow(QStringLiteral("First mirror VPS"),
                           m_cloudflareVpsHostEdit);
    m_cloudflareVpsUserEdit = new QLineEdit;
    m_cloudflareVpsUserEdit->setObjectName(
        QStringLiteral("cloudflareFirstMirrorUser"));
    m_cloudflareVpsUserEdit->setPlaceholderText(
        QStringLiteral("dedicated non-root SSH user"));
    cloudflareForm->addRow(QStringLiteral("VPS SSH user"),
                           m_cloudflareVpsUserEdit);
    m_cloudflareVpsPasswordEdit = new QLineEdit;
    m_cloudflareVpsPasswordEdit->setObjectName(
        QStringLiteral("cloudflareFirstMirrorPassword"));
    m_cloudflareVpsPasswordEdit->setEchoMode(QLineEdit::Password);
    m_cloudflareVpsPasswordEdit->setPlaceholderText(
        QStringLiteral("optional; local SSH key / agent preferred"));
    m_cloudflareVpsPasswordEdit->setToolTip(
        QStringLiteral(
            "Session-only. Supplied to local SSH tooling through environment/"
            "stdin, never argv, settings, D1, or the Worker."));
    cloudflareForm->addRow(QStringLiteral("VPS SSH password"),
                           m_cloudflareVpsPasswordEdit);
    cloudflareCol->addLayout(cloudflareForm);

    m_cloudflareConnectCheck =
        new QCheckBox(QStringLiteral("Add and connect to the relay after deployment"));
    m_cloudflareConnectCheck->setChecked(true);
    cloudflareCol->addWidget(m_cloudflareConnectCheck);
    m_cloudflareInstallVpsCheck = new QCheckBox(
        QStringLiteral(
            "Install ForkMesh on the first mirror VPS after deployment"));
    m_cloudflareInstallVpsCheck->setChecked(false);
    m_cloudflareInstallVpsCheck->setToolTip(
        QStringLiteral(
            "Adds the host locally, runs the checksum-verified installer over "
            "SSH, and starts the managed mirror node against the newly selected "
            "relay. The desktop remains a client and never hosts repositories."));
    cloudflareCol->addWidget(m_cloudflareInstallVpsCheck);
    auto *deployRow = new QHBoxLayout;
    m_cloudflareDryRunButton =
        new QPushButton(QStringLiteral("Validate Worker + mirror"));
    m_cloudflareDryRunButton->setObjectName(
        QStringLiteral("cloudflareDryRunButton"));
    m_cloudflareDryRunButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_cloudflareDryRunButton, QStringLiteral("shield-check"), 14);
    connect(m_cloudflareDryRunButton, &QPushButton::clicked, this,
            [this] { runCloudflareBootstrap(true); });
    deployRow->addWidget(m_cloudflareDryRunButton);
    m_cloudflareDeployButton =
        new QPushButton(QStringLiteral("Deploy Worker + mirror"));
    m_cloudflareDeployButton->setObjectName(
        QStringLiteral("cloudflareDeployButton"));
    m_cloudflareDeployButton->setProperty("buttonSize", "primary");
    m_cloudflareDeployButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_cloudflareDeployButton, QStringLiteral("rocket"), 14);
    connect(m_cloudflareDeployButton, &QPushButton::clicked, this,
            [this] { runCloudflareBootstrap(false); });
    deployRow->addWidget(m_cloudflareDeployButton);
    m_cloudflareCancelButton = new QPushButton(QStringLiteral("Cancel"));
    m_cloudflareCancelButton->setObjectName(
        QStringLiteral("cloudflareCancelButton"));
    m_cloudflareCancelButton->setCursor(Qt::PointingHandCursor);
    m_cloudflareCancelButton->setEnabled(false);
    connect(m_cloudflareCancelButton, &QPushButton::clicked, this,
            &MainWindow::cancelCloudflareBootstrap);
    deployRow->addWidget(m_cloudflareCancelButton);
    deployRow->addStretch();
    cloudflareCol->addLayout(deployRow);

    m_controlNodeOutput = new QPlainTextEdit;
    m_controlNodeOutput->setObjectName(
        QStringLiteral("controlNodeDeploymentOutput"));
    m_controlNodeOutput->setReadOnly(true);
    m_controlNodeOutput->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_controlNodeOutput->setMinimumHeight(150);
    m_controlNodeOutput->document()->setMaximumBlockCount(1000);
    m_controlNodeOutput->setPlaceholderText(
        QStringLiteral("Local deployment output appears here; credentials are redacted."));
    applyLogFont(m_controlNodeOutput);
    cloudflareCol->addWidget(m_controlNodeOutput);
    if (next) {
        cloudflareCard->setProperty("controlNextCard", true);
        cloudflareCol->setContentsMargins(16, 16, 16, 16);
    }
    m_controlCloudflareTabIndex = tabs->count();
    addTab(cloudflareCard, QStringLiteral("Cloudflare relay"),
           QStringLiteral("Relay"), QStringLiteral("cloud"));

    addTab(buildCloudflareTokenCard(), QStringLiteral("API token"),
           QStringLiteral("Token"), QStringLiteral("shield-check"));

    connect(tabGroup, &QButtonGroup::idClicked, tabs,
            [tabs](int index) { tabs->setCurrentIndex(index); });

    const QString privacy = QStringLiteral(
        "This desktop is the local control plane. Repository bytes, identity "
        "keys, wallet ownership, Cloudflare credentials, and host credentials "
        "remain on devices you control; the relay receives only signed public "
        "metadata and ordinary repository traffic.");
    auto *helpTile = new VerticalIconButton(QStringLiteral("Help"),
                                            VerticalIconButton::Tab);
    helpTile->setObjectName(QStringLiteral("controlNodeHelpToggle"));
    helpTile->setCheckable(true);
    helpTile->setToolTip(privacy);
    helpTile->setOcticonName(QStringLiteral("info"));
    tabTiles.append(helpTile);
    tabRow->addStretch();
    tabRow->addWidget(helpTile);

    int tileWidth = railItemWidth();
    for (VerticalIconButton *tile : std::as_const(tabTiles))
        tileWidth = qMax(tileWidth, tile->sizeHint().width());
    for (VerticalIconButton *tile : std::as_const(tabTiles))
        tile->setFixedWidth(tileWidth);

    QPointer<QLabel> relayDot;
    QPointer<QLabel> relayLabel;
    if (next) {
        helpTile->setProperty(kControlTabIcon, QStringLiteral("info"));
        auto *head = new QHBoxLayout;
        head->setContentsMargins(24, 14, 24, 4);
        head->setSpacing(12);
        auto *titleCol = new QVBoxLayout;
        titleCol->setContentsMargins(0, 0, 0, 0);
        titleCol->setSpacing(2);
        auto *title = new QLabel(QStringLiteral("Control"));
        title->setObjectName(QStringLiteral("controlNextTitle"));
        titleCol->addWidget(title);
        auto *lede = new QLabel(QStringLiteral(
            "Deploy, mirrors, keys and the relay for the fleet that serves "
            "forkmesh.com"));
        lede->setObjectName(QStringLiteral("controlNextLede"));
        lede->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        titleCol->addWidget(lede);
        head->addLayout(titleCol, 1);
        auto *relayRow = new QHBoxLayout;
        relayRow->setContentsMargins(0, 0, 0, 0);
        relayRow->setSpacing(6);
        relayDot = new QLabel;
        relayDot->setObjectName(QStringLiteral("controlNextRelayDot"));
        relayDot->setFixedSize(6, 6);
        relayRow->addWidget(relayDot, 0, Qt::AlignVCenter);
        relayLabel = new QLabel;
        relayLabel->setObjectName(QStringLiteral("controlNextRelayStatus"));
        relayLabel->setTextFormat(Qt::PlainText);
        relayRow->addWidget(relayLabel, 0, Qt::AlignVCenter);
        head->addLayout(relayRow);
        outer->addLayout(head);

        for (VerticalIconButton *tile : std::as_const(tabTiles))
            tabRow->removeWidget(tile);
        while (QLayoutItem *item = tabRow->takeAt(0))
            delete item;
        tabRow->setContentsMargins(14, 4, 24, 0);
        tabRow->setSpacing(0);
        QList<VerticalIconButton *> ordered;
        if (!tabTiles.isEmpty())
            ordered.append(tabTiles.first());
        if (mirrorsTabIndex > 0 && mirrorsTabIndex < tabTiles.size())
            ordered.append(tabTiles.at(mirrorsTabIndex));
        for (VerticalIconButton *tile : std::as_const(tabTiles)) {
            if (!ordered.contains(tile))
                ordered.append(tile);
        }
        auto *styler = new ControlTabStyler(page);
        for (VerticalIconButton *tile : std::as_const(ordered)) {
            tabRow->addWidget(tile);
            styler->adopt(tile, tile->property(kControlTabIcon).toString());
        }
        tabRow->addStretch();
    }

    outer->addLayout(tabRow);
    if (next) {
        auto *rule = new QFrame;
        rule->setObjectName(QStringLiteral("controlNextHeaderRule"));
        rule->setFrameShape(QFrame::NoFrame);
        rule->setFixedHeight(1);
        outer->addWidget(rule);
    }
    QLabel *privacyHint = controlHint(privacy);
    privacyHint->setObjectName(QStringLiteral("controlNodePrivacyHint"));
    if (next)
        privacyHint->setContentsMargins(24, 12, 24, 0);
    outer->addWidget(privacyHint);
    tabs->tabBar()->hide();
    outer->addWidget(tabs, 1);

    QPointer<QWidget> pageGuard(page);
    const auto applyHelpVisibility = [pageGuard](bool visible) {
        if (!pageGuard)
            return;
        const auto labels = pageGuard->findChildren<QLabel *>();
        for (QLabel *label : labels) {
            if (label->property("fmControlHelp").toBool())
                label->setVisible(visible);
        }
    };
    connect(helpTile, &QPushButton::toggled, page, applyHelpVisibility);
    applyHelpVisibility(false);

    m_controlNodeRefreshTimer = new QTimer(page);
    m_controlNodeRefreshTimer->setInterval(10000);
    connect(m_controlNodeRefreshTimer, &QTimer::timeout, this, [this] {
        if (m_sectionStack &&
            m_sectionStack->currentIndex() == kControlNodeSectionIndex) {
            refreshControlNode();
        }
    });
    m_controlNodeRefreshTimer->start();
    if (next) {
        const auto updateRelay = [this, relayDot, relayLabel] {
            if (!relayDot || !relayLabel)
                return;
            const bool connected = m_backend != nullptr;
            const QString tone =
                connected ? QStringLiteral("success") : QStringLiteral("warning");
            relayLabel->setText(connected ? QStringLiteral("Relay connected")
                                          : QStringLiteral("Relay offline"));
            if (m_controlNodeHealth && !m_controlNodeHealth->text().isEmpty())
                relayLabel->setToolTip(m_controlNodeHealth->text());
            if (relayDot->property("tone").toString() != tone) {
                relayDot->setProperty("tone", tone);
                relayDot->style()->unpolish(relayDot);
                relayDot->style()->polish(relayDot);
            }
        };
        connect(m_controlNodeRefreshTimer, &QTimer::timeout, relayLabel.data(), updateRelay);
        updateRelay();
        polishControlNextChrome(page);
    }
    return page;
}

void MainWindow::openCloudflareSetupFromSystemLink(const QString &target)
{
    showSection(kControlNodeSectionIndex);
    if (m_controlNodeTabs && m_controlCloudflareTabIndex >= 0)
        m_controlNodeTabs->setCurrentIndex(m_controlCloudflareTabIndex);
    const QUrl url(target);
    if (url.isValid() && url.scheme() == QLatin1String("forkmesh") &&
        url.host() == QLatin1String("control") &&
        url.path() == QLatin1String("/cloudflare") &&
        url.userInfo().isEmpty() && url.fragment().isEmpty()) {
        const QUrlQuery query(url);
        const auto bounded = [&query](const QString &name, qsizetype maximum) {
            const QString value =
                query.queryItemValue(name, QUrl::FullyDecoded).trimmed();
            return value.size() <= maximum && !value.contains(QChar(u'\0')) &&
                           !value.contains(QLatin1Char('\n')) &&
                           !value.contains(QLatin1Char('\r'))
                       ? value
                       : QString();
        };
        if (bounded(QStringLiteral("mode"), 16) ==
            QLatin1String("vultr")) {
            const QString token =
                QApplication::clipboard()->text().trimmed();
            const QString node =
                bounded(QStringLiteral("node"), 63).toLower();
            static const QRegularExpression tokenPattern(
                QStringLiteral("^[A-Za-z0-9]{20,128}$"));
            static const QRegularExpression nodePattern(
                QStringLiteral("^(?:[a-z][a-z0-9-]{0,62})?$"));
            if (tokenPattern.match(token).hasMatch() &&
                nodePattern.match(node).hasMatch()) {
                if (m_vultrApiKeyEdit)
                    m_vultrApiKeyEdit->setText(token);
                if (m_vultrNameEdit && !node.isEmpty())
                    m_vultrNameEdit->setText(node);
                if (QApplication::clipboard()->text() == token)
                    QApplication::clipboard()->clear();
                showNetworkTab(kNetworkHostsTab);
                if (m_vultrStatus)
                    m_vultrStatus->setText(QStringLiteral(
                        "Filled in from the World. Check that the Vultr token "
                        "is your own, then click Create Vultr mirror to "
                        "deploy it."));
                if (m_vultrCreateButton)
                    m_vultrCreateButton->setFocus(Qt::OtherFocusReason);
            } else if (m_vultrStatus) {
                m_vultrStatus->setText(QStringLiteral(
                    "The World launch handoff did not contain a valid Vultr "
                    "token. Return to the LAUNCH MIRROR button and try again."));
            }
            return;
        }
        if (bounded(QStringLiteral("mode"), 16) == QLatin1String("ssh")) {
            const QString host = bounded(QStringLiteral("vpsHost"), 253);
            const QString user = bounded(QStringLiteral("vpsUser"), 64);
            const QString node =
                bounded(QStringLiteral("node"), 63).toLower();
            static const QRegularExpression hostPattern(
                QStringLiteral(
                    "^(?:[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?|"
                    "(?:[0-9]{1,3}\\.){3}[0-9]{1,3})$"));
            static const QRegularExpression userPattern(
                QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]{0,63}$"));
            static const QRegularExpression nodePattern(
                QStringLiteral("^(?:[a-z][a-z0-9-]{0,62})?$"));
            showNetworkTab(kNetworkHostsTab);
            if (!hostPattern.match(host).hasMatch() ||
                !userPattern.match(user).hasMatch() ||
                !nodePattern.match(node).hasMatch()) {
                if (m_hostInstallStatus)
                    m_hostInstallStatus->setText(QStringLiteral(
                        "The World handoff did not contain a valid server "
                        "address and SSH user. Enter them here instead."));
                return;
            }
            if (m_hostIpEdit)
                m_hostIpEdit->setText(host);
            if (m_hostUserEdit)
                m_hostUserEdit->setText(user);
            if (m_hostPassEdit)
                m_hostPassEdit->clear();
            const forkmesh::runners::RunnerSetupHandoff runner =
                forkmesh::runners::parseRunnerSetupHandoff(
                    bounded(QStringLiteral("role"), 16),
                    bounded(QStringLiteral("runnerToken"), 80),
                    bounded(QStringLiteral("node"), 63),
                    bounded(QStringLiteral("app"), 200));
            m_hostRunnerToken = runner.token;
            m_hostRunnerName = runner.name;
            m_hostRunnerApp = runner.appUrl;
            if (runner.isValid()) {
                if (m_hostNameEdit)
                    m_hostNameEdit->setText(runner.name);
                if (m_hostInstallStatus)
                    m_hostInstallStatus->setText(QStringLiteral(
                        "Filled in from the dashboard. Click Install ForkMesh "
                        "to set up the runner \"%1\" over SSH from this "
                        "computer. Your password or key stays here; the "
                        "one-time token works once, for 30 minutes.")
                        .arg(runner.name));
                if (m_hostPassEdit)
                    m_hostPassEdit->setFocus(Qt::OtherFocusReason);
                return;
            }
            if (m_hostNameEdit && !node.isEmpty())
                m_hostNameEdit->setText(node);
            if (m_hostInstallStatus)
                m_hostInstallStatus->setText(QStringLiteral(
                    "Filled in from the World. Click Install ForkMesh to "
                    "sign in with your SSH key or agent and start your "
                    "world on this server."));
            if (m_hostNameEdit && m_hostNameEdit->text().trimmed().isEmpty())
                m_hostNameEdit->setFocus(Qt::OtherFocusReason);
            else if (m_hostInstallButton)
                m_hostInstallButton->setFocus(Qt::OtherFocusReason);
            return;
        }
        const auto dns = [&bounded](const QString &name) {
            const QString value = bounded(name, 253).toLower();
            static const QRegularExpression pattern(
                QStringLiteral(
                    "^(?=.{4,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}"
                    "[a-z0-9])?\\.)+[a-z](?:[a-z0-9-]{0,61}"
                    "[a-z0-9])?$"));
            return pattern.match(value).hasMatch() ? value : QString();
        };
        const auto assign = [](QLineEdit *edit, const QString &value) {
            if (edit && !value.isEmpty())
                edit->setText(value);
        };
        assign(m_cloudflareHostnameEdit, dns(QStringLiteral("hostname")));
        assign(m_cloudflareMirrorHostnameEdit, dns(QStringLiteral("mirror")));
        assign(m_cloudflareZoneEdit, dns(QStringLiteral("zone")));
        const QString account = bounded(QStringLiteral("account"), 128);
        static const QRegularExpression accountPattern(
            QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
        if (accountPattern.match(account).hasMatch())
            assign(m_cloudflareAccountEdit, account);
        const QString node = bounded(QStringLiteral("node"), 63).toLower();
        static const QRegularExpression nodePattern(
            QStringLiteral("^[a-z][a-z0-9-]{0,62}$"));
        if (nodePattern.match(node).hasMatch())
            assign(m_cloudflareNodeNameEdit, node);
        assign(m_cloudflareRelayLabelEdit,
               bounded(QStringLiteral("label"), 80));
        const QString upstream = bounded(QStringLiteral("upstream"), 300);
        const QUrl upstreamUrl(upstream);
        if (upstreamUrl.isValid() &&
            upstreamUrl.scheme() == QLatin1String("https") &&
            !upstreamUrl.host().isEmpty() && upstreamUrl.userInfo().isEmpty())
            assign(m_cloudflareMainRelayEdit, upstream);
        const QString vpsHost = bounded(QStringLiteral("vpsHost"), 253);
        static const QRegularExpression vpsHostPattern(
            QStringLiteral(
                "^(?:[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?|"
                "(?:[0-9]{1,3}\\.){3}[0-9]{1,3})$"));
        if (vpsHostPattern.match(vpsHost).hasMatch()) {
            assign(m_cloudflareVpsHostEdit, vpsHost);
            if (m_cloudflareInstallVpsCheck)
                m_cloudflareInstallVpsCheck->setChecked(true);
        }
        const QString vpsUser = bounded(QStringLiteral("vpsUser"), 64);
        static const QRegularExpression vpsUserPattern(
            QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]{0,63}$"));
        if (vpsUserPattern.match(vpsUser).hasMatch())
            assign(m_cloudflareVpsUserEdit, vpsUser);
    }
    if (m_cloudflareTokenEdit)
        m_cloudflareTokenEdit->setFocus(Qt::OtherFocusReason);
}

void MainWindow::installCloudflareFirstMirror()
{
    if (!m_cloudflareInstallVpsAfterDeploy)
        return;
    m_cloudflareInstallVpsAfterDeploy = false;
    const QString host =
        m_cloudflareVpsHostEdit
            ? m_cloudflareVpsHostEdit->text().trimmed()
            : QString();
    const QString user =
        m_cloudflareVpsUserEdit
            ? m_cloudflareVpsUserEdit->text().trimmed()
            : QString();
    const QString password =
        m_cloudflareVpsPasswordEdit
            ? m_cloudflareVpsPasswordEdit->text()
            : QString();
    const QString node =
        m_cloudflareNodeNameEdit
            ? m_cloudflareNodeNameEdit->text().trimmed()
            : QString();
    if (host.isEmpty() || user.isEmpty() || node.isEmpty()) {
        appendControlNodeOutput(
            QStringLiteral(
                "First mirror VPS install skipped because its local SSH "
                "fields are incomplete.\n"));
        return;
    }
    if (m_hostIpEdit)
        m_hostIpEdit->setText(host);
    if (m_hostUserEdit)
        m_hostUserEdit->setText(user);
    if (m_hostPassEdit)
        m_hostPassEdit->setText(password);
    if (m_hostNameEdit)
        m_hostNameEdit->setText(node);
    if (m_cloudflareVpsPasswordEdit)
        m_cloudflareVpsPasswordEdit->clear();
    addHostFromForm();
    appendControlNodeOutput(
        QStringLiteral(
            "Starting the checksum-verified ForkMesh install on first mirror "
            "%1@%2. SSH credentials remain in local process memory only.\n")
            .arg(user, host));
    runHostInstall(false, [this, node](bool ok) {
        appendControlNodeOutput(
            ok
                ? QStringLiteral(
                      "First mirror %1 installed. Its managed service will "
                      "register signed health and mirror availability with "
                      "the selected relay.\n")
                      .arg(node)
                : QStringLiteral(
                      "First mirror %1 did not install; review the local Hosts "
                      "log and retry without redeploying the Worker.\n")
                      .arg(node));
    });
}

void MainWindow::refreshControlNode()
{
    refreshControlMirrorReadiness();

    int tracked = 0;
    int mirrors = 0;
    int missing = 0;
    int checking = 0;
    int serving = 0;
    QHash<QString, int> readiness;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly)
            continue;
        ++tracked;
        const QString key = controlMirrorReadyKey(repo);
        const auto cached = m_controlMirrorReadyCache.constFind(key);
        const int state =
            key.isEmpty() ? 0
                          : (cached == m_controlMirrorReadyCache.constEnd()
                                 ? -1
                                 : (*cached ? 1 : 0));
        readiness.insert(repoControlKey(repo), state);
        if (state > 0) {
            ++mirrors;
        } else if (state < 0) {
            ++checking;
        } else {
            ++missing;
        }
        if (repo.publishToNetwork)
            ++serving;
    }
    if (m_controlNodeStatus) {
        m_controlNodeStatus->setText(
            QStringLiteral(
                "<b>Desktop client</b> · relay %1 · "
                "%2/%3 encrypted local mirror(s) · "
                "%4 configured for managed nodes · %5 sync job(s)%6")
                .arg(m_backend ? QStringLiteral("connected")
                               : QStringLiteral("offline"))
                .arg(mirrors)
                .arg(tracked)
                .arg(serving)
                .arg(m_syncingRepos.size())
                .arg(checking > 0
                         ? QStringLiteral(" · %1 readiness check(s) running")
                               .arg(checking)
                         : QString()));
        m_controlNodeStatus->setTextFormat(Qt::RichText);
    }
    if (m_controlNodeHealth) {
        m_controlNodeHealth->setText(
            QStringLiteral("Relay: %1 · encrypted mirrors unavailable: %2 · "
                           "desktop hosting disabled · last checked %3")
                .arg(m_backend ? QStringLiteral("connected")
                               : QStringLiteral("offline"))
                .arg(missing)
                .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))));
    }

    if (m_controlIdentityStatus) {
        const bool valid =
            m_profileIdentity.isValid();
        m_controlIdentityStatus->setText(
            valid
                ? QStringLiteral(
                      "Identity key: loaded locally · public node ID: %1 · "
                      "private key is owner-readable on this device only.")
                      .arg(m_profileIdentity.publicKey())
                : QStringLiteral(
                      "Identity key: not loaded yet. Run the health check or "
                      "connect this node to load/create the owner-only local key."));
    }
    const QString wallet = savedSolanaAddress().trimmed();
    if (m_controlWalletEdit && !m_controlWalletEdit->hasFocus() &&
        m_controlWalletEdit->text() != wallet) {
        m_controlWalletEdit->setText(wallet);
    }
    if (m_controlWalletStatus) {
        m_controlWalletStatus->setText(
            wallet.isEmpty()
                ? QStringLiteral(
                      "No payout address connected. Funds and keys are never "
                      "created or held by ForkMesh.")
                : QStringLiteral(
                      "Connected public payout address. Non-custodial: signing "
                      "and private wallet keys remain in your external wallet."));
    }
    refreshRewardPoolControls();

    if (!m_controlPermissionsTable || m_controlRefreshingPermissions)
        return;
    QString permissionsSignature;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly)
            continue;
        permissionsSignature +=
            repoControlKey(repo) + QLatin1Char('|') +
            QString::number(readiness.value(repoControlKey(repo), -1)) +
            QLatin1Char('|') + QString::number(repo.isPrivate) +
            QLatin1Char('|') + QString::number(repo.publishToNetwork) +
            QLatin1Char('|') + QString::number(repo.actionsEnabled) +
            QLatin1Char('|') + QString::number(repo.secretScanningEnabled) +
            QLatin1Char('|') + QString::number(repo.lastSyncMs) +
            QLatin1Char('\n');
    }
    if (permissionsSignature == m_controlPermissionsSignature)
        return;
    m_controlPermissionsSignature = permissionsSignature;
    m_controlRefreshingPermissions = true;
    m_controlPermissionsTable->setRowCount(0);
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly)
            continue;
        const int row = m_controlPermissionsTable->rowCount();
        m_controlPermissionsTable->insertRow(row);
        const QString key = repoControlKey(repo);
        auto *name = new QTableWidgetItem(key);
        name->setData(Qt::UserRole, key);
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        m_controlPermissionsTable->setItem(row, 0, name);
        const int mirrorState = readiness.value(key, -1);
        auto *mirror = new QTableWidgetItem(
            mirrorState > 0
                ? QStringLiteral("Ready")
                : (mirrorState < 0 ? QStringLiteral("Checking…")
                                   : QStringLiteral("Missing")));
        mirror->setFlags(mirror->flags() & ~Qt::ItemIsEditable);
        mirror->setToolTip(
            mirrorState > 0
                ? QStringLiteral(
                      "Authenticated encrypted mirror storage is ready. "
                      "Plaintext exists only in owner-only runtime storage.")
                : (mirrorState < 0
                       ? QStringLiteral(
                             "Authenticating encrypted mirror storage on a "
                             "background worker.")
                       : QStringLiteral(
                             "No authenticated encrypted mirror archive is "
                             "ready.")));
        m_controlPermissionsTable->setItem(row, 1, mirror);

        auto addToggle = [this, row, &key](int column, const QString &permission,
                                           bool checked, bool enabled,
                                           const QString &tooltip) {
            auto *item = new QTableWidgetItem;
            item->setData(Qt::UserRole, key);
            item->setData(Qt::UserRole + 1, permission);
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsUserCheckable |
                                  Qt::ItemIsEnabled;
            if (!enabled)
                flags &= ~Qt::ItemIsEnabled;
            item->setFlags(flags);
            item->setToolTip(tooltip);
            m_controlPermissionsTable->setItem(row, column, item);
        };
        const bool owner =
            hasOwnerSigningCapability(catalogOwner(repo));
        addToggle(2, QStringLiteral("private"), repo.isPrivate, owner,
                  QStringLiteral("Hide from public discovery and require authorized access"));
        addToggle(3, QStringLiteral("serve"), repo.publishToNetwork, owner,
                  QStringLiteral("Serve this mirror through the active relay"));
        addToggle(4, QStringLiteral("actions"), repo.actionsEnabled, true,
                  QStringLiteral("Allow trusted local .forkmesh workflows to run"));
        addToggle(5, QStringLiteral("secret-scan"),
                  repo.secretScanningEnabled, true,
                  QStringLiteral("Block pushes when high-confidence secrets are detected"));
        auto *last = new QTableWidgetItem(
            repo.lastSyncMs > 0
                ? QDateTime::fromMSecsSinceEpoch(repo.lastSyncMs)
                      .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                : QString::fromUtf8("\xE2\x80\x94"));
        last->setFlags(last->flags() & ~Qt::ItemIsEditable);
        m_controlPermissionsTable->setItem(row, 6, last);
    }
    m_controlRefreshingPermissions = false;
}

void MainWindow::refreshControlMirrorReadiness()
{
    if (m_controlMirrorProbeInFlight)
        return;

    struct Probe {
        QString key;
        RepositoryRecord repo;
    };
    QList<Probe> probes;
    bool hasUnknown = false;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly)
            continue;
        const QString key = controlMirrorReadyKey(repo);
        if (key.isEmpty())
            continue;
        probes.append({key, repo});
        hasUnknown = hasUnknown || !m_controlMirrorReadyCache.contains(key);
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kReadinessRefreshMs = 5 * 60 * 1000;
    if (!hasUnknown && m_controlMirrorProbeCompletedAtMs > 0 &&
        now - m_controlMirrorProbeCompletedAtMs < kReadinessRefreshMs) {
        return;
    }
    if (probes.isEmpty()) {
        m_controlMirrorReadyCache.clear();
        m_controlMirrorProbeCompletedAtMs = now;
        return;
    }

    m_controlMirrorProbeInFlight = true;
    auto result = std::make_shared<QHash<QString, bool>>();
    QThread *worker = QThread::create([probes, result] {
        const forkmesh::BackgroundScope activity(
            QStringLiteral("mirrors"),
            QStringLiteral("validate %1 mirror(s)")
                .arg(probes.size()),
            forkmesh::ActionTelemetry::Execution::Worker);
        for (const Probe &probe : probes)
            result->insert(probe.key, controlMirrorReady(probe.repo));
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, result] {
        m_controlMirrorProbeInFlight = false;
        m_controlMirrorProbeCompletedAtMs =
            QDateTime::currentMSecsSinceEpoch();
        if (m_controlMirrorReadyCache != *result) {
            m_controlMirrorReadyCache = *result;
            m_controlPermissionsSignature.clear();
        }
        refreshControlNode();
    });
    worker->start();
}

void MainWindow::runControlNodeHealthCheck()
{
    QStringList results;
    if (m_profileIdentity.isValid() || m_profileIdentity.load()) {
        results << QStringLiteral("identity signature key OK");
    } else {
        results << QStringLiteral("identity key unavailable");
    }
    results << (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()
                    ? QStringLiteral("git missing")
                    : QStringLiteral("git OK"));
    results <<
        (forkmesh::control::findCloudflareBootstrapScript(
             QStringLiteral(FORKMESH_SOURCE_DIR),
             QCoreApplication::applicationDirPath())
                 .isEmpty()
             ? QStringLiteral("Cloudflare bootstrap source missing")
             : QStringLiteral("Cloudflare bootstrap OK"));
    results <<
        (forkmesh::control::findCloudflareTunnelBootstrapScript(
             QStringLiteral(FORKMESH_SOURCE_DIR),
             QCoreApplication::applicationDirPath())
                 .isEmpty()
             ? QStringLiteral("Tunnel bootstrap missing")
             : QStringLiteral("Tunnel bootstrap OK"));
    results <<
        (forkmesh::control::findMirrorGatewayScript(
             QStringLiteral(FORKMESH_SOURCE_DIR),
             QCoreApplication::applicationDirPath())
                 .isEmpty()
             ? QStringLiteral("mirror gateway missing")
             : QStringLiteral("mirror gateway OK"));
    results <<
        (forkmesh::control::findCloudflaredInstallerScript(
             QStringLiteral(FORKMESH_SOURCE_DIR),
             QCoreApplication::applicationDirPath())
                 .isEmpty()
             ? QStringLiteral("verified cloudflared installer missing")
             : QStringLiteral("verified cloudflared installer OK"));
    refreshControlMirrorReadiness();
    int unhealthy = 0;
    int checking = 0;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly)
            continue;
        const QString key = controlMirrorReadyKey(repo);
        const auto cached = m_controlMirrorReadyCache.constFind(key);
        if (!key.isEmpty() &&
            cached == m_controlMirrorReadyCache.constEnd()) {
            ++checking;
        } else if (key.isEmpty() ||
                   cached == m_controlMirrorReadyCache.constEnd() ||
                   !*cached) {
            ++unhealthy;
        }
    }
    results << QStringLiteral("%1 missing mirror path(s)").arg(unhealthy);
    if (checking > 0) {
        results << QStringLiteral("%1 mirror check(s) running in background")
                       .arg(checking);
    }
    results << (m_backend ? QStringLiteral("relay connected")
                          : QStringLiteral("relay offline"));
    if (m_controlNodeHealth)
        m_controlNodeHealth->setText(results.join(QStringLiteral(" · ")));
    appendControlNodeOutput(
        QStringLiteral("Health check: %1\n").arg(results.join(QStringLiteral("; "))));
    logSystem(QStringLiteral(
        "Control node: local health check completed (%1 issue(s)).")
                  .arg(unhealthy));
    refreshControlNode();
}

void MainWindow::syncControlNodeMirrors()
{
    appendControlNodeOutput(
        QStringLiteral("Sync requested for every tracked local mirror.\n"));
    logSystem(QStringLiteral("Control node: syncing all mirrors."));
    autoSyncMirrors();
    refreshControlNode();
}

void MainWindow::updateControlRepositoryPermission(QTableWidgetItem *item)
{
    if (!item || m_controlRefreshingPermissions)
        return;
    const QString key = item->data(Qt::UserRole).toString();
    const QString permission = item->data(Qt::UserRole + 1).toString();
    if (key.isEmpty() || permission.isEmpty())
        return;
    int index = -1;
    for (int i = 0; i < m_repositories.size(); ++i) {
        if (repoControlKey(m_repositories.at(i)) == key) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return;
    RepositoryRecord &repo = m_repositories[index];
    const bool enabled = item->checkState() == Qt::Checked;
    if ((permission == QLatin1String("private") ||
         permission == QLatin1String("serve")) &&
        !hasOwnerSigningCapability(catalogOwner(repo))) {
        flashMessage(
            QStringLiteral("Only the repository owner's signing identity can "
                           "change that permission."),
            true);
        QTimer::singleShot(0, this, &MainWindow::refreshControlNode);
        return;
    }

    QString action;
    bool visibilityChanged = false;
    if (permission == QLatin1String("private")) {
        if (repo.isPrivate == enabled)
            return;
        repo.isPrivate = enabled;
        visibilityChanged = true;
        action = enabled ? QStringLiteral("visibility set to private")
                         : QStringLiteral("visibility set to public");
    } else if (permission == QLatin1String("serve")) {
        // Shared with the "Serve on the network" checkbox in repo settings so
        // both surfaces apply the same transition. It saves, publishes or
        // drops the gateway route, and refreshes the list itself; `repo` must
        // not be touched afterwards because the sync pumps the event loop.
        const bool applied = setRepositoryNetworkServing(index, enabled);
        if (applied) {
            const QString serveAction =
                enabled ? QStringLiteral("network serving enabled")
                        : QStringLiteral("network serving disabled");
            logSystem(
                QStringLiteral("Control node: %1 for %2.").arg(serveAction, key));
            appendControlNodeOutput(
                QStringLiteral("Repository %1: %2.\n").arg(key, serveAction));
        }
        QTimer::singleShot(0, this, &MainWindow::refreshControlNode);
        return;
    } else if (permission == QLatin1String("actions")) {
        if (repo.actionsEnabled == enabled)
            return;
        repo.actionsEnabled = enabled;
        action = enabled ? QStringLiteral("local actions enabled")
                         : QStringLiteral("local actions disabled");
    } else if (permission == QLatin1String("secret-scan")) {
        if (repo.secretScanningEnabled == enabled)
            return;
        repo.secretScanningEnabled = enabled;
        action = enabled ? QStringLiteral("push secret scanning enabled")
                         : QStringLiteral("push secret scanning disabled");
    } else {
        return;
    }

    saveRepositories();
    if (visibilityChanged) {
        // Private transitions seal and authenticate before publishing; public
        // transitions rebuild a durable direct-HTTPS mirror. Neither path
        // exposes repository bytes through the persistent update channel.
        QString ignoredGatewayError;
        rebuildDirectMirrorGatewayConfiguration(
            &ignoredGatewayError, true);
        if (enabled)
            syncPrivateRepository(index, /*quiet=*/false);
        else
            syncRepository(index, /*quiet=*/false);
    }
    startNodeEventSocket();
    logSystem(QStringLiteral("Control node: %1 for %2.").arg(action, key));
    appendControlNodeOutput(
        QStringLiteral("Repository %1: %2.\n").arg(key, action));
    refreshRepositoryList();
    QTimer::singleShot(0, this, &MainWindow::refreshControlNode);
}

void MainWindow::saveControlWalletAddress()
{
    const QString address =
        m_controlWalletEdit ? m_controlWalletEdit->text().trimmed() : QString();
    if (!address.isEmpty() &&
        !forkmesh::control::isValidSolanaPublicAddress(address)) {
        if (m_controlWalletStatus) {
            m_controlWalletStatus->setText(
                QStringLiteral(
                    "Enter one valid Solana public address. Private keys, seed "
                    "phrases, and recovery phrases are never accepted."));
        }
        fileMessage(QStringLiteral("That is not a valid Solana public address."),
                    true);
        return;
    }
    saveSolanaAddress(address);
    if (m_settingsSolanaEdit)
        m_settingsSolanaEdit->setText(address);
    updateSolanaNotice();
    updateWalletVerifyNotice();
    updateNavSolanaBalance();
    logSystem(address.isEmpty()
                  ? QStringLiteral("Control node: public payout address disconnected.")
                  : QStringLiteral("Control node: public payout address updated."));
    appendControlNodeOutput(
        address.isEmpty()
            ? QStringLiteral("Public payout address disconnected.\n")
            : QStringLiteral(
                  "Public payout address connected; no wallet key was stored.\n"));
    refreshControlNode();
}

bool MainWindow::directMirrorGatewayConfigured() const
{
    return resolveDirectMirrorGatewayIdentity(nullptr, nullptr, nullptr);
}

bool MainWindow::resolveDirectMirrorGatewayIdentity(
    QString *nodeName, QString *hostname, QString *routerPublicKey) const
{
    QSettings settings;
    const QString node =
        settings.value(QStringLiteral("control/cloudflareNodeName"))
            .toString()
            .trimmed()
            .toLower();
    const QString host =
        settings.value(QStringLiteral("control/cloudflareMirrorHostname"))
            .toString().trimmed().toLower();
    const QString routerKey =
        settings.value(QStringLiteral("control/directMirrorRouterPublicKey"))
            .toString().trimmed();
    if (nodeName)
        *nodeName = node;
    if (hostname)
        *hostname = host;
    if (routerPublicKey)
        *routerPublicKey = routerKey;
    return forkmesh::control::directMirrorGatewayIsConfigured(
        node, normalizedHttpsOrigin(host), routerKey);
}

bool MainWindow::rebuildDirectMirrorGatewayConfiguration(
    QString *error, bool restartRunningGateway)
{
    if ((!m_profileIdentity.isValid() && !m_profileIdentity.load()) ||
        !m_profileIdentity.isValid()) {
        if (error)
            *error = QStringLiteral(
                "The local node identity is unavailable.");
        return false;
    }
    QSettings settings;
    QString node;
    QString hostname;
    QString routerKey;
    if (!resolveDirectMirrorGatewayIdentity(&node, &hostname, &routerKey)) {
        if (error) {
            *error = QStringLiteral(
                "The mirror hostname, node name, or Worker router public key "
                "is not configured.");
        }
        return false;
    }
    const QString origin = normalizedHttpsOrigin(hostname);
    const QString gatewayScript =
        forkmesh::control::findMirrorGatewayScript(
            QStringLiteral(FORKMESH_SOURCE_DIR),
            QCoreApplication::applicationDirPath());
    if (gatewayScript.isEmpty()) {
        if (error)
            *error = QStringLiteral(
                "The repository-pinned mirror gateway is unavailable.");
        return false;
    }

    QJsonArray repositories;
    static const QRegularExpression forkParentPattern(
        QStringLiteral("^[A-Za-z0-9._:-]{1,80}/[A-Za-z0-9._:-]{1,80}$"));
    static const QStringList operations{
        QStringLiteral("blob"), QStringLiteral("blobs"),
        QStringLiteral("branches"), QStringLiteral("commit"),
        QStringLiteral("compare"),
        QStringLiteral("git-info-refs"),
        QStringLiteral("git-upload-pack"),
        QStringLiteral("history"), QStringLiteral("raw"),
        QStringLiteral("release-blob"), QStringLiteral("search"),
        QStringLiteral("sizes"), QStringLiteral("stats"),
        QStringLiteral("tree")};
    for (const RepositoryRecord &repo :
         std::as_const(m_repositories)) {
        if (repo.previewOnly || repo.isPrivate ||
            !repo.publishToNetwork)
            continue;
        // The gateway answers the active relay's router. A repository added
        // under another relay is not that relay's to serve or list.
        if (!repositoryBelongsToActiveRelay(repo))
            continue;
        const QString refsSha256 =
            PublicMirrorRuntime::repositoryRefsSha256(
                repo.mirrorPath, PublicMirrorRuntime::Tools(), nullptr);
        if (refsSha256.isEmpty())
            continue;
        QJsonArray operationArray;
        for (const QString &operation : operations)
            operationArray.append(operation);
        QJsonObject repository{
            {QStringLiteral("name"),
             repoSegment(repo.name,
                         QStringLiteral("repository"))},
            {QStringLiteral("visibility"),
             QStringLiteral("public")},
            {QStringLiteral("enabled"), true},
            {QStringLiteral("integrity"),
             QJsonObject{
                 {QStringLiteral("expectedRefsSha256"),
                  refsSha256}}},
            {QStringLiteral("operations"), operationArray},
            {QStringLiteral("gitDir"), repo.mirrorPath},
            // `release-blob` is advertised above, so name the store those
            // bytes live in: without it the gateway serves no artifact at
            // all, and every download this node holds answers 404. Named
            // unconditionally — the CAS directory first appears when an
            // artifact is published or mirrored here, long after this config
            // is written.
            {QStringLiteral("releaseStore"),
             mirrorReleaseStoreRoot(repo.mirrorPath)},
        };
        const QString forkParent = repo.forkedFrom.trimmed().toLower();
        if (forkParentPattern.match(forkParent).hasMatch())
            repository.insert(QStringLiteral("forkedFrom"), forkParent);
        const QStringList owners =
            forkmesh::control::directMirrorRepositoryOwners(
                repoSegment(repo.owner, QStringLiteral("owner")),
                repoSegment(catalogOwner(repo), QStringLiteral("owner")));
        for (const QString &owner : owners) {
            repository.insert(QStringLiteral("owner"), owner);
            repositories.append(repository);
        }
    }
    QJsonObject config{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("node"),
         QJsonObject{{QStringLiteral("name"), node},
                     {QStringLiteral("publicKey"),
                      m_profileIdentity.publicKey()}}},
        {QStringLiteral("routerPublicKey"), routerKey},
        {QStringLiteral("publicOrigin"), origin},
        {QStringLiteral("listen"),
         QJsonObject{{QStringLiteral("host"),
                      QStringLiteral("127.0.0.1")},
                     {QStringLiteral("port"), 8790}}},
        {QStringLiteral("manifestPath"),
         directGatewayManifestPath()},
        {QStringLiteral("repositories"), repositories},
        {QStringLiteral("limits"),
         QJsonObject{
             {QStringLiteral("maxReleaseBytes"),
              double(2LL * 1024 * 1024 * 1024)},
             {QStringLiteral("maxPrivateReplicaBytes"),
              double(512LL * 1024 * 1024)}}},
    };
    const QString privateStore =
        forkmesh::paths::dataRoot() +
        QStringLiteral("/private-replicas");
    const QFileInfo privateStoreInfo(privateStore);
    if (privateStoreInfo.isDir() &&
        !privateStoreInfo.isSymLink()) {
        config.insert(QStringLiteral("privateReplicaStore"),
                      privateStoreInfo.absoluteFilePath());
    }
    if (!writeOwnerJson(directGatewayConfigPath(), config, error))
        return false;

    settings.setValue(
        QStringLiteral("control/cloudflareMirrorHostname"), hostname);
    settings.setValue(
        QStringLiteral("control/directMirrorRouterPublicKey"), routerKey);

    Q_UNUSED(restartRunningGateway);
    return true;
}

void MainWindow::provisionDirectMirrorEndpoint(bool dryRun)
{
    auto releaseDeploymentUi = [this] {
        m_cloudflareInstallVpsAfterDeploy = false;
        if (m_cloudflareDryRunButton)
            m_cloudflareDryRunButton->setEnabled(true);
        if (m_cloudflareDeployButton)
            m_cloudflareDeployButton->setEnabled(true);
        if (m_cloudflareCancelButton)
            m_cloudflareCancelButton->setEnabled(false);
        if (m_cloudflareTokenEdit)
            m_cloudflareTokenEdit->setPlaceholderText(
                QStringLiteral("session-only Cloudflare API token"));
        m_cloudflareActiveSecret.fill(QChar(u'\0'));
        m_cloudflareActiveSecret.clear();
    };
    if (m_cloudflareTunnelBootstrapProcess &&
        m_cloudflareTunnelBootstrapProcess->state() !=
            QProcess::NotRunning)
        return;
    if (!m_networkAccess && !dryRun) {
        flashMessage(
            QStringLiteral(
                "Network access is unavailable for mirror router discovery."),
            true);
        releaseDeploymentUi();
        return;
    }
    const QString mirrorHostname =
        m_cloudflareMirrorHostnameEdit
            ? m_cloudflareMirrorHostnameEdit->text()
                  .trimmed()
                  .toLower()
            : QString();
    forkmesh::control::CloudflareBootstrapRequest request;
    request.hostname = mirrorHostname;
    request.zoneName =
        m_cloudflareZoneEdit
            ? m_cloudflareZoneEdit->text()
            : QString();
    request.accountId =
        m_cloudflareAccountEdit
            ? m_cloudflareAccountEdit->text()
            : QString();
    request.nodeName =
        m_cloudflareNodeNameEdit
            ? m_cloudflareNodeNameEdit->text()
            : QString();
    request.mainRelayUrl =
        m_cloudflareMainRelayEdit
            ? m_cloudflareMainRelayEdit->text()
            : QString();
    request.dryRun = dryRun;
    const QString validation =
        forkmesh::control::
            validateCloudflareBootstrapRequest(request);
    if (!validation.isEmpty() ||
        mirrorHostname == m_cloudflareDeployHostname) {
        flashMessage(
            validation.isEmpty()
                ? QStringLiteral(
                      "The Worker relay and direct mirror endpoint require "
                      "different hostnames.")
                : validation,
            true);
        releaseDeploymentUi();
        return;
    }
    const QString tunnelScript =
        forkmesh::control::
            findCloudflareTunnelBootstrapScript(
                QStringLiteral(FORKMESH_SOURCE_DIR),
                QCoreApplication::applicationDirPath());
    QString python =
        QStandardPaths::findExecutable(
            QStringLiteral("python3"));
    if (python.isEmpty())
        python = QStandardPaths::findExecutable(
            QStringLiteral("python"));
    if (tunnelScript.isEmpty() || python.isEmpty() ||
        m_cloudflareActiveSecret.isEmpty()) {
        flashMessage(
            QStringLiteral(
                "The Tunnel bootstrap, Python 3, or session API token is "
                "unavailable."),
            true);
        releaseDeploymentUi();
        return;
    }

    auto launchBootstrap =
        [this, request, mirrorHostname, tunnelScript, python,
         dryRun, releaseDeploymentUi](const QString &routerKey) {
            if (!dryRun) {
                QSettings settings;
                settings.setValue(
                    QStringLiteral("control/directMirrorRouterPublicKey"),
                    routerKey);
                settings.setValue(
                    QStringLiteral("control/cloudflareMirrorHostname"),
                    mirrorHostname);
                QString configError;
                if (!rebuildDirectMirrorGatewayConfiguration(
                        &configError, false)) {
                    appendControlNodeOutput(
                        QStringLiteral(
                            "Direct mirror configuration failed: %1\n")
                            .arg(configError));
                    flashMessage(configError, true);
                    releaseDeploymentUi();
                    return;
                }
            }
            const auto command =
                forkmesh::control::
                    buildCloudflareTunnelBootstrapCommand(
                        request, mirrorHostname,
                        m_cloudflareActiveSecret,
                        tunnelScript, python,
                        QCoreApplication::applicationFilePath(),
                        m_profileIdentity.publicKey(),
                        directGatewayConfigPath(),
                        directGatewayManifestPath(),
                        directGatewayConnectorTokenPath());
            if (command.arguments.join(QChar(u'\0'))
                    .contains(
                        m_cloudflareActiveSecret)) {
                flashMessage(
                    QStringLiteral(
                        "Refusing an unsafe Tunnel command containing a "
                        "credential."),
                    true);
                releaseDeploymentUi();
                return;
            }
            auto *process = new QProcess(this);
            m_cloudflareTunnelBootstrapProcess = process;
            process->setProcessEnvironment(
                command.environment);
            process->setProcessChannelMode(
                QProcess::MergedChannels);
            connect(
                process,
                &QProcess::readyReadStandardOutput,
                this, [this, process] {
                    appendControlNodeOutput(
                        QString::fromUtf8(
                            process
                                ->readAllStandardOutput()));
                });
            connect(
                process, &QProcess::finished, this,
                [this, process, dryRun](
                    int exitCode,
                    QProcess::ExitStatus status) {
                    appendControlNodeOutput(
                        QString::fromUtf8(
                            process
                                ->readAllStandardOutput()));
                    const bool ok =
                        status ==
                            QProcess::NormalExit &&
                        exitCode == 0;
                    if (ok && !dryRun) {
                        appendControlNodeOutput(
                            QStringLiteral(
                                "Tunnel/DNS/manifest provisioning completed. "
                                "Install it on a managed mirror host from the "
                                "Hosts page.\n"));
                        if (m_cloudflareConnectAfterDeploy)
                            connectToDeployedRelay(
                                m_cloudflareDeployHostname);
                        installCloudflareFirstMirror();
                    } else if (ok) {
                        appendControlNodeOutput(
                            QStringLiteral(
                                "Worker and Tunnel validation completed.\n"));
                    } else {
                        m_cloudflareInstallVpsAfterDeploy = false;
                        appendControlNodeOutput(
                            QStringLiteral(
                                "Tunnel provisioning failed (exit %1).\n")
                                .arg(exitCode));
                        flashMessage(
                            QStringLiteral(
                                "Direct mirror Tunnel provisioning failed; "
                                "see the local output."),
                            true);
                    }
                    process->setProcessEnvironment(
                        QProcessEnvironment());
                    if (m_cloudflareTunnelBootstrapProcess ==
                        process) {
                        m_cloudflareTunnelBootstrapProcess =
                            nullptr;
                    }
                    process->deleteLater();
                    if (m_cloudflareDryRunButton)
                        m_cloudflareDryRunButton
                            ->setEnabled(true);
                    if (m_cloudflareDeployButton)
                        m_cloudflareDeployButton
                            ->setEnabled(true);
                    if (m_cloudflareCancelButton)
                        m_cloudflareCancelButton
                            ->setEnabled(false);
                    if (m_cloudflareTokenEdit)
                        m_cloudflareTokenEdit
                            ->setPlaceholderText(
                                QStringLiteral(
                                    "session-only Cloudflare API token"));
                    m_cloudflareActiveSecret.fill(
                        QChar(u'\0'));
                    m_cloudflareActiveSecret.clear();
                    refreshControlNode();
                });
            process->start(command.program,
                           command.arguments);
        };

    if (dryRun) {
        launchBootstrap(QString());
        return;
    }
    QUrl routerUrl;
    routerUrl.setScheme(QStringLiteral("https"));
    routerUrl.setHost(m_cloudflareDeployHostname);
    routerUrl.setPath(
        QStringLiteral("/api/mirrors/https"));
    QNetworkRequest routerRequest(routerUrl);
    routerRequest.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply *reply =
        m_networkAccess->get(routerRequest);
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, releaseDeploymentUi,
         launchBootstrap = std::move(launchBootstrap)]() mutable {
            const int status =
                reply
                    ->attribute(
                        QNetworkRequest::
                            HttpStatusCodeAttribute)
                    .toInt();
            const QJsonObject object =
                QJsonDocument::fromJson(
                    reply->readAll())
                    .object();
            const QString routerKey =
                object
                    .value(QStringLiteral(
                        "routerPublicKey"))
                    .toString();
            const bool ok =
                reply->error() ==
                    QNetworkReply::NoError &&
                status == 200 &&
                object.value(QStringLiteral("ok"))
                    .toBool() &&
                forkmesh::control::isValidMirrorRouterPublicKey(
                    routerKey);
            reply->deleteLater();
            if (!ok) {
                appendControlNodeOutput(
                    QStringLiteral(
                        "The deployed Worker has no valid mirror router "
                        "public key. Configure its dedicated "
                        "MIRROR_ROUTER_PUBLIC_KEY/signing seed before "
                        "registering endpoints.\n"));
                flashMessage(
                    QStringLiteral(
                        "Direct mirror routing is not enabled on the "
                        "deployed Worker."),
                    true);
                releaseDeploymentUi();
                return;
            }
            launchBootstrap(routerKey);
        });
}

void MainWindow::runCloudflareBootstrap(bool dryRun)
{
    if ((m_cloudflareBootstrapProcess &&
         m_cloudflareBootstrapProcess->state() != QProcess::NotRunning) ||
        (m_cloudflareTunnelBootstrapProcess &&
         m_cloudflareTunnelBootstrapProcess->state() !=
             QProcess::NotRunning)) {
        flashMessage(QStringLiteral("A Cloudflare deployment is already running."),
                     true);
        return;
    }
    forkmesh::control::CloudflareBootstrapRequest request;
    request.hostname =
        m_cloudflareHostnameEdit ? m_cloudflareHostnameEdit->text() : QString();
    request.zoneName =
        m_cloudflareZoneEdit ? m_cloudflareZoneEdit->text() : QString();
    request.accountId =
        m_cloudflareAccountEdit ? m_cloudflareAccountEdit->text() : QString();
    request.nodeName =
        m_cloudflareNodeNameEdit ? m_cloudflareNodeNameEdit->text() : QString();
    const QString mirrorHostname =
        m_cloudflareMirrorHostnameEdit
            ? m_cloudflareMirrorHostnameEdit->text().trimmed().toLower()
            : QString();
    const bool automaticTopology =
        request.hostname.trimmed().isEmpty() &&
        request.zoneName.trimmed().isEmpty() &&
        mirrorHostname.isEmpty();
    if (!automaticTopology && request.nodeName.trimmed().isEmpty()) {
        request.nodeName =
            mirrorHostname.section(QLatin1Char('.'), 0, 0);
        if (m_cloudflareNodeNameEdit)
            m_cloudflareNodeNameEdit->setText(request.nodeName);
    }
    request.relayLabel =
        m_cloudflareRelayLabelEdit ? m_cloudflareRelayLabelEdit->text() : QString();
    request.mainRelayUrl =
        m_cloudflareMainRelayEdit ? m_cloudflareMainRelayEdit->text() : QString();
    request.dryRun = dryRun;
    const QString requestError =
        forkmesh::control::validateCloudflareBootstrapRequest(
            request, automaticTopology);
    if (!requestError.isEmpty()) {
        flashMessage(requestError, true);
        return;
    }
    if (!automaticTopology) {
        forkmesh::control::CloudflareBootstrapRequest mirrorRequest =
            request;
        mirrorRequest.hostname = mirrorHostname;
        const QString mirrorRequestError =
            forkmesh::control::validateCloudflareBootstrapRequest(
                mirrorRequest);
        if (!mirrorRequestError.isEmpty() ||
            mirrorHostname ==
                request.hostname.trimmed().toLower()) {
            flashMessage(
                mirrorRequestError.isEmpty()
                    ? QStringLiteral(
                          "The relay hostname and direct mirror endpoint must "
                          "be different.")
                    : mirrorRequestError,
                true);
            return;
        }
    }
    const QString apiToken =
        m_cloudflareTokenEdit ? m_cloudflareTokenEdit->text().trimmed() : QString();
    if (apiToken.isEmpty()) {
        flashMessage(
            QStringLiteral("Enter a scoped Cloudflare API token for this run."),
            true);
        return;
    }
    const bool installFirstMirror =
        !dryRun && m_cloudflareInstallVpsCheck &&
        m_cloudflareInstallVpsCheck->isChecked();
    if (installFirstMirror &&
        ((!m_cloudflareVpsHostEdit ||
          m_cloudflareVpsHostEdit->text().trimmed().isEmpty()) ||
         (!m_cloudflareVpsUserEdit ||
          m_cloudflareVpsUserEdit->text().trimmed().isEmpty()))) {
        flashMessage(
            QStringLiteral(
                "Enter the first mirror VPS host and SSH user, or disable "
                "the post-deployment VPS install."),
            true);
        return;
    }
    if ((!m_profileIdentity.isValid() && !m_profileIdentity.load()) ||
        !m_profileIdentity.isValid()) {
        flashMessage(
            QStringLiteral(
                "The local identity key is unavailable, so ForkMesh cannot sign "
                "the mirror endpoint manifest."),
            true);
        return;
    }
    const QString script =
        forkmesh::control::findCloudflareBootstrapScript(
            QStringLiteral(FORKMESH_SOURCE_DIR),
            QCoreApplication::applicationDirPath());
    if (script.isEmpty()) {
        flashMessage(
            QStringLiteral(
                "The bundled Cloudflare bootstrap or its complete Worker "
                "source/static/migration resource tree could not be found."),
            true);
        return;
    }
    QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
        python = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (python.isEmpty()) {
        flashMessage(QStringLiteral("Python 3 is required for Cloudflare deployment."),
                     true);
        return;
    }

    const auto command =
        forkmesh::control::buildCloudflareBootstrapCommand(
            request, apiToken, script, python,
            QString(), QString());
    if (command.arguments.join(QChar(u'\0')).contains(apiToken)) {
        flashMessage(
            QStringLiteral("Refusing an unsafe deployment command containing a credential."),
            true);
        return;
    }

    QSettings settings;
    if (!automaticTopology) {
        settings.setValue(QStringLiteral("control/cloudflareHostname"),
                          request.hostname.trimmed().toLower());
        settings.setValue(
            QStringLiteral("control/cloudflareMirrorHostname"),
            mirrorHostname);
        settings.setValue(QStringLiteral("control/cloudflareZone"),
                          request.zoneName.trimmed().toLower());
        settings.setValue(QStringLiteral("control/cloudflareAccount"),
                          request.accountId.trimmed());
        settings.setValue(QStringLiteral("control/cloudflareNodeName"),
                          request.nodeName.trimmed());
        settings.setValue(QStringLiteral("control/cloudflareRelayLabel"),
                          request.relayLabel.trimmed());
        settings.setValue(QStringLiteral("control/cloudflareMainRelay"),
                          request.mainRelayUrl.trimmed());
    }

    m_cloudflareDeployHostname = request.hostname.trimmed().toLower();
    m_cloudflareConnectAfterDeploy =
        !dryRun && m_cloudflareConnectCheck &&
        m_cloudflareConnectCheck->isChecked();
    m_cloudflareInstallVpsAfterDeploy = installFirstMirror;
    m_cloudflareActiveSecret = apiToken;
    if (m_cloudflareTokenEdit) {
        m_cloudflareTokenEdit->clear();
        m_cloudflareTokenEdit->setPlaceholderText(
            QStringLiteral("token held in memory only while deployment runs"));
    }
    if (m_controlNodeOutput)
        m_controlNodeOutput->clear();
    appendControlNodeOutput(
        QStringLiteral("%1 Cloudflare deployment for %2.\n")
            .arg(dryRun ? QStringLiteral("Validating")
                        : QStringLiteral("Starting"),
                 automaticTopology
                     ? QStringLiteral(
                           "the single account/zone visible to this token")
                     : m_cloudflareDeployHostname));
    if (m_cloudflareDryRunButton)
        m_cloudflareDryRunButton->setEnabled(false);
    if (m_cloudflareDeployButton)
        m_cloudflareDeployButton->setEnabled(false);
    if (m_cloudflareCancelButton)
        m_cloudflareCancelButton->setEnabled(true);

    auto *process = new QProcess(this);
    m_cloudflareBootstrapProcess = process;
    process->setProperty("forkmeshBootstrapOutput", QByteArray());
    process->setProcessEnvironment(command.environment);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        const QByteArray chunk = process->readAllStandardOutput();
        QByteArray captured =
            process->property("forkmeshBootstrapOutput").toByteArray();
        if (captured.size() < 1024 * 1024)
            captured.append(chunk.left(1024 * 1024 - captured.size()));
        process->setProperty("forkmeshBootstrapOutput", captured);
        appendControlNodeOutput(QString::fromUtf8(chunk));
    });
    connect(process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    appendControlNodeOutput(
                        QStringLiteral(
                            "Deployment process could not start; verify Python "
                            "and the installed ForkMesh deployment resources.\n"));
                }
            });
    connect(process, &QProcess::finished, this,
            [this, process, dryRun, automaticTopology](int exitCode,
                                    QProcess::ExitStatus exitStatus) {
                const QByteArray trailing =
                    process->readAllStandardOutput();
                QByteArray captured =
                    process
                        ->property("forkmeshBootstrapOutput")
                        .toByteArray();
                if (captured.size() < 1024 * 1024)
                    captured.append(
                        trailing.left(
                            1024 * 1024 -
                            captured.size()));
                appendControlNodeOutput(
                    QString::fromUtf8(trailing));
                QString resultError;
                const QJsonObject result =
                    forkmesh::control::
                        parseCloudflareBootstrapResult(
                            captured, &resultError);
                bool ok =
                    exitStatus == QProcess::NormalExit &&
                    exitCode == 0 &&
                    !result.isEmpty();
                if (ok && automaticTopology) {
                    forkmesh::control::
                        CloudflareBootstrapRequest resolved;
                    resolved.hostname =
                        result
                            .value(QStringLiteral("hostname"))
                            .toString();
                    resolved.zoneName =
                        result
                            .value(QStringLiteral("zoneName"))
                            .toString();
                    resolved.accountId =
                        result
                            .value(QStringLiteral("accountId"))
                            .toString();
                    resolved.nodeName =
                        result
                            .value(QStringLiteral("nodeName"))
                            .toString();
                    resolved.relayLabel =
                        m_cloudflareRelayLabelEdit
                            ? m_cloudflareRelayLabelEdit
                                  ->text()
                            : QString();
                    resolved.mainRelayUrl =
                        m_cloudflareMainRelayEdit
                            ? m_cloudflareMainRelayEdit
                                  ->text()
                            : QString();
                    const QString directHostname =
                        result
                            .value(QStringLiteral(
                                "directMirrorHostname"))
                            .toString()
                            .trimmed()
                            .toLower();
                    auto directRequest = resolved;
                    directRequest.hostname =
                        directHostname;
                    const QString relayError =
                        forkmesh::control::
                            validateCloudflareBootstrapRequest(
                                resolved);
                    const QString directError =
                        forkmesh::control::
                            validateCloudflareBootstrapRequest(
                                directRequest);
                    if (!relayError.isEmpty() ||
                        !directError.isEmpty() ||
                        directHostname ==
                            resolved.hostname
                                .trimmed()
                                .toLower()) {
                        ok = false;
                        resultError = QStringLiteral(
                            "Automatic Cloudflare discovery returned "
                            "unsafe topology.");
                    } else {
                        if (m_cloudflareHostnameEdit)
                            m_cloudflareHostnameEdit
                                ->setText(
                                    resolved.hostname);
                        if (m_cloudflareMirrorHostnameEdit)
                            m_cloudflareMirrorHostnameEdit
                                ->setText(
                                    directHostname);
                        if (m_cloudflareZoneEdit)
                            m_cloudflareZoneEdit
                                ->setText(
                                    resolved.zoneName);
                        if (m_cloudflareAccountEdit)
                            m_cloudflareAccountEdit
                                ->setText(
                                    resolved.accountId);
                        if (m_cloudflareNodeNameEdit)
                            m_cloudflareNodeNameEdit
                                ->setText(
                                    resolved.nodeName);
                        QSettings settings;
                        settings.setValue(
                            QStringLiteral(
                                "control/cloudflareHostname"),
                            resolved.hostname);
                        settings.setValue(
                            QStringLiteral(
                                "control/cloudflareMirrorHostname"),
                            directHostname);
                        settings.setValue(
                            QStringLiteral(
                                "control/cloudflareZone"),
                            resolved.zoneName);
                        settings.setValue(
                            QStringLiteral(
                                "control/cloudflareAccount"),
                            resolved.accountId);
                        settings.setValue(
                            QStringLiteral(
                                "control/cloudflareNodeName"),
                            resolved.nodeName);
                        settings.setValue(
                            QStringLiteral(
                                "control/cloudflareRelayLabel"),
                            resolved.relayLabel.trimmed());
                        settings.setValue(
                            QStringLiteral(
                                "control/cloudflareMainRelay"),
                            resolved.mainRelayUrl.trimmed());
                        m_cloudflareDeployHostname =
                            resolved.hostname
                                .trimmed()
                                .toLower();
                        appendControlNodeOutput(
                            QStringLiteral(
                                "Discovered one unambiguous active zone; "
                                "using relay %1 and direct mirror %2.\n")
                                .arg(
                                    m_cloudflareDeployHostname,
                                    directHostname));
                    }
                }
                if (ok) {
                    appendControlNodeOutput(
                        dryRun
                            ? QStringLiteral(
                                  "Worker validation completed; validating "
                                  "the direct mirror Tunnel next.\n")
                            : QStringLiteral(
                                  "Worker deployment completed; provisioning "
                                  "the direct mirror Tunnel next.\n"));
                    logSystem(
                        QStringLiteral("Control node: Cloudflare %1 completed for %2.")
                            .arg(dryRun ? QStringLiteral("validation")
                                        : QStringLiteral("deployment"),
                                 m_cloudflareDeployHostname));
                } else {
                    m_cloudflareInstallVpsAfterDeploy = false;
                    if (!resultError.isEmpty()) {
                        appendControlNodeOutput(
                            QStringLiteral(
                                "Machine result rejected: %1\n")
                                .arg(resultError));
                    }
                    appendControlNodeOutput(
                        QStringLiteral("Deployment failed (exit %1). Review the "
                                       "redacted local output above.\n")
                            .arg(exitCode));
                    logSystem(
                        QStringLiteral("Control node: Cloudflare deployment failed "
                                       "for %1 (exit %2).")
                            .arg(m_cloudflareDeployHostname)
                            .arg(exitCode));
                    flashMessage(
                        QStringLiteral("Cloudflare deployment failed; see the local "
                                       "control-node output."),
                        true);
                }
                process->setProcessEnvironment(QProcessEnvironment());
                process->deleteLater();
                if (m_cloudflareBootstrapProcess == process)
                    m_cloudflareBootstrapProcess = nullptr;
                if (ok) {
                    provisionDirectMirrorEndpoint(dryRun);
                    refreshControlNode();
                    return;
                }
                if (m_cloudflareDryRunButton)
                    m_cloudflareDryRunButton->setEnabled(true);
                if (m_cloudflareDeployButton)
                    m_cloudflareDeployButton->setEnabled(true);
                if (m_cloudflareCancelButton)
                    m_cloudflareCancelButton->setEnabled(false);
                if (m_cloudflareTokenEdit)
                    m_cloudflareTokenEdit->setPlaceholderText(
                        QStringLiteral(
                            "session-only Cloudflare API token"));
                m_cloudflareActiveSecret.fill(QChar(u'\0'));
                m_cloudflareActiveSecret.clear();
                refreshControlNode();
            });
    process->start(command.program, command.arguments);
}

void MainWindow::cancelCloudflareBootstrap()
{
    QProcess *active =
        (m_cloudflareBootstrapProcess &&
         m_cloudflareBootstrapProcess->state() !=
             QProcess::NotRunning)
            ? m_cloudflareBootstrapProcess
            : ((m_cloudflareTunnelBootstrapProcess &&
                m_cloudflareTunnelBootstrapProcess->state() !=
                    QProcess::NotRunning)
                   ? m_cloudflareTunnelBootstrapProcess
                   : nullptr);
    if (!active)
        return;
    appendControlNodeOutput(
        QStringLiteral("Cancellation requested; waiting for local cleanup.\n"));
    active->terminate();
    QPointer<QProcess> process(active);
    QTimer::singleShot(5000, this, [process] {
        if (process && process->state() != QProcess::NotRunning)
            process->kill();
    });
}

void MainWindow::appendControlNodeOutput(const QString &text)
{
    if (!m_controlNodeOutput || text.isEmpty())
        return;
    const QString safe = forkmesh::control::redactProcessOutput(
        text, {m_cloudflareActiveSecret});
    m_controlNodeOutput->moveCursor(QTextCursor::End);
    m_controlNodeOutput->insertPlainText(safe);
    m_controlNodeOutput->moveCursor(QTextCursor::End);
    m_controlNodeOutput->ensureCursorVisible();
}

QJsonObject MainWindow::currentDeploymentDescriptor()
{
    QSettings settings;
    const QMap<QString, QString> variables = ActionStore::variables();
    QJsonObject savedHostPasswords;
    for (auto it = m_hostSessionPasswords.constBegin();
         it != m_hostSessionPasswords.constEnd(); ++it) {
        savedHostPasswords.insert(it.key(), it.value());
    }
    const forkmesh::control::AgentCliCredentials agentCredentials =
        localAgentCliCredentials();
    QJsonObject agentEnvironment;
    for (auto it = agentCredentials.env.constBegin();
         it != agentCredentials.env.constEnd(); ++it) {
        agentEnvironment.insert(it.key(), it.value());
    }
    const auto currentOrSetting = [&settings](QLineEdit *edit,
                                               const QString &key) {
        return edit ? edit->text().trimmed()
                    : settings.value(key).toString().trimmed();
    };
    const QJsonObject deployment{
        {QStringLiteral("provider"), QStringLiteral("vultr")},
        {QStringLiteral("credentials"),
         QJsonObject{
             {QStringLiteral("cloudflareApiToken"),
              resolvedCloudflareApiToken()},
             {QStringLiteral("vultrApiKey"),
              m_vultrApiKeyEdit
                  ? m_vultrApiKeyEdit->text().trimmed()
                  : forkmesh::control::vultrApiKeyFromVariables(variables)},
             {QStringLiteral("savedHostPasswords"), savedHostPasswords},
             {QStringLiteral("claudeCredentialsBase64"),
              QString::fromLatin1(agentCredentials.claudeCredentials.toBase64())},
             {QStringLiteral("codexAuthBase64"),
              QString::fromLatin1(agentCredentials.codexAuth.toBase64())},
             {QStringLiteral("agentEnvironment"), agentEnvironment},
         }},
        {QStringLiteral("instance"),
         QJsonObject{
             {QStringLiteral("nodeName"),
              m_vultrNameEdit ? m_vultrNameEdit->text().trimmed() : QString()},
             {QStringLiteral("installAgentClis"),
              m_vultrAgentClisCheck ? m_vultrAgentClisCheck->isChecked() : true},
         }},
        {QStringLiteral("cloudflare"),
         QJsonObject{
             {QStringLiteral("hostname"),
              currentOrSetting(m_cloudflareHostnameEdit,
                               QStringLiteral("control/cloudflareHostname"))},
             {QStringLiteral("mirrorHostname"),
              currentOrSetting(
                  m_cloudflareMirrorHostnameEdit,
                  QStringLiteral("control/cloudflareMirrorHostname"))},
             {QStringLiteral("zone"),
              currentOrSetting(m_cloudflareZoneEdit,
                               QStringLiteral("control/cloudflareZone"))},
             {QStringLiteral("accountId"),
              currentOrSetting(m_cloudflareAccountEdit,
                               QStringLiteral("control/cloudflareAccount"))},
             {QStringLiteral("nodeName"),
              currentOrSetting(m_cloudflareNodeNameEdit,
                               QStringLiteral("control/cloudflareNodeName"))},
             {QStringLiteral("relayLabel"),
              currentOrSetting(m_cloudflareRelayLabelEdit,
                               QStringLiteral("control/cloudflareRelayLabel"))},
             {QStringLiteral("upstreamRelay"),
              currentOrSetting(m_cloudflareMainRelayEdit,
                               QStringLiteral("control/cloudflareMainRelay"))},
         }},
        {QStringLiteral("firstMirror"),
         QJsonObject{
             {QStringLiteral("host"),
              m_cloudflareVpsHostEdit
                  ? m_cloudflareVpsHostEdit->text().trimmed()
                  : QString()},
             {QStringLiteral("sshUser"),
              m_cloudflareVpsUserEdit
                  ? m_cloudflareVpsUserEdit->text().trimmed()
                  : QString()},
             {QStringLiteral("sshPassword"),
              m_cloudflareVpsPasswordEdit
                  ? m_cloudflareVpsPasswordEdit->text()
                  : QString()},
        }},
        {QStringLiteral("applicationVersion"),
         QStringLiteral(FORKMESH_VERSION)},
    };
    return deployment;
}

QStringList MainWindow::systemExportExcludedNames() const
{
    if (m_exportSurveyDone) {
        return QStringList(m_exportExcludedData.constBegin(),
                           m_exportExcludedData.constEnd());
    }
    // The export page has not surveyed yet (an export triggered from a
    // shortcut, say). Fall back to the documented bulk defaults from a cheap
    // top-level listing so no export ever tries to base64 a multi-gigabyte
    // mirror or sandbox tree.
    QStringList names;
    const QDir::Filters filters =
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
    for (const QString &name :
         QDir(forkmesh::paths::dataDir()).entryList(filters, QDir::Name)) {
        if (name == QLatin1String("core")) {
            for (const QString &child :
                 QDir(forkmesh::paths::dataDir() + QStringLiteral("/core"))
                     .entryList(filters, QDir::Name)) {
                if (forkmesh::control::completeSystemDataIsBulk(QStringLiteral("core/") + child))
                    names.append(QStringLiteral("core/") + child);
            }
            continue;
        }
        if (forkmesh::control::completeSystemDataIsBulk(name))
            names.append(name);
    }
    return names;
}

void MainWindow::exportCompleteSystemBundle()
{
    const int confirmed = QMessageBox::warning(
        this, QStringLiteral("Export complete system"),
        QStringLiteral(
            "This JSON contains the complete control-plane state in clear text: "
            "identity and SSH private keys, API keys, action variables/secrets, "
            "all settings, account/history data, and deployment configuration.\n\n"
            "Anyone with the file can act as this node. Store it only in an "
            "encrypted, private location."),
        QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);
    if (confirmed != QMessageBox::Ok)
        return;

    QString suggested =
        QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
            .filePath(QStringLiteral("forkmesh-complete-system-%1.json")
                          .arg(QDateTime::currentDateTime().toString(
                              QStringLiteral("yyyyMMdd-HHmmss"))));
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export complete ForkMesh system"), suggested,
        QStringLiteral("ForkMesh complete system (*.json);;JSON files (*.json)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        path += QStringLiteral(".json");

    const QJsonObject deployment = currentDeploymentDescriptor();
    QSettings settings;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const QJsonObject bundle = forkmesh::control::createCompleteSystemBundle(
        settings, ActionStore::variables(),
        forkmesh::paths::dataDir(),
        settings.fileName(),
        forkmesh::control::siteDeployEnvFilePath(
            QStringLiteral(FORKMESH_SOURCE_DIR),
            QCoreApplication::applicationDirPath()),
        deployment, &error, systemExportExcludedNames());
    bool written = false;
    if (!bundle.isEmpty()) {
        written = forkmesh::control::writeCompleteSystemBundle(path, bundle,
                                                               &error);
    }
    QGuiApplication::restoreOverrideCursor();
    if (!written) {
        QMessageBox::warning(
            this, QStringLiteral("Export failed"),
            error.isEmpty() ? QStringLiteral("The complete system could not be exported.")
                            : error);
        return;
    }
    const QJsonObject summary = bundle.value(QStringLiteral("summary")).toObject();
    const int skippedCount =
        summary.value(QStringLiteral("skippedCount")).toInt();
    flashMessage(
        QStringLiteral("Exported %1 settings, %2 variables, and %3 files to %4.%5")
            .arg(summary.value(QStringLiteral("settingCount")).toInt())
            .arg(summary.value(QStringLiteral("variableCount")).toInt())
            .arg(summary.value(QStringLiteral("fileCount")).toInt())
            .arg(QDir::toNativeSeparators(path),
                 skippedCount > 0
                     ? QStringLiteral(" %1 pipe/socket/link entries were "
                                      "skipped; JSON cannot carry them.")
                           .arg(skippedCount)
                     : QString()));
}

void MainWindow::showSystemExportPage()
{
    showSection(kControlNodeSectionIndex);
    if (m_controlNodeTabs && m_controlExportTabIndex >= 0)
        m_controlNodeTabs->setCurrentIndex(m_controlExportTabIndex);
    refreshSystemExportValues();
    if (!m_exportSurveyDone && !m_exportSurveyRunning)
        startSystemExportSurvey();
}

void MainWindow::showSystemImportPage()
{
    showSection(kControlNodeSectionIndex);
    if (m_controlNodeTabs && m_controlImportTabIndex >= 0)
        m_controlNodeTabs->setCurrentIndex(m_controlImportTabIndex);
}

QWidget *MainWindow::buildSystemExportCard()
{
    QVBoxLayout *col = nullptr;
    QFrame *card = controlCard(QStringLiteral("Export complete system"), &col);
    col->addWidget(controlHint(QStringLiteral(
        "Every value listed below is written to one owner-only JSON in clear "
        "text: identity and SSH private keys, API tokens, Actions variables "
        "and secrets, all settings, and the ticked application-data folders. "
        "Anyone holding that file can act as this node, so keep it encrypted "
        "and private. Folders the mesh can rebuild — served mirrors, "
        "archives, CI sandboxes, agent scratch and caches — start unticked so "
        "a recovery file stays small; tick one to carry it anyway.")));

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    m_exportRevealSecrets = new QCheckBox(QStringLiteral("Show secret values"));
    m_exportRevealSecrets->setObjectName(QStringLiteral("exportRevealSecrets"));
    m_exportRevealSecrets->setToolTip(QStringLiteral(
        "Values are masked on screen only. The exported JSON always contains "
        "them in full."));
    connect(m_exportRevealSecrets, &QCheckBox::toggled, this,
            [this] { refreshSystemExportValues(); });
    row->addWidget(m_exportRevealSecrets);
    auto *rescan = new QPushButton(QStringLiteral("Rescan"));
    rescan->setObjectName(QStringLiteral("exportRescanButton"));
    rescan->setCursor(Qt::PointingHandCursor);
    setOcticon(rescan, QStringLiteral("sync"), 14);
    connect(rescan, &QPushButton::clicked, this, [this] {
        refreshSystemExportValues();
        startSystemExportSurvey();
    });
    row->addWidget(rescan);
    row->addStretch();
    m_exportRunButton = new QPushButton(QStringLiteral("Export JSON…"));
    m_exportRunButton->setObjectName(QStringLiteral("exportCompleteSystemButton"));
    m_exportRunButton->setProperty("buttonSize", "primary");
    m_exportRunButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_exportRunButton, QStringLiteral("download"), 14);
    connect(m_exportRunButton, &QPushButton::clicked, this,
            &MainWindow::exportCompleteSystemBundle);
    row->addWidget(m_exportRunButton);
    col->addLayout(row);

    m_exportSummary = new QLabel;
    m_exportSummary->setObjectName(QStringLiteral("exportSummary"));
    m_exportSummary->setWordWrap(true);
    m_exportSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(m_exportSummary);

    m_exportValuesTable = systemValuesTable(QStringLiteral("exportValuesTable"));
    col->addWidget(m_exportValuesTable, 1);

    col->addWidget(cardSubheading(QStringLiteral("Application data")));
    m_exportDataStatus = new QLabel;
    m_exportDataStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_exportDataStatus->setWordWrap(true);
    col->addWidget(m_exportDataStatus);

    m_exportDataTable = new QTableWidget(0, 5);
    m_exportDataTable->setObjectName(QStringLiteral("exportDataTable"));
    m_exportDataTable->setHorizontalHeaderLabels(
        {QStringLiteral("Include"), QStringLiteral("Folder"),
         QStringLiteral("Files"), QStringLiteral("Size"),
         QStringLiteral("Note")});
    m_exportDataTable->verticalHeader()->setVisible(false);
    m_exportDataTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_exportDataTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_exportDataTable->setShowGrid(false);
    m_exportDataTable->setMinimumHeight(220);
    for (int column = 0; column < 4; ++column) {
        m_exportDataTable->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    m_exportDataTable->horizontalHeader()->setSectionResizeMode(
        4, QHeaderView::Stretch);
    connect(m_exportDataTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) {
                if (m_exportApplyingDataTable || !item || item->column() != 0)
                    return;
                const QString name = item->data(Qt::UserRole).toString();
                if (name.isEmpty())
                    return;
                if (item->checkState() == Qt::Checked) {
                    m_exportExcludedData.remove(name);
                    m_exportForcedExcluded.remove(name);
                    if (forkmesh::control::completeSystemDataIsBulk(name))
                        m_exportForcedIncluded.insert(name);
                } else {
                    m_exportExcludedData.insert(name);
                    m_exportForcedIncluded.remove(name);
                    if (!forkmesh::control::completeSystemDataIsBulk(name))
                        m_exportForcedExcluded.insert(name);
                }
                QSettings settings;
                settings.setValue(
                    QStringLiteral("control/exportExcludedData"),
                    QStringList(m_exportForcedExcluded.constBegin(),
                                m_exportForcedExcluded.constEnd()));
                settings.setValue(
                    QStringLiteral("control/exportIncludedData"),
                    QStringList(m_exportForcedIncluded.constBegin(),
                                m_exportForcedIncluded.constEnd()));
                updateSystemExportSummary();
            });
    col->addWidget(m_exportDataTable, 1);

    QSettings settings;
    const QStringList savedOff =
        settings.value(QStringLiteral("control/exportExcludedData"))
            .toStringList();
    m_exportForcedExcluded = QSet<QString>(savedOff.constBegin(),
                                           savedOff.constEnd());
    const QStringList savedOn =
        settings.value(QStringLiteral("control/exportIncludedData"))
            .toStringList();
    m_exportForcedIncluded = QSet<QString>(savedOn.constBegin(),
                                           savedOn.constEnd());

    refreshSystemExportValues();
    startSystemExportSurvey();
    return card;
}

void MainWindow::refreshSystemExportValues()
{
    if (!m_exportValuesTable)
        return;
    const bool reveal =
        m_exportRevealSecrets && m_exportRevealSecrets->isChecked();
    m_exportValuesTable->setRowCount(0);

    const QJsonObject deployment = currentDeploymentDescriptor();
    appendDeploymentValueRows(m_exportValuesTable, QString(), deployment,
                              reveal);

    QSettings settings;
    settings.sync();
    appendValueRow(m_exportValuesTable, QStringLiteral("Files"),
                   QStringLiteral("Native settings file"),
                   QDir::toNativeSeparators(settings.fileName()));
    const QString workerEnvironment = forkmesh::control::siteDeployEnvFilePath(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
    appendValueRow(m_exportValuesTable, QStringLiteral("Files"),
                   QStringLiteral("Worker environment"),
                   QFileInfo::exists(workerEnvironment)
                       ? QDir::toNativeSeparators(workerEnvironment)
                       : QStringLiteral("(not present on this machine)"));
    appendValueRow(m_exportValuesTable, QStringLiteral("Files"),
                   QStringLiteral("Application data"),
                   QDir::toNativeSeparators(forkmesh::paths::dataDir()));

    const QMap<QString, QString> variables = ActionStore::variables();
    for (auto it = variables.constBegin(); it != variables.constEnd(); ++it) {
        appendValueRow(m_exportValuesTable, QStringLiteral("Variables"),
                       it.key(), maskedExportValue(it.key(), it.value(), reveal));
    }
    const QStringList keys = settings.allKeys();
    for (const QString &key : keys) {
        appendValueRow(m_exportValuesTable, QStringLiteral("Settings"), key,
                       maskedExportValue(key,
                                         exportSettingText(settings.value(key)),
                                         reveal));
    }
    updateSystemExportSummary();
}

void MainWindow::startSystemExportSurvey()
{
    if (!m_exportDataTable || m_exportSurveyRunning)
        return;
    m_exportSurveyRunning = true;
    if (m_exportDataStatus) {
        m_exportDataStatus->setText(
            QStringLiteral("Measuring application data …"));
    }
    const QString appData =
        forkmesh::paths::dataDir();
    const int epoch = ++m_exportSurveyEpoch;
    auto *watcher =
        new QFutureWatcher<QList<forkmesh::control::CompleteSystemDataEntry>>(
            this);
    connect(watcher,
            &QFutureWatcher<
                QList<forkmesh::control::CompleteSystemDataEntry>>::finished,
            this, [this, watcher, epoch] {
                watcher->deleteLater();
                m_exportSurveyRunning = false;
                if (epoch != m_exportSurveyEpoch)
                    return; // a newer scan owns the table now
                applySystemExportSurvey(watcher->result());
            });
    watcher->setFuture(QtConcurrent::run([appData] {
        return forkmesh::control::surveyCompleteSystemApplicationData(appData);
    }));
}

void MainWindow::applySystemExportSurvey(
    const QList<forkmesh::control::CompleteSystemDataEntry> &entries)
{
    m_exportDataEntries = entries;
    m_exportSurveyDone = true;
    m_exportExcludedData.clear();
    for (const forkmesh::control::CompleteSystemDataEntry &entry : entries) {
        const bool excluded =
            m_exportForcedExcluded.contains(entry.name) ||
            (entry.bulk && !m_exportForcedIncluded.contains(entry.name));
        if (excluded)
            m_exportExcludedData.insert(entry.name);
    }
    if (!m_exportDataTable)
        return;

    m_exportApplyingDataTable = true;
    m_exportDataTable->setRowCount(entries.size());
    int skippedTotal = 0;
    for (int row = 0; row < entries.size(); ++row) {
        const forkmesh::control::CompleteSystemDataEntry &entry =
            entries.at(row);
        skippedTotal += entry.skippedEntries;
        auto *include = new QTableWidgetItem;
        include->setData(Qt::UserRole, entry.name);
        include->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        include->setCheckState(m_exportExcludedData.contains(entry.name)
                                   ? Qt::Unchecked
                                   : Qt::Checked);
        m_exportDataTable->setItem(row, 0, include);
        m_exportDataTable->setItem(row, 1,
                                   new QTableWidgetItem(entry.name));
        m_exportDataTable->setItem(
            row, 2, new QTableWidgetItem(QLocale().toString(entry.fileCount)));
        m_exportDataTable->setItem(
            row, 3,
            new QTableWidgetItem(QLocale().formattedDataSize(entry.bytes)));
        QStringList notes;
        if (entry.bulk)
            notes.append(QStringLiteral("rebuildable from the mesh"));
        if (!entry.directory)
            notes.append(QStringLiteral("single file"));
        if (entry.skippedEntries > 0) {
            notes.append(QStringLiteral("%1 pipe/socket/link entries cannot be "
                                        "carried by JSON")
                             .arg(entry.skippedEntries));
        }
        m_exportDataTable->setItem(
            row, 4, new QTableWidgetItem(notes.join(QStringLiteral(" · "))));
    }
    m_exportApplyingDataTable = false;

    if (m_exportDataStatus) {
        m_exportDataStatus->setText(
            skippedTotal > 0
                ? QStringLiteral(
                      "%1 entries are pipes, sockets or symbolic links. They "
                      "are skipped and named in the bundle's skipped list "
                      "instead of failing the export.")
                      .arg(skippedTotal)
                : QStringLiteral("Every entry can be carried by the bundle."));
    }
    updateSystemExportSummary();
}

void MainWindow::updateSystemExportSummary()
{
    if (!m_exportSummary)
        return;
    QSettings settings;
    const int settingCount = settings.allKeys().size();
    const int variableCount = ActionStore::variables().size();
    if (!m_exportSurveyDone) {
        m_exportSummary->setText(
            QStringLiteral("%1 settings · %2 variables · measuring "
                           "application data …")
                .arg(settingCount)
                .arg(variableCount));
        return;
    }
    int includedFiles = 0;
    qint64 includedBytes = 0;
    qint64 excludedBytes = 0;
    for (const forkmesh::control::CompleteSystemDataEntry &entry :
         std::as_const(m_exportDataEntries)) {
        if (m_exportExcludedData.contains(entry.name)) {
            excludedBytes += entry.bytes;
            continue;
        }
        includedFiles += entry.fileCount;
        includedBytes += entry.bytes;
    }
    m_exportSummary->setText(
        QStringLiteral("%1 settings · %2 variables · %3 application-data files "
                       "· %4 of data (JSON is roughly a third larger). %5 left "
                       "out.")
            .arg(settingCount)
            .arg(variableCount)
            .arg(includedFiles)
            .arg(QLocale().formattedDataSize(includedBytes),
                 QLocale().formattedDataSize(excludedBytes)));
    if (m_exportRunButton) {
        m_exportRunButton->setToolTip(
            QStringLiteral("Write the %1 of ticked data, every setting and "
                           "every secret to one owner-only JSON.")
                .arg(QLocale().formattedDataSize(includedBytes)));
    }
}

QWidget *MainWindow::buildSystemImportCard()
{
    QVBoxLayout *col = nullptr;
    QFrame *card =
        controlCard(QStringLiteral("Import a system and deploy"), &col);
    col->addWidget(controlHint(QStringLiteral(
        "Every field starts blank. Choose one complete-system JSON: it is "
        "integrity-checked first, then its credentials and topology fill the "
        "form below and the full contents are listed for review. Nothing is "
        "applied and no paid resource is created until you press Deploy "
        "instance.")));

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    auto *choose = new QPushButton(QStringLiteral("Choose bundle JSON…"));
    choose->setObjectName(QStringLiteral("deploymentImportBundleButton"));
    choose->setCursor(Qt::PointingHandCursor);
    setOcticon(choose, QStringLiteral("upload"), 14);
    connect(choose, &QPushButton::clicked, this,
            &MainWindow::chooseSystemImportBundle);
    row->addWidget(choose);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    clear->setObjectName(QStringLiteral("deploymentClearBundleButton"));
    clear->setCursor(Qt::PointingHandCursor);
    connect(clear, &QPushButton::clicked, this,
            &MainWindow::clearSystemImportForm);
    row->addWidget(clear);
    row->addStretch();
    m_importDeployButton = new QPushButton(QStringLiteral("Deploy instance"));
    m_importDeployButton->setObjectName(
        QStringLiteral("deploymentDeployButton"));
    m_importDeployButton->setProperty("buttonSize", "primary");
    m_importDeployButton->setCursor(Qt::PointingHandCursor);
    m_importDeployButton->setEnabled(false);
    setOcticon(m_importDeployButton, QStringLiteral("rocket"), 14);
    connect(m_importDeployButton, &QPushButton::clicked, this,
            &MainWindow::deployImportedSystemBundle);
    row->addWidget(m_importDeployButton);
    col->addLayout(row);

    m_importBundlePathLabel = new QLabel(QStringLiteral("No bundle chosen"));
    m_importBundlePathLabel->setObjectName(QStringLiteral("mutedLabel"));
    m_importBundlePathLabel->setWordWrap(true);
    m_importBundlePathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(m_importBundlePathLabel);
    m_importSummaryLabel = new QLabel;
    m_importSummaryLabel->setObjectName(QStringLiteral("mutedLabel"));
    m_importSummaryLabel->setWordWrap(true);
    col->addWidget(m_importSummaryLabel);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setSpacing(7);
    m_importVultrKeyEdit = new QLineEdit;
    m_importVultrKeyEdit->setObjectName(QStringLiteral("deploymentVultrApiKey"));
    m_importVultrKeyEdit->setEchoMode(QLineEdit::Password);
    m_importVultrKeyEdit->setPlaceholderText(
        QStringLiteral("filled in from the bundle"));
    m_importVultrKeyEdit->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("Vultr API key"), m_importVultrKeyEdit);
    m_importCloudflareTokenEdit = new QLineEdit;
    m_importCloudflareTokenEdit->setObjectName(
        QStringLiteral("deploymentCloudflareApiToken"));
    m_importCloudflareTokenEdit->setEchoMode(QLineEdit::Password);
    m_importCloudflareTokenEdit->setPlaceholderText(
        QStringLiteral("filled in from the bundle"));
    m_importCloudflareTokenEdit->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("Cloudflare token"),
                 m_importCloudflareTokenEdit);
    m_importNodeNameEdit = new QLineEdit;
    m_importNodeNameEdit->setObjectName(QStringLiteral("deploymentNodeName"));
    m_importNodeNameEdit->setPlaceholderText(
        QStringLiteral("filled in from the bundle"));
    form->addRow(QStringLiteral("Node name"), m_importNodeNameEdit);
    m_importZoneEdit = new QLineEdit;
    m_importZoneEdit->setObjectName(
        QStringLiteral("deploymentCloudflareZone"));
    m_importZoneEdit->setPlaceholderText(
        QStringLiteral("filled in from the bundle"));
    form->addRow(QStringLiteral("Cloudflare zone"), m_importZoneEdit);
    m_importAccountEdit = new QLineEdit;
    m_importAccountEdit->setObjectName(
        QStringLiteral("deploymentCloudflareAccount"));
    m_importAccountEdit->setPlaceholderText(
        QStringLiteral("filled in from the bundle"));
    form->addRow(QStringLiteral("Account ID"), m_importAccountEdit);
    m_importRelayHostEdit = new QLineEdit;
    m_importRelayHostEdit->setObjectName(
        QStringLiteral("deploymentRelayHostname"));
    m_importRelayHostEdit->setPlaceholderText(
        QStringLiteral("filled in from the bundle"));
    form->addRow(QStringLiteral("Relay hostname"), m_importRelayHostEdit);
    m_importMirrorHostEdit = new QLineEdit;
    m_importMirrorHostEdit->setObjectName(
        QStringLiteral("deploymentMirrorHostname"));
    m_importMirrorHostEdit->setPlaceholderText(
        QStringLiteral("filled in from the bundle"));
    form->addRow(QStringLiteral("Mirror hostname"), m_importMirrorHostEdit);
    col->addLayout(form);

    m_importAgentClisCheck =
        new QCheckBox(QStringLiteral("Install Claude Code + Codex access"));
    m_importAgentClisCheck->setObjectName(
        QStringLiteral("deploymentInstallAgentClis"));
    col->addWidget(m_importAgentClisCheck);
    m_importApplySettingsCheck = new QCheckBox(
        QStringLiteral("Apply all imported settings + variables"));
    m_importApplySettingsCheck->setObjectName(
        QStringLiteral("deploymentApplySettings"));
    m_importApplySettingsCheck->setToolTip(QStringLiteral(
        "Imports every setting and shared secret before provisioning. Existing "
        "newer settings not named by the bundle are retained."));
    col->addWidget(m_importApplySettingsCheck);

    m_importReviewLabel = new QLabel(QStringLiteral(
        "Choose a bundle to see everything it carries."));
    m_importReviewLabel->setObjectName(QStringLiteral("mutedLabel"));
    m_importReviewLabel->setWordWrap(true);
    col->addWidget(m_importReviewLabel);

    col->addWidget(cardSubheading(QStringLiteral("Bundle contents")));
    m_importValuesTable = systemValuesTable(QStringLiteral("importValuesTable"));
    col->addWidget(m_importValuesTable, 1);

    clearSystemImportForm();
    return card;
}

void MainWindow::clearSystemImportForm()
{
    m_importedSystemBundle = QJsonObject();
    for (QLineEdit *edit :
         {m_importVultrKeyEdit, m_importCloudflareTokenEdit,
          m_importNodeNameEdit, m_importZoneEdit, m_importAccountEdit,
          m_importRelayHostEdit, m_importMirrorHostEdit}) {
        if (edit)
            edit->clear();
    }
    if (m_importAgentClisCheck)
        m_importAgentClisCheck->setChecked(true);
    if (m_importApplySettingsCheck)
        m_importApplySettingsCheck->setChecked(true);
    if (m_importBundlePathLabel)
        m_importBundlePathLabel->setText(QStringLiteral("No bundle chosen"));
    if (m_importSummaryLabel)
        m_importSummaryLabel->clear();
    if (m_importReviewLabel) {
        m_importReviewLabel->setText(QStringLiteral(
            "Choose a bundle to see everything it carries."));
    }
    if (m_importValuesTable)
        m_importValuesTable->setRowCount(0);
    if (m_importDeployButton)
        m_importDeployButton->setEnabled(false);
}

void MainWindow::chooseSystemImportBundle()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import complete ForkMesh system"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        QStringLiteral("ForkMesh complete system (*.json);;JSON files (*.json)"));
    if (path.isEmpty())
        return;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const QJsonObject candidate =
        forkmesh::control::readCompleteSystemBundle(path, &error);
    QGuiApplication::restoreOverrideCursor();
    if (candidate.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Import failed"), error);
        return;
    }
    applySystemImportBundle(candidate, path);
}

void MainWindow::applySystemImportBundle(const QJsonObject &bundle,
                                         const QString &path)
{
    bool variablesOk = false;
    const QMap<QString, QString> variables =
        forkmesh::control::completeSystemBundleVariables(bundle, &variablesOk);
    if (!variablesOk) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QStringLiteral("The variables section is invalid."));
        clearSystemImportForm();
        return;
    }
    m_importedSystemBundle = bundle;

    const QJsonObject deployment =
        bundle.value(QStringLiteral("deployment")).toObject();
    const QJsonObject instance =
        deployment.value(QStringLiteral("instance")).toObject();
    const QJsonObject credentials =
        deployment.value(QStringLiteral("credentials")).toObject();
    const QJsonObject cloudflare =
        deployment.value(QStringLiteral("cloudflare")).toObject();
    const auto importedSetting = [&bundle](const QString &key) {
        bool ok = false;
        const QVariant value =
            forkmesh::control::completeSystemBundleSetting(bundle, key, &ok);
        return ok ? value.toString().trimmed() : QString();
    };

    if (m_importVultrKeyEdit) {
        m_importVultrKeyEdit->setText(
            credentials.value(QStringLiteral("vultrApiKey")).toString());
        if (m_importVultrKeyEdit->text().trimmed().isEmpty()) {
            m_importVultrKeyEdit->setText(
                forkmesh::control::vultrApiKeyFromVariables(variables));
        }
    }
    if (m_importCloudflareTokenEdit) {
        m_importCloudflareTokenEdit->setText(
            credentials.value(QStringLiteral("cloudflareApiToken")).toString());
        if (m_importCloudflareTokenEdit->text().trimmed().isEmpty()) {
            m_importCloudflareTokenEdit->setText(
                forkmesh::control::cloudflareApiTokenFromVariables(variables));
        }
    }
    if (m_importNodeNameEdit) {
        m_importNodeNameEdit->setText(
            instance.value(QStringLiteral("nodeName")).toString());
    }
    if (m_importAgentClisCheck) {
        m_importAgentClisCheck->setChecked(
            instance.value(QStringLiteral("installAgentClis")).toBool(true));
    }
    if (m_importZoneEdit) {
        m_importZoneEdit->setText(
            cloudflare.value(QStringLiteral("zone")).toString());
        if (m_importZoneEdit->text().trimmed().isEmpty()) {
            m_importZoneEdit->setText(
                importedSetting(QStringLiteral("control/cloudflareZone")));
        }
        if (m_importZoneEdit->text().trimmed().isEmpty()) {
            m_importZoneEdit->setText(
                forkmesh::control::cloudflareZoneNameFromVariables(variables));
        }
    }
    if (m_importAccountEdit) {
        m_importAccountEdit->setText(
            cloudflare.value(QStringLiteral("accountId")).toString());
    }
    if (m_importRelayHostEdit) {
        m_importRelayHostEdit->setText(
            cloudflare.value(QStringLiteral("hostname")).toString());
    }
    if (m_importMirrorHostEdit) {
        m_importMirrorHostEdit->setText(
            cloudflare.value(QStringLiteral("mirrorHostname")).toString());
    }

    const QJsonObject summary =
        bundle.value(QStringLiteral("summary")).toObject();
    if (m_importBundlePathLabel)
        m_importBundlePathLabel->setText(QDir::toNativeSeparators(path));
    if (m_importSummaryLabel) {
        const int skippedCount =
            summary.value(QStringLiteral("skippedCount")).toInt();
        m_importSummaryLabel->setText(
            QStringLiteral("%1 settings · %2 variables · %3 files · %4%5")
                .arg(summary.value(QStringLiteral("settingCount")).toInt())
                .arg(summary.value(QStringLiteral("variableCount")).toInt())
                .arg(summary.value(QStringLiteral("fileCount")).toInt())
                .arg(QLocale().formattedDataSize(
                         summary.value(QStringLiteral("uncompressedBytes"))
                             .toString()
                             .toLongLong()),
                     skippedCount > 0
                         ? QStringLiteral(" · %1 unsupported entries were "
                                          "skipped at export time")
                               .arg(skippedCount)
                         : QString()));
    }
    if (m_importReviewLabel) {
        m_importReviewLabel->setText(
            QStringLiteral("Integrity verified. Exported %1. Deploy will create "
                           "one billed Vultr instance, install ForkMesh, "
                           "configure its Cloudflare Tunnel, and wait for "
                           "healthy public traffic.")
                .arg(bundle.value(QStringLiteral("exportedAt")).toString()));
    }

    if (m_importValuesTable) {
        m_importValuesTable->setRowCount(0);
        appendDeploymentValueRows(m_importValuesTable, QString(), deployment,
                                  false);
        for (const QJsonValue &value :
             bundle.value(QStringLiteral("excludedApplicationData")).toArray()) {
            appendValueRow(m_importValuesTable,
                           QStringLiteral("Not in bundle"), value.toString(),
                           QStringLiteral("left out at export time"));
        }
        for (const QJsonValue &value :
             bundle.value(QStringLiteral("skipped")).toArray()) {
            const QJsonObject entry = value.toObject();
            appendValueRow(m_importValuesTable, QStringLiteral("Skipped"),
                           entry.value(QStringLiteral("path")).toString(),
                           entry.value(QStringLiteral("reason")).toString());
        }
        for (auto it = variables.constBegin(); it != variables.constEnd(); ++it) {
            appendValueRow(m_importValuesTable, QStringLiteral("Variables"),
                           it.key(),
                           maskedExportValue(it.key(), it.value(), false));
        }
        const QJsonObject settings =
            bundle.value(QStringLiteral("settings")).toObject();
        for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
            bool ok = false;
            const QVariant value = forkmesh::control::completeSystemBundleSetting(
                bundle, it.key(), &ok);
            appendValueRow(m_importValuesTable, QStringLiteral("Settings"),
                           it.key(),
                           maskedExportValue(it.key(),
                                             ok ? exportSettingText(value)
                                                : QStringLiteral("(unreadable)"),
                                             false));
        }
        for (const QJsonValue &value :
             bundle.value(QStringLiteral("files")).toArray()) {
            const QJsonObject file = value.toObject();
            appendValueRow(
                m_importValuesTable, QStringLiteral("Files"),
                file.value(QStringLiteral("path")).toString(),
                QLocale().formattedDataSize(
                    file.value(QStringLiteral("size")).toString().toLongLong()));
        }
    }
    if (m_importDeployButton)
        m_importDeployButton->setEnabled(true);
}

void MainWindow::deployImportedSystemBundle()
{
    if (m_importedSystemBundle.isEmpty() || !m_importVultrKeyEdit ||
        !m_importCloudflareTokenEdit || !m_importZoneEdit ||
        !m_importNodeNameEdit) {
        return;
    }
    const QString key = m_importVultrKeyEdit->text().trimmed();
    const QString requestedNode = m_importNodeNameEdit->text().trimmed().toLower();
    const QString invalid = forkmesh::control::validateVultrMirrorRequest(
        key, requestedNode.isEmpty() ? QStringLiteral("mirror1") : requestedNode);
    if (!invalid.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Cannot deploy"), invalid);
        return;
    }
    if (m_importCloudflareTokenEdit->text().trimmed().isEmpty() ||
        m_importZoneEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("Cannot deploy"),
            QStringLiteral("A Cloudflare API token and zone are required so "
                           "the new instance can serve live mirror traffic."));
        return;
    }
    const int confirmed = QMessageBox::warning(
        this, QStringLiteral("Deploy instance"),
        QStringLiteral(
            "This creates a billable Vultr server and applies the imported "
            "clear-text deployment credentials on this controller. Continue?"),
        QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);
    if (confirmed != QMessageBox::Ok)
        return;

    if (m_importApplySettingsCheck && m_importApplySettingsCheck->isChecked()) {
        QSettings settings;
        QString error;
        if (!forkmesh::control::mergeCompleteSystemBundleSettings(
                m_importedSystemBundle, settings, &error)) {
            QMessageBox::warning(this, QStringLiteral("Import failed"), error);
            return;
        }
        // A checkpoint describes an old paid-resource attempt. Import the
        // configuration, but always start the explicit new deployment as a
        // new attempt rather than resuming a stale server id.
        settings.remove(QStringLiteral("hosts/vultrProvision/v1"));
        settings.sync();
    }
    bool variablesOk = false;
    QMap<QString, QString> variables =
        forkmesh::control::completeSystemBundleVariables(m_importedSystemBundle,
                                                          &variablesOk);
    if (!variablesOk) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QStringLiteral("The variables section is invalid."));
        return;
    }
    variables.insert(QStringLiteral("VULTR_API_KEY"), key);
    variables.insert(QStringLiteral("CLOUDFLARE_API_TOKEN"),
                     m_importCloudflareTokenEdit->text().trimmed());
    variables.insert(QStringLiteral("CLOUDFLARE_ZONE"),
                     m_importZoneEdit->text().trimmed().toLower());
    const QString account =
        m_importAccountEdit ? m_importAccountEdit->text().trimmed() : QString();
    if (!account.isEmpty())
        variables.insert(QStringLiteral("CLOUDFLARE_ACCOUNT_ID"), account);
    ActionStore::setVariables(variables);
    QSettings settings;
    settings.setValue(QStringLiteral("control/cloudflareZone"),
                      m_importZoneEdit->text().trimmed().toLower());
    settings.setValue(QStringLiteral("control/cloudflareAccount"), account);
    if (m_importRelayHostEdit) {
        settings.setValue(QStringLiteral("control/cloudflareHostname"),
                          m_importRelayHostEdit->text().trimmed().toLower());
    }
    if (m_importMirrorHostEdit) {
        settings.setValue(QStringLiteral("control/cloudflareMirrorHostname"),
                          m_importMirrorHostEdit->text().trimmed().toLower());
    }
    settings.sync();

    const QString deployNode = requestedNode;
    const QString deployVultrKey = key;
    const bool deployAgents =
        !m_importAgentClisCheck || m_importAgentClisCheck->isChecked();
    showNetworkTab(kNetworkHostsTab);
    QTimer::singleShot(0, this,
                       [this, deployNode, deployVultrKey, deployAgents] {
        if (m_vultrApiKeyEdit)
            m_vultrApiKeyEdit->setText(deployVultrKey);
        if (m_vultrNameEdit)
            m_vultrNameEdit->setText(deployNode);
        if (m_vultrAgentClisCheck)
            m_vultrAgentClisCheck->setChecked(deployAgents);
        createVultrMirrorFromForm();
    });
}

QWidget *MainWindow::buildSiteDeployCard()
{
    QVBoxLayout *col = nullptr;
    QFrame *card =
        controlCard(QStringLiteral("ForkMesh site deployment"), &col);
    auto *systemRow = new QHBoxLayout;
    systemRow->setContentsMargins(0, 0, 0, 0);
    systemRow->setSpacing(8);
    auto *exportSystem =
        new QPushButton(QStringLiteral("Export complete system…"));
    exportSystem->setObjectName(QStringLiteral("openSystemExportPageButton"));
    exportSystem->setCursor(Qt::PointingHandCursor);
    setOcticon(exportSystem, QStringLiteral("download"), 14);
    exportSystem->setToolTip(QStringLiteral(
        "Open the Export system page: it lists every setting, variable, key, "
        "secret and application-data folder before anything is written."));
    connect(exportSystem, &QPushButton::clicked, this,
            &MainWindow::showSystemExportPage);
    systemRow->addWidget(exportSystem);
    auto *newDeployment = new QPushButton(QStringLiteral("New deployment…"));
    newDeployment->setObjectName(QStringLiteral("newDeploymentButton"));
    newDeployment->setProperty("buttonSize", "primary");
    newDeployment->setCursor(Qt::PointingHandCursor);
    setOcticon(newDeployment, QStringLiteral("rocket"), 14);
    newDeployment->setToolTip(QStringLiteral(
        "Open the Import and deploy page: choose a complete-system JSON, "
        "review what it fills in, then deploy a fresh instance from it."));
    connect(newDeployment, &QPushButton::clicked, this,
            &MainWindow::showSystemImportPage);
    systemRow->addWidget(newDeployment);
    systemRow->addStretch();
    col->addLayout(systemRow);
    col->addWidget(controlHint(
        QStringLiteral(
            "Deploy app, World, and www independently. The app target includes "
            "the backend API. Each action "
            "fingerprints only that Worker's inputs and exits without touching "
            "Cloudflare when nothing changed since its last successful deploy. "
            "Deploy changed workers checks all three in order. Force skips that "
            "change check for one run, so the next action deploys even when "
            "nothing changed. Credentials come "
            "from this machine's Wrangler login and production environment; "
            "output streams below.")));
    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(8);
    m_siteDeploySpinner = new BusySpinner(nullptr, 16);
    m_siteDeploySpinner->setObjectName(QStringLiteral("siteDeploySpinner"));
    m_siteDeploySpinner->hide();
    statusRow->addWidget(m_siteDeploySpinner, 0, Qt::AlignTop);
    m_siteDeployStatus = new QLabel;
    m_siteDeployStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_siteDeployStatus->setWordWrap(true);
    const QString deployScript = forkmesh::control::findSiteDeployScript(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
    m_siteDeployReadyStatus =
        deployScript.isEmpty()
            ? QStringLiteral(
                  "The Worker deployment orchestrator was not found next to "
                  "this checkout or installed resource bundle.")
            : QStringLiteral("Ready: %1 · unchanged workers will be skipped")
                  .arg(deployScript);
    m_siteDeployStatus->setText(m_siteDeployReadyStatus);
    statusRow->addWidget(m_siteDeployStatus, 1);
    col->addLayout(statusRow);

    auto *buttons = new QHBoxLayout;
    m_siteDeployForceButton = new QPushButton(QStringLiteral("Force"));
    m_siteDeployForceButton->setObjectName(
        QStringLiteral("siteDeployForceButton"));
    m_siteDeployForceButton->setCheckable(true);
    m_siteDeployForceButton->setCursor(Qt::PointingHandCursor);
    m_siteDeployForceButton->setToolTip(QStringLiteral(
        "Arm --force: the next deployment action uploads its Worker even when "
        "deploy.sh finds no input changes. Migrations, secrets and the "
        "post-deploy verification still run. Clears itself once that run "
        "starts."));
    setOcticon(m_siteDeployForceButton, QStringLiteral("alert"), 14);
    connect(m_siteDeployForceButton, &QPushButton::toggled, this,
            [this](bool armed) {
                if (!m_siteDeployStatus || m_siteDeployForced)
                    return;
                m_siteDeployStatus->setStyleSheet(QString());
                m_siteDeployStatus->show();
                m_siteDeployStatus->setText(
                    armed ? QStringLiteral(
                                "Force is armed: the next action deploys even "
                                "when nothing changed.")
                          : m_siteDeployReadyStatus);
            });
    buttons->addWidget(m_siteDeployForceButton);
    m_siteDeployTargetButtons.clear();
    const QList<QPair<QString, QString>> targets = {
        {QStringLiteral("changed"), QStringLiteral("Deploy changed workers")},
        {QStringLiteral("app"), QStringLiteral("Deploy app")},
        {QStringLiteral("world"), QStringLiteral("Deploy World")},
        {QStringLiteral("www"), QStringLiteral("Deploy www")},
    };
    for (const auto &target : targets) {
        auto *button = new QPushButton(target.second);
        const QString objectSuffix =
            target.first == QLatin1String("changed")
                ? QString()
                : target.first.left(1).toUpper() + target.first.mid(1);
        button->setObjectName(
            QStringLiteral("siteDeploy%1Button").arg(objectSuffix));
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(
            target.first == QLatin1String("changed")
                ? QStringLiteral(
                      "Deploy only app, World, or www workers whose "
                      "input fingerprint changed")
                : QStringLiteral(
                      "Deploy %1 only if its inputs changed since its last "
                      "successful deploy")
                      .arg(target.first));
        setOcticon(button,
                   target.first == QLatin1String("changed")
                       ? QStringLiteral("rocket")
                       : QStringLiteral("cloud"),
                   14);
        if (target.first == QLatin1String("changed")) {
            button->setProperty("buttonSize", "primary");
            m_siteDeployButton = button;
        }
        connect(button, &QPushButton::clicked, this,
                [this, deployTarget = target.first] {
                    runSiteDeploy(deployTarget);
                });
        buttons->addWidget(button);
        m_siteDeployTargetButtons.append(button);
    }
    m_siteDeployCancelButton = new QPushButton(QStringLiteral("Cancel"));
    m_siteDeployCancelButton->setObjectName(
        QStringLiteral("siteDeployCancelButton"));
    m_siteDeployCancelButton->setCursor(Qt::PointingHandCursor);
    m_siteDeployCancelButton->setEnabled(false);
    connect(m_siteDeployCancelButton, &QPushButton::clicked, this,
            &MainWindow::cancelSiteDeploy);
    buttons->addWidget(m_siteDeployCancelButton);
    buttons->addStretch();
    col->addLayout(buttons);

    m_siteDeployOutput = new QPlainTextEdit;
    m_siteDeployOutput->setObjectName(QStringLiteral("siteDeployOutput"));
    m_siteDeployOutput->setReadOnly(true);
    m_siteDeployOutput->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_siteDeployOutput->setMinimumHeight(220);
    m_siteDeployOutput->document()->setMaximumBlockCount(5000);
    m_siteDeployOutput->setPlaceholderText(
        QStringLiteral("deploy.sh output appears here live; credentials are "
                       "redacted."));
    applyLogFont(m_siteDeployOutput);
    col->addWidget(m_siteDeployOutput);
    if (Theme::isNext()) {
        QList<QPushButton *> extraTargets;
        for (QPushButton *button : std::as_const(m_siteDeployTargetButtons)) {
            if (button && button != m_siteDeployButton)
                extraTargets.append(button);
        }
        auto *more = new QPushButton(QStringLiteral("More targets"));
        more->setObjectName(QStringLiteral("siteDeployMoreButton"));
        more->setCursor(Qt::PointingHandCursor);
        more->setToolTip(QStringLiteral(
            "Deploy app, World or www on its own, arm Force for the next run, "
            "or export and import a complete system"));
        more->setLayoutDirection(Qt::RightToLeft);
        more->setProperty("nextIconRole", int(Theme::Role::FgMuted));
        setOcticon(more, QStringLiteral("kebab-horizontal"), 14);
        auto *menu = new QMenu(more);
        menu->setToolTipsVisible(true);
        QList<QPointer<QPushButton>> entries;
        for (QPushButton *button : std::as_const(extraTargets))
            entries.append(button);
        entries.append(QPointer<QPushButton>());
        entries.append(m_siteDeployForceButton);
        entries.append(QPointer<QPushButton>());
        entries.append(exportSystem);
        entries.append(newDeployment);
        connect(menu, &QMenu::aboutToShow, more, [menu, entries] {
            menu->clear();
            bool separate = false;
            for (const QPointer<QPushButton> &button : entries) {
                if (!button) {
                    separate = true;
                    continue;
                }
                if (separate && !menu->isEmpty())
                    menu->addSeparator();
                separate = false;
                QAction *action = menu->addAction(button->icon(), button->text());
                action->setToolTip(button->toolTip());
                action->setEnabled(button->isEnabled());
                if (button->isCheckable()) {
                    action->setCheckable(true);
                    action->setChecked(button->isChecked());
                }
                QPointer<QPushButton> target = button;
                QObject::connect(action, &QAction::triggered, menu, [target] {
                    if (target)
                        target->click();
                });
            }
        });
        more->setMenu(menu);
        if (m_siteDeployCancelButton) {
            m_siteDeployCancelButton->installEventFilter(new ControlShowWhenEnabled(card));
            m_siteDeployCancelButton->setVisible(m_siteDeployCancelButton->isEnabled());
        }
        QList<QWidget *> hidden;
        for (QPushButton *button : std::as_const(extraTargets))
            hidden.append(button);
        hidden << m_siteDeployForceButton << exportSystem << newDeployment;
        arrangeNextDeployCard(card, col, QStringLiteral("cloud"),
                              QStringLiteral("Deploy workers"), m_siteDeploySpinner,
                              m_siteDeployStatus,
                              {m_siteDeployButton, more, m_siteDeployCancelButton},
                              hidden);
    }
    return card;
}

void MainWindow::runSiteDeploy(const QString &target)
{
    if (m_siteDeployProcess &&
        m_siteDeployProcess->state() != QProcess::NotRunning) {
        flashMessage(QStringLiteral("A site deployment is already running."),
                     true);
        return;
    }
    static const QSet<QString> allowedTargets = {
        QStringLiteral("changed"), QStringLiteral("app"),
        QStringLiteral("world"), QStringLiteral("www")};
    const QString deployTarget = target.trimmed().toLower();
    if (!allowedTargets.contains(deployTarget)) {
        showLocalNotice(m_siteDeployStatus,
                        QStringLiteral("Unknown Worker deployment target."), true);
        return;
    }
    const bool force =
        m_siteDeployForceButton && m_siteDeployForceButton->isChecked();
    const QString script = forkmesh::control::findSiteDeployScript(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
    if (script.isEmpty()) {
        const QString missing = QStringLiteral(
            "The Worker deployment orchestrator was not found next to this "
            "checkout or installed resource bundle.");
        showLocalNotice(m_siteDeployStatus, missing, true);
        return;
    }
    const QString bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
    if (bash.isEmpty()) {
        showLocalNotice(
            m_siteDeployStatus,
            QStringLiteral("bash is required to run the Worker deploy orchestrator."),
            true);
        return;
    }
    const QStringList arguments =
        forkmesh::control::siteDeployArguments(script, deployTarget, force);
    if (arguments.isEmpty()) {
        showLocalNotice(m_siteDeployStatus,
                        QStringLiteral("Unknown Worker deployment target."), true);
        return;
    }
    if (m_siteDeployOutput)
        m_siteDeployOutput->clear();
    appendSiteDeployOutput(QStringLiteral("$ %1\n")
                               .arg(arguments.join(QLatin1Char(' '))));
    m_siteDeployForced = force;
    if (m_siteDeployStatus) {
        m_siteDeployStatus->setStyleSheet(QString());
        m_siteDeployStatus->show();
        m_siteDeployStatus->setText(
            force ? QStringLiteral("Force deploying %1 without the change "
                                   "check…")
                        .arg(deployTarget == QLatin1String("changed")
                                 ? QStringLiteral("every worker")
                                 : deployTarget)
                  : deployTarget == QLatin1String("changed")
                        ? QStringLiteral("Checking and deploying changed "
                                         "workers…")
                        : QStringLiteral("Checking %1 for changes…")
                              .arg(deployTarget));
    }
    // Force is armed for one run: clear it now so the next click cannot skip
    // the change check by accident.
    if (m_siteDeployForceButton) {
        m_siteDeployForceButton->setChecked(false);
        m_siteDeployForceButton->setEnabled(false);
    }
    for (QPushButton *button : std::as_const(m_siteDeployTargetButtons)) {
        if (button)
            button->setEnabled(false);
    }
    if (m_siteDeployCancelButton)
        m_siteDeployCancelButton->setEnabled(true);
    setSiteDeployRunning(true);
    m_siteDeployTarget = deployTarget;
    logSystem(force
                  ? QStringLiteral("Control node: %1 Worker deployment started "
                                   "with --force.")
                        .arg(deployTarget)
                  : QStringLiteral("Control node: %1 Worker deployment "
                                   "started.")
                        .arg(deployTarget));

    auto *process = new QProcess(this);
    m_siteDeployProcess = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(QFileInfo(script).absolutePath());
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    process->setProcessEnvironment(environment);
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process] {
                appendSiteDeployOutput(
                    QString::fromUtf8(process->readAllStandardOutput()));
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    appendSiteDeployOutput(
                        QStringLiteral(
                            "deploy.sh could not start; verify bash and the "
                            "Worker bundle on this machine.\n"));
                    if (m_siteDeployStatus) {
                        m_siteDeployStatus->setText(
                            QStringLiteral("deploy.sh could not start."));
                    }
                    for (QPushButton *button :
                         std::as_const(m_siteDeployTargetButtons)) {
                        if (button)
                            button->setEnabled(true);
                    }
                    if (m_siteDeployForceButton)
                        m_siteDeployForceButton->setEnabled(true);
                    m_siteDeployForced = false;
                    if (m_siteDeployCancelButton)
                        m_siteDeployCancelButton->setEnabled(false);
                    setSiteDeployRunning(false);
                    if (m_siteDeployProcess == process)
                        m_siteDeployProcess = nullptr;
                    process->deleteLater();
                }
            });
    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus status) {
                appendSiteDeployOutput(
                    QString::fromUtf8(process->readAllStandardOutput()));
                const bool ok =
                    status == QProcess::NormalExit && exitCode == 0;
                appendSiteDeployOutput(
                    ok ? QStringLiteral("\nDeployment finished successfully.\n")
                       : QStringLiteral("\nDeployment failed (exit %1).\n")
                             .arg(exitCode));
                const QString completedTarget = m_siteDeployTarget;
                const bool forced = m_siteDeployForced;
                if (m_siteDeployStatus)
                    m_siteDeployStatus->setText(
                        !ok ? QStringLiteral("%1 deployment failed (exit %2).")
                                  .arg(completedTarget)
                                  .arg(exitCode)
                        : forced
                            ? QStringLiteral("%1 was force deployed without "
                                             "the change check.")
                                  .arg(completedTarget)
                            : QStringLiteral("%1 deployment check succeeded; "
                                             "unchanged workers were skipped.")
                                  .arg(completedTarget));
                m_siteDeployForced = false;
                for (QPushButton *button :
                     std::as_const(m_siteDeployTargetButtons)) {
                    if (button)
                        button->setEnabled(true);
                }
                if (m_siteDeployForceButton)
                    m_siteDeployForceButton->setEnabled(true);
                if (m_siteDeployCancelButton)
                    m_siteDeployCancelButton->setEnabled(false);
                setSiteDeployRunning(false);
                logSystem(ok
                              ? QStringLiteral(
                                    "Control node: %1 deployment succeeded.")
                                    .arg(completedTarget)
                              : QStringLiteral(
                                    "Control node: %1 deployment failed "
                                    "(exit %2).")
                                    .arg(completedTarget)
                                    .arg(exitCode));
                if (m_siteDeployProcess == process)
                    m_siteDeployProcess = nullptr;
                process->deleteLater();
            });
    process->start(bash, arguments);
}

void MainWindow::setSiteDeployRunning(bool running)
{
    if (m_siteDeploySpinner)
        m_siteDeploySpinner->setVisible(running);
    m_siteDeployActive = running;
    updateControlDeployActivity();
}

void MainWindow::updateControlDeployActivity()
{
    if (auto *rail =
            dynamic_cast<ActivityRailButton *>(m_controlNodeNavButton))
        rail->setActivityBlink(m_siteDeployActive || m_nodeDeployActive);
}

void MainWindow::cancelSiteDeploy()
{
    if (!m_siteDeployProcess ||
        m_siteDeployProcess->state() == QProcess::NotRunning) {
        return;
    }
    appendSiteDeployOutput(
        QStringLiteral("Cancellation requested; waiting for deploy.sh to "
                       "stop.\n"));
    m_siteDeployProcess->terminate();
    QPointer<QProcess> process(m_siteDeployProcess);
    QTimer::singleShot(5000, this, [process] {
        if (process && process->state() != QProcess::NotRunning)
            process->kill();
    });
}

void MainWindow::appendSiteDeployOutput(const QString &text)
{
    if (!m_siteDeployOutput || text.isEmpty())
        return;
    const QString safe = forkmesh::control::redactProcessOutput(
        text, {m_cloudflareActiveSecret});
    m_siteDeployOutput->moveCursor(QTextCursor::End);
    m_siteDeployOutput->insertPlainText(safe);
    m_siteDeployOutput->moveCursor(QTextCursor::End);
    m_siteDeployOutput->ensureCursorVisible();
}

QWidget *MainWindow::buildNodeDeployCard()
{
    QVBoxLayout *col = nullptr;
    QFrame *card =
        controlCard(QStringLiteral("Core node deployment"), &col);
    col->addWidget(controlHint(
        QStringLiteral(
            "Deploy this checkout's main branch to every core node. The same "
            "gated run as an auto-deploy: main must fast-forward what the "
            "nodes serve, the release tests pass, and the app is built in a "
            "clean worktree, uploaded, migrated, and restarted one node at a "
            "time, each gated on its health and version. A node that fails "
            "is rolled back and the rollout stops. If every node already "
            "runs main, nothing is deployed. Fleet keys are read in place "
            "and never printed; output streams below.")));
    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(8);
    m_nodeDeploySpinner = new BusySpinner(nullptr, 16);
    m_nodeDeploySpinner->setObjectName(QStringLiteral("nodeDeploySpinner"));
    m_nodeDeploySpinner->hide();
    statusRow->addWidget(m_nodeDeploySpinner, 0, Qt::AlignTop);
    m_nodeDeployStatus = new QLabel;
    m_nodeDeployStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_nodeDeployStatus->setWordWrap(true);
    const QString script = forkmesh::control::findNextAutodeployScript(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
    m_nodeDeployStatus->setText(
        script.isEmpty()
            ? QStringLiteral("tools/next_autodeploy.py was not found next to "
                             "this app or in its source checkout.")
            : QStringLiteral("Ready: %1").arg(script));
    statusRow->addWidget(m_nodeDeployStatus, 1);
    col->addLayout(statusRow);

    auto *buttons = new QHBoxLayout;
    m_nodeDeployButton =
        new QPushButton(QStringLiteral("Deploy app to all nodes"));
    m_nodeDeployButton->setObjectName(QStringLiteral("nodeDeployButton"));
    m_nodeDeployButton->setProperty("buttonSize", "primary");
    m_nodeDeployButton->setCursor(Qt::PointingHandCursor);
    m_nodeDeployButton->setToolTip(QStringLiteral(
        "Run next_autodeploy.py deploy --commit main: gates, tests, build, "
        "then a rolling restart of every core node"));
    setOcticon(m_nodeDeployButton, QStringLiteral("rocket"), 14);
    m_nodeDeployButton->setEnabled(!script.isEmpty());
    connect(m_nodeDeployButton, &QPushButton::clicked, this,
            &MainWindow::runNodeDeploy);
    buttons->addWidget(m_nodeDeployButton);
    buttons->addStretch();
    col->addLayout(buttons);

    m_nodeDeployOutput = new QPlainTextEdit;
    m_nodeDeployOutput->setObjectName(QStringLiteral("nodeDeployOutput"));
    m_nodeDeployOutput->setReadOnly(true);
    m_nodeDeployOutput->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_nodeDeployOutput->setMinimumHeight(220);
    m_nodeDeployOutput->document()->setMaximumBlockCount(5000);
    m_nodeDeployOutput->setPlaceholderText(
        QStringLiteral("next_autodeploy.py output appears here live."));
    applyLogFont(m_nodeDeployOutput);
    col->addWidget(m_nodeDeployOutput);
    if (Theme::isNext())
        arrangeNextDeployCard(card, col, QStringLiteral("rocket"),
                              QStringLiteral("Deploy the app to every node"),
                              m_nodeDeploySpinner, m_nodeDeployStatus,
                              {m_nodeDeployButton}, {});
    return card;
}

void MainWindow::runNodeDeploy()
{
    if (m_nodeDeployProcess &&
        m_nodeDeployProcess->state() != QProcess::NotRunning) {
        flashMessage(QStringLiteral("A core node deployment is already "
                                    "running."),
                     true);
        return;
    }
    const QString script = forkmesh::control::findNextAutodeployScript(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
    if (script.isEmpty()) {
        const QString missing = QStringLiteral(
            "tools/next_autodeploy.py was not found next to this app or in "
            "its source checkout.");
        showLocalNotice(m_nodeDeployStatus, missing, true);
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("Deploy app to all nodes"),
            QStringLiteral(
                "Deploy main to every core node now? Production restarts "
                "one node at a time once the gates and tests pass.")) !=
        QMessageBox::Yes)
        return;

    const QString python = forkmesh::autoscale::fleetPython(script);
    const QStringList arguments = {script, QStringLiteral("deploy"),
                                   QStringLiteral("--commit"),
                                   QStringLiteral("main")};
    if (m_nodeDeployOutput)
        m_nodeDeployOutput->clear();
    appendNodeDeployOutput(QStringLiteral("$ %1 %2\n")
                               .arg(python, arguments.join(QLatin1Char(' '))));
    if (m_nodeDeployStatus) {
        m_nodeDeployStatus->setStyleSheet(QString());
        m_nodeDeployStatus->show();
        m_nodeDeployStatus->setText(
            QStringLiteral("Deploying main to every core node…"));
    }
    if (m_nodeDeployButton)
        m_nodeDeployButton->setEnabled(false);
    setNodeDeployRunning(true);
    logSystem(QStringLiteral("Control node: core node deployment of main "
                             "started."));

    auto *process = new QProcess(this);
    m_nodeDeployProcess = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(QFileInfo(script).absolutePath());
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    process->setProcessEnvironment(environment);
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process] {
                appendNodeDeployOutput(
                    QString::fromUtf8(process->readAllStandardOutput()));
            });
    const auto finish = [this, process](bool ok, const QString &status) {
        if (m_nodeDeployStatus)
            m_nodeDeployStatus->setText(status);
        if (m_nodeDeployButton)
            m_nodeDeployButton->setEnabled(true);
        setNodeDeployRunning(false);
        logSystem(QStringLiteral("Control node: core node deployment %1: %2")
                      .arg(ok ? QStringLiteral("succeeded")
                              : QStringLiteral("failed"),
                           status));
        if (m_nodeDeployProcess == process)
            m_nodeDeployProcess = nullptr;
        process->deleteLater();
    };
    connect(process, &QProcess::errorOccurred, this,
            [this, process, python, finish](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                appendNodeDeployOutput(
                    QStringLiteral("%1 could not start.\n").arg(python));
                finish(false, QStringLiteral("%1 could not start.").arg(python));
            });
    connect(process, &QProcess::finished, this,
            [this, process, finish](int exitCode,
                                    QProcess::ExitStatus exitStatus) {
                appendNodeDeployOutput(
                    QString::fromUtf8(process->readAllStandardOutput()));
                const bool ok =
                    exitStatus == QProcess::NormalExit && exitCode == 0;
                QString result;
                if (m_nodeDeployOutput) {
                    static const QRegularExpression resultLine(
                        QStringLiteral("result: (?:ok|FAILED): (.*)$"),
                        QRegularExpression::MultilineOption);
                    QRegularExpressionMatchIterator it = resultLine.globalMatch(
                        m_nodeDeployOutput->toPlainText());
                    while (it.hasNext())
                        result = it.next().captured(1).trimmed();
                }
                appendNodeDeployOutput(
                    ok ? QStringLiteral("\nCore node deployment finished.\n")
                       : QStringLiteral("\nCore node deployment failed "
                                        "(exit %1).\n")
                             .arg(exitCode));
                finish(ok, ok ? (result.isEmpty()
                                     ? QStringLiteral("Deployment finished.")
                                     : result)
                              : result.isEmpty()
                                  ? QStringLiteral("Deployment failed "
                                                   "(exit %1).")
                                        .arg(exitCode)
                                  : QStringLiteral("Deployment failed: %1")
                                        .arg(result));
            });
    process->start(python, arguments);
}

void MainWindow::setNodeDeployRunning(bool running)
{
    if (m_nodeDeploySpinner)
        m_nodeDeploySpinner->setVisible(running);
    m_nodeDeployActive = running;
    updateControlDeployActivity();
}

void MainWindow::appendNodeDeployOutput(const QString &text)
{
    if (!m_nodeDeployOutput || text.isEmpty())
        return;
    const QString safe = forkmesh::control::redactProcessOutput(
        text, {m_cloudflareActiveSecret});
    m_nodeDeployOutput->moveCursor(QTextCursor::End);
    m_nodeDeployOutput->insertPlainText(safe);
    m_nodeDeployOutput->moveCursor(QTextCursor::End);
    m_nodeDeployOutput->ensureCursorVisible();
}

QWidget *MainWindow::buildCloudflareTokenCard()
{
    QVBoxLayout *col = nullptr;
    QFrame *card =
        controlCard(QStringLiteral("Cloudflare API token"), &col);
    col->addWidget(controlHint(
        QStringLiteral(
            "Checks a Cloudflare API token against the exact permissions this "
            "checkout's deploy path needs. The token's own policy is read when "
            "it may read itself; otherwise each capability is confirmed with a "
            "read-only probe, which proves reach but never edit rights. "
            "Generating a replacement mints a token scoped to the table below "
            "and stores it in this device's CLOUDFLARE_API_TOKEN variable and in "
            "app/.env.production — the two places deploys read it "
            "from. Token values are shown only as their last four characters "
            "and are redacted from the output below; the previous token stays "
            "valid until you delete it in the Cloudflare dashboard.")));

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    m_controlTokenEdit = new QLineEdit(
        forkmesh::control::cloudflareApiTokenFromVariables(
            ActionStore::variables()));
    m_controlTokenEdit->setObjectName(QStringLiteral("controlTokenValue"));
    m_controlTokenEdit->setEchoMode(QLineEdit::Password);
    m_controlTokenEdit->setClearButtonEnabled(true);
    m_controlTokenEdit->setPlaceholderText(
        QStringLiteral("empty: use this device's saved CLOUDFLARE_API_TOKEN"));
    form->addRow(QStringLiteral("API token"), m_controlTokenEdit);
    col->addLayout(form);

    auto *buttons = new QHBoxLayout;
    m_controlTokenTestButton =
        new QPushButton(QStringLiteral("Check token permissions"));
    m_controlTokenTestButton->setObjectName(
        QStringLiteral("controlTokenTestButton"));
    m_controlTokenTestButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_controlTokenTestButton, QStringLiteral("shield-check"), 14);
    connect(m_controlTokenTestButton, &QPushButton::clicked, this,
            &MainWindow::testCloudflareApiToken);
    buttons->addWidget(m_controlTokenTestButton);
    m_controlTokenGenerateButton =
        new QPushButton(QStringLiteral("Generate and install a new token"));
    m_controlTokenGenerateButton->setObjectName(
        QStringLiteral("controlTokenGenerateButton"));
    m_controlTokenGenerateButton->setProperty("buttonSize", "primary");
    m_controlTokenGenerateButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_controlTokenGenerateButton, QStringLiteral("key"), 14);
    connect(m_controlTokenGenerateButton, &QPushButton::clicked, this,
            &MainWindow::generateCloudflareApiToken);
    buttons->addWidget(m_controlTokenGenerateButton);
    buttons->addStretch();
    col->addLayout(buttons);

    m_controlTokenStatus = new QLabel(
        QStringLiteral("No token has been checked in this session."));
    m_controlTokenStatus->setObjectName(QStringLiteral("controlTokenStatus"));
    m_controlTokenStatus->setWordWrap(true);
    m_controlTokenStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(m_controlTokenStatus);

    m_controlTokenTable = new QTableWidget(0, 5);
    m_controlTokenTable->setObjectName(
        QStringLiteral("controlTokenPermissionsTable"));
    m_controlTokenTable->setHorizontalHeaderLabels(
        {QStringLiteral("Cloudflare permission"), QStringLiteral("ForkMesh needs"),
         QStringLiteral("Token policy"), QStringLiteral("Live check"),
         QStringLiteral("Used for")});
    m_controlTokenTable->verticalHeader()->setVisible(false);
    m_controlTokenTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_controlTokenTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_controlTokenTable->setShowGrid(false);
    m_controlTokenTable->setWordWrap(true);
    for (int column = 0; column < 4; ++column) {
        m_controlTokenTable->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    m_controlTokenTable->horizontalHeader()->setSectionResizeMode(
        4, QHeaderView::Stretch);
    m_controlTokenTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    col->addWidget(m_controlTokenTable);

    m_controlTokenTargets = new QLabel;
    m_controlTokenTargets->setObjectName(QStringLiteral("mutedLabel"));
    m_controlTokenTargets->setWordWrap(true);
    const QString envPath = forkmesh::control::siteDeployEnvFilePath(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
    m_controlTokenTargets->setText(
        envPath.isEmpty()
            ? QStringLiteral(
                  "A new token is written to this device's CLOUDFLARE_API_TOKEN "
                  "variable. app/.env.production was not found "
                  "next to this build's Worker bundle, so no file is rewritten.")
            : QStringLiteral(
                  "A new token replaces CLOUDFLARE_API_TOKEN in this device's "
                  "variables and in %1.")
                  .arg(envPath));
    col->addWidget(m_controlTokenTargets);

    m_controlTokenOutput = new QPlainTextEdit;
    m_controlTokenOutput->setObjectName(QStringLiteral("controlTokenOutput"));
    m_controlTokenOutput->setReadOnly(true);
    m_controlTokenOutput->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_controlTokenOutput->setMinimumHeight(120);
    m_controlTokenOutput->document()->setMaximumBlockCount(500);
    m_controlTokenOutput->setPlaceholderText(
        QStringLiteral("Cloudflare API calls made by this tab appear here; "
                       "credentials are redacted."));
    applyLogFont(m_controlTokenOutput);
    col->addWidget(m_controlTokenOutput);

    renderCloudflareTokenReport();
    return card;
}

QString MainWindow::resolvedCloudflareApiToken() const
{
    if (m_controlTokenEdit) {
        const QString typed = m_controlTokenEdit->text().trimmed();
        if (!typed.isEmpty())
            return typed;
    }
    if (m_cloudflareTokenEdit) {
        const QString deployToken = m_cloudflareTokenEdit->text().trimmed();
        if (!deployToken.isEmpty())
            return deployToken;
    }
    return forkmesh::control::cloudflareApiTokenFromVariables(
        ActionStore::variables());
}

void MainWindow::setCloudflareTokenBusy(bool busy)
{
    m_controlTokenBusy = busy;
    if (m_controlTokenTestButton)
        m_controlTokenTestButton->setEnabled(!busy);
    if (m_controlTokenGenerateButton)
        m_controlTokenGenerateButton->setEnabled(!busy);
}

void MainWindow::endCloudflareTokenRun()
{
    setCloudflareTokenBusy(false);
    m_controlTokenSecret.clear();
}

void MainWindow::appendCloudflareTokenOutput(const QString &text)
{
    if (!m_controlTokenOutput || text.isEmpty())
        return;
    const QString safe = forkmesh::control::redactProcessOutput(
        text, {m_controlTokenSecret, m_cloudflareActiveSecret});
    m_controlTokenOutput->moveCursor(QTextCursor::End);
    m_controlTokenOutput->insertPlainText(safe);
    m_controlTokenOutput->moveCursor(QTextCursor::End);
    m_controlTokenOutput->ensureCursorVisible();
}

void MainWindow::testCloudflareApiToken()
{
    if (m_controlTokenBusy)
        return;
    const QString token = resolvedCloudflareApiToken();
    if (token.isEmpty()) {
        if (m_controlTokenStatus)
            m_controlTokenStatus->setText(QStringLiteral(
                "No Cloudflare API token: paste one above, or save "
                "CLOUDFLARE_API_TOKEN in Settings > Secrets & Coves."));
        return;
    }
    if (!forkmesh::control::isPlausibleCloudflareApiToken(token)) {
        if (m_controlTokenStatus)
            m_controlTokenStatus->setText(QStringLiteral(
                "That value does not look like a Cloudflare API token (40 "
                "characters, no spaces). Nothing was sent."));
        return;
    }

    m_controlTokenSecret = token;
    m_controlTokenGrantedGroups.clear();
    m_controlTokenProbeResults.clear();
    m_controlTokenPolicyReadable = false;
    m_controlTokenUserResource.clear();
    m_controlTokenZoneId.clear();
    m_controlTokenAccountId =
        m_cloudflareAccountEdit ? m_cloudflareAccountEdit->text().trimmed()
                                : QString();
    if (m_controlTokenAccountId.isEmpty()) {
        m_controlTokenAccountId =
            forkmesh::control::cloudflareAccountIdFromVariables(
                ActionStore::variables());
    }
    setCloudflareTokenBusy(true);
    if (m_controlTokenOutput)
        m_controlTokenOutput->clear();
    const QString masked = forkmesh::control::maskedTokenSuffix(token);
    if (m_controlTokenStatus) {
        m_controlTokenStatus->setText(
            QStringLiteral("Checking token %1 against Cloudflare…").arg(masked));
    }
    appendCloudflareTokenOutput(
        QStringLiteral("GET /user/tokens/verify (token %1)\n").arg(masked));
    renderCloudflareTokenReport();

    cloudflareApiCall(
        token, QStringLiteral("/user/tokens/verify"),
        QByteArrayLiteral("GET"), {},
        [this, token, masked](const QJsonObject &response,
                              const QString &error) {
            if (!error.isEmpty()) {
                appendCloudflareTokenOutput(error + QLatin1Char('\n'));
                if (m_controlTokenStatus) {
                    m_controlTokenStatus->setText(
                        QStringLiteral("Token %1 was rejected by Cloudflare: %2")
                            .arg(masked, error));
                }
                endCloudflareTokenRun();
                return;
            }
            const QString status =
                forkmesh::control::cloudflareTokenVerifyStatus(response);
            appendCloudflareTokenOutput(
                QStringLiteral("Token status: %1\n")
                    .arg(status.isEmpty() ? QStringLiteral("unknown") : status));
            if (status != QLatin1String("active")) {
                if (m_controlTokenStatus) {
                    m_controlTokenStatus->setText(
                        QStringLiteral(
                            "Token %1 is %2, so no permission can be used. "
                            "Generate a replacement below or re-enable it in the "
                            "Cloudflare dashboard.")
                            .arg(masked, status.isEmpty()
                                             ? QStringLiteral("not active")
                                             : status));
                }
                endCloudflareTokenRun();
                return;
            }
            checkCloudflareTokenPolicies(
                token, forkmesh::control::cloudflareTokenVerifyId(response));
        });
}

void MainWindow::checkCloudflareTokenPolicies(const QString &token,
                                              const QString &tokenId)
{
    if (tokenId.isEmpty()) {
        appendCloudflareTokenOutput(QStringLiteral(
            "Cloudflare did not return this token's id, so its policy cannot be "
            "read; falling back to read-only probes.\n"));
        resolveCloudflareTokenTopology(token);
        return;
    }
    appendCloudflareTokenOutput(
        QStringLiteral("GET /user/tokens/%1\n").arg(tokenId));
    cloudflareApiCall(
        token, QStringLiteral("/user/tokens/") + tokenId,
        QByteArrayLiteral("GET"), {},
        [this, token](const QJsonObject &response, const QString &error) {
            if (error.isEmpty()) {
                m_controlTokenPolicyReadable = true;
                m_controlTokenGrantedGroups =
                    forkmesh::control::cloudflareTokenPermissionGroupNames(
                        response);
                m_controlTokenUserResource =
                    forkmesh::control::cloudflareTokenUserResourceKey(response);
                const QStringList accounts =
                    forkmesh::control::cloudflareTokenAccountIds(response);
                if (m_controlTokenAccountId.isEmpty() && accounts.size() == 1)
                    m_controlTokenAccountId = accounts.first();
                appendCloudflareTokenOutput(
                    QStringLiteral("Token policy grants: %1\n")
                        .arg(m_controlTokenGrantedGroups.isEmpty()
                                 ? QStringLiteral("(no permission group)")
                                 : m_controlTokenGrantedGroups.join(
                                       QStringLiteral(", "))));
            } else {
                appendCloudflareTokenOutput(
                    QStringLiteral(
                        "Token policy is not readable (%1); using read-only "
                        "probes instead.\n")
                        .arg(error));
            }
            resolveCloudflareTokenTopology(token);
        });
}

void MainWindow::resolveCloudflareTokenTopology(const QString &token)
{
    if (m_controlTokenAccountId.isEmpty()) {
        appendCloudflareTokenOutput(QStringLiteral("GET /accounts\n"));
        cloudflareApiCall(
            token, QStringLiteral("/accounts?per_page=2"),
            QByteArrayLiteral("GET"), {},
            [this, token](const QJsonObject &response, const QString &error) {
                const QJsonArray accounts =
                    response.value(QStringLiteral("result")).toArray();
                if (error.isEmpty() && accounts.size() == 1) {
                    m_controlTokenAccountId = accounts.first()
                                                  .toObject()
                                                  .value(QStringLiteral("id"))
                                                  .toString()
                                                  .trimmed();
                    appendCloudflareTokenOutput(
                        QStringLiteral("Account: %1\n")
                            .arg(m_controlTokenAccountId));
                } else if (error.isEmpty()) {
                    appendCloudflareTokenOutput(QStringLiteral(
                        "The token reaches %1 accounts; set Account ID on the "
                        "Cloudflare relay tab so checks are unambiguous.\n")
                            .arg(accounts.size()));
                } else {
                    appendCloudflareTokenOutput(
                        QStringLiteral("Accounts are not listable (%1).\n")
                            .arg(error));
                }
                if (m_controlTokenAccountId.isEmpty())
                    m_controlTokenAccountId = QStringLiteral("-");
                resolveCloudflareTokenTopology(token);
            });
        return;
    }

    if (m_controlTokenZoneId.isEmpty()) {
        QString zoneName = m_cloudflareZoneEdit
                               ? m_cloudflareZoneEdit->text().trimmed().toLower()
                               : QString();
        if (zoneName.isEmpty()) {
            zoneName = forkmesh::control::cloudflareZoneNameFromVariables(
                ActionStore::variables());
        }
        appendCloudflareTokenOutput(
            zoneName.isEmpty()
                ? QStringLiteral("GET /zones\n")
                : QStringLiteral("GET /zones?name=%1\n").arg(zoneName));
        const QString path =
            zoneName.isEmpty()
                ? QStringLiteral("/zones?per_page=2")
                : QStringLiteral("/zones?name=") +
                      QString::fromLatin1(
                          QUrl::toPercentEncoding(zoneName));
        cloudflareApiCall(
            token, path, QByteArrayLiteral("GET"), {},
            [this, token, zoneName](const QJsonObject &response,
                                    const QString &error) {
                const QJsonArray zones =
                    response.value(QStringLiteral("result")).toArray();
                if (error.isEmpty() && !zoneName.isEmpty()) {
                    m_controlTokenZoneId =
                        forkmesh::control::cloudflareZoneId(zones, zoneName);
                } else if (error.isEmpty() && zones.size() == 1) {
                    m_controlTokenZoneId = zones.first()
                                               .toObject()
                                               .value(QStringLiteral("id"))
                                               .toString()
                                               .trimmed();
                }
                if (!m_controlTokenZoneId.isEmpty()) {
                    appendCloudflareTokenOutput(
                        QStringLiteral("Zone: %1\n").arg(m_controlTokenZoneId));
                } else if (error.isEmpty()) {
                    appendCloudflareTokenOutput(QStringLiteral(
                        "No single zone matched; set Cloudflare zone on the "
                        "relay tab to check the DNS permission.\n"));
                } else {
                    appendCloudflareTokenOutput(
                        QStringLiteral("Zones are not listable (%1).\n")
                            .arg(error));
                }
                if (m_controlTokenZoneId.isEmpty())
                    m_controlTokenZoneId = QStringLiteral("-");
                resolveCloudflareTokenTopology(token);
            });
        return;
    }

    runCloudflareTokenProbe(token, 0);
}

void MainWindow::runCloudflareTokenProbe(const QString &token, int index)
{
    const QList<forkmesh::control::CloudflareTokenRequirement> requirements =
        forkmesh::control::cloudflareTokenRequirements();
    if (index >= requirements.size()) {
        endCloudflareTokenRun();
        renderCloudflareTokenReport();
        return;
    }
    const forkmesh::control::CloudflareTokenRequirement requirement =
        requirements.at(index);
    const QString path = forkmesh::control::cloudflareTokenProbePath(
        requirement, m_controlTokenAccountId, m_controlTokenZoneId);
    if (path.isEmpty()) {
        m_controlTokenProbeResults.insert(
            requirement.key,
            requirement.probePath.isEmpty()
                ? QStringLiteral("no read-only probe")
                : QStringLiteral("skipped: id unknown"));
        runCloudflareTokenProbe(token, index + 1);
        return;
    }
    appendCloudflareTokenOutput(QStringLiteral("GET %1\n").arg(path));
    cloudflareApiCall(
        token, path, QByteArrayLiteral("GET"), {},
        [this, token, index, requirement](const QJsonObject &,
                                          const QString &error) {
            m_controlTokenProbeResults.insert(
                requirement.key,
                error.isEmpty()
                    ? QStringLiteral("read access confirmed")
                    : QStringLiteral("denied"));
            if (!error.isEmpty())
                appendCloudflareTokenOutput(error + QLatin1Char('\n'));
            runCloudflareTokenProbe(token, index + 1);
        });
}

void MainWindow::renderCloudflareTokenReport()
{
    if (!m_controlTokenTable)
        return;
    const QList<forkmesh::control::CloudflareTokenRequirement> requirements =
        forkmesh::control::cloudflareTokenRequirements();
    m_controlTokenTable->setRowCount(requirements.size());
    QStringList missing;
    QStringList denied;
    int requiredCount = 0;
    int grantedCount = 0;
    for (int row = 0; row < requirements.size(); ++row) {
        const forkmesh::control::CloudflareTokenRequirement requirement =
            requirements.at(row);
        const bool granted = forkmesh::control::cloudflareTokenGrantsRequirement(
            requirement, m_controlTokenGrantedGroups);
        const QString probe = m_controlTokenProbeResults.value(requirement.key);
        if (requirement.required) {
            ++requiredCount;
            if (m_controlTokenPolicyReadable && granted)
                ++grantedCount;
            if (m_controlTokenPolicyReadable && !granted)
                missing.append(requirement.label);
            if (probe == QLatin1String("denied"))
                denied.append(requirement.label);
        }
        const auto cell = [](const QString &text) {
            auto *item = new QTableWidgetItem(text);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            return item;
        };
        m_controlTokenTable->setItem(row, 0, cell(requirement.label));
        m_controlTokenTable->setItem(
            row, 1,
            cell(requirement.required ? QStringLiteral("Required")
                                      : QStringLiteral("Optional")));
        m_controlTokenTable->setItem(
            row, 2,
            cell(!m_controlTokenPolicyReadable
                     ? QStringLiteral("not readable")
                     : granted ? QStringLiteral("granted")
                               : QStringLiteral("missing")));
        m_controlTokenTable->setItem(
            row, 3, cell(probe.isEmpty() ? QStringLiteral("not checked") : probe));
        m_controlTokenTable->setItem(row, 4, cell(requirement.purpose));
    }
    m_controlTokenTable->resizeRowsToContents();
    int tableHeight = m_controlTokenTable->horizontalHeader()->height() + 6;
    for (int row = 0; row < m_controlTokenTable->rowCount(); ++row)
        tableHeight += m_controlTokenTable->rowHeight(row);
    m_controlTokenTable->setMinimumHeight(tableHeight);

    if (!m_controlTokenStatus || m_controlTokenBusy)
        return;
    if (m_controlTokenSecret.isEmpty() && m_controlTokenProbeResults.isEmpty() &&
        !m_controlTokenPolicyReadable) {
        return; // never checked in this session: keep the initial hint
    }
    QStringList lines;
    if (m_controlTokenPolicyReadable) {
        lines << QStringLiteral("Token policy: %1 of %2 required permissions "
                                "granted.")
                     .arg(grantedCount)
                     .arg(requiredCount);
        if (!missing.isEmpty()) {
            lines << QStringLiteral("Missing: %1.")
                         .arg(missing.join(QStringLiteral(", ")));
        }
    } else {
        lines << QStringLiteral(
            "The token cannot read its own policy (that needs API Tokens: "
            "Read), so the live checks below are what ForkMesh can confirm.");
    }
    if (!denied.isEmpty()) {
        lines << QStringLiteral("Live checks denied: %1.")
                     .arg(denied.join(QStringLiteral(", ")));
    } else {
        lines << QStringLiteral(
            "Every probed capability answered; edit rights cannot be probed "
            "without writing, so a deploy is still the final proof.");
    }
    m_controlTokenStatus->setText(lines.join(QLatin1Char(' ')));
}

void MainWindow::generateCloudflareApiToken()
{
    if (m_controlTokenBusy)
        return;
    const QString token = resolvedCloudflareApiToken();
    if (token.isEmpty() ||
        !forkmesh::control::isPlausibleCloudflareApiToken(token)) {
        if (m_controlTokenStatus)
            m_controlTokenStatus->setText(QStringLiteral(
                "Minting a token needs a current Cloudflare token with API "
                "Tokens: Edit. Paste one above first."));
        return;
    }
    const QString account = m_controlTokenAccountId;
    const QString zone = m_controlTokenZoneId;
    static const QRegularExpression idPattern(
        QStringLiteral("^[A-Za-z0-9]{1,64}$"));
    if (!idPattern.match(account).hasMatch() ||
        !idPattern.match(zone).hasMatch()) {
        if (m_controlTokenStatus)
            m_controlTokenStatus->setText(QStringLiteral(
                "Run \"Check token permissions\" first (and set Account ID / "
                "Cloudflare zone on the relay tab if they stay unknown): a new "
                "token has to be scoped to one account and one zone."));
        return;
    }
    const QString envPath = forkmesh::control::siteDeployEnvFilePath(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
    if (QMessageBox::question(
            this, QStringLiteral("Generate a Cloudflare API token"),
            QStringLiteral(
                "Create a new Cloudflare API token on account %1, scoped to the "
                "permissions this page lists, and store it as this device's "
                "CLOUDFLARE_API_TOKEN variable%2?\n\nThe token you are using now "
                "stays valid until you delete it in the Cloudflare dashboard.")
                .arg(account,
                     envPath.isEmpty()
                         ? QString()
                         : QStringLiteral(" and in %1").arg(envPath)),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    m_controlTokenSecret = token;
    setCloudflareTokenBusy(true);
    appendCloudflareTokenOutput(
        QStringLiteral("GET /user/tokens/permission_groups\n"));
    // No per_page bound: this endpoint answers with the complete catalog, and a
    // truncated page would look like a missing permission group.
    cloudflareApiCall(
        token, QStringLiteral("/user/tokens/permission_groups"),
        QByteArrayLiteral("GET"), {},
        [this, token, account, zone](const QJsonObject &response,
                                     const QString &error) {
            if (!error.isEmpty()) {
                appendCloudflareTokenOutput(error + QLatin1Char('\n'));
                if (m_controlTokenStatus) {
                    m_controlTokenStatus->setText(QStringLiteral(
                        "Cloudflare would not list its permission groups (%1). "
                        "The current token needs API Tokens: Read and API "
                        "Tokens: Edit to mint a replacement.").arg(error));
                }
                endCloudflareTokenRun();
                return;
            }
            static const QRegularExpression unsafeName(
                QStringLiteral("[^A-Za-z0-9._-]"));
            QString node = machineNodeName();
            node.remove(unsafeName);
            if (node.isEmpty())
                node = QStringLiteral("node");
            const QString name =
                QStringLiteral("ForkMesh %1 %2")
                    .arg(node.left(48),
                         QDateTime::currentDateTimeUtc().toString(
                             QStringLiteral("yyyyMMdd-HHmmss")));
            QString payloadError;
            const QJsonObject payload =
                forkmesh::control::cloudflareTokenCreatePayload(
                    name, account, zone, m_controlTokenUserResource,
                    response.value(QStringLiteral("result")).toArray(),
                    &payloadError);
            if (payload.isEmpty()) {
                appendCloudflareTokenOutput(payloadError + QLatin1Char('\n'));
                if (m_controlTokenStatus)
                    m_controlTokenStatus->setText(payloadError);
                endCloudflareTokenRun();
                return;
            }
            appendCloudflareTokenOutput(
                QStringLiteral("POST /user/tokens (%1)\n").arg(name));
            cloudflareApiCall(
                token, QStringLiteral("/user/tokens"),
                QByteArrayLiteral("POST"), payload,
                [this](const QJsonObject &created, const QString &createError) {
                    if (!createError.isEmpty()) {
                        appendCloudflareTokenOutput(createError +
                                                    QLatin1Char('\n'));
                        if (m_controlTokenStatus) {
                            m_controlTokenStatus->setText(
                                QStringLiteral("Cloudflare refused to create the "
                                               "token: %1")
                                    .arg(createError));
                        }
                        endCloudflareTokenRun();
                        return;
                    }
                    const QString value =
                        forkmesh::control::cloudflareCreatedTokenValue(created);
                    if (value.isEmpty()) {
                        appendCloudflareTokenOutput(QStringLiteral(
                            "Cloudflare created a token but returned no usable "
                            "value; nothing was stored.\n"));
                        if (m_controlTokenStatus) {
                            m_controlTokenStatus->setText(QStringLiteral(
                                "Cloudflare created a token but returned no "
                                "usable value; nothing was stored."));
                        }
                        endCloudflareTokenRun();
                        return;
                    }
                    adoptRotatedCloudflareToken(value);
                });
        });
}

QStringList MainWindow::rememberVultrApiKey(const QString &apiKey,
                                            QString *error)
{
    if (error)
        error->clear();
    const QString key = apiKey.trimmed();
    if (key.isEmpty() || key == m_vultrRememberedKey ||
        key.contains(QLatin1Char('\n')) || key.contains(QLatin1Char('\r'))) {
        return {};
    }

    const QStringList names = forkmesh::control::vultrApiKeyVariableNames();
    const QString canonical = names.constFirst();
    QMap<QString, QString> variables = ActionStore::variables();
    bool changed = false;
    bool haveCanonical = false;
    for (auto it = variables.begin(); it != variables.end(); ++it) {
        const QString name = it.key().trimmed();
        if (!names.contains(name, Qt::CaseInsensitive))
            continue;
        if (name.compare(canonical, Qt::CaseInsensitive) == 0)
            haveCanonical = true;
        if (it.value().trimmed() != key) {
            it.value() = key;
            changed = true;
        }
    }
    if (!haveCanonical) {
        variables.insert(canonical, key);
        changed = true;
    }
    QStringList applied;
    if (changed) {
        ActionStore::setVariables(variables);
        reloadVariablesList();
        applied << QStringLiteral("this device's %1 variable").arg(canonical);
    }

    const QString envPath = forkmesh::control::siteDeployEnvFilePath(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
    QString envError;
    bool retryable = false;
    bool envChanged = false;
    if (envPath.isEmpty()) {
        envError = QStringLiteral(
            "app/.env.production was not found next to this "
            "build's Worker bundle, so no file was rewritten.");
    } else if (writeEnvAssignments(envPath, {{canonical, key}}, &envError,
                                   &envChanged)) {
        if (envChanged)
            applied << envPath;
    } else {
        retryable = true;
    }
    if (error)
        *error = envError;
    if (!retryable)
        m_vultrRememberedKey = key;
    return applied;
}

void MainWindow::adoptRotatedCloudflareToken(const QString &token)
{
    // Redact the new secret from this tab's own output before anything else can
    // print it.
    m_controlTokenSecret = token;
    const QString masked = forkmesh::control::maskedTokenSuffix(token);

    QMap<QString, QString> variables = ActionStore::variables();
    variables.insert(QStringLiteral("CLOUDFLARE_API_TOKEN"), token);
    if (!m_controlTokenAccountId.isEmpty() &&
        forkmesh::control::cloudflareAccountIdFromVariables(variables)
            .isEmpty()) {
        variables.insert(QStringLiteral("CLOUDFLARE_ACCOUNT_ID"),
                         m_controlTokenAccountId);
    }
    ActionStore::setVariables(variables);
    QStringList applied;
    applied << QStringLiteral("this device's CLOUDFLARE_API_TOKEN variable");

    const QString envPath = forkmesh::control::siteDeployEnvFilePath(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
    QString envError;
    if (envPath.isEmpty()) {
        envError = QStringLiteral(
            "app/.env.production was not found next to this "
            "build's Worker bundle, so no file was rewritten.");
    } else {
        QMap<QString, QString> assignments;
        assignments.insert(QStringLiteral("CLOUDFLARE_API_TOKEN"), token);
        if (!m_controlTokenAccountId.isEmpty()) {
            assignments.insert(QStringLiteral("CLOUDFLARE_ACCOUNT_ID"),
                               m_controlTokenAccountId);
        }
        if (writeEnvAssignments(envPath, assignments, &envError))
            applied << envPath;
    }

    if (m_controlTokenEdit)
        m_controlTokenEdit->setText(token);
    if (m_cloudflareTokenEdit)
        m_cloudflareTokenEdit->setText(token);

    appendCloudflareTokenOutput(
        QStringLiteral("Created token %1; stored in %2.\n")
            .arg(masked, applied.join(QStringLiteral(" and "))));
    if (!envError.isEmpty())
        appendCloudflareTokenOutput(envError + QLatin1Char('\n'));
    logSystem(QStringLiteral(
                  "Control node: created a scoped Cloudflare API token (%1) and "
                  "replaced the stored deployment credential.")
                  .arg(masked));
    flashMessage(
        QStringLiteral("New Cloudflare API token %1 stored.").arg(masked),
        false);
    if (m_controlTokenStatus) {
        m_controlTokenStatus->setText(
            QStringLiteral("Created token %1 and replaced %2. %3Re-checking it "
                           "now…")
                .arg(masked, applied.join(QStringLiteral(" and ")),
                     envError.isEmpty() ? QString()
                                        : envError + QLatin1Char(' ')));
    }

    endCloudflareTokenRun();
    QTimer::singleShot(0, this, &MainWindow::testCloudflareApiToken);
}

QStringList MainWindow::rememberCloudflareApiToken(const QString &token,
                                                    QString *error)
{
    if (error)
        error->clear();
    const QString key = token.trimmed();
    if (key.isEmpty() || key.contains(QLatin1Char('\n')) ||
        key.contains(QLatin1Char('\r')))
        return {};

    QMap<QString, QString> variables = ActionStore::variables();
    QStringList applied;
    if (variables.value(QStringLiteral("CLOUDFLARE_API_TOKEN")) != key) {
        variables.insert(QStringLiteral("CLOUDFLARE_API_TOKEN"), key);
        ActionStore::setVariables(variables);
        reloadVariablesList();
        applied << QStringLiteral(
            "this device's CLOUDFLARE_API_TOKEN variable");
    }

    const QString envPath = forkmesh::control::siteDeployEnvFilePath(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
    QString envError;
    bool envChanged = false;
    if (envPath.isEmpty()) {
        envError = QStringLiteral(
            "app/.env.production was not found next to this "
            "build's Worker bundle, so no file was rewritten.");
    } else if (writeEnvAssignments(
                   envPath,
                   {{QStringLiteral("CLOUDFLARE_API_TOKEN"), key}},
                   &envError, &envChanged) &&
               envChanged) {
        applied << envPath;
    }
    if (error)
        *error = envError;
    return applied;
}

void MainWindow::connectToDeployedRelay(const QString &hostname)
{
    const QString host = hostname.trimmed().toLower();
    if (host.isEmpty())
        return;
    int index = -1;
    for (int i = 0; i < m_servers.size(); ++i) {
        if (serverHost(m_servers.at(i).url) == host) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        if (refuseInstanceOfAnotherProject(host))
            return;
        ServerConfig server;
        server.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        server.name = host;
        server.url = canonicalServerUrl(host);
        server.room = kDefaultRoomName;
        m_servers.append(server);
        index = m_servers.size() - 1;
        saveServers();
        fetchFavicon(index);
    }
    appendControlNodeOutput(
        QStringLiteral("Added %1 to this desktop's relay list.\n").arg(host));
    refreshRelayNodes();
    switchToServer(index);
}
