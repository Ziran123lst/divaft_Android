// SPDX-License-Identifier: MIT
// SSE4.2 PCMPISTRI/PCMPISTRM intermediate result (VPCMPISTRX), ported from FEX's
// StringCompareFallbacks/VectorFallbacks (MIT). Returns IntRes2 | (flags << 28).
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static int pcmp_el(const uint8_t* v, int i, uint16_t c) {
  switch (c & 3) {
    case 0: return v[i];
    case 1: { uint16_t x; memcpy(&x, v + 2*i, 2); return x; }
    case 2: return (int8_t)v[i];
    default: { int16_t x; memcpy(&x, v + 2*i, 2); return x; }
  }
}
static int pcmp_implen(const uint8_t* v, uint16_t c) {
  int len = 0;
  if (c & 1) { while (len < 8) { uint16_t x; memcpy(&x, v + 2*len, 2); if (!x) break; len++; } }
  else { while (len < 16 && v[len]) len++; }
  return len;
}
static uint32_t pcmp_equalany(const uint8_t* l, int vl, const uint8_t* r, int vr, uint16_t c) {
  uint32_t res = 0;
  for (int j = vr; j >= 0; j--) { res <<= 1; int rv = pcmp_el(r,j,c);
    for (int i = vl; i >= 0; i--) res |= (uint32_t)(rv == pcmp_el(l,i,c)); }
  return res;
}
static uint32_t pcmp_ranges(const uint8_t* l, int vl, const uint8_t* r, int vr, uint16_t c) {
  uint32_t res = 0;
  for (int j = vr; j >= 0; j--) { res <<= 1; int el = pcmp_el(r,j,c);
    for (int i = (vl-1)|1; i >= 0; i -= 2) { int up = pcmp_el(l,i,c), lo = pcmp_el(l,i-1,c);
      res |= (uint32_t)(up >= el && lo <= el); } }
  return res;
}
static uint32_t pcmp_equaleach(const uint8_t* l, int vl, const uint8_t* r, int vr, uint16_t c) {
  int ul = (16 >> (c&1)) - 1, mx = vl>vr?vl:vr, mn = vl<vr?vl:vr;
  uint32_t res = (1U << (ul - mx)) - 1; res <<= (mx - mn);
  for (int i = mn; i >= 0; i--) { res <<= 1; res |= (uint32_t)(pcmp_el(l,i,c) == pcmp_el(r,i,c)); }
  return res;
}
static uint32_t pcmp_equalordered(const uint8_t* l, int vl, const uint8_t* r, int vr, uint16_t c) {
  int ul = (16 >> (c&1)) - 1;
  if (vl == -1) return (2U << ul) - 1;
  uint32_t res = 0; int initial = vr == ul ? vr : vr - vl;
  for (int j = initial; j >= 0; j--) { res <<= 1; uint32_t val = 1;
    int start = (vr - j) < vl ? (vr - j) : vl;
    for (int i = start; i >= 0; i--) val &= (uint32_t)(pcmp_el(l,i,c) == pcmp_el(r,i+j,c));
    res |= val; }
  return res;
}
uint32_t bbr_pcmpistrx(uint64_t lhs_lo, uint64_t lhs_hi, uint64_t rhs_lo, uint64_t rhs_hi, uint32_t control) {
  uint8_t l[16], r[16];
  memcpy(l, &lhs_lo, 8); memcpy(l+8, &lhs_hi, 8);
  memcpy(r, &rhs_lo, 8); memcpy(r+8, &rhs_hi, 8);
  uint16_t c = (uint16_t)control;
  int vl = pcmp_implen(l,c) - 1, vr = pcmp_implen(r,c) - 1;
  uint32_t agg;
  switch ((c >> 2) & 3) {
    case 0: agg = pcmp_equalany(l,vl,r,vr,c); break;
    case 1: agg = pcmp_ranges(l,vl,r,vr,c); break;
    case 2: agg = pcmp_equaleach(l,vl,r,vr,c); break;
    default: agg = pcmp_equalordered(l,vl,r,vr,c); break;
  }
  int ul = (16 >> (c&1)) - 1;
  uint32_t flags = 0;
  if (vr < ul) flags |= 0b0100;
  if (vl < ul) flags |= 0b1000;
  uint32_t res;
  switch ((c >> 4) & 3) {
    case 1: res = agg ^ ((2U << ul) - 1); break;
    case 3: res = agg ^ ((1U << (vr + 1)) - 1); break;
    default: res = agg; break;
  }
  if (res != 0) flags |= 0b0010;
  if (res & 1) flags |= 0b0001;
  return res | (flags << 28);
}
