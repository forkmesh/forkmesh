"""D1-backed API orchestration for ForkMesh World community services.

The module receives a tiny runtime adapter from ``entry.py``.  Keeping route
logic here avoids coupling the independently testable validation rules in
``world_community.py`` to Cloudflare/Pyodide globals, while the adapter ensures
the API uses ForkMesh's established account sessions, blind indexes, D1
helpers, and metadata-only sensitive audit trail.
"""

import json

import world_community as policy


FEDIVERSE_PREFIX = "/api/world/fediverse"
MEDIA_PREFIX = "/api/world/media"


def _response(runtime, data, status=200, cache_control=None, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        data,
        status=status,
        cache_control=cache_control or
        "no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _fediverse_target(path):
    clean = str(path or "").rstrip("/")
    if clean == FEDIVERSE_PREFIX:
        return ""
    prefix = FEDIVERSE_PREFIX + "/"
    candidate = clean[len(prefix):] if clean.startswith(prefix) else ""
    return candidate if policy.valid_resource_id(candidate) else None


def _media_route(path):
    clean = str(path or "").rstrip("/")
    if clean == MEDIA_PREFIX:
        return ("spaces", "", "")
    prefix = MEDIA_PREFIX + "/"
    if not clean.startswith(prefix):
        return None
    parts = clean[len(prefix):].split("/")
    if parts == ["spaces"]:
        return ("spaces", "", "")
    if len(parts) >= 2 and parts[0] == "spaces":
        space_id = parts[1]
        if not policy.valid_resource_id(space_id):
            return None
        if len(parts) == 2:
            return ("space", space_id, "")
        if len(parts) == 3 and parts[2] in (
                "items", "playback", "stop", "schedules", "roles"):
            return (parts[2], space_id, "")
        if (
            len(parts) == 4
            and parts[2] in ("items", "schedules", "roles")
            and policy.valid_resource_id(parts[3])
        ):
            return (parts[2][:-1], space_id, parts[3])
    return None


async def _body(runtime):
    data, error = await runtime.json_body(
        policy.WORLD_COMMUNITY_BODY_MAX_BYTES)
    if error:
        status = 413 if error == "payload_too_large" else 400
        return None, _response(runtime, {"error": error}, status=status)
    return data, None


async def _actor(runtime, data=None):
    account_bi, account = await runtime.session(data or {})
    name = str((account or {}).get("name") or "").strip().lower()
    return account_bi, name


async def handle_fediverse(runtime, path):
    """Handle public reads and platform-admin directory mutations."""
    await runtime.ensure_schema()
    target = _fediverse_target(path)
    if target is None:
        return _response(runtime, {"error": "not_found"}, status=404)
    method = runtime.method()

    if method == "GET":
        if target:
            return _response(runtime, {"error": "not_found"}, status=404)
        rows = await runtime.d1_all(
            "SELECT instance_id,kind,data,updated_at "
            "FROM world_fediverse_instances "
            "ORDER BY kind ASC, updated_at DESC LIMIT ?",
            policy.MAX_DIRECTORY_RECORDS,
        )
        return _response(
            runtime,
            policy.fediverse_directory_payload(rows, runtime.now()),
            cache_control="public, max-age=60, stale-while-revalidate=300",
        )

    if method not in ("POST", "PATCH", "DELETE"):
        return _response(
            runtime,
            {"error": "method_not_allowed"},
            status=405,
            allow="GET, POST, PATCH, DELETE",
        )
    if (method == "POST" and target) or (
            method in ("PATCH", "DELETE") and not target):
        return _response(runtime, {"error": "not_found"}, status=404)

    data = {}
    if method in ("POST", "PATCH"):
        data, body_error = await _body(runtime)
        if body_error is not None:
            return body_error
    actor_bi, actor = await _actor(runtime, data)
    if not actor:
        return _response(runtime, {"error": "invalid_session"}, status=401)
    if not await runtime.is_admin(actor):
        await runtime.audit(
            actor,
            "world.fediverse." + method.lower(),
            "fediverse_instance",
            target or "new",
            outcome="denied",
            details={"reason": "platform_administrator_required"},
        )
        return _response(runtime, {"error": "forbidden"}, status=403)

    now = runtime.now()
    if method == "POST":
        record, error = policy.normalize_fediverse_record(data, now)
        if error:
            return _response(runtime, {"error": error}, status=400)
        count = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM world_fediverse_instances")
        if int((count or {}).get("n") or 0) >= policy.MAX_DIRECTORY_RECORDS:
            return _response(runtime, {"error": "directory_full"}, status=409)
        duplicate = await runtime.d1_first(
            "SELECT instance_id FROM world_fediverse_instances "
            "WHERE host=? OR url=?",
            record["host"], record["url"],
        )
        if duplicate:
            return _response(runtime, {"error": "instance_exists"}, status=409)
        instance_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO world_fediverse_instances "
            "(instance_id,kind,host,url,data,created_by_bi,created_at,updated_at) "
            "VALUES (?,?,?,?,?,?,?,?)",
            instance_id,
            record["kind"],
            record["host"],
            record["url"],
            json.dumps(
                {key: value for key, value in record.items() if key != "host"},
                sort_keys=True,
                separators=(",", ":"),
            ),
            actor_bi,
            now,
            now,
        )
        await runtime.audit(
            actor,
            "world.fediverse.create",
            "fediverse_instance",
            instance_id,
            details={"kind": record["kind"], "consent": True},
        )
        return _response(
            runtime,
            {"ok": True, "instance": {
                **record, "id": instance_id, "updatedAt": now}},
            status=201,
        )

    existing = await runtime.d1_first(
        "SELECT instance_id,kind,host,url,data,updated_at "
        "FROM world_fediverse_instances WHERE instance_id=?",
        target,
    )
    if not existing:
        return _response(runtime, {"error": "not_found"}, status=404)
    if method == "DELETE":
        await runtime.d1_run(
            "DELETE FROM world_fediverse_instances WHERE instance_id=?",
            target,
        )
        await runtime.audit(
            actor,
            "world.fediverse.delete",
            "fediverse_instance",
            target,
            details={"kind": str(existing.get("kind") or "")},
        )
        return _response(runtime, {"ok": True, "deleted": target})

    # PATCH supports bounded partial updates, but the administrator must
    # re-affirm the public/consent boundary on every update.
    if not policy.has_public_consent_attestation(data):
        return _response(
            runtime,
            {"error": "public_consent_attestation_required"},
            status=400,
        )
    current = policy.fediverse_public_record(existing, now) or {}
    current.pop("id", None)
    current.pop("updatedAt", None)
    merged = {**current, **data}
    record, error = policy.normalize_fediverse_record(merged, now)
    if error:
        return _response(runtime, {"error": error}, status=400)
    duplicate = await runtime.d1_first(
        "SELECT instance_id FROM world_fediverse_instances "
        "WHERE (host=? OR url=?) AND instance_id<>?",
        record["host"], record["url"], target,
    )
    if duplicate:
        return _response(runtime, {"error": "instance_exists"}, status=409)
    await runtime.d1_run(
        "UPDATE world_fediverse_instances SET kind=?,host=?,url=?,data=?,"
        "updated_at=? WHERE instance_id=?",
        record["kind"],
        record["host"],
        record["url"],
        json.dumps(
            {key: value for key, value in record.items() if key != "host"},
            sort_keys=True,
            separators=(",", ":"),
        ),
        now,
        target,
    )
    await runtime.audit(
        actor,
        "world.fediverse.update",
        "fediverse_instance",
        target,
        details={"kind": record["kind"], "consent": True},
    )
    return _response(
        runtime,
        {"ok": True, "instance": {
            **record, "id": target, "updatedAt": now}},
    )


async def _space_and_role(runtime, space_id, account_bi):
    space = await runtime.d1_first(
        "SELECT * FROM world_media_spaces WHERE space_id=?",
        space_id,
    )
    if not space:
        return None, ""
    if str(space.get("owner_bi") or "") == str(account_bi or ""):
        return space, "owner"
    role = await runtime.d1_first(
        "SELECT role FROM world_media_roles "
        "WHERE space_id=? AND account_bi=?",
        space_id, account_bi,
    )
    return space, (
        "moderator" if role and role.get("role") == "moderator" else "")


async def _deny_media(runtime, actor, action, space_id, owner_only=False):
    await runtime.audit(
        actor,
        "world.media." + action,
        "media_space",
        space_id,
        outcome="denied",
        details={
            "reason": (
                "owner_required" if owner_only else "owner_or_moderator_required"
            )
        },
    )
    return _response(runtime, {"error": "forbidden"}, status=403)


async def _media_detail(runtime, space, viewer_role):
    space_id = str(space.get("space_id") or "")
    item_rows = await runtime.d1_all(
        "SELECT item_id,status,data,created_at FROM world_media_items "
        "WHERE space_id=? AND status<>'removed' "
        "ORDER BY position ASC,created_at ASC LIMIT ?",
        space_id, policy.MAX_MEDIA_ITEMS_RESPONSE,
    )
    schedule_rows = await runtime.d1_all(
        "SELECT schedule_id,item_id,status,starts_at,ends_at,data "
        "FROM world_media_schedules WHERE space_id=? "
        "AND status<>'cancelled' ORDER BY starts_at ASC LIMIT ?",
        space_id, policy.MAX_MEDIA_SCHEDULES_RESPONSE,
    )
    role_rows = []
    if viewer_role == "owner":
        role_rows = await runtime.d1_all(
            "SELECT role_id,account_label,role,created_at "
            "FROM world_media_roles WHERE space_id=? "
            "ORDER BY created_at ASC LIMIT ?",
            space_id, policy.MAX_MEDIA_MODERATORS_PER_SPACE,
        )
    playback_row = await runtime.d1_first(
        "SELECT item_id,state,position_ms,started_at,changed_at,revision "
        "FROM world_media_playback WHERE space_id=?",
        space_id,
    )
    return policy.media_detail_payload(
        space, viewer_role, item_rows, schedule_rows, role_rows,
        playback_row, runtime.now())


async def handle_media(runtime, path):
    """Handle authenticated shared-media space and moderation routes."""
    await runtime.ensure_schema()
    route = _media_route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, status=404)
    resource, space_id, resource_id = route
    method = runtime.method()
    data = {}
    if method in ("POST", "PATCH", "DELETE"):
        # DELETE routes do not require a body, but accept a bounded empty JSON
        # object for clients that put their session token there.
        data, body_error = await _body(runtime)
        if body_error is not None:
            return body_error
    account_bi, actor = await _actor(runtime, data)
    if not actor:
        return _response(runtime, {"error": "invalid_session"}, status=401)
    now = runtime.now()

    if resource == "spaces":
        if method == "GET":
            rows = await runtime.d1_all(
                "SELECT s.*, "
                "(SELECT COUNT(*) FROM world_media_items i "
                " WHERE i.space_id=s.space_id AND i.status<>'removed') "
                "AS item_count, "
                "(SELECT COUNT(*) FROM world_media_schedules c "
                " WHERE c.space_id=s.space_id AND c.status='scheduled') "
                "AS schedule_count "
                "FROM world_media_spaces s WHERE s.status='active' "
                "ORDER BY s.last_activity_at DESC LIMIT ?",
                policy.MAX_MEDIA_SPACES_RESPONSE,
            )
            role_rows = await runtime.d1_all(
                "SELECT space_id,role FROM world_media_roles "
                "WHERE account_bi=?",
                account_bi,
            )
            roles = {
                str(row.get("space_id") or ""): row.get("role")
                for row in role_rows or []
            }
            spaces = []
            for row in rows or []:
                role = (
                    "owner"
                    if str(row.get("owner_bi") or "") == str(account_bi)
                    else roles.get(str(row.get("space_id") or ""), "")
                )
                spaces.append(policy.media_space_public(
                    row,
                    role,
                    row.get("item_count"),
                    row.get("schedule_count"),
                ))
            return _response(runtime, {
                "ok": True,
                "spaces": spaces,
                "retention": {
                    "removedItemsDays": (
                        policy.MEDIA_REMOVED_RETENTION_MS // 86400000),
                    "completedSchedulesDays": (
                        policy.MEDIA_SCHEDULE_RETENTION_MS // 86400000),
                    "archivedSpacesDays": (
                        policy.MEDIA_ARCHIVED_RETENTION_MS // 86400000),
                },
            })
        if method != "POST":
            return _response(
                runtime,
                {"error": "method_not_allowed"},
                status=405,
                allow="GET, POST",
            )
        normalized, error = policy.normalize_media_space(data)
        if error:
            return _response(runtime, {"error": error}, status=400)
        count = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM world_media_spaces "
            "WHERE owner_bi=? AND status='active'",
            account_bi,
        )
        if int((count or {}).get("n") or 0) >= (
                policy.MAX_MEDIA_SPACES_PER_OWNER):
            return _response(runtime, {"error": "space_limit"}, status=409)
        new_space_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO world_media_spaces "
            "(space_id,name,description,session_type,owner_bi,owner_label,"
            "status,playback_state,created_at,updated_at,last_activity_at,"
            "archived_at) VALUES (?,?,?,?,?,?,'active','idle',?,?,?,0)",
            new_space_id,
            normalized["name"],
            normalized["description"],
            normalized["sessionType"],
            account_bi,
            actor,
            now,
            now,
            now,
        )
        await runtime.d1_run(
            "INSERT INTO world_media_playback "
            "(space_id,item_id,state,position_ms,started_at,changed_at,"
            "changed_by_bi,revision,change_id) "
            "VALUES (?,'','idle',0,0,?,?,0,'')",
            new_space_id, now, account_bi,
        )
        await runtime.audit(
            actor,
            "world.media.space.create",
            "media_space",
            new_space_id,
            details={"sessionType": normalized["sessionType"]},
        )
        space = await runtime.d1_first(
            "SELECT * FROM world_media_spaces WHERE space_id=?",
            new_space_id,
        )
        return _response(
            runtime,
            await _media_detail(runtime, space, "owner"),
            status=201,
        )

    space, viewer_role = await _space_and_role(
        runtime, space_id, account_bi)
    if not space:
        return _response(runtime, {"error": "not_found"}, status=404)
    if str(space.get("status") or "") != "active":
        if not (resource == "space" and method == "GET"
                and viewer_role == "owner"):
            return _response(runtime, {"error": "not_found"}, status=404)

    if resource == "space":
        if method == "GET":
            return _response(
                runtime, await _media_detail(runtime, space, viewer_role))
        if method not in ("PATCH", "DELETE"):
            return _response(
                runtime,
                {"error": "method_not_allowed"},
                status=405,
                allow="GET, PATCH, DELETE",
            )
        if viewer_role != "owner":
            return await _deny_media(
                runtime, actor, "space." + method.lower(), space_id, True)
        if method == "DELETE":
            await runtime.d1_run(
                "UPDATE world_media_spaces SET status='archived',"
                "playback_state='stopped',archived_at=?,updated_at=?,"
                "last_activity_at=? WHERE space_id=?",
                now, now, now, space_id,
            )
            await runtime.d1_run(
                "UPDATE world_media_items SET status='stopped',updated_at=? "
                "WHERE space_id=? AND status='queued'",
                now, space_id,
            )
            await runtime.audit(
                actor,
                "world.media.space.archive",
                "media_space",
                space_id,
            )
            return _response(runtime, {"ok": True, "archived": space_id})
        merged = {
            "name": data.get("name", space.get("name")),
            "description": data.get(
                "description", space.get("description")),
            "sessionType": data.get(
                "sessionType", space.get("session_type")),
        }
        normalized, error = policy.normalize_media_space(merged)
        if error:
            return _response(runtime, {"error": error}, status=400)
        await runtime.d1_run(
            "UPDATE world_media_spaces SET name=?,description=?,"
            "session_type=?,updated_at=?,last_activity_at=? WHERE space_id=?",
            normalized["name"],
            normalized["description"],
            normalized["sessionType"],
            now,
            now,
            space_id,
        )
        await runtime.audit(
            actor,
            "world.media.space.update",
            "media_space",
            space_id,
            details={"sessionType": normalized["sessionType"]},
        )
        updated = await runtime.d1_first(
            "SELECT * FROM world_media_spaces WHERE space_id=?",
            space_id,
        )
        return _response(
            runtime, await _media_detail(runtime, updated, viewer_role))

    if resource == "items":
        if method != "POST":
            return _response(
                runtime, {"error": "method_not_allowed"},
                status=405, allow="POST")
        if viewer_role not in policy.MEDIA_ROLES:
            return await _deny_media(
                runtime, actor, "item.create", space_id)
        normalized, error = policy.normalize_media_item(data)
        if error:
            return _response(runtime, {"error": error}, status=400)
        count = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM world_media_items "
            "WHERE space_id=? AND status<>'removed'",
            space_id,
        )
        if int((count or {}).get("n") or 0) >= (
                policy.MAX_MEDIA_ITEMS_PER_SPACE):
            return _response(runtime, {"error": "playlist_full"}, status=409)
        position = await runtime.d1_first(
            "SELECT COALESCE(MAX(position),0) AS n FROM world_media_items "
            "WHERE space_id=?",
            space_id,
        )
        item_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO world_media_items "
            "(item_id,space_id,position,status,data,added_by_bi,created_at,"
            "updated_at,removed_at) VALUES (?,?,?,'queued',?,?,?,?,0)",
            item_id,
            space_id,
            int((position or {}).get("n") or 0) + 1,
            json.dumps(normalized, sort_keys=True, separators=(",", ":")),
            account_bi,
            now,
            now,
        )
        await runtime.d1_run(
            "UPDATE world_media_spaces SET playback_state='idle',"
            "updated_at=?,last_activity_at=? WHERE space_id=?",
            now, now, space_id,
        )
        await runtime.audit(
            actor,
            "world.media.item.create",
            "media_item",
            item_id,
            details={"provider": normalized["provider"]},
        )
        return _response(
            runtime,
            {"ok": True, "item": {
                **normalized,
                "id": item_id,
                "status": "queued",
                "addedAt": now,
            }},
            status=201,
        )

    if resource == "item":
        if method != "DELETE":
            return _response(
                runtime, {"error": "method_not_allowed"},
                status=405, allow="DELETE")
        if viewer_role not in policy.MEDIA_ROLES:
            return await _deny_media(
                runtime, actor, "item.remove", space_id)
        item = await runtime.d1_first(
            "SELECT item_id FROM world_media_items "
            "WHERE item_id=? AND space_id=? AND status<>'removed'",
            resource_id, space_id,
        )
        if not item:
            return _response(runtime, {"error": "not_found"}, status=404)
        await runtime.d1_run(
            "UPDATE world_media_items SET status='removed',removed_at=?,"
            "updated_at=? WHERE item_id=? AND space_id=?",
            now, now, resource_id, space_id,
        )
        await runtime.d1_run(
            "UPDATE world_media_spaces SET updated_at=?,last_activity_at=? "
            "WHERE space_id=?",
            now, now, space_id,
        )
        await runtime.d1_run(
            "UPDATE world_media_playback SET item_id='',state='stopped',"
            "position_ms=0,started_at=0,changed_at=?,changed_by_bi=?,"
            "revision=revision+1,change_id=? "
            "WHERE space_id=? AND item_id=?",
            now, account_bi, runtime.new_id(), space_id, resource_id,
        )
        await runtime.audit(
            actor,
            "world.media.item.remove",
            "media_item",
            resource_id,
        )
        return _response(runtime, {"ok": True, "removed": resource_id})

    if resource == "playback":
        if method == "GET":
            playback = await runtime.d1_first(
                "SELECT item_id,state,position_ms,started_at,changed_at,"
                "revision FROM world_media_playback WHERE space_id=?",
                space_id,
            )
            return _response(runtime, {
                "ok": True,
                "spaceId": space_id,
                "playback": policy.media_playback_public(playback, now),
            })
        if method not in ("POST", "PATCH"):
            return _response(
                runtime, {"error": "method_not_allowed"},
                status=405, allow="GET, POST, PATCH")
        if viewer_role not in policy.MEDIA_ROLES:
            return await _deny_media(runtime, actor, "playback", space_id)
        normalized, error = policy.normalize_media_playback(data)
        if error:
            return _response(runtime, {"error": error}, status=400)
        if normalized["itemId"]:
            item = await runtime.d1_first(
                "SELECT item_id FROM world_media_items "
                "WHERE item_id=? AND space_id=? AND status<>'removed'",
                normalized["itemId"], space_id,
            )
            if not item:
                return _response(
                    runtime, {"error": "item_not_found"}, status=404)
        await runtime.d1_run(
            "INSERT OR IGNORE INTO world_media_playback "
            "(space_id,item_id,state,position_ms,started_at,changed_at,"
            "changed_by_bi,revision,change_id) "
            "VALUES (?,'','idle',0,0,?,'',0,'')",
            space_id, now,
        )
        current = await runtime.d1_first(
            "SELECT revision FROM world_media_playback WHERE space_id=?",
            space_id,
        )
        current_revision = int((current or {}).get("revision") or 0)
        if current_revision != normalized["expectedRevision"]:
            playback = await runtime.d1_first(
                "SELECT item_id,state,position_ms,started_at,changed_at,"
                "revision FROM world_media_playback WHERE space_id=?",
                space_id,
            )
            return _response(runtime, {
                "error": "playback_conflict",
                "playback": policy.media_playback_public(playback, now),
            }, status=409)
        change_id = runtime.new_id()
        started_at = now if normalized["state"] == "playing" else 0
        await runtime.d1_run(
            "UPDATE world_media_playback SET item_id=?,state=?,position_ms=?,"
            "started_at=?,changed_at=?,changed_by_bi=?,revision=revision+1,"
            "change_id=? WHERE space_id=? AND revision=?",
            normalized["itemId"],
            normalized["state"],
            normalized["positionMs"],
            started_at,
            now,
            account_bi,
            change_id,
            space_id,
            normalized["expectedRevision"],
        )
        updated = await runtime.d1_first(
            "SELECT item_id,state,position_ms,started_at,changed_at,revision,"
            "change_id FROM world_media_playback WHERE space_id=?",
            space_id,
        )
        if not updated or str(updated.get("change_id") or "") != change_id:
            return _response(runtime, {
                "error": "playback_conflict",
                "playback": policy.media_playback_public(updated, now),
            }, status=409)
        await runtime.d1_run(
            "UPDATE world_media_spaces SET playback_state=?,updated_at=?,"
            "last_activity_at=? WHERE space_id=?",
            (
                "stopped"
                if normalized["state"] == "stopped"
                else "idle"
            ),
            now, now, space_id,
        )
        await runtime.audit(
            actor,
            "world.media.playback.update",
            "media_space",
            space_id,
            details={
                "state": normalized["state"],
                "hasItem": bool(normalized["itemId"]),
                "revision": int(updated.get("revision") or 0),
            },
        )
        return _response(runtime, {
            "ok": True,
            "spaceId": space_id,
            "playback": policy.media_playback_public(updated, now),
        })

    if resource == "stop":
        if method != "POST":
            return _response(
                runtime, {"error": "method_not_allowed"},
                status=405, allow="POST")
        if viewer_role not in policy.MEDIA_ROLES:
            return await _deny_media(runtime, actor, "stop", space_id)
        item_id = str(data.get("itemId") or "").strip().lower()
        if item_id:
            if not policy.valid_resource_id(item_id):
                return _response(
                    runtime, {"error": "invalid_item_id"}, status=400)
            item = await runtime.d1_first(
                "SELECT item_id FROM world_media_items "
                "WHERE item_id=? AND space_id=? AND status='queued'",
                item_id, space_id,
            )
            if not item:
                return _response(
                    runtime, {"error": "not_found"}, status=404)
            await runtime.d1_run(
                "UPDATE world_media_items SET status='stopped',updated_at=? "
                "WHERE item_id=? AND space_id=?",
                now, item_id, space_id,
            )
        else:
            await runtime.d1_run(
                "UPDATE world_media_items SET status='stopped',updated_at=? "
                "WHERE space_id=? AND status='queued'",
                now, space_id,
            )
        await runtime.d1_run(
            "UPDATE world_media_spaces SET playback_state='stopped',"
            "updated_at=?,last_activity_at=? WHERE space_id=?",
            now, now, space_id,
        )
        await runtime.d1_run(
            "INSERT OR IGNORE INTO world_media_playback "
            "(space_id,item_id,state,position_ms,started_at,changed_at,"
            "changed_by_bi,revision,change_id) "
            "VALUES (?,'','idle',0,0,?,'',0,'')",
            space_id, now,
        )
        await runtime.d1_run(
            "UPDATE world_media_playback SET item_id='',state='stopped',"
            "position_ms=0,started_at=0,changed_at=?,changed_by_bi=?,"
            "revision=revision+1,change_id=? WHERE space_id=?",
            now, account_bi, runtime.new_id(), space_id,
        )
        await runtime.audit(
            actor,
            "world.media.stop",
            "media_space",
            space_id,
            details={"singleItem": bool(item_id)},
        )
        return _response(runtime, {
            "ok": True, "spaceId": space_id, "playbackState": "stopped"})

    if resource == "schedules":
        if method != "POST":
            return _response(
                runtime, {"error": "method_not_allowed"},
                status=405, allow="POST")
        if viewer_role not in policy.MEDIA_ROLES:
            return await _deny_media(
                runtime, actor, "schedule.create", space_id)
        normalized, error = policy.normalize_media_schedule(data, now)
        if error:
            return _response(runtime, {"error": error}, status=400)
        if normalized["itemId"]:
            item = await runtime.d1_first(
                "SELECT item_id FROM world_media_items "
                "WHERE item_id=? AND space_id=? AND status<>'removed'",
                normalized["itemId"], space_id,
            )
            if not item:
                return _response(
                    runtime, {"error": "item_not_found"}, status=404)
        count = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM world_media_schedules "
            "WHERE space_id=? AND status='scheduled'",
            space_id,
        )
        if int((count or {}).get("n") or 0) >= (
                policy.MAX_MEDIA_SCHEDULES_PER_SPACE):
            return _response(runtime, {"error": "schedule_full"}, status=409)
        schedule_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO world_media_schedules "
            "(schedule_id,space_id,item_id,status,starts_at,ends_at,data,"
            "created_by_bi,created_at,updated_at,cancelled_at) "
            "VALUES (?,?,?,'scheduled',?,?,?,?,?,?,0)",
            schedule_id,
            space_id,
            normalized["itemId"],
            normalized["startsAt"],
            normalized["endsAt"],
            json.dumps(normalized, sort_keys=True, separators=(",", ":")),
            account_bi,
            now,
            now,
        )
        await runtime.d1_run(
            "UPDATE world_media_spaces SET updated_at=?,last_activity_at=? "
            "WHERE space_id=?",
            now, now, space_id,
        )
        await runtime.audit(
            actor,
            "world.media.schedule.create",
            "media_schedule",
            schedule_id,
            details={"sessionType": normalized["sessionType"]},
        )
        return _response(runtime, {
            "ok": True,
            "schedule": {
                **normalized,
                "id": schedule_id,
                "status": "scheduled",
            },
        }, status=201)

    if resource == "schedule":
        if method != "DELETE":
            return _response(
                runtime, {"error": "method_not_allowed"},
                status=405, allow="DELETE")
        if viewer_role not in policy.MEDIA_ROLES:
            return await _deny_media(
                runtime, actor, "schedule.cancel", space_id)
        schedule = await runtime.d1_first(
            "SELECT schedule_id FROM world_media_schedules "
            "WHERE schedule_id=? AND space_id=? AND status='scheduled'",
            resource_id, space_id,
        )
        if not schedule:
            return _response(runtime, {"error": "not_found"}, status=404)
        await runtime.d1_run(
            "UPDATE world_media_schedules SET status='cancelled',"
            "cancelled_at=?,updated_at=? WHERE schedule_id=? AND space_id=?",
            now, now, resource_id, space_id,
        )
        await runtime.audit(
            actor,
            "world.media.schedule.cancel",
            "media_schedule",
            resource_id,
        )
        return _response(runtime, {"ok": True, "cancelled": resource_id})

    if resource == "roles":
        if viewer_role != "owner":
            return await _deny_media(
                runtime, actor, "role.manage", space_id, True)
        if method == "GET":
            rows = await runtime.d1_all(
                "SELECT role_id,account_label,role,created_at "
                "FROM world_media_roles WHERE space_id=? "
                "ORDER BY created_at ASC LIMIT ?",
                space_id, policy.MAX_MEDIA_MODERATORS_PER_SPACE,
            )
            return _response(runtime, {
                "ok": True,
                "roles": [
                    role for role in (
                        policy.media_role_public(row) for row in rows or [])
                    if role is not None
                ],
            })
        if method != "POST":
            return _response(
                runtime, {"error": "method_not_allowed"},
                status=405, allow="GET, POST")
        target_bi, target_name = await runtime.account(
            data.get("account"))
        if not target_name:
            return _response(
                runtime, {"error": "account_not_found"}, status=404)
        if target_bi == account_bi:
            return _response(
                runtime, {"error": "owner_role_is_implicit"}, status=409)
        count = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM world_media_roles WHERE space_id=?",
            space_id,
        )
        if int((count or {}).get("n") or 0) >= (
                policy.MAX_MEDIA_MODERATORS_PER_SPACE):
            return _response(runtime, {"error": "role_limit"}, status=409)
        existing = await runtime.d1_first(
            "SELECT role_id FROM world_media_roles "
            "WHERE space_id=? AND account_bi=?",
            space_id, target_bi,
        )
        if existing:
            return _response(
                runtime, {"error": "role_exists"}, status=409)
        role_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO world_media_roles "
            "(role_id,space_id,account_bi,account_label,role,granted_by_bi,"
            "created_at) VALUES (?,?,?,?,'moderator',?,?)",
            role_id,
            space_id,
            target_bi,
            target_name,
            account_bi,
            now,
        )
        await runtime.audit(
            actor,
            "world.media.role.grant",
            "media_role",
            role_id,
            details={"role": "moderator"},
        )
        return _response(runtime, {
            "ok": True,
            "role": {
                "id": role_id,
                "account": target_name,
                "role": "moderator",
                "grantedAt": now,
            },
        }, status=201)

    if resource == "role":
        if method != "DELETE":
            return _response(
                runtime, {"error": "method_not_allowed"},
                status=405, allow="DELETE")
        if viewer_role != "owner":
            return await _deny_media(
                runtime, actor, "role.revoke", space_id, True)
        role = await runtime.d1_first(
            "SELECT role_id FROM world_media_roles "
            "WHERE role_id=? AND space_id=?",
            resource_id, space_id,
        )
        if not role:
            return _response(runtime, {"error": "not_found"}, status=404)
        await runtime.d1_run(
            "DELETE FROM world_media_roles WHERE role_id=? AND space_id=?",
            resource_id, space_id,
        )
        await runtime.audit(
            actor,
            "world.media.role.revoke",
            "media_role",
            resource_id,
        )
        return _response(runtime, {"ok": True, "revoked": resource_id})

    return _response(runtime, {"error": "not_found"}, status=404)


async def cleanup_media_records(runtime):
    """Apply the documented bounded-retention policy.

    This is called by the Worker's hourly privacy cleanup slot. Active spaces
    first become archived after six months without activity and remain
    recoverable for another 90 days; removed/stopped playlist entries and
    completed/cancelled schedules are retained for 30 days.
    """
    await runtime.ensure_schema()
    now = runtime.now()
    removed_cutoff = now - policy.MEDIA_REMOVED_RETENTION_MS
    schedule_cutoff = now - policy.MEDIA_SCHEDULE_RETENTION_MS
    archived_cutoff = now - policy.MEDIA_ARCHIVED_RETENTION_MS
    active_cutoff = now - policy.MEDIA_ACTIVE_RETENTION_MS
    await runtime.d1_run(
        "UPDATE world_media_schedules SET status='completed',updated_at=? "
        "WHERE status='scheduled' AND ends_at<?",
        now, now,
    )
    await runtime.d1_run(
        "DELETE FROM world_media_items WHERE "
        "(status='removed' AND removed_at>0 AND removed_at<?) OR "
        "(status='stopped' AND updated_at<?)",
        removed_cutoff, removed_cutoff,
    )
    await runtime.d1_run(
        "DELETE FROM world_media_schedules WHERE "
        "(status='cancelled' AND cancelled_at>0 AND cancelled_at<?) OR "
        "(status='completed' AND ends_at<?)",
        schedule_cutoff, schedule_cutoff,
    )
    await runtime.d1_run(
        "UPDATE world_media_spaces SET status='archived',"
        "playback_state='stopped',archived_at=?,updated_at=? "
        "WHERE status='active' AND last_activity_at<?",
        now, now, active_cutoff,
    )
    # Delete children before the archived parent; this remains correct even if
    # a D1 deployment does not enable SQLite foreign-key cascades.
    for table in (
            "world_media_roles", "world_media_items",
            "world_media_playback",
            "world_media_schedules"):
        await runtime.d1_run(
            "DELETE FROM " + table + " WHERE space_id IN ("
            "SELECT space_id FROM world_media_spaces "
            "WHERE status='archived' AND archived_at>0 AND archived_at<?)",
            archived_cutoff,
        )
    await runtime.d1_run(
        "DELETE FROM world_media_spaces WHERE status='archived' "
        "AND archived_at>0 AND archived_at<?",
        archived_cutoff,
    )
