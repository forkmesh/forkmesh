#pragma once

#include "NativePackageExtension.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QWidget>

#include <functional>

class QComboBox;
class QCompleter;
class QHBoxLayout;
class QDateEdit;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStandardItemModel;
class QTableWidget;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace forkmesh::calories {

class CaloriesRing;
class DayChart;
class WeightChart;

// The desktop face of the Calorie Counter package: the same per-account
// Worker API the dashboard page and the mobile app use, signed with the
// package's own request proof by the generic host. Each account installs it
// for itself, so the one diary follows the user across web, desktop, and
// mobile. Nothing is stored on the node; diaries, goals, reminders, and
// challenges all live behind /api/calories.
class CaloriesFeature final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;
    using Request = std::function<void(const QByteArray &, const QString &,
                                       const QJsonObject &, ReplyHandler)>;

    explicit CaloriesFeature(const forkmesh::market::NativePackageHost &host,
                             QWidget *parent = nullptr);
    void refresh() override;
    // The window's top search box lists the diary entries and the
    // member's challenges; choosing one lands on its row.
    QList<forkmesh::market::NativePackageSearchHit>
    searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    forkmesh::market::NativePackagePromptTarget hostPromptTarget() const override;
    bool submitHostPrompt(const QString &text, const QStringList &attachments) override;

private:
    void request(const QByteArray &method, const QString &path,
                 const QJsonObject &body, ReplyHandler handler);
    QString apiPath(const QString &suffix = QString()) const;
    void applyInstallState();
    void installPlugin();
    void load();
    void applyOverview(const QJsonObject &payload);
    void renderDiary();
    void renderTrends();
    void renderGoals();
    void renderChallenges();
    bool imperial() const;
    bool formImperial() const;
    QString weightText(double kilograms) const;
    double displayWeight(double kilograms) const;
    double kilograms(double displayed) const;
    QString logDay() const;
    QJsonObject entryAt(int row) const;
    void editEntry(const QJsonObject &entry = QJsonObject());
    void deleteEntry(const QString &id);
    void addEntries(const QJsonArray &items, const QString &meal,
                    const QString &source, const QString &day = QString(),
                    const QString &doneMessage = QString());
    QJsonArray frequentFoods() const;
    QJsonArray promptMatches(const QString &text) const;
    QString loggedMessage(const QJsonArray &items, const QString &meal,
                          const QString &day) const;
    void renderQuick();
    QJsonObject quickFood(const QTreeWidgetItem *item) const;
    void quickAdd(QTreeWidgetItem *item);
    void starFood(const QJsonObject &food);
    void removeMyFood(const QJsonObject &food);
    void saveMyFoods(const QJsonArray &foods, const QString &note);
    void pickReceipt();
    void readReceipt(const QString &path);
    void setQuickStatus(const QString &text);
    void photoEstimate();
    void describeMeal();
    void showSuggestions(const QJsonObject &payload);
    void barcodeLookup();
    void searchFoods(bool now = false);
    void showFoods(const QString &query, const QJsonArray &foods);
    void pickFood(int row);
    void addProduct(const QJsonObject &product, const QString &source);
    void weighIn(const QString &day = QString());
    void saveGoals();
    void saveSettings();
    void addReminderRow(const QJsonObject &reminder = QJsonObject());
    QJsonArray remindersPayload() const;
    void exportMine();
    void uninstallPlugin();
    bool saveExportBundle(const QJsonObject &payload);
    void newChallenge();
    void openChallenge(const QString &id);
    void showBoard(const QJsonObject &challenge);
    void inviteFriends(const QString &id);
    void reportInvites(const QJsonObject &payload);
    void setStatus(const QString &text, bool error = false);
    void focusSearchHit();
    void arrangeNextHeader(QHBoxLayout *heading, QLabel *title, QLabel *lede,
                           QPushButton *refreshButton);
    void applyNextStyle();
    void renderNextDiary();
    void syncNextDay();

    Request m_request;
    QLabel *m_status = nullptr;
    QWidget *m_installPanel = nullptr;
    QLabel *m_installNote = nullptr;
    QPushButton *m_installButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_uninstallButton = nullptr;
    QTabWidget *m_tabs = nullptr;
    // Diary
    QDateEdit *m_day = nullptr;
    QLabel *m_caloriesLabel = nullptr;
    QLabel *m_caloriesCaption = nullptr;
    QProgressBar *m_caloriesBar = nullptr;
    QProgressBar *m_proteinBar = nullptr;
    QProgressBar *m_carbsBar = nullptr;
    QProgressBar *m_fatBar = nullptr;
    QLabel *m_proteinLabel = nullptr;
    QLabel *m_carbsLabel = nullptr;
    QLabel *m_fatLabel = nullptr;
    QLabel *m_streakLabel = nullptr;
    QLabel *m_weightLabel = nullptr;
    CaloriesRing *m_ring = nullptr;
    QLabel *m_remainingTitle = nullptr;
    QLabel *m_remainingLabel = nullptr;
    QLabel *m_entriesTodayLabel = nullptr;
    QLabel *m_streakValue = nullptr;
    QLabel *m_streakCaption = nullptr;
    QLabel *m_weightValue = nullptr;
    QLabel *m_weightCaption = nullptr;
    QPushButton *m_todayButton = nullptr;
    QWidget *m_menuHolder = nullptr;
    QLineEdit *m_foodSearch = nullptr;
    QCompleter *m_foodCompleter = nullptr;
    QStandardItemModel *m_foodModel = nullptr;
    QTimer *m_foodTimer = nullptr;
    QString m_foodQuery;
    QJsonArray m_foods;
    QHash<QString, QJsonArray> m_foodCache;
    QTableWidget *m_entriesTable = nullptr;
    QLabel *m_entriesEmpty = nullptr;
    QLabel *m_entriesMore = nullptr;
    // Trends
    DayChart *m_dayChart = nullptr;
    WeightChart *m_weightChart = nullptr;
    QLabel *m_trendsSummary = nullptr;
    // Goals & reminders
    QSpinBox *m_goalCalories = nullptr;
    QSpinBox *m_goalProtein = nullptr;
    QSpinBox *m_goalCarbs = nullptr;
    QSpinBox *m_goalFat = nullptr;
    QComboBox *m_units = nullptr;
    QDoubleSpinBox *m_targetWeight = nullptr;
    QDoubleSpinBox *m_startWeight = nullptr;
    QDoubleSpinBox *m_height = nullptr;
    QLabel *m_bmiLabel = nullptr;
    QTableWidget *m_remindersTable = nullptr;
    QLineEdit *m_usdaKey = nullptr;
    QLabel *m_usdaStatus = nullptr;
    // Challenges
    QTableWidget *m_challengesTable = nullptr;
    QLabel *m_challengesEmpty = nullptr;
    // Quick add
    QComboBox *m_quickMeal = nullptr;
    QLineEdit *m_quickFilter = nullptr;
    QTreeWidget *m_quickTree = nullptr;
    QLabel *m_quickStatus = nullptr;
    QPushButton *m_quickLogButton = nullptr;
    QPushButton *m_quickStarButton = nullptr;
    QPushButton *m_quickRemoveButton = nullptr;

    QJsonObject m_overview;
    QJsonArray m_entries;
    QJsonArray m_challenges;
    std::function<void()> m_searchIndexChanged;
    std::function<void()> m_promptTargetChanged;
    QJsonArray m_frequentFoods;
    // The search hit waiting for its row: opening the page reloads the diary,
    // which rebuilds both tables under the selection.
    QString m_focusHit;
    QString m_afterLoadStatus;
    bool m_checked = false;
    bool m_installed = false;
    bool m_loading = false;
    bool m_rendering = false;
    // Set once the member moves the logging day off today themselves.
    bool m_dayPicked = false;
};

} // namespace forkmesh::calories
