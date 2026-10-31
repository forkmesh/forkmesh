#pragma once

#include "NativePackageExtension.h"
#include "RepositoryCalendar.h"

#include <QDate>
#include <QHash>
#include <QJsonArray>
#include <QPointer>

class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QTableWidget;
class QTimer;

namespace forkmesh::calendar {

class CalendarMiniMonth;

class CalendarPage : public forkmesh::market::NativePackageWidget
{
public:
    using CalendarReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;

    CalendarPage(const forkmesh::market::NativePackageHost &host, QWidget *parent);

    void refresh() override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query,
                                                               int limit) const override;
    void activateSearchHit(const QString &id) override;

    void openCalendarEventDialog(const QString &eventId = QString(),
                                 const QDate &date = QDate());
    void renderActivity(const QJsonArray &activity, const QDate &anchor,
                        const QString &mode);
    void extendAgenda(int by);
    QTableWidget *monthTable() const { return m_calendarMonthTable; }
    QTableWidget *weekTable() const { return m_calendarWeekTable; }
    QListWidget *agendaList() const { return m_calendarAgendaList; }
    QLabel *status() const { return m_calendarStatus; }

private:
    void buildCalendarSection();
    void requestCalendar(const QByteArray &method, const QString &path,
                         const QJsonObject &body, CalendarReplyHandler handler);
    void refreshCalendar();
    void refreshRepositoryCalendarEvents();
    void scanRepositoryCalendarSlice();
    void publishRepositoryCalendarEvents();
    void rebuildCalendarList();
    void renderCalendar();

    forkmesh::market::NativePackageHost m_host;
    std::function<void()> m_searchIndexChanged;
    QStackedWidget *m_calendarViews = nullptr;
    QTableWidget *m_calendarMonthTable = nullptr;
    QTableWidget *m_calendarWeekTable = nullptr;
    QListWidget *m_calendarDayList = nullptr;
    QListWidget *m_calendarAgendaList = nullptr;
    QListWidget *m_calendarList = nullptr;
    QLineEdit *m_calendarSearch = nullptr;
    QLabel *m_calendarHeading = nullptr;
    QLabel *m_calendarStatus = nullptr;
    QPointer<CalendarMiniMonth> m_calendarMiniMonth;
    QPointer<QLabel> m_calendarTodayLine;
    QJsonArray m_calendarEvents;
    QJsonArray m_repositoryCalendarEvents;
    QJsonArray m_calendars;
    QString m_calendarActor;
    QDate m_calendarAnchor;
    QHash<QString, QJsonArray> m_repositoryCalendarMonths;
    QStringList m_repositoryCalendarMonthOrder;
    QList<forkmesh::ui::RepositoryCalendarScan> m_repositoryCalendarScans;
    QList<forkmesh::ui::RepositoryCalendarScan> m_repositoryCalendarQueue;
    QString m_repositoryCalendarRequest;
    int m_repositoryCalendarCursor = 0;
    bool m_repositoryCalendarScanning = false;
    bool m_repositoryCalendarStale = false;
    QTimer *m_calendarSearchTimer = nullptr;
    int m_calendarAgendaLimit = 0;
    QString m_calendarMode = QStringLiteral("month");
    bool m_calendarLoading = false;
};

}
