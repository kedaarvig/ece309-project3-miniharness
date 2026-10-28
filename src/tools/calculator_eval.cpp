// src/tools/calculator_eval.cpp
//
// Two-stack evaluation: numbers go on `values`, operators and '(' on `ops`.
// Everything is a loop, so a long or deeply nested expression can't blow the
// call stack.

#include "tools/calculator.h"

#include <vector>

namespace {

using Kind = CalcToken::Kind;

// '(' gets 0 on purpose: no operator ever outranks it, so the apply loop
// below stops there without a special case.
int precedence(Kind k) {
    switch (k) {
        case Kind::Times:
        case Kind::Divide: return 2;
        case Kind::Plus:
        case Kind::Minus:  return 1;
        default:           return 0;
    }
}

// Pops one operator and its two operands and pushes the result.
bool apply_top(std::vector<double>& values, std::vector<Kind>& ops, std::string* error) {
    if (ops.empty() || values.size() < 2) {
        *error = "malformed expression";
        return false;
    }
    Kind op = ops.back();
    ops.pop_back();
    double right = values.back();
    values.pop_back();
    double left = values.back();
    values.pop_back();

    double result = 0.0;
    switch (op) {
        case Kind::Plus:  result = left + right; break;
        case Kind::Minus: result = left - right; break;
        case Kind::Times: result = left * right; break;
        case Kind::Divide:
            if (right == 0.0) {
                *error = "division by zero";
                return false;
            }
            result = left / right;
            break;
        default:   // a '(' that never got closed
            *error = "malformed expression";
            return false;
    }
    values.push_back(result);
    return true;
}

}  // namespace

bool calc_evaluate(const std::vector<CalcToken>& tokens, double* out, std::string* error) {
    std::vector<double> values;
    std::vector<Kind> ops;

    for (const CalcToken& t : tokens) {
        switch (t.kind) {
            case Kind::Number:
                values.push_back(t.value);
                break;
            case Kind::LParen:
                ops.push_back(Kind::LParen);
                break;
            case Kind::RParen:
                while (!ops.empty() && ops.back() != Kind::LParen) {
                    if (!apply_top(values, ops, error)) return false;
                }
                if (ops.empty()) {
                    *error = "unbalanced parentheses";
                    return false;
                }
                ops.pop_back();
                break;
            case Kind::Plus:
            case Kind::Minus:
            case Kind::Times:
            case Kind::Divide:
                // >= and not >, or 10-4-3 would come out as 10-(4-3)
                while (!ops.empty() && precedence(ops.back()) >= precedence(t.kind)) {
                    if (!apply_top(values, ops, error)) return false;
                }
                ops.push_back(t.kind);
                break;
        }
    }

    while (!ops.empty()) {
        if (!apply_top(values, ops, error)) return false;
    }
    if (values.size() != 1) {
        *error = "malformed expression";
        return false;
    }
    *out = values.back();
    return true;
}
