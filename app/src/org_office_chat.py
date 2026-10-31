import org_office as o


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


OFFICE_KIND = "office"
ROOMS = ("general", "engineering", "design", "coffee", "lobby", "garden")
MEMBER_ROLES = ("owner", "admin", "maintainer", "member", "auditor")


def channel_name(org, room):
    return "office/" + str(org) + "/" + room


async def _mapped(env, org_bi):
    rows = await d1_all(
        env,
        "SELECT room,channel_id FROM org_office_channels WHERE org_bi=?",
        org_bi)
    return {str(r.get("room")): str(r.get("channel_id")) for r in rows or []}


async def _members(env, org_bi):
    rows = await d1_all(
        env,
        "SELECT om.member_bi AS member_bi,om.role AS role,om.name AS name "
        "FROM org_members om WHERE om.org_bi=? AND om.role IN "
        "('owner','admin','maintainer','member','auditor')",
        org_bi)
    team_rows = await d1_all(
        env,
        "SELECT tm.member_bi AS member_bi,tm.team AS team "
        "FROM org_team_members tm INNER JOIN org_teams ot "
        "ON ot.org_bi=tm.org_bi AND ot.team=tm.team "
        "WHERE tm.org_bi=? AND ot.permission IN "
        "('read','write','maintain','admin')",
        org_bi)
    teams = {}
    for row in team_rows or []:
        teams.setdefault(str(row.get("member_bi")), []).append(row.get("team"))
    out = {}
    for row in rows or []:
        member_bi = str(row.get("member_bi") or "")
        name = str(row.get("name") or "").strip().lower()
        if not member_bi or not name:
            continue
        out[member_bi] = {
            "name": name,
            "unlocked": set(o.unlocked_zones(teams.get(member_bi, []),
                                             str(row.get("role") or ""))),
        }
    return out


def eligible(members, room):
    zone = o.chat_channel_zone(room)
    if zone in o.GATED_ZONES:
        return {bi for bi, m in members.items() if zone in m["unlocked"]}
    return set(members)


async def ensure_channels(env, org_bi, org):
    mapped = await _mapped(env, org_bi)
    now = int(Date.now())
    for room in ROOMS:
        if room in mapped:
            continue
        channel_id = new_world_peer_id()
        record = {"name": channel_name(org, room), "createdBy": "",
                  "visibility": "private", "kind": OFFICE_KIND,
                  "org": org, "room": room}
        name_bi = await blind_index(
            env, "chat-channel-name:" + channel_name(org, room))
        await d1_run(
            env,
            "INSERT INTO chat_channels (channel_id,name_bi,data,"
            "created_by_bi,created_at,updated_at,key_version) "
            "VALUES (?,?,?,?,?,?,1) ON CONFLICT(name_bi) DO NOTHING",
            channel_id, name_bi, await encrypt_row(env, record),
            org_bi, now, now)
        row = await d1_first(
            env, "SELECT channel_id FROM chat_channels WHERE name_bi=? "
            "AND created_by_bi=?", name_bi, org_bi)
        if not row:
            continue
        await d1_run(
            env,
            "INSERT INTO org_office_channels (org_bi,room,channel_id,"
            "created_at) VALUES (?,?,?,?) ON CONFLICT(org_bi,room) "
            "DO NOTHING",
            org_bi, room, str(row.get("channel_id")), now)
    return await _mapped(env, org_bi)


async def sync(env, org_bi, org, create=True):
    mapped = await ensure_channels(env, org_bi, org) if create \
        else await _mapped(env, org_bi)
    if not mapped:
        return {}
    members = await _members(env, org_bi)
    now = int(Date.now())
    changed = set()
    names = {}
    for room, channel_id in mapped.items():
        want = eligible(members, room)
        rows = await d1_all(
            env,
            "SELECT member_bi,data FROM chat_channel_members "
            "WHERE channel_id=?",
            channel_id)
        have = {str(r.get("member_bi")) for r in rows or []}
        for r in rows or []:
            if str(r.get("member_bi")) not in want:
                record = await decrypt_row(env, r.get("data")) or {}
                names[str(r.get("member_bi"))] = str(
                    record.get("username") or "")
        for member_bi in sorted(want - have):
            await d1_run(
                env,
                "INSERT INTO chat_channel_members (channel_id,member_bi,data,"
                "invited_by_bi,joined_at) VALUES (?,?,?,?,?) "
                "ON CONFLICT(channel_id,member_bi) DO NOTHING",
                channel_id, member_bi,
                await encrypt_row(env, {"username": members[member_bi]["name"]}),
                org_bi, now)
        removed = sorted(have - want)
        for member_bi in removed:
            before = await d1_first(
                env, "SELECT key_version FROM chat_channels WHERE channel_id=?",
                channel_id)
            await d1_run(
                env,
                "DELETE FROM chat_channel_members WHERE channel_id=? "
                "AND member_bi=?", channel_id, member_bi)
            await d1_run(
                env,
                "UPDATE chat_channels SET updated_at=MAX(updated_at,?) "
                "WHERE channel_id=?", now, channel_id)
            version = int((before or {}).get("key_version") or 1)
            for revoke in (_revoke_chat_channel_room,
                           _revoke_office_channel_room):
                try:
                    await revoke(env, channel_id, version)
                except Exception:
                    pass
        changed.update(want - have)
        changed.update(removed)
    for member_bi in sorted(changed):
        name = (members.get(member_bi) or {}).get("name") or \
            names.get(member_bi, "")
        if name:
            try:
                await notify_account_event(env, name, "private-channels")
            except Exception:
                pass
    return mapped


async def dissolve(env, org_bi):
    mapped = await _mapped(env, org_bi)
    for channel_id in mapped.values():
        before = await d1_first(
            env, "SELECT key_version FROM chat_channels WHERE channel_id=?",
            channel_id)
        rows = await d1_all(
            env, "SELECT data FROM chat_channel_members WHERE channel_id=?",
            channel_id)
        for table in ("chat_channel_webhooks", "chat_channel_invites",
                      "chat_channel_members", "chat_channel_reads"):
            await d1_run(
                env, "DELETE FROM %s WHERE channel_id=?" % table, channel_id)
        await d1_run(
            env, "DELETE FROM chat_channels WHERE channel_id=? "
            "AND created_by_bi=?", channel_id, org_bi)
        version = int((before or {}).get("key_version") or 1)
        for revoke in (_revoke_chat_channel_room,
                       _revoke_office_channel_room):
            try:
                await revoke(env, channel_id, version)
            except Exception:
                pass
        for row in rows or []:
            record = await decrypt_row(env, row.get("data")) or {}
            name = str(record.get("username") or "")
            if name:
                try:
                    await notify_account_event(env, name, "private-channels")
                except Exception:
                    pass
    await d1_run(
        env, "DELETE FROM org_office_channels WHERE org_bi=?", org_bi)
    return len(mapped)


async def rooms_for(env, org_bi, account_bi, mapped):
    if not mapped:
        return {}
    rows = await d1_all(
        env,
        "SELECT channel_id FROM chat_channel_members WHERE member_bi=?",
        account_bi)
    mine = {str(r.get("channel_id")) for r in rows or []}
    return {room: cid for room, cid in mapped.items() if cid in mine}


async def resync_org(env, org_name):
    try:
        org_bi, row = await _org_row(env, org_name)
        if not row:
            return
        await sync(env, str(org_bi), str(org_name).strip().lower(),
                   create=False)
    except Exception:
        return
