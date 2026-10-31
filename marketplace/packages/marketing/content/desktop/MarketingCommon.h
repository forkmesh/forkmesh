#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QtCore/qnamespace.h>

class QLayout;
class QLineEdit;
class QTableWidget;
class QTableWidgetItem;

namespace forkmesh::marketing {

constexpr qint64 kDay = 24 * 3600 * 1000LL;
inline const QString kDash = QString::fromUtf8("\xE2\x80\x94");
constexpr int kSortKeyRole = Qt::UserRole + 40;
constexpr int kPinnedRowRole = Qt::UserRole + 41;
using Choices = QList<QPair<QString, QString>>;

QString text(const QJsonObject &object, const char *key);
QString timestamp(qint64 value);
QLineEdit *lineEdit(const QString &value = QString());
QString channelName(const QString &channel);
QString channelName(const QString &channel, const QJsonObject &settings, const QJsonArray &catalog);
QString money(const QJsonValue &cents, const QString &currency);
QString count(const QJsonValue &value);
QString healthLabel(const QString &state);
QString compactCount(qint64 value);
QString choiceLabel(const Choices &choices, const QString &value);
double number(const QJsonObject &object, const char *key);
bool missing(const QJsonObject &object, const char *key);
QString fullCount(double value);
QString shareText(const QJsonValue &value);
QString shortDate(qint64 at);
QString relativeLabel(qint64 at);
QString escaped(const QString &value);
QTableWidgetItem *numberItem(const QString &value);
void clearLayout(QLayout *layout);
void dashboardTable(QTableWidget *table);
void sortableTable(QTableWidget *table);
void fitTable(QTableWidget *table, int maxRows);

} // namespace forkmesh::marketing
