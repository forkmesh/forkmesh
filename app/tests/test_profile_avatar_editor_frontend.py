"""Frontend contracts for the Settings profile-picture editor."""

from _dashboard_bundle import assembled_dashboard_js
from _dashboard_shell import assembled_dashboard_page


def test_top_bar_avatar_links_directly_to_settings():
    dashboard = assembled_dashboard_page("home")
    link_start = dashboard.index("<a\n              data-profile-settings-button")
    link = dashboard[link_start:dashboard.index("</a>", link_start)]

    assert 'href="/dashboard/settings"' in link
    assert 'aria-label="Open account settings"' in link
    assert "data-dashboard-profile-avatar" in link


def test_profile_picture_edit_opens_an_image_picker():
    settings = assembled_dashboard_page("settings")
    dashboard_js = assembled_dashboard_js()

    assert "data-profile-avatar-edit" in settings
    assert 'data-profile-avatar-input type="file"' in settings
    assert 'accept="image/png,image/jpeg,image/webp"' in settings
    assert 'event.target.closest("[data-profile-avatar-edit]")' in dashboard_js
    assert "input.click()" in dashboard_js


def test_chosen_avatar_is_square_cropped_to_png_and_saved():
    dashboard_js = assembled_dashboard_js()

    assert 'dialogLabel: "Crop profile picture"' in dashboard_js
    assert 'outputType: "image/png"' in dashboard_js
    assert "aspectRatio: 1" in dashboard_js
    assert "PROFILE_AVATAR_MAX_BYTES = 256 * 1024" in dashboard_js
    assert "await postProfile({ avatarPng: result.dataUrl })" in dashboard_js
    assert 'setProfileAvatarHint("Profile picture saved.", "good")' in dashboard_js
