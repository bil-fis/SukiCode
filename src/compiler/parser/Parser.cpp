#include "compiler/parser/Parser.h"

#include <algorithm>

namespace suki {

// ─── construction & token helpers ────────────────────────────────────────────
Parser::Parser(std::vector<Token> tokens, DiagnosticEngine& diags)
    : tokens_(std::move(tokens)), diags_(diags) {}

const Token& Parser::cur() const {
    if (pos_ >= tokens_.size()) return tokens_.back();
    return tokens_[pos_];
}
const Token& Parser::peek(size_t off) const {
    size_t i = pos_ + off;
    if (i >= tokens_.size()) return tokens_.back();
    return tokens_[i];
}
bool Parser::atEnd() const {
    return pos_ >= tokens_.size() || tokens_[pos_].kind == TokenKind::TK_EOF;
}
bool Parser::check(TokenKind k) const { return cur().kind == k; }
bool Parser::checkPunct(PunctuatorID p) const {
    return cur().kind == TokenKind::TK_Punctuator && cur().punct == p;
}
bool Parser::checkKw(KeywordID k) const {
    return cur().kind == TokenKind::TK_Keyword && cur().keyword == k;
}
bool Parser::match(TokenKind k) {
    if (check(k)) { advance(); return true; }
    return false;
}
bool Parser::matchPunct(PunctuatorID p) {
    if (checkPunct(p)) { advance(); return true; }
    return false;
}
bool Parser::matchKw(KeywordID k) {
    if (checkKw(k)) { advance(); return true; }
    return false;
}
Token Parser::advance() {
    Token t = cur();
    if (!atEnd()) ++pos_;
    return t;
}
Token Parser::expect(TokenKind k, const char* msg) {
    if (check(k)) return advance();
    errorAt(cur(), msg);
    return cur();
}
Token Parser::expectPunct(PunctuatorID p, const char* msg) {
    if (checkPunct(p)) return advance();
    errorAt(cur(), msg);
    return cur();
}
Token Parser::expectKw(KeywordID k, const char* msg) {
    if (checkKw(k)) return advance();
    errorAt(cur(), msg);
    return cur();
}
void Parser::errorAt(const Token& t, const std::string& msg) {
    SourceRange r{t.loc, t.loc};
    diags_.reportError(msg, r);
}
void Parser::synchronize() {
    advance();
    while (!atEnd()) {
        if (checkPunct(PunctuatorID::RBrace)) return;
        switch (cur().kind) {
            case TokenKind::TK_Keyword:
                switch (cur().keyword) {
                    case KeywordID::Func: case KeywordID::Struct:
                    case KeywordID::Enum: case KeywordID::Class:
                    case KeywordID::Protocol: case KeywordID::Extension:
                    case KeywordID::Let: case KeywordID::Var:
                    case KeywordID::Typealias: case KeywordID::Init:
                    case KeywordID::Deinit: case KeywordID::Subscript:
                    case KeywordID::Import: case KeywordID::Module:
                    case KeywordID::Return: case KeywordID::If:
                    case KeywordID::Guard: case KeywordID::While:
                    case KeywordID::For: case KeywordID::Switch:
                    case KeywordID::Do: case KeywordID::Throw:
                        return;
                    default: break;
                }
                break;
            default: break;
        }
        advance();
    }
}

// ─── module ───────────────────────────────────────────────────────────────────
NodeList Parser::parseModule() {
    NodeList decls;
    // optional module declaration
    if (checkKw(KeywordID::Module)) {
        auto m = std::make_unique<ModuleDecl>();
        advance();
        if (check(TokenKind::TK_Identifier)) { m->name = cur().text; advance(); }
        if (checkPunct(PunctuatorID::Semicolon)) advance();
        decls.push_back(std::move(m));
    }
    while (!atEnd()) {
        // import
        if (checkKw(KeywordID::Import)) {
            auto imp = std::make_unique<ImportDecl>();
            advance();
            if (check(TokenKind::TK_Identifier)) { imp->moduleName = cur().text; advance(); }
            if (checkPunct(PunctuatorID::Semicolon)) advance();
            decls.push_back(std::move(imp));
            continue;
        }
        // top-level statements (e.g. do/catch blocks, control-flow, throw)
        if (checkKw(KeywordID::Do) || checkKw(KeywordID::Return) ||
            checkKw(KeywordID::If) || checkKw(KeywordID::Guard) ||
            checkKw(KeywordID::While) || checkKw(KeywordID::Repeat) ||
            checkKw(KeywordID::For) || checkKw(KeywordID::Switch) ||
            checkKw(KeywordID::Break) || checkKw(KeywordID::Continue) ||
            checkKw(KeywordID::Defer) || checkKw(KeywordID::Throw) ||
            checkKw(KeywordID::Unsafe) || checkKw(KeywordID::Select)) {
            NodePtr s = parseStatement();
            if (s) decls.push_back(std::move(s));
            else synchronize();
            continue;
        }
        NodePtr d = parseDecl();
        if (d) decls.push_back(std::move(d));
        else synchronize();
    }
    return decls;
}

// ─── declarations ──────────────────────────────────────────────────────────────
NodePtr Parser::parseDecl() {
    std::vector<std::string> attrs;
    while (checkPunct(PunctuatorID::At)) {
        advance();
        if (check(TokenKind::TK_Identifier)) attrs.push_back(cur().text);
        advance();
        if (checkPunct(PunctuatorID::LParen)) {
            // @attr(...) — skip contents
            int depth = 0;
            do {
                if (checkPunct(PunctuatorID::LParen)) ++depth;
                else if (checkPunct(PunctuatorID::RParen)) --depth;
                advance();
            } while (!atEnd() && depth > 0);
        }
    }

    auto attach = [&](NodePtr n) {
        if (n) {
            n->attributes = attrs;
            if (!attrs.empty() && !cur().docComment.empty()) {
                // doc comment belongs to the previous token; attach via attribute handling
            }
        }
        return n;
    };

    std::vector<std::string> modifiers = parseModifiers();

    if (checkKw(KeywordID::Func)) return attach(parseFunctionDecl(modifiers));
    if (checkKw(KeywordID::Init)) return attach(parseInitializerDecl(modifiers));
    if (checkKw(KeywordID::Deinit)) return attach(parseDeinitializerDecl());
    if (checkKw(KeywordID::Subscript)) return attach(parseSubscriptDecl(modifiers));
    if (checkKw(KeywordID::Struct))   return attach(parseTypeDecl(NodeKind::StructDecl, modifiers));
    if (checkKw(KeywordID::Enum))     return attach(parseTypeDecl(NodeKind::EnumDecl, modifiers));
    if (checkKw(KeywordID::Class))    return attach(parseTypeDecl(NodeKind::ClassDecl, modifiers));
    if (checkKw(KeywordID::Actor))    return attach(parseTypeDecl(NodeKind::ActorDecl, modifiers));
    if (checkKw(KeywordID::Protocol)) return attach(parseTypeDecl(NodeKind::ProtocolDecl, modifiers));
    if (checkKw(KeywordID::Extension))return attach(parseTypeDecl(NodeKind::ExtensionDecl, modifiers));
    if (checkKw(KeywordID::Typealias))return attach(parseTypealiasDecl());
    if (checkKw(KeywordID::Associatedtype)) return attach(parseAssociatedTypeDecl());
    if (checkKw(KeywordID::Let) || checkKw(KeywordID::Var)) {
        bool isLet = checkKw(KeywordID::Let);
        return attach(parseVarDecl(isLet, modifiers, /*member=*/false));
    }

    errorAt(cur(), "unexpected token at top level");
    advance();
    return nullptr;
}

std::vector<std::string> Parser::parseModifiers() {
    std::vector<std::string> mods;
    static const KeywordID modKw[] = {
        KeywordID::Public, KeywordID::Private, KeywordID::Internal,
        KeywordID::Fileprivate, KeywordID::Open, KeywordID::Static,
        KeywordID::Final, KeywordID::Override, KeywordID::Required,
        KeywordID::Convenience, KeywordID::Lazy, KeywordID::Mutating,
        KeywordID::Nonmutating, KeywordID::Async, KeywordID::Foreign,
        KeywordID::Extern, KeywordID::Weak, KeywordID::Inout,
    };
    bool again = true;
    while (again) {
        again = false;
        for (KeywordID k : modKw) {
            if (checkKw(k)) { mods.push_back(keywordToString(k)); advance(); again = true; break; }
        }
    }
    return mods;
}

NodePtr Parser::parseFunctionDecl(std::vector<std::string> modifiers) {
    auto fn = std::make_unique<FunctionDecl>();
    fn->modifiers = modifiers;
    advance(); // consume func
    // `get`/`set` are contextual keywords (property accessors). A function may
    // legitimately be named `get`, so accept the keyword when a parameter
    // list follows.
    if (check(TokenKind::TK_Identifier) ||
        ((checkKw(KeywordID::Get) || checkKw(KeywordID::Set)) &&
         peek(1).kind == TokenKind::TK_Punctuator && peek(1).punct == PunctuatorID::LParen)) {
        fn->name = cur().text;
        advance();
    }
    if (checkPunct(PunctuatorID::Less)) fn->genericParams = parseGenericParamNames();
    fn->params = parseParameterList();
    // `async` / `throws` / `rethrows` and `-> RetType` may appear in any order
    while (true) {
        if (matchPunct(PunctuatorID::Arrow)) {
            fn->returnType = parseType();
        } else if (matchKw(KeywordID::Throws) || matchKw(KeywordID::Rethrows)) {
            fn->isThrows = true;
        } else if (matchKw(KeywordID::Async)) {
            fn->isAsync = true;
        } else {
            break;
        }
    }
    if (std::find(modifiers.begin(), modifiers.end(), "async") != modifiers.end()) fn->isAsync = true;
    if (std::find(modifiers.begin(), modifiers.end(), "mutating") != modifiers.end()) fn->isMutating = true;
    if (std::find(modifiers.begin(), modifiers.end(), "foreign") != modifiers.end() ||
        std::find(modifiers.begin(), modifiers.end(), "extern") != modifiers.end()) {
        fn->isForeign = true;
    }
    if (fn->isForeign) {
        if (checkPunct(PunctuatorID::Semicolon)) advance();
        return fn;
    }
    if (checkPunct(PunctuatorID::LBrace)) {
        fn->body = parseBlockStatements();
    } else if (checkPunct(PunctuatorID::Semicolon)) {
        advance();
    }
    // else: declaration only (protocol method / foreign) — body stays empty
    return fn;
}

std::vector<std::string> Parser::parseGenericParamNames() {
    std::vector<std::string> names;
    expectPunct(PunctuatorID::Less, "expected '<'");
    while (!atEnd() && !checkPunct(PunctuatorID::Greater)) {
        if (check(TokenKind::TK_Identifier)) { names.push_back(cur().text); advance(); }
        else break;
        if (matchPunct(PunctuatorID::Colon)) { parseType(); } // discard constraint
        if (!matchPunct(PunctuatorID::Comma)) break;
    }
    expectPunct(PunctuatorID::Greater, "expected '>'");
    return names;
}

std::vector<Param> Parser::parseParameterList() {
    std::vector<Param> params;
    expectPunct(PunctuatorID::LParen, "expected '('");
    if (checkPunct(PunctuatorID::RParen)) { advance(); return params; }
    while (!atEnd()) {
        Param p;
        if (matchKw(KeywordID::Inout)) p.isInout = true;
        if (check(TokenKind::TK_Identifier) && cur().text == "_") {
            advance(); p.externalName = "";
        } else if (check(TokenKind::TK_Identifier)) {
            p.externalName = cur().text; advance();
        } else { errorAt(cur(), "expected parameter name"); break; }
        if (check(TokenKind::TK_Identifier)) { p.internalName = cur().text; advance(); }
        else p.internalName = p.externalName;
        expectPunct(PunctuatorID::Colon, "expected ':' in parameter");
        p.type = parseType();
        if (matchPunct(PunctuatorID::DotDot)) p.isVariadic = true; // `T...`
        if (matchPunct(PunctuatorID::Equal)) p.defaultValue = parseExpr();
        params.push_back(std::move(p));
        if (!matchPunct(PunctuatorID::Comma)) break;
    }
    expectPunct(PunctuatorID::RParen, "expected ')'");
    return params;
}

NodePtr Parser::parseInitializerDecl(std::vector<std::string> modifiers) {
    auto init = std::make_unique<InitDecl>();
    init->modifiers = modifiers;
    advance(); // init
    if (std::find(modifiers.begin(), modifiers.end(), "convenience") != modifiers.end())
        init->isConvenience = true;
    if (std::find(modifiers.begin(), modifiers.end(), "required") != modifiers.end())
        init->isRequired = true;
    init->params = parseParameterList();
    init->body = parseBlockStatements();
    return init;
}

NodePtr Parser::parseDeinitializerDecl() {
    auto de = std::make_unique<DeinitDecl>();
    advance(); // deinit
    // A destructor takes no parameters, so the empty parens are optional:
    // accept both `deinit { ... }` and `deinit() { ... }`.
    if (checkPunct(PunctuatorID::LParen)) {
        advance();
        expectPunct(PunctuatorID::RParen, "expected ')'");
    }
    de->body = parseBlockStatements();
    return de;
}

NodePtr Parser::parseSubscriptDecl(std::vector<std::string> modifiers) {
    (void)modifiers;
    auto sub = std::make_unique<SubscriptDecl>();
    advance(); // subscript
    sub->params = parseParameterList();
    expectPunct(PunctuatorID::Arrow, "expected '->'");
    sub->elementType = parseType();
    expectPunct(PunctuatorID::LBrace, "expected '{'");
    // getter/setter
    while (!checkPunct(PunctuatorID::RBrace) && !atEnd()) {
        if (matchKw(KeywordID::Get)) {
            sub->getter = parseBlockStatements();
        } else if (matchKw(KeywordID::Set)) {
            sub->setter = parseBlockStatements();
        } else { errorAt(cur(), "expected getter or setter"); advance(); }
    }
    expectPunct(PunctuatorID::RBrace, "expected '}'");
    return sub;
}

NodePtr Parser::parseTypeDecl(NodeKind kind, std::vector<std::string> modifiers) {
    auto decl = std::make_unique<StructDecl>(); // placeholder; retyped below
    NodePtr node;
    switch (kind) {
        case NodeKind::StructDecl:    node = std::make_unique<StructDecl>(); break;
        case NodeKind::EnumDecl:      node = std::make_unique<EnumDecl>(); break;
        case NodeKind::ClassDecl:     node = std::make_unique<ClassDecl>(); break;
        case NodeKind::ActorDecl:     node = std::make_unique<ActorDecl>(); break;
        case NodeKind::ProtocolDecl:  node = std::make_unique<ProtocolDecl>(); break;
        case NodeKind::ExtensionDecl:  node = std::make_unique<ExtensionDecl>(); break;
        default: node = std::make_unique<StructDecl>(); break;
    }
    (void)decl;
    auto* td = static_cast<TypeDecl*>(node.get());
    td->modifiers = modifiers;
    advance(); // struct/enum/...
    if (check(TokenKind::TK_Identifier)) { td->name = cur().text; advance(); }
    if (checkPunct(PunctuatorID::Less)) td->genericParams = parseGenericParamNames();
    if (matchPunct(PunctuatorID::Colon)) td->inherited = parseInheritedTypes();
    expectPunct(PunctuatorID::LBrace, "expected '{'");
    while (!checkPunct(PunctuatorID::RBrace) && !atEnd()) {
        // member declarations
        std::vector<std::string> memberAttrs;
        while (checkPunct(PunctuatorID::At)) {
            advance();
            if (check(TokenKind::TK_Identifier)) memberAttrs.push_back(cur().text);
            advance();
        }
        std::vector<std::string> mmods = parseModifiers();
        if (checkKw(KeywordID::Let) || checkKw(KeywordID::Var)) {
            bool isLet = checkKw(KeywordID::Let);
            NodePtr v = parseVarDecl(isLet, mmods, /*member=*/true);
            if (v) {
                v->attributes = memberAttrs;
                td->members.push_back(std::move(v));
            }
        } else if (checkKw(KeywordID::Func)) {
            NodePtr f = parseFunctionDecl(mmods);
            if (f) { f->attributes = memberAttrs; td->members.push_back(std::move(f)); }
        } else if (checkKw(KeywordID::Init)) {
            NodePtr i = parseInitializerDecl(mmods);
            if (i) td->members.push_back(std::move(i));
        } else if (checkKw(KeywordID::Deinit)) {
            NodePtr d = parseDeinitializerDecl();
            if (d) td->members.push_back(std::move(d));
        } else if (checkKw(KeywordID::Subscript)) {
            NodePtr s = parseSubscriptDecl(mmods);
            if (s) td->members.push_back(std::move(s));
        } else if (checkKw(KeywordID::Typealias)) {
            NodePtr t = parseTypealiasDecl();
            if (t) td->members.push_back(std::move(t));
        } else if (checkKw(KeywordID::Associatedtype)) {
            NodePtr a = parseAssociatedTypeDecl();
            if (a) td->members.push_back(std::move(a));
        } else if (checkKw(KeywordID::Case)) {
            // enum case (only valid inside enum)
            NodePtr c = parseEnumCase();
            if (c) td->members.push_back(std::move(c));
        } else {
            errorAt(cur(), "unexpected member declaration");
            advance();
        }
    }
    expectPunct(PunctuatorID::RBrace, "expected '}'");
    return node;
}

NodePtr Parser::parseEnumCase() {
    auto c = std::make_unique<EnumCaseDecl>();
    advance(); // case
    if (check(TokenKind::TK_Identifier)) { c->name = cur().text; advance(); }
    if (matchPunct(PunctuatorID::LParen)) {
        c->hasAssociated = true;
        if (!checkPunct(PunctuatorID::RParen)) {
            while (!atEnd()) {
                // associated value: either `Type` or `label: Type`
                std::string label;
                if (check(TokenKind::TK_Identifier) && peek(1).kind == TokenKind::TK_Punctuator && peek(1).punct == PunctuatorID::Colon) {
                    label = cur().text; advance(); advance(); // consume ':'
                }
                NodePtr ty = parseType();
                c->associatedTypes.push_back(std::move(ty));
                (void)label;
                if (!matchPunct(PunctuatorID::Comma)) break;
            }
        }
        expectPunct(PunctuatorID::RParen, "expected ')'");
    }
    return c;
}

NodePtr Parser::parseTypealiasDecl() {
    auto t = std::make_unique<TypealiasDecl>();
    advance(); // typealias
    if (check(TokenKind::TK_Identifier)) { t->name = cur().text; advance(); }
    if (checkPunct(PunctuatorID::Less)) t->genericParams = parseGenericParamNames();
    expectPunct(PunctuatorID::Equal, "expected '='");
    t->underlying = parseType();
    if (checkPunct(PunctuatorID::Semicolon)) advance();
    return t;
}

NodePtr Parser::parseAssociatedTypeDecl() {
    auto a = std::make_unique<AssociatedTypeDecl>();
    advance(); // associatedtype
    if (check(TokenKind::TK_Identifier)) { a->name = cur().text; advance(); }
    if (matchPunct(PunctuatorID::Colon)) a->inherited = parseInheritedTypes();
    if (matchPunct(PunctuatorID::Equal)) a->defaultType = parseType();
    return a;
}

// Parse the body of a computed property:
//
//     var total: Int { self.x + self.y }        // implicit getter
//     var total: Int { get { ... } set { ... } }
//     var x: Int = 0 { willSet { ... } didSet { ... } }
//
// Explicit accessors are returned as AccessorDecl nodes so each keeps its kind;
// statements written directly in the braces form an implicit getter.
std::vector<NodePtr> Parser::parseAccessors() {
    std::vector<NodePtr> accessors;
    expectPunct(PunctuatorID::LBrace, "expected '{'");
    while (!checkPunct(PunctuatorID::RBrace) && !atEnd()) {
        AccessorDecl::Kind kind;
        bool explicitKind = true;
        if (checkKw(KeywordID::Get)) kind = AccessorDecl::Kind::Getter;
        else if (checkKw(KeywordID::Set)) kind = AccessorDecl::Kind::Setter;
        else if (checkKw(KeywordID::WillSet)) kind = AccessorDecl::Kind::WillSet;
        else if (checkKw(KeywordID::DidSet)) kind = AccessorDecl::Kind::DidSet;
        else { kind = AccessorDecl::Kind::Getter; explicitKind = false; }

        if (explicitKind) {
            advance(); // get / set / willSet / didSet
            auto acc = std::make_unique<AccessorDecl>();
            acc->kind = kind;
            // A setter (and the observers) may name their value parameter:
            // `set(newValue) { ... }`.
            if (checkPunct(PunctuatorID::LParen)) {
                advance();
                if (check(TokenKind::TK_Identifier)) {
                    acc->valueParam = cur().text;
                    advance();
                }
                expectPunct(PunctuatorID::RParen, "expected ')'");
            }
            if (checkPunct(PunctuatorID::LBrace))
                acc->body = parseBlockStatements();
            accessors.push_back(std::move(acc));
            continue;
        }
        // Statements written directly in the braces: an implicit getter.
        auto acc = std::make_unique<AccessorDecl>();
        acc->kind = AccessorDecl::Kind::Getter;
        NodePtr st = parseStatement();
        if (st) acc->body.push_back(std::move(st));
        else synchronize();
        accessors.push_back(std::move(acc));
    }
    expectPunct(PunctuatorID::RBrace, "expected '}'");
    return accessors;
}

NodePtr Parser::parseVarDecl(bool isLet, std::vector<std::string> modifiers, bool member) {
    auto first = std::make_unique<VarDecl>();
    first->isLet = isLet;
    first->modifiers = modifiers;
    if (std::find(modifiers.begin(), modifiers.end(), "weak") != modifiers.end())
        first->isWeak = true;
    // `unowned` is an ordinary identifier lexically (it is a common local name),
    // so it is recognised as a contextual modifier here.
    if (std::find(modifiers.begin(), modifiers.end(), "unowned") != modifiers.end())
        first->isUnowned = true;
    advance(); // let/var
    if (check(TokenKind::TK_Identifier)) { first->name = cur().text; advance(); }
    if (matchPunct(PunctuatorID::Colon)) first->type = parseType();
    if (matchPunct(PunctuatorID::Equal)) {
        // Suppress a trailing closure on the initialiser so that a `{`
        // immediately after it is recognised as an accessor block.
        ++suppressTrailingClosure_;
        first->initializer = parseExpr();
        --suppressTrailingClosure_;
    }
    // A `{` right after a *property declaration* is a computed-property body or
    // an observer block. In an ordinary local binding the brace belongs to the
    // enclosing statement, so only a member declaration takes it.
    if (member && checkPunct(PunctuatorID::LBrace))
        first->accessors = parseAccessors();
    if (checkPunct(PunctuatorID::Semicolon)) advance();

    if (!checkPunct(PunctuatorID::Comma)) return first;

    // multiple bindings: collect into a BlockStmt
    auto block = std::make_unique<BlockStmt>();
    block->statements.push_back(std::move(first));
    while (matchPunct(PunctuatorID::Comma)) {
        auto v = std::make_unique<VarDecl>();
        v->isLet = isLet;
        if (check(TokenKind::TK_Identifier)) { v->name = cur().text; advance(); }
        if (matchPunct(PunctuatorID::Colon)) v->type = parseType();
        if (matchPunct(PunctuatorID::Equal)) {
            // A literal initialiser can never take a trailing closure, so a `{`
            // right after it is an accessor block (`var x: Int = 0 { didSet { } }`).
            // Any other initialiser keeps the brace for the expression parser.
            // Only true literals qualify: an identifier may be a call taking a
            // trailing closure (`Task { ... }`), which must keep its brace.
            bool lit = check(TokenKind::TK_IntLiteral) ||
                       check(TokenKind::TK_FloatLiteral) ||
                       check(TokenKind::TK_StringLiteral) ||
                       check(TokenKind::TK_StringFragment) ||
                       check(TokenKind::TK_CharLiteral);
            if (lit) ++suppressTrailingClosure_;
            v->initializer = parseExpr();
            if (lit) --suppressTrailingClosure_;
        }
        // Only a property declaration takes a following brace as accessors.
        if (member && checkPunct(PunctuatorID::LBrace))
            v->accessors = parseAccessors();
        block->statements.push_back(std::move(v));
        if (checkPunct(PunctuatorID::Semicolon)) advance();
    }
    return block;
}

NodeList Parser::parseInheritedTypes() {
    NodeList list;
    while (!atEnd()) {
        list.push_back(parseType());
        if (!matchPunct(PunctuatorID::Comma)) break;
    }
    return list;
}

// ─── statements ───────────────────────────────────────────────────────────────
NodePtr Parser::parseStatement() {
    if (checkKw(KeywordID::Return)) return parseReturnStmt();
    if (checkKw(KeywordID::If)) return parseIfStmt();
    if (checkKw(KeywordID::Guard)) return parseGuardStmt();
    if (checkKw(KeywordID::While)) return parseWhileStmt();
    if (checkKw(KeywordID::Repeat)) return parseRepeatStmt();
    if (checkKw(KeywordID::For)) return parseForInStmt();
    if (checkKw(KeywordID::Switch)) return parseSwitchStmt();
    if (checkKw(KeywordID::Break)) { advance(); auto b=std::make_unique<BreakStmt>(); if(check(TokenKind::TK_Identifier)){b->label=cur().text;advance();} if(checkPunct(PunctuatorID::Semicolon))advance(); return b; }
    if (checkKw(KeywordID::Continue)) { advance(); auto c=std::make_unique<ContinueStmt>(); if(check(TokenKind::TK_Identifier)){c->label=cur().text;advance();} if(checkPunct(PunctuatorID::Semicolon))advance(); return c; }
    if (checkKw(KeywordID::Defer)) return parseDeferStmt();
    if (checkKw(KeywordID::Do)) return parseDoStmt();
    if (checkKw(KeywordID::Throw)) return parseThrowStmt();
    if (checkKw(KeywordID::Unsafe)) return parseUnsafeStmt();
    if (checkKw(KeywordID::Select)) return parseSelectStmt();
    if (checkKw(KeywordID::Let) || checkKw(KeywordID::Var)) {
        bool isLet = checkKw(KeywordID::Let);
        return parseVarDecl(isLet, /*modifiers=*/{}, /*member=*/false);
    }
    // expression statement
    auto e = std::make_unique<ExprStmt>();
    e->expr = parseExpr();
    if (checkPunct(PunctuatorID::Semicolon)) advance();
    return e;
}

NodePtr Parser::parseBlock() {
    auto b = std::make_unique<BlockStmt>();
    expectPunct(PunctuatorID::LBrace, "expected '{'");
    while (!checkPunct(PunctuatorID::RBrace) && !atEnd()) {
        // 兜底：任何一条语句都必须推进 token 流。若某个 token 让语句解析器
        // 无所消耗（例如后缀链缺少终止分支），这里会强制前进并报错，
        // 把"挂死"降级为"一条诊断 + 跳过该 token"。
        size_t before = pos_;
        NodePtr s = parseStatement();
        if (pos_ == before) {
            errorAt(cur(), "无法解析的 token，已跳过");
            advance();
            continue;
        }
        if (s) b->statements.push_back(std::move(s));
        else synchronize();
    }
    expectPunct(PunctuatorID::RBrace, "expected '}'");
    return b;
}

NodeList Parser::parseBlockStatements() {
    auto b = parseBlock();
    return std::move(static_cast<BlockStmt*>(b.get())->statements);
}

NodePtr Parser::parseReturnStmt() {
    auto r = std::make_unique<ReturnStmt>();
    advance(); // return
    if (!checkPunct(PunctuatorID::Semicolon) && !checkPunct(PunctuatorID::RBrace) &&
        !checkKw(KeywordID::Else) && !atEnd()) {
        r->value = parseExpr();
    }
    if (checkPunct(PunctuatorID::Semicolon)) advance();
    return r;
}

NodePtr Parser::parseIfStmt() {
    auto ifs = std::make_unique<IfStmt>();
    advance(); // if
    // optional condition in parens
    if (checkPunct(PunctuatorID::LParen)) { advance(); ifs->condition = parseCondition(); expectPunct(PunctuatorID::RParen, "expected ')'"); }
    else ifs->condition = parseCondition();
    ifs->thenBody = parseBlockStatements();
    if (matchKw(KeywordID::Else)) {
        if (checkKw(KeywordID::If)) ifs->elseBranch = parseIfStmt();
        else ifs->elseBranch = parseBlock();
    }
    return ifs;
}

NodePtr Parser::parseGuardStmt() {
    auto g = std::make_unique<GuardStmt>();
    advance(); // guard
    if (checkPunct(PunctuatorID::LParen)) { advance(); g->condition = parseCondition(); expectPunct(PunctuatorID::RParen, "expected ')'"); }
    else g->condition = parseCondition();
    expectKw(KeywordID::Else, "expected 'else'");
    g->elseBody = parseBlockStatements();
    return g;
}

NodePtr Parser::parseWhileStmt() {
    auto w = std::make_unique<WhileStmt>();
    advance(); // while
    if (checkPunct(PunctuatorID::LParen)) { advance(); w->condition = parseCondition(); expectPunct(PunctuatorID::RParen, "expected ')'"); }
    else w->condition = parseCondition();
    w->body = parseBlockStatements();
    return w;
}

NodePtr Parser::parseRepeatStmt() {
    auto r = std::make_unique<RepeatWhileStmt>();
    advance(); // repeat
    r->body = parseBlockStatements();
    expectKw(KeywordID::While, "expected 'while'");
    if (checkPunct(PunctuatorID::LParen)) { advance(); r->condition = parseExpr(); expectPunct(PunctuatorID::RParen, "expected ')'"); }
    else r->condition = parseExpr();
    return r;
}

NodePtr Parser::parseForInStmt() {
    auto f = std::make_unique<ForInStmt>();
    advance(); // for
    if (matchKw(KeywordID::Await)) f->isAsync = true;
    // pattern: `let x` / `var x` / bare `x` (implicitly a `let`) / tuple `(k, v)`
    if (checkKw(KeywordID::Let) || checkKw(KeywordID::Var)) {
        f->pattern = parseVarDecl(checkKw(KeywordID::Let), {}, /*member=*/false);
    } else if (checkPunct(PunctuatorID::LParen)) {
        auto tup = std::make_unique<TupleExpr>();
        advance(); // (
        while (!checkPunct(PunctuatorID::RParen) && !atEnd()) {
            if (checkKw(KeywordID::Let) || checkKw(KeywordID::Var)) {
                tup->elements.push_back(
                    parseVarDecl(checkKw(KeywordID::Let), {}, /*member=*/false));
            } else if (check(TokenKind::TK_Identifier)) {
                auto id = std::make_unique<IdentExpr>();
                id->name = cur().text; id->range = SourceRange{cur().loc, cur().loc};
                advance();
                tup->elements.push_back(std::move(id));
            } else {
                errorAt(cur(), "expected binding pattern in tuple");
                break;
            }
            if (!matchPunct(PunctuatorID::Comma)) break;
        }
        expectPunct(PunctuatorID::RParen, "expected ')'");
        f->pattern = std::move(tup);
    } else {
        auto v = std::make_unique<VarDecl>();
        v->isLet = true;
        if (check(TokenKind::TK_Identifier)) { v->name = cur().text; advance(); }
        if (matchPunct(PunctuatorID::Colon)) v->type = parseType();
        f->pattern = std::move(v);
    }
    expectKw(KeywordID::In, "expected 'in'");
    f->sequence = parseExprNoTrailingClosure();
    f->body = parseBlockStatements();
    return f;
}

// Parse a condition that may be a binding pattern (`let x = expr`) or an expression.
NodePtr Parser::parseCondition() {
    // A `{` after the condition opens the statement block, not a trailing closure.
    ++suppressTrailingClosure_;
    NodePtr result;
    if (checkKw(KeywordID::Let) || checkKw(KeywordID::Var)) {
        bool isLet = checkKw(KeywordID::Let);
        result = parseVarDecl(isLet, {}, /*member=*/false);
    } else {
        result = parseExpr();
    }
    --suppressTrailingClosure_;
    return result;
}

NodePtr Parser::parseSwitchStmt() {
    auto s = std::make_unique<SwitchStmt>();
    advance(); // switch
    if (checkPunct(PunctuatorID::LParen)) { advance(); s->subject = parseExprNoTrailingClosure(); expectPunct(PunctuatorID::RParen, "expected ')'"); }
    else s->subject = parseExprNoTrailingClosure();
    expectPunct(PunctuatorID::LBrace, "expected '{'");
    while (!checkPunct(PunctuatorID::RBrace) && !atEnd()) {
        auto c = std::make_unique<CaseClause>();
        if (matchKw(KeywordID::Default)) {
            c->isDefault = true;
        } else {
            expectKw(KeywordID::Case, "expected 'case'");
            c->pattern = parseCasePatternExpr(c->bindings);
            // `case 1, 2, 3:` — additional patterns for the same arm. They are
            // alternatives, so any of them selects this body.
            while (matchPunct(PunctuatorID::Comma)) {
                auto alt = std::make_unique<CaseClause>();
                alt->bindings = c->bindings;
                alt->pattern = parseCasePatternExpr(alt->bindings);
                c->alternatives.push_back(std::move(alt));
            }
            if (matchKw(KeywordID::Where)) c->whereExpr = parseExpr();
        }
        expectPunct(PunctuatorID::Colon, "expected ':'");
        // case body: statements until next case/default or '}'
        while (!atEnd() && !checkKw(KeywordID::Case) && !checkKw(KeywordID::Default) &&
               !checkPunct(PunctuatorID::RBrace)) {
            NodePtr st = parseStatement();
            if (st) c->body.push_back(std::move(st));
            else synchronize();
        }
        s->cases.push_back(std::move(c));
    }
    expectPunct(PunctuatorID::RBrace, "expected '}'");
    return s;
}

NodePtr Parser::parseDeferStmt() {
    auto d = std::make_unique<DeferStmt>();
    advance(); // defer
    d->body = parseStatement();
    return d;
}

NodePtr Parser::parseThrowStmt() {
    auto t = std::make_unique<ThrowStmt>();
    advance(); // throw
    t->value = parseExpr();
    return t;
}

NodePtr Parser::parseUnsafeStmt() {
    auto u = std::make_unique<UnsafeStmt>();
    advance(); // unsafe
    u->body = parseBlockStatements();
    return u;
}

NodePtr Parser::parseSelectStmt() {
    // `select { case <pat> <- <chan>: ... default: ... }` — structurally a
    // SwitchStmt without a subject; each case pattern is a `<-` channel expression.
    auto s = std::make_unique<SwitchStmt>();
    advance(); // select
    expectPunct(PunctuatorID::LBrace, "expected '{'");
    while (!checkPunct(PunctuatorID::RBrace) && !atEnd()) {
        auto c = std::make_unique<CaseClause>();
        if (matchKw(KeywordID::Default)) {
            c->isDefault = true;
        } else {
            expectKw(KeywordID::Case, "expected 'case'");
            c->pattern = parseCasePatternExpr(c->bindings);
            // `case 1, 2, 3:` — additional patterns for the same arm. They are
            // alternatives, so any of them selects this body.
            while (matchPunct(PunctuatorID::Comma)) {
                auto alt = std::make_unique<CaseClause>();
                alt->bindings = c->bindings;
                alt->pattern = parseCasePatternExpr(alt->bindings);
                c->alternatives.push_back(std::move(alt));
            }
            if (matchKw(KeywordID::Where)) c->whereExpr = parseExpr();
        }
        expectPunct(PunctuatorID::Colon, "expected ':'");
        while (!atEnd() && !checkKw(KeywordID::Case) && !checkKw(KeywordID::Default) &&
               !checkPunct(PunctuatorID::RBrace)) {
            NodePtr st = parseStatement();
            if (st) c->body.push_back(std::move(st));
            else synchronize();
        }
        s->cases.push_back(std::move(c));
    }
    expectPunct(PunctuatorID::RBrace, "expected '}'");
    return s;
}

NodePtr Parser::parseDoStmt() {
    auto d = std::make_unique<DoStmt>();
    advance(); // do
    d->body = parseBlockStatements();
    while (matchKw(KeywordID::Catch)) {
        auto c = std::make_unique<CatchClause>();
        // catch [let error as Type] or [pattern]
        if (!checkPunct(PunctuatorID::LBrace)) {
            // `catch let error as FileError` / `catch let error` / `catch let error where ...`
            if (matchKw(KeywordID::Let) && check(TokenKind::TK_Identifier)) {
                auto v = std::make_unique<VarDecl>();
                v->isLet = true; v->name = cur().text; advance();
                if (matchKw(KeywordID::As)) v->type = parseType();
                c->pattern = std::move(v);
            } else {
                c->pattern = parseExpr();
            }
            if (matchKw(KeywordID::Where)) c->whereExpr = parseExpr();
        }
        c->body = parseBlockStatements();
        d->catches.push_back(std::move(c));
    }
    return d;
}

// ─── expressions ───────────────────────────────────────────────────────────────
NodePtr Parser::parseExpression() { return parseExpr(); }

NodePtr Parser::parseExpr() { return parseAssignment(); }

NodePtr Parser::parseExprNoTrailingClosure() {
    ++suppressTrailingClosure_;
    NodePtr e = parseExpr();
    --suppressTrailingClosure_;
    return e;
}

NodePtr Parser::parseCasePatternExpr(std::vector<std::string>& bindings) {
    // A `(` here introduces a payload binding list (`case E.c(let x, let y)`),
    // not a call, so call parsing is suppressed for the subject expression.
    ++suppressCall_;
    NodePtr subject = parseExpr();
    --suppressCall_;

    if (checkPunct(PunctuatorID::LParen)) {
        advance(); // (
        while (!checkPunct(PunctuatorID::RParen) && !atEnd()) {
            // `let` / `var` markers are optional in binding patterns.
            if (checkKw(KeywordID::Let) || checkKw(KeywordID::Var)) advance();
            if (check(TokenKind::TK_Identifier)) {
                bindings.push_back(cur().text);
                advance();
            } else {
                errorAt(cur(), "expected binding name in case pattern");
                break;
            }
            if (!matchPunct(PunctuatorID::Comma)) break;
        }
        expectPunct(PunctuatorID::RParen, "expected ')'");
    }
    return subject;
}

// Parse a conditional whose condition is already-parsed. Used for the then
// branch of `?:`, so that a nested conditional claims its own `:`.
NodePtr Parser::parseConditionalFromRange() {
    NodePtr lhs = parseRange();
    if (!checkPunct(PunctuatorID::Question)) return lhs;
    advance();                                       // ?
    auto t = std::make_unique<TernaryExpr>();
    t->condition = std::move(lhs);
    t->thenValue = parseConditionalFromRange();
    expectPunct(PunctuatorID::Colon, "expected ':' in conditional expression");
    t->elseValue = parseAssignment();
    return t;
}

NodePtr Parser::parseAssignment() {
    NodePtr lhs = parseRange();
    // `cond ? a : b`. The then-branch is a conditional too, so nesting works;
    // the else-branch is a full assignment, which makes it right-associative.
    if (checkPunct(PunctuatorID::Question)) {
        advance();                                   // ?
        auto t = std::make_unique<TernaryExpr>();
        t->condition = std::move(lhs);
        t->thenValue = parseConditionalFromRange();
        expectPunct(PunctuatorID::Colon,
                    "expected ':' in conditional expression");
        t->elseValue = parseAssignment();
        return t;
    }
    if (checkPunct(PunctuatorID::Equal)) {
        advance();
        NodePtr rhs = parseAssignment(); // right associative
        auto a = std::make_unique<AssignmentExpr>();
        a->lhs = std::move(lhs);
        a->rhs = std::move(rhs);
        return a;
    }
    PunctuatorID compound = PunctuatorID::None;
    if (checkPunct(PunctuatorID::PlusEqual)) compound = PunctuatorID::Plus;
    else if (checkPunct(PunctuatorID::MinusEqual)) compound = PunctuatorID::Minus;
    else if (checkPunct(PunctuatorID::StarEqual)) compound = PunctuatorID::Star;
    else if (checkPunct(PunctuatorID::SlashEqual)) compound = PunctuatorID::Slash;
    else if (checkPunct(PunctuatorID::PercentEqual)) compound = PunctuatorID::Percent;
    if (compound != PunctuatorID::None) {
        advance();
        NodePtr rhs = parseAssignment(); // right associative
        auto a = std::make_unique<AssignmentExpr>();
        a->lhs = std::move(lhs);
        a->rhs = std::move(rhs);
        a->isCompound = true;
        a->compoundOp = compound;
        return a;
    }
    return lhs;
}

NodePtr Parser::parseRange() {
    NodePtr lhs = parseNilCoalescing();
    if (checkPunct(PunctuatorID::DotDotLess) || checkPunct(PunctuatorID::DotDot)) {
        bool halfOpen = checkPunct(PunctuatorID::DotDotLess);
        advance();
        auto r = std::make_unique<RangeExpr>();
        r->lower = std::move(lhs);
        r->upper = parseNilCoalescing();
        r->halfOpen = halfOpen;
        return r;
    }
    return lhs;
}

// `??` — nil-coalescing, loosest binary operator (right-associative)
NodePtr Parser::parseNilCoalescing() {
    NodePtr left = parseLogicalOr();
    while (checkPunct(PunctuatorID::QuestionQuestion)) {
        advance();
        NodePtr right = parseNilCoalescing();
        auto b = std::make_unique<BinaryExpr>();
        b->op = PunctuatorID::QuestionQuestion;
        b->lhs = std::move(left); b->rhs = std::move(right);
        left = std::move(b);
    }
    return left;
}

NodePtr Parser::parseLogicalOr() {
    NodePtr left = parseLogicalAnd();
    while (checkPunct(PunctuatorID::PipePipe)) {
        advance();
        NodePtr right = parseLogicalAnd();
        auto b = std::make_unique<BinaryExpr>();
        b->op = PunctuatorID::PipePipe;
        b->lhs = std::move(left); b->rhs = std::move(right);
        left = std::move(b);
    }
    return left;
}

NodePtr Parser::parseLogicalAnd() {
    NodePtr left = parseBitwiseOr();
    while (checkPunct(PunctuatorID::AmpAmp)) {
        advance();
        NodePtr right = parseBitwiseOr();
        auto b = std::make_unique<BinaryExpr>();
        b->op = PunctuatorID::AmpAmp;
        b->lhs = std::move(left); b->rhs = std::move(right);
        left = std::move(b);
    }
    return left;
}

NodePtr Parser::parseBitwiseOr() {
    NodePtr left = parseBitwiseXor();
    while (checkPunct(PunctuatorID::Pipe)) {
        advance();
        NodePtr right = parseBitwiseXor();
        auto b = std::make_unique<BinaryExpr>();
        b->op = PunctuatorID::Pipe;
        b->lhs = std::move(left); b->rhs = std::move(right);
        left = std::move(b);
    }
    return left;
}

NodePtr Parser::parseBitwiseXor() {
    NodePtr left = parseBitwiseAnd();
    while (checkPunct(PunctuatorID::Caret)) {
        advance();
        NodePtr right = parseBitwiseAnd();
        auto b = std::make_unique<BinaryExpr>();
        b->op = PunctuatorID::Caret;
        b->lhs = std::move(left); b->rhs = std::move(right);
        left = std::move(b);
    }
    return left;
}

NodePtr Parser::parseBitwiseAnd() {
    NodePtr left = parseComparison();
    while (checkPunct(PunctuatorID::Amp)) {
        advance();
        NodePtr right = parseComparison();
        auto b = std::make_unique<BinaryExpr>();
        b->op = PunctuatorID::Amp;
        b->lhs = std::move(left); b->rhs = std::move(right);
        left = std::move(b);
    }
    return left;
}

NodePtr Parser::parseComparison() {
    NodePtr left = parseShift();
    while (true) {
        if (checkPunct(PunctuatorID::EqualEqual) || checkPunct(PunctuatorID::BangEqual) ||
            checkPunct(PunctuatorID::Less) || checkPunct(PunctuatorID::Greater) ||
            checkPunct(PunctuatorID::LessEqual) || checkPunct(PunctuatorID::GreaterEqual) ||
            checkPunct(PunctuatorID::LeftArrow)) {
            PunctuatorID op = cur().punct; advance();
            NodePtr right = parseShift();
            auto b = std::make_unique<BinaryExpr>();
            b->op = op; b->lhs = std::move(left); b->rhs = std::move(right);
            left = std::move(b);
        } else if (matchKw(KeywordID::Is)) {
            auto is = std::make_unique<IsExpr>();
            is->expr = std::move(left);
            is->type = parseType();
            left = std::move(is);
        } else if (matchKw(KeywordID::As)) {
            auto as = std::make_unique<AsExpr>();
            as->expr = std::move(left);
            as->asKind = AsExpr::As;
            if (matchPunct(PunctuatorID::Question)) as->asKind = AsExpr::AsQuestion;
            else if (matchPunct(PunctuatorID::Bang)) as->asKind = AsExpr::AsBang;
            as->type = parseType();
            left = std::move(as);
        } else break;
    }
    return left;
}

NodePtr Parser::parseShift() {
    NodePtr left = parseAdditive();
    while (checkPunct(PunctuatorID::LessLess) || checkPunct(PunctuatorID::GreaterGreater)) {
        PunctuatorID op = cur().punct; advance();
        NodePtr right = parseAdditive();
        auto b = std::make_unique<BinaryExpr>();
        b->op = op; b->lhs = std::move(left); b->rhs = std::move(right);
        left = std::move(b);
    }
    return left;
}

NodePtr Parser::parseAdditive() {
    NodePtr left = parseMultiplicative();
    while (checkPunct(PunctuatorID::Plus) || checkPunct(PunctuatorID::Minus)) {
        PunctuatorID op = cur().punct; advance();
        NodePtr right = parseMultiplicative();
        auto b = std::make_unique<BinaryExpr>();
        b->op = op; b->lhs = std::move(left); b->rhs = std::move(right);
        left = std::move(b);
    }
    return left;
}

NodePtr Parser::parseMultiplicative() {
    NodePtr left = parseUnary();
    while (checkPunct(PunctuatorID::Star) || checkPunct(PunctuatorID::Slash) ||
           checkPunct(PunctuatorID::Percent)) {
        PunctuatorID op = cur().punct; advance();
        NodePtr right = parseUnary();
        auto b = std::make_unique<BinaryExpr>();
        b->op = op; b->lhs = std::move(left); b->rhs = std::move(right);
        left = std::move(b);
    }
    return left;
}

NodePtr Parser::parseUnary() {
    if (checkPunct(PunctuatorID::Minus) || checkPunct(PunctuatorID::Plus) ||
        checkPunct(PunctuatorID::Bang) || checkPunct(PunctuatorID::Tilde) ||
        checkPunct(PunctuatorID::Amp)) {
        PunctuatorID op = cur().punct; advance();
        auto u = std::make_unique<UnaryExpr>();
        u->op = op; u->isPostfix = false;
        u->operand = parseUnary();
        return u;
    }
    if (matchKw(KeywordID::Try)) {
        auto u = std::make_unique<UnaryExpr>();
        u->isTry = true; // dedicated flag, not an operator marker
        u->isPostfix = false;
        u->operand = parseUnary();
        return u;
    }
    if (matchKw(KeywordID::Await)) {
        auto u = std::make_unique<UnaryExpr>();
        u->isAwait = true; // dedicated flag, not an operator marker
        u->isPostfix = false;
        u->operand = parseUnary();
        return u;
    }
    if (matchKw(KeywordID::Move)) {
        auto m = std::make_unique<MoveExpr>();
        m->operand = parseUnary();
        return m;
    }
    return parsePostfix();
}

NodePtr Parser::parsePostfix() {
    NodePtr expr = parsePrimary();
    while (true) {
        // 后缀链：调用 / 尾随闭包 / 下标 / 成员访问 / 可选链 / 强制解包 / 泛型实参。
        //
        // 每个分支自己判断起始标点，链尾是无条件 `break`，因此任何不构成后缀的
        // token（例如 `}`、`;`、EOF）都会让循环正常结束。
        // 注意：这里绝不能在本循环体外再套一层 `if (cur 是 ?/./!)` 之类的守卫
        // —— 那样守卫为假时整条 if-else 链（含 break）都会被跳过，循环永不推进。
        if (checkPunct(PunctuatorID::LParen) && suppressCall_ == 0) {
            expr = parseCallSuffix(std::move(expr));
        } else if (checkPunct(PunctuatorID::LBrace) && suppressTrailingClosure_ == 0) {
            // trailing closure without parentheses: `Task { ... }`
            auto call = std::make_unique<CallExpr>();
            call->callee = std::move(expr);
            call->hasTrailingClosure = true;
            call->arguments.push_back(parseClosure(/*arrowSyntax=*/false));
            expr = std::move(call);
        } else if (checkPunct(PunctuatorID::LBracket)) {
            auto s = std::make_unique<SubscriptExpr>();
            s->base = std::move(expr);
            advance(); // [
            while (!checkPunct(PunctuatorID::RBracket) && !atEnd()) {
                s->indices.push_back(parseExpr());
                if (!matchPunct(PunctuatorID::Comma)) break;
            }
            expectPunct(PunctuatorID::RBracket, "expected ']'");
            expr = std::move(s);
        } else if (checkPunct(PunctuatorID::Dot)) {
            advance();
            bool optionalChain = false;
            if (checkPunct(PunctuatorID::Question)) { optionalChain = true; advance(); }
            // member name may be an identifier or a contextual keyword
            bool isName = check(TokenKind::TK_Identifier) ||
                          checkKw(KeywordID::Get) || checkKw(KeywordID::Set) ||
                          checkKw(KeywordID::WillSet) || checkKw(KeywordID::DidSet) ||
                          checkKw(KeywordID::Init) || checkKw(KeywordID::Deinit) ||
                          checkKw(KeywordID::Self) || checkKw(KeywordID::Super) ||
                          (check(TokenKind::TK_Identifier) &&
                           (cur().text == "Type" || cur().text == "Protocol")) ||
                          // Tuple element access uses a numeric member: `t.0`.
                          check(TokenKind::TK_IntLiteral);
            if (isName) {
                auto m = std::make_unique<MemberExpr>();
                m->base = std::move(expr);
                m->member = cur().text; advance();
                m->optionalChain = optionalChain;
                expr = std::move(m);
            } else { errorAt(cur(), "expected member name after '.'"); break; }
        } else if (checkPunct(PunctuatorID::Question) &&
                   peek(1).kind == TokenKind::TK_Punctuator &&
                   peek(1).punct == PunctuatorID::Dot) {
            // `?.` is optional chaining. A bare `?` is *not* consumed here: it
            // belongs to the conditional operator `c ? a : b`, which is parsed
            // one level up. Peeking keeps both spellings working.
            advance(); // ?
            advance(); // .
            if (check(TokenKind::TK_Identifier)) {
                auto m = std::make_unique<MemberExpr>();
                m->base = std::move(expr);
                m->member = cur().text; advance();
                m->optionalChain = true;
                expr = std::move(m);
                continue;
            }
            errorAt(cur(), "expected member name after '?.'");
            break;
        } else if (checkPunct(PunctuatorID::Question) && !nextStartsExpression()) {
            // A `?` that is not followed by anything expression-like promotes the
            // value to an Optional (`Int` -> `Int?`). `?:` and `?.` both have an
            // expression after them, so they are left to their own rules.
            advance();
            auto o = std::make_unique<OptionalChainExpr>();
            o->expr = std::move(expr);
            expr = std::move(o);
        } else if (checkPunct(PunctuatorID::Bang)) {
            advance();
            auto f = std::make_unique<ForceUnwrapExpr>();
            f->expr = std::move(expr);
            expr = std::move(f);
        } else if (checkPunct(PunctuatorID::Less) &&
                   (expr->kind == NodeKind::IdentExpr ||
                    expr->kind == NodeKind::MemberExpr ||
                    expr->kind == NodeKind::GenericExpr)) {
            // Possible generic specialization: Base<Args>. To avoid mistaking a
            // binary comparison `a < b` for specialization, only attempt it when
            // the token immediately after '<' can begin a type (identifier,
            // '[', '(', or '&'). Otherwise this '<' is a real comparison
            // operator and must be handled by parseComparison.
            const Token& after = peek(1);
            bool typeStart =
                after.kind == TokenKind::TK_Identifier ||
                (after.kind == TokenKind::TK_Punctuator &&
                 (after.punct == PunctuatorID::LBracket ||
                  after.punct == PunctuatorID::LParen ||
                  after.punct == PunctuatorID::Amp));
            if (!typeStart) break; // leave '<' for the comparison parser

            size_t save = pos_;
            advance(); // consume '<'
            NodeList args;
            while (!atEnd() && !checkPunct(PunctuatorID::Greater)) {
                args.push_back(parseType());
                if (!matchPunct(PunctuatorID::Comma)) break;
            }
            if (checkPunct(PunctuatorID::Greater)) {
                advance(); // consume '>'
                auto g = std::make_unique<GenericExpr>();
                g->base = std::move(expr);
                g->args = std::move(args);
                expr = std::move(g);
                continue;
            }
            // Not a specialization (no matching '>'); rewind and stop the
            // postfix loop so the '<' is treated as a binary operator.
            pos_ = save;
            break;
        } else {
            break;
        }
    }
    return expr;
}

NodePtr Parser::parseCallSuffix(NodePtr callee) {
    auto call = std::make_unique<CallExpr>();
    call->callee = std::move(callee);
    advance(); // (
    if (!checkPunct(PunctuatorID::RParen)) {
        while (!atEnd()) {
            std::string label;
            if (check(TokenKind::TK_Identifier) && peek(1).kind == TokenKind::TK_Punctuator && peek(1).punct == PunctuatorID::Colon) {
                label = cur().text; advance(); advance(); // name:
            }
            call->argumentLabels.push_back(label);
            call->arguments.push_back(parseExpr());
            if (!matchPunct(PunctuatorID::Comma)) break;
        }
    }
    expectPunct(PunctuatorID::RParen, "expected ')'");
    // trailing closure (e.g. `withTaskGroup(...) { ... }`), unless we are in a
    // control-flow condition where a following `{` opens the statement block.
    if (checkPunct(PunctuatorID::LBrace) && suppressTrailingClosure_ == 0) {
        call->hasTrailingClosure = true;
        call->arguments.push_back(parsePrimary()); // closure parsed in parsePrimary
    }
    return call;
}

bool Parser::looksLikeArrowClosure() {
    if (cur().kind != TokenKind::TK_Punctuator || cur().punct != PunctuatorID::LParen)
        return false;
    size_t i = pos_;
    int depth = 0;
    for (; i < tokens_.size(); ++i) {
        const Token& t = tokens_[i];
        if (t.kind == TokenKind::TK_Punctuator && t.punct == PunctuatorID::LParen) ++depth;
        else if (t.kind == TokenKind::TK_Punctuator && t.punct == PunctuatorID::RParen) {
            --depth;
            if (depth == 0) {
                if (i + 1 < tokens_.size() &&
                    tokens_[i + 1].kind == TokenKind::TK_Punctuator &&
                    tokens_[i + 1].punct == PunctuatorID::FatArrow)
                    return true;
                return false;
            }
        }
    }
    return false;
}

NodePtr Parser::parsePrimary() {
    Token t = cur();
    // if used as an expression: `let x = if cond { a } else { b }`
    if (checkKw(KeywordID::If)) {
        auto e = std::make_unique<IfExpr>();
        advance(); // if
        if (checkPunct(PunctuatorID::LParen)) {
            advance(); e->condition = parseCondition();
            expectPunct(PunctuatorID::RParen, "expected ')'");
        } else {
            e->condition = parseCondition();
        }
        e->thenBody = parseBlockStatements();
        if (matchKw(KeywordID::Else)) {
            if (checkKw(KeywordID::If)) e->elseBranch = parsePrimary();
            else e->elseBranch = parseBlock();
        }
        return e;
    }
    if (check(TokenKind::TK_IntLiteral)) {
        advance();
        auto n = std::make_unique<IntLitExpr>();
        n->value = t.numberText; n->range = SourceRange{t.loc, t.loc}; return n;
    }
    if (check(TokenKind::TK_FloatLiteral)) {
        advance();
        auto n = std::make_unique<FloatLitExpr>();
        n->value = t.numberText; n->range = SourceRange{t.loc, t.loc}; return n;
    }
    if (check(TokenKind::TK_CharLiteral)) {
        advance();
        auto n = std::make_unique<CharLitExpr>();
        n->value = t.stringValue; n->range = SourceRange{t.loc, t.loc}; return n;
    }
    if (checkKw(KeywordID::True) || checkKw(KeywordID::False)) {
        advance();
        auto n = std::make_unique<BoolLitExpr>();
        n->value = (t.keyword == KeywordID::True); n->range = SourceRange{t.loc, t.loc}; return n;
    }
    if (checkKw(KeywordID::Nil)) {
        advance();
        auto n = std::make_unique<NilLitExpr>();
        n->range = SourceRange{t.loc, t.loc}; return n;
    }
    if (checkKw(KeywordID::Self) || checkKw(KeywordID::Super)) {
        advance();
        auto id = std::make_unique<IdentExpr>();
        id->name = (t.keyword == KeywordID::Self) ? "self" : "super";
        id->range = SourceRange{t.loc, t.loc}; return id;
    }
    if (check(TokenKind::TK_StringLiteral)) {
        advance();
        auto s = std::make_unique<StrLitExpr>();
        s->isRaw = (t.text.size() >= 2 && t.text[0] == 'r');
        // strip surrounding quotes
        std::string inner = t.stringValue;
        s->segments.push_back(inner);
        s->range = SourceRange{t.loc, t.loc};
        return s;
    }
    if (check(TokenKind::TK_StringFragment)) {
        // interpolated string
        auto s = std::make_unique<StrLitExpr>();
        s->segments.push_back(t.stringValue);
        advance();
        while (!atEnd()) {
            if (check(TokenKind::TK_StringEnd)) { advance(); break; }
            if (check(TokenKind::TK_StringFragment)) {
                s->segments.push_back(cur().stringValue);
                advance();
            } else {
                s->expressions.push_back(parseExpr());
                if (check(TokenKind::TK_StringFragment)) {
                    s->segments.push_back(cur().stringValue);
                    advance();
                }
            }
        }
        return s;
    }
    if (check(TokenKind::TK_Identifier)) {
        advance();
        auto id = std::make_unique<IdentExpr>();
        id->name = t.text; id->range = SourceRange{t.loc, t.loc}; return id;
    }
    if (checkPunct(PunctuatorID::LParen)) {
        if (looksLikeArrowClosure()) return parseClosure(true);
        advance();
        if (checkPunct(PunctuatorID::RParen)) {
            advance();
            auto p = std::make_unique<ParenExpr>(); // empty tuple / unit
            return p;
        }
        // tuple or parenthesized expr
        NodePtr first = parseExpr();
        if (matchPunct(PunctuatorID::Comma)) {
            auto tup = std::make_unique<TupleExpr>();
            tup->elements.push_back(std::move(first));
            while (!checkPunct(PunctuatorID::RParen) && !atEnd()) {
                tup->elements.push_back(parseExpr());
                if (!matchPunct(PunctuatorID::Comma)) break;
            }
            expectPunct(PunctuatorID::RParen, "expected ')'");
            return tup;
        }
        expectPunct(PunctuatorID::RParen, "expected ')'");
        auto p = std::make_unique<ParenExpr>();
        p->expr = std::move(first);
        return p;
    }
    if (checkPunct(PunctuatorID::LBracket)) {
        return parseCollectionLiteral();
    }
    if (checkPunct(PunctuatorID::LBrace)) {
        // `{` starts a closure, but `{ 1, 2 }` is a set literal. A brace whose
        // next token is a value (or `}`) therefore opens a collection; anything
        // else — a parameter list, a statement — is a closure.
        // `{ x in ... }` / `{ x, y in ... }` is a closure with a bare parameter
        // list, never a set literal. The `in` may follow several names, so the
        // whole run is scanned.
        if (closureParamsAhead())
            return parseClosure(/*arrowSyntax=*/false);
        const Token& nxt = peek(1);
        bool setLiteral = nxt.kind == TokenKind::TK_IntLiteral ||
                          nxt.kind == TokenKind::TK_FloatLiteral ||
                          nxt.kind == TokenKind::TK_StringLiteral ||
                          nxt.kind == TokenKind::TK_StringFragment ||
                          nxt.kind == TokenKind::TK_CharLiteral ||
                          nxt.kind == TokenKind::TK_Identifier ||
                          (nxt.kind == TokenKind::TK_Punctuator &&
                           (nxt.punct == PunctuatorID::Minus ||
                            nxt.punct == PunctuatorID::LBracket));
        if (setLiteral) return parseSetLiteral();
        return parseClosure(/*arrowSyntax=*/false);
    }
    errorAt(cur(), "unexpected token in expression");
    advance();
    return nullptr;
}

// True when the tokens after the current `{` are a closure parameter list —
// one or more names, optionally typed, then `in`. A set literal has no `in`, so
// this is what tells `{ x in ... }` apart from `{ x }`.
bool Parser::closureParamsAhead() {
    size_t off = 1;
    // An empty or immediately-closed brace is a literal, not a parameter list.
    if (peek(off).kind == TokenKind::TK_Punctuator &&
        peek(off).punct == PunctuatorID::RBrace)
        return false;
    bool sawName = false;
    while (off < 64) {
        const Token& t = peek(off);
        if (t.kind == TokenKind::TK_Identifier ||
            t.kind == TokenKind::TK_Keyword) {
            sawName = true;
            ++off;
            // `name: Type` is a typed parameter; skip the type up to the comma
            // or the `in` that ends the list.
            if (peek(off).kind == TokenKind::TK_Punctuator &&
                peek(off).punct == PunctuatorID::Colon) {
                ++off;
                while (off < 64) {
                    const Token& ty = peek(off);
                    if (ty.kind == TokenKind::TK_Punctuator &&
                        (ty.punct == PunctuatorID::Comma ||
                         ty.punct == PunctuatorID::RBrace))
                        break;
                    ++off;
                }
            }
            if (peek(off).kind == TokenKind::TK_Keyword &&
                peek(off).keyword == KeywordID::In)
                return sawName;
            if (peek(off).kind == TokenKind::TK_Punctuator &&
                peek(off).punct == PunctuatorID::Comma) { ++off; continue; }
            return false;
        }
        return false;
    }
    return false;
}

// True when the token after the current one can begin an expression. Used to
// tell a trailing `?` (Optional promotion) from `? :` (conditional) and `?.`
// (chaining), both of which are followed by something expression-like.
bool Parser::nextStartsExpression() {
    const Token& t = peek(1);
    switch (t.kind) {
        case TokenKind::TK_IntLiteral:
        case TokenKind::TK_FloatLiteral:
        case TokenKind::TK_StringLiteral:
        case TokenKind::TK_StringFragment:
        case TokenKind::TK_CharLiteral:
        case TokenKind::TK_Identifier:
        case TokenKind::TK_Keyword:
            return true;
        case TokenKind::TK_Punctuator:
            // `.` continues a member access and `-` a negation; both continue
            // the expression rather than starting a new one after `?`.
            return t.punct == PunctuatorID::Dot ||
                   t.punct == PunctuatorID::Minus ||
                   t.punct == PunctuatorID::Bang;
        default:
            return false;
    }
}

// `{1, 2, 3}` — a set literal. The opening brace is consumed by the caller.
NodePtr Parser::parseSetLiteral() {
    expectPunct(PunctuatorID::LBrace, "expected '{'");
    auto set = std::make_unique<SetLitExpr>();
    if (checkPunct(PunctuatorID::RBrace)) {
        advance();
        return set;   // the empty set
    }
    while (!atEnd()) {
        NodePtr e = parseExpr();
        if (!e) break;
        set->elements.push_back(std::move(e));
        if (!matchPunct(PunctuatorID::Comma)) break;
        if (checkPunct(PunctuatorID::RBrace)) break;
    }
    expectPunct(PunctuatorID::RBrace, "expected '}'");
    return set;
}

NodePtr Parser::parseCollectionLiteral() {
    expectPunct(PunctuatorID::LBracket, "expected '['");
    if (matchPunct(PunctuatorID::RBracket)) {
        auto a = std::make_unique<ArrayLitExpr>();
        return a; // empty array
    }
    // Empty dictionary literal `[:]`. This must be recognised before parsing
    // a first element, otherwise the leading colon is reported as a stray
    // token.
    if (checkPunct(PunctuatorID::Colon)) {
        advance(); // :
        expectPunct(PunctuatorID::RBracket, "expected ']'");
        return std::make_unique<DictLitExpr>();
    }
    NodePtr first = parseExpr();
    if (matchPunct(PunctuatorID::Colon)) {
        // dictionary
        auto d = std::make_unique<DictLitExpr>();
        NodePtr firstVal = parseExpr();
        d->keys.push_back(std::move(first));
        d->values.push_back(std::move(firstVal));
        while (matchPunct(PunctuatorID::Comma)) {
            if (checkPunct(PunctuatorID::RBracket)) break;
            NodePtr k = parseExpr();
            expectPunct(PunctuatorID::Colon, "expected ':' in dictionary");
            NodePtr v = parseExpr();
            d->keys.push_back(std::move(k));
            d->values.push_back(std::move(v));
        }
        expectPunct(PunctuatorID::RBracket, "expected ']'");
        return d;
    }
    auto a = std::make_unique<ArrayLitExpr>();
    a->elements.push_back(std::move(first));
    while (matchPunct(PunctuatorID::Comma)) {
        if (checkPunct(PunctuatorID::RBracket)) break;
        a->elements.push_back(parseExpr());
    }
    expectPunct(PunctuatorID::RBracket, "expected ']'");
    return a;
}

NodePtr Parser::parseClosure(bool arrowSyntax) {
    auto c = std::make_unique<ClosureExpr>();
    c->isArrowSyntax = arrowSyntax;
    if (arrowSyntax) {
        // (params) => expr   OR   params => expr
        if (matchPunct(PunctuatorID::LParen)) {
            if (!checkPunct(PunctuatorID::RParen)) {
                while (!atEnd()) {
                    Param p;
                    if (check(TokenKind::TK_Identifier)) { p.externalName = cur().text; p.internalName = cur().text; advance(); }
                    if (matchPunct(PunctuatorID::Colon)) p.type = parseType();
                    c->params.push_back(std::move(p));
                    if (!matchPunct(PunctuatorID::Comma)) break;
                }
            }
            expectPunct(PunctuatorID::RParen, "expected ')'");
        } else if (check(TokenKind::TK_Identifier)) {
            Param p; p.externalName = cur().text; p.internalName = cur().text; advance();
            if (matchPunct(PunctuatorID::Colon)) p.type = parseType();
            c->params.push_back(std::move(p));
        }
        expectPunct(PunctuatorID::FatArrow, "expected '=>'");
        c->body.push_back([&]() -> NodePtr {
            auto r = std::make_unique<ReturnStmt>();
            r->value = parseExpr();
            return r;
        }());
        return c;
    }
    // block closure: { (params) -> Ret in ... }
    expectPunct(PunctuatorID::LBrace, "expected '{'");
    // optional parameter clause before `in`
    if (checkPunct(PunctuatorID::LParen)) {
        advance();
        while (!checkPunct(PunctuatorID::RParen) && !atEnd()) {
            Param p;
            if (check(TokenKind::TK_Identifier)) { p.externalName = cur().text; p.internalName = cur().text; advance(); }
            if (matchPunct(PunctuatorID::Colon)) p.type = parseType();
            c->params.push_back(std::move(p));
            if (!matchPunct(PunctuatorID::Comma)) break;
        }
        expectPunct(PunctuatorID::RParen, "expected ')'");
    } else if (check(TokenKind::TK_Identifier) &&
               peek(1).kind == TokenKind::TK_Punctuator && peek(1).punct == PunctuatorID::Colon) {
        Param p;
        p.externalName = cur().text; p.internalName = cur().text; advance();
        advance(); // consume ':'
        p.type = parseType();
        c->params.push_back(std::move(p));
    } else {
        // `{ x, y in ... }` — bare identifier parameter list terminated by `in`
        size_t save = pos_;
        std::vector<std::string> names;
        bool ok = true;
        while (true) {
            if (!check(TokenKind::TK_Identifier)) { ok = false; break; }
            names.push_back(cur().text);
            advance();
            if (checkKw(KeywordID::In)) break;
            if (matchPunct(PunctuatorID::Comma)) continue;
            ok = false; break;
        }
        if (ok && checkKw(KeywordID::In)) {
            for (auto& n : names) {
                Param p; p.externalName = n; p.internalName = n;
                c->params.push_back(std::move(p));
            }
        } else {
            pos_ = save; // not a parameter list; rewind
        }
    }
    if (matchPunct(PunctuatorID::Arrow)) c->returnType = parseType();
    if (matchKw(KeywordID::In)) {
        while (!checkPunct(PunctuatorID::RBrace) && !atEnd()) {
            NodePtr s = parseStatement();
            if (s) c->body.push_back(std::move(s));
            else synchronize();
        }
    } else {
        // no `in`: the whole body is statements; but we already may have parsed a param
        // treat until '}'
        while (!checkPunct(PunctuatorID::RBrace) && !atEnd()) {
            NodePtr s = parseStatement();
            if (s) c->body.push_back(std::move(s));
            else synchronize();
        }
    }
    expectPunct(PunctuatorID::RBrace, "expected '}'");
    return c;
}

// ─── types ─────────────────────────────────────────────────────────────────────
NodePtr Parser::parseType() {
    NodePtr base;
    if (checkKw(KeywordID::Inout)) {
        advance();
        auto it = std::make_unique<InoutType>();
        it->pointee = parseType();
        base = std::move(it);
        return parseTypePostfix(std::move(base));
    }
    if (checkPunct(PunctuatorID::LBracket)) {
        advance();
        if (checkPunct(PunctuatorID::RBracket)) {
            advance();
            auto a = std::make_unique<ArrayType>();
            a->element = std::make_unique<PlaceholderType>();
            base = std::move(a);
        } else {
            NodePtr first = parseType();
            if (matchPunct(PunctuatorID::Colon)) {
                NodePtr val = parseType();
                auto d = std::make_unique<DictType>();
                d->key = std::move(first); d->value = std::move(val);
                expectPunct(PunctuatorID::RBracket, "expected ']'");
                base = std::move(d);
            } else {
                auto a = std::make_unique<ArrayType>();
                a->element = std::move(first);
                expectPunct(PunctuatorID::RBracket, "expected ']'");
                base = std::move(a);
            }
        }
        return parseTypePostfix(std::move(base));
    }
    if (check(TokenKind::TK_Identifier)) {
        auto n = std::make_unique<NamedType>();
        n->name = cur().text; advance();
        if (checkPunct(PunctuatorID::Less)) {
            advance();
            while (!checkPunct(PunctuatorID::Greater) && !atEnd()) {
                n->genericArgs.push_back(parseType());
                if (!matchPunct(PunctuatorID::Comma)) break;
            }
            expectPunct(PunctuatorID::Greater, "expected '>'");
        }
        base = std::move(n);
    } else if (checkPunct(PunctuatorID::LParen)) {
        // tuple type or function type
        advance();
        if (checkPunct(PunctuatorID::RParen)) {
            advance();
            // unit type () -> treat as empty tuple
            auto t = std::make_unique<TupleType>();
            base = std::move(t);
        } else {
            NodePtr first = parseType();
            if (matchPunct(PunctuatorID::Arrow)) {
                // function type () -> R
                auto ft = std::make_unique<FuncType>();
                ft->params.push_back(std::move(first));
                ft->ret = parseType();
                base = std::move(ft);
            } else if (matchPunct(PunctuatorID::Comma)) {
                auto t = std::make_unique<TupleType>();
                t->elements.push_back(std::move(first));
                while (!checkPunct(PunctuatorID::RParen) && !atEnd()) {
                    t->elements.push_back(parseType());
                    if (!matchPunct(PunctuatorID::Comma)) break;
                }
                expectPunct(PunctuatorID::RParen, "expected ')'");
                base = std::move(t);
            } else {
                expectPunct(PunctuatorID::RParen, "expected ')'");
                auto t = std::make_unique<TupleType>();
                t->elements.push_back(std::move(first));
                base = std::move(t);
            }
        }
        return parseTypePostfix(std::move(base));
    } else if (checkPunct(PunctuatorID::Bang)) {
        advance();
        base = std::make_unique<NeverType>();
        return parseTypePostfix(std::move(base));
    } else if (check(TokenKind::TK_Identifier) && cur().text == "_") {
        advance();
        base = std::make_unique<PlaceholderType>();
        return parseTypePostfix(std::move(base));
    } else {
        errorAt(cur(), "expected type");
        base = std::make_unique<NamedType>();
        static_cast<NamedType*>(base.get())->name = "<<error>>";
    }
    return parseTypePostfix(std::move(base));
}

NodePtr Parser::parseTypePostfix(NodePtr base) {
    while (true) {
        if (checkPunct(PunctuatorID::Question)) {
            advance();
            auto o = std::make_unique<OptionalType>();
            o->wrapped = std::move(base);
            base = std::move(o);
        } else if (matchKw(KeywordID::Some)) {
            // opaque type `some Protocol`
            auto o = std::make_unique<OptionalType>(); // reuse; sema distinguishes
            o->wrapped = std::move(base);
            base = std::move(o);
        } else if (checkPunct(PunctuatorID::LBracket)) {
            advance();
            if (checkPunct(PunctuatorID::RBracket)) {
                advance();
                auto a = std::make_unique<ArrayType>();
                a->element = std::move(base);
                base = std::move(a);
            } else {
                NodePtr key = parseType();
                if (matchPunct(PunctuatorID::Colon)) {
                    NodePtr val = parseType();
                    auto d = std::make_unique<DictType>();
                    d->key = std::move(key); d->value = std::move(val);
                    // wrap: base[key:value]? SukiCode uses base as element; here we treat [K:V]
                    base = std::move(d);
                } else {
                    auto a = std::make_unique<ArrayType>();
                    a->element = std::move(key);
                    base = std::move(a);
                }
                expectPunct(PunctuatorID::RBracket, "expected ']'");
            }
        } else if (checkPunct(PunctuatorID::Arrow)) {
            advance();
            auto ft = std::make_unique<FuncType>();
            // base could be a single param type or tuple
            ft->params.push_back(std::move(base));
            ft->ret = parseType();
            base = std::move(ft);
        } else if (checkPunct(PunctuatorID::Amp)) {
            advance();
            auto r = std::make_unique<RefType>();
            r->refKind = "shared";
            r->pointee = parseType();
            base = std::move(r);
        } else if (checkPunct(PunctuatorID::At)) {
            advance();
            std::string kind;
            if (check(TokenKind::TK_Identifier)) { kind = cur().text; advance(); }
            auto r = std::make_unique<RefType>();
            r->refKind = kind.empty() ? "shared" : kind;
            r->pointee = parseType();
            base = std::move(r);
        } else if (checkPunct(PunctuatorID::Dot)) {
            advance();
            if (check(TokenKind::TK_Identifier) && cur().text == "Type") {
                advance();
                auto m = std::make_unique<MetatypeType>();
                m->base = std::move(base); m->isMeta = true;
                base = std::move(m);
            } else if (matchKw(KeywordID::Protocol)) {
                auto m = std::make_unique<MetatypeType>();
                m->base = std::move(base); m->isMeta = false;
                base = std::move(m);
            } else { errorAt(cur(), "expected 'Type' or 'Protocol'"); break; }
        } else {
            break;
        }
    }
    return base;
}

} // namespace suki
