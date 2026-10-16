#include "AppStoreStatusFeature.h"
#include "AppStoreStatusModel.h"

#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QScreen>
#include <QToolButton>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>

#include <memory>
#include <utility>

namespace forkmesh::appstorestatus {
namespace {

const QString kApiPath = QStringLiteral("/api/app-store-status");

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString().trimmed();
}

QLabel *label(const QString &value, const char *objectName, bool wrap = true)
{
    auto *widget = new QLabel(value);
    widget->setObjectName(QLatin1String(objectName));
    widget->setTextFormat(Qt::PlainText);
    widget->setWordWrap(wrap);
    widget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return widget;
}

QLabel *badge(const QString &value, Tone tone, const QString &tooltip = QString())
{
    auto *widget = new QLabel(value);
    widget->setObjectName(QStringLiteral("badge"));
    widget->setTextFormat(Qt::PlainText);
    widget->setAlignment(Qt::AlignCenter);
    widget->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    widget->setProperty("tone", toneName(tone));
    widget->setProperty("appStoreTone", toneName(tone));
    if (!tooltip.isEmpty())
        widget->setToolTip(tooltip);
    return widget;
}

QLabel *statusBadge(const QJsonObject &status, const QString &prefix = QString())
{
    const QString value = statusLabel(status);
    const QString state = text(status, "state");
    return badge(prefix.isEmpty() ? value : prefix + QStringLiteral(": ") + value, toneOf(status),
                 state.isEmpty() ? QString() : state);
}

QFrame *notice(const QString &tone, const QString &heading, const QStringList &lines)
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("inlineNotice"));
    frame->setProperty("tone", tone);
    frame->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(4);
    if (!heading.isEmpty()) {
        QLabel *title = label(heading, "appStoreNoticeTitle");
        title->setStyleSheet(QStringLiteral("font-weight: 600;"));
        layout->addWidget(title);
    }
    for (const QString &line : lines)
        layout->addWidget(label(line, "appStoreNoticeLine"));
    return frame;
}

class Section
{
public:
    Section(QVBoxLayout *page, const QJsonObject &snapshot, const QString &key)
    {
        m_block = new QWidget;
        m_block->setObjectName(QStringLiteral("appStoreSection"));
        m_block->setProperty("section", key);
        m_layout = new QVBoxLayout(m_block);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(6);
        m_layout->addWidget(label(sectionTitle(key), "groupHeading", false));
        const QString unavailable = unavailableText(snapshot, key);
        if (!unavailable.isEmpty()) {
            QFrame *missing = notice(QStringLiteral("warning"), QString(),
                                     {QStringLiteral("Not available: ") + unavailable});
            missing->setProperty("unavailable", key);
            m_layout->addWidget(missing);
        }
        m_unavailable = !unavailable.isEmpty();
        page->addWidget(m_block);
    }

    QVBoxLayout *card()
    {
        if (!m_card) {
            m_card = new QFrame;
            m_card->setObjectName(QStringLiteral("card"));
            m_card->setAttribute(Qt::WA_StyledBackground, true);
            m_cardLayout = new QVBoxLayout(m_card);
            m_cardLayout->setContentsMargins(14, 2, 14, 2);
            m_cardLayout->setSpacing(0);
            m_layout->addWidget(m_card);
        }
        return m_cardLayout;
    }

    QVBoxLayout *row(const QString &title, const QString &detail, const QList<QWidget *> &badges = {})
    {
        auto *frame = new QFrame;
        frame->setObjectName(QStringLiteral("settingsRow"));
        frame->setProperty("firstRow", m_rows == 0);
        frame->setAttribute(Qt::WA_StyledBackground, true);
        auto *layout = new QVBoxLayout(frame);
        layout->setContentsMargins(0, 9, 0, 9);
        layout->setSpacing(3);
        auto *top = new QHBoxLayout;
        top->setSpacing(6);
        top->addWidget(label(title, "settingsRowTitle"), 1);
        for (QWidget *widget : badges)
            top->addWidget(widget, 0, Qt::AlignTop);
        layout->addLayout(top);
        if (!detail.isEmpty())
            layout->addWidget(label(detail, "settingsRowDescription"));
        card()->addWidget(frame);
        ++m_rows;
        return layout;
    }

    void empty(const QString &message)
    {
        if (m_rows == 0 && !m_unavailable)
            m_layout->addWidget(label(message, "fieldHint"));
    }

private:
    QWidget *m_block = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QFrame *m_card = nullptr;
    QVBoxLayout *m_cardLayout = nullptr;
    int m_rows = 0;
    bool m_unavailable = false;
};

QLabel *figure(qint64 value)
{
    QLabel *number = label(QLocale().toString(value), "appStoreFigure", false);
    number->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));
    return number;
}

}

AppStoreStatusFeature::AppStoreStatusFeature(const forkmesh::market::NativePackageHost &host,
                                             QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request)
{
    setObjectName(QStringLiteral("appStoreStatusSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    heading->setSpacing(10);
    heading->addWidget(label(QStringLiteral("App Store Status"), "pageTitle", false));
    m_bundle = label(QString(), "pageLede", false);
    m_bundle->setAccessibleName(QStringLiteral("Bundle id"));
    heading->addWidget(m_bundle, 0, Qt::AlignBottom);
    heading->addStretch();
    m_published = label(QString(), "pageLede", false);
    m_published->setAccessibleName(QStringLiteral("Published"));
    heading->addWidget(m_published);
    m_refresh = new QPushButton(QStringLiteral("Refresh"));
    m_refresh->setObjectName(QStringLiteral("ghostButton"));
    m_refresh->setToolTip(QStringLiteral("Read the snapshot your desktop last published"));
    heading->addWidget(m_refresh);
    outer->addLayout(heading);

    m_status = label(QStringLiteral("Loading App Store status…"), "statusLine");
    outer->addWidget(m_status);

    m_error = new QFrame;
    m_error->setObjectName(QStringLiteral("inlineNotice"));
    m_error->setProperty("tone", QStringLiteral("danger"));
    m_error->setAttribute(Qt::WA_StyledBackground, true);
    auto *errorLayout = new QHBoxLayout(m_error);
    errorLayout->setContentsMargins(12, 8, 12, 8);
    m_errorText = label(QString(), "appStoreError");
    errorLayout->addWidget(m_errorText, 1);
    m_error->hide();
    outer->addWidget(m_error);

    m_scroll = new QScrollArea;
    m_scroll->setObjectName(QStringLiteral("appStoreStatusScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(m_scroll, 1);

    connect(m_refresh, &QPushButton::clicked, this, [this] { refresh(); });
    render();
}

void AppStoreStatusFeature::request(const QByteArray &method, const QString &path,
                                    forkmesh::market::NativePackageReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("App Store Status network access is unavailable."));
        return;
    }
    const QPointer<AppStoreStatusFeature> guard(this);
    m_request(method, path, {},
              [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload,
                                                    const QString &error) {
        if (guard)
            handler(ok, payload, error);
    });
}

void AppStoreStatusFeature::refresh()
{
    if (m_loading)
        return;
    m_loading = true;
    m_refresh->setEnabled(false);
    m_status->setText(m_loaded ? QStringLiteral("Refreshing…") : QStringLiteral("Loading App Store status…"));
    m_status->show();
    request(QByteArrayLiteral("GET"), kApiPath,
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        m_refresh->setEnabled(true);
        if (!ok) {
            m_status->hide();
            setError(QStringLiteral("App Store status could not load: %1")
                         .arg(error.isEmpty() ? text(payload, "error") : error));
            return;
        }
        setError(QString());
        m_status->hide();
        m_payload = payload;
        m_loaded = true;
        render();
    });
}

bool AppStoreStatusFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    if (m_loaded && snapshotOf(m_payload).isEmpty())
        return false;
    request(QByteArrayLiteral("GET"), kApiPath + QStringLiteral("/export"),
            [done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        QJsonObject result = payload;
        result.insert(QStringLiteral("hasData"), exportHasData(payload));
        done(ok, result, error);
    });
    return true;
}

void AppStoreStatusFeature::setError(const QString &value)
{
    m_errorText->setText(value);
    m_error->setVisible(!value.isEmpty());
}

void AppStoreStatusFeature::render()
{
    const QJsonObject snapshot = snapshotOf(m_payload);
    const QJsonObject app = snapshot.value(QStringLiteral("app")).toObject();
    m_bundle->setText(text(app, "bundleId"));
    m_bundle->setVisible(!m_bundle->text().isEmpty());
    const QJsonValue publishedAt = m_payload.value(QStringLiteral("publishedAt"));
    m_published->setText(snapshot.isEmpty() ? QString()
                                            : publishedText(publishedAt, QDateTime::currentDateTimeUtc()));
    m_published->setToolTip(dateText(publishedAt));
    m_published->setVisible(!m_published->text().isEmpty());

    const int scrolled = m_scroll->verticalScrollBar()->value();
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("appStoreStatusBody"));
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(18);
    if (!m_loaded) {
        layout->addStretch();
    } else if (snapshot.isEmpty()) {
        addEmptyState(layout);
    } else {
        addAttention(layout, snapshot);
        addVersions(layout, snapshot);
        addSubmissions(layout, snapshot);
        addDownloads(layout, snapshot);
        addBuilds(layout, snapshot);
        addGroups(layout, snapshot);
        addFeedback(layout, snapshot);
        addReviews(layout, snapshot);
        layout->addStretch();
    }
    m_scroll->setWidget(body);
    m_scroll->verticalScrollBar()->setValue(scrolled);
}

void AppStoreStatusFeature::addEmptyState(QVBoxLayout *layout)
{
    auto *empty = new QWidget;
    empty->setObjectName(QStringLiteral("emptyState"));
    auto *column = new QVBoxLayout(empty);
    column->setContentsMargins(24, 48, 24, 24);
    column->setSpacing(6);
    QLabel *title = label(QStringLiteral("Nothing published yet"), "emptyStateTitle");
    title->setAlignment(Qt::AlignHCenter);
    column->addWidget(title);
    QLabel *body = label(QStringLiteral(
        "Your desktop publishes what App Store Connect says about your iOS app when the "
        "“iOS App Store status” shortcut runs. Run it from the Shortcuts page or "
        "from your phone, then press Refresh."), "emptyStateBody");
    body->setAlignment(Qt::AlignHCenter);
    column->addWidget(body);
    layout->addWidget(empty);
    layout->addStretch();
}

void AppStoreStatusFeature::addAttention(QVBoxLayout *layout, const QJsonObject &snapshot)
{
    const QStringList lines = attentionLines(snapshot);
    if (lines.isEmpty())
        return;
    QFrame *banner = notice(QStringLiteral("danger"), QStringLiteral("Needs your action"), lines);
    banner->setProperty("appStoreAttention", true);
    layout->addWidget(banner);
}

void AppStoreStatusFeature::addVersions(QVBoxLayout *layout, const QJsonObject &snapshot)
{
    Section section(layout, snapshot, QStringLiteral("versions"));
    for (const QJsonValue &value : snapshot.value(QStringLiteral("versions")).toArray()) {
        const QJsonObject version = value.toObject();
        section.row(versionTitle(text(version, "version"), text(version, "build")),
                    versionDetail(version),
                    {statusBadge(version.value(QStringLiteral("status")).toObject())});
    }
    section.empty(QStringLiteral("No App Store versions yet."));
}

void AppStoreStatusFeature::addSubmissions(QVBoxLayout *layout, const QJsonObject &snapshot)
{
    Section section(layout, snapshot, QStringLiteral("submissions"));
    for (const QJsonValue &value : snapshot.value(QStringLiteral("submissions")).toArray()) {
        const QJsonObject submission = value.toObject();
        QVBoxLayout *row = section.row(submissionTitle(submission), QString(),
                                       {statusBadge(submission.value(QStringLiteral("status")).toObject())});
        for (const QJsonValue &itemValue : submission.value(QStringLiteral("items")).toArray()) {
            const QJsonObject item = itemValue.toObject();
            auto *line = new QHBoxLayout;
            line->setContentsMargins(12, 0, 0, 0);
            line->setSpacing(6);
            const QString name = text(item, "name");
            line->addWidget(label(name.isEmpty() ? QStringLiteral("Item") : name,
                                  "settingsRowDescription", false));
            line->addWidget(statusBadge(item.value(QStringLiteral("status")).toObject()));
            line->addStretch();
            row->addLayout(line);
        }
    }
    section.empty(QStringLiteral("Nothing has been sent to App Review."));
}

void AppStoreStatusFeature::addDownloads(QVBoxLayout *layout, const QJsonObject &snapshot)
{
    Section section(layout, snapshot, QStringLiteral("downloads"));
    const QJsonObject downloads = snapshot.value(QStringLiteral("downloads")).toObject();
    const QList<Figures> rows = downloadFigures(downloads);
    if (rows.isEmpty()) {
        const QString note = text(downloads, "note");
        section.empty(note.isEmpty() ? QStringLiteral("Download figures are not available yet.") : note);
        return;
    }
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 10, 0, 10);
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(6);
    const QStringList headers{QStringLiteral("Downloads"), QStringLiteral("Redownloads"),
                              QStringLiteral("Updates")};
    for (int column = 0; column < headers.size(); ++column)
        grid->addWidget(label(headers.at(column), "settingsRowDescription", false), 0, column + 1);
    for (int index = 0; index < rows.size(); ++index) {
        const Figures &row = rows.at(index);
        grid->addWidget(label(row.label, "settingsRowTitle", false), index + 1, 0);
        grid->addWidget(figure(row.downloads), index + 1, 1);
        grid->addWidget(figure(row.redownloads), index + 1, 2);
        grid->addWidget(figure(row.updates), index + 1, 3);
    }
    grid->setColumnStretch(4, 1);
    section.card()->addLayout(grid);
    const QString note = text(downloads, "note");
    if (!note.isEmpty())
        section.card()->addWidget(label(note, "settingsRowDescription"));
}

void AppStoreStatusFeature::addBuilds(QVBoxLayout *layout, const QJsonObject &snapshot)
{
    Section section(layout, snapshot, QStringLiteral("builds"));
    for (const QJsonValue &value : snapshot.value(QStringLiteral("builds")).toArray()) {
        const QJsonObject build = value.toObject();
        QList<QWidget *> top;
        if (build.value(QStringLiteral("expired")).toBool())
            top.append(badge(QStringLiteral("Expired"), Tone::Neutral));
        QVBoxLayout *row = section.row(versionTitle(text(build, "version"), text(build, "build")),
                                       buildDetail(build), top);
        auto *badges = new QHBoxLayout;
        badges->setSpacing(6);
        const QList<std::pair<const char *, QString>> stages{
            {"processing", QStringLiteral("Processing")},
            {"internal", QStringLiteral("Internal")},
            {"external", QStringLiteral("External")},
            {"betaReview", QStringLiteral("Beta review")},
        };
        for (const auto &stage : stages) {
            const QJsonValue status = build.value(QLatin1String(stage.first));
            if (status.isObject())
                badges->addWidget(statusBadge(status.toObject(), stage.second));
        }
        badges->addStretch();
        row->addLayout(badges);
    }
    section.empty(QStringLiteral("No TestFlight builds uploaded."));
}

void AppStoreStatusFeature::addGroups(QVBoxLayout *layout, const QJsonObject &snapshot)
{
    Section section(layout, snapshot, QStringLiteral("groups"));
    for (const QJsonValue &value : sortedGroups(snapshot.value(QStringLiteral("groups")).toArray())) {
        const QJsonObject group = value.toObject();
        const bool internal = group.value(QStringLiteral("internal")).toBool();
        const QString name = text(group, "name");
        section.row(name.isEmpty() ? QStringLiteral("Unnamed group") : name, groupDetail(group),
                    {badge(internal ? QStringLiteral("Internal") : QStringLiteral("External"),
                           Tone::Neutral)});
    }
    section.empty(QStringLiteral("No TestFlight groups."));
}

void AppStoreStatusFeature::addFeedback(QVBoxLayout *layout, const QJsonObject &snapshot)
{
    Section section(layout, snapshot, QStringLiteral("feedback"));
    for (const QJsonValue &value : snapshot.value(QStringLiteral("feedback")).toArray()) {
        const QJsonObject feedback = value.toObject();
        QVBoxLayout *row = section.row(feedbackTitle(feedback), feedbackDetail(feedback));
        const QString comment = text(feedback, "comment");
        if (!comment.isEmpty())
            row->addWidget(label(comment, "appStoreBody"));
        addScreenshots(row, feedback);
    }
    section.empty(QStringLiteral("No tester feedback."));
}

void AppStoreStatusFeature::addScreenshots(QVBoxLayout *row, const QJsonObject &feedback)
{
    const QList<Screenshot> shots = screenshotsOf(feedback);
    if (shots.isEmpty())
        return;
    const QString detail = feedbackTitle(feedback) + QStringLiteral(" · ") + feedbackDetail(feedback);
    auto *strip = new QHBoxLayout;
    strip->setSpacing(8);
    strip->setContentsMargins(0, 4, 0, 0);
    for (int index = 0; index < shots.size(); ++index) {
        auto *button = new QToolButton;
        button->setObjectName(QStringLiteral("appStoreScreenshot"));
        button->setProperty("screenshotId", shots[index].id);
        button->setFixedSize(72, 108);
        button->setIconSize(QSize(64, 100));
        button->setText(QStringLiteral("…"));
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(QStringLiteral("Open screenshot %1 of %2 full size").arg(index + 1).arg(shots.size()));
        button->setAccessibleName(button->toolTip());
        connect(button, &QToolButton::clicked, this, [this, shots, index, detail] {
            openScreenshot(shots, index, detail);
        });
        const QPointer<QToolButton> guard(button);
        loadScreenshot(shots[index].id, [guard](const QByteArray &bytes, const QString &) {
            if (!guard)
                return;
            const QImage image = QImage::fromData(bytes);
            if (image.isNull()) {
                guard->setText(QStringLiteral("Not uploaded"));
                guard->setToolTip(QStringLiteral("This screenshot has not been uploaded yet. Run the "
                                                 "iOS App Store status shortcut again."));
                return;
            }
            guard->setIcon(QIcon(QPixmap::fromImage(image.scaled(guard->iconSize(), Qt::KeepAspectRatio,
                                                                  Qt::SmoothTransformation))));
            guard->setToolButtonStyle(Qt::ToolButtonIconOnly);
            guard->setProperty("loaded", true);
        });
        strip->addWidget(button);
    }
    strip->addStretch();
    row->addLayout(strip);
}

void AppStoreStatusFeature::loadScreenshot(
    const QString &id, std::function<void(const QByteArray &, const QString &)> done)
{
    if (m_screenshots.contains(id)) {
        done(m_screenshots.value(id), QString());
        return;
    }
    const bool started = m_screenshotWaits.contains(id);
    m_screenshotWaits[id].append(std::move(done));
    if (started)
        return;
    request(QByteArrayLiteral("GET"), screenshotPath(id),
            [this, id](bool ok, const QJsonObject &payload, const QString &error) {
        const QByteArray bytes = ok ? screenshotBytes(payload) : QByteArray();
        if (!bytes.isEmpty())
            m_screenshots.insert(id, bytes);
        const QString problem = !bytes.isEmpty() ? QString()
                                : !ok           ? (error.isEmpty() ? text(payload, "error") : error)
                                                : QStringLiteral("not a picture");
        const auto waits = m_screenshotWaits.take(id);
        for (const auto &wait : waits)
            wait(bytes, problem);
    });
}

void AppStoreStatusFeature::openScreenshot(const QList<Screenshot> &shots, int index, const QString &detail)
{
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("appStoreScreenshotDialog"));
    dialog->setWindowTitle(QStringLiteral("Tester screenshot"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(true);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);
    QLabel *heading = label(QString(), "settingsRowTitle", false);
    layout->addWidget(heading);
    layout->addWidget(label(detail, "settingsRowDescription"));
    auto *picture = new QLabel;
    picture->setObjectName(QStringLiteral("appStoreScreenshotImage"));
    picture->setAlignment(Qt::AlignCenter);
    picture->setWordWrap(true);
    picture->setMinimumSize(240, 240);
    layout->addWidget(picture, 1);
    auto *buttons = new QHBoxLayout;
    auto *previous = new QPushButton(QStringLiteral("Previous"));
    auto *next = new QPushButton(QStringLiteral("Next"));
    auto *save = new QPushButton(QStringLiteral("Save…"));
    auto *close = new QPushButton(QStringLiteral("Close"));
    for (QPushButton *button : {previous, next, save})
        button->setObjectName(QStringLiteral("ghostButton"));
    previous->setVisible(shots.size() > 1);
    next->setVisible(shots.size() > 1);
    buttons->addWidget(previous);
    buttons->addWidget(next);
    buttons->addStretch();
    buttons->addWidget(save);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    const QScreen *display = screen();
    const QRect area = display ? display->availableGeometry() : QRect(0, 0, 1280, 800);
    const QSize bound(qMax(240, int(area.width() * 0.8)), qMax(240, int(area.height() * 0.7)));
    auto current = std::make_shared<int>(qBound(0, index, int(shots.size()) - 1));
    const QPointer<QDialog> guard(dialog);
    auto showCurrent = std::make_shared<std::function<void()>>();
    *showCurrent = [this, guard, shots, current, heading, picture, previous, next, save, bound] {
        if (!guard)
            return;
        const Screenshot shot = shots.value(*current);
        heading->setText(shots.size() > 1
                             ? QStringLiteral("Screenshot %1 of %2").arg(*current + 1).arg(shots.size())
                             : QStringLiteral("Screenshot"));
        previous->setEnabled(*current > 0);
        next->setEnabled(*current < shots.size() - 1);
        save->setEnabled(false);
        picture->setPixmap(QPixmap());
        picture->setText(QStringLiteral("Loading…"));
        loadScreenshot(shot.id, [guard, current, shots, shot, picture, save, bound](const QByteArray &bytes,
                                                                                     const QString &error) {
            if (!guard || shots.value(*current).id != shot.id)
                return;
            const QImage image = QImage::fromData(bytes);
            if (image.isNull()) {
                picture->setText(QStringLiteral("This screenshot could not be loaded%1. Run the iOS App Store "
                                                "status shortcut again to upload it.")
                                     .arg(error.isEmpty() ? QString() : QStringLiteral(": ") + error));
                return;
            }
            const QImage fitted = image.width() > bound.width() || image.height() > bound.height()
                ? image.scaled(bound, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                : image;
            picture->setText(QString());
            picture->setPixmap(QPixmap::fromImage(fitted));
            save->setEnabled(true);
        });
    };
    connect(previous, &QPushButton::clicked, dialog, [current, showCurrent] {
        if (*current > 0) {
            --*current;
            (*showCurrent)();
        }
    });
    connect(next, &QPushButton::clicked, dialog, [current, showCurrent, count = int(shots.size())] {
        if (*current < count - 1) {
            ++*current;
            (*showCurrent)();
        }
    });
    connect(save, &QPushButton::clicked, dialog, [this, dialog, shots, current] {
        const QString id = shots.value(*current).id;
        const QByteArray bytes = m_screenshots.value(id);
        if (bytes.isEmpty())
            return;
        const bool png = bytes.startsWith("\x89PNG");
        const QString path = QFileDialog::getSaveFileName(
            dialog, QStringLiteral("Save screenshot"),
            QStringLiteral("testflight-screenshot-%1.%2")
                .arg(id.left(8), png ? QStringLiteral("png") : QStringLiteral("jpg")));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
            file.write(bytes);
    });
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    (*showCurrent)();
    dialog->show();
}

void AppStoreStatusFeature::addReviews(QVBoxLayout *layout, const QJsonObject &snapshot)
{
    Section section(layout, snapshot, QStringLiteral("reviews"));
    for (const QJsonValue &value : snapshot.value(QStringLiteral("reviews")).toArray()) {
        const QJsonObject review = value.toObject();
        const int rating = review.value(QStringLiteral("rating")).toInt();
        QLabel *rated = badge(stars(rating), rating >= 4 ? Tone::Good : rating >= 3 ? Tone::Pending
                                                                                     : Tone::Action,
                              QStringLiteral("%1 of 5 stars").arg(qBound(0, rating, 5)));
        rated->setAccessibleName(rated->toolTip());
        QVBoxLayout *row = section.row(reviewTitle(review), reviewDetail(review), {rated});
        const QString body = text(review, "body");
        if (!body.isEmpty())
            row->addWidget(label(body, "appStoreBody"));
    }
    section.empty(QStringLiteral("No customer reviews yet."));
}

}

namespace {
const forkmesh::market::NativePackageRegistration appStoreStatusRegistration({
    QStringLiteral("builtin:app-store-status"),
    {QStringLiteral("extension.native")},
    QStringLiteral("App Store"),
    QStringLiteral("device-mobile"),
    QStringLiteral("/api/app-store-status"),
    QStringLiteral("forkmesh-app-store-status-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::appstorestatus::AppStoreStatusFeature(host, parent);
    },
});
}
