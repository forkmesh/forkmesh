#pragma once

#include "ProjectAgentAccess.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;

class AgentAccessEditor : public QWidget
{
public:
    explicit AgentAccessEditor(QWidget *parent = nullptr);

    void setAccess(const forkmesh::projects::AgentAccess &access);
    forkmesh::projects::AgentAccess access() const;
    void setChoicesEnabled(bool enabled);

private:
    void syncControls();
    void addFolder(const QString &path, bool writable);
    void chooseFolder(bool writable);
    void addHost();
    void addDeletePath();

    QRadioButton *m_locked = nullptr;
    QRadioButton *m_open = nullptr;
    QCheckBox *m_sharedTools = nullptr;
    QListWidget *m_folders = nullptr;
    QPushButton *m_addReadOnly = nullptr;
    QPushButton *m_addWritable = nullptr;
    QPushButton *m_removeFolder = nullptr;
    QRadioButton *m_ask = nullptr;
    QRadioButton *m_block = nullptr;
    QRadioButton *m_allow = nullptr;
    QListWidget *m_hosts = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QPushButton *m_addHost = nullptr;
    QPushButton *m_removeHost = nullptr;
    QListWidget *m_destructive = nullptr;
    QListWidget *m_deletePaths = nullptr;
    QLineEdit *m_deletePathEdit = nullptr;
    QLabel *m_error = nullptr;
    bool m_choicesEnabled = true;
};
