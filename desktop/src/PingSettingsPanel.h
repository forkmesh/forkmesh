#pragma once

// The account-wide half of Settings > Pings (issue #347): quiet hours, the
// on/off and email switch for every ping kind, and the master email switch.
// Below them, the desktop-only kinds a Pings row's gear turned off, so each can be
// turned back on (those stay in this node's QSettings and never sync).
// Each widget writes its QSettings copy (PingSettings.h) and then calls
// `changed`, which MainWindow answers with a signed heartbeat push; refresh()
// repaints from QSettings after the relay's copy is adopted.
//
// No Q_OBJECT: everything is wired with lambdas, so nothing needs moc.

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QTimeEdit;
class QWidget;

class PingSettingsPanel : public QObject
{
public:
    explicit PingSettingsPanel(QObject *parent = nullptr);

    QWidget *quietHoursWidget() const { return m_quietBox; }
    QWidget *kindsWidget() const { return m_kindsBox; }
    QCheckBox *emailMasterCheck() const { return m_emailMaster; }

    // Widgets built elsewhere whose value this panel keeps in step on
    // refresh(): the email pacing combo and a kind's standalone email box.
    void attachDeliveryCombo(QComboBox *combo, const QString &settingKey);
    void attachEmailCheck(const QString &kind, QCheckBox *box);

    void refresh();
    void setAdmin(bool admin);

    // Highlights a kind's row for a moment; the row to scroll to, or null.
    QWidget *revealKind(const QString &kind);

    std::function<void()> changed;
    // Names a desktop-only kind ("agent_done" -> "Agent done"); unset shows
    // the kind itself.
    std::function<QString(const QString &)> desktopKindLabel;

private:
    void buildQuietHours();
    void buildKinds();
    void saveQuietHours();
    void updateEmailEnablement();
    void updateQuietStatus();
    void notifyChanged();
    void addDesktopKindRow(const QString &kind);

    QPointer<QWidget> m_quietBox;
    QPointer<QWidget> m_kindsBox;
    QPointer<QCheckBox> m_emailMaster;
    QPointer<QCheckBox> m_quietEnabled;
    QPointer<QTimeEdit> m_quietStart;
    QPointer<QTimeEdit> m_quietEnd;
    QList<QPointer<QCheckBox>> m_quietDays;
    QPointer<QLabel> m_quietStatus;
    QPointer<QComboBox> m_deliveryCombo;
    QString m_deliverySettingKey;
    QHash<QString, QPointer<QCheckBox>> m_pingChecks;
    QHash<QString, QPointer<QCheckBox>> m_emailChecks;
    QHash<QString, QPointer<QFrame>> m_rows;
    // Rows stay for the session once added, so switching one back on does not
    // pull it out from under the pointer.
    QPointer<QWidget> m_desktopBox;
    QHash<QString, QPointer<QCheckBox>> m_desktopChecks;
    QHash<QString, QPointer<QLabel>> m_desktopNames;
    QList<QPointer<QWidget>> m_adminOnly;
    bool m_admin = false;
    bool m_refreshing = false;
};
