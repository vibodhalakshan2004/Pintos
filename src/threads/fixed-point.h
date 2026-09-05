#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

/* A signed 17.14 fixed-point number. */
typedef int32_t fixed_t;

/* Number of fractional bits and the resulting scaling factor. */
#define FP_Q 14
#define FP_F (1 << FP_Q)

/* Converts integer N to fixed-point representation. */
static inline fixed_t
fp_from_int (int n)
{
  return n * FP_F;
}

/* Converts fixed-point X to an integer, rounding toward zero. */
static inline int
fp_to_int_zero (fixed_t x)
{
  return x / FP_F;
}

/* Converts fixed-point X to the nearest integer. */
static inline int
fp_to_int_nearest (fixed_t x)
{
  if (x >= 0)
    return (x + FP_F / 2) / FP_F;
  else
    return (x - FP_F / 2) / FP_F;
}

/* Adds two fixed-point values. */
static inline fixed_t
fp_add (fixed_t x, fixed_t y)
{
  return x + y;
}

/* Subtracts fixed-point Y from fixed-point X. */
static inline fixed_t
fp_subtract (fixed_t x, fixed_t y)
{
  return x - y;
}

/* Adds integer N to fixed-point X. */
static inline fixed_t
fp_add_int (fixed_t x, int n)
{
  return x + n * FP_F;
}

/* Subtracts integer N from fixed-point X. */
static inline fixed_t
fp_subtract_int (fixed_t x, int n)
{
  return x - n * FP_F;
}

/* Multiplies two fixed-point values. */
static inline fixed_t
fp_multiply (fixed_t x, fixed_t y)
{
  return ((int64_t) x) * y / FP_F;
}

/* Multiplies fixed-point X by integer N. */
static inline fixed_t
fp_multiply_int (fixed_t x, int n)
{
  return x * n;
}

/* Divides fixed-point X by fixed-point Y. */
static inline fixed_t
fp_divide (fixed_t x, fixed_t y)
{
  return ((int64_t) x) * FP_F / y;
}

/* Divides fixed-point X by integer N. */
static inline fixed_t
fp_divide_int (fixed_t x, int n)
{
  return x / n;
}

#endif /* threads/fixed-point.h */
