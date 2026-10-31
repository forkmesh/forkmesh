from pathlib import Path


CRM_PACKAGE = Path(__file__).resolve().parents[1]
FEATURE = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.cpp").read_text(encoding="utf-8")
FEATURE_HEADER = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.h").read_text(encoding="utf-8")
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")


def _body(source, start):
    return source.split(start, 1)[1].split("\n}\n", 1)[0]


def _class(source, name):
    return source.split(f"class {name} final", 1)[1].split("\n};\n", 1)[0]


def test_desktop_contacts_table_drags_down_one_column_into_a_cell_pick():
    assert "m_contactsTable = new ContactsTable(0, 0);" in FEATURE
    table = _class(FEATURE, "ContactsTable")
    move = table.split("void mouseMoveEvent(", 1)[1].split("void mouseReleaseEvent(", 1)[0]
    assert "columnAt(position.x()) != m_anchor.column()" in move
    assert "QItemSelectionModel::ClearAndSelect" in move
    assert "QItemSelection(model()->index(std::min(row, m_anchor.row()), column)" in move
    press = table.split("void mousePressEvent(", 1)[1].split("void mouseMoveEvent(", 1)[0]
    assert "Qt::ControlModifier | Qt::ShiftModifier | Qt::MetaModifier" in press
    release = table.split("void mouseReleaseEvent(", 1)[1].split("void keyPressEvent(", 1)[0]
    assert "setState(NoState);" in release
    assert release.index("if (!m_columnDrag)") < release.index("QTableWidget::mouseReleaseEvent")
    keys = table.split("void keyPressEvent(", 1)[1]
    assert "QKeySequence::Copy" in keys and "pickedColumnCells(this)" in keys


def test_desktop_right_click_copies_a_pick_or_a_whole_column():
    picked = _body(FEATURE, "QModelIndexList pickedColumnCells(")
    assert "cell.column() != column" in picked
    values = _body(FEATURE, "QStringList columnValues(")
    assert "isRowHidden(cell.row())" in values
    menu = _body(FEATURE, "void CrmFeature::showContactMenu(")
    assert menu.index("pickedColumnCells(m_contactsTable)") < menu.index("selectedContacts()")
    assert "copyContactValues(values, label)" in menu
    header = _body(FEATURE, "void CrmFeature::showColumnMenu(")
    assert header.index("copyContactColumn(id)") < header.index("setColumnsHidden({id}, true)")
    copy = _body(FEATURE, "void CrmFeature::copyContactValues(")
    assert "clipboard->setText(values.join(QLatin1Char('\\n')))" in copy
    assert "void copyContactColumn(const QString &columnId);" in FEATURE_HEADER


def test_web_contacts_table_picks_and_copies_column_cells():
    assert '<td data-crm-cell="${escapeHtml(column.field)}"' in WEB_SCRIPT
    assert ".crm-cell-picked {" in WEB_VIEW and "--dashboard-ring-rgb" in WEB_VIEW.split(".crm-cell-picked {", 1)[1].split("}", 1)[0]
    move = _body(WEB_SCRIPT, "  function handleCrmCellPickMove(")
    assert "cell?.dataset.crmCell !== crmCellPick.pressField" in move
    assert "document.elementFromPoint(event.clientX, event.clientY)" in move
    assert "event.preventDefault();" in move
    column_menu = _body(WEB_SCRIPT, "  function openCrmColumnMenu(")
    assert 'label: "Copy column"' in column_menu
    assert column_menu.index("crmActiveShared()") > column_menu.index("const copy =")
    copy = _body(WEB_SCRIPT, "  async function copyCrmCells(")
    assert 'navigator.clipboard.writeText(values.join("\\n"))' in copy
    assert 'list?.addEventListener("mousedown", handleCrmCellPickPress, true);' in WEB_SCRIPT
    assert "if (!event.target.closest(\".crm-cell-picked\")) return;" in WEB_SCRIPT
    assert "crmPaintSelection();\n    crmPaintCellPick();" in WEB_SCRIPT
