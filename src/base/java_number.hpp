// SPDX-License-Identifier: GPL-3.0-or-later
// Numbers as Java prints them. Vanilla puts numbers into text a client is sent - a coordinate in
// a command's answer, a float in an NBT tag that is shown - by String.valueOf and String.format,
// and a client that shows our text beside vanilla's must see the same digits.
#pragma once

#include <string>

#include "base/types.hpp"

namespace base {

// Double.toString / Float.toString (the JDK since 19): the shortest digits that read back as
// the same value - two of them at least, the nearest such pair where one would do - as
// "123.45" from a thousandth up to ten million and as "1.2345E-5" outside; "NaN", "Infinity",
// "-0.0".
std::string java_double(double v);
std::string java_float(float v);

// String.format(Locale.ROOT, "%.<decimals>f", v): rounded half up from the shortest decimal
// form of the double - java.util.Formatter does not round the binary value, as printf does
// ("%.2f" of 1.005 is "1.01").
std::string java_format_fixed(double v, u32 decimals);

}  // namespace base
