#!/usr/bin/env python3
"""Guard the Tasks page's current-user and teammate filters."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/MainWindow.h").read_text(encoding="utf-8")
TASKS = (ROOT / "src/MainWindowTasks.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def main() -> None:
    assert "m_organizationTaskMineFilterButton" in HEADER
    assert "m_organizationTaskAssigneeFilterButton" not in HEADER
    assert "QButtonGroup *m_organizationTaskPeopleGroup" in HEADER
    assert "m_organizationTaskAssigneeFilter" in HEADER

    builder = between(
        TASKS,
        "QWidget *MainWindow::buildOrganizationTasksSection()",
        "void MainWindow::requestOrganizationTasks(",
    )
    assert 'setAccessibleName(\n        QStringLiteral("Mine"))' in builder
    assert 'QStringLiteral("organizationTaskMineFilterAction")' in builder
    assert "m_organizationTaskMineFilterButton->setCheckable(true)" in builder
    assert "&QPushButton::toggled" in builder
    assert 'QStringLiteral("All assignees")' not in builder
    assert 'QStringLiteral("organizationTaskPeople")' in builder
    assert "&QButtonGroup::buttonClicked" in builder
    assert "button->setAccessibleName(label);" in builder
    assert "new VerticalIconButton(QString(), VerticalIconButton::Tab)" in builder

    strip = between(
        TASKS,
        "void MainWindow::rebuildOrganizationTaskPeopleStrip()",
        "QJsonObject MainWindow::organizationTaskForRow(",
    )
    assert 'QStringLiteral("organizationTaskPerson")' in strip
    assert "return left == actor;" in strip
    assert 'QStringLiteral("Everyone")' in strip
    assert 'QStringLiteral("Bot")' in strip
    assert 'QStringLiteral("Unassigned")' in strip
    assert "chatSenderAvatarSource(QString(), name)" in strip

    matcher = between(
        TASKS,
        "bool organizationTaskAssignedTo(",
        "constexpr int kTaskStatusRole",
    )
    assert 'QStringLiteral("assigneeKind")' in matcher
    assert 'QLatin1String("user")' in matcher
    assert 'QStringLiteral("assignee")' in matcher
    assert "Qt::CaseInsensitive" in matcher

    renderer = between(
        TASKS,
        "void MainWindow::renderOrganizationTaskRows(",
        "void MainWindow::openCreatedOrganizationTask(",
    )
    assert "mineOnly" in renderer
    assert "organizationTaskAssignedTo(task, m_organizationTaskActor)" in renderer
    assert 'key != assigneeFilter' in renderer
    assert 'taskText(task, QStringLiteral("assignee"))' in renderer
    assert "QString::number(mineCount)" in renderer
    assert "setTabCount(m_organizationTaskOpenFilterButton, openCount);" in renderer
    assert "updateOrganizationTaskPeopleStrip(openByAssignee);" in renderer

    payload = between(
        TASKS,
        "void MainWindow::applyOrganizationTasks(",
        "bool MainWindow::replaceOrganizationTask(",
    )
    assert "rebuildOrganizationTaskPeopleStrip();" in payload


if __name__ == "__main__":
    main()
