#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AgentAccessEditor.h"
#include "AgentToolCatalog.h"
#include "ClaudeTranscriptView.h"
#include "ProjectAgentAccess.h"

#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QScrollArea>

void MainWindow::showGlobalAgentRules()
{
    using Rules = forkmesh::projects::GlobalAgentRules;
    const QString path = forkmesh::projects::globalRulesPath(forkmesh::paths::homeDir());
    const Rules rules = forkmesh::projects::loadGlobalRules(path);

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("globalAgentRulesDialog"));
    dialog.setWindowTitle(QStringLiteral("Global agent rules"));
    dialog.setMinimumWidth(600);
    dialog.resize(640, 760);
    auto *outer = new QVBoxLayout(&dialog);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget;
    auto *column = new QVBoxLayout(body);
    column->setSpacing(8);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    auto sectionLabel = [](const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("sectionLabel");
        return label;
    };
    auto hint = [](const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("modeHint");
        label->setWordWrap(true);
        return label;
    };
    auto ghostButton = [](const QString &text) {
        auto *button = new QPushButton(text);
        button->setObjectName("ghostButton");
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };

    column->addWidget(hint(QStringLiteral(
        "Every agent this desktop starts, in every project and with every provider, follows "
        "these rules. A project that follows the global rules takes its folder boundary and "
        "external-call setting from here; the extra folders and approved destinations below "
        "apply to every project.")));
    auto *accessEditor = new AgentAccessEditor;
    accessEditor->setAccess(rules.defaults);
    column->addWidget(accessEditor);

    column->addWidget(sectionLabel(QStringLiteral("BLOCKED DESTINATIONS")));
    column->addWidget(hint(QStringLiteral(
        "Never reachable by any agent, whatever a project allows (“*.example.com” covers its "
        "subdomains; “web-search” is the web search tool).")));
    auto *blockedList = new QListWidget;
    blockedList->setObjectName(QStringLiteral("globalAgentRulesBlockedHosts"));
    blockedList->setMaximumHeight(90);
    blockedList->addItems(rules.blockedHosts);
    column->addWidget(blockedList);
    auto *blockedRow = new QHBoxLayout;
    auto *blockedEdit = new QLineEdit;
    blockedEdit->setPlaceholderText(QStringLiteral("pastebin.com"));
    auto *addBlocked = ghostButton(QStringLiteral("Add"));
    auto *removeBlocked = ghostButton(QStringLiteral("Remove"));
    blockedRow->addWidget(blockedEdit, 1);
    blockedRow->addWidget(addBlocked);
    blockedRow->addWidget(removeBlocked);
    column->addLayout(blockedRow);
    auto addBlockedHost = [blockedEdit, blockedList] {
        const QString host = forkmesh::projects::normalizeHost(blockedEdit->text());
        if (host.isEmpty())
            return;
        if (blockedList->findItems(host, Qt::MatchExactly).isEmpty())
            blockedList->addItem(host);
        blockedEdit->clear();
    };
    connect(addBlocked, &QPushButton::clicked, &dialog, addBlockedHost);
    connect(blockedEdit, &QLineEdit::returnPressed, &dialog, addBlockedHost);
    connect(removeBlocked, &QPushButton::clicked, &dialog,
            [blockedList] { delete blockedList->takeItem(blockedList->currentRow()); });

    column->addWidget(sectionLabel(QStringLiteral("DESTRUCTIVE COMMANDS")));
    column->addWidget(hint(QStringLiteral(
        "Recursive deletes (rm -r, find -delete), force pushes and remote branch deletes, "
        "git reset --hard, git clean -f, git branch -D, dropped stashes, history rewrites, "
        "disk wipes and gh repo delete. Deletes inside /tmp do not count.")));
    auto *askDestructive =
        new QRadioButton(QStringLiteral("Ask me before each one (recommended)"));
    auto *blockDestructive = new QRadioButton(QStringLiteral("Block them"));
    auto *allowDestructive = new QRadioButton(QStringLiteral("Let them run"));
    auto *destructiveGroup = new QButtonGroup(&dialog);
    for (QRadioButton *radio : {askDestructive, blockDestructive, allowDestructive}) {
        destructiveGroup->addButton(radio);
        column->addWidget(radio);
    }
    (rules.destructive == Rules::Destructive::Block   ? blockDestructive
     : rules.destructive == Rules::Destructive::Allow ? allowDestructive
                                                      : askDestructive)
        ->setChecked(true);

    column->addWidget(sectionLabel(QStringLiteral("TOOLS")));
    auto *allowlist =
        new QCheckBox(QStringLiteral("Only let agents use the tool families checked below"));
    allowlist->setObjectName(QStringLiteral("globalAgentRulesToolAllowlist"));
    allowlist->setChecked(rules.toolAllowlist);
    allowlist->setToolTip(QStringLiteral(
        "Claude Code launches with --tools naming only these families' built-in tools, so a "
        "tool added by a newer Claude Code stays off until you allow it. Codex and the script "
        "providers switch off what they can of the rest."));
    column->addWidget(allowlist);
    auto *families = new QListWidget;
    families->setObjectName(QStringLiteral("globalAgentRulesToolFamilies"));
    families->setMinimumHeight(180);
    for (const AgentToolFamily &family : agentToolFamilies()) {
        if (family.hidden)
            continue;
        auto *item = new QListWidgetItem(
            QStringLiteral("%1 — %2").arg(family.displayName, family.summary), families);
        item->setData(Qt::UserRole, family.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(!rules.toolAllowlist ||
                                    rules.allowedToolFamilies.contains(family.id)
                                ? Qt::Checked
                                : Qt::Unchecked);
    }
    families->setEnabled(rules.toolAllowlist);
    connect(allowlist, &QCheckBox::toggled, families, &QWidget::setEnabled);
    column->addWidget(families);
    column->addWidget(hint(QStringLiteral(
        "Tool efficiency can still switch families off on top of this list. Applies to agents "
        "started after you save.")));
    column->addStretch(1);

    auto *errorLabel = new QLabel;
    errorLabel->setStyleSheet("color:#ff6b6b; background:transparent;");
    errorLabel->setWordWrap(true);
    errorLabel->hide();
    outer->addWidget(errorLabel);
    auto *buttons = new QDialogButtonBox;
    QPushButton *saveButton =
        buttons->addButton(QStringLiteral("Save"), QDialogButtonBox::AcceptRole);
    saveButton->setObjectName("primaryButton");
    buttons->addButton(QDialogButtonBox::Cancel);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        Rules saved;
        saved.defaults = accessEditor->access();
        for (int i = 0; i < blockedList->count(); ++i)
            saved.blockedHosts.append(blockedList->item(i)->text());
        saved.destructive = blockDestructive->isChecked()   ? Rules::Destructive::Block
                            : allowDestructive->isChecked() ? Rules::Destructive::Allow
                                                            : Rules::Destructive::Ask;
        saved.toolAllowlist = allowlist->isChecked();
        for (int i = 0; i < families->count(); ++i)
            if (families->item(i)->checkState() == Qt::Checked)
                saved.allowedToolFamilies.append(families->item(i)->data(Qt::UserRole).toString());
        QString error;
        if (!forkmesh::projects::saveGlobalRules(path, saved, &error)) {
            errorLabel->setText(error);
            errorLabel->show();
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (m_agentTranscript)
        m_agentTranscript->refreshCommandApprovals();
    flashMessage(QStringLiteral("Saved the global agent rules."));
}
