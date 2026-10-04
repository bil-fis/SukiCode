#include "compiler/lexer/Lexer.h"

#include <cctype>

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

std::vector<Token> Lexer::tokenizeAll() {
    std::vector<Token> toks;
    for (;;) {
        Token t = next();
        toks.push_back(t);
        if (t.kind == TokenKind::TK_EOF) break;
    }
    return toks;
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
