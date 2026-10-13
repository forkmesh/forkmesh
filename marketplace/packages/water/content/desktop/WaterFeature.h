#pragma once

#include "NativePackageExtension.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace forkmesh::water {

class WaterChart;

class WaterFeature final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;
    using Request = std::function<void(const QByteArray &, const QString &,
                                       const QJsonObject &, ReplyHandler)>;

    explicit WaterFeature(const forkmesh::market::NativePackageHost &host,
                          QWidget *parent = nullptr);
    void refresh() override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    forkmesh::market::NativePackagePromptTarget hostPromptTarget() const override;
    bool submitHostPrompt(const QString &text, const QStringList &attachments) override;

    static QString amountText(double milliliters, const QString &units);
    static double promptMilliliters(const QString &text, const QString &units);
    static double unitMilliliters(const QString &units);
    static bool exportHasData(const QJsonObject &payload);

private:
    void request(const QByteArray &method, const QString &path,
                 const QJsonObject &body, ReplyHandler handler);
    QString apiPath(const QString &suffix = QString()) const;
    void applyInstallState();
    void installPlugin();
    void load();
    void render();
    void rebuildPresets(const QJsonArray &presets);
    void addDrink(int milliliters, const QString &drink, const QString &source);
    void addCustom();
    void deleteEntry(const QString &id);
    void saveGoal();
    void exportMine();
    void uninstallPlugin();
    bool saveExportBundle(const QJsonObject &payload);
    void setStatus(const QString &text, bool error = false);
    QString units() const;

    Request m_request;
    std::function<void()> m_promptTargetChanged;
    QLabel *m_status = nullptr;
    QWidget *m_installPanel = nullptr;
    QPushButton *m_installButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_uninstallButton = nullptr;
    QWidget *m_content = nullptr;
    QLabel *m_total = nullptr;
    QLabel *m_detail = nullptr;
    QProgressBar *m_goalBar = nullptr;
    QHBoxLayout *m_presetRow = nullptr;
    QList<QPushButton *> m_presetButtons;
    QDoubleSpinBox *m_amount = nullptr;
    QLabel *m_amountUnit = nullptr;
    QComboBox *m_drink = nullptr;
    QTableWidget *m_entries = nullptr;
    QLabel *m_entriesEmpty = nullptr;
    WaterChart *m_chart = nullptr;
    QLabel *m_summary = nullptr;
    QSpinBox *m_goal = nullptr;
    QLabel *m_goalUnit = nullptr;
    QComboBox *m_units = nullptr;
    QJsonObject m_overview;
    bool m_checked = false;
    bool m_installed = false;
    bool m_loading = false;
};

} // namespace forkmesh::water
