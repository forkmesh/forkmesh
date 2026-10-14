"""Full web chat consumes only server-authorized private channel records."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHAT = (ROOT / "public" / "chat.js").read_text(encoding="utf-8")
HTML = (ROOT / "public" / "chat.html").read_text(encoding="utf-8")
CSS = (ROOT / "public" / "chat.css").read_text(encoding="utf-8")
PROTOCOL = (
    ROOT.parent / "www" / "public" / "docs" / "protocol" / "index.html"
).read_text(encoding="utf-8")


def _function_source(name):
    marker = f"function {name}("
    async_marker = f"async function {name}("
    start = CHAT.find(async_marker)
    if start < 0:
        start = CHAT.index(marker)
    next_function = CHAT.find("\nfunction ", start + 1)
    next_async = CHAT.find("\nasync function ", start + 1)
    ends = [value for value in (next_function, next_async) if value >= 0]
    return CHAT[start:min(ends) if ends else len(CHAT)]


def test_chat_loads_authorized_private_channels_instead_of_fixed_labels():
    assert 'const PRIVATE_CHANNELS_ENDPOINT = "/api/chat/channels"' in CHAT
    assert "async function refreshPrivateChannels(" in CHAT
    assert "function reconcilePrivateChannels(" in CHAT
    assert "Authorization" in CHAT
    assert 'DEFAULT_CHANNELS = ["#general", "#welcome", "#random"]' not in CHAT
    assert "privateChannels" in CHAT
    assert "keyVersion" in CHAT


def test_private_channel_access_uses_ephemeral_server_room_ticket():
    source = _function_source("fetchRoomAccess")
    assert '"/room-access"' in source
    assert "access.webSocketUrl" in CHAT
    assert "access.passphrase" in CHAT
    assert "access.room" in CHAT
    assert "localStorage" not in source


def test_removed_or_rotated_channel_closes_stale_room_and_falls_back():
    source = _function_source("reconcilePrivateChannels")
    assert "keyVersion" in source
    assert "switchChatRoom" in source
    assert 'setActiveChannel(defaultChannelKey())' in source
    assert 'setActiveChannel("#general")' not in source
    assert "privateChannels.delete" in source or "privateChannels.clear" in source


def test_channel_controls_are_accessible_and_hidden_by_default():
    assert 'id="chat-channel-create"' in HTML
    assert 'aria-label="Create channel"' in HTML
    assert 'id="chat-channel-dialog"' in HTML
    assert 'aria-labelledby="chat-channel-dialog-title"' in HTML
    assert 'id="chat-channel-manage"' in HTML
    assert 'id="chat-channel-members"' in HTML
    assert 'id="chat-channel-error"' in HTML
    assert "[hidden] { display: none !important; }" in CSS
    assert "margin: auto;" in CSS
    assert "channel?.canManage" in CHAT


def test_channel_creator_can_create_invite_list_and_remove_members():
    assert "async function createChannel(" in CHAT
    assert 'id="chat-channel-create-form"' in HTML
    assert "async function refreshChannelMembers(" in CHAT
    assert "async function inviteChannelMember(" in CHAT
    assert "async function removeChannelMember(" in CHAT
    assert '`/api/chat/channels/${channel.id}/members`' in CHAT
    assert 'method: "POST"' in CHAT
    assert 'method: "DELETE"' in CHAT


def test_create_form_supports_private_public_and_initial_members():
    for marker in (
        'id="chat-channel-visibility"',
        '<option value="private"',
        '<option value="public"',
        'id="chat-channel-initial-members"',
    ):
        assert marker in HTML

    assert 'id="chat-channel-visibility-badge"' in HTML
    assert 'visibility === "private"' in _function_source("createChannel")
    assert "channelInviteeNames()" in _function_source("createChannel")
    assert '.split(",")' in _function_source("channelInviteeNames")
    assert "registeredUsers" in CHAT


def test_channel_visibility_controls_labels_and_private_member_management():
    reconcile = _function_source("reconcilePrivateChannels")
    controls = _function_source("updateAdminChannelControls")
    rooms = _function_source("renderRooms")

    assert "visibility" in reconcile
    assert "channel?.canManage" in controls
    assert "Public channel" in rooms
    assert "Private channel" in rooms


def test_office_embed_opens_channel_creation_to_organization_members():
    controls = _function_source("updateAdminChannelControls")
    author = _function_source("canAuthorChannelsHere")
    load = _function_source("loadOfficeOrganizationMembership")
    dialog = _function_source("openChannelDialog")

    # The Office embed used to hide creation from everyone and offer only an
    # admin-only link out to full chat.
    assert "channelCreateBtn.hidden = !session || isOfficeEmbed" not in CHAT
    assert "isOfficeEmbed && userSession()?.isAdmin" not in CHAT
    assert "channelCreateBtn.hidden = !authorsChannels" in controls
    assert "officeManageLink.hidden = !(isOfficeEmbed && authorsChannels)" in (
        controls
    )

    # Membership decides it inside the embed; full chat stays open to any
    # signed-in account, matching what the channels API authorizes.
    assert "!isOfficeEmbed || officeOrganizationMember" in author
    assert "session.isAdmin" in author
    assert 'const OFFICE_FLOOR_ACCESS_ENDPOINT = "/api/world/office/floors"' in (
        CHAT
    )
    assert 'new Set(["owner", "admin", "maintainer", "member"])' in CHAT
    assert "OFFICE_FLOOR_ACCESS_ENDPOINT" in load
    assert "data?.organizationRole" in load
    assert "officeOrganizationMember = false" in load
    assert "await loadOfficeOrganizationMembership()" in CHAT

    # The role comes from the Office endpoint, never from the embedding page.
    assert "organizationRole" not in _function_source("receiveOfficeMessage")
    assert "if (creating && !canAuthorChannelsHere()) return;" in dialog


def test_office_embed_css_lets_the_create_control_and_dialog_render():
    # The [hidden] gate above is only half of it: the embed also blanket-hides
    # .chat-admin-action and the dialog in CSS, in BOTH the inline <style> and
    # chat.css. Re-hiding either one silently reverts the feature.
    for source in (HTML, CSS):
        assert (
            'html[data-chat-embed="office"] #chat-channel-dialog '
            "{ display: none; }"
        ) not in source
        assert 'html[data-chat-embed="office"] #chat-channel-create {' in source
        assert "display: inline-flex;" in source
        assert (
            'html[data-chat-embed="office"] #chat-channel-dialog:not([open])'
        ) in source
    # Manage, webhooks and direct messages stay out of the embed.
    assert 'html[data-chat-embed="office"] .chat-admin-action { display: none; }' in (
        CSS
    )
    assert 'html[data-chat-embed="office"] #chat-direct-dialog { display: none; }' in (
        HTML
    )


def test_channel_manager_creates_and_revokes_one_time_webhook_urls():
    assert 'id="chat-channel-webhook-form"' in HTML
    assert 'id="chat-channel-webhooks"' in HTML
    assert "async function createChannelWebhook(" in CHAT
    assert "async function removeChannelWebhook(" in CHAT
    assert "shown only once" in CHAT
    assert '`/api/chat/channels/${channel.id}/webhooks`' in CHAT


def test_channel_settings_open_from_a_control_beside_the_channel_name():
    heading = HTML[
        HTML.index('<span class="chat-channel-heading">'):
        HTML.index('<div class="chat-main-controls">')
    ]
    # The entry point sits with the channel name, not off in the status rail.
    assert 'id="chat-channel-manage"' in heading
    assert 'aria-controls="chat-channel-settings"' in heading
    assert 'aria-expanded="false"' in heading
    for marker in (
        'id="chat-channel-settings"',
        'id="chat-channel-settings-close"',
        'id="chat-channel-settings-error"',
        'id="chat-channel-settings-name"',
        'id="chat-channel-settings-topic"',
        'id="chat-channel-settings-visibility"',
    ):
        assert marker in HTML
    # Members and webhooks moved out of the create dialog into that panel.
    assert HTML.index('id="chat-channel-settings"') < HTML.index(
        'id="chat-channel-members-section"')
    assert HTML.index('id="chat-channel-webhooks-section"') < HTML.index(
        'id="chat-channel-dialog"')
    assert ".chat-channel-settings {" in CSS
    assert (
        'html[data-chat-embed="office"] .chat-channel-settings '
        "{ display: none; }"
    ) in CSS
    assert "toggleChannelSettings()" in CHAT
    assert "closeChannelSettings({ restoreFocus: true })" in CHAT


def test_channel_settings_save_name_topic_and_visibility_together():
    save = _function_source("saveChannelSettings")
    assert 'method: "PATCH"' in save
    assert "JSON.stringify({ name, topic, visibility })" in save
    assert 'makePlain("channel-topic"' in save
    # Closing a public channel rotates its key, so re-read the room after.
    assert "refreshPrivateChannels({ selectSaved: false })" in save
    populate = _function_source("populateChannelSettings")
    members_gate = 'channelMembersSection.hidden = channel.visibility'
    assert members_gate + ' !== "private"' in populate


def test_channel_deletion_is_confirmed_and_leaves_the_removed_room():
    assert 'id="chat-channel-delete"' in HTML
    assert 'id="chat-channel-delete-confirm"' in HTML
    assert 'id="chat-channel-delete-confirmed"' in HTML
    assert "channelDeleteConfirm.hidden = false" in CHAT
    remove = _function_source("deleteActiveChannel")
    assert 'method: "DELETE"' in remove
    assert "closeChannelSettings()" in remove
    # Reconciling the emptied list is what falls back to #general.
    assert "refreshPrivateChannels({ selectSaved: false })" in remove


def test_webhook_urls_come_from_this_tab_or_a_rotation_only():
    rotate = _function_source("rotateChannelWebhook")
    assert "/webhooks/${id}`" in rotate
    assert 'method: "POST"' in rotate
    assert "shown only once" in CHAT
    assert "URL hidden" in CHAT
    # A bearer capability never reaches storage that outlives the tab.
    note = _function_source("noteWebhookUrl")
    assert "revealedWebhookUrls.set" in note
    assert "localStorage" not in note
    assert "sessionStorage" not in note


def test_topic_slash_command_updates_managed_channel_metadata():
    reconcile = _function_source("reconcilePrivateChannels")
    heading = _function_source("updateChannelHeading")
    command = _function_source("runTopicSlashCommand")
    slash = _function_source("handleSlashCommand")

    assert "topic: String(value.topic" in reconcile
    assert "channelTopicEl.textContent" in heading
    assert "activeTopicChannel()" in command
    assert 'method: "PATCH"' in command
    assert "JSON.stringify({ topic })" in command
    assert "channel.topic =" in command
    assert 'makePlain("channel-topic"' in command
    assert "refreshPrivateChannels({ selectSaved: false" in CHAT
    assert '/(issue|topic)' in slash
    assert 'id="chat-channel-topic"' in HTML


def test_only_opaque_private_channel_id_is_persisted_as_preference():
    source = _function_source("setActiveChannel")
    assert "record.id" in source
    assert "ACTIVE_CHANNEL_KEY" in source
    assert "passphrase" not in source
    assert "webSocketUrl" not in source


def test_protocol_documents_private_access_rotation_and_attachments():
    for marker in (
        'id="private-chat-channels"',
        "/api/chat/channels/&lt;channel-id&gt;/room-access",
        "60-second signed WebSocket ticket",
        "Removing a private member\n          atomically increments the key",
        "fileName",
        "fileMime",
        "1 MiB",
        "CHAT_HISTORY_MAX_BYTES_PER_ROOM",
        "16 MiB",
    ):
        assert marker in PROTOCOL
