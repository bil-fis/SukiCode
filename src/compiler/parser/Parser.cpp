// SukiCode recursive descent parser implementation.
// Parses tokens into an AST. Handles all SukiCode syntax including
// expressions, statements, declarations, types, and patterns.

#include "Parser.h"
#include "compiler/lexer/Lexer.h"
#include <cassert>

namespace suki {

Parser::Parser(std::vector<Token> tokens, std::string_view source,
               std::string_view filename, DiagnosticEngine& diag)
    : tokens_(std::move(tokens)), pos_(0), source_(source),
      filename_(filename), diag_(diag) {}

// ─── Token navigation ─────────────────────────────────────────────────────

const Token& Parser::peek() const {
    if (pos_ >= tokens_.size()) return tokens_.back(); // Eof
    return tokens_[pos_];
}

const Token& Parser::peekAt(size_t offset) const {
    size_t idx = pos_ + offset;
    if (idx >= tokens_.size()) return tokens_.back();
    return tokens_[idx];
}

const Token& Parser::advance() {
    const Token& tok = peek();
    if (pos_ < tokens_.size() - 1) pos_++;
    return tok;
}

bool Parser::check(TokenKind kind) const {
    return peek().is(kind);
}

bool Parser::match(TokenKind kind) {
    if (check(kind)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::expect(TokenKind kind) {
    if (check(kind)) {
        advance();
        return true;
    }
    std::string msg = "expected ";
    msg += Token::kindName(kind);
    msg += ", got ";
    msg += Token::kindName(peek().kind);
    error(msg);
    return false;
}

bool Parser::isAtEnd() const {
    return check(TokenKind::Eof);
}

SourceLocation Parser::loc() const {
    return peek().loc;
}

void Parser::error(std::string_view message) {
    diag_.error(loc(), filename_, message);
}

void Parser::synchronize() {
    // Skip tokens until we find a statement boundary
    while (!isAtEnd()) {
        if (peek().is(TokenKind::Semicolon)) {
            advance();
            return;
        }
        // Check for tokens that typically start a new statement
        if (peek().isOneOf({
            TokenKind::KwFunc, TokenKind::KwLet, TokenKind::KwVar,
            TokenKind::KwReturn, TokenKind::KwIf, TokenKind::KwFor,
            TokenKind::KwWhile, TokenKind::KwSwitch, TokenKind::KwClass,
            TokenKind::KwStruct, TokenKind::KwEnum, TokenKind::KwProtocol,
            TokenKind::KwImport, TokenKind::KwModule,
        })) {
            return;
        }
        advance();
    }
}

// ─── Top-level parsing ────────────────────────────────────────────────────

std::unique_ptr<CompilationUnit> Parser::parse() {
    auto cu = makeNode<CompilationUnit>();
    cu->filename = std::string(filename_);

    // Optional module declaration
    if (match(TokenKind::KwModule)) {
        auto mod = makeNode<ModuleDecl>();
        if (check(TokenKind::Identifier)) {
            mod->name = std::string(advance().stringValue);
        } else {
            error("expected module name after 'module'");
        }
        // Semicolons are optional (spec says no semicolons, but allow for module line)
        match(TokenKind::Semicolon);
        cu->moduleDecl = mod.get();
        cu->declarations.push_back(std::move(mod));
    }

    // Parse declarations
    while (!isAtEnd()) {
        try {
            parseDeclarationInto(cu->declarations);
        } catch (...) {
            synchronize();
        }
    }

    return cu;
}

// ─── Declaration parsing ──────────────────────────────────────────────────

void Parser::parseDeclarationInto(std::vector<DeclPtr>& out) {
    auto decl = parseDeclaration();
    if (decl) {
        out.push_back(std::move(decl));
        for (auto& extra : multiDecls_) {
            out.push_back(std::move(extra));
        }
        multiDecls_.clear();
    }
}

DeclPtr Parser::parseDeclaration() {
    // Check for attributes
    // (Attributes are parsed inline where needed for now)

    // Access level modifiers
    AccessLevel access = AccessLevel::Internal;
    bool isStatic = false;

    // Parse access modifiers and other modifiers
    bool isOverride = false;
    bool isMutating = false;
    bool isConvenience = false;
    bool isRequired = false;
    bool isFinal = false;
    while (true) {
        if (match(TokenKind::KwPublic)) { access = AccessLevel::Public; continue; }
        if (match(TokenKind::KwInternal)) { access = AccessLevel::Internal; continue; }
        if (match(TokenKind::KwFileprivate)) { access = AccessLevel::FilePrivate; continue; }
        if (match(TokenKind::KwPrivate)) { access = AccessLevel::Private; continue; }
        if (match(TokenKind::KwOpen)) { access = AccessLevel::Open; continue; }
        if (match(TokenKind::KwStatic)) { isStatic = true; continue; }
        if (match(TokenKind::KwOverride)) { isOverride = true; continue; }
        if (match(TokenKind::KwMutating)) { isMutating = true; continue; }
        if (match(TokenKind::KwConvenience)) { isConvenience = true; continue; }
        if (match(TokenKind::KwRequired)) { isRequired = true; continue; }
        if (match(TokenKind::KwFinal)) { isFinal = true; continue; }
        if (match(TokenKind::KwAsync)) { continue; } // async modifier (consumed, stored later)
        break;
    }

    // Collect attributes
    std::vector<Attribute> attrs;
    while (true) {
        if (check(TokenKind::AtMain) || check(TokenKind::AtCImport) ||
            check(TokenKind::AtCDecl) || check(TokenKind::AtMacro) ||
            check(TokenKind::AtTest) || check(TokenKind::AtEnumC) ||
            check(TokenKind::AtNoMangle) || check(TokenKind::AtPanicHandler) ||
            check(TokenKind::AtAttribute)) {
            Attribute attr;
            attr.loc = loc();
            attr.name = std::string(advance().stringValue);
            // Handle parenthesized arguments: @attr(args)
            if (check(TokenKind::LParen)) {
                advance(); // (
                while (!check(TokenKind::RParen) && !isAtEnd()) {
                    if (check(TokenKind::Identifier)) {
                        attr.args.push_back(std::string(advance().stringValue));
                    } else {
                        advance(); // skip
                    }
                    match(TokenKind::Comma);
                }
                match(TokenKind::RParen);
            }
            attrs.push_back(std::move(attr));
            continue;
        }
        break;
    }

    DeclPtr decl;

    switch (peek().kind) {
        case TokenKind::KwModule:    decl = parseModuleDecl(); break;
        case TokenKind::KwImport:    decl = parseImportDecl(); break;
        case TokenKind::KwLet:
        case TokenKind::KwVar:       decl = parseVariableDecl(); break;
        case TokenKind::KwFunc:      decl = parseFunctionDecl(); break;
        case TokenKind::KwStruct:    decl = parseStructDecl(); break;
        case TokenKind::KwClass:     decl = parseClassDecl(); break;
        case TokenKind::KwEnum:      decl = parseEnumDecl(); break;
        case TokenKind::KwProtocol:  decl = parseProtocolDecl(); break;
        case TokenKind::KwActor:     decl = parseActorDecl(); break;
        case TokenKind::KwExtension: decl = parseExtensionDecl(); break;
        case TokenKind::KwTypealias: decl = parseTypealiasDecl(); break;
        case TokenKind::KwInit:      decl = parseInitDecl(); break;
        case TokenKind::KwDeinit:    decl = parseDeinitDecl(); break;
        case TokenKind::KwSubscript: decl = parseSubscriptDecl(); break;

        // Control flow
        case TokenKind::KwIf:        decl = parseIfDecl(); break;
        case TokenKind::KwGuard:     decl = parseGuardDecl(); break;
        case TokenKind::KwSwitch:    decl = parseSwitchDecl(); break;
        case TokenKind::KwFor:       decl = parseForInDecl(); break;
        case TokenKind::KwWhile:     decl = parseWhileDecl(); break;
        case TokenKind::KwRepeat:    decl = parseRepeatWhileDecl(); break;
        case TokenKind::KwDo:        decl = parseDoCatchDecl(); break;
        case TokenKind::KwSelect:    decl = parseSelectDecl(); break;
        case TokenKind::KwUnsafe:    decl = parseUnsafeDecl(); break;
        case TokenKind::KwAsm:       decl = parseAsmDecl(); break;

        default: {
            // Try to parse as expression statement
            ExprPtr expr = parseExpression();
            if (expr) {
                auto stmt = makeNode<ExpressionStmt>();
                stmt->expression = std::move(expr);
                // Wrap in a declaration for top-level
                // For now, return as expression statement wrapped in a Decl
                // TODO: proper statement/declaration distinction at top level
                error("unexpected expression at top level");
            } else {
                error("expected declaration");
                advance(); // skip unknown token
            }
            return nullptr;
        }
    }

    if (decl) {
        decl->access = access;
        decl->isStatic = isStatic;
        decl->isOverride = isOverride;
        decl->isMutating = isMutating;
        decl->attributes = std::move(attrs);
    }
    return decl;
}

// ─── Module / Import ──────────────────────────────────────────────────────

DeclPtr Parser::parseModuleDecl() {
    expect(TokenKind::KwModule);
    auto decl = makeNode<ModuleDecl>();
    if (check(TokenKind::Identifier)) {
        decl->name = std::string(advance().stringValue);
    } else {
        error("expected module name");
    }
    match(TokenKind::Semicolon);
    return decl;
}

DeclPtr Parser::parseImportDecl() {
    expect(TokenKind::KwImport);
    auto decl = makeNode<ImportDecl>();
    if (check(TokenKind::Identifier)) {
        decl->moduleName = std::string(advance().stringValue);
    } else {
        error("expected module name after 'import'");
    }
    match(TokenKind::Semicolon);
    return decl;
}

// ─── Variable declaration ─────────────────────────────────────────────────

DeclPtr Parser::parseVariableDecl() {
    bool isLet = peek().is(TokenKind::KwLet);
    advance(); // consume let/var

    // Parse first variable name
    std::vector<std::string> names;
    if (check(TokenKind::Identifier)) {
        names.push_back(std::string(advance().stringValue));
    } else if (match(TokenKind::Underscore)) {
        names.push_back("_");
    } else {
        error("expected variable name");
    }

    // Check for multi-variable declaration: var x, y, z: Type
    while (match(TokenKind::Comma)) {
        if (check(TokenKind::Identifier)) {
            names.push_back(std::string(advance().stringValue));
        } else if (match(TokenKind::Underscore)) {
            names.push_back("_");
        } else {
            error("expected variable name after ','");
        }
    }

    // Shared type annotation
    TypeReprPtr sharedType;
    if (match(TokenKind::Colon)) {
        sharedType = parseType();
    }

    // Shared initializer
    ExprPtr sharedInit;
    if (match(TokenKind::Assign)) {
        sharedInit = parseExpression();
    }

    // Property accessors or computed property getter:
    // { get }, { get set } (protocol requirements)
    // { return expr } (computed property getter body)
    // Only check if no '=' was found AND no type annotation with initializer
    std::vector<StmtPtr> getterBody;
    std::vector<StmtPtr> setterBody;
    if (!sharedInit && check(TokenKind::LBrace)) {
        if (peekAt(1).is(TokenKind::KwGet) || peekAt(1).is(TokenKind::KwSet)) {
            // Accessor block: { get { ... } set { ... } }
            advance(); // {
            while (!check(TokenKind::RBrace) && !isAtEnd()) {
                if (match(TokenKind::KwGet)) {
                    getterBody = parseBlock();
                } else if (match(TokenKind::KwSet)) {
                    setterBody = parseBlock();
                } else {
                    advance(); // skip unknown
                }
            }
            match(TokenKind::RBrace); // }
        } else {
            // Computed property getter body: { statements }
            getterBody = parseBlock();
        }
    }

    // Create the first VariableDecl (returned as primary)
    auto primary = makeNode<VariableDecl>();
    primary->isLet = isLet;
    auto pat = makeNode<IdentifierPattern>();
    pat->name = names[0];
    pat->isLet = isLet;
    primary->pattern = std::move(pat);
    primary->typeAnnotation = std::move(sharedType);
    primary->initializer = std::move(sharedInit);
    primary->getterBody = std::move(getterBody);
    primary->setterBody = std::move(setterBody);

    // Create additional VariableDecls for remaining names (same type/init)
    multiDecls_.clear();
    for (size_t i = 1; i < names.size(); i++) {
        auto extra = makeNode<VariableDecl>();
        extra->isLet = isLet;
        auto extraPat = makeNode<IdentifierPattern>();
        extraPat->name = names[i];
        extraPat->isLet = isLet;
        extra->pattern = std::move(extraPat);
        multiDecls_.push_back(std::move(extra));
    }

    // TODO: parse willSet/didSet blocks
    return primary;
}

// ─── Function declaration ─────────────────────────────────────────────────

DeclPtr Parser::parseFunctionDecl() {
    expect(TokenKind::KwFunc);
    auto decl = makeNode<FunctionDecl>();

    // Function name (or operator for operator functions)
    if (check(TokenKind::Identifier)) {
        decl->name = std::string(advance().stringValue);
    } else if (check(TokenKind::KwInit)) {
        decl->name = "init";
        advance();
    } else {
        error("expected function name");
    }

    // Generic parameters
    if (check(TokenKind::Less)) {
        decl->genericParams = parseGenericParams();
    }

    // Parameter list
    if (expect(TokenKind::LParen)) {
        decl->params = parseParamList();
        expect(TokenKind::RParen);
    }

    // async/throws
    while (true) {
        if (match(TokenKind::KwAsync)) { decl->isAsync = true; continue; }
        if (match(TokenKind::KwThrows)) { decl->isThrows = true; continue; }
        break;
    }

    // Return type
    if (match(TokenKind::Arrow)) {
        decl->returnType = parseType();
    }

    // Body (optional for protocol requirements)
    if (check(TokenKind::LBrace)) {
        decl->body = parseBlock();
    }

    return decl;
}

// ─── Type declarations ────────────────────────────────────────────────────

DeclPtr Parser::parseStructDecl() {
    expect(TokenKind::KwStruct);
    auto decl = makeNode<StructDecl>();

    if (check(TokenKind::Identifier)) {
        decl->name = std::string(advance().stringValue);
    } else {
        error("expected struct name");
    }

    if (check(TokenKind::Less)) {
        decl->genericParams = parseGenericParams();
    }

    // Conformances: struct Foo: Protocol1, Protocol2
    if (match(TokenKind::Colon)) {
        do {
            decl->conformsTo.push_back(parseType());
        } while (match(TokenKind::Comma));
    }

    // Body
    if (expect(TokenKind::LBrace)) {
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            parseDeclarationInto(decl->members);
        }
        expect(TokenKind::RBrace);
    }

    return decl;
}

DeclPtr Parser::parseClassDecl() {
    expect(TokenKind::KwClass);
    auto decl = makeNode<ClassDecl>();

    if (check(TokenKind::Identifier)) {
        decl->name = std::string(advance().stringValue);
    } else {
        error("expected class name");
    }

    if (check(TokenKind::Less)) {
        decl->genericParams = parseGenericParams();
    }

    // Superclass and/or protocol conformances
    if (match(TokenKind::Colon)) {
        // First type is the superclass (classes have single inheritance)
        auto firstType = parseType();
        decl->superclass = std::move(firstType);
        // Additional conformances
        while (match(TokenKind::Comma)) {
            decl->conformsTo.push_back(parseType());
        }
    }

    // Body
    if (expect(TokenKind::LBrace)) {
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            parseDeclarationInto(decl->members);
        }
        expect(TokenKind::RBrace);
    }

    return decl;
}

DeclPtr Parser::parseEnumDecl() {
    expect(TokenKind::KwEnum);
    auto decl = makeNode<EnumDecl>();

    if (check(TokenKind::Identifier)) {
        decl->name = std::string(advance().stringValue);
    } else {
        error("expected enum name");
    }

    if (check(TokenKind::Less)) {
        decl->genericParams = parseGenericParams();
    }

    // Raw value type: enum Foo: Int
    if (match(TokenKind::Colon)) {
        decl->rawValueType = parseType();
    }

    // Body
    if (expect(TokenKind::LBrace)) {
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            if (match(TokenKind::KwCase)) {
                auto enumCase = makeNode<EnumCaseDecl>();
                if (check(TokenKind::Identifier)) {
                    enumCase->name = std::string(advance().stringValue);
                } else {
                    error("expected case name");
                }

                // Associated values: case foo(Int, String)
                if (check(TokenKind::LParen)) {
                    advance(); // (
                    do {
                        EnumCaseDecl::AssociatedValue av;
                        if (check(TokenKind::Identifier)) {
                            // Check if next is ':' (labeled) or type (unlabeled)
                            auto& next = peekAt(1);
                            if (next.is(TokenKind::Colon)) {
                                av.label = std::string(advance().stringValue);
                                advance(); // :
                            }
                        }
                        av.type = parseType();
                        enumCase->associatedValues.push_back(std::move(av));
                    } while (match(TokenKind::Comma));
                    expect(TokenKind::RParen);
                }

                // Raw value: case foo = 42
                if (match(TokenKind::Assign)) {
                    enumCase->rawValue = parseExpression();
                }

                decl->cases.push_back(std::move(enumCase));
            } else {
                parseDeclarationInto(decl->members);
            }
        }
        expect(TokenKind::RBrace);
    }

    return decl;
}

DeclPtr Parser::parseProtocolDecl() {
    expect(TokenKind::KwProtocol);
    auto decl = makeNode<ProtocolDecl>();

    if (check(TokenKind::Identifier)) {
        decl->name = std::string(advance().stringValue);
    } else {
        error("expected protocol name");
    }

    // Inheritance: protocol Foo: Bar, Baz
    if (match(TokenKind::Colon)) {
        do {
            decl->conformsTo.push_back(parseType());
        } while (match(TokenKind::Comma));
    }

    if (expect(TokenKind::LBrace)) {
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            parseDeclarationInto(decl->members);
        }
        expect(TokenKind::RBrace);
    }

    return decl;
}

DeclPtr Parser::parseActorDecl() {
    expect(TokenKind::KwActor);
    auto decl = makeNode<ActorDecl>();

    if (check(TokenKind::Identifier)) {
        decl->name = std::string(advance().stringValue);
    } else {
        error("expected actor name");
    }

    if (check(TokenKind::Less)) {
        decl->genericParams = parseGenericParams();
    }

    if (expect(TokenKind::LBrace)) {
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            parseDeclarationInto(decl->members);
        }
        expect(TokenKind::RBrace);
    }

    return decl;
}

DeclPtr Parser::parseExtensionDecl() {
    expect(TokenKind::KwExtension);
    auto decl = makeNode<ExtensionDecl>();

    decl->extendedType = parseType();

    if (match(TokenKind::Colon)) {
        do {
            decl->conformsTo.push_back(parseType());
        } while (match(TokenKind::Comma));
    }

    if (expect(TokenKind::LBrace)) {
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            parseDeclarationInto(decl->members);
        }
        expect(TokenKind::RBrace);
    }

    return decl;
}

DeclPtr Parser::parseTypealiasDecl() {
    expect(TokenKind::KwTypealias);
    auto decl = makeNode<TypealiasDecl>();

    if (check(TokenKind::Identifier)) {
        decl->name = std::string(advance().stringValue);
    } else {
        error("expected typealias name");
    }

    if (check(TokenKind::Less)) {
        decl->genericParams = parseGenericParams();
    }

    expect(TokenKind::Assign);
    decl->underlyingType = parseType();

    return decl;
}

DeclPtr Parser::parseInitDecl() {
    expect(TokenKind::KwInit);
    auto decl = makeNode<InitDecl>();

    if (expect(TokenKind::LParen)) {
        decl->params = parseParamList();
        expect(TokenKind::RParen);
    }

    if (check(TokenKind::LBrace)) {
        decl->body = parseBlock();
    }

    return decl;
}

DeclPtr Parser::parseDeinitDecl() {
    expect(TokenKind::KwDeinit);
    auto decl = makeNode<DeinitDecl>();

    if (check(TokenKind::LBrace)) {
        decl->body = parseBlock();
    }

    return decl;
}

DeclPtr Parser::parseSubscriptDecl() {
    expect(TokenKind::KwSubscript);
    auto decl = makeNode<SubscriptDecl>();

    if (expect(TokenKind::LParen)) {
        decl->params = parseParamList();
        expect(TokenKind::RParen);
    }

    if (match(TokenKind::Arrow)) {
        decl->returnType = parseType();
    }

    // Body: get/set or single expression
    if (expect(TokenKind::LBrace)) {
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            if (match(TokenKind::KwGet)) {
                decl->getterBody = parseBlock();
            } else if (match(TokenKind::KwSet)) {
                decl->setterBody = parseBlock();
            } else {
                error("expected 'get' or 'set' in subscript");
                advance();
            }
        }
        expect(TokenKind::RBrace);
    }

    return decl;
}

// ─── Control flow declarations ────────────────────────────────────────────

DeclPtr Parser::parseIfDecl() {
    expect(TokenKind::KwIf);
    auto decl = makeNode<IfDecl>();
    decl->condition = parseExpression();
    decl->thenBody = parseBlock();

    if (match(TokenKind::KwElse)) {
        if (check(TokenKind::KwIf)) {
            // else if — store as nested IfDecl
            decl->elseIfDecl = parseIfDecl();
        } else {
            decl->elseBody = parseBlock();
        }
    }
    return decl;
}

DeclPtr Parser::parseGuardDecl() {
    expect(TokenKind::KwGuard);
    auto decl = makeNode<GuardDecl>();
    decl->condition = parseExpression();
    expect(TokenKind::KwElse);
    decl->elseBody = parseBlock();
    return decl;
}

DeclPtr Parser::parseSwitchDecl() {
    expect(TokenKind::KwSwitch);
    auto decl = makeNode<SwitchDecl>();
    decl->subject = parseExpression();

    if (expect(TokenKind::LBrace)) {
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            SwitchCase sc;

            if (match(TokenKind::KwCase)) {
                // Parse case labels
                do {
                    SwitchCase::Label label;
                    // Try to parse as pattern first, fall back to expression
                    label.expression = parseExpression();
                    sc.labels.push_back(std::move(label));
                } while (match(TokenKind::Comma));
            } else if (match(TokenKind::KwDefault)) {
                SwitchCase::Label label;
                label.isDefault = true;
                sc.labels.push_back(std::move(label));
            } else {
                error("expected 'case' or 'default' in switch");
                advance();
                continue;
            }

            expect(TokenKind::Colon);

            // Parse case body (until next case/default/})
            while (!check(TokenKind::KwCase) && !check(TokenKind::KwDefault) &&
                   !check(TokenKind::RBrace) && !isAtEnd()) {
                auto stmt = parseStatement();
                if (stmt) sc.body.push_back(std::move(stmt));
            }

            // Check for fallthrough
            if (!sc.body.empty()) {
                auto& lastStmt = sc.body.back();
                if (lastStmt && lastStmt->stmtKind == StmtKind::Fallthrough) {
                    sc.isFallthrough = true;
                }
            }

            decl->cases.push_back(std::move(sc));
        }
        expect(TokenKind::RBrace);
    }

    return decl;
}

DeclPtr Parser::parseForInDecl() {
    expect(TokenKind::KwFor);
    auto decl = makeNode<ForInDecl>();
    decl->pattern = parsePattern();
    expect(TokenKind::KwIn);
    decl->sequence = parseExpression();

    // Optional where clause
    if (match(TokenKind::KwWhere)) {
        decl->whereClause = parseExpression();
    }

    decl->body = parseBlock();
    return decl;
}

DeclPtr Parser::parseWhileDecl() {
    expect(TokenKind::KwWhile);
    auto decl = makeNode<WhileDecl>();

    // Handle 'while let pattern = expr' (optional binding)
    if (check(TokenKind::KwLet) || check(TokenKind::KwVar)) {
        // Parse as a variable declaration used as condition
        // For now, parse the whole thing as an expression
        // TODO: proper while-let binding support
        advance(); // consume let/var
        if (check(TokenKind::Identifier)) advance(); // consume pattern name
        if (match(TokenKind::Assign)) {
            decl->condition = parseExpression();
        }
    } else {
        decl->condition = parseExpression();
    }

    decl->body = parseBlock();
    return decl;
}

DeclPtr Parser::parseRepeatWhileDecl() {
    expect(TokenKind::KwRepeat);
    auto decl = makeNode<RepeatWhileDecl>();
    decl->body = parseBlock();
    expect(TokenKind::KwWhile);
    decl->condition = parseExpression();
    return decl;
}

DeclPtr Parser::parseDoCatchDecl() {
    expect(TokenKind::KwDo);
    auto decl = makeNode<DoCatchDecl>();
    decl->doBody = parseBlock();

    while (match(TokenKind::KwCatch)) {
        DoCatchDecl::CatchClause cc;
        // Optional catch pattern: catch { ... }, catch let error { ... },
        // catch let error as FileError { ... }, catch is FileError { ... }
        if (!check(TokenKind::LBrace)) {
            cc.pattern = parsePattern();
            // Handle 'pattern as Type' in catch
            if (match(TokenKind::KwAs)) {
                auto asPat = makeNode<AsTypePattern>();
                asPat->subPattern = std::move(cc.pattern);
                asPat->type = parseType();
                cc.pattern = std::move(asPat);
            }
            if (match(TokenKind::KwWhere)) {
                cc.whereClause = parseExpression();
            }
        }
        cc.body = parseBlock();
        decl->catches.push_back(std::move(cc));
    }

    return decl;
}

DeclPtr Parser::parseSelectDecl() {
    expect(TokenKind::KwSelect);
    auto decl = makeNode<SelectDecl>();

    if (expect(TokenKind::LBrace)) {
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            SelectCase sc;

            if (match(TokenKind::KwDefault)) {
                // Standalone default: (without case prefix)
                sc.kind = SelectCase::Kind::Default;
                expect(TokenKind::Colon);
            } else if (match(TokenKind::KwCase)) {
                if (match(TokenKind::KwDefault)) {
                    sc.kind = SelectCase::Kind::Default;
                    expect(TokenKind::Colon);
                } else {
                    // Check if this is send or receive
                    // send: case value <- channel
                    // receive: case let x <- channel (or case x <- channel)
                    sc.recvPattern = parsePattern();
                    if (expect(TokenKind::LeftArrow)) {
                        sc.channel = parseExpression();
                    }
                    expect(TokenKind::Colon);

                    // Determine send vs receive by pattern type
                    if (sc.recvPattern && sc.recvPattern->patternKind == PatternKind::Identifier) {
                        auto* ip = static_cast<IdentifierPattern*>(sc.recvPattern.get());
                        if (ip->name.empty()) {
                            sc.kind = SelectCase::Kind::Send;
                        } else {
                            sc.kind = SelectCase::Kind::Receive;
                        }
                    } else {
                        sc.kind = SelectCase::Kind::Receive;
                    }
                }
            } else {
                error("expected 'case' or 'default' in select");
                advance();
                continue;
            }

            // Parse case body
            while (!check(TokenKind::KwCase) && !check(TokenKind::RBrace) && !isAtEnd()) {
                auto stmt = parseStatement();
                if (stmt) sc.body.push_back(std::move(stmt));
            }

            decl->cases.push_back(std::move(sc));
        }
        expect(TokenKind::RBrace);
    }

    return decl;
}

DeclPtr Parser::parseUnsafeDecl() {
    expect(TokenKind::KwUnsafe);
    auto decl = makeNode<UnsafeDecl>();
    decl->body = parseBlock();
    return decl;
}

DeclPtr Parser::parseAsmDecl() {
    expect(TokenKind::KwAsm);
    auto decl = makeNode<AsmDecl>();

    // asm("assembly template" : outputs : inputs : clobbers)
    if (expect(TokenKind::LParen)) {
        // 解析汇编模板字符串
        if (check(TokenKind::StringLiteral)) {
            decl->assembly = std::string(advance().stringValue);
        } else {
            error("expected string literal in asm");
        }

        // 可选的约束部分 : outputs : inputs : clobbers
        while (match(TokenKind::Colon)) {
            // 跳过约束字符串
            if (check(TokenKind::StringLiteral)) {
                decl->constraints += std::string(advance().stringValue);
            }
        }

        expect(TokenKind::RParen);
    }

    return decl;
}

// ─── Statements ───────────────────────────────────────────────────────────

StmtPtr Parser::parseStatement() {
    switch (peek().kind) {
        case TokenKind::KwReturn:   return parseReturnStmt();
        case TokenKind::KwBreak:    return parseBreakStmt();
        case TokenKind::KwContinue: return parseContinueStmt();
        case TokenKind::KwFallthrough: return parseFallthroughStmt();
        case TokenKind::KwDefer:    return parseDeferStmt();
        case TokenKind::KwThrow:    return parseThrowStmt();
        case TokenKind::KwLet:
        case TokenKind::KwVar: {
            auto varDecl = parseVariableDecl();
            if (varDecl) {
                auto stmt = makeNode<VariableDeclStmt>();
                stmt->varDecl = std::move(varDecl);
                return stmt;
            }
            return nullptr;
        }
        // Control flow declarations used as statements
        case TokenKind::KwIf:
        case TokenKind::KwGuard:
        case TokenKind::KwSwitch:
        case TokenKind::KwFor:
        case TokenKind::KwWhile:
        case TokenKind::KwRepeat:
        case TokenKind::KwDo:
        case TokenKind::KwSelect:
        case TokenKind::KwUnsafe:
        case TokenKind::KwAsm: {
            auto decl = parseDeclaration();
            if (decl) {
                auto stmt = makeNode<DeclStmt>();
                stmt->decl = std::move(decl);
                return stmt;
            }
            return nullptr;
        }
        default: {
            // Try expression statement
            ExprPtr expr = parseExpression();
            if (expr) {
                auto stmt = makeNode<ExpressionStmt>();
                stmt->expression = std::move(expr);
                return stmt;
            }
            error("expected statement");
            advance();
            return nullptr;
        }
    }
}

StmtPtr Parser::parseReturnStmt() {
    expect(TokenKind::KwReturn);
    auto stmt = makeNode<ReturnStmt>();
    // Return value is optional (return vs return expr)
    if (!check(TokenKind::RBrace) && !isAtEnd() &&
        !peek().isOneOf({TokenKind::KwCase, TokenKind::KwDefault, TokenKind::KwElse})) {
        stmt->value = parseExpression();
    }
    return stmt;
}

StmtPtr Parser::parseBreakStmt() {
    expect(TokenKind::KwBreak);
    return makeNode<BreakStmt>();
}

StmtPtr Parser::parseContinueStmt() {
    expect(TokenKind::KwContinue);
    return makeNode<ContinueStmt>();
}

StmtPtr Parser::parseFallthroughStmt() {
    expect(TokenKind::KwFallthrough);
    return makeNode<FallthroughStmt>();
}

StmtPtr Parser::parseDeferStmt() {
    expect(TokenKind::KwDefer);
    auto stmt = makeNode<DeferStmt>();
    stmt->body = parseBlock();
    return stmt;
}

StmtPtr Parser::parseThrowStmt() {
    expect(TokenKind::KwThrow);
    auto stmt = makeNode<ThrowStmt>();
    stmt->value = parseExpression();
    return stmt;
}

std::vector<StmtPtr> Parser::parseBlock() {
    std::vector<StmtPtr> stmts;
    if (expect(TokenKind::LBrace)) {
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            auto stmt = parseStatement();
            if (stmt) stmts.push_back(std::move(stmt));
        }
        expect(TokenKind::RBrace);
    }
    return stmts;
}

// ─── Expression parsing (precedence climbing) ─────────────────────────────

ExprPtr Parser::parseExpression() {
    return parseAssignmentExpr();
}

ExprPtr Parser::parseAssignmentExpr() {
    ExprPtr left = parseRangeExpr();

    // Assignment operators
    if (peek().isAssignmentOperator()) {
        TokenKind op = advance().kind;
        ExprPtr right = parseAssignmentExpr();
        auto expr = makeNode<AssignmentExpr>();
        expr->target = std::move(left);
        expr->value = std::move(right);
        return expr;
    }

    return left;
}

ExprPtr Parser::parseTernaryExpr() {
    ExprPtr cond = parseLogicalOrExpr();

    // No ternary operator in SukiCode (use if expressions instead)
    return cond;
}

ExprPtr Parser::parseLogicalOrExpr() {
    ExprPtr left = parseLogicalAndExpr();

    while (match(TokenKind::PipePipe) || match(TokenKind::KwOr)) {
        TokenKind op = tokens_[pos_ - 1].kind;
        ExprPtr right = parseLogicalAndExpr();
        auto expr = makeNode<BinaryExpr>();
        expr->op = op;
        expr->left = std::move(left);
        expr->right = std::move(right);
        left = std::move(expr);
    }

    return left;
}

ExprPtr Parser::parseLogicalAndExpr() {
    ExprPtr left = parseBitwiseOrExpr();

    while (match(TokenKind::AmpAmp) || match(TokenKind::KwAnd)) {
        TokenKind op = tokens_[pos_ - 1].kind;
        ExprPtr right = parseBitwiseOrExpr();
        auto expr = makeNode<BinaryExpr>();
        expr->op = op;
        expr->left = std::move(left);
        expr->right = std::move(right);
        left = std::move(expr);
    }

    return left;
}

ExprPtr Parser::parseBitwiseOrExpr() {
    ExprPtr left = parseBitwiseXorExpr();

    while (match(TokenKind::Pipe)) {
        ExprPtr right = parseBitwiseXorExpr();
        auto expr = makeNode<BinaryExpr>();
        expr->op = TokenKind::Pipe;
        expr->left = std::move(left);
        expr->right = std::move(right);
        left = std::move(expr);
    }

    return left;
}

ExprPtr Parser::parseBitwiseXorExpr() {
    ExprPtr left = parseBitwiseAndExpr();

    while (match(TokenKind::Caret)) {
        ExprPtr right = parseBitwiseAndExpr();
        auto expr = makeNode<BinaryExpr>();
        expr->op = TokenKind::Caret;
        expr->left = std::move(left);
        expr->right = std::move(right);
        left = std::move(expr);
    }

    return left;
}

ExprPtr Parser::parseBitwiseAndExpr() {
    ExprPtr left = parseComparisonExpr();

    while (match(TokenKind::Amp)) {
        ExprPtr right = parseComparisonExpr();
        auto expr = makeNode<BinaryExpr>();
        expr->op = TokenKind::Amp;
        expr->left = std::move(left);
        expr->right = std::move(right);
        left = std::move(expr);
    }

    return left;
}

ExprPtr Parser::parseComparisonExpr() {
    ExprPtr left = parseShiftExpr();

    while (true) {
        TokenKind op;
        if (match(TokenKind::Equal))         op = TokenKind::Equal;
        else if (match(TokenKind::NotEqual)) op = TokenKind::NotEqual;
        else if (match(TokenKind::Less))     op = TokenKind::Less;
        else if (match(TokenKind::Greater))  op = TokenKind::Greater;
        else if (match(TokenKind::LessEqual))    op = TokenKind::LessEqual;
        else if (match(TokenKind::GreaterEqual)) op = TokenKind::GreaterEqual;
        else break;

        ExprPtr right = parseShiftExpr();
        auto expr = makeNode<BinaryExpr>();
        expr->op = op;
        expr->left = std::move(left);
        expr->right = std::move(right);
        left = std::move(expr);
    }

    return left;
}

ExprPtr Parser::parseRangeExpr() {
    ExprPtr left = parseLogicalOrExpr();

    // Range operators: ..< (half-open), ... (closed)
    // Lower precedence than logical or, higher than assignment
    while (true) {
        TokenKind op;
        if (match(TokenKind::Range))     op = TokenKind::Range;     // ..<
        else if (match(TokenKind::Ellipsis)) op = TokenKind::Ellipsis; // ...
        else break;

        ExprPtr right = parseLogicalOrExpr();
        auto expr = makeNode<BinaryExpr>();
        expr->op = op;
        expr->left = std::move(left);
        expr->right = std::move(right);
        left = std::move(expr);
    }

    return left;
}

ExprPtr Parser::parseShiftExpr() {
    ExprPtr left = parseAdditionExpr();

    while (true) {
        TokenKind op;
        if (match(TokenKind::LShift))      op = TokenKind::LShift;
        else if (match(TokenKind::RShift)) op = TokenKind::RShift;
        else break;

        ExprPtr right = parseAdditionExpr();
        auto expr = makeNode<BinaryExpr>();
        expr->op = op;
        expr->left = std::move(left);
        expr->right = std::move(right);
        left = std::move(expr);
    }

    return left;
}

ExprPtr Parser::parseAdditionExpr() {
    ExprPtr left = parseMultiplicationExpr();

    while (true) {
        TokenKind op;
        if (match(TokenKind::Plus))       op = TokenKind::Plus;
        else if (match(TokenKind::Minus)) op = TokenKind::Minus;
        else break;

        ExprPtr right = parseMultiplicationExpr();
        auto expr = makeNode<BinaryExpr>();
        expr->op = op;
        expr->left = std::move(left);
        expr->right = std::move(right);
        left = std::move(expr);
    }

    return left;
}

ExprPtr Parser::parseMultiplicationExpr() {
    ExprPtr left = parsePrefixExpr();

    while (true) {
        TokenKind op;
        if (match(TokenKind::Star))        op = TokenKind::Star;
        else if (match(TokenKind::Slash))  op = TokenKind::Slash;
        else if (match(TokenKind::Percent)) op = TokenKind::Percent;
        else break;

        ExprPtr right = parsePrefixExpr();
        auto expr = makeNode<BinaryExpr>();
        expr->op = op;
        expr->left = std::move(left);
        expr->right = std::move(right);
        left = std::move(expr);
    }

    return left;
}

ExprPtr Parser::parsePrefixExpr() {
    // Prefix operators: -, !, ~, try, await, move, unsafe
    if (match(TokenKind::Minus)) {
        auto expr = makeNode<UnaryExpr>();
        expr->op = TokenKind::Minus;
        expr->isPrefix = true;
        expr->operand = parsePrefixExpr();
        return expr;
    }
    if (match(TokenKind::Bang)) {
        auto expr = makeNode<UnaryExpr>();
        expr->op = TokenKind::Bang;
        expr->isPrefix = true;
        expr->operand = parsePrefixExpr();
        return expr;
    }
    if (match(TokenKind::Tilde)) {
        auto expr = makeNode<UnaryExpr>();
        expr->op = TokenKind::Tilde;
        expr->isPrefix = true;
        expr->operand = parsePrefixExpr();
        return expr;
    }
    if (match(TokenKind::KwTry)) {
        auto expr = makeNode<TryExpr>();
        if (match(TokenKind::Bang)) expr->isForce = true;
        else if (match(TokenKind::Question)) expr->isOptional = true;
        expr->subExpr = parsePrefixExpr();
        return expr;
    }
    if (match(TokenKind::KwAwait)) {
        auto expr = makeNode<AwaitExpr>();
        expr->subExpr = parsePrefixExpr();
        return expr;
    }
    if (match(TokenKind::KwMove)) {
        auto expr = makeNode<MoveExpr>();
        expr->subExpr = parsePrefixExpr();
        return expr;
    }
    if (match(TokenKind::Amp)) {
        auto expr = makeNode<InOutExpr>();
        expr->subExpr = parsePrefixExpr();
        return expr;
    }

    return parsePostfixExpr();
}

ExprPtr Parser::parsePostfixExpr() {
    ExprPtr expr = parsePrimaryExpr();
    if (!expr) return nullptr;

    while (true) {
        if (check(TokenKind::LParen)) {
            expr = parseCallExpr(std::move(expr));
            // Trailing closure: call() { params in body }
            if (check(TokenKind::LBrace)) {
                auto closure = makeNode<ClosureExpr>();
                advance(); // {
                // Parse optional parameters before 'in'
                if (!check(TokenKind::KwIn) && !check(TokenKind::RBrace)) {
                    // Could be params: { x in ... } or { (a, b) in ... }
                    if (check(TokenKind::Identifier)) {
                        // Simple param: { x in ... }
                        ClosureExpr::Param param;
                        param.name = std::string(advance().stringValue);
                        closure->params.push_back(std::move(param));
                        // Check for more params
                        while (match(TokenKind::Comma) && check(TokenKind::Identifier)) {
                            ClosureExpr::Param p;
                            p.name = std::string(advance().stringValue);
                            closure->params.push_back(std::move(p));
                        }
                    }
                }
                match(TokenKind::KwIn); // consume 'in' if present
                // Parse body
                while (!check(TokenKind::RBrace) && !isAtEnd()) {
                    auto stmt = parseStatement();
                    if (stmt) closure->body.push_back(std::move(stmt));
                }
                match(TokenKind::RBrace); // }
                // Add as last argument
                if (expr->exprKind == ExprKind::Call) {
                    auto& call = static_cast<CallExpr&>(*expr);
                    CallExpr::Arg arg;
                    arg.value = std::move(closure);
                    call.args.push_back(std::move(arg));
                }
            }
        } else if (check(TokenKind::Dot) || check(TokenKind::Question)) {
            bool isOptional = match(TokenKind::Question);
            if (match(TokenKind::Dot)) {
                // Member name can be an identifier or a keyword (init, deinit, self, etc.)
                if (check(TokenKind::Identifier) || peek().isKeyword()) {
                    auto member = makeNode<MemberAccessExpr>();
                    member->base = std::move(expr);
                    member->member = std::string(advance().stringValue);
                    member->isOptionalChain = isOptional;
                    expr = std::move(member);
                } else {
                    error("expected member name after '.'");
                    break;
                }
            } else if (isOptional) {
                // ?. optional chain
                auto chain = makeNode<OptionalChainExpr>();
                chain->subExpr = std::move(expr);
                expr = std::move(chain);
            } else {
                break;
            }
        } else if (check(TokenKind::LBracket)) {
            expr = parseSubscriptExpr(std::move(expr));
        } else if (match(TokenKind::Bang)) {
            auto unwrap = makeNode<ForceUnwrapExpr>();
            unwrap->subExpr = std::move(expr);
            expr = std::move(unwrap);
        } else if (match(TokenKind::QuestionQuestion)) {
            // Null coalescing: expr ?? default
            ExprPtr rhs = parsePrefixExpr();
            auto binary = makeNode<BinaryExpr>();
            binary->op = TokenKind::QuestionQuestion;
            binary->left = std::move(expr);
            binary->right = std::move(rhs);
            expr = std::move(binary);
        } else if (match(TokenKind::KwIs)) {
            // Type check: expr is Type
            auto check = makeNode<TypeCheckExpr>();
            check->subExpr = std::move(expr);
            check->checkType = parseType();
            expr = std::move(check);
        } else if (check(TokenKind::KwAs)) {
            // Type cast: expr as Type, expr as? Type, expr as! Type
            advance(); // consume 'as'
            auto cast = makeNode<TypeCastExpr>();
            cast->subExpr = std::move(expr);
            if (match(TokenKind::Question)) {
                cast->castKind = CastKind::Conditional;
            } else if (match(TokenKind::Bang)) {
                cast->castKind = CastKind::Force;
            } else {
                cast->castKind = CastKind::Coerce;
            }
            cast->targetType = parseType();
            expr = std::move(cast);
        } else {
            break;
        }
    }

    return expr;
}

ExprPtr Parser::parsePrimaryExpr() {
    switch (peek().kind) {
        // Literals
        case TokenKind::IntegerLiteral: {
            auto expr = makeNode<IntegerLiteralExpr>();
            expr->value = advance().literal.intValue;
            return expr;
        }
        case TokenKind::FloatLiteral: {
            auto expr = makeNode<FloatLiteralExpr>();
            expr->value = advance().literal.floatValue;
            return expr;
        }
        case TokenKind::StringLiteral: {
            std::string strValue = std::string(advance().stringValue);
            // 检查是否包含字符串插值 \(expr)
            size_t interpPos = strValue.find("\\(");
            if (interpPos == std::string::npos) {
                // 普通字符串
                auto expr = makeNode<StringLiteralExpr>();
                expr->value = strValue;
                return expr;
            }
            // 字符串插值：拆分为片段
            auto interpExpr = makeNode<InterpolatedStringExpr>();
            size_t pos = 0;
            while (pos < strValue.size()) {
                size_t found = strValue.find("\\(", pos);
                if (found == std::string::npos) {
                    // 剩余部分是字面文本
                    InterpolatedStringExpr::Segment seg;
                    seg.literalText = strValue.substr(pos);
                    interpExpr->segments.push_back(std::move(seg));
                    break;
                }
                // \( 之前的字面文本
                if (found > pos) {
                    InterpolatedStringExpr::Segment seg;
                    seg.literalText = strValue.substr(pos, found - pos);
                    interpExpr->segments.push_back(std::move(seg));
                }
                // 解析 \(expr) 中的表达式
                // 查找匹配的 )
                size_t exprStart = found + 2;
                int depth = 1;
                size_t exprEnd = exprStart;
                while (exprEnd < strValue.size() && depth > 0) {
                    if (strValue[exprEnd] == '(') depth++;
                    else if (strValue[exprEnd] == ')') depth--;
                    if (depth > 0) exprEnd++;
                }
                std::string exprStr = strValue.substr(exprStart, exprEnd - exprStart);
                // 创建临时词法分析器解析表达式
                DiagnosticEngine tempDiag;
                Lexer tempLexer(exprStr, "interpolation", tempDiag);
                auto tempTokens = tempLexer.lexAll();
                Parser tempParser(std::move(tempTokens), exprStr, "interpolation", tempDiag);
                auto parsedExpr = tempParser.parseExpression();
                InterpolatedStringExpr::Segment seg;
                seg.expression = std::move(parsedExpr);
                interpExpr->segments.push_back(std::move(seg));
                pos = exprEnd + 1;
            }
            return interpExpr;
        }
        case TokenKind::CharLiteral: {
            auto expr = makeNode<CharLiteralExpr>();
            expr->value = advance().literal.charValue;
            return expr;
        }
        case TokenKind::True: {
            advance();
            auto expr = makeNode<BoolLiteralExpr>();
            expr->value = true;
            return expr;
        }
        case TokenKind::False: {
            advance();
            auto expr = makeNode<BoolLiteralExpr>();
            expr->value = false;
            return expr;
        }
        case TokenKind::Nil: {
            advance();
            return makeNode<NilLiteralExpr>();
        }

        // Identifier
        case TokenKind::Identifier: {
            auto expr = makeNode<IdentifierExpr>();
            expr->name = std::string(advance().stringValue);
            return expr;
        }

        // self / super / Self
        case TokenKind::KwSelf: {
            advance();
            return makeNode<SelfRefExpr>();
        }
        case TokenKind::KwSuper: {
            advance();
            return makeNode<SuperRefExpr>();
        }
        case TokenKind::KwSelfType: {
            auto expr = makeNode<IdentifierExpr>();
            expr->name = "Self";
            advance();
            return expr;
        }

        // Parenthesized expression, tuple, or arrow closure
        case TokenKind::LParen: {
            // Lookahead: check if this is an arrow closure (...)=> expr
            // Only detect by checking for '=>' AFTER the closing ')'
            bool isArrowClosure = false;
            {
                size_t save = pos_;
                advance(); // (
                // Skip to matching ')'
                int depth = 1;
                while (pos_ < tokens_.size() && depth > 0) {
                    if (tokens_[pos_].is(TokenKind::LParen)) depth++;
                    else if (tokens_[pos_].is(TokenKind::RParen)) depth--;
                    if (depth > 0) pos_++;
                }
                // pos_ is now at the matching ')'
                // Check if next token after ')' is '=>'
                if (pos_ + 1 < tokens_.size() && tokens_[pos_ + 1].is(TokenKind::FatArrow)) {
                    isArrowClosure = true;
                }
                pos_ = save; // restore
            }

            if (isArrowClosure) {
                // Parse as arrow closure: (params) => expr
                advance(); // (
                auto closure = makeNode<ClosureExpr>();
                closure->isArrow = true;
                if (!check(TokenKind::RParen)) {
                    do {
                        ClosureExpr::Param param;
                        if (check(TokenKind::Identifier)) {
                            param.name = std::string(advance().stringValue);
                        }
                        if (match(TokenKind::Colon)) {
                            param.type = parseType();
                        }
                        closure->params.push_back(std::move(param));
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RParen);
                expect(TokenKind::FatArrow);
                // Parse body expression
                auto bodyExpr = parseExpression();
                if (bodyExpr) {
                    auto retStmt = makeNode<ReturnStmt>();
                    retStmt->value = std::move(bodyExpr);
                    closure->body.push_back(std::move(retStmt));
                }
                return closure;
            }

            advance(); // (
            if (check(TokenKind::RParen)) {
                advance(); // empty tuple ()
                auto expr = makeNode<TupleExpr>();
                return expr;
            }
            ExprPtr inner = parseExpression();
            if (match(TokenKind::Comma)) {
                // Tuple
                auto tuple = makeNode<TupleExpr>();
                TupleExpr::Element first;
                first.value = std::move(inner);
                tuple->elements.push_back(std::move(first));
                do {
                    TupleExpr::Element elem;
                    elem.value = parseExpression();
                    tuple->elements.push_back(std::move(elem));
                } while (match(TokenKind::Comma));
                expect(TokenKind::RParen);
                return tuple;
            }
            expect(TokenKind::RParen);
            return inner;
        }

        // Array literal [1, 2, 3] or Dictionary literal ["key": "value"]
        case TokenKind::LBracket: {
            advance(); // [
            if (check(TokenKind::RBracket)) {
                advance(); // empty []
                return makeNode<ArrayLiteralExpr>();
            }
            // Parse first expression
            ExprPtr first = parseExpression();
            // Check if it's a dictionary: [key: value, ...]
            if (match(TokenKind::Colon)) {
                auto dict = makeNode<DictLiteralExpr>();
                DictLiteralExpr::Entry entry;
                entry.key = std::move(first);
                entry.value = parseExpression();
                dict->entries.push_back(std::move(entry));
                while (match(TokenKind::Comma)) {
                    DictLiteralExpr::Entry e;
                    e.key = parseExpression();
                    expect(TokenKind::Colon);
                    e.value = parseExpression();
                    dict->entries.push_back(std::move(e));
                }
                expect(TokenKind::RBracket);
                return dict;
            }
            // Array literal
            auto arr = makeNode<ArrayLiteralExpr>();
            arr->elements.push_back(std::move(first));
            while (match(TokenKind::Comma)) {
                arr->elements.push_back(parseExpression());
            }
            expect(TokenKind::RBracket);
            return arr;
        }

        // Closure: { params -> return in body } or () => expr
        // Set literal: {1, 2, 3} — distinguished by context
        case TokenKind::LBrace: {
            // Heuristic: if { is followed by a literal, number, or string,
            // it's likely a set literal, not a closure. Return nullptr to let
            // the caller handle it as an expression statement.
            if (peekAt(1).isOneOf({
                TokenKind::IntegerLiteral, TokenKind::FloatLiteral,
                TokenKind::StringLiteral, TokenKind::CharLiteral,
                TokenKind::True, TokenKind::False, TokenKind::Nil})) {
                return nullptr; // caller will report error
            }

            auto closure = makeNode<ClosureExpr>();
            advance(); // {

            // Capture list: [weak self, unowned delegate]
            if (check(TokenKind::LBracket)) {
                advance(); // [
                do {
                    if (check(TokenKind::Identifier)) {
                        std::string capture = std::string(advance().stringValue);
                        if (capture == "weak" || capture == "unowned") {
                            if (check(TokenKind::Identifier)) {
                                capture += " " + std::string(advance().stringValue);
                            }
                        }
                        closure->captureList.push_back(capture);
                    }
                } while (match(TokenKind::Comma));
                expect(TokenKind::RBracket);
            }

            // Parameters: (a, b) or a, b in
            if (check(TokenKind::LParen)) {
                advance(); // (
                do {
                    ClosureExpr::Param param;
                    if (check(TokenKind::Identifier)) {
                        param.name = std::string(advance().stringValue);
                    }
                    if (match(TokenKind::Colon)) {
                        param.type = parseType();
                    }
                    closure->params.push_back(std::move(param));
                } while (match(TokenKind::Comma));
                expect(TokenKind::RParen);
            }

            // Return type
            if (match(TokenKind::Arrow)) {
                closure->returnType = parseType();
            }

            // 'in' keyword separates params from body
            match(TokenKind::KwIn);

            // Body
            while (!check(TokenKind::RBrace) && !isAtEnd()) {
                auto stmt = parseStatement();
                if (stmt) closure->body.push_back(std::move(stmt));
            }
            expect(TokenKind::RBrace);
            return closure;
        }

        // if expression
        case TokenKind::KwIf: {
            advance(); // if
            auto expr = makeNode<IfExpr>();
            expr->condition = parseExpression();
            // if x > 0 { expr } else { expr }
            if (expect(TokenKind::LBrace)) {
                expr->thenExpr = parseExpression();
                expect(TokenKind::RBrace);
            }
            if (match(TokenKind::KwElse)) {
                if (expect(TokenKind::LBrace)) {
                    expr->elseExpr = parseExpression();
                    expect(TokenKind::RBrace);
                }
            }
            return expr;
        }

        // Type cast: expr as Type, expr as? Type, expr as! Type
        case TokenKind::KwAs: {
            // This shouldn't happen in prefix position; handled in postfix
            break;
        }

        // Selector: #selector(method)
        case TokenKind::Hash: {
            advance(); // #
            if (check(TokenKind::Identifier)) {
                auto& name = advance().stringValue;
                if (name == "selector") {
                    if (expect(TokenKind::LParen)) {
                        // For now, parse the selector argument as an expression
                        auto inner = parseExpression();
                        expect(TokenKind::RParen);
                        // TODO: create proper SelectorExpr
                        return inner;
                    }
                }
            }
            error("expected '#selector(...)'");
            return nullptr;
        }

        default:
            break;
    }

    return nullptr;
}

// ─── Call / Member / Subscript ────────────────────────────────────────────

ExprPtr Parser::parseCallExpr(ExprPtr callee) {
    expect(TokenKind::LParen);
    auto expr = makeNode<CallExpr>();
    expr->callee = std::move(callee);

    if (!check(TokenKind::RParen)) {
        do {
            CallExpr::Arg arg;
            // Check for labeled argument: label: expr
            if (check(TokenKind::Identifier) && peekAt(1).is(TokenKind::Colon)) {
                arg.label = std::string(advance().stringValue);
                advance(); // :
            }
            arg.value = parseExpression();
            expr->args.push_back(std::move(arg));
        } while (match(TokenKind::Comma));
    }

    expect(TokenKind::RParen);
    return expr;
}

ExprPtr Parser::parseMemberAccessExpr(ExprPtr base) {
    auto expr = makeNode<MemberAccessExpr>();
    expr->base = std::move(base);
    if (check(TokenKind::Identifier)) {
        expr->member = std::string(advance().stringValue);
    } else {
        error("expected member name");
    }
    return expr;
}

ExprPtr Parser::parseSubscriptExpr(ExprPtr base) {
    expect(TokenKind::LBracket);
    auto expr = makeNode<SubscriptExpr>();
    expr->base = std::move(base);

    do {
        expr->indices.push_back(parseExpression());
    } while (match(TokenKind::Comma));

    expect(TokenKind::RBracket);
    return expr;
}

// ─── Type parsing ─────────────────────────────────────────────────────────

TypeReprPtr Parser::parseType() {
    // Check for function type first: (A, B) -> C
    if (check(TokenKind::LParen)) {
        auto& next = peekAt(1);
        if (next.is(TokenKind::RParen) || next.is(TokenKind::Identifier) ||
            next.is(TokenKind::KwSelfType)) {
            // Could be function type or tuple type — try function type
            // if we see -> after the closing paren
            size_t savePos = pos_;
            // Quick lookahead for ->
            int depth = 1;
            size_t i = pos_ + 1;
            while (i < tokens_.size() && depth > 0) {
                if (tokens_[i].is(TokenKind::LParen)) depth++;
                else if (tokens_[i].is(TokenKind::RParen)) depth--;
                i++;
            }
            if (i < tokens_.size() && tokens_[i].is(TokenKind::Arrow)) {
                return parseFunctionType();
            }
            pos_ = savePos; // restore
        }
    }

    return parseSimpleType();
}

TypeReprPtr Parser::parseSimpleType() {
    // 'some' opaque type
    if (match(TokenKind::KwSome)) {
        auto type = makeNode<OpaqueTypeRepr>();
        type->constraint = parseSimpleType();
        return type;
    }

    // 'any' existential type
    if (match(TokenKind::KwAny)) {
        auto type = makeNode<ExistentialTypeRepr>();
        type->constraint = parseSimpleType();
        return type;
    }

    // Self type
    if (match(TokenKind::KwSelfType)) {
        return makeNode<SelfTypeRepr>();
    }

    // Named type: Identifier, Identifier<T, U>
    if (check(TokenKind::Identifier)) {
        auto namedType = makeNode<NamedTypeRepr>();
        namedType->name = std::string(advance().stringValue);

        // Module prefix: Module.Type
        while (match(TokenKind::Dot)) {
            if (check(TokenKind::Identifier)) {
                namedType->name += ".";
                namedType->name += advance().stringValue;
            }
        }

        // Generic arguments
        if (match(TokenKind::Less)) {
            do {
                namedType->genericArgs.push_back(parseType());
            } while (match(TokenKind::Comma));
            expect(TokenKind::Greater);
        }

        TypeReprPtr type = std::move(namedType);

        // Postfix: [] for Array, [:] for Dict, ? for Optional
        while (true) {
            if (check(TokenKind::LBracket)) {
                advance(); // [
                if (match(TokenKind::Colon)) {
                    // Dictionary: [Key: Value]
                    expect(TokenKind::RBracket);
                    auto dictType = makeNode<DictTypeRepr>();
                    dictType->keyType = std::move(type);
                    dictType->valueType = parseType();
                    type = std::move(dictType);
                } else if (match(TokenKind::RBracket)) {
                    // Array: [Element]
                    auto arrType = makeNode<ArrayTypeRepr>();
                    arrType->elementType = std::move(type);
                    type = std::move(arrType);
                } else {
                    // Shouldn't happen in type context
                    break;
                }
            } else if (match(TokenKind::Question)) {
                auto optType = makeNode<OptionalTypeRepr>();
                optType->base = std::move(type);
                type = std::move(optType);
            } else {
                break;
            }
        }

        return type;
    }

    // Array type: [T]
    if (check(TokenKind::LBracket)) {
        advance(); // [
        auto elemType = parseType();
        if (match(TokenKind::Colon)) {
            auto valType = parseType();
            expect(TokenKind::RBracket);
            auto dictType = makeNode<DictTypeRepr>();
            dictType->keyType = std::move(elemType);
            dictType->valueType = std::move(valType);
            return dictType;
        }
        expect(TokenKind::RBracket);
        auto arrType = makeNode<ArrayTypeRepr>();
        arrType->elementType = std::move(elemType);
        return arrType;
    }

    error("expected type");
    return nullptr;
}

TypeReprPtr Parser::parseFunctionType() {
    auto type = makeNode<FunctionTypeRepr>();

    expect(TokenKind::LParen);
    if (!check(TokenKind::RParen)) {
        do {
            FunctionTypeRepr::Param param;
            param.type = parseType();
            type->params.push_back(std::move(param));
        } while (match(TokenKind::Comma));
    }
    expect(TokenKind::RParen);

    // async/throws
    while (true) {
        if (match(TokenKind::KwAsync)) { type->isAsync = true; continue; }
        if (match(TokenKind::KwThrows)) { type->isThrows = true; continue; }
        break;
    }

    expect(TokenKind::Arrow);
    type->returnType = parseType();

    return type;
}

// ─── Pattern parsing ──────────────────────────────────────────────────────

PatternPtr Parser::parsePattern() {
    // Wildcard
    if (match(TokenKind::Underscore)) {
        return makeNode<WildcardPattern>();
    }

    // let/var binding
    bool isLet = true;
    if (match(TokenKind::KwLet)) isLet = true;
    else if (match(TokenKind::KwVar)) isLet = false;

    // Identifier pattern
    if (check(TokenKind::Identifier)) {
        auto pat = makeNode<IdentifierPattern>();
        pat->name = std::string(advance().stringValue);
        pat->isLet = isLet;
        return pat;
    }

    // Enum case pattern: .caseName
    if (check(TokenKind::Dot) && peekAt(1).is(TokenKind::Identifier)) {
        advance(); // .
        auto pat = makeNode<EnumCasePattern>();
        pat->caseName = std::string(advance().stringValue);

        // Associated values: .case(let x, let y)
        if (check(TokenKind::LParen)) {
            advance();
            do {
                pat->associatedPatterns.push_back(parsePattern());
            } while (match(TokenKind::Comma));
            expect(TokenKind::RParen);
        }
        return pat;
    }

    // Tuple pattern: (a, b, c)
    if (check(TokenKind::LParen)) {
        advance();
        auto pat = makeNode<TuplePattern>();
        if (!check(TokenKind::RParen)) {
            do {
                pat->elements.push_back(parsePattern());
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen);
        return pat;
    }

    // Fallback: try expression pattern
    auto expr = parseExpression();
    if (expr) {
        // Wrap as an expression pattern
        auto pat = makeNode<IdentifierPattern>();
        pat->name = "_expr_"; // TODO: proper expression pattern
        return pat;
    }

    error("expected pattern");
    return makeNode<WildcardPattern>();
}

// ─── Function parameters ──────────────────────────────────────────────────

FunctionParam Parser::parseFunctionParam() {
    FunctionParam param;

    // External label (optional): externalName internalName: Type
    if (check(TokenKind::Identifier)) {
        // First identifier is the external label
        param.externalLabel = std::string(advance().stringValue);

        // Second identifier (if present) is the internal name
        if (check(TokenKind::Identifier)) {
            param.internalName = std::string(advance().stringValue);
        } else {
            // No internal name — external label IS the internal name
            // Unless external label is "_" (unlabeled)
            if (param.externalLabel == "_") {
                param.internalName = "_";
            } else {
                param.internalName = param.externalLabel;
            }
        }
    } else if (match(TokenKind::Underscore)) {
        // Unlabeled parameter: _ name: Type
        param.externalLabel = "_";
        if (check(TokenKind::Identifier)) {
            param.internalName = std::string(advance().stringValue);
        }
    }

    expect(TokenKind::Colon);

    // inout modifier
    if (match(TokenKind::KwInOut)) {
        param.isInOut = true;
    }

    param.type = parseType();

    // Default value
    if (match(TokenKind::Assign)) {
        param.defaultValue = parseExpression();
    }

    // Variadic
    if (match(TokenKind::Ellipsis)) {
        param.isVariadic = true;
    }

    return param;
}

std::vector<FunctionParam> Parser::parseParamList() {
    std::vector<FunctionParam> params;
    if (!check(TokenKind::RParen)) {
        do {
            params.push_back(parseFunctionParam());
        } while (match(TokenKind::Comma));
    }
    return params;
}

// ─── Generic parameters ───────────────────────────────────────────────────

std::vector<std::string> Parser::parseGenericParams() {
    std::vector<std::string> params;
    expect(TokenKind::Less);
    do {
        if (check(TokenKind::Identifier)) {
            std::string name = std::string(advance().stringValue);
            // Optional constraint: T: Protocol, T: A & B
            if (match(TokenKind::Colon)) {
                // Skip constraint types until we hit ',' or '>'
                // TODO: parse and store constraints properly
                while (!check(TokenKind::Comma) && !check(TokenKind::Greater) && !isAtEnd()) {
                    advance();
                }
            }
            params.push_back(name);
        } else {
            error("expected generic parameter name");
        }
    } while (match(TokenKind::Comma));
    expect(TokenKind::Greater);
    return params;
}

// ─── Access control ───────────────────────────────────────────────────────

AccessLevel Parser::parseAccessLevel() {
    if (match(TokenKind::KwPublic)) return AccessLevel::Public;
    if (match(TokenKind::KwInternal)) return AccessLevel::Internal;
    if (match(TokenKind::KwFileprivate)) return AccessLevel::FilePrivate;
    if (match(TokenKind::KwPrivate)) return AccessLevel::Private;
    if (match(TokenKind::KwOpen)) return AccessLevel::Open;
    return AccessLevel::Internal;
}

} // namespace suki
