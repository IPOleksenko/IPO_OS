#ifndef _MATH_H
#define _MATH_H

#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#define HUGE_VAL   (__builtin_huge_val())
#define HUGE_VALF  (__builtin_huge_valf())
#define HUGE_VALL  (__builtin_huge_vall())
#define INFINITY   (__builtin_inff())
#define NAN        (__builtin_nanf(""))

#define isnan(x)   __builtin_isnan(x)
#define isinf(x)   __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)

#ifdef __cplusplus
extern "C" {
#endif

double acos(double x);
double asin(double x);
double atan(double x);
double atan2(double y, double x);
double cos(double x);
double sin(double x);
double tan(double x);
double cosh(double x);
double sinh(double x);
double tanh(double x);
double exp(double x);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
long double ldexpl(long double x, int exp);
double log(double x);
double log10(double x);
double modf(double x, double *iptr);
double pow(double x, double y);
double sqrt(double x);
double ceil(double x);
double fabs(double x);
double floor(double x);
double fmod(double x, double y);
double round(double x);
double trunc(double x);
void sincos(double x, double *sin_out, double *cos_out);
double nearbyint(double x);
double log2(double x);
double expm1(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double erf(double x);
double erfc(double x);
double tgamma(double x);
double lgamma(double x);

float sinf(float x);
float cosf(float x);
float tanf(float x);
float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float expf(float x);
float logf(float x);
float powf(float x, float y);

#ifdef __cplusplus
}
#endif

#endif
