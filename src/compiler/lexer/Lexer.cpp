#include "compiler/lexer/Lexer.h"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace suki {

namespace {

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string cleanDocBlock(const std::string& raw) {
    std::string out;
    size_t i = 0;
    bool atLineStart = true;
    while (i < raw.size()) {
        char c = raw[i];
        if (c == '\n') {
            out += c;
            atLineStart = true;
            ++i;
            continue;
        }
        if (atLineStart) {
            // skip leading whitespace
            if (c == ' ' || c == '\t') { ++i; continue; }
            // skip a leading '*'
            if (c == '*') { ++i; if (i < raw.size() && raw[i] == ' ') ++i; continue; }
            atLineStart = false;
        }
        out += c;
        ++i;
    }
    return out;
}

} // namespace

bool Lexer::isIdentStart(char c) {
    return std::isalpha((unsigned char)c) != 0 || c == '_' || c == '$';
}
bool Lexer::isIdentCont(char c) {
    return std::isalnum((unsigned char)c) != 0 || c == '_' || c == '$';
}
bool Lexer::isDigit(char c) { return c >= '0' && c <= '9'; }

Lexer::Lexer(std::string source, DiagnosticEngine& diags)
    : source_(std::move(source)), diags_(diags) {}

// ─── 条件编译预处理（规范 1.2）──────────────────────────────────────────────
// 支持 #if / #else / #elif / #endif / #ifdef / #ifndef / #define / #undef /
// #error / #warning。指令以 '#' 引入，作用域到下一个 '#' 或文件结束（单行指令）。
namespace {
std::string directiveName(const Token& t) {
    if (t.kind == TokenKind::TK_Keyword) return keywordToString(t.keyword);
    return t.text;
}
bool dDefined(const std::unordered_map<std::string, std::string>& macros, const std::string& id) {
    return macros.find(id) != macros.end();
}

bool dCondOr(const std::vector<Token>&, size_t&, const std::unordered_map<std::string, std::string>&, DiagnosticEngine&);
bool dCondPrimary(const std::vector<Token>& t, size_t& i,
                  const std::unordered_map<std::string, std::string>& macros, DiagnosticEngine& diags) {
    if (i >= t.size()) { diags.reportError("unexpected end of #if condition"); return false; }
    const Token& p = t[i];
    if (p.isPunct(PunctuatorID::LParen)) {
        ++i; bool v = dCondOr(t, i, macros, diags);
        if (i < t.size() && t[i].isPunct(PunctuatorID::RParen)) ++i;
        return v;
    }
    if (p.kind == TokenKind::TK_Identifier) {
        std::string s = p.text; ++i;
        if (s == "defined") {
            bool hasParen = i < t.size() && t[i].isPunct(PunctuatorID::LParen);
            if (hasParen) ++i;
            if (i >= t.size() || t[i].kind != TokenKind::TK_Identifier) {
                diags.reportError("expected identifier after 'defined'"); return false;
            }
            std::string id = t[i].text; ++i;
            if (hasParen && i < t.size() && t[i].isPunct(PunctuatorID::RParen)) ++i;
            return dDefined(macros, id);
        }
        if (s == "canImport") {
            if (i < t.size() && t[i].isPunct(PunctuatorID::LParen)) ++i;
            while (i < t.size() && !t[i].isPunct(PunctuatorID::RParen)) ++i;
            if (i < t.size() && t[i].isPunct(PunctuatorID::RParen)) ++i;
            return false; // 跳过标准库时 canImport 视为不可用
        }
        if (dDefined(macros, s)) return true;
        if (s == "compilerVersion") return true;
        diags.reportError("use of undeclared identifier '" + s + "' in #if condition");
        return false;
    }
    if (p.isKeyword(KeywordID::True)) { ++i; return true; }
    if (p.isKeyword(KeywordID::False)) { ++i; return false; }
    if (p.kind == TokenKind::TK_IntLiteral) {
        long long v = std::strtoll(p.numberText.c_str(), nullptr, 10); ++i;
        if (i < t.size()) {
            PunctuatorID op = t[i].punct;
            if (op == PunctuatorID::EqualEqual || op == PunctuatorID::BangEqual ||
                op == PunctuatorID::Less || op == PunctuatorID::Greater ||
                op == PunctuatorID::LessEqual || op == PunctuatorID::GreaterEqual) {
                ++i;
                long long rhs = 0;
                if (i < t.size() && t[i].kind == TokenKind::TK_IntLiteral) {
                    rhs = std::strtoll(t[i].numberText.c_str(), nullptr, 10); ++i;
                } else { diags.reportError("invalid #if comparison"); return false; }
                switch (op) {
                    case PunctuatorID::EqualEqual: return v == rhs;
                    case PunctuatorID::BangEqual: return v != rhs;
                    case PunctuatorID::Less: return v < rhs;
                    case PunctuatorID::Greater: return v > rhs;
                    case PunctuatorID::LessEqual: return v <= rhs;
                    case PunctuatorID::GreaterEqual: return v >= rhs;
                    default: return false;
                }
            }
        }
        return v != 0;
    }
    diags.reportError("invalid #if condition"); ++i; return false;
}
bool dCondUnary(const std::vector<Token>& t, size_t& i,
                const std::unordered_map<std::string, std::string>& macros, DiagnosticEngine& diags) {
    if (i < t.size() && t[i].isPunct(PunctuatorID::Bang)) { ++i; return !dCondUnary(t, i, macros, diags); }
    return dCondPrimary(t, i, macros, diags);
}
bool dCondAnd(const std::vector<Token>& t, size_t& i,
              const std::unordered_map<std::string, std::string>& macros, DiagnosticEngine& diags) {
    bool l = dCondUnary(t, i, macros, diags);
    while (i < t.size() && t[i].isPunct(PunctuatorID::AmpAmp)) {
        ++i; bool r = dCondUnary(t, i, macros, diags); l = l && r;
    }
    return l;
}
bool dCondOr(const std::vector<Token>& t, size_t& i,
             const std::unordered_map<std::string, std::string>& macros, DiagnosticEngine& diags) {
    bool l = dCondAnd(t, i, macros, diags);
    while (i < t.size() && t[i].isPunct(PunctuatorID::PipePipe)) {
        ++i; bool r = dCondAnd(t, i, macros, diags); l = l || r;
    }
    return l;
}
bool evalDirectiveCond(const std::vector<Token>& t,
                       const std::unordered_map<std::string, std::string>& macros, DiagnosticEngine& diags) {
    size_t i = 0;
    return dCondOr(t, i, macros, diags);
}

std::vector<Token> preprocess(std::vector<Token> toks, DiagnosticEngine& diags) {
    std::vector<Token> out;
    std::unordered_map<std::string, std::string> macros;
    macros["compilerVersion"] = "1"; // 内置宏
    struct CondFrame { bool inTakenBranch; bool anyTaken; bool elseSeen; };
    std::vector<CondFrame> stack;
    auto emitNow = [&]() -> bool {
        for (auto& f : stack) if (!f.inTakenBranch) return false;
        return true;
    };
    // 外层（不含栈顶当前帧）是否生效：用于 #else/#elif 判断所在 #if 组是否处于
    // 生效上下文，而非当前分支是否已采纳。
    auto enclosingEmit = [&]() -> bool {
        for (size_t k = 0; k + 1 < stack.size(); ++k) if (!stack[k].inTakenBranch) return false;
        return true;
    };
    size_t n = toks.size();
    size_t i = 0;
    while (i < n) {
        if (toks[i].isPunct(PunctuatorID::Hash)) {
            size_t hpos = i; ++i;
            if (i >= n) { diags.reportError("expected directive after '#'"); break; }
            std::string dname = directiveName(toks[i]);
            size_t dirLine = toks[i].loc.line; // 指令关键字所在行，操作数仅限同一行
            ++i;
            // 收集同一行上的操作数 token（到下一个 '#' 或换行为止）。分支体位于
            // 其他行，由主循环按当前是否生效决定保留/丢弃，避免误吞分支与 EOF。
            auto collect = [&](size_t line) -> std::vector<Token> {
                std::vector<Token> body;
                while (i < n && toks[i].loc.line == line && !toks[i].isPunct(PunctuatorID::Hash))
                    body.push_back(toks[i++]);
                return body;
            };
            bool parentEmit = enclosingEmit();
            if (dname == "if") {
                auto body = collect(dirLine);
                bool val = evalDirectiveCond(body, macros, diags);
                CondFrame f; f.inTakenBranch = parentEmit && val; f.anyTaken = f.inTakenBranch; f.elseSeen = false;
                stack.push_back(f);
            } else if (dname == "else") {
                if (stack.empty()) { diags.reportError("#else without matching #if"); break; }
                CondFrame& f = stack.back();
                if (f.elseSeen) diags.reportError("#else after #else");
                f.elseSeen = true;
                bool taken = parentEmit && !f.anyTaken; if (taken) f.anyTaken = true;
                f.inTakenBranch = taken;
            } else if (dname == "elif") {
                if (stack.empty()) { diags.reportError("#elif without matching #if"); break; }
                auto body = collect(dirLine);
                CondFrame& f = stack.back();
                if (f.elseSeen) diags.reportError("#elif after #else");
                bool val = evalDirectiveCond(body, macros, diags);
                bool taken = parentEmit && !f.anyTaken && val; if (taken) f.anyTaken = true;
                f.inTakenBranch = taken;
            } else if (dname == "endif") {
                if (stack.empty()) { diags.reportError("#endif without matching #if"); break; }
                stack.pop_back();
            } else if (dname == "ifdef" || dname == "ifndef") {
                auto body = collect(dirLine);
                bool def = !body.empty() && body[0].kind == TokenKind::TK_Identifier &&
                           dDefined(macros, body[0].text);
                if (dname == "ifndef") def = !def;
                CondFrame f; f.inTakenBranch = parentEmit && def; f.anyTaken = f.inTakenBranch; f.elseSeen = false;
                stack.push_back(f);
            } else if (dname == "define") {
                auto body = collect(dirLine);
                if (parentEmit && !body.empty() && body[0].kind == TokenKind::TK_Identifier)
                    macros[body[0].text] = (body.size() > 1 ? body[1].text : "");
            } else if (dname == "undef") {
                auto body = collect(dirLine);
                if (parentEmit && !body.empty() && body[0].kind == TokenKind::TK_Identifier)
                    macros.erase(body[0].text);
            } else if (dname == "error" || dname == "warning") {
                auto body = collect(dirLine);
                if (parentEmit) {
                    std::string msg; for (auto& tk : body) msg += tk.text + " ";
                    if (dname == "error") diags.reportError("#error " + msg);
                    else diags.reportWarning("#warning " + msg);
                }
            } else {
                // 未知指令名（#makeExpr / #macroName / #selector 等）：这不是预处理
                // 指令，而是宏调用 / 宏原语引用的 `#ident`。原样放回流，交给 Parser
                // 解析为 Hash + Ident，再由 Sema 的宏展开 pass 处理（规范 5.6）。
                if (emitNow()) {
                    out.push_back(toks[hpos]);     // '#'
                    out.push_back(toks[hpos + 1]); // 标识符
                }
            }
        } else {
            if (emitNow()) out.push_back(toks[i]);
            ++i;
        }
    }
    if (!stack.empty()) diags.reportError("missing #endif");
    return out;
}
} // namespace

std::vector<Token> Lexer::tokenizeAll() {
    std::vector<Token> toks;
    for (;;) {
        Token t = next();
        toks.push_back(t);
        if (t.kind == TokenKind::TK_EOF) break;
    }
    return preprocess(std::move(toks), diags_);
}

void Lexer::advance() {
    if (atEnd()) return;
    if (source_[pos_] == '\n') { ++line_; col_ = 1; }
    else ++col_;
    ++pos_;
}

char Lexer::peek(size_t off) const {
    if (pos_ + off >= source_.size()) return '\0';
    return source_[pos_ + off];
}
bool Lexer::atEnd() const { return pos_ >= source_.size(); }

Token Lexer::makeToken(TokenKind kind) {
    Token t;
    t.kind = kind;
    t.loc = SourceLocation{(uint32_t)line_, (uint32_t)col_, (uint32_t)pos_};
    return t;
}

Token Lexer::makeError(const std::string& msg) {
    SourceLocation here{(uint32_t)line_, (uint32_t)col_, (uint32_t)pos_};
    SourceRange r{here, here};
    diags_.reportError(msg, r);
    return makeToken(TokenKind::TK_Error);
}

Token Lexer::stringEndToken() {
    Token t = makeToken(TokenKind::TK_StringEnd);
    t.text = "";
    return t;
}

void Lexer::skipTrivia() {
    while (!atEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
            advance();
            continue;
        }
        if (c == '\n') { advance(); continue; }

        // line comment
        if (c == '/' && peek(1) == '/') {
            bool isDoc = (peek(2) == '/');
            advance(); advance(); // consume //
            if (isDoc) {
                while (!atEnd() && peek() == '/') advance(); // consume extra '/'
                if (!atEnd() && peek() == ' ') advance();
                std::string line;
                while (!atEnd() && peek() != '\n') { line += peek(); advance(); }
                if (!pendingDoc_.empty()) pendingDoc_ += "\n";
                pendingDoc_ += line;
            } else {
                while (!atEnd() && peek() != '\n') advance();
            }
            continue;
        }

        // block comment
        if (c == '/' && peek(1) == '*') {
            bool isDoc = (peek(2) == '*');
            advance(); advance(); // consume /*
            std::string content;
            while (!atEnd() && !(peek() == '*' && peek(1) == '/')) {
                if (peek() == '\n') { content += '\n'; advance(); continue; }
                content += peek(); advance();
            }
            if (!atEnd()) advance(); // consume '*'
            if (!atEnd()) advance(); // consume '/'
            if (isDoc) {
                if (!pendingDoc_.empty()) pendingDoc_ += "\n";
                pendingDoc_ += cleanDocBlock(content);
            }
            continue;
        }

        break; // not trivia
    }
}

Token Lexer::next() {
    if (pendingStringEnd_) {
        pendingStringEnd_ = false;
        return stringEndToken();
    }
    skipTrivia();
    if (atEnd()) return makeToken(TokenKind::TK_EOF);

    Token t = lexSingle();

    if (interpDepth_ > 0 && t.isPunct(PunctuatorID::RParen)) {
        interpDepth_--;
        if (interpDepth_ == 0) {
            return resumeString();
        }
        return t;
    }
    if (interpDepth_ > 0 && t.isPunct(PunctuatorID::LParen)) {
        interpDepth_++;
    }

    if (!pendingDoc_.empty() && t.kind != TokenKind::TK_EOF) {
        t.docComment = pendingDoc_;
        pendingDoc_.clear();
    }
    return t;
}

Token Lexer::lexSingle() {
    char c = peek();
    if (isIdentStart(c)) return lexIdentifierOrKeyword();
    if (isDigit(c)) return lexNumber();
    if (c == '"') {
        if (peek(1) == '"' && peek(2) == '"') return lexString(false, true);
        return lexString(false, false);
    }
    if (c == '\'') return lexChar();
    // `.5` starts a float literal only at a token boundary. In `t.0` the dot is
    // member access and the digits are a separate integer token, so require the
    // preceding character not to be an identifier continuation.
    if (c == '.' && isDigit(peek(1)) &&
        (pos_ == 0 || !isIdentCont(source_[pos_ - 1])))
        return lexNumber();
    return lexPunctuator();
}

Token Lexer::lexIdentifierOrKeyword() {
    size_t start = pos_;
    Token t = makeToken(TokenKind::TK_Identifier);
    std::string txt;
    txt += peek(); advance();
    while (!atEnd() && isIdentCont(peek())) { txt += peek(); advance(); }
    t.text = source_.substr(start, pos_ - start);

    // raw string literal: r"..."
    if (txt == "r" && !atEnd() && peek() == '"') {
        return lexString(true, false);
    }

    KeywordID kw = keywordFromString(txt);
    if (kw != KeywordID::None) {
        t.kind = TokenKind::TK_Keyword;
        t.keyword = kw;
    }
    return t;
}

Token Lexer::lexNumber() {
    size_t start = pos_;
    Token t = makeToken(TokenKind::TK_IntLiteral);
    std::string txt;
    bool isFloat = false;
    char c = peek();

    if (c == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        txt += '0'; advance(); txt += peek(); advance();
        while (!atEnd() && (isHexDigit(peek()) || peek() == '_')) {
            if (peek() != '_') txt += peek();
            advance();
        }
    } else if (c == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        txt += '0'; advance(); txt += peek(); advance();
        while (!atEnd() && ((peek() >= '0' && peek() <= '1') || peek() == '_')) {
            if (peek() != '_') txt += peek();
            advance();
        }
    } else if (c == '0' && (peek(1) == 'o' || peek(1) == 'O')) {
        txt += '0'; advance(); txt += peek(); advance();
        while (!atEnd() && ((peek() >= '0' && peek() <= '7') || peek() == '_')) {
            if (peek() != '_') txt += peek();
            advance();
        }
    } else {
        while (!atEnd() && (isDigit(peek()) || peek() == '_')) {
            if (peek() != '_') txt += peek();
            advance();
        }
    }

    // fractional part
    if (!atEnd() && peek() == '.' && isDigit(peek(1))) {
        isFloat = true;
        txt += '.'; advance();
        while (!atEnd() && (isDigit(peek()) || peek() == '_')) {
            if (peek() != '_') txt += peek();
            advance();
        }
    }
    // exponent
    if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
        isFloat = true;
        txt += peek(); advance();
        if (!atEnd() && (peek() == '+' || peek() == '-')) { txt += peek(); advance(); }
        while (!atEnd() && (isDigit(peek()) || peek() == '_')) {
            if (peek() != '_') txt += peek();
            advance();
        }
    }
    // type suffix (e.g., u8, i32, f64) — identifier-like suffix (may contain digits)
    if (!atEnd() && std::isalpha((unsigned char)peek())) {
        while (!atEnd() && (std::isalnum((unsigned char)peek()) || peek() == '_')) {
            txt += peek(); advance();
        }
    }

    t.kind = isFloat ? TokenKind::TK_FloatLiteral : TokenKind::TK_IntLiteral;
    t.numberText = txt;
    t.text = source_.substr(start, pos_ - start);
    return t;
}

Token Lexer::lexString(bool raw, bool multiline) {
    // current char is the opening quote
    curOpenStart_ = pos_;
    curOpenLoc_   = SourceLocation{(uint32_t)line_, (uint32_t)col_, (uint32_t)pos_};
    interpolatedString_ = false; // reset per-string state
    advance(); // consume opening quote
    if (!raw && multiline) {
        if (!atEnd() && peek() == '"') advance();
        if (!atEnd() && peek() == '"') advance();
    }
    curRaw_ = raw;
    curMultiline_ = multiline;
    return lexStringBody(raw, multiline);
}

Token Lexer::lexStringBody(bool raw, bool multiline) {
    std::string fragment;
    while (!atEnd()) {
        char c = peek();
        if (c == '\\' && !raw) {
            if (peek(1) == '(') {
                advance(); advance(); // consume '\('
                interpolatedString_ = true;
                interpDepth_++;
                Token frag = makeToken(TokenKind::TK_StringFragment);
                frag.stringValue = fragment;
                frag.text = fragment;
                return frag;
            }
            advance(); // consume backslash
            char e = peek();
            switch (e) {
                case 'n': fragment += '\n'; break;
                case 't': fragment += '\t'; break;
                case 'r': fragment += '\r'; break;
                case '0': fragment += '\0'; break;
                case '\\': fragment += '\\'; break;
                case '\'': fragment += '\''; break;
                case '"': fragment += '"'; break;
                default: fragment += e; break;
            }
            advance();
            continue;
        }
        if (c == '"') {
            if (multiline) {
                if (peek(1) == '"' && peek(2) == '"') {
                    size_t closePos = pos_;
                    advance(); advance(); advance(); // consume """
                    return finishString(fragment, closePos, 3);
                }
                fragment += '"'; advance();
                continue;
            }
            size_t closePos = pos_;
            advance(); // consume closing quote
            return finishString(fragment, closePos, 1);
        }
        if (c == '\n' && !multiline) {
            SourceRange r{curOpenLoc_, curOpenLoc_};
            diags_.reportError("unterminated string literal", r);
            return makeToken(TokenKind::TK_Error);
        }
        fragment += c; advance();
    }
    SourceRange r{curOpenLoc_, curOpenLoc_};
    diags_.reportError("unterminated string literal", r);
    return makeToken(TokenKind::TK_Error);
}

Token Lexer::finishString(const std::string& fragment, size_t closePos, size_t quoteLen) {
    if (!interpolatedString_) {
        Token t;
        t.kind = TokenKind::TK_StringLiteral;
        t.loc = curOpenLoc_;
        t.stringValue = fragment;
        t.text = source_.substr(curOpenStart_, closePos + quoteLen - curOpenStart_);
        if (!pendingDoc_.empty()) { t.docComment = pendingDoc_; pendingDoc_.clear(); }
        return t;
    }
    Token frag;
    frag.kind = TokenKind::TK_StringFragment;
    frag.loc = curOpenLoc_;
    frag.stringValue = fragment;
    frag.text = fragment;
    pendingStringEnd_ = true;
    if (!pendingDoc_.empty()) { frag.docComment = pendingDoc_; pendingDoc_.clear(); }
    return frag;
}

Token Lexer::resumeString() {
    return lexStringBody(curRaw_, curMultiline_);
}

Token Lexer::lexChar() {
    size_t start = pos_;
    Token t = makeToken(TokenKind::TK_CharLiteral);
    advance(); // consume opening '
    std::string val;
    if (!atEnd() && peek() == '\\') {
        advance();
        char e = peek();
        switch (e) {
            case 'n': val += '\n'; break;
            case 't': val += '\t'; break;
            case 'r': val += '\r'; break;
            case '0': val += '\0'; break;
            case '\\': val += '\\'; break;
            case '\'': val += '\''; break;
            case '"': val += '"'; break;
            default: val += e; break;
        }
        advance();
    } else if (!atEnd()) {
        val += peek(); advance();
    }
    if (!atEnd() && peek() == '\'') advance();
    else diags_.reportError("unterminated character literal");
    t.stringValue = val;
    t.text = source_.substr(start, pos_ - start);
    return t;
}

Token Lexer::lexPunctuator() {
    size_t start = pos_;
    SourceLocation sloc{(uint32_t)line_, (uint32_t)col_, (uint32_t)pos_};
    char c = peek();
    char n = peek(1);
    PunctuatorID pid = PunctuatorID::None;
    int consume = 1;

    switch (c) {
        case '+':
            if (n == '=') { pid = PunctuatorID::PlusEqual; consume = 2; }
            else pid = PunctuatorID::Plus;
            break;
        case '-':
            if (n == '>') { pid = PunctuatorID::Arrow; consume = 2; }
            else if (n == '=') { pid = PunctuatorID::MinusEqual; consume = 2; }
            else pid = PunctuatorID::Minus;
            break;
        case '*':
            if (n == '=') { pid = PunctuatorID::StarEqual; consume = 2; }
            else pid = PunctuatorID::Star;
            break;
        case '/':
            if (n == '=') { pid = PunctuatorID::SlashEqual; consume = 2; }
            else pid = PunctuatorID::Slash;
            break;
        case '%':
            if (n == '=') { pid = PunctuatorID::PercentEqual; consume = 2; }
            else pid = PunctuatorID::Percent;
            break;
        case '=':
            if (n == '=') { pid = PunctuatorID::EqualEqual; consume = 2; }
            else if (n == '>') { pid = PunctuatorID::FatArrow; consume = 2; }
            else pid = PunctuatorID::Equal;
            break;
        case '!':
            if (n == '=') { pid = PunctuatorID::BangEqual; consume = 2; }
            else pid = PunctuatorID::Bang;
            break;
        case '<':
            if (n == '<') { pid = PunctuatorID::LessLess; consume = 2; }
            else if (n == '=') { pid = PunctuatorID::LessEqual; consume = 2; }
            else if (n == '-') { pid = PunctuatorID::LeftArrow; consume = 2; }
            else pid = PunctuatorID::Less;
            break;
        case '>':
            if (n == '>') { pid = PunctuatorID::GreaterGreater; consume = 2; }
            else if (n == '=') { pid = PunctuatorID::GreaterEqual; consume = 2; }
            else pid = PunctuatorID::Greater;
            break;
        case '&':
            if (n == '&') { pid = PunctuatorID::AmpAmp; consume = 2; }
            else pid = PunctuatorID::Amp;
            break;
        case '|':
            if (n == '|') { pid = PunctuatorID::PipePipe; consume = 2; }
            else pid = PunctuatorID::Pipe;
            break;
        case '^': pid = PunctuatorID::Caret; break;
        case '~': pid = PunctuatorID::Tilde; break;
        case '.':
            if (n == '.' ) {
                if (peek(2) == '<') { pid = PunctuatorID::DotDotLess; consume = 3; }
                else if (peek(2) == '.') { pid = PunctuatorID::DotDot; consume = 3; }
                else {
                    advance(); advance();
                    diags_.reportError("invalid token '..' (use '..<' or '...')",
                        SourceRange{sloc, sloc});
                    return makeToken(TokenKind::TK_Error);
                }
            } else pid = PunctuatorID::Dot;
            break;
        case '?':
            if (n == '?') { pid = PunctuatorID::QuestionQuestion; consume = 2; }
            else pid = PunctuatorID::Question;
            break;
        case ':': pid = PunctuatorID::Colon; break;
        case ';': pid = PunctuatorID::Semicolon; break;
        case ',': pid = PunctuatorID::Comma; break;
        case '(': pid = PunctuatorID::LParen; break;
        case ')': pid = PunctuatorID::RParen; break;
        case '{': pid = PunctuatorID::LBrace; break;
        case '}': pid = PunctuatorID::RBrace; break;
        case '[': pid = PunctuatorID::LBracket; break;
        case ']': pid = PunctuatorID::RBracket; break;
        case '@': pid = PunctuatorID::At; break;
        case '#': pid = PunctuatorID::Hash; break;
        default:
            advance();
            diags_.reportError(std::string("unexpected character '") + c + "'",
                SourceRange{sloc, sloc});
            return makeToken(TokenKind::TK_Error);
    }

    Token t;
    t.kind = TokenKind::TK_Punctuator;
    t.punct = pid;
    t.loc = sloc;
    for (int i = 0; i < consume; ++i) advance();
    t.text = source_.substr(start, pos_ - start);
    return t;
}

} // namespace suki
