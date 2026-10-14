from pathlib import Path
import shutil
import subprocess

import pytest


CRM_PACKAGE = Path(__file__).resolve().parents[1]
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(
    encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(
    encoding="utf-8")
FEATURE = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.cpp").read_text(
    encoding="utf-8")
NODE = shutil.which("node")


def _function(name):
    return WEB_SCRIPT.split(f"function {name}(", 1)[1].split("\n  }\n", 1)[0]


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_the_tab_strip_and_list_helpers_hold_up_when_run():
    result = subprocess.run(
        [NODE, "--no-warnings", str(CRM_PACKAGE / "tests" / "pipeline_tabs_checks.js")],
        capture_output=True, text=True, timeout=600,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin"})
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert "pipeline tab checks passed" in result.stdout


def test_there_is_no_every_contact_tab():
    assert "All contacts" not in WEB_SCRIPT
    assert "All contacts" not in WEB_VIEW
    assert "forkmesh-crm-columns:" not in WEB_SCRIPT
    filtered = _function("crmFilteredContacts")
    assert "if (!shared && !(view && crmContactOnTab(contact, view, pipelines))) return false;" in filtered
    assert 'const CRM_PIPELINE_TAB_PREFIX = "pipeline:";' in WEB_SCRIPT
    assert 'const CRM_NO_PIPELINE_TAB = "no-pipeline";' in WEB_SCRIPT
    assert 'constexpr char kPipelineTabPrefix[] = "pipeline:";' in FEATURE
    assert 'constexpr char kNoPipelineTabId[] = "no-pipeline";' in FEATURE
    tabs = _function("renderCrmTabs")
    assert "const tabs = crmTabViews();" in tabs
    assert "Contacts whose pipeline was deleted. Right-click them to move them into another one." in tabs
    assert 'view.virtual ? "" : ` data-crm-tab-drag=' in tabs


def test_the_open_tab_falls_back_to_the_first_and_opens_on_its_first_stage():
    sync = _function("crmSyncActiveTab")
    assert "state.crmView.activeView = tabs[0]?.id || \"\";" in sync
    assert "selectCrmFirstStage();" in sync
    assert "crmSyncActiveTab();" in _function("renderCrmContacts")
    assert 'stages.find((stage) => stage.toLowerCase() === "new") || stages[0]' in _function("crmFirstStage")
    assert "crmTabViews().some((view) => view.id === id)) { selectCrmSheet(id); return; }" in _function("openCrmListFromUrl")


def test_virtual_tabs_store_nothing_on_the_worker():
    assert "if (view?.virtual) return state.crmView.virtualColumns[view.id] || [];" in _function("crmStoredColumns")
    rename = _function("renameCrmSheet")
    pipeline = rename.split("    if (pipeline) {\n", 1)[1].split("const previous", 1)[0]
    assert 'crmOrganizationPath("/settings"), "PUT", { flows }' in pipeline
    assert "/views/" not in pipeline


def test_the_stage_strip_sits_above_the_filters_and_edits_its_pipeline():
    contacts = WEB_VIEW.split('data-crm-section="contacts"', 1)[1]
    assert contacts.index("data-crm-stage-strip") < contacts.index("data-crm-search")
    assert contacts.index("data-crm-tabs") < contacts.index("data-crm-stage-strip")
    assert 'strip?.addEventListener("contextmenu", openCrmStageStripMenu);' in WEB_SCRIPT
    assert "pickCrmStage(chip.dataset.crmStageChip)" in WEB_SCRIPT
    assert 'const next = stage === select.value ? "" : stage;' in _function("pickCrmStage")
    assert "label: `Edit “${crmFlowName(flowId)}” stages…`" in _function("crmEditStagesItem")
    assert "renderCrmStageStrip();" in _function("renderCrmContacts")
    assert ".crm-stage-chip[aria-pressed=\"true\"]" in WEB_VIEW
    assert 'QColor(QStringLiteral("#f85149"))' in (CRM_PACKAGE / "content" / "desktop" / "CrmStageStrip.cpp").read_text(encoding="utf-8")


def test_the_tab_menu_matches_the_desktop():
    menu = _function("openCrmSheetMenu")
    for label in ("Rename…", "Add members…", "Copy link", "Activity history…", "Move left",
                  "Move right", '{ heading: "Merge into" }', "Pipeline type", "New list…",
                  "New pipeline…", "Manage pipelines…", "Delete list…", "Delete pipeline…",
                  "An organization keeps at least one pipeline",
                  "use Delete pipeline… to remove it"):
        assert label in menu, label
    assert "...crmSheetShareItems(view)" in menu
    assert "const CRM_SHARE_SHOWN = 15;" in WEB_SCRIPT
    assert "Delete sheet" not in WEB_SCRIPT + WEB_VIEW
    assert "+ List" in WEB_VIEW and "+ Sheet" not in WEB_VIEW
    assert "&& (!view.flow || crmPipelineLists(view.flow).length > 1)" in _function("crmCanDeleteList")
    assert "Its contacts stay in the organization and on their pipelines." in _function("deleteCrmSheet")


def test_add_members_turns_a_pipeline_tab_into_a_list():
    save = _function("saveCrmMembers")
    assert 'crmOrganizationPath("/views"), "POST", {' in save
    assert 'name: view.name, flow: view.flow, visibility: "organization", sharedWith: wanted' in save
    assert 'crmOrganizationPath(`/views/${view.id}`), "PATCH", { sharedWith: wanted }' in save
    assert "No ForkMesh account: ${missing.join(\", \")}." in save
    assert "data-crm-members-dialog" in WEB_VIEW


def test_deleting_a_pipeline_drops_its_flow_and_its_lists():
    remove = _function("deleteCrmPipeline")
    assert 'crmOrganizationPath("/settings"), "PUT", { flows: remaining }' in remove
    assert 'crmOrganizationPath(`/views/${listId}`), "DELETE", {}' in remove
    assert "under No pipeline" in remove
    assert "An organization keeps at least one pipeline." in remove


def test_merging_a_list_is_one_bulk_move_a_layout_and_a_delete():
    merge = _function("mergeCrmSheetInto")
    assert "Its contacts move onto “${target.name}”, every field it shows is shown there too, and “${source.name}” is then deleted." in merge
    order = [merge.index(needle) for needle in (
        'crmOrganizationPath("/contacts/bulk"), "POST", { edits }',
        'crmOrganizationPath(`/views/${target.id}`), "PATCH", { columns }',
        'crmOrganizationPath(`/views/${source.id}`), "DELETE", {}')]
    assert order == sorted(order)


def test_new_lists_pick_a_pipeline_and_new_contacts_join_only_plain_lists():
    assert "data-crm-sheet-flow" in WEB_VIEW
    save = _function("saveCrmSheet")
    assert "if (!id && flow && state.crmView.canManageSettings) body.flow = flow;" in save
    dialog = _function("openCrmSheetDialog")
    assert 'const open = crmActiveShared() ? "" : crmActiveView()?.flow || "";' in dialog
    assert "flow.disabled = !state.crmView.canManageSettings;" in dialog
    contact = _function("saveCrmContact")
    assert "if (sheet && !sheet.virtual && !sheet.flow && !id) body.views = [sheet.id];" in contact
    assert 'storedSheet(listOpen) && text(listOpen, "flow").isEmpty()' in FEATURE


def test_contacts_move_to_pipeline_lists_and_leave_lists_that_have_siblings():
    menu = _function("openCrmContactMenu")
    assert "crmMenuGroup(crmPipelineMoveItems([id]))" in menu
    assert '{ heading: "Move to pipeline list" }' in menu
    assert "(!open.flow || crmPipelineLists(open.flow).length > 1)" in menu
    assert 'crmOrganizationPath("/contacts/bulk"), "POST", { edits }' in _function("crmBulkMoveContacts")


def test_pipelines_are_managed_in_one_dialog_with_a_lists_page():
    for hook in ("data-crm-pipelines-dialog", "data-crm-pipelines-body", 'data-crm-pipelines-page="lists"',
                 "data-crm-pipelines-open"):
        assert hook in WEB_VIEW, hook
    assert "data-crm-flow-editors" not in WEB_VIEW
    assert "flows: readCrmFlows()" not in WEB_SCRIPT
    assert "const CRM_STAGE_SHAPE = /^[a-z][a-z0-9-]*$/;" in WEB_SCRIPT
    click = _function("handleCrmPipelinesClick")
    assert "They keep that stage until they are moved to another one." in click
    assert "An organization keeps at least one pipeline. Add another first." in click
    assert '["New", "In progress", "Complete"]' in click
    save = _function("saveCrmPipelines")
    order = [save.index(needle) for needle in (
        'crmOrganizationPath("/settings"), "PUT", { flows: plan.flows }',
        'crmOrganizationPath("/contacts/bulk"), "POST", { edits: plan.stageEdits }',
        'crmOrganizationPath(`/views/${binding.id}`), "PATCH", { flow: binding.flow }')]
    assert order == sorted(order)
