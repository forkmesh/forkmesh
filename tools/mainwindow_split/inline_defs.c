// Lists the namespace-scope function definitions in one header, with the
// byte ranges needed to move their bodies into a .cpp file.
//
//   inline_defs <header> <tu.cpp> <clang args...>
//
// Output, one tab-separated record per function defined in <header>:
//   F <name> <namespace> <start> <body-start> <end> <static> <inline> <flags> <defaults> <usr>
//   P <usr> <start> <end>    a separate declaration (no body) of a function
// Offsets are bytes into <header>; <end> is one past the closing brace.
// <flags> lists reasons the function must stay in the header (template,
// constexpr, auto, deleted), or "-". <defaults> is a comma-separated list of
// "eq:end" byte ranges covering each default argument (" = value"), or "-".
#include <clang-c/Index.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *gHeader;
static CXTranslationUnit gTu;

static int inHeader(CXCursor c)
{
    CXFile f;
    clang_getSpellingLocation(clang_getCursorLocation(c), &f, NULL, NULL, NULL);
    if (!f)
        return 0;
    CXString name = clang_getFileName(f);
    char *real = realpath(clang_getCString(name), NULL);
    const int yes = real && strcmp(real, gHeader) == 0;
    free(real);
    clang_disposeString(name);
    return yes;
}

static unsigned offsetOf(CXSourceLocation loc)
{
    unsigned off;
    clang_getSpellingLocation(loc, NULL, NULL, NULL, &off);
    return off;
}

static void namespacePath(CXCursor c, char *out, size_t n)
{
    char parts[16][128];
    int count = 0;
    for (CXCursor p = clang_getCursorSemanticParent(c);
         clang_getCursorKind(p) == CXCursor_Namespace && count < 16;
         p = clang_getCursorSemanticParent(p)) {
        CXString s = clang_getCursorSpelling(p);
        snprintf(parts[count++], sizeof parts[0], "%s", clang_getCString(s));
        clang_disposeString(s);
    }
    out[0] = 0;
    for (int i = count - 1; i >= 0; --i) {
        strncat(out, parts[i], n - strlen(out) - 1);
        if (i)
            strncat(out, "::", n - strlen(out) - 1);
    }
    if (!out[0])
        snprintf(out, n, "-");
}

typedef struct {
    CXCursor body;
    int found;
} BodyFind;

static enum CXChildVisitResult findBody(CXCursor c, CXCursor p, CXClientData d)
{
    (void)p;
    BodyFind *b = (BodyFind *)d;
    if (clang_getCursorKind(c) == CXCursor_CompoundStmt) {
        b->body = c;
        b->found = 1;
        return CXChildVisit_Break;
    }
    return CXChildVisit_Continue;
}

// Tokens between the declaration start and the body: the specifiers that
// decide whether the function can leave the header at all.
static void scanSpecifiers(CXSourceRange r, char *flags, size_t n)
{
    CXToken *tokens;
    unsigned count;
    clang_tokenize(gTu, r, &tokens, &count);
    flags[0] = 0;
    for (unsigned i = 0; i < count; ++i) {
        CXString s = clang_getTokenSpelling(gTu, tokens[i]);
        const char *t = clang_getCString(s);
        if (!strcmp(t, "template") || !strcmp(t, "constexpr") ||
            !strcmp(t, "consteval") || !strcmp(t, "delete") ||
            !strcmp(t, "auto")) {
            if (!strstr(flags, t)) {
                if (flags[0])
                    strncat(flags, ",", n - strlen(flags) - 1);
                strncat(flags, t, n - strlen(flags) - 1);
            }
        }
        clang_disposeString(s);
    }
    clang_disposeTokens(gTu, tokens, count);
    if (!flags[0])
        snprintf(flags, n, "-");
}

// " = value" of one parameter: from the '=' token to the parameter's end.
static void defaultRange(CXCursor parm, char *out, size_t n)
{
    CXSourceRange r = clang_getCursorExtent(parm);
    CXToken *tokens;
    unsigned count;
    clang_tokenize(gTu, r, &tokens, &count);
    int depth = 0;
    for (unsigned i = 0; i < count; ++i) {
        CXString s = clang_getTokenSpelling(gTu, tokens[i]);
        const char *t = clang_getCString(s);
        if (!strcmp(t, "(") || !strcmp(t, "<") || !strcmp(t, "[") || !strcmp(t, "{"))
            ++depth;
        else if (!strcmp(t, ")") || !strcmp(t, ">") || !strcmp(t, "]") || !strcmp(t, "}"))
            --depth;
        else if (!strcmp(t, "=") && depth == 0) {
            const unsigned eq = offsetOf(clang_getTokenLocation(gTu, tokens[i]));
            const unsigned end = offsetOf(clang_getRangeEnd(r));
            char one[64];
            snprintf(one, sizeof one, "%s%u:%u", out[0] ? "," : "", eq, end);
            strncat(out, one, n - strlen(out) - 1);
            clang_disposeString(s);
            break;
        }
        clang_disposeString(s);
    }
    clang_disposeTokens(gTu, tokens, count);
}

static enum CXChildVisitResult visit(CXCursor c, CXCursor parent, CXClientData d)
{
    (void)parent;
    (void)d;
    const enum CXCursorKind k = clang_getCursorKind(c);
    if (k == CXCursor_Namespace)
        return CXChildVisit_Recurse;
    if (!inHeader(c))
        return CXChildVisit_Continue;
    if (k != CXCursor_FunctionDecl && k != CXCursor_FunctionTemplate)
        return CXChildVisit_Continue;
    if (!clang_isCursorDefinition(c)) {
        // A separate `inline` declaration keeps a moved definition inline, so
        // the mover has to strip it too.
        CXSourceRange r = clang_getCursorExtent(c);
        CXString usr = clang_getCursorUSR(c);
        printf("P\t%s\t%u\t%u\n", clang_getCString(usr),
               offsetOf(clang_getRangeStart(r)), offsetOf(clang_getRangeEnd(r)));
        clang_disposeString(usr);
        return CXChildVisit_Continue;
    }
    BodyFind b = {clang_getNullCursor(), 0};
    clang_visitChildren(c, findBody, &b);
    if (!b.found)
        return CXChildVisit_Continue;

    CXSourceRange whole = clang_getCursorExtent(c);
    const unsigned start = offsetOf(clang_getRangeStart(whole));
    const unsigned end = offsetOf(clang_getRangeEnd(whole));
    const unsigned bodyStart =
        offsetOf(clang_getRangeStart(clang_getCursorExtent(b.body)));
    char flags[128];
    scanSpecifiers(clang_getRange(clang_getRangeStart(whole),
                                  clang_getRangeStart(clang_getCursorExtent(b.body))),
                   flags, sizeof flags);
    if (k == CXCursor_FunctionTemplate && !strstr(flags, "template")) {
        strncat(flags, strcmp(flags, "-") ? ",template" : "", sizeof flags - strlen(flags) - 1);
        if (!strcmp(flags, "-"))
            snprintf(flags, sizeof flags, "template");
    }
    if (clang_getCursorType(c).kind != CXType_Invalid &&
        clang_getResultType(clang_getCursorType(c)).kind == CXType_Auto &&
        !strstr(flags, "auto")) {
        strncat(flags, strcmp(flags, "-") ? ",auto" : "", sizeof flags - strlen(flags) - 1);
        if (!strcmp(flags, "-"))
            snprintf(flags, sizeof flags, "auto");
    }
    char defaults[1024] = "";
    const int nargs = clang_Cursor_getNumArguments(c);
    for (int i = 0; i < nargs; ++i)
        defaultRange(clang_Cursor_getArgument(c, (unsigned)i), defaults,
                     sizeof defaults);
    char ns[512];
    namespacePath(c, ns, sizeof ns);
    CXString name = clang_getCursorSpelling(c);
    CXString usr = clang_getCursorUSR(c);
    printf("F\t%s\t%s\t%u\t%u\t%u\t%d\t%d\t%s\t%s\t%s\n", clang_getCString(name), ns,
           start, bodyStart, end,
           clang_Cursor_getStorageClass(c) == CX_SC_Static,
           clang_Cursor_isFunctionInlined(c), flags,
           defaults[0] ? defaults : "-", clang_getCString(usr));
    clang_disposeString(name);
    clang_disposeString(usr);
    return CXChildVisit_Continue;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <header> <tu.cpp> <clang args...>\n", argv[0]);
        return 2;
    }
    gHeader = realpath(argv[1], NULL);
    if (!gHeader) {
        perror(argv[1]);
        return 2;
    }
    CXIndex index = clang_createIndex(0, 1);
    const enum CXErrorCode rc = clang_parseTranslationUnit2(
        index, argv[2], (const char *const *)(argv + 3), argc - 3, NULL, 0,
        CXTranslationUnit_KeepGoing, &gTu);
    if (rc != CXError_Success || !gTu) {
        fprintf(stderr, "parse failed (%d)\n", rc);
        return 1;
    }
    unsigned errors = 0;
    for (unsigned i = 0, n = clang_getNumDiagnostics(gTu); i < n; ++i) {
        CXDiagnostic dg = clang_getDiagnostic(gTu, i);
        if (clang_getDiagnosticSeverity(dg) >= CXDiagnostic_Error) {
            if (errors < 5) {
                CXString s = clang_formatDiagnostic(dg, clang_defaultDiagnosticDisplayOptions());
                fprintf(stderr, "%s\n", clang_getCString(s));
                clang_disposeString(s);
            }
            ++errors;
        }
        clang_disposeDiagnostic(dg);
    }
    clang_visitChildren(clang_getTranslationUnitCursor(gTu), visit, NULL);
    clang_disposeTranslationUnit(gTu);
    clang_disposeIndex(index);
    return errors ? 3 : 0;
}
