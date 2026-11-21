#pragma once

// WinterWolf's own math functions (o-complex-linux): the same bits on every system and every
// processor, where each system's C library gives its own last digit. Each is within an ulp of
// the true value, almost always the nearest double (tools/fmath/check.py measures them against
// Python's decimal). Special values (NaN, infinities, zeros and their signs) as C's Annex F
// asks. Built of nothing but +, -, *, / and sqrt, with no multiply and add fused into one step
// (src/fmath.cpp turns that off), so x86-64 and ARM64 give the same.
namespace ww::fmath {

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double exp(double x);
double expm1(double x);
double log(double x);      // the natural logarithm
double log2(double x);     // exact at powers of 2
double log10(double x);    // exact at powers of 10
double log1p(double x);
double logBase(double x, double base);  // ln x / ln base, worked out as one

double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);

double pow(double x, double y);
double hypot(double x, double y);

}  // namespace ww::fmath
