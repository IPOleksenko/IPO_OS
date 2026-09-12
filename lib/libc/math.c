#include <math.h>
#include <stdint.h>

double fabs(double x) {
    double res;
    __asm__ __volatile__ ("fabs" : "=t" (res) : "0" (x));
    return res;
}

float fabsf(float x) {
    return (float)fabs((double)x);
}

double sqrt(double x) {
    if (x < 0.0) return NAN;
    double res;
    __asm__ __volatile__ ("fsqrt" : "=t" (res) : "0" (x));
    return res;
}

float sqrtf(float x) {
    return (float)sqrt((double)x);
}

double sin(double x) {
    double res;
    __asm__ __volatile__ ("fsin" : "=t" (res) : "0" (x));
    return res;
}

float sinf(float x) {
    return (float)sin((double)x);
}

double cos(double x) {
    double res;
    __asm__ __volatile__ ("fcos" : "=t" (res) : "0" (x));
    return res;
}

float cosf(float x) {
    return (float)cos((double)x);
}

double tan(double x) {
    double sin_val, cos_val;
    sin_val = sin(x);
    cos_val = cos(x);
    if (cos_val == 0.0) return HUGE_VAL;
    return sin_val / cos_val;
}

float tanf(float x) {
    return (float)tan((double)x);
}

double atan(double x) {
    double res;
    __asm__ __volatile__ (
        "fld1\n\t"
        "fpatan"
        : "=t" (res) : "0" (x) : "st(1)"
    );
    return res;
}

double atan2(double y, double x) {
    double res;
    __asm__ __volatile__ (
        "fpatan"
        : "=t" (res) : "0" (x), "u" (y) : "st(1)"
    );
    return res;
}

double asin(double x) {
    if (x < -1.0 || x > 1.0) return NAN;
    if (x == 1.0) return M_PI_2;
    if (x == -1.0) return -M_PI_2;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x < -1.0 || x > 1.0) return NAN;
    return M_PI_2 - asin(x);
}

double log(double x) {
    if (x < 0.0) return NAN;
    if (x == 0.0) return -HUGE_VAL;
    double res;
    __asm__ __volatile__ (
        "fldln2\n\t"
        "fxch\n\t"
        "fyl2x"
        : "=t" (res) : "0" (x) : "st(1)"
    );
    return res;
}

float logf(float x) {
    return (float)log((double)x);
}

double log10(double x) {
    if (x < 0.0) return NAN;
    if (x == 0.0) return -HUGE_VAL;
    double res;
    __asm__ __volatile__ (
        "fldlg2\n\t"
        "fxch\n\t"
        "fyl2x"
        : "=t" (res) : "0" (x) : "st(1)"
    );
    return res;
}

double exp(double x) {
    if (x == 0.0) return 1.0;
    if (isnan(x)) return NAN;
    if (x > 709.78) return HUGE_VAL;
    if (x < -708.39) return 0.0;

    // e^x = 2^(x * log2(e))
    double val = x * M_LOG2E;
    double int_part;
    double frac_part = modf(val, &int_part);

    double res;
    __asm__ __volatile__ (
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp"
        : "=t" (res) : "0" (frac_part) : "st(1)"
    );

    return ldexp(res, (int)int_part);
}

float expf(float x) {
    return (float)exp((double)x);
}

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return (y > 0.0) ? 0.0 : HUGE_VAL;
    if (x == 1.0) return 1.0;
    if (isnan(x) || isnan(y)) return NAN;

    if (x < 0.0) {
        double int_part;
        if (modf(y, &int_part) != 0.0) return NAN; // Fractional power of negative number
        double res = exp(y * log(-x));
        if ((long long)y % 2 != 0) res = -res;
        return res;
    }

    return exp(y * log(x));
}

float powf(float x, float y) {
    return (float)pow((double)x, (double)y);
}

double sinh(double x) {
    return (exp(x) - exp(-x)) / 2.0;
}

double cosh(double x) {
    return (exp(x) + exp(-x)) / 2.0;
}

double tanh(double x) {
    double e1 = exp(x);
    double e2 = exp(-x);
    return (e1 - e2) / (e1 + e2);
}

double floor(double x) {
    uint16_t old_cw, new_cw;
    double res;
    __asm__ __volatile__ ("fnstcw %0" : "=m" (old_cw));
    new_cw = (old_cw & ~0x0c00) | 0x0400; // Round down
    __asm__ __volatile__ ("fldcw %0" : : "m" (new_cw));
    __asm__ __volatile__ ("frndint" : "=t" (res) : "0" (x));
    __asm__ __volatile__ ("fldcw %0" : : "m" (old_cw));
    return res;
}

float floorf(float x) {
    return (float)floor((double)x);
}

double ceil(double x) {
    uint16_t old_cw, new_cw;
    double res;
    __asm__ __volatile__ ("fnstcw %0" : "=m" (old_cw));
    new_cw = (old_cw & ~0x0c00) | 0x0800; // Round up
    __asm__ __volatile__ ("fldcw %0" : : "m" (new_cw));
    __asm__ __volatile__ ("frndint" : "=t" (res) : "0" (x));
    __asm__ __volatile__ ("fldcw %0" : : "m" (old_cw));
    return res;
}

float ceilf(float x) {
    return (float)ceil((double)x);
}

double trunc(double x) {
    uint16_t old_cw, new_cw;
    double res;
    __asm__ __volatile__ ("fnstcw %0" : "=m" (old_cw));
    new_cw = (old_cw & ~0x0c00) | 0x0c00; // Chop / truncate
    __asm__ __volatile__ ("fldcw %0" : : "m" (new_cw));
    __asm__ __volatile__ ("frndint" : "=t" (res) : "0" (x));
    __asm__ __volatile__ ("fldcw %0" : : "m" (old_cw));
    return res;
}

double round(double x) {
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

double fmod(double x, double y) {
    if (y == 0.0) return NAN;
    double res;
    __asm__ __volatile__ (
        "1: fprem\n\t"
        "fnstsw %%ax\n\t"
        "sahf\n\t"
        "jp 1b"
        : "=t" (res) : "0" (x), "u" (y) : "ax", "cc"
    );
    return res;
}

double modf(double x, double *iptr) {
    double int_part = trunc(x);
    if (iptr) *iptr = int_part;
    return x - int_part;
}

double ldexp(double x, int exp) {
    double res;
    __asm__ __volatile__ (
        "fscale"
        : "=t" (res) : "0" (x), "u" ((double)exp)
    );
    return res;
}

long double ldexpl(long double x, int exp) {
    return (long double)ldexp((double)x, exp);
}

double frexp(double x, int *exp) {
    if (x == 0.0) {
        if (exp) *exp = 0;
        return 0.0;
    }
    union {
        double d;
        uint64_t u;
    } val;
    val.d = x;
    int e = (int)((val.u >> 52) & 0x7FF) - 1022;
    val.u = (val.u & ~0x7FF0000000000000ULL) | (0x3FEULL << 52);
    if (exp) *exp = e;
    return val.d;
}

void sincos(double x, double *sin_out, double *cos_out) {
    if (sin_out) *sin_out = sin(x);
    if (cos_out) *cos_out = cos(x);
}

double nearbyint(double x) {
    return round(x);
}

double log2(double x) {
    return log(x) / M_LN2;
}

double expm1(double x) {
    return exp(x) - 1.0;
}

double asinh(double x) {
    return log(x + sqrt(x * x + 1.0));
}

double acosh(double x) {
    if (x < 1.0) return NAN;
    return log(x + sqrt(x * x - 1.0));
}

double atanh(double x) {
    if (x <= -1.0 || x >= 1.0) return NAN;
    return 0.5 * log((1.0 + x) / (1.0 - x));
}

double erf(double x) {
    double a1 =  0.254829592;
    double a2 = -0.284496736;
    double a3 =  1.421413741;
    double a4 = -1.453152027;
    double a5 =  1.061405429;
    double p  =  0.3275911;

    int sign = (x < 0.0) ? -1 : 1;
    double abs_x = fabs(x);
    double t = 1.0 / (1.0 + p * abs_x);
    double y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * exp(-abs_x * abs_x);
    return sign * y;
}

double erfc(double x) {
    return 1.0 - erf(x);
}

double tgamma(double x) {
    if (x <= 0.0 && floor(x) == x) return NAN;
    static const double p[] = {
        0.99999999999980993, 676.5203681218851, -1259.1392167224028,
        771.32342877765313, -176.61502916214059, 12.507343278686905,
        -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7
    };
    if (x < 0.5) {
        return M_PI / (sin(M_PI * x) * tgamma(1.0 - x));
    }
    x -= 1.0;
    double a = p[0];
    double t = x + 7.5;
    for (int i = 1; i < 9; i++) {
        a += p[i] / (x + (double)i);
    }
    return sqrt(2.0 * M_PI) * pow(t, x + 0.5) * exp(-t) * a;
}

double lgamma(double x) {
    return log(fabs(tgamma(x)));
}

