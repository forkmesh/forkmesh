#pragma once

#include <QCheckBox>
#include <QFrame>
#include <QLabel>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

class QBoxLayout;
class QButtonGroup;
class QComboBox;
class QHBoxLayout;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class QVariantAnimation;

namespace forkmesh::ui::kit {

enum class Tone { Neutral, Accent, Success, Warning, Danger };

QString toneName(Tone tone);

QPixmap tintedIcon(const QString &octicon, const QColor &color, int px,
                   qreal devicePixelRatio = 1.0);

class Toggle : public QCheckBox
{
public:
    explicit Toggle(QWidget *parent = nullptr);
    explicit Toggle(const QString &text, QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool hitButton(const QPoint &pos) const override;
    void checkStateSet() override;
    void nextCheckState() override;

private:
    void init();
    void animateTo(bool checked);
    QSize trackSize() const;

    QVariantAnimation *m_animation = nullptr;
    qreal m_position = 0.0;
};

class Card : public QFrame
{
public:
    explicit Card(QWidget *parent = nullptr);

    QVBoxLayout *body() const;
    void addRow(QWidget *row);
    int rowCount() const;

private:
    QVBoxLayout *m_body = nullptr;
    int m_rows = 0;
};

class SettingsRow : public QFrame
{
public:
    SettingsRow(const QString &title, const QString &description,
                QWidget *control, QWidget *parent = nullptr);

    QLabel *titleLabel() const;
    QLabel *descriptionLabel() const;
    QWidget *control() const;
    void setFirstRow(bool first);

private:
    QLabel *m_title = nullptr;
    QLabel *m_description = nullptr;
    QWidget *m_control = nullptr;
};

class Badge : public QLabel
{
public:
    explicit Badge(const QString &text, Tone tone = Tone::Neutral,
                   bool outline = false, QWidget *parent = nullptr);

    void setTone(Tone tone, bool outline = false);
};

class KeyHint : public QLabel
{
public:
    explicit KeyHint(const QString &key, QWidget *parent = nullptr);
};

class InlineNotice : public QFrame
{
public:
    explicit InlineNotice(QWidget *parent = nullptr);

    void showMessage(const QString &text, Tone tone = Tone::Neutral);
    void clear();
    QString text() const;
    Tone tone() const;
    QPushButton *addAction(const QString &label,
                           const QString &octicon = QString());
    QPushButton *addCloseButton();
    QList<QPushButton *> actions() const;

private:
    QHBoxLayout *m_layout = nullptr;
    QLabel *m_icon = nullptr;
    QLabel *m_text = nullptr;
    QList<QPushButton *> m_actions;
    Tone m_tone = Tone::Neutral;
};

class EmptyState : public QWidget
{
public:
    EmptyState(const QString &octicon, const QString &title,
               const QString &body, QWidget *parent = nullptr);

    QPushButton *addAction(const QString &label);

private:
    QVBoxLayout *m_layout = nullptr;
};

class SegmentedControl : public QFrame
{
public:
    explicit SegmentedControl(const QStringList &labels,
                              QWidget *parent = nullptr);

    int currentIndex() const;
    void setCurrentIndex(int index);
    void onChanged(std::function<void(int)> callback);
    QList<QPushButton *> buttons() const;

private:
    QButtonGroup *m_group = nullptr;
    std::function<void(int)> m_changed;
};

class Spinner : public QWidget
{
public:
    explicit Spinner(int px = 16, QWidget *parent = nullptr);

    void start();
    void stop();
    bool isSpinning() const;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void timerEvent(QTimerEvent *event) override;

private:
    int m_px = 16;
    int m_angle = 0;
    int m_timer = 0;
};

QLabel *groupHeading(const QString &text, QWidget *parent = nullptr);
QLabel *pageTitle(const QString &text, QWidget *parent = nullptr);
QLabel *pageLede(const QString &text, QWidget *parent = nullptr);
QWidget *labeledField(const QString &label, QWidget *field,
                      const QString &hint = QString(),
                      QWidget *parent = nullptr);

inline constexpr int kCellDisplayChars = 512;
QString cellDisplayText(const QString &text, int maxChars = kCellDisplayChars);

void installStockPolish();
void syncStockWidgets();
void prepareComboPopup(QComboBox *combo);

}
