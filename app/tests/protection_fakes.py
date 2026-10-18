from types import SimpleNamespace


async def _none(*_args, **_kwargs):
    return None


async def _false(*_args, **_kwargs):
    return False


async def _true(*_args, **_kwargs):
    return True


async def _no_refs(*_args, **_kwargs):
    return [], None


UNPROTECTED = SimpleNamespace(
    CONFIG_KEYS=("protectDefaultBranch", "actions"),
    push_protection=_no_refs,
    branch_refusal=_none,
    merge_refusal=_none,
    is_protected=_false,
    actions_off=_false,
    may_configure=_true,
    settings_only=_false,
    record_review=_false,
    branch_is_protected=lambda _branch, _refs: False,
)
