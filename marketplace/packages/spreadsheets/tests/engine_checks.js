const fs = require("fs");
const path = require("path");
const S = require(path.join(__dirname, "..", "content", "web", "page.js"));

let failures = 0, checks = 0;
function same(actual, expected, what) {
  checks += 1;
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) { failures += 1; console.error(`FAIL ${what}: expected ${e}, got ${a}`); }
}
function near(actual, expected, what, tolerance = 1e-9) {
  checks += 1;
  if (typeof actual !== "number" || Math.abs(actual - expected) > tolerance) { failures += 1; console.error(`FAIL ${what}: expected ${expected}, got ${JSON.stringify(actual)}`); }
}

function book(rows, name = "Sheet1") {
  const wb = S.createWorkbook();
  const sheet = wb.sheets[0];
  sheet.name = name;
  rows.forEach((row, r) => row.forEach((text, c) => {
    if (text === null || text === undefined) return;
    const parsed = S.parseInput(String(text));
    const s = parsed.format ? S.internStyle(wb, { fmt: parsed.format }) : 0;
    const cell = parsed.formula !== undefined ? S.makeCell(undefined, parsed.formula, s) : S.makeCell(parsed.value, undefined, s);
    if (cell) sheet.cells.set(S.cellKey(r, c), cell);
  }));
  return wb;
}
function value(wb, ref, sheetIndex = 0) {
  const at = S.parseAddr(ref);
  const v = wb.calc.cellValue(wb.sheets[sheetIndex], at.r, at.c);
  return S.isErr(v) ? v.code : v;
}
function calc(wb) { wb.calc = new S.Calc(wb); wb.calc.recalc(); return wb; }
function evalFormula(text, rows = [[]]) {
  const wb = calc(book(rows));
  const v = wb.calc.evaluate(text, wb.sheets[0], 0, 0);
  if (Array.isArray(v)) return v.map((row) => row.map((x) => (S.isErr(x) ? x.code : x)));
  return S.isErr(v) ? v.code : v;
}

const fmt = (v, code) => S.formatValue(v, code).text;
same(fmt(1234.567, "#,##0.00"), "1,234.57", "thousands with two decimals");
same(fmt(-1234.5, '"$"#,##0.00_);[Red]("$"#,##0.00)'), "($1,234.50)", "accounting negative");
same(S.formatValue(-5, "0;[Red]-0").color, "#FF0000", "red negative colour");
same(fmt(0.256, "0.0%"), "25.6%", "percent");
same(fmt(12345.678, "0.00E+00"), "1.23E+04", "scientific");
same(fmt(5551234567, "(000) 000-0000"), "(555) 123-4567", "phone format");
same(fmt(0.5, "# ?/?"), " 1/2", "fraction without whole part");
same(fmt(1.25, "# ?/?"), "1 1/4", "fraction with whole part");
same(fmt(46327, "yyyy-mm-dd"), "2026-11-01", "ISO date");
same(fmt(46327, "dddd, mmmm d, yyyy"), "Sunday, November 1, 2026", "long date");
same(fmt(46327.75, "h:mm AM/PM"), "6:00 PM", "12-hour time");
same(fmt(1.5, "[h]:mm:ss"), "36:00:00", "elapsed hours");
same(fmt(46327.5, "m/d/yyyy h:mm"), "11/1/2026 12:00", "date time with minutes");
same(fmt(0.1 + 0.2, "General"), "0.3", "general rounds float noise");
same(fmt(123456789012, "General"), "1.23457E+11", "general switches to exponent");
same(fmt("abc", '0;-0;0;"<"@">"'), "<abc>", "text section");
same(fmt(1234567, "#,##0,"), "1,235", "thousands scaling");
same(fmt(0, "0.00;-0.00;\"zero\""), "zero", "zero section");

same(S.parseInput("$1,234.50"), { value: 1234.5, format: '"$"#,##0.00' }, "currency input");
same(S.parseInput("12%"), { value: 0.12, format: "0%" }, "percent input");
same(S.parseInput("2026-11-01").value, 46327, "ISO date input");
same(S.parseInput("11/1/2026").value, 46327, "US date input");
same(S.parseInput("1/11/2026", { dayFirst: true }).value, 46327, "day-first date input");
same(S.parseInput("14:30").format, "h:mm", "time input");
same(S.parseInput("'007"), { value: "007", text: true }, "forced text");
same(S.parseInput("=A1+1"), { formula: "A1+1" }, "formula input");
same(S.parseInput("TRUE"), { value: true }, "boolean input");
same(S.parseInput("(42)").value, -42, "parenthesised negative");

same(evalFormula("1+2*3^2"), 19, "operator precedence");
same(evalFormula("-2^2"), 4, "negation binds tighter than power");
same(evalFormula('"a"&1&TRUE'), "a1TRUE", "concatenation");
same(evalFormula("1/0"), "#DIV/0!", "division by zero");
same(evalFormula("SUM(A1:A3)", [["1"], ["2"], ["x"]]), 3, "SUM skips text");
same(evalFormula("AVERAGE(A1:A3)", [["1"], ["2"], ["6"]]), 3, "AVERAGE");
same(evalFormula("COUNTIF(A1:A5,\">2\")", [["1"], ["2"], ["3"], ["4"], ["x"]]), 2, "COUNTIF comparison");
same(evalFormula("COUNTIF(A1:A4,\"a*\")", [["apple"], ["Avocado"], ["pear"], ["5"]]), 2, "COUNTIF wildcard");
same(evalFormula("SUMIF(A1:A4,\"x\",B1:B4)", [["x", "1"], ["y", "2"], ["x", "3"], ["z", "4"]]), 4, "SUMIF");
same(evalFormula("SUMIFS(C1:C4,A1:A4,\"x\",B1:B4,\">1\")", [["x", "1", "10"], ["x", "2", "20"], ["y", "3", "30"], ["x", "3", "40"]]), 60, "SUMIFS");
same(evalFormula("VLOOKUP(\"b\",A1:B3,2,FALSE)", [["a", "1"], ["b", "2"], ["c", "3"]]), 2, "VLOOKUP exact");
same(evalFormula("VLOOKUP(25,A1:B3,2)", [["10", "low"], ["20", "mid"], ["30", "high"]]), "mid", "VLOOKUP approximate");
same(evalFormula("INDEX(B1:B3,MATCH(\"c\",A1:A3,0))", [["a", "1"], ["b", "2"], ["c", "3"]]), 3, "INDEX/MATCH");
same(evalFormula("XLOOKUP(\"z\",A1:A3,B1:B3,\"none\")", [["a", "1"], ["b", "2"], ["c", "3"]]), "none", "XLOOKUP not found");
same(evalFormula("XLOOKUP(\"b\",A1:A3,B1:B3)", [["a", "1"], ["b", "2"], ["c", "3"]]), 2, "XLOOKUP found");
same(evalFormula("IFERROR(1/0,\"oops\")"), "oops", "IFERROR");
same(evalFormula("IF(A1>1,\"big\",\"small\")", [["5"]]), "big", "IF");
same(evalFormula("IFS(A1>10,\"a\",A1>1,\"b\",TRUE,\"c\")", [["5"]]), "b", "IFS");
same(evalFormula("SWITCH(2,1,\"one\",2,\"two\",\"other\")"), "two", "SWITCH");
same(evalFormula("LET(x,2,y,3,x*y)"), 6, "LET");
same(evalFormula("TEXT(0.5,\"0%\")"), "50%", "TEXT");
same(evalFormula("PROPER(\"hello wORLD\")"), "Hello World", "PROPER");
same(evalFormula("SUBSTITUTE(\"a-b-c\",\"-\",\"+\",2)"), "a-b+c", "SUBSTITUTE instance");
same(evalFormula("TEXTJOIN(\", \",TRUE,A1:A3)", [["a"], [null], ["c"]]), "a, c", "TEXTJOIN skips empty");
same(evalFormula("LEFT(\"spreadsheet\",6)&RIGHT(\"abc\")&MID(\"abcdef\",2,3)"), "spreadcbcd", "LEFT RIGHT MID");
same(evalFormula("SEARCH(\"s?e\",\"Spreadsheet\")"), 7, "SEARCH wildcard");
same(evalFormula("DATE(2026,11,1)"), 46327, "DATE");
same(evalFormula("EOMONTH(DATE(2026,1,31),1)"), S.serialFromDate(2026, 2, 28), "EOMONTH");
same(evalFormula("EDATE(DATE(2026,1,31),1)"), S.serialFromDate(2026, 2, 28), "EDATE clamps");
same(evalFormula("DATEDIF(DATE(2020,5,15),DATE(2026,11,1),\"Y\")"), 6, "DATEDIF years");
same(evalFormula("NETWORKDAYS(DATE(2026,11,2),DATE(2026,11,13))"), 10, "NETWORKDAYS");
same(evalFormula("WEEKDAY(DATE(2026,11,1))"), 1, "WEEKDAY Sunday");
same(evalFormula("ISOWEEKNUM(DATE(2026,1,1))"), 1, "ISOWEEKNUM");
near(evalFormula("PMT(0.05/12,360,200000)"), -1073.6432460242797, "PMT", 1e-6);
near(evalFormula("NPV(0.1,-10000,3000,4200,6800)"), 1188.4434123352207, "NPV", 1e-6);
near(evalFormula("IRR(A1:A4)", [["-100"], ["40"], ["50"], ["30"]]), 0.1013310, "IRR", 1e-6);
near(evalFormula("RATE(48,-200,8000)"), 0.007701472488, "RATE", 1e-8);
near(evalFormula("STDEV(A1:A4)", [["2"], ["4"], ["4"], ["6"]]), 1.632993161855452, "STDEV");
near(evalFormula("NORM.S.DIST(1,TRUE)"), 0.841344746, "NORM.S.DIST", 1e-6);
near(evalFormula("NORM.S.INV(0.975)"), 1.959963985, "NORM.S.INV", 1e-6);
same(evalFormula("MEDIAN(1,3,2,4)"), 2.5, "MEDIAN");
same(evalFormula("ROUND(2.345,2)&\"/\"&ROUNDUP(2.341,2)&\"/\"&ROUNDDOWN(-2.349,2)"), "2.35/2.35/-2.34", "rounding");
same(evalFormula("MOD(-3,2)"), 1, "MOD sign follows divisor");
same(evalFormula("SUMPRODUCT(A1:A3,B1:B3)", [["1", "4"], ["2", "5"], ["3", "6"]]), 32, "SUMPRODUCT");
same(evalFormula("SUM(A1:A3*B1:B3)", [["1", "4"], ["2", "5"], ["3", "6"]]), 32, "array arithmetic inside SUM");
same(evalFormula("SEQUENCE(2,3)"), [[1, 2, 3], [4, 5, 6]], "SEQUENCE");
same(evalFormula("SORT(A1:A3,1,-1)", [["2"], ["9"], ["4"]]), [[9], [4], [2]], "SORT descending");
same(evalFormula("UNIQUE(A1:A4)", [["a"], ["b"], ["A"], ["c"]]), [["a"], ["b"], ["c"]], "UNIQUE ignores case");
same(evalFormula("FILTER(A1:A4,B1:B4>1)", [["a", "1"], ["b", "2"], ["c", "3"], ["d", "0"]]), [["b"], ["c"]], "FILTER");
same(evalFormula("TRANSPOSE(A1:B1)", [["1", "2"]]), [[1], [2]], "TRANSPOSE");
same(evalFormula("SUBTOTAL(9,A1:A3)", [["1"], ["2"], ["3"]]), 6, "SUBTOTAL");
same(evalFormula("NOSUCHFUNCTION(1)"), "#NAME?", "unknown function");
same(evalFormula("INDIRECT(\"A2\")", [["1"], ["7"]]), 7, "INDIRECT");
same(evalFormula("SUM(OFFSET(A1,1,0,2,1))", [["1"], ["2"], ["3"]]), 5, "OFFSET");
same(evalFormula("ADDRESS(2,3)"), "$C$2", "ADDRESS");
same(evalFormula("COLUMNS(A1:C9)*ROWS(A1:C9)"), 27, "ROWS COLUMNS");
same(evalFormula("SUM(A:A)", [["1"], ["2"], ["3"]]), 6, "whole column");

{
  const wb = calc(book([["=SEQUENCE(3)", null], [null, null], [null, null], ["=SUM(A1:A3)", "=A2*10"]]));
  same([value(wb, "A1"), value(wb, "A2"), value(wb, "A3"), value(wb, "A4"), value(wb, "B4")], [1, 2, 3, 6, 20], "spilled values are readable by other formulas");
  wb.sheets[0].cells.set(S.cellKey(1, 0), S.makeCell("block"));
  wb.calc.recalc();
  same(value(wb, "A1"), "#SPILL!", "a filled cell blocks the spill");
}
{
  const wb = calc(book([["=B1"], [null]].map((row, i) => (i === 0 ? ["=B1", "=A1"] : row))));
  same(value(wb, "A1"), "#REF!", "circular reference");
}
{
  const wb = book([["1", "=A1*2"], ["2", "=A2*2"]]);
  wb.sheets.push(S.createSheet("Data"));
  wb.sheets[1].cells.set(S.cellKey(0, 0), S.makeCell(undefined, "SUM(Sheet1!B1:B2)"));
  calc(wb);
  same(value(wb, "A1", 1), 6, "cross-sheet reference");
  const tx = new S.Transaction(wb, "insert");
  S.insertRows(tx, wb, wb.sheets[0], 0, 2);
  same(wb.sheets[1].cells.get(S.cellKey(0, 0)).f, "SUM(Sheet1!B3:B4)", "inserting rows moves references on other sheets");
  same(wb.sheets[0].cells.get(S.cellKey(2, 1)).f, "A3*2", "inserting rows moves the formula and its references");
  const entry = tx.finish();
  S.applyHistory(wb, entry, "undo");
  same(wb.sheets[1].cells.get(S.cellKey(0, 0)).f, "SUM(Sheet1!B1:B2)", "undo restores references");
  same(wb.sheets[0].cells.get(S.cellKey(0, 1)).f, "A1*2", "undo restores moved cells");
  S.applyHistory(wb, entry, "redo");
  same(wb.sheets[0].cells.get(S.cellKey(2, 1)).f, "A3*2", "redo replays");
  const tx2 = new S.Transaction(wb, "delete");
  S.deleteRows(tx2, wb, wb.sheets[0], 2, 1);
  same(wb.sheets[1].cells.get(S.cellKey(0, 0)).f, "SUM(Sheet1!B3:B3)", "deleting a row shrinks ranges");
  same(wb.sheets[0].cells.get(S.cellKey(2, 1)).f, "A3*2", "a formula below a deleted row moves up with its references");
  const tx3 = new S.Transaction(wb, "rename");
  S.renameSheet(tx3, wb, wb.sheets[0], "My Data");
  same(wb.sheets[1].cells.get(S.cellKey(0, 0)).f, "SUM('My Data'!B3:B3)", "renaming a sheet rewrites references");
}
{
  const wb = book([["1"], ["=A1+1"]]);
  const tx = new S.Transaction(wb, "delete");
  S.deleteRows(tx, wb, wb.sheets[0], 0, 1);
  same(wb.sheets[0].cells.get(S.cellKey(0, 0)).f, "#REF!+1", "deleting a referenced row gives #REF!");
}
same(S.shiftFormula("A1+$B$2+B$3+$C4+SUM(D1:D2)", 2, 1), "B3+$B$2+C$3+$C6+SUM(E3:E4)", "relative shift");
same(S.shiftFormula("A1", -1, 0), "#REF!", "shift off the sheet");
same(S.toggleAbsolute("A1"), "$A$1", "F4 cycle 1");
same(S.toggleAbsolute("$A$1"), "A$1", "F4 cycle 2");
{
  const wb = S.createWorkbook();
  const sources = [S.makeCell(1), S.makeCell(3)];
  same(S.fillValues(sources, 3, wb).map((x) => x.cell.v), [5, 7, 9], "linear fill");
  same(S.fillValues([S.makeCell("Item 9")], 2, wb).map((x) => x.cell.v), ["Item 10", "Item 11"], "text with number fill");
  same(S.fillValues([S.makeCell("Mon")], 3, wb).map((x) => x.cell.v), ["Tue", "Wed", "Thu"], "weekday fill");
  same(S.fillValues([S.makeCell("January")], 2, wb).map((x) => x.cell.v), ["February", "March"], "month fill");
}
{
  const wb = calc(book([["Name", "Score"], ["b", "2"], ["a", "3"], ["c", "1"]]));
  const tx = new S.Transaction(wb, "sort");
  S.sortRows(tx, wb, wb.calc, wb.sheets[0], { r1: 1, c1: 0, r2: 3, c2: 1 }, [{ col: 1, desc: true }]);
  wb.calc.recalc();
  same([value(wb, "A2"), value(wb, "A3"), value(wb, "A4")], ["a", "b", "c"], "sort descending by score");
}
{
  const wb = calc(book([["Region", "Product", "Sales"], ["East", "A", "10"], ["West", "A", "5"], ["East", "B", "7"], ["East", "A", "3"]]));
  const out = S.pivotTable(wb.calc, wb.sheets[0], { r1: 0, c1: 0, r2: 4, c2: 2 }, { rowField: 0, colField: 1, valueField: 2, aggregate: "sum" });
  same(out, [["Region", "A", "B", "Grand total"], ["East", 13, 7, 20], ["West", 5, null, 5], ["Grand total", 18, 7, 25]], "pivot table");
}
{
  const wb = calc(book([["5"], ["50"], ["500"]]));
  wb.sheets[0].conditional = [{ id: "x", range: "A1:A3", type: "cell", op: "gt", value: "10", style: { fill: "#FF0000" } }];
  const cf = S.conditionalContext(wb.calc, wb, wb.sheets[0]);
  same([cf(0, 0), cf(1, 0)], [null, { fill: "#FF0000" }], "conditional formatting rule");
  wb.sheets[0].conditional = [{ id: "y", range: "A1:A3", type: "scale", colors: ["#000000", "#FFFFFF"] }];
  same(S.conditionalContext(wb.calc, wb, wb.sheets[0])(2, 0), { fill: "#FFFFFF" }, "colour scale top");
}

async function roundTrips() {
  const wb = book([["Item", "Qty", "Price", "Total"], ["Pen", "3", "$1.50", "=B2*C2"], ["Book", "2", "$12.00", "=B3*C3"], ["Sum", null, null, "=SUM(D2:D3)"], ["When", "2026-11-01", "TRUE", "=IF(D4>20,\"big\",\"small\")"]]);
  const sheet = wb.sheets[0];
  sheet.cells.set(S.cellKey(0, 0), S.makeCell("Item", undefined, S.internStyle(wb, { b: true, fill: "#FFF2CC", color: "#C00000", h: "center", bb: { w: "medium", c: "#000000" } })));
  sheet.merges.push({ r1: 6, c1: 0, r2: 6, c2: 2 });
  sheet.cells.set(S.cellKey(6, 0), S.makeCell("Merged title", undefined, S.internStyle(wb, { i: true, size: 14, font: "Georgia", wrap: true, v: "middle" })));
  sheet.colWidths.set(0, 140);
  sheet.rowHeights.set(6, 40);
  sheet.freezeRows = 1;
  sheet.color = "#4472C4";
  sheet.notes.set(S.cellKey(1, 0), "A note");
  sheet.links.set(S.cellKey(2, 0), "https://forkmesh.com/");
  sheet.validations.push({ id: "v", range: "E2:E5", type: "list", values: ["Yes", "No"], dropdown: true, strict: true });
  sheet.conditional.push({ id: "c", range: "D2:D3", type: "cell", op: "gt", value: "10", style: { fill: "#F4CCCC" } });
  const second = S.createSheet("Second sheet");
  second.cells.set(S.cellKey(0, 0), S.makeCell(undefined, "'Sheet1'!D4*2"));
  second.cells.set(S.cellKey(1, 0), S.makeCell("Ünïcödé ✓"));
  wb.sheets.push(second);
  calc(wb);
  same(value(wb, "D4"), 28.5, "source totals");

  const check = (copy, label, options = {}) => {
    calc(copy);
    same(copy.sheets.map((s) => s.name), ["Sheet1", "Second sheet"], `${label}: sheet names`);
    same([value(copy, "D2"), value(copy, "D4"), value(copy, "D5"), value(copy, "A1", 1), value(copy, "A2", 1)], [4.5, 28.5, "big", 57, "Ünïcödé ✓"], `${label}: values`);
    same(copy.sheets[0].cells.get(S.cellKey(3, 3)).f, "SUM(D2:D3)", `${label}: formula text`);
    const head = S.styleOf(copy, copy.sheets[0].cells.get(S.cellKey(0, 0)));
    same([head.b, head.fill, head.color, head.h], [true, "#FFF2CC", "#C00000", "center"], `${label}: header style`);
    if (!options.noBorders) same(head.bb && head.bb.w, "medium", `${label}: border`);
    same(S.formatValue(-1234.5, S.styleOf(copy, copy.sheets[0].cells.get(S.cellKey(1, 2))).fmt).text, "-$1,234.50", `${label}: currency format`);
    same(copy.sheets[0].merges.map(S.rangeText), ["A7:C7"], `${label}: merges`);
    same(copy.sheets[0].colWidths.get(0), 140, `${label}: column width`);
    if (!options.noFreeze) same(copy.sheets[0].freezeRows, 1, `${label}: frozen row`);
    if (!options.noNotes) same(copy.sheets[0].notes.get(S.cellKey(1, 0)), "A note", `${label}: note`);
    if (!options.noRich) {
      same(copy.sheets[0].color, "#4472C4", `${label}: tab colour`);
      same(copy.sheets[0].links.get(S.cellKey(2, 0)), "https://forkmesh.com/", `${label}: link`);
      same(copy.sheets[0].validations.map((v) => [v.range, v.type, v.values]), [["E2:E5", "list", ["Yes", "No"]]], `${label}: validation`);
      same(copy.sheets[0].conditional.map((v) => [v.range, v.type, v.op, v.style.fill]), [["D2:D3", "cell", "gt", "#F4CCCC"]], `${label}: conditional`);
    }
  };
  const xlsx = await S.writeXlsx(wb, wb.calc);
  check((await S.importWorkbook(xlsx, "book.xlsx")).wb, "xlsx");
  const xls = S.writeXls(wb, wb.calc);
  same((await S.importWorkbook(xls, "book.xls")).kind, "xls", "xls sniffed");
  check((await S.importWorkbook(xls, "book.xls")).wb, "xls", { noNotes: true, noRich: true });
  const ods = await S.writeOds(wb, wb.calc);
  same((await S.importWorkbook(ods, "book.ods")).kind, "ods", "ods sniffed");
  check((await S.importWorkbook(ods, "book.ods")).wb, "ods", { noFreeze: true, noRich: true });
  const packed = await S.packWorkbook(wb, wb.calc);
  check(await S.unpackWorkbook(packed), "packed json");

  const csv = S.sheetToCsv(wb, wb.sheets[0], wb.calc);
  same(csv.split("\r\n")[1], "Pen,3,$1.50,4.5", "csv uses shown values");
  const fromCsv = (await S.importWorkbook(new TextEncoder().encode("a;b\n1;\"x;y\"\n"), "data.csv")).wb;
  calc(fromCsv);
  same([value(fromCsv, "A2"), value(fromCsv, "B2")], [1, "x;y"], "csv semicolon detection");
  const md = S.sheetToMarkdown(wb, wb.sheets[0], wb.calc);
  same(md.split("\n")[1], "| --- | --- | --- | --- |", "markdown header rule");
  same(S.workbookJson(wb, wb.calc).sheets[0].rows[0].Item, "Pen", "json keyed rows");
  same(S.workbookHtml(wb, wb.calc, "t").includes("<td colspan=\"3\""), true, "html merges");

  const fixtures = path.join(__dirname, "fixtures");
  if (fs.existsSync(path.join(fixtures, "libreoffice.xls"))) {
    const real = (await S.importWorkbook(new Uint8Array(fs.readFileSync(path.join(fixtures, "libreoffice.xls"))), "libreoffice.xls")).wb;
    calc(real);
    check(real, "LibreOffice xls", { noNotes: true, noRich: true, noFreeze: true });
  }
  if (fs.existsSync(path.join(fixtures, "libreoffice.xlsx"))) {
    const real = (await S.importWorkbook(new Uint8Array(fs.readFileSync(path.join(fixtures, "libreoffice.xlsx"))), "libreoffice.xlsx")).wb;
    calc(real);
    check(real, "LibreOffice xlsx", { noRich: true, noFreeze: true });
  }

  const ai = book([["Month", "Sales"], ["Jan", "10"], ["Feb", "20"]]);
  calc(ai);
  const context = S.aiContext(ai, ai.calc, ai.sheets[0], { r1: 0, c1: 0, r2: 2, c2: 1 });
  same(context.includes("B3=20"), true, "AI context lists cells");
  const tx = new S.Transaction(ai, "ai");
  const result = S.applyAiActions(tx, ai, ai.calc, ai.sheets[0], [
    { type: "write", cell: "A4", rows: [["Total", "=SUM(B2:B3)"]] },
    { type: "format", range: "A1:B1", bold: true, fill: "lightblue" },
    { type: "chart", range: "A1:B3", chartType: "line", title: "Sales" },
    { type: "bogus" },
  ]);
  ai.calc.recalc();
  same(value(ai, "B4"), 30, "AI write with formula");
  same(S.styleOf(ai, ai.sheets[0].cells.get(S.cellKey(0, 1))).b, true, "AI format");
  same(ai.sheets[0].charts.length, 1, "AI chart");
  same(result.notes.length, 3, "AI notes skip unknown actions");
  const entry = tx.finish();
  S.applyHistory(ai, entry, "undo");
  same(ai.sheets[0].cells.has(S.cellKey(3, 0)) || ai.sheets[0].charts.length > 0, false, "AI changes undo as one step");
  return { xlsx, xls, ods };
}

roundTrips().then((files) => {
  if (process.argv[2]) {
    fs.writeFileSync(path.join(process.argv[2], "out.xlsx"), files.xlsx);
    fs.writeFileSync(path.join(process.argv[2], "out.xls"), files.xls);
    fs.writeFileSync(path.join(process.argv[2], "out.ods"), files.ods);
  }
  if (failures) { console.error(`${failures} of ${checks} checks failed`); process.exit(1); }
  console.log(`engine checks passed (${checks})`);
}).catch((error) => { console.error(error.stack || error); process.exit(1); });
