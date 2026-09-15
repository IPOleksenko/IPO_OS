#ifndef LIB_GL_MATH_H
#define LIB_GL_MATH_H

#include <stdint.h>
#include <stdbool.h>

static inline float gl_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float res;
    __asm__ __volatile__ ("fsqrt" : "=t" (res) : "0" (x));
    return res;
}

static inline double gl_sqrtd(double x) {
    if (x <= 0.0) return 0.0;
    double res;
    __asm__ __volatile__ ("fsqrt" : "=t" (res) : "0" (x));
    return res;
}

static inline float gl_sinf(float x) {
    float res;
    __asm__ __volatile__ ("fsin" : "=t" (res) : "0" (x));
    return res;
}

static inline float gl_cosf(float x) {
    float res;
    __asm__ __volatile__ ("fcos" : "=t" (res) : "0" (x));
    return res;
}

static inline double gl_tand(double x) {
    double sin_val, cos_val;
    __asm__ __volatile__ ("fsin" : "=t" (sin_val) : "0" (x));
    __asm__ __volatile__ ("fcos" : "=t" (cos_val) : "0" (x));
    if (cos_val == 0.0) return 1e9;
    return sin_val / cos_val;
}

static inline float gl_tanf(float x) {
    return (float)gl_tand((double)x);
}

static inline int gl_floor_to_int(float x) {
    int i = (int)x;
    return ((float)i > x) ? (i - 1) : i;
}

static inline int gl_ceil_to_int(float x) {
    int i = (int)x;
    return ((float)i < x) ? (i + 1) : i;
}

static inline float gl_minf(float a, float b) {
    return (a < b) ? a : b;
}

static inline float gl_maxf(float a, float b) {
    return (a > b) ? a : b;
}

/* Fast exponential approximation for fog factor f = exp(-density * d) */
static inline float gl_exp_neg(float x) {
    if (x < -16.0f) return 0.0f;
    if (x >= 0.0f) return 1.0f;
    /* Range reduction: exp(x) = (exp(x/8))^8 */
    float z = x * 0.125f;
    float sum = 1.0f + z * (1.0f + z * 0.5f * (1.0f + z * 0.3333333f * (1.0f + z * 0.25f * (1.0f + z * 0.2f * (1.0f + z * 0.1666667f)))));
    sum *= sum; /* ^2 */
    sum *= sum; /* ^4 */
    sum *= sum; /* ^8 */
    return (sum < 0.0f) ? 0.0f : ((sum > 1.0f) ? 1.0f : sum);
}

#endif /* LIB_GL_MATH_H */

