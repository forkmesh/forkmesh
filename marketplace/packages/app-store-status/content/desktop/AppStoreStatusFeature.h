#pragma once

#include "NativePackageExtension.h"

#include "AppStoreStatusModel.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

class QFrame;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace forkmesh::appstorestatus {

class AppStoreStatusFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit AppStoreStatusFeature(const forkmesh::market::NativePackageHost &host,
                                   QWidget *parent = nullptr);
    void refresh() override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;

private:
    void request(const QByteArray &method, const QString &path,
                 forkmesh::market::NativePackageReplyHandler handler);
    void render();
    void addEmptyState(QVBoxLayout *layout);
    void addAttention(QVBoxLayout *layout, const QJsonObject &snapshot);
    void addVersions(QVBoxLayout *layout, const QJsonObject &snapshot);
    void addSubmissions(QVBoxLayout *layout, const QJsonObject &snapshot);
    void addDownloads(QVBoxLayout *layout, const QJsonObject &snapshot);
    void addBuilds(QVBoxLayout *layout, const QJsonObject &snapshot);
    void addGroups(QVBoxLayout *layout, const QJsonObject &snapshot);
    void addFeedback(QVBoxLayout *layout, const QJsonObject &snapshot);
    void addReviews(QVBoxLayout *layout, const QJsonObject &snapshot);
    void addScreenshots(QVBoxLayout *row, const QJsonObject &feedback);
    void loadScreenshot(const QString &id,
                        std::function<void(const QByteArray &, const QString &)> done);
    void openScreenshot(const QList<Screenshot> &shots, int index, const QString &detail);
    void setError(const QString &text);

    forkmesh::market::NativePackageRequest m_request;
    QLabel *m_bundle = nullptr;
    QLabel *m_published = nullptr;
    QLabel *m_status = nullptr;
    QFrame *m_error = nullptr;
    QLabel *m_errorText = nullptr;
    QPushButton *m_refresh = nullptr;
    QScrollArea *m_scroll = nullptr;
    QJsonObject m_payload;
    QHash<QString, QByteArray> m_screenshots;
    QHash<QString, QList<std::function<void(const QByteArray &, const QString &)>>> m_screenshotWaits;
    bool m_loaded = false;
    bool m_loading = false;
};

}
