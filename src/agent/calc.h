#pragma once
#include <string>

namespace calc {
// Evaluates an arithmetic expression. Returns false and sets err on a syntax or math error.
bool evaluate(const std::string &expression, double &result, std::string &err);
std::string format(double value);   // tidy decimal text without trailing zeros
}
