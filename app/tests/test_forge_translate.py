import importlib.util
import io
import json
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("forge_translate", ROOT / "tools" / "forge_translate.py")
ft = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ft)

RU = "Свой VPN-сервис для конечных пользователей в России. Протокол работает, но мобильный интернет режут."
ZH_EN = ("这个协议上周开始被封锁了，我们正在寻找绕过的方法。 "
         "This protocol was blocked last week and we are looking for a way around it.")
EN = "The provider started blocking this protocol last week and we are looking for a way around it."


class FakeEngine:
    def __init__(self, pairs=("ru-en", "zh-en", "fa-en")):
        self._pairs = set(pairs)
        self.calls = []

    def pairs(self):
        return self._pairs

    def translate(self, source, target, sentences):
        self.calls.append((source, target, list(sentences)))
        return ["<%s:%s>" % (source, s) for s in sentences]


def write_issue(root, number, events, side="open", title="Thread"):
    folder = root / ".forkmesh" / "issues" / side / str(number)
    folder.mkdir(parents=True, exist_ok=True)
    (folder / ("issue-%d.json" % number)).write_text(json.dumps(
        {"number": number, "title": title, "events": events}), encoding="utf-8")


def opener(body, title="Thread"):
    return {"type": "open", "id": "forge-github-issue-1", "title": title, "body": body}


def comment(cid, body):
    return {"type": "comment", "id": cid, "body": body}


def shard(root, number):
    return json.loads((root / ft.DIR_REL / ("%d.json" % number)).read_text("utf-8"))


def test_languages_match_the_dashboard_rules():
    assert ft.guess(RU) == "ru"
    assert ft.guess("Це повідомлення про блокування. Їх провайдер змінює налаштування щодня.") == "uk"
    assert ft.guess("این یک گزارش درباره فیلترینگ پیام رسان های ایرانی است") == "fa"
    assert ft.guess(ZH_EN) == "zh"
    assert ft.guess(EN) == "en"
    assert ft.guess("curl -v https://example.com --resolve") == ""
    assert ft.needs(ZH_EN) == "zh" and ft.needs(EN) == "" and ft.needs(RU) == "ru"


def test_markdown_structure_code_and_links_are_kept():
    text = ("# Заголовок\n\nПривет, мир! Как дела?\n\n```\nкод не трогать\n```\n"
            "- пункт [ссылка](https://example.com/a) и `код`\n> цитата @bob #12\n    отступ кода")
    out = ft.translate_markdown(text, lambda batch: ["<%s>" % s for s in batch])
    assert out.split("\n") == [
        "# <Заголовок>", "", "<Привет, мир!> <Как дела?>", "", "```", "код не трогать", "```",
        "- <пункт> [<ссылка>](https://example.com/a) <и> `код`",
        "> <цитата> @bob #12", "    отступ кода"]


def test_a_pass_translates_new_posts_and_keeps_unchanged_ones(tmp_path):
    write_issue(tmp_path, 7, [opener(RU, title="Обход белых списков мобильного интернета в России"),
                              comment("c1", EN), comment("c2", "Протокол работает, но мобильный интернет режут."),
                              comment("c3", ZH_EN)])
    engine = FakeEngine()
    summary = ft.sync(tmp_path, engine)
    doc = shard(tmp_path, 7)
    assert doc["schema"] == ft.SCHEMA and doc["target"] == "en"
    assert sorted(doc["posts"]) == ["body", "c2", "c3", "title"]
    assert doc["posts"]["body"]["source"] == "ru" and doc["posts"]["body"]["hash"] == ft.digest(RU)
    assert doc["posts"]["body"]["text"].startswith("<ru:Свой VPN-сервис")
    assert summary["translated"] == 4
    again = FakeEngine()
    assert ft.sync(tmp_path, again)["kept"] == 4 and again.calls == []


def test_an_edit_is_translated_again_and_a_deleted_post_is_dropped(tmp_path):
    write_issue(tmp_path, 3, [opener(RU), comment("c1", RU)])
    ft.sync(tmp_path, FakeEngine())
    edited = "Теперь всё работает через другой протокол, спасибо всем за помощь."
    write_issue(tmp_path, 3, [opener(RU), comment("c1", RU),
                              {"type": "edit", "target": "forge-github-issue-1", "body": edited},
                              {"type": "delete", "target": "c1"}])
    engine = FakeEngine()
    ft.sync(tmp_path, engine)
    doc = shard(tmp_path, 3)
    assert sorted(doc["posts"]) == ["body", "title"] or sorted(doc["posts"]) == ["body"]
    assert doc["posts"]["body"]["hash"] == ft.digest(edited)
    assert any(edited.split(",")[0] in " ".join(call[2]) for call in engine.calls)


def test_missing_models_are_skipped_and_the_time_budget_is_kept(tmp_path):
    write_issue(tmp_path, 1, [opener("Це повідомлення про блокування. Їх провайдер змінює налаштування щодня.")])
    write_issue(tmp_path, 2, [opener(RU)])
    write_issue(tmp_path, 4, [opener(RU)])
    ticks = iter([0.0, 0.0, 999.0, 999.0, 999.0, 999.0, 999.0, 999.0])
    summary = ft.sync(tmp_path, FakeEngine(), seconds=10, clock=lambda: next(ticks, 999.0))
    assert summary["skipped"] == {"uk": 1}
    assert summary["translated"] == 1 and summary["pending"] >= 1
    assert (tmp_path / ft.DIR_REL / "4.json").is_file()
    assert not (tmp_path / ft.DIR_REL / "1.json").exists()


def test_model_packages_unpack_only_what_the_engine_needs(tmp_path):
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("translate-ru_en-1_9/model/model.bin", b"m")
        z.writestr("translate-ru_en-1_9/sentencepiece.model", b"s")
        z.writestr("translate-ru_en-1_9/stanza/ru/tokenize.pt", b"big")
        z.writestr("translate-ru_en-1_9/../../escape.txt", b"x")
    index = [{"from_code": "ru", "to_code": "en", "links": ["https://example.invalid/ru_en.argosmodel"]}]
    files = {ft.ARGOS_INDEX: json.dumps(index).encode(), index[0]["links"][0]: archive.getvalue()}
    done = ft.install_models(tmp_path / "models", ["ru", "xx"], fetch=files.__getitem__)
    assert done == ["ru-en"]
    pair = tmp_path / "models" / "ru-en"
    assert (pair / "model" / "model.bin").read_bytes() == b"m"
    assert (pair / "sentencepiece.model").is_file()
    assert not (pair / "stanza").exists() and not (tmp_path / "escape.txt").exists()
    assert ft.ArgosEngine(tmp_path / "models").pairs() == {"ru-en"}


def test_a_post_with_english_further_down_is_still_translated():
    long_zh = "这个协议上周开始被封锁了，我们正在寻找绕过的方法。" * 120
    assert ft.needs(long_zh) == "zh"
    assert ft.needs(long_zh + "\n\n## English\n\n" + EN * 3) == "zh"


def _package(size):
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("pkg/model/model.bin", b"x" * size)
        z.writestr("pkg/sentencepiece.model", b"s")
    return archive.getvalue()


def test_a_model_too_big_for_a_small_server_falls_back_to_the_older_one(tmp_path):
    link = "https://example.invalid/translate-ru_en-1_9.argosmodel"
    index = [{"from_code": "ru", "to_code": "en", "links": [link]}]
    files = {ft.ARGOS_INDEX: json.dumps(index).encode(), link: _package(3 * 2 ** 20),
             link.replace("1_9", "1_0"): _package(2 ** 20)}
    assert ft.install_models(tmp_path, ["ru"], fetch=files.__getitem__, max_mb=2) == ["ru-en"]
    assert (tmp_path / "ru-en" / "model" / "model.bin").stat().st_size == 2 ** 20
    assert sorted(p.name for p in tmp_path.iterdir()) == ["ru-en"]



def test_a_post_written_twice_translates_only_its_foreign_lines(tmp_path):
    body = ("Протокол работает, но мобильный интернет режут каждый вечер.\n\n"
            "*The protocol works, but the mobile internet is cut every evening.*\n\n"
            "```\ncurl -v https://example.com\n```\n\n"
            "Мы ищем способ обойти белые списки.")
    assert ft.needs(body) == "ru"
    english_first = EN + "\n\n" + "Если выложите здесь адрес, я проверю его с симок трёх операторов."
    assert ft.needs(english_first) == "ru"
    assert ft.needs(EN + "\n\nПривет") == ""
    write_issue(tmp_path, 9, [opener(body), comment("c1", english_first)])
    engine = FakeEngine()
    ft.sync(tmp_path, engine)
    doc = shard(tmp_path, 9)
    text = doc["posts"]["body"]["text"].split("\n")
    assert text[0].startswith("<ru:Протокол") and text[2] == "*The protocol works, but the mobile internet is cut every evening.*"
    assert text[4:7] == ["```", "curl -v https://example.com", "```"]
    assert text[-1].startswith("<ru:Мы ищем")
    lines = doc["posts"]["c1"]["text"].split("\n")
    assert lines[0] == EN and lines[-1].startswith("<ru:Если")
    sent = [sentence for call in engine.calls for sentence in call[2]]
    assert not any("protocol works" in sentence or "provider started" in sentence for sentence in sent)
