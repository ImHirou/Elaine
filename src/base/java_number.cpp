// SPDX-License-Identifier: GPL-3.0-or-later
#include "base/java_number.hpp"

#include <charconv>
#include <cmath>
#include <cstring>

namespace base {

namespace {

// The digits of `v` (positive, finite, not zero) and the power of ten of the first one:
// d.ddd x 10^exp. The digits are the shortest that read back as `v`; where that is one digit,
// Java takes the two-digit decimal nearest to `v` among those that read back (Double.toString's
// contract: "4.9E-324", not "5.0E-324").
template <class T>
void shortest(T v, std::string& digits, int& exp) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    const auto split = [&](const char* end) {
        digits.clear();
        const char* e = buf;
        while (e < end && *e != 'e') {
            if (*e != '.') digits += *e;
            ++e;
        }
        exp = 0;
        std::from_chars(e + 1 + (e[1] == '+'), end, exp);
    };
    split(r.ptr);
    if (digits.size() > 1) return;
    char two[64];
    const auto r2 = std::to_chars(two, two + sizeof two, v, std::chars_format::scientific, 1);
    T back{};
    std::from_chars(two, r2.ptr, back);
    if (back != v) return;  // the nearest pair of digits is another number: the single digit stands
    std::memcpy(buf, two, static_cast<usize>(r2.ptr - two));
    split(buf + (r2.ptr - two));
}

template <class T>
std::string java_number(T v) {
    if (v != v) return "NaN";
    if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
    std::string out = std::signbit(v) ? "-" : "";
    const T a = std::fabs(v);
    if (a == 0) return out + "0.0";
    std::string digits;
    int exp = 0;
    shortest(a, digits, exp);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    if (exp >= -3 && exp < 7) {  // 10^-3 <= the decimal < 10^7: written out
        if (exp < 0) {           // 0.00ddd
            out += "0.";
            out.append(static_cast<usize>(-exp - 1), '0');
            out += digits;
        } else {
            const usize whole = static_cast<usize>(exp) + 1;
            if (digits.size() <= whole) {
                out += digits;
                out.append(whole - digits.size(), '0');
                out += ".0";
            } else {
                out.append(digits, 0, whole);
                out += '.';
                out.append(digits, whole);
            }
        }
        return out;
    }
    out += digits[0];
    out += '.';
    if (digits.size() > 1) {
        out.append(digits, 1);
    } else {
        out += '0';
    }
    out += 'E';
    out += std::to_string(exp);
    return out;
}

}  // namespace

std::string java_double(double v) { return java_number(v); }
std::string java_float(float v) { return java_number(v); }

std::string java_format_fixed(double v, u32 decimals) {
    if (v != v) return "NaN";
    if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
    char buf[400];  // the longest fixed form of a double has 309 digits before the point
    const auto r = std::to_chars(buf, buf + sizeof buf, v < 0 ? -v : v, std::chars_format::fixed);  // shortest digits
    std::string digits(buf, r.ptr);
    usize dot = digits.find('.');
    if (dot == std::string::npos) {
        dot = digits.size();
        digits += '.';
    }
    const bool up = digits.size() > dot + 1 + decimals && digits[dot + 1 + decimals] >= '5';
    digits.resize(dot + 1 + decimals, '0');
    if (up) {
        usize i = digits.size();
        while (i-- > 0) {
            if (digits[i] == '.') continue;
            if (digits[i] != '9') {
                ++digits[i];
                break;
            }
            digits[i] = '0';
            if (i == 0) digits.insert(digits.begin(), '1');
        }
    }
    if (decimals == 0) digits.pop_back();  // "%.0f" has no point
    return std::signbit(v) ? "-" + digits : digits;
}

}  // namespace base
