// Lists every use of a MainWindow member in one translation unit, so the
// class can be split along the files that actually use each member.
//
//   mw_refs <source.cpp> <clang args...>
//
// Output, one tab-separated record per line:
//   M <usr> <name> <kind> <access> <virtual> <static> <line> <extent>
//                                  member declared in MainWindow
//   D <usr> <name> <file> <line> <col> <extent>
//                                  MainWindow method defined here (line:col of the name)
//   R <usr> <name> <file> <line> <col> <in-method-usr> <implicit-this> <owner-usr>
//                                  a use of a class member: every use of a
//                                  MainWindow member, and inside MainWindow
//                                  methods also uses of inherited members
//   T <file> <line> <col> <in-method-usr>
//                                  a `this` expression inside a MainWindow method
// <extent> is startLine:startCol-endLine:endCol.
// Uses in system headers are skipped. A header's uses repeat once per TU that
// includes it; merge on (file, line, col).
#include <clang-c/Index.h>

#include <stdio.h>
#include <string.h>

static const char *kClassUsr = "c:@S@MainWindow";

static int inMainWindow(CXCursor c)
{
    CXCursor parent = clang_getCursorSemanticParent(c);
    if (clang_Cursor_isNull(parent))
        return 0;
    CXString usr = clang_getCursorUSR(parent);
    const int yes = strcmp(clang_getCString(usr), kClassUsr) == 0;
    clang_disposeString(usr);
    return yes;
}

static void where(CXCursor c, CXString *file, unsigned *line, unsigned *col)
{
    CXFile f;
    clang_getSpellingLocation(clang_getCursorLocation(c), &f, line, col, NULL);
    *file = clang_getFileName(f);
}

static void extent(CXCursor c, char *out, size_t n)
{
    CXSourceRange r = clang_getCursorExtent(c);
    unsigned sl, sc, el, ec;
    clang_getSpellingLocation(clang_getRangeStart(r), NULL, &sl, &sc, NULL);
    clang_getSpellingLocation(clang_getRangeEnd(r), NULL, &el, &ec, NULL);
    snprintf(out, n, "%u:%u-%u:%u", sl, sc, el, ec);
}

static int isClassMember(CXCursor c)
{
    CXCursor parent = clang_getCursorSemanticParent(c);
    const enum CXCursorKind k = clang_getCursorKind(parent);
    return k == CXCursor_ClassDecl || k == CXCursor_StructDecl ||
           k == CXCursor_ClassTemplate ||
           k == CXCursor_ClassTemplatePartialSpecialization;
}

static const char *kindName(enum CXCursorKind k)
{
    switch (k) {
    case CXCursor_FieldDecl: return "field";
    case CXCursor_CXXMethod: return "method";
    case CXCursor_Constructor: return "ctor";
    case CXCursor_Destructor: return "dtor";
    case CXCursor_VarDecl: return "static-field";
    case CXCursor_FunctionTemplate: return "method-template";
    case CXCursor_StructDecl:
    case CXCursor_ClassDecl: return "type";
    case CXCursor_EnumDecl: return "enum";
    case CXCursor_TypedefDecl:
    case CXCursor_TypeAliasDecl: return "alias";
    default: return "other";
    }
}

static const char *accessName(enum CX_CXXAccessSpecifier a)
{
    switch (a) {
    case CX_CXXPublic: return "public";
    case CX_CXXProtected: return "protected";
    case CX_CXXPrivate: return "private";
    default: return "?";
    }
}

typedef struct {
    CXCursor method; // enclosing MainWindow method definition, or null
} Context;

static enum CXChildVisitResult visitClass(CXCursor c, CXCursor parent, CXClientData data)
{
    (void)parent;
    (void)data;
    const enum CXCursorKind k = clang_getCursorKind(c);
    if (k == CXCursor_CXXAccessSpecifier || k == CXCursor_CXXBaseSpecifier ||
        k == CXCursor_FriendDecl || k == CXCursor_StaticAssert)
        return CXChildVisit_Continue;
    CXString usr = clang_getCursorUSR(c);
    CXString name = clang_getCursorSpelling(c);
    CXString file;
    unsigned line, col;
    where(c, &file, &line, &col);
    char span[64];
    extent(c, span, sizeof span);
    printf("M\t%s\t%s\t%s\t%s\t%d\t%d\t%u\t%s\n", clang_getCString(usr),
           clang_getCString(name), kindName(k),
           accessName(clang_getCXXAccessSpecifier(c)),
           clang_CXXMethod_isVirtual(c), clang_CXXMethod_isStatic(c), line,
           span);
    clang_disposeString(usr);
    clang_disposeString(name);
    clang_disposeString(file);
    return CXChildVisit_Continue;
}

static int membersPrinted = 0;

static enum CXChildVisitResult visit(CXCursor c, CXCursor parent, CXClientData data)
{
    (void)parent;
    Context *ctx = (Context *)data;
    CXSourceLocation loc = clang_getCursorLocation(c);
    if (clang_Location_isInSystemHeader(loc))
        return CXChildVisit_Continue;
    const enum CXCursorKind k = clang_getCursorKind(c);

    if (!membersPrinted && (k == CXCursor_ClassDecl) &&
        clang_isCursorDefinition(c)) {
        CXString usr = clang_getCursorUSR(c);
        const int isClass = strcmp(clang_getCString(usr), kClassUsr) == 0;
        clang_disposeString(usr);
        if (isClass) {
            membersPrinted = 1;
            clang_visitChildren(c, visitClass, NULL);
        }
    }

    if ((k == CXCursor_CXXMethod || k == CXCursor_Constructor ||
         k == CXCursor_Destructor || k == CXCursor_FunctionTemplate) &&
        clang_isCursorDefinition(c) && inMainWindow(c)) {
        CXString usr = clang_getCursorUSR(c);
        CXString name = clang_getCursorSpelling(c);
        CXString file;
        unsigned line, col;
        where(c, &file, &line, &col);
        char span[64];
        extent(c, span, sizeof span);
        printf("D\t%s\t%s\t%s\t%u\t%u\t%s\n", clang_getCString(usr),
               clang_getCString(name), clang_getCString(file), line, col, span);
        clang_disposeString(usr);
        clang_disposeString(name);
        clang_disposeString(file);
        Context inner = {c};
        clang_visitChildren(c, visit, &inner);
        return CXChildVisit_Continue;
    }

    if (k == CXCursor_MemberRefExpr || k == CXCursor_DeclRefExpr ||
        k == CXCursor_MemberRef || k == CXCursor_OverloadedDeclRef) {
        CXCursor target = clang_getCursorReferenced(c);
        if (!clang_Cursor_isNull(target) &&
            (inMainWindow(target) ||
             (!clang_Cursor_isNull(ctx->method) && isClassMember(target)))) {
            CXString ownerUsr =
                clang_getCursorUSR(clang_getCursorSemanticParent(target));
            CXString usr = clang_getCursorUSR(target);
            CXString name = clang_getCursorSpelling(target);
            CXString file;
            unsigned line, col;
            where(c, &file, &line, &col);
            // An implicit `this` has no base expression: the first child of a
            // MemberRefExpr is the object expression when one was written.
            int implicitThis = 0;
            if (k == CXCursor_MemberRefExpr) {
                CXSourceRange range = clang_getCursorExtent(c);
                CXSourceLocation start = clang_getRangeStart(range);
                unsigned sl, sc;
                clang_getSpellingLocation(start, NULL, &sl, &sc, NULL);
                implicitThis = (sl == line && sc == col);
            }
            CXString methodUsr = clang_Cursor_isNull(ctx->method)
                                     ? clang_getCursorUSR(clang_getNullCursor())
                                     : clang_getCursorUSR(ctx->method);
            printf("R\t%s\t%s\t%s\t%u\t%u\t%s\t%d\t%s\n",
                   clang_getCString(usr), clang_getCString(name),
                   clang_getCString(file), line, col,
                   clang_getCString(methodUsr), implicitThis,
                   clang_getCString(ownerUsr));
            clang_disposeString(ownerUsr);
            clang_disposeString(usr);
            clang_disposeString(name);
            clang_disposeString(file);
            clang_disposeString(methodUsr);
        }
    }
    if (k == CXCursor_CXXThisExpr && !clang_Cursor_isNull(ctx->method)) {
        CXString file;
        unsigned line, col;
        where(c, &file, &line, &col);
        CXString methodUsr = clang_getCursorUSR(ctx->method);
        printf("T\t%s\t%u\t%u\t%s\n", clang_getCString(file), line, col,
               clang_getCString(methodUsr));
        clang_disposeString(file);
        clang_disposeString(methodUsr);
    }
    return CXChildVisit_Recurse;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <source> <clang args...>\n", argv[0]);
        return 2;
    }
    CXIndex index = clang_createIndex(0, 1);
    CXTranslationUnit tu = NULL;
    const enum CXErrorCode rc = clang_parseTranslationUnit2(
        index, argv[1], (const char *const *)(argv + 2), argc - 2, NULL, 0,
        CXTranslationUnit_KeepGoing | CXTranslationUnit_SkipFunctionBodies * 0,
        &tu);
    if (rc != CXError_Success || !tu) {
        fprintf(stderr, "parse failed (%d): %s\n", rc, argv[1]);
        return 1;
    }
    unsigned errors = 0;
    for (unsigned i = 0, n = clang_getNumDiagnostics(tu); i < n; ++i) {
        CXDiagnostic d = clang_getDiagnostic(tu, i);
        if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error) {
            if (errors < 5) {
                CXString s = clang_formatDiagnostic(
                    d, clang_defaultDiagnosticDisplayOptions());
                fprintf(stderr, "%s\n", clang_getCString(s));
                clang_disposeString(s);
            }
            ++errors;
        }
        clang_disposeDiagnostic(d);
    }
    Context ctx = {clang_getNullCursor()};
    clang_visitChildren(clang_getTranslationUnitCursor(tu), visit, &ctx);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return errors ? 3 : 0;
}
