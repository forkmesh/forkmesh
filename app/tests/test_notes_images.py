import json
import subprocess

from test_notes_documents import NOTES, PUBLIC, _block, _source


def _images_source():
    notes = NOTES.read_text(encoding="utf-8")
    constants = notes[notes.index("  const NOTE_IMAGE_SETTINGS_LINE"):notes.index("  const NOTE_CHAT_SEEN_KEY")]
    functions = notes[notes.index("  function noteImageSettingsRead("):notes.index("  function noteImageFilter(")]
    start = notes.index("  function noteImageLineOf(")
    line_of = notes[start:_block(notes, "  function noteImageLineOf(")]
    start = notes.index("  function notePageGeometry(")
    geometry = notes[start:_block(notes, "  function notePageGeometry(")]
    return _source() + "\n" + constants + functions + line_of + "\n" + geometry


def _run(expression):
    script = _images_source() + "\nconsole.log(JSON.stringify(" + expression + "));\n"
    result = subprocess.run(["node", "--input-type=module"], input=script,
                            check=True, text=True, capture_output=True)
    return json.loads(result.stdout)


DEFAULTS = {"wrap": "inline", "align": "left", "width": 0, "height": 0, "rotate": 0,
            "margin": 3.2, "x": 0, "y": 0, "pages": "anchor", "recolor": "none",
            "transparency": 0, "brightness": 0, "contrast": 0}


def test_image_settings_follow_the_contract():
    assert _run('noteImageSettingsRead("")') == DEFAULTS
    read = _run('noteImageSettingsRead(" WRAP = Behind ;width=80.26; x=-5; pages=all; '
                'transparency=140; brightness=-300; recolor=SEPIA; size=huge; rotate=abc ")')
    assert read == {**DEFAULTS, "wrap": "behind", "width": 80.3, "x": -5, "pages": "all",
                    "transparency": 100, "brightness": -100, "recolor": "sepia"}
    assert _run('noteImageSettingsRead("wrap=floating; align=middle; pages=2")') == DEFAULTS


def test_writing_settings_keeps_only_what_differs_in_a_fixed_order():
    line = _run('noteImageSettingsLine({ ...NOTE_IMAGE_DEFAULTS, recolor: "grayscale", wrap: "behind", '
                'width: 210, height: 297, pages: "all" })')
    assert line == "<!-- image: wrap=behind; width=210; height=297; pages=all; recolor=grayscale -->"
    assert _run("noteImageSettingsLine(NOTE_IMAGE_DEFAULTS)") == ""


def test_settings_lines_are_added_replaced_and_removed_above_the_image():
    markdown = "Intro\n\n![a](a.png)\n\nEnd"
    added = _run('noteWithImageSettings(%s, 2, { ...NOTE_IMAGE_DEFAULTS, wrap: "wrap" })' % json.dumps(markdown))
    assert added == {"markdown": "Intro\n\n<!-- image: wrap=wrap -->\n![a](a.png)\n\nEnd", "line": 3}
    replaced = _run('noteWithImageSettings(%s, 3, { ...NOTE_IMAGE_DEFAULTS, wrap: "break", align: "center" })'
                    % json.dumps(added["markdown"]))
    assert replaced == {"markdown": "Intro\n\n<!-- image: wrap=break; align=center -->\n![a](a.png)\n\nEnd", "line": 3}
    removed = _run("noteWithImageSettings(%s, 3, NOTE_IMAGE_DEFAULTS)" % json.dumps(replaced["markdown"]))
    assert removed == {"markdown": markdown, "line": 2}


def test_settings_lines_are_hidden_and_mark_the_image_paragraph():
    html = _run('renderMarkdown("Intro\\n\\n<!-- image: wrap=wrap; width=40 -->\\n![a](https://x.test/a.png) beside\\n\\n'
                '```\\n<!-- image: wrap=front -->\\n```", { noteDocument: true })')
    assert 'data-note-image-options="wrap=wrap; width=40" data-note-line="2" hidden' in html
    assert '<p class="my-2 leading-6" data-note-line="3">' in html
    assert "&lt;!-- image: wrap=front --&gt;" in html
    assert "image: wrap=wrap" not in html.replace('data-note-image-options="wrap=wrap; width=40"', "")
    plain = _run('renderMarkdown("<!-- image: wrap=wrap -->\\n![a](https://x.test/a.png)", {})')
    assert "data-note-line" not in plain


def test_each_image_maps_to_the_line_it_starts():
    lines = ["![a](a.png) and ![b](b.png)", "![c](c.png)", "text ![d](d.png)", "", "![e](e.png)"]
    assert _run("[0, 1, 2, 3].map((index) => noteImageLineOf(%s, 0, index))" % json.dumps(lines)) == [0, -1, 1, -1]
    assert _run("noteImageLineOf(%s, 4, 0)" % json.dumps(lines)) == 4


def test_page_geometry_turns_landscape_and_subtracts_margins():
    geometry = _run('notePageGeometry({ size: "letter", orientation: "landscape", margins: "narrow" })')
    assert geometry == {"width": 279.4, "height": 215.9, "margin": 12.7,
                        "contentWidth": 279.4 - 25.4, "contentHeight": 215.9 - 25.4}


def test_published_page_hides_image_settings_lines():
    page = PUBLIC.read_text(encoding="utf-8")
    script = page[page.index("const esc="):page.index("(async()=>")]
    result = subprocess.run(
        ["node", "--input-type=module"],
        input=script + "\nconsole.log(JSON.stringify(md(%s)));" % json.dumps("<!-- image: wrap=behind -->\n![a](https://x.test/a.png)"),
        check=True, text=True, capture_output=True)
    html = json.loads(result.stdout)
    assert "wrap=behind" not in html and "x.test/a.png" in html
