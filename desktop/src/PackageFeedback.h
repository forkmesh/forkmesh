#pragma once

#include <QByteArray>
#include <QDialog>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <functional>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace forkmesh::market {

inline constexpr int kPackageFeedbackMinMessage = 4;
inline constexpr int kPackageFeedbackMaxMessage = 4000;
inline constexpr int kPackageFeedbackMaxContact = 200;

struct PackageFeedbackReport {
    QString packageId;
    QString packageName;
    QString packageVersion;
    QString message;
    QString contact;
    QString appVersion;
    QString platform;
    QString osVersion;
    QString device;
};

struct PackageFeedbackOutcome {
    bool sent = false;
    QString text;
};

QString desktopFeedbackPlatform();
PackageFeedbackReport packageFeedbackReport(const QString &packageId,
                                            const QString &packageName,
                                            const QString &packageVersion,
                                            const QString &appVersion);
QJsonObject packageFeedbackPayload(const PackageFeedbackReport &report);
PackageFeedbackOutcome packageFeedbackOutcome(int httpStatus,
                                              const QByteArray &body,
                                              const QString &networkError);
QUrl packageRatingUrl(const QString &origin, const QString &marketplaceId,
                      const QString &packageId);
QUrl packageShareUrl(const QString &marketplaceId, const QString &packageId);

using PackageFeedbackReplyHandler =
    std::function<void(int, const QByteArray &, const QString &)>;
using PackageFeedbackSender =
    std::function<void(const QJsonObject &, PackageFeedbackReplyHandler)>;

class PackageFeedbackDialog final : public QDialog
{
public:
    PackageFeedbackDialog(const PackageFeedbackReport &base,
                          PackageFeedbackSender sender,
                          QWidget *parent = nullptr);

    QJsonObject payload() const;
    void send();

private:
    void updateSendEnabled();
    void finish(const PackageFeedbackOutcome &outcome);

    PackageFeedbackReport m_base;
    PackageFeedbackSender m_sender;
    QPlainTextEdit *m_message = nullptr;
    QLineEdit *m_contact = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_send = nullptr;
    QPushButton *m_cancel = nullptr;
    bool m_sending = false;
    bool m_sent = false;
};

} // namespace forkmesh::market
