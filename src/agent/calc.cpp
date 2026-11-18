#include "agent/calc.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace calc {

namespace {

// Recursive-descent parser:  expr := term (('+'|'-') term)*
//                            term := unary (('*'|'/'|'%') unary)*
//                            unary := ('-'|'+') unary | power
//                            power := postfix ('^' unary)?
//                            postfix := primary ('!' | '%')*
struct Parser {
    const std::string &s;
    size_t i = 0;
    int depth = 0;
    explicit Parser(const std::string &text) : s(text) {}

    // Each bracket, sign or power goes one level deeper into the parser's own stack. A deep enough
    // pile (tens of thousands of brackets) would overflow it and end the whole app, so nesting
    // beyond anything a real sum needs is refused.
    struct Deeper {
        Parser &p;
        explicit Deeper(Parser &parser) : p(parser) {
            if (++p.depth > kMaxDepth) p.fail("the expression is nested too deeply; split it into smaller steps");
        }
        ~Deeper() { --p.depth; }
    };
    static constexpr int kMaxDepth = 200;

    void skip() { while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i; }
    bool eat(char c) { skip(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
    bool eat2(const char *two) {
        skip();
        if (s.compare(i, 2, two) == 0) { i += 2; return true; }
        return false;
    }
    [[noreturn]] void fail(const std::string &msg) { throw std::runtime_error(msg); }

    double expr() {
        Deeper guard(*this);
        skip();
        const size_t start = i;
        double v = term();
        for (;;) {
            const bool plus = eat('+');
            if (!plus && !eat('-')) return v;
            const size_t from = i;
            const double t = term();
            // "200 + 10%" reads as adding ten percent, but is 200 + 0.1. Asked to work out a tip or
            // a price rise, that is a wrong answer said with confidence; say how to write it.
            if (ends_in_percent(from)) {
                const std::string pct = trimmed(from, i), base = trimmed(start, from - 1);
                const std::string factor = format(plus ? 1 + t : 1 - t);
                const std::string b = base.find_first_of("+-") == std::string::npos || one_group(base) ? base : "(" + base + ")";
                fail("'" + base + (plus ? " + " : " - ") + pct + "' is ambiguous: to " + (plus ? "add " : "take off ") + pct +
                     " write " + b + "*" + factor + ", or for " + pct + " of it alone " + b + "*" + format(t));
            }
            v = plus ? v + t : v - t;
        }
    }

    // "(1 + 2)" is one bracketed group; "(1) + (2)" is not.
    static bool one_group(const std::string &t) {
        if (t.size() < 2 || t.front() != '(' || t.back() != ')') return false;
        int depth = 0;
        for (size_t k = 0; k < t.size(); ++k) {
            depth += t[k] == '(' ? 1 : t[k] == ')' ? -1 : 0;
            if (depth == 0 && k + 1 < t.size()) return false;
        }
        return true;
    }

    std::string trimmed(size_t from, size_t to) const {
        while (from < to && isspace(static_cast<unsigned char>(s[from]))) ++from;
        while (to > from && isspace(static_cast<unsigned char>(s[to - 1]))) --to;
        return s.substr(from, to - from);
    }

    // The term just read, from `from` to here, is a bare percentage such as "10%" or "12.5 %".
    bool ends_in_percent(size_t from) const {
        const std::string t = trimmed(from, i);
        if (t.size() < 2 || t.back() != '%') return false;
        for (size_t k = 0; k + 1 < t.size(); ++k)
            if (!isdigit(static_cast<unsigned char>(t[k])) && t[k] != '.' && t[k] != ' ') return false;
        return true;
    }

    double term() {
        double v = unary();
        for (;;) {
            if (eat2("**")) { i -= 2; return v; }  // handled by power()
            if (eat('*')) v *= unary();
            else if (eat('/')) {
                const double d = unary();
                if (d == 0) fail("division by zero");
                v /= d;
            } else if (peek_mod()) {
                ++i;
                const double d = unary();
                if (d == 0) fail("division by zero");
                v = std::fmod(v, d);
            } else return v;
        }
    }

    // '%' is modulo only when followed by an operand; otherwise it is a percent sign.
    bool peek_mod() {
        skip();
        if (i >= s.size() || s[i] != '%') return false;
        size_t j = i + 1;
        while (j < s.size() && isspace(static_cast<unsigned char>(s[j]))) ++j;
        return j < s.size() && (isalnum(static_cast<unsigned char>(s[j])) || s[j] == '(' || s[j] == '.');
    }

    double unary() {
        Deeper guard(*this);
        if (eat('-')) return -unary();
        if (eat('+')) return unary();
        return power();
    }

    double power() {
        double base = postfix();
        if (eat2("**") || eat('^')) return std::pow(base, unary());
        return base;
    }

    double postfix() {
        double v = primary();
        for (;;) {
            skip();
            if (i < s.size() && s[i] == '!') {
                ++i;
                if (v < 0 || v > 170 || std::floor(v) != v) fail("factorial needs a whole number from 0 to 170");
                v = std::tgamma(v + 1);
            } else if (i < s.size() && s[i] == '%' && !peek_mod()) {
                ++i;
                v /= 100.0;
            } else return v;
        }
    }

    double primary() {
        skip();
        if (i >= s.size()) fail("unexpected end of expression");
        if (eat('(')) {
            const double v = expr();
            if (!eat(')')) fail("missing closing bracket");
            return v;
        }
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (isdigit(c) || c == '.') {
            size_t used = 0;
            double v;
            try { v = std::stod(s.substr(i), &used); }
            catch (const std::out_of_range &) { fail("a number is too large or too small to work with"); }
            catch (...) { fail("bad number"); }
            i += used;
            return v;
        }
        if (isalpha(c)) {
            std::string name;
            while (i < s.size() && (isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
                name += static_cast<char>(tolower(static_cast<unsigned char>(s[i++])));
            if (name == "pi") return M_PI;
            if (name == "e") return M_E;
            if (name == "tau") return 2 * M_PI;
            if (name == "of")
                fail("'of' is not arithmetic: write '15% of 80' as 0.15*80, or 15/100*80");
            if (!eat('(')) fail("unknown name '" + name + "'");
            std::vector<double> args;
            if (!eat(')')) {
                do args.push_back(expr()); while (eat(','));
                if (!eat(')')) fail("missing closing bracket");
            }
            return call(name, args);
        }
        fail(unexpected(s[i]));
    }

    static std::string unexpected(char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) return "unexpected control character in the expression";
        if (u >= 0x80) return "unexpected character in the expression; use plain digits and + - * / ^ ( )";
        return std::string("unexpected '") + c + "'";
    }

    double call(const std::string &f, const std::vector<double> &a) {
        auto need = [&](size_t n) { if (a.size() != n) fail(f + " takes " + std::to_string(n) + " argument(s)"); };
        if (f == "sqrt") { need(1); if (a[0] < 0) fail("square root of a negative number"); return std::sqrt(a[0]); }
        if (f == "cbrt") { need(1); return std::cbrt(a[0]); }
        if (f == "abs") { need(1); return std::fabs(a[0]); }
        if (f == "sin") { need(1); return std::sin(a[0]); }
        if (f == "cos") { need(1); return std::cos(a[0]); }
        if (f == "tan") { need(1); return std::tan(a[0]); }
        if (f == "asin") { need(1); return std::asin(a[0]); }
        if (f == "acos") { need(1); return std::acos(a[0]); }
        if (f == "atan") { need(1); return std::atan(a[0]); }
        if (f == "exp") { need(1); return std::exp(a[0]); }
        if (f == "ln") { need(1); if (a[0] <= 0) fail("log of a non-positive number"); return std::log(a[0]); }
        if (f == "log10") { need(1); if (a[0] <= 0) fail("log of a non-positive number"); return std::log10(a[0]); }
        if (f == "log2") { need(1); if (a[0] <= 0) fail("log of a non-positive number"); return std::log2(a[0]); }
        if (f == "log") {
            if (a.size() == 1) { if (a[0] <= 0) fail("log of a non-positive number"); return std::log10(a[0]); }
            need(2);
            return std::log(a[0]) / std::log(a[1]);
        }
        if (f == "floor") { need(1); return std::floor(a[0]); }
        if (f == "ceil") { need(1); return std::ceil(a[0]); }
        if (f == "round") {
            if (a.size() == 2) { const double m = std::pow(10, a[1]); return std::round(a[0] * m) / m; }
            need(1);
            return std::round(a[0]);
        }
        if (f == "deg") { need(1); return a[0] * 180.0 / M_PI; }
        if (f == "rad") { need(1); return a[0] * M_PI / 180.0; }
        if (f == "pow") { need(2); return std::pow(a[0], a[1]); }
        if (f == "min" || f == "max") {
            if (a.empty()) fail(f + " needs at least one argument");
            double v = a[0];
            for (double x : a) v = f == "min" ? std::fmin(v, x) : std::fmax(v, x);
            return v;
        }
        fail("unknown function '" + f + "'");
    }
};

}  // namespace

bool evaluate(const std::string &expression, double &result, std::string &err) {
    // Accept the symbols people and models naturally write.
    std::string text;
    int depth = 0;
    for (size_t i = 0; i < expression.size(); ++i) {
        const char c = expression[i];
        if (expression.compare(i, 2, "\xC3\x97") == 0) { text += '*'; ++i; }              // multiplication sign
        else if (expression.compare(i, 2, "\xC3\xB7") == 0) { text += '/'; ++i; }         // division sign
        else if (expression.compare(i, 3, "\xE2\x88\x92") == 0) { text += '-'; i += 2; }  // minus sign
        else if (c == ',' && depth == 0 && i > 0 && i + 1 < expression.size() &&
                 isdigit(static_cast<unsigned char>(expression[i - 1])) &&
                 isdigit(static_cast<unsigned char>(expression[i + 1]))) {
            // thousands separator outside any function call: skip it
        } else {
            if (c == '(') ++depth;
            if (c == ')') --depth;
            text += c;
        }
    }
    try {
        Parser p(text);
        result = p.expr();
        p.skip();
        if (p.i != text.size()) throw std::runtime_error(Parser::unexpected(text[p.i]));
        if (std::isnan(result) || std::isinf(result)) throw std::runtime_error("result is not a finite number");
        return true;
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
}

std::string format(double value) {
    if (std::fabs(value) < 1e15 && std::floor(value) == value) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", value);
        return buf;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%.10g", value);
    return buf;
}

}  // namespace calc
