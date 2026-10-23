#include "WorkflowsFeature.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace forkmesh::workflows {
namespace {

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

QLabel *hint(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QString warningsText(const QJsonArray &warnings)
{
    QStringList lines;
    for (const QJsonValue &value : warnings) {
        const QString line = value.isString() ? value.toString()
                                              : value.toObject().value(QStringLiteral("message")).toString();
        if (!line.isEmpty())
            lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace

void WorkflowsFeature::newWorkflowDialog()
{
    if (m_pages->currentIndex() == 1 && !leaveEditor())
        return;
    loadLocal();
    const QJsonArray repositories = m_local.value(QStringLiteral("repositories")).toArray();
    const bool local = localAvailable() && !repositories.isEmpty();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New workflow"));
    dialog.resize(560, 520);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(hint(QStringLiteral("Start blank, from an example, or import an n8n workflow.")));
    auto *list = new QListWidget;
    list->setAccessibleName(QStringLiteral("Starting point"));
    list->setWordWrap(true);
    if (local) {
        auto *heading = new QListWidgetItem(QStringLiteral("On this desktop"));
        heading->setFlags(Qt::NoItemFlags);
        list->addItem(heading);
        const QList<QPair<QString, QString>> starts{
            {QStringLiteral("local:agent"),
             QStringLiteral("Special agent\nA schedule, a special agent step with its role, permissions and skills, "
                            "and its saved report.")},
            {QStringLiteral("local:shortcut"),
             QStringLiteral("Run a shortcut\nOne of the repository's shortcuts, run by hand, on a schedule or from a "
                            "pinned rail button.")},
            {QStringLiteral("local:blank"),
             QStringLiteral("Blank desktop workflow\nA Run by hand trigger; add special agents, shortcuts, desktop "
                            "steps and Save output.")},
            {QStringLiteral("local:cleanup"),
             QStringLiteral("Free disk space\nThe six cleanup steps of tools/free_disk_space.py on this desktop.")}};
        for (const auto &start : starts) {
            auto *row = new QListWidgetItem(start.second);
            row->setData(Qt::UserRole, start.first);
            list->addItem(row);
        }
        auto *accountHeading = new QListWidgetItem(QStringLiteral("On your account (runs on ForkMesh)"));
        accountHeading->setFlags(Qt::NoItemFlags);
        list->addItem(accountHeading);
    }
    auto *blank = new QListWidgetItem(QStringLiteral("Blank workflow\nA Manual trigger and nothing else."));
    blank->setData(Qt::UserRole, QStringLiteral("account:blank"));
    list->addItem(blank);
    auto addTemplates = [list](const QJsonArray &templates) {
        for (const QJsonValue &value : templates) {
            const QJsonObject item = value.toObject();
            auto *row = new QListWidgetItem(QStringLiteral("%1\n%2 · %3 steps")
                                                .arg(text(item, "name"), text(item, "description"))
                                                .arg(item.value(QStringLiteral("steps")).toInt()));
            row->setData(Qt::UserRole, text(item, "key"));
            list->addItem(row);
        }
    };
    addTemplates(m_templates);
    list->setCurrentRow(local ? 1 : 0);
    layout->addWidget(list, 1);
    auto *repoRow = new QFormLayout;
    auto *repoChoice = new QComboBox;
    repoChoice->setAccessibleName(QStringLiteral("Repository"));
    for (const QJsonValue &value : repositories)
        repoChoice->addItem(text(value.toObject(), "title"), text(value.toObject(), "key"));
    const int forkmesh = repoChoice->findData(QStringLiteral("forkmesh/forkmesh"));
    if (forkmesh >= 0)
        repoChoice->setCurrentIndex(forkmesh);
    repoRow->addRow(QStringLiteral("Repository"), repoChoice);
    auto *repoBox = new QWidget;
    repoBox->setLayout(repoRow);
    repoBox->setVisible(local);
    layout->addWidget(repoBox);
    connect(list, &QListWidget::currentItemChanged, &dialog, [repoChoice](QListWidgetItem *item) {
        repoChoice->setEnabled(item && item->data(Qt::UserRole).toString().startsWith(QLatin1String("local:")));
    });
    auto *imports = new QHBoxLayout;
    auto *fileButton = new QPushButton(QStringLiteral("Import file…"));
    fileButton->setObjectName(QStringLiteral("ghostButton"));
    auto *linkButton = new QPushButton(QStringLiteral("Import from link…"));
    linkButton->setObjectName(QStringLiteral("ghostButton"));
    linkButton->setToolTip(QStringLiteral("An n8n template page, a GitHub file or a Gist"));
    imports->addWidget(fileButton);
    imports->addWidget(linkButton);
    imports->addStretch();
    layout->addLayout(imports);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    QString importPath;
    QJsonObject importBody;
    connect(fileButton, &QPushButton::clicked, &dialog, [&] {
        if (m_workflows.size() >= kMaxWorkflows) {
            QMessageBox::warning(&dialog, QStringLiteral("Import workflow"),
                                 errorText(QStringLiteral("too_many_workflows"), QString()));
            return;
        }
        const QString file = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Import workflow"), QString(),
                                                          QStringLiteral("Workflow JSON (*.json);;All files (*)"));
        if (file.isEmpty())
            return;
        QFile input(file);
        if (!input.open(QIODevice::ReadOnly) || input.size() > 2 * 1024 * 1024) {
            QMessageBox::warning(&dialog, QStringLiteral("Import workflow"),
                                 QStringLiteral("That file could not be read or is larger than 2 MB."));
            return;
        }
        const QByteArray bytes = input.readAll();
        const QJsonDocument parsed = QJsonDocument::fromJson(bytes);
        importBody = QJsonObject{{QStringLiteral("document"),
                                  parsed.isObject() ? QJsonValue(parsed.object()) : QJsonValue(QString::fromUtf8(bytes))}};
        importPath = QStringLiteral("/convert");
        dialog.accept();
    });
    connect(linkButton, &QPushButton::clicked, &dialog, [&] {
        bool ok = false;
        const QString url = QInputDialog::getText(&dialog, QStringLiteral("Import from link"),
                                                  QStringLiteral("Link to an n8n template, a GitHub file or a Gist:"),
                                                  QLineEdit::Normal, QString(), &ok)
                                .trimmed();
        if (!ok || url.isEmpty())
            return;
        importBody = QJsonObject{{QStringLiteral("url"), url}};
        importPath = QStringLiteral("/import-url");
        dialog.accept();
    });
    if (m_templates.isEmpty()) {
        const QPointer<QListWidget> guard(list);
        request(QByteArrayLiteral("GET"), QStringLiteral("/api/workflows/templates"), {},
                [this, guard, addTemplates](bool ok, const QJsonObject &payload, const QString &) {
            if (!ok)
                return;
            m_templates = payload.value(QStringLiteral("templates")).toArray();
            if (guard)
                addTemplates(m_templates);
        });
    }
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (!importPath.isEmpty()) {
        importDocument(importBody, importPath);
        return;
    }
    QListWidgetItem *chosen = list->currentItem();
    QString key = chosen ? chosen->data(Qt::UserRole).toString() : QString();
    if (key.startsWith(QLatin1String("local:"))) {
        startLocalDraft(key.mid(6), repoChoice->currentData().toString());
        return;
    }
    if (m_workflows.size() >= kMaxWorkflows)
        return setStatus(errorText(QStringLiteral("too_many_workflows"), QString()), true);
    if (key == QLatin1String("account:blank"))
        key.clear();
    if (key.isEmpty()) {
        loadCatalog([this] {
            openDraft(Draft::blank(), true);
            setStatus(QStringLiteral("New workflow. Add steps, then Save."));
        });
        return;
    }
    setStatus(QStringLiteral("Loading the example…"));
    loadCatalog([this, key] {
        request(QByteArrayLiteral("GET"), QStringLiteral("/api/workflows/templates/") + key, {},
                [this](bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok)
                return setStatus(QStringLiteral("The example did not load: %1").arg(failureText(payload, error)), true);
            const QJsonObject item = payload.value(QStringLiteral("template")).toObject();
            QJsonObject workflow = item.value(QStringLiteral("workflow")).toObject();
            workflow.remove(QStringLiteral("id"));
            if (text(workflow, "name").isEmpty())
                workflow.insert(QStringLiteral("name"), text(item, "name"));
            Draft draft = Draft::fromJson(workflow);
            draft.id.clear();
            draft.active = false;
            draft.hookPath.clear();
            openDraft(draft, true);
            setStatus(QStringLiteral("Example %1 opened. Fill in its credentials, then Save.").arg(text(item, "name")));
        });
    });
}

void WorkflowsFeature::importDocument(const QJsonObject &body, const QString &suffix)
{
    setStatus(QStringLiteral("Importing…"));
    loadCatalog([this, body, suffix] {
        request(QByteArrayLiteral("POST"), QStringLiteral("/api/workflows") + suffix, body,
                [this](bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok)
                return setStatus(QStringLiteral("Not imported: %1").arg(failureText(payload, error)), true);
            Draft draft = Draft::fromJson(payload.value(QStringLiteral("workflow")).toObject());
            draft.id.clear();
            draft.active = false;
            draft.hookPath.clear();
            openDraft(draft, true);
            const QString warnings = warningsText(payload.value(QStringLiteral("warnings")).toArray());
            setStatus(QStringLiteral("Imported %1; check it, then Save.").arg(draft.name));
            if (!warnings.isEmpty())
                QMessageBox::information(this, QStringLiteral("Imported with changes"), warnings);
        });
    });
}

void WorkflowsFeature::addStepDialog()
{
    if (m_draft.nodes.size() >= kMaxNodes)
        return setStatus(errorText(QStringLiteral("too_many_nodes"), QString()), true);
    const bool hasTrigger = !m_draft.trigger(catalog()).isEmpty();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add step"));
    dialog.resize(640, 600);
    auto *layout = new QVBoxLayout(&dialog);
    auto *search = new QLineEdit;
    search->setPlaceholderText(QStringLiteral("Search steps and integrations"));
    search->setAccessibleName(QStringLiteral("Search steps"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto *tree = new QTreeWidget;
    tree->setHeaderHidden(true);
    tree->setAccessibleName(QStringLiteral("Step types"));
    layout->addWidget(tree, 1);
    auto *about = hint(QString());
    about->setMinimumHeight(36);
    layout->addWidget(about);

    QStringList types = catalog().stepTypes();
    if (!hasTrigger || m_draft.isLocal())
        types = catalog().triggerTypes() + types;
    QHash<QString, QTreeWidgetItem *> groups;
    for (const QString &type : std::as_const(types)) {
        QString group = catalog().group(type);
        if (catalog().isApp(type))
            group = QStringLiteral("Integrations: ") + group;
        QTreeWidgetItem *parent = groups.value(group);
        if (!parent) {
            parent = new QTreeWidgetItem(tree, {group});
            parent->setFlags(Qt::ItemIsEnabled);
            groups.insert(group, parent);
        }
        auto *item = new QTreeWidgetItem(parent, {catalog().name(type)});
        item->setData(0, Qt::UserRole, type);
        item->setToolTip(0, catalog().description(type));
    }
    tree->expandToDepth(0);
    for (auto it = groups.begin(); it != groups.end(); ++it)
        it.value()->setExpanded(!it.key().startsWith(QLatin1String("Integrations: ")));

    auto *form = new QFormLayout;
    auto *after = new QComboBox;
    after->setAccessibleName(QStringLiteral("Add after"));
    after->addItem(QStringLiteral("Not connected"), QStringList());
    for (const QString &id : m_draft.ordered()) {
        const QJsonObject node = m_draft.node(id);
        for (const QString &output : catalog().outputs(text(node, "type"))) {
            const QString label = output == QLatin1String("main")
                                      ? text(node, "name")
                                      : QStringLiteral("%1: %2").arg(text(node, "name"), output);
            after->addItem(label, QStringList{id, output});
        }
    }
    const QStringList order = m_draft.ordered();
    const QString anchor = m_selectedStep.isEmpty() ? (order.isEmpty() ? QString() : order.last()) : m_selectedStep;
    for (int index = 0; index < after->count(); ++index) {
        if (after->itemData(index).toStringList().value(0) == anchor) {
            after->setCurrentIndex(index);
            break;
        }
    }
    form->addRow(QStringLiteral("Runs after"), after);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Add step"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);

    auto chosenType = [tree] {
        QTreeWidgetItem *item = tree->currentItem();
        return item ? item->data(0, Qt::UserRole).toString() : QString();
    };
    connect(tree, &QTreeWidget::currentItemChanged, &dialog, [this, buttons, about, after, chosenType] {
        const QString type = chosenType();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!type.isEmpty());
        about->setText(type.isEmpty() ? QString() : catalog().description(type));
        after->setEnabled(!type.isEmpty() && !catalog().isTrigger(type));
    });
    connect(tree, &QTreeWidget::itemDoubleClicked, &dialog, [&dialog, chosenType] {
        if (!chosenType().isEmpty())
            dialog.accept();
    });
    connect(search, &QLineEdit::textChanged, &dialog, [tree](const QString &query) {
        const QString needle = query.trimmed();
        for (int top = 0; top < tree->topLevelItemCount(); ++top) {
            QTreeWidgetItem *group = tree->topLevelItem(top);
            int shown = 0;
            for (int index = 0; index < group->childCount(); ++index) {
                QTreeWidgetItem *item = group->child(index);
                const bool match = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive) ||
                                   item->toolTip(0).contains(needle, Qt::CaseInsensitive) ||
                                   group->text(0).contains(needle, Qt::CaseInsensitive);
                item->setHidden(!match);
                shown += match ? 1 : 0;
            }
            group->setHidden(shown == 0);
            group->setExpanded(!needle.isEmpty() ? shown > 0
                                                 : !group->text(0).startsWith(QLatin1String("Integrations: ")));
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    search->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString type = chosenType();
    if (type.isEmpty())
        return;
    const QStringList parent = after->currentData().toStringList();
    QString added;
    if (catalog().isTrigger(type) && m_draft.isLocal()) {
        added = m_draft.addTrigger(catalog(), type);
    } else if (catalog().isTrigger(type)) {
        m_draft.setTrigger(catalog(), type);
        added = text(m_draft.trigger(catalog()), "id");
    } else if (parent.size() == 2) {
        added = m_draft.addStep(catalog(), type, parent.at(0), parent.at(1));
    } else {
        const int before = int(m_draft.connections.size());
        added = m_draft.addStep(catalog(), type, QString());
        while (m_draft.connections.size() > before)
            m_draft.connections.removeLast();
    }
    setDirty(true);
    renderEditor();
    selectStep(added);
    if (catalog().isApp(type) && !catalog().credentialFields(type.mid(4)).isEmpty()) {
        bool owned = false;
        for (const QJsonValue &value : std::as_const(m_credentials))
            owned = owned || text(value.toObject(), "integration") == type.mid(4);
        if (!owned)
            setStatus(QStringLiteral("%1 needs a credential: press Manage… beside Credential to add one.")
                          .arg(catalog().name(type)));
    }
}

void WorkflowsFeature::loadCredentials(std::function<void()> done)
{
    request(QByteArrayLiteral("GET"), QStringLiteral("/api/workflows/credentials"), {},
            [this, done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("Credentials did not load: %1").arg(failureText(payload, error)), true);
            return;
        }
        m_credentials = payload.value(QStringLiteral("credentials")).toArray();
        m_credentialsLoaded = true;
        if (done)
            done();
    });
}

void WorkflowsFeature::credentialsDialog(const QString &integration)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Credentials"));
    dialog.resize(720, 620);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(hint(QStringLiteral(
        "Credentials are kept encrypted on your account; their values are never shown again after saving.")));
    auto *table = new QTableWidget(0, 4);
    table->setAccessibleName(QStringLiteral("Saved credentials"));
    table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Integration"),
                                      QStringLiteral("Details"), QStringLiteral("Added")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(table, 1);
    auto *removeRow = new QHBoxLayout;
    removeRow->addStretch();
    auto *remove = new QPushButton(QStringLiteral("Delete credential"));
    remove->setObjectName(QStringLiteral("dangerButton"));
    remove->setEnabled(false);
    removeRow->addWidget(remove);
    layout->addLayout(removeRow);

    auto *addTitle = new QLabel(QStringLiteral("Add a credential"));
    addTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
    layout->addWidget(addTitle);
    auto *form = new QFormLayout;
    auto *service = new QComboBox;
    service->setAccessibleName(QStringLiteral("Integration"));
    QStringList keys = catalog().integrationKeys();
    std::sort(keys.begin(), keys.end(), [this](const QString &a, const QString &b) {
        return catalog().name(QStringLiteral("app.") + a).compare(catalog().name(QStringLiteral("app.") + b),
                                                                    Qt::CaseInsensitive) < 0;
    });
    for (const QString &key : std::as_const(keys))
        service->addItem(catalog().name(QStringLiteral("app.") + key), key);
    service->setCurrentIndex(std::max(0, service->findData(integration)));
    form->addRow(QStringLiteral("Integration"), service);
    auto *name = new QLineEdit;
    name->setAccessibleName(QStringLiteral("Credential name"));
    name->setMaxLength(60);
    form->addRow(QStringLiteral("Name"), name);
    layout->addLayout(form);
    auto *fieldsHost = new QWidget;
    auto *fields = new QFormLayout(fieldsHost);
    fields->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(fieldsHost);
    auto *saveRow = new QHBoxLayout;
    auto *problem = hint(QString());
    saveRow->addWidget(problem, 1);
    auto *save = new QPushButton(QStringLiteral("Save credential"));
    save->setObjectName(QStringLiteral("primaryButton"));
    saveRow->addWidget(save);
    layout->addLayout(saveRow);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QList<QPair<QString, QLineEdit *>> inputs;
    auto rebuildFields = [&, this] {
        while (fields->rowCount() > 0)
            fields->removeRow(0);
        inputs.clear();
        const QString key = service->currentData().toString();
        name->setPlaceholderText(catalog().name(QStringLiteral("app.") + key) + QStringLiteral(" account"));
        for (const QJsonValue &value : catalog().credentialFields(key)) {
            const QJsonObject field = value.toObject();
            auto *edit = new QLineEdit;
            edit->setPlaceholderText(text(field, "placeholder"));
            edit->setAccessibleName(text(field, "label"));
            if (field.value(QStringLiteral("secret")).toBool())
                edit->setEchoMode(QLineEdit::Password);
            fields->addRow(text(field, "label"), edit);
            inputs.append({text(field, "key"), edit});
        }
    };
    auto renderTable = [&, this] {
        table->setRowCount(0);
        for (const QJsonValue &value : std::as_const(m_credentials)) {
            const QJsonObject item = value.toObject();
            const int row = table->rowCount();
            table->insertRow(row);
            auto *first = new QTableWidgetItem(text(item, "name"));
            first->setData(Qt::UserRole, text(item, "id"));
            table->setItem(row, 0, first);
            table->setItem(row, 1, new QTableWidgetItem(catalog().name(QStringLiteral("app.") + text(item, "integration"))));
            QStringList hints;
            const QJsonObject masked = item.value(QStringLiteral("hint")).toObject();
            for (auto it = masked.begin(); it != masked.end(); ++it)
                hints << it.key() + QLatin1Char(' ') + it.value().toVariant().toString();
            table->setItem(row, 2, new QTableWidgetItem(hints.join(QStringLiteral(" · "))));
            const qint64 created = qint64(item.value(QStringLiteral("createdAt")).toDouble());
            table->setItem(row, 3, new QTableWidgetItem(
                created > 0 ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(created).date(), QLocale::ShortFormat)
                            : QString()));
        }
        remove->setEnabled(false);
    };
    rebuildFields();
    renderTable();
    const QPointer<QDialog> guard(&dialog);
    loadCredentials([guard, renderTable] {
        if (guard)
            renderTable();
    });
    connect(service, &QComboBox::currentIndexChanged, &dialog, [&] { rebuildFields(); });
    connect(table, &QTableWidget::itemSelectionChanged, &dialog, [&] {
        remove->setEnabled(!table->selectedItems().isEmpty());
    });
    connect(remove, &QPushButton::clicked, &dialog, [&, this] {
        const int row = table->currentRow();
        if (row < 0 || !table->item(row, 0))
            return;
        const QString id = table->item(row, 0)->data(Qt::UserRole).toString();
        const QString label = table->item(row, 0)->text();
        if (QMessageBox::warning(&dialog, QStringLiteral("Delete credential"),
                                 QStringLiteral("Delete \"%1\"? Steps that use it stop working until you choose "
                                                "another.").arg(label),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        request(QByteArrayLiteral("POST"), QStringLiteral("/api/workflows/credentials/%1/delete").arg(id), {},
                [this, guard, renderTable, problem](bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok) {
                if (guard)
                    problem->setText(failureText(payload, error));
                return;
            }
            loadCredentials([guard, renderTable] {
                if (guard)
                    renderTable();
            });
        });
    });
    connect(save, &QPushButton::clicked, &dialog, [&, this] {
        QJsonObject values;
        for (const auto &[key, edit] : std::as_const(inputs))
            values.insert(key, edit->text());
        const QJsonObject body{{QStringLiteral("integration"), service->currentData().toString()},
                               {QStringLiteral("name"), name->text().trimmed()},
                               {QStringLiteral("values"), values}};
        save->setEnabled(false);
        problem->setText(QStringLiteral("Saving…"));
        request(QByteArrayLiteral("POST"), QStringLiteral("/api/workflows/credentials"), body,
                [this, guard, renderTable, problem, save, name, &inputs](bool ok, const QJsonObject &payload,
                                                                          const QString &error) {
            if (!guard)
                return;
            save->setEnabled(true);
            if (!ok) {
                problem->setText(QStringLiteral("Not saved: %1").arg(failureText(payload, error)));
                return;
            }
            problem->setText(QStringLiteral("Saved %1.")
                                 .arg(text(payload.value(QStringLiteral("credential")).toObject(), "name")));
            name->clear();
            for (const auto &entry : std::as_const(inputs))
                entry.second->clear();
            loadCredentials([guard, renderTable] {
                if (guard)
                    renderTable();
            });
        });
    });
    dialog.exec();
    if (m_pages->currentIndex() == 1)
        renderStepForm();
}

} // namespace forkmesh::workflows
