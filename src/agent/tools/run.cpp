// tools::run: a call checked against its tool's schema, run, and every failure given one shape.
#include "agent/tools/common.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace tools {

namespace {

// A tool's own schema properties, or null when it has none here: its schema is made at run time
// (smart_home, radio) and it checks its own arguments.
const json *properties_of(const Tool &t) {
    const auto it = t.parameters.find("properties");
    return it != t.parameters.end() && it->is_object() && !it->empty() ? &*it : nullptr;
}

// What was sent, in words, for an error that says so.
std::string sent_text(const json &v) {
    if (v.is_array()) return "a list";
    if (v.is_object()) return "an object";
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_string()) return "'" + util::truncate_utf8(clean_text(v.get<std::string>()), 40) + "'";
    return v.dump();
}

// Each argument against the type the tool's schema gives it, before the tool runs, so that every
// tool answers a wrong type the same way: which argument, what it must be, what was sent. A yes or
// no sent as "true", "yes" or 1 is taken as meant, an enum value in other capitals becomes the
// listed one, and empty text for a number counts as not given; other text for a number must be one
// as a whole (util::to_number: "20 minutes" is not 20). Ranges, dates and which names exist stay
// with each tool. Returns "" when the arguments may go to the tool; `unknown` collects names the
// schema does not have.
std::string check_args(const json &props, json &args, std::vector<std::string> &unknown) {
    std::vector<std::string> drop;
    for (auto it = args.begin(); it != args.end(); ++it) {
        const std::string key = it.key();
        json &v = it.value();
        const auto p = props.find(key);
        if (p == props.end()) {
            if (key != "say") unknown.push_back(key);   // a confirmation offered to a tool that takes none is harmless
            continue;
        }
        if (v.is_null()) continue;
        const std::string type = jget(*p, "type", std::string());
        if (type == "string") {
            if (v.is_string()) {
                const auto values = p->find("enum");
                if (values != p->end() && values->is_array())
                    for (const auto &e : *values)
                        if (e.is_string() && util::lower(e.get<std::string>()) == util::lower(util::trim(v.get<std::string>()))) v = e;
            } else if (!v.is_number()) {   // a number is taken as its digits
                return key + " must be text, not " + sent_text(v);
            }
        } else if (type == "number" || type == "integer") {
            const bool id = key == "id" || (key.size() > 3 && key.compare(key.size() - 3, 3, "_id") == 0);
            const std::string must = id ? key + " must be a whole number such as 12, taken from an earlier result"
                                        : key + " must be a number, such as 5";
            if (v.is_string()) {
                double d = 0;
                if (util::trim(v.get<std::string>()).empty()) { drop.push_back(key); continue; }
                if (!util::to_number(v.get<std::string>(), d)) return must + "; " + sent_text(v) + " is not one";
            } else if (v.is_number()) {
                if (!std::isfinite(v.get<double>())) return must + "; " + sent_text(v) + " is not one";
            } else {
                return must + ", not " + sent_text(v);
            }
        } else if (type == "boolean") {
            if (v.is_boolean()) continue;
            const std::string s = v.is_string() ? util::lower(util::trim(v.get<std::string>())) : v.is_number() ? v.dump() : "?";
            if (s.empty()) drop.push_back(key);
            else if (s == "true" || s == "yes" || s == "on" || s == "1") v = true;
            else if (s == "false" || s == "no" || s == "off" || s == "0") v = false;
            else return key + " must be true or false, not " + sent_text(v);
        }
    }
    for (const auto &k : drop) args.erase(k);
    return "";
}

// "(this tool has no argument called 'minutes'; its arguments are text, in_minutes, at)", for an
// error that may come from a misnamed argument.
std::string unknown_note(const json &props, const std::vector<std::string> &unknown) {
    if (unknown.empty()) return "";
    std::string names, have;
    for (const auto &u : unknown) names += (names.empty() ? "'" : ", '") + util::truncate_utf8(clean_text(u), 30) + "'";
    for (auto it = props.begin(); it != props.end(); ++it)
        if (it.key() != "say") have += (have.empty() ? "" : ", ") + it.key();
    return " (this tool has no argument called " + names + "; its arguments are " + have + ")";
}

// An example of the arguments, for an error about their form: {"expression": "..."}.
std::string example_args(const Tool &t) {
    const json *props = properties_of(t);
    if (!props) return "{\"action\": \"...\"}";
    std::string key;
    const auto req = t.parameters.find("required");
    if (req != t.parameters.end() && req->is_array() && !req->empty() && (*req)[0].is_string()) key = (*req)[0].get<std::string>();
    if (key.empty() || !props->contains(key)) key = props->begin().key();
    const std::string type = jget(props->at(key), "type", std::string());
    return "{\"" + key + "\": " + (type == "number" || type == "integer" ? "5" : type == "boolean" ? "true" : "\"...\"") + "}";
}

// Edit distance, for suggesting the tool a misspelt name meant.
size_t distance(const std::string &a, const std::string &b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diag = up;
        }
    }
    return row[b.size()];
}

std::string no_such_tool(const std::string &name) {
    std::vector<std::string> offered;
    for (const auto &t : schema()) offered.push_back(jget(t["function"], "name", std::string()));
    const std::string asked = util::lower(util::truncate_utf8(clean_text(name), 40));
    std::string best;
    size_t best_d = 4;
    for (const auto &n : offered) {
        const size_t d = distance(asked, n);
        if (d < best_d) { best_d = d; best = n; }
    }
    if (!best.empty()) return fail("there is no tool called '" + asked + "'; the one meant is probably " + best);
    std::string all;
    for (const auto &n : offered) all += (all.empty() ? "" : ", ") + n;
    return fail("there is no tool called '" + asked + "'; the tools are " + all);
}

// One shape for every failure: "ERROR: what was wrong. What to do." A tool that answered with a
// JSON object holding "error", whatever its value, gets the same shape, its other fields kept after
// it, and so does text that only begins like one. Done before anything is added to a result or it is
// cut short, either of which would leave a JSON failure unreadable, and so a success (ZB-090).
std::string one_shape(const std::string &result) {
    json j;   // the result as JSON, read once by result::failed
    if (result.compare(0, 5, "ERROR") == 0 || !result::failed(result, &j)) return result;
    if (!j.is_object()) return fail(result);   // {"error"... that is not JSON: as it came
    const json e = j["error"];
    std::string what = e.is_string() ? e.get<std::string>() : e.is_null() ? "" : e.dump(-1, ' ', false, json::error_handler_t::replace);
    if (what.empty()) what = "the tool failed without saying why";
    j.erase("error");
    return fail(what + (j.empty() ? "" : ". " + j.dump(-1, ' ', false, json::error_handler_t::replace)));
}

}  // namespace

bool failed(const std::string &text) { return result::failed(text); }

std::string run(const std::string &name, const std::string &raw_args, Context &ctx) {
    ctx.say_refused = false;
    const Tool *tool = find(name);
    if (!tool) return no_such_tool(name);
    const std::string raw = util::trim(raw_args).empty() ? "{}" : raw_args;
    // Nested deeper than any tool's arguments are: refused unread, as copying or printing them would
    // recurse once a level (ZB-093).
    if (util::json_too_deep(raw, util::kMaxJsonDepth))
        return fail("the arguments nest lists or objects more than " + std::to_string(util::kMaxJsonDepth) +
                    " deep, which no tool takes. Send the call again with one JSON object of named values, e.g. " + example_args(*tool));
    json args;
    try {
        args = json::parse(raw);
    } catch (const json::parse_error &e) {
        // Most often the call was cut off part way (the reply ran out of room): the parser then
        // stops past the end. Otherwise say where it broke.
        const size_t at = e.byte > 0 ? std::min<size_t>(e.byte, raw.size()) - 1 : 0;
        const std::string where = e.byte > raw.size() ? "they stop before the object is closed"
                                                      : "they break off at character " + std::to_string(at + 1) + ", near '" +
                                                            util::truncate_utf8(clean_text(raw.substr(at > 10 ? at - 10 : 0, 20)), 20) + "'";
        return fail("the arguments were not valid JSON (" + where + "). Send the call again with one complete JSON object, e.g. " +
                    example_args(*tool));
    }
    if (!args.is_object())
        return fail("the arguments must be one JSON object of named values, e.g. " + example_args(*tool) + ", not " + sent_text(args));
    const json *props = properties_of(*tool);
    std::vector<std::string> unknown;
    if (props) {
        const std::string wrong = check_args(*props, args, unknown);
        if (!wrong.empty()) return fail(wrong + unknown_note(*props, unknown));
    }
    try {
        std::string result = one_shape(util::sanitize_utf8(tool->run(args, ctx)));
        if (props && failed(result)) result += unknown_note(*props, unknown);
        else if (!failed(result) && !result::awaits_yes(result)) {
            const std::string action = util::lower(str_arg(args, "action"));
            ctx.worked.push_back(action.empty() ? name : name + ":" + action);
        }
        return result;
    } catch (const std::exception &e) {
        // nlohmann's messages begin with a code in brackets: "[json.exception.type_error.302] type must be ..."
        std::string what = e.what();
        const size_t close = what.find("] ");
        if (!what.empty() && what[0] == '[' && close != std::string::npos) what.erase(0, close + 2);
        return fail(name + " failed unexpectedly (" + util::truncate_utf8(clean_text(what), 120) +
                    "). If its arguments are right, tell the user it did not work rather than calling it again");
    }
}

}  // namespace tools
