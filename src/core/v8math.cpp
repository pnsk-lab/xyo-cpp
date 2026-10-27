// xyo-cpp: V8 (fdlibm) 準拠の Math.asin/acos/atan/exp。
//
// Scratch (V8) の Math.* は fdlibm を同梱しており、glibc の libm とは
// 最終ビットが一致しないことがある。interp / JIT / AOT で同じ結果を返す
// ため、V8 14.6 の src/base/ieee754.cc から該当関数を移植した。
//
// ====================================================
// Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//
// Developed at SunSoft, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this
// software is freely granted, provided that this notice
// is preserved.
// ====================================================
//
// The following is adapted from fdlibm (http://www.netlib.org/fdlibm).
//
// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "v8math.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace xyo {
namespace v8math {
namespace {

inline uint64_t bits(double d) { return std::bit_cast<uint64_t>(d); }
inline double fromBits(uint64_t u) { return std::bit_cast<double>(u); }

}  // namespace

#define EXTRACT_WORDS(ix0, ix1, d)            \
    do {                                      \
        const uint64_t u_ = bits(d);          \
        (ix0) = int32_t(u_ >> 32);            \
        (ix1) = uint32_t(u_);                 \
    } while (0)
#define GET_HIGH_WORD(i, d) ((i) = int32_t(bits(d) >> 32))
#define GET_LOW_WORD(i, d) ((i) = uint32_t(bits(d)))
#define INSERT_WORDS(d, ix0, ix1)             \
    ((d) = fromBits((uint64_t(uint32_t(ix0)) << 32) | uint32_t(ix1)))
#define SET_HIGH_WORD(d, v)                   \
    ((d) = fromBits((bits(d) & 0xffffffffull) | (uint64_t(uint32_t(v)) << 32)))
#define SET_LOW_WORD(d, v)                    \
    ((d) = fromBits((bits(d) & 0xffffffff00000000ull) | uint32_t(v)))

double acos(double x) {
  static const double
      one = 1.00000000000000000000e+00,     /* 0x3FF00000, 0x00000000 */
      pi = 3.14159265358979311600e+00,      /* 0x400921FB, 0x54442D18 */
      pio2_hi = 1.57079632679489655800e+00, /* 0x3FF921FB, 0x54442D18 */
      pio2_lo = 6.12323399573676603587e-17, /* 0x3C91A626, 0x33145C07 */
      pS0 = 1.66666666666666657415e-01,     /* 0x3FC55555, 0x55555555 */
      pS1 = -3.25565818622400915405e-01,    /* 0xBFD4D612, 0x03EB6F7D */
      pS2 = 2.01212532134862925881e-01,     /* 0x3FC9C155, 0x0E884455 */
      pS3 = -4.00555345006794114027e-02,    /* 0xBFA48228, 0xB5688F3B */
      pS4 = 7.91534994289814532176e-04,     /* 0x3F49EFE0, 0x7501B288 */
      pS5 = 3.47933107596021167570e-05,     /* 0x3F023DE1, 0x0DFDF709 */
      qS1 = -2.40339491173441421878e+00,    /* 0xC0033A27, 0x1C8A2D4B */
      qS2 = 2.02094576023350569471e+00,     /* 0x40002AE5, 0x9C598AC8 */
      qS3 = -6.88283971605453293030e-01,    /* 0xBFE6066C, 0x1B8D0159 */
      qS4 = 7.70381505559019352791e-02;     /* 0x3FB3B8C5, 0xB12E9282 */

  double z, p, q, r, w, s, c, df;
  int32_t hx, ix;
  GET_HIGH_WORD(hx, x);
  ix = hx & 0x7FFFFFFF;
  if (ix >= 0x3FF00000) { /* |x| >= 1 */
    uint32_t lx;
    GET_LOW_WORD(lx, x);
    if (((ix - 0x3FF00000) | lx) == 0) { /* |x|==1 */
      if (hx > 0)
        return 0.0; /* acos(1) = 0  */
      else
        return pi + 2.0 * pio2_lo; /* acos(-1)= pi */
    }
    return std::numeric_limits<double>::quiet_NaN();  // acos(|x|>1) is NaN
  }
  if (ix < 0x3FE00000) {                            /* |x| < 0.5 */
    if (ix <= 0x3C600000) return pio2_hi + pio2_lo; /*if|x|<2**-57*/
    z = x * x;
    p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    r = p / q;
    return pio2_hi - (x - (pio2_lo - x * r));
  } else if (hx < 0) { /* x < -0.5 */
    z = (one + x) * 0.5;
    p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    s = std::sqrt(z);
    r = p / q;
    w = r * s - pio2_lo;
    return pi - 2.0 * (s + w);
  } else { /* x > 0.5 */
    z = (one - x) * 0.5;
    s = std::sqrt(z);
    df = s;
    SET_LOW_WORD(df, 0);
    c = (z - df * df) / (s + df);
    p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    r = p / q;
    w = r * s + c;
    return 2.0 * (df + w);
  }
}

double asin(double x) {
  static const double
      one = 1.00000000000000000000e+00, /* 0x3FF00000, 0x00000000 */
      huge = 1.000e+300,
      pio2_hi = 1.57079632679489655800e+00, /* 0x3FF921FB, 0x54442D18 */
      pio2_lo = 6.12323399573676603587e-17, /* 0x3C91A626, 0x33145C07 */
      pio4_hi = 7.85398163397448278999e-01, /* 0x3FE921FB, 0x54442D18 */
                                            /* coefficient for R(x^2) */
      pS0 = 1.66666666666666657415e-01,     /* 0x3FC55555, 0x55555555 */
      pS1 = -3.25565818622400915405e-01,    /* 0xBFD4D612, 0x03EB6F7D */
      pS2 = 2.01212532134862925881e-01,     /* 0x3FC9C155, 0x0E884455 */
      pS3 = -4.00555345006794114027e-02,    /* 0xBFA48228, 0xB5688F3B */
      pS4 = 7.91534994289814532176e-04,     /* 0x3F49EFE0, 0x7501B288 */
      pS5 = 3.47933107596021167570e-05,     /* 0x3F023DE1, 0x0DFDF709 */
      qS1 = -2.40339491173441421878e+00,    /* 0xC0033A27, 0x1C8A2D4B */
      qS2 = 2.02094576023350569471e+00,     /* 0x40002AE5, 0x9C598AC8 */
      qS3 = -6.88283971605453293030e-01,    /* 0xBFE6066C, 0x1B8D0159 */
      qS4 = 7.70381505559019352791e-02;     /* 0x3FB3B8C5, 0xB12E9282 */

  double t, w, p, q, c, r, s;
  int32_t hx, ix;

  t = 0;
  GET_HIGH_WORD(hx, x);
  ix = hx & 0x7FFFFFFF;
  if (ix >= 0x3FF00000) { /* |x|>= 1 */
    uint32_t lx;
    GET_LOW_WORD(lx, x);
    if (((ix - 0x3FF00000) | lx) == 0) { /* asin(1)=+-pi/2 with inexact */
      return x * pio2_hi + x * pio2_lo;
    }
    return std::numeric_limits<double>::quiet_NaN();  // asin(|x|>1) is NaN
  } else if (ix < 0x3FE00000) {     /* |x|<0.5 */
    if (ix < 0x3E400000) {          /* if |x| < 2**-27 */
      if (huge + x > one) return x; /* return x with inexact if x!=0*/
    } else {
      t = x * x;
    }
    p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
    q = one + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
    w = p / q;
    return x + x * w;
  }
  /* 1> |x|>= 0.5 */
  w = one - std::fabs(x);
  t = w * 0.5;
  p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
  q = one + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
  s = std::sqrt(t);
  if (ix >= 0x3FEF3333) { /* if |x| > 0.975 */
    w = p / q;
    t = pio2_hi - (2.0 * (s + s * w) - pio2_lo);
  } else {
    w = s;
    SET_LOW_WORD(w, 0);
    c = (t - w * w) / (s + w);
    r = p / q;
    p = 2.0 * s * r - (pio2_lo - 2.0 * c);
    q = pio4_hi - 2.0 * w;
    t = pio4_hi - (p - q);
  }
  if (hx > 0)
    return t;
  else
    return -t;
}

double atan(double x) {
  static const double atanhi[] = {
      4.63647609000806093515e-01, /* atan(0.5)hi 0x3FDDAC67, 0x0561BB4F */
      7.85398163397448278999e-01, /* atan(1.0)hi 0x3FE921FB, 0x54442D18 */
      9.82793723247329054082e-01, /* atan(1.5)hi 0x3FEF730B, 0xD281F69B */
      1.57079632679489655800e+00, /* atan(inf)hi 0x3FF921FB, 0x54442D18 */
  };

  static const double atanlo[] = {
      2.26987774529616870924e-17, /* atan(0.5)lo 0x3C7A2B7F, 0x222F65E2 */
      3.06161699786838301793e-17, /* atan(1.0)lo 0x3C81A626, 0x33145C07 */
      1.39033110312309984516e-17, /* atan(1.5)lo 0x3C700788, 0x7AF0CBBD */
      6.12323399573676603587e-17, /* atan(inf)lo 0x3C91A626, 0x33145C07 */
  };

  static const double aT[] = {
      3.33333333333329318027e-01,  /* 0x3FD55555, 0x5555550D */
      -1.99999999998764832476e-01, /* 0xBFC99999, 0x9998EBC4 */
      1.42857142725034663711e-01,  /* 0x3FC24924, 0x920083FF */
      -1.11111104054623557880e-01, /* 0xBFBC71C6, 0xFE231671 */
      9.09088713343650656196e-02,  /* 0x3FB745CD, 0xC54C206E */
      -7.69187620504482999495e-02, /* 0xBFB3B0F2, 0xAF749A6D */
      6.66107313738753120669e-02,  /* 0x3FB10D66, 0xA0D03D51 */
      -5.83357013379057348645e-02, /* 0xBFADDE2D, 0x52DEFD9A */
      4.97687799461593236017e-02,  /* 0x3FA97B4B, 0x24760DEB */
      -3.65315727442169155270e-02, /* 0xBFA2B444, 0x2C6A6C2F */
      1.62858201153657823623e-02,  /* 0x3F90AD3A, 0xE322DA11 */
  };

  static const double one = 1.0, huge = 1.0e300;

  double w, s1, s2, z;
  int32_t ix, hx, id;

  GET_HIGH_WORD(hx, x);
  ix = hx & 0x7FFFFFFF;
  if (ix >= 0x44100000) { /* if |x| >= 2^66 */
    uint32_t low;
    GET_LOW_WORD(low, x);
    if (ix > 0x7FF00000 || (ix == 0x7FF00000 && (low != 0)))
      return x + x; /* NaN */
    if (hx > 0)
      return atanhi[3] + *const_cast<volatile double*>(&atanlo[3]);
    else
      return -atanhi[3] - *const_cast<volatile double*>(&atanlo[3]);
  }
  if (ix < 0x3FDC0000) {            /* |x| < 0.4375 */
    if (ix < 0x3E400000) {          /* |x| < 2^-27 */
      if (huge + x > one) return x; /* raise inexact */
    }
    id = -1;
  } else {
    x = std::fabs(x);
    if (ix < 0x3FF30000) {   /* |x| < 1.1875 */
      if (ix < 0x3FE60000) { /* 7/16 <=|x|<11/16 */
        id = 0;
        x = (2.0 * x - one) / (2.0 + x);
      } else { /* 11/16<=|x|< 19/16 */
        id = 1;
        x = (x - one) / (x + one);
      }
    } else {
      if (ix < 0x40038000) { /* |x| < 2.4375 */
        id = 2;
        x = (x - 1.5) / (one + 1.5 * x);
      } else { /* 2.4375 <= |x| < 2^66 */
        id = 3;
        x = -1.0 / x;
      }
    }
  }
  /* end of argument reduction */
  z = x * x;
  w = z * z;
  /* break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly */
  s1 = z * (aT[0] +
            w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
  s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
  if (id < 0) {
    return x - x * (s1 + s2);
  } else {
    z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return (hx < 0) ? -z : z;
  }
}

double exp(double x) {
  static const double
      one = 1.0,
      halF[2] = {0.5, -0.5},
      o_threshold = 7.09782712893383973096e+02,  /* 0x40862E42, 0xFEFA39EF */
      u_threshold = -7.45133219101941108420e+02, /* 0xC0874910, 0xD52D3051 */
      ln2HI[2] = {6.93147180369123816490e-01,    /* 0x3FE62E42, 0xFEE00000 */
                  -6.93147180369123816490e-01},  /* 0xBFE62E42, 0xFEE00000 */
      ln2LO[2] = {1.90821492927058770002e-10,    /* 0x3DEA39EF, 0x35793C76 */
                  -1.90821492927058770002e-10},  /* 0xBDEA39EF, 0x35793C76 */
      invln2 = 1.44269504088896338700e+00,       /* 0x3FF71547, 0x652B82FE */
      P1 = 1.66666666666666019037e-01,           /* 0x3FC55555, 0x5555553E */
      P2 = -2.77777777770155933842e-03,          /* 0xBF66C16C, 0x16BEBD93 */
      P3 = 6.61375632143793436117e-05,           /* 0x3F11566A, 0xAF25DE2C */
      P4 = -1.65339022054652515390e-06,          /* 0xBEBBBD41, 0xC5D26BF1 */
      P5 = 4.13813679705723846039e-08,           /* 0x3E663769, 0x72BEA4D0 */
      E = 2.718281828459045;                     /* 0x4005BF0A, 0x8B145769 */

  static volatile double
      huge = 1.0e+300,
      twom1000 = 9.33263618503218878990e-302, /* 2**-1000=0x01700000,0*/
      two1023 = 8.988465674311579539e307;     /* 0x1p1023 */

  double y, hi = 0.0, lo = 0.0, c, t, twopk;
  int32_t k = 0, xsb;
  uint32_t hx;

  GET_HIGH_WORD(hx, x);
  xsb = (hx >> 31) & 1; /* sign bit of x */
  hx &= 0x7FFFFFFF;     /* high word of |x| */

  /* filter out non-finite argument */
  if (hx >= 0x40862E42) { /* if |x|>=709.78... */
    if (hx >= 0x7FF00000) {
      uint32_t lx;
      GET_LOW_WORD(lx, x);
      if (((hx & 0xFFFFF) | lx) != 0)
        return x + x; /* NaN */
      else
        return (xsb == 0) ? x : 0.0; /* exp(+-inf)={inf,0} */
    }
    if (x > o_threshold) return huge * huge;         /* overflow */
    if (x < u_threshold) return twom1000 * twom1000; /* underflow */
  }

  /* argument reduction */
  if (hx > 0x3FD62E42) {   /* if  |x| > 0.5 ln2 */
    if (hx < 0x3FF0A2B2) { /* and |x| < 1.5 ln2 */
      /* TODO(rtoy): We special case exp(1) here to return the correct
       * value of E, as the computation below would get the last bit
       * wrong. We should probably fix the algorithm instead.
       */
      if (x == 1.0) return E;
      hi = x - ln2HI[xsb];
      lo = ln2LO[xsb];
      k = 1 - xsb - xsb;
    } else {
      k = static_cast<int>(invln2 * x + halF[xsb]);
      t = k;
      hi = x - t * ln2HI[0]; /* t*ln2HI is exact here */
      lo = t * ln2LO[0];
    }
    x = hi - lo;
  } else if (hx < 0x3E300000) {         /* when |x|<2**-28 */
    if (huge + x > one) return one + x; /* trigger inexact */
  } else {
    k = 0;
  }

  /* x is now in primary range */
  t = x * x;
  if (k >= -1021) {
    INSERT_WORDS(
        twopk,
        0x3FF00000 + static_cast<int32_t>(static_cast<uint32_t>(k) << 20), 0);
  } else {
    INSERT_WORDS(twopk, 0x3FF00000 + (static_cast<uint32_t>(k + 1000) << 20),
                 0);
  }
  c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
  if (k == 0) {
    return one - ((x * c) / (c - 2.0) - x);
  } else {
    y = one - ((lo - (x * c) / (2.0 - c)) - hi);
  }
  if (k >= -1021) {
    if (k == 1024) return y * 2.0 * two1023;
    return y * twopk;
  } else {
    return y * twopk * twom1000;
  }
}



double atan2(double y, double x) {
  static volatile double tiny = 1.0e-300;
  static const double
      zero = 0.0,
      pi_o_4 = 7.8539816339744827900E-01, /* 0x3FE921FB, 0x54442D18 */
      pi_o_2 = 1.5707963267948965580E+00, /* 0x3FF921FB, 0x54442D18 */
      pi = 3.1415926535897931160E+00;     /* 0x400921FB, 0x54442D18 */
  static volatile double pi_lo =
      1.2246467991473531772E-16; /* 0x3CA1A626, 0x33145C07 */

  double z;
  int32_t k, m, hx, hy, ix, iy;
  uint32_t lx, ly;

  EXTRACT_WORDS(hx, lx, x);
  ix = hx & 0x7FFFFFFF;
  EXTRACT_WORDS(hy, ly, y);
  iy = hy & 0x7FFFFFFF;
  if (ix > 0x7FF00000 || (ix == 0x7FF00000 && lx != 0) ||
      iy > 0x7FF00000 || (iy == 0x7FF00000 && ly != 0)) {
    return x + y; /* x or y is NaN */
  }
  if (hx == 0x3FF00000 && lx == 0) {
    return atan(y); /* x=1.0 */
  }
  m = ((hy >> 31) & 1) | ((hx >> 30) & 2);           /* 2*sign(x)+sign(y) */

  /* when y = 0 */
  if ((iy | ly) == 0) {
    switch (m) {
      case 0:
      case 1:
        return y; /* atan(+-0,+anything)=+-0 */
      case 2:
        return pi + tiny; /* atan(+0,-anything) = pi */
      case 3:
        return -pi - tiny; /* atan(-0,-anything) =-pi */
    }
  }
  /* when x = 0 */
  if ((ix | lx) == 0) return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny;

  /* when x is INF */
  if (ix == 0x7FF00000) {
    if (iy == 0x7FF00000) {
      switch (m) {
        case 0:
          return pi_o_4 + tiny; /* atan(+INF,+INF) */
        case 1:
          return -pi_o_4 - tiny; /* atan(-INF,+INF) */
        case 2:
          return 3.0 * pi_o_4 + tiny; /*atan(+INF,-INF)*/
        case 3:
          return -3.0 * pi_o_4 - tiny; /*atan(-INF,-INF)*/
      }
    } else {
      switch (m) {
        case 0:
          return zero; /* atan(+...,+INF) */
        case 1:
          return -zero; /* atan(-...,+INF) */
        case 2:
          return pi + tiny; /* atan(+...,-INF) */
        case 3:
          return -pi - tiny; /* atan(-...,-INF) */
      }
    }
  }
  /* when y is INF */
  if (iy == 0x7FF00000) return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny;

  /* compute y/x */
  k = (iy - ix) >> 20;
  if (k > 60) { /* |y/x| >  2**60 */
    z = pi_o_2 + 0.5 * pi_lo;
    m &= 1;
  } else if (hx < 0 && k < -60) {
    z = 0.0; /* 0 > |y|/x > -2**-60 */
  } else {
    z = atan(fabs(y / x)); /* safe to do y/x */
  }
  switch (m) {
    case 0:
      return z; /* atan(+,+) */
    case 1:
      return -z; /* atan(-,+) */
    case 2:
      return pi - (z - pi_lo); /* atan(+,-) */
    default:                   /* case 3 */
      return (z - pi_lo) - pi; /* atan(-,-) */
  }
}

int32_t __kernel_rem_pio2(double* x, double* y, int e0, int nx, int prec,
                           const int32_t* ipio2);
int32_t __ieee754_rem_pio2(double x, double* y);
double __kernel_sin(double x, double y, int iy);
double __kernel_cos(double x, double y);
double sin(double x);
double cos(double x);

int32_t __kernel_rem_pio2(double *x, double *y, int e0, int nx, int prec,
                      const int32_t *ipio2) {
  /* Constants:
   * The hexadecimal values are the intended ones for the following
   * constants. The decimal values may be used, provided that the
   * compiler will convert from decimal to binary accurately enough
   * to produce the hexadecimal values shown.
   */
  static const int init_jk[] = {2, 3, 4, 6}; /* initial value for jk */

  static const double PIo2[] = {
      1.57079625129699707031e+00, /* 0x3FF921FB, 0x40000000 */
      7.54978941586159635335e-08, /* 0x3E74442D, 0x00000000 */
      5.39030252995776476554e-15, /* 0x3CF84698, 0x80000000 */
      3.28200341580791294123e-22, /* 0x3B78CC51, 0x60000000 */
      1.27065575308067607349e-29, /* 0x39F01B83, 0x80000000 */
      1.22933308981111328932e-36, /* 0x387A2520, 0x40000000 */
      2.73370053816464559624e-44, /* 0x36E38222, 0x80000000 */
      2.16741683877804819444e-51, /* 0x3569F31D, 0x00000000 */
  };

  static const double
      zero = 0.0,
      one = 1.0,
      two24 = 1.67772160000000000000e+07,  /* 0x41700000, 0x00000000 */
      twon24 = 5.96046447753906250000e-08; /* 0x3E700000, 0x00000000 */

  int32_t jz, jx, jv, jp, jk, carry, n, iq[20], i, j, k, m, q0, ih;
  double z, fw, f[20], fq[20], q[20];

  /* initialize jk*/
  jk = init_jk[prec];
  jp = jk;

  /* determine jx,jv,q0, note that 3>q0 */
  jx = nx - 1;
  jv = (e0 - 3) / 24;
  if (jv < 0) jv = 0;
  q0 = e0 - 24 * (jv + 1);

  /* set up f[0] to f[jx+jk] where f[jx+jk] = ipio2[jv+jk] */
  j = jv - jx;
  m = jx + jk;
  for (i = 0; i <= m; i++, j++) {
    f[i] = (j < 0) ? zero : static_cast<double>(ipio2[j]);
  }

  /* compute q[0],q[1],...q[jk] */
  for (i = 0; i <= jk; i++) {
    for (j = 0, fw = 0.0; j <= jx; j++) fw += x[j] * f[jx + i - j];
    q[i] = fw;
  }

  jz = jk;
recompute:
  /* distill q[] into iq[] reversingly */
  for (i = 0, j = jz, z = q[jz]; j > 0; i++, j--) {
    fw = static_cast<double>(static_cast<int32_t>(twon24 * z));
    iq[i] = static_cast<int32_t>(z - two24 * fw);
    z = q[j - 1] + fw;
  }

  /* compute n */
  z = std::scalbn(z, q0);           /* actual value of z */
  z -= 8.0 * std::floor(z * 0.125); /* trim off integer >= 8 */
  n = static_cast<int32_t>(z);
  z -= static_cast<double>(n);
  ih = 0;
  if (q0 > 0) { /* need iq[jz-1] to determine n */
    i = (iq[jz - 1] >> (24 - q0));
    n += i;
    iq[jz - 1] -= i << (24 - q0);
    ih = iq[jz - 1] >> (23 - q0);
  } else if (q0 == 0) {
    ih = iq[jz - 1] >> 23;
  } else if (z >= 0.5) {
    ih = 2;
  }

  if (ih > 0) { /* q > 0.5 */
    n += 1;
    carry = 0;
    for (i = 0; i < jz; i++) { /* compute 1-q */
      j = iq[i];
      if (carry == 0) {
        if (j != 0) {
          carry = 1;
          iq[i] = 0x1000000 - j;
        }
      } else {
        iq[i] = 0xFFFFFF - j;
      }
    }
    if (q0 > 0) { /* rare case: chance is 1 in 12 */
      switch (q0) {
        case 1:
          iq[jz - 1] &= 0x7FFFFF;
          break;
        case 2:
          iq[jz - 1] &= 0x3FFFFF;
          break;
      }
    }
    if (ih == 2) {
      z = one - z;
      if (carry != 0) z -= std::scalbn(one, q0);
    }
  }

  /* check if recomputation is needed */
  if (z == zero) {
    j = 0;
    for (i = jz - 1; i >= jk; i--) j |= iq[i];
    if (j == 0) { /* need recomputation */
      for (k = 1; jk >= k && iq[jk - k] == 0; k++) {
        /* k = no. of terms needed */
      }

      for (i = jz + 1; i <= jz + k; i++) { /* add q[jz+1] to q[jz+k] */
        f[jx + i] = ipio2[jv + i];
        for (j = 0, fw = 0.0; j <= jx; j++) fw += x[j] * f[jx + i - j];
        q[i] = fw;
      }
      jz += k;
      goto recompute;
    }
  }

  /* chop off zero terms */
  if (z == 0.0) {
    jz -= 1;
    q0 -= 24;
    while (iq[jz] == 0) {
      jz--;
      q0 -= 24;
    }
  } else { /* break z into 24-bit if necessary */
    z = std::scalbn(z, -q0);
    if (z >= two24) {
      fw = static_cast<double>(static_cast<int32_t>(twon24 * z));
      iq[jz] = z - two24 * fw;
      jz += 1;
      q0 += 24;
      iq[jz] = fw;
    } else {
      iq[jz] = z;
    }
  }

  /* convert integer "bit" chunk to floating-point value */
  fw = std::scalbn(one, q0);
  for (i = jz; i >= 0; i--) {
    q[i] = fw * iq[i];
    fw *= twon24;
  }

  /* compute PIo2[0,...,jp]*q[jz,...,0] */
  for (i = jz; i >= 0; i--) {
    for (fw = 0.0, k = 0; k <= jp && k <= jz - i; k++) fw += PIo2[k] * q[i + k];
    fq[jz - i] = fw;
  }

  /* compress fq[] into y[] */
  switch (prec) {
    case 0:
      fw = 0.0;
      for (i = jz; i >= 0; i--) fw += fq[i];
      y[0] = (ih == 0) ? fw : -fw;
      break;
    case 1:
    case 2:
      fw = 0.0;
      for (i = jz; i >= 0; i--) fw += fq[i];
      y[0] = (ih == 0) ? fw : -fw;
      fw = fq[0] - fw;
      for (i = 1; i <= jz; i++) fw += fq[i];
      y[1] = (ih == 0) ? fw : -fw;
      break;
    case 3: /* painful */
      for (i = jz; i > 0; i--) {
        fw = fq[i - 1] + fq[i];
        fq[i] += fq[i - 1] - fw;
        fq[i - 1] = fw;
      }
      for (i = jz; i > 1; i--) {
        fw = fq[i - 1] + fq[i];
        fq[i] += fq[i - 1] - fw;
        fq[i - 1] = fw;
      }
      for (fw = 0.0, i = jz; i >= 2; i--) fw += fq[i];
      if (ih == 0) {
        y[0] = fq[0];
        y[1] = fq[1];
        y[2] = fw;
      } else {
        y[0] = -fq[0];
        y[1] = -fq[1];
        y[2] = -fw;
      }
  }
  return n & 7;
}

int32_t __ieee754_rem_pio2(double x, double *y) {
  /*
   * Table of constants for 2/pi, 396 Hex digits (476 decimal) of 2/pi
   */
  static const int32_t two_over_pi[] = {
      0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62, 0x95993C,
      0x439041, 0xFE5163, 0xABDEBB, 0xC561B7, 0x246E3A, 0x424DD2, 0xE00649,
      0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129, 0xA73EE8, 0x8235F5, 0x2EBB44,
      0x84E99C, 0x7026B4, 0x5F7E41, 0x3991D6, 0x398353, 0x39F49C, 0x845F8B,
      0xBDF928, 0x3B1FF8, 0x97FFDE, 0x05980F, 0xEF2F11, 0x8B5A0A, 0x6D1F6D,
      0x367ECF, 0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5,
      0xF17B3D, 0x0739F7, 0x8A5292, 0xEA6BFB, 0x5FB11F, 0x8D5D08, 0x560330,
      0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3, 0x91615E, 0xE61B08,
      0x659985, 0x5F14A0, 0x68408D, 0xFFD880, 0x4D7327, 0x310606, 0x1556CA,
      0x73A8C9, 0x60E27B, 0xC08C6B,
  };

  static const int32_t npio2_hw[] = {
      0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C,
      0x4025FDBB, 0x402921FB, 0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C,
      0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB, 0x403AB41B, 0x403C463A,
      0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
      0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB,
      0x404858EB, 0x404921FB,
  };

  /*
   * invpio2:  53 bits of 2/pi
   * pio2_1:   first  33 bit of pi/2
   * pio2_1t:  pi/2 - pio2_1
   * pio2_2:   second 33 bit of pi/2
   * pio2_2t:  pi/2 - (pio2_1+pio2_2)
   * pio2_3:   third  33 bit of pi/2
   * pio2_3t:  pi/2 - (pio2_1+pio2_2+pio2_3)
   */

  static const double
      zero = 0.00000000000000000000e+00,    /* 0x00000000, 0x00000000 */
      half = 5.00000000000000000000e-01,    /* 0x3FE00000, 0x00000000 */
      two24 = 1.67772160000000000000e+07,   /* 0x41700000, 0x00000000 */
      invpio2 = 6.36619772367581382433e-01, /* 0x3FE45F30, 0x6DC9C883 */
      pio2_1 = 1.57079632673412561417e+00,  /* 0x3FF921FB, 0x54400000 */
      pio2_1t = 6.07710050650619224932e-11, /* 0x3DD0B461, 0x1A626331 */
      pio2_2 = 6.07710050630396597660e-11,  /* 0x3DD0B461, 0x1A600000 */
      pio2_2t = 2.02226624879595063154e-21, /* 0x3BA3198A, 0x2E037073 */
      pio2_3 = 2.02226624871116645580e-21,  /* 0x3BA3198A, 0x2E000000 */
      pio2_3t = 8.47842766036889956997e-32; /* 0x397B839A, 0x252049C1 */

  double z, w, t, r, fn;
  double tx[3];
  int32_t e0, i, j, nx, n, ix, hx;
  uint32_t low;

  z = 0;
  GET_HIGH_WORD(hx, x); /* high word of x */
  ix = hx & 0x7FFFFFFF;
  if (ix <= 0x3FE921FB) { /* |x| ~<= pi/4 , no need for reduction */
    y[0] = x;
    y[1] = 0;
    return 0;
  }
  if (ix < 0x4002D97C) { /* |x| < 3pi/4, special case with n=+-1 */
    if (hx > 0) {
      z = x - pio2_1;
      if (ix != 0x3FF921FB) { /* 33+53 bit pi is good enough */
        y[0] = z - pio2_1t;
        y[1] = (z - y[0]) - pio2_1t;
      } else { /* near pi/2, use 33+33+53 bit pi */
        z -= pio2_2;
        y[0] = z - pio2_2t;
        y[1] = (z - y[0]) - pio2_2t;
      }
      return 1;
    } else { /* negative x */
      z = x + pio2_1;
      if (ix != 0x3FF921FB) { /* 33+53 bit pi is good enough */
        y[0] = z + pio2_1t;
        y[1] = (z - y[0]) + pio2_1t;
      } else { /* near pi/2, use 33+33+53 bit pi */
        z += pio2_2;
        y[0] = z + pio2_2t;
        y[1] = (z - y[0]) + pio2_2t;
      }
      return -1;
    }
  }
  if (ix <= 0x413921FB) { /* |x| ~<= 2^19*(pi/2), medium size */
    t = std::fabs(x);
    n = static_cast<int32_t>(t * invpio2 + half);
    fn = static_cast<double>(n);
    r = t - fn * pio2_1;
    w = fn * pio2_1t; /* 1st round good to 85 bit */
    if (n < 32 && ix != npio2_hw[n - 1]) {
      y[0] = r - w; /* quick check no cancellation */
    } else {
      uint32_t high;
      j = ix >> 20;
      y[0] = r - w;
      GET_HIGH_WORD(high, y[0]);
      i = j - ((high >> 20) & 0x7FF);
      if (i > 16) { /* 2nd iteration needed, good to 118 */
        t = r;
        w = fn * pio2_2;
        r = t - w;
        w = fn * pio2_2t - ((t - r) - w);
        y[0] = r - w;
        GET_HIGH_WORD(high, y[0]);
        i = j - ((high >> 20) & 0x7FF);
        if (i > 49) { /* 3rd iteration need, 151 bits acc */
          t = r;      /* will cover all possible cases */
          w = fn * pio2_3;
          r = t - w;
          w = fn * pio2_3t - ((t - r) - w);
          y[0] = r - w;
        }
      }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) {
      y[0] = -y[0];
      y[1] = -y[1];
      return -n;
    } else {
      return n;
    }
  }
  /*
   * all other (large) arguments
   */
  if (ix >= 0x7FF00000) { /* x is inf or NaN */
    y[0] = y[1] = x - x;
    return 0;
  }
  /* set z = std::scalbn(|x|,ilogb(x)-23) */
  GET_LOW_WORD(low, x);
  SET_LOW_WORD(z, low);
  e0 = (ix >> 20) - 1046; /* e0 = ilogb(z)-23; */
  SET_HIGH_WORD(z, ix - static_cast<int32_t>(static_cast<uint32_t>(e0) << 20));
  for (i = 0; i < 2; i++) {
    tx[i] = static_cast<double>(static_cast<int32_t>(z));
    z = (z - tx[i]) * two24;
  }
  tx[2] = z;
  nx = 3;
  while (tx[nx - 1] == zero) nx--; /* skip zero term */
  n = __kernel_rem_pio2(tx, y, e0, nx, 2, two_over_pi);
  if (hx < 0) {
    y[0] = -y[0];
    y[1] = -y[1];
    return -n;
  }
  return n;
}

double __kernel_cos(double x, double y) {
  static const double
      one = 1.00000000000000000000e+00, /* 0x3FF00000, 0x00000000 */
      C1 = 4.16666666666666019037e-02,  /* 0x3FA55555, 0x5555554C */
      C2 = -1.38888888888741095749e-03, /* 0xBF56C16C, 0x16C15177 */
      C3 = 2.48015872894767294178e-05,  /* 0x3EFA01A0, 0x19CB1590 */
      C4 = -2.75573143513906633035e-07, /* 0xBE927E4F, 0x809C52AD */
      C5 = 2.08757232129817482790e-09,  /* 0x3E21EE9E, 0xBDB4B1C4 */
      C6 = -1.13596475577881948265e-11; /* 0xBDA8FAE9, 0xBE8838D4 */

  double a, iz, z, r, qx;
  int32_t ix;
  GET_HIGH_WORD(ix, x);
  ix &= 0x7FFFFFFF;                           /* ix = |x|'s high word*/
  if (ix < 0x3E400000) {                      /* if x < 2**27 */
    if (static_cast<int>(x) == 0) return one; /* generate inexact */
  }
  z = x * x;
  r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
  if (ix < 0x3FD33333) { /* if |x| < 0.3 */
    return one - (0.5 * z - (z * r - x * y));
  } else {
    if (ix > 0x3FE90000) { /* x > 0.78125 */
      qx = 0.28125;
    } else {
      INSERT_WORDS(qx, ix - 0x00200000, 0); /* x/4 */
    }
    iz = 0.5 * z - qx;
    a = one - qx;
    return a - (iz - (z * r - x * y));
  }
}

double __kernel_sin(double x, double y, int iy) {
  static const double
      half = 5.00000000000000000000e-01, /* 0x3FE00000, 0x00000000 */
      S1 = -1.66666666666666324348e-01,  /* 0xBFC55555, 0x55555549 */
      S2 = 8.33333333332248946124e-03,   /* 0x3F811111, 0x1110F8A6 */
      S3 = -1.98412698298579493134e-04,  /* 0xBF2A01A0, 0x19C161D5 */
      S4 = 2.75573137070700676789e-06,   /* 0x3EC71DE3, 0x57B1FE7D */
      S5 = -2.50507602534068634195e-08,  /* 0xBE5AE5E6, 0x8A2B9CEB */
      S6 = 1.58969099521155010221e-10;   /* 0x3DE5D93A, 0x5ACFD57C */

  double z, r, v;
  int32_t ix;
  GET_HIGH_WORD(ix, x);
  ix &= 0x7FFFFFFF;      /* high word of x */
  if (ix < 0x3E400000) { /* |x| < 2**-27 */
    if (static_cast<int>(x) == 0) return x;
  } /* generate inexact */
  z = x * x;
  v = z * x;
  r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
  if (iy == 0) {
    return x + v * (S1 + z * r);
  } else {
    return x - ((z * (half * y - v * r) - y) - v * S1);
  }
}

double cos(double x) {
  double y[2], z = 0.0;
  int32_t n, ix;

  /* High word of x. */
  GET_HIGH_WORD(ix, x);

  /* |x| ~< pi/4 */
  ix &= 0x7FFFFFFF;
  if (ix <= 0x3FE921FB) {
    return __kernel_cos(x, z);
  } else if (ix >= 0x7FF00000) {
    /* cos(Inf or NaN) is NaN */
    return x - x;
  } else {
    /* argument reduction needed */
    n = __ieee754_rem_pio2(x, y);
    switch (n & 3) {
      case 0:
        return __kernel_cos(y[0], y[1]);
      case 1:
        return -__kernel_sin(y[0], y[1], 1);
      case 2:
        return -__kernel_cos(y[0], y[1]);
      default:
        return __kernel_sin(y[0], y[1], 1);
    }
  }
}

double sin(double x) {
  double y[2], z = 0.0;
  int32_t n, ix;

  /* High word of x. */
  GET_HIGH_WORD(ix, x);

  /* |x| ~< pi/4 */
  ix &= 0x7FFFFFFF;
  if (ix <= 0x3FE921FB) {
    return __kernel_sin(x, z, 0);
  } else if (ix >= 0x7FF00000) {
    /* sin(Inf or NaN) is NaN */
    return x - x;
  } else {
    /* argument reduction needed */
    n = __ieee754_rem_pio2(x, y);
    switch (n & 3) {
      case 0:
        return __kernel_sin(y[0], y[1], 1);
      case 1:
        return __kernel_cos(y[0], y[1]);
      case 2:
        return -__kernel_sin(y[0], y[1], 1);
      default:
        return -__kernel_cos(y[0], y[1]);
    }
  }
}

}  // namespace v8math
}  // namespace xyo
