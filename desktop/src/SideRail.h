#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

class QMimeData;

namespace forkmesh::siderail {

inline constexpr char kWorkflowMime[] = "application/x-forkmesh-workflow";
inline constexpr char kWorkflowPinMime[] = "application/x-forkmesh-workflow-pin";
inline constexpr char kItemMime[] = "application/x-forkmesh-side-rail-item";
inline const QString kItemsSetting = QStringLiteral("ui/sideRailItems");
inline const QString kShowSetting = QStringLiteral("ui/showSideRail");
inline const QString kLegacyShowSetting = QStringLiteral("ui/showTeammateRail");
inline const QString kBrowserOpenSetting = QStringLiteral("ui/sideRailBrowserOpen");
inline const QString kBrowserWidthSetting = QStringLiteral("ui/sideRailBrowserWidth");
inline const QString kBrowserUrlSetting = QStringLiteral("ui/sideRailBrowserUrl");
inline const QString kTerminalWidthSetting = QStringLiteral("ui/sideRailTerminalWidth");
inline const QString kWorkflowKind = QStringLiteral("workflow");
inline const QString kPageKind = QStringLiteral("page");
inline const QString kLinkKind = QStringLiteral("link");

inline constexpr int kMaxItems = 40;
inline constexpr int kMaxTitleChars = 80;
inline constexpr int kMaxUrlChars = 2048;
inline constexpr int kDefaultBrowserWidth = 420;
inline constexpr int kDefaultTerminalWidth = 560;
inline constexpr int kMinBrowserWidth = 280;
inline constexpr int kMaxBrowserWidth = 1200;

struct Item {
    QString kind;
    QString repo;
    QString id;
    QString url;
    QString title;

    QString key() const;
    bool valid() const;
    bool operator==(const Item &other) const { return key() == other.key(); }
};

struct WorkflowRef {
    QString repo;
    QString id;
};

Item workflowItem(const QString &repo, const QString &id);
Item pageItem(const QString &railKey, const QString &title = QString());
Item linkItem(const QUrl &url, const QString &title = QString());

QList<Item> parseItems(const QString &json);
QString serializeItems(const QList<Item> &items);

QList<Item> withWorkflows(const QList<Item> &items, const QList<WorkflowRef> &pinnedRight);
QList<Item> insertItem(const QList<Item> &items, const Item &item, int index);
QList<Item> removeItem(const QList<Item> &items, const QString &key);
int indexOfKey(const QList<Item> &items, const QString &key);

bool acceptableLink(const QUrl &url);
int clampBrowserWidth(int width);

QByteArray encodeWorkflow(const QString &repo, const QString &id);
QByteArray encodeWorkflowPin(const QString &repo, const QString &id);
bool decodeWorkflow(const QByteArray &data, QString *repo, QString *id);
QList<Item> itemsFromMime(const QMimeData *mime, const QString &railItemMime);

}
