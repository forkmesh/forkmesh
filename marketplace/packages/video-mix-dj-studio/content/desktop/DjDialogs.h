#pragma once

#include <QDialog>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;

namespace forkmesh::djstudio {

class DjModel;

class DjReactionOverlay : public QWidget
{
public:
    DjReactionOverlay(DjModel *model, QWidget *parent);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    DjModel *m_model;
};

class DjPeopleStrip : public QWidget
{
public:
    DjPeopleStrip(DjModel *model, QWidget *parent);

    void refresh();
    std::function<void()> clicked;

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    DjModel *m_model;
};

class DjJamDialog : public QDialog
{
public:
    DjJamDialog(DjModel *model, std::function<QStringList()> directory, QWidget *parent);

    void refresh();

private:
    DjModel *m_model;
    std::function<QStringList()> m_directory;
    QLabel *m_heading = nullptr;
    QLabel *m_problem = nullptr;
    QListWidget *m_jams = nullptr;
    QListWidget *m_people = nullptr;
    QLineEdit *m_invite = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_join = nullptr;
    QPushButton *m_leave = nullptr;
    QPushButton *m_inviteButton = nullptr;
    QPushButton *m_remove = nullptr;
    QWidget *m_outside = nullptr;
    QWidget *m_inside = nullptr;
};

class DjSongDialog : public QDialog
{
public:
    DjSongDialog(DjModel *model, const QString &id, QWidget *parent);

    void refresh();

private:
    DjModel *m_model;
    QString m_id;
    QListWidget *m_tags = nullptr;
    QListWidget *m_playlists = nullptr;
    QLineEdit *m_newTags = nullptr;
    QLabel *m_heading = nullptr;
    bool m_building = false;
};

}
