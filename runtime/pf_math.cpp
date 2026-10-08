// 3dokit runtime -- the Operamath folio: the calls the programs run so far make, as the disc's
// System/Folios/OPERAMATH (V20.27, "Sat Aug 14 15:11:26 PDT 1993"; an uncompressed AIF image, the
// addresses its own) does them on the console.
//
// The folio picks its SWIs when it starts (0x528), by KernelBase's kb_MadamRev: MADAM revision 0
// ("Red") the table at 0x2ca8, revision 1 ("Green") 0x2d0c, but a Green wirewrap (kb_CPUFlags'
// KB_WIREWRAP) and any other revision the software routines, 0x2c44. On Red and Green silicon the
// vector and matrix calls load MADAM's matrix engine (0x3300600 on) and read its results back; the
// software routines instead add products each truncated toward zero (MulSF16, 0x2864: the
// magnitudes' product shifted down, then the sign), which can come out a unit or two lower in the
// last place. A retail console's MADAM is a Green one, so the engine's arithmetic is the one here:
// each output the 64-bit sum of the three products shifted down 16, rounding down -- what the
// Opera emulator's MADAM computes (madam_matrix_mul3x3); the SDK's documents give no precision.
// The folio also takes its "MathSemaphore" around the engine; one task runs at a time here.
#include "pf.h"

namespace {

// The engine's 3x3 product (MADAM_MATRIX_CONTROL 2): the vector v times the matrix m, row by row
// as operamath.h lays out a mat33f16 -- out[j] is the sum over i of v[i] * m[i][j]. The folio
// loads the matrix's columns into the engine's rows (0x1c60), which comes to the same.
void mul3(const int32_t m[9], const int32_t v[3], int32_t out[3]) {
    for (int j = 0; j < 3; ++j) {
        int64_t s = (int64_t)v[0] * m[j] + (int64_t)v[1] * m[3 + j] + (int64_t)v[2] * m[6 + j];
        out[j] = (int32_t)(s >> 16);
    }
}

void read3(uint32_t a, int32_t v[3]) {
    for (int i = 0; i < 3; ++i) v[i] = (int32_t)pf_r32(a + 4 * i);
}
void write3(uint32_t a, const int32_t v[3]) {
    for (int i = 0; i < 3; ++i) pf_w32(a + 4 * i, (uint32_t)v[i]);
}

// swi 0x50002: void MulManyVec3Mat33_F16(vec3f16* dest, vec3f16* src, mat33f16 mat, int32 count)
// -- Green 0x1ec4, Red 0x1dec: one vector (0x1c30) or a pipeline that reads vector k + 1 before it
// writes result k, so dest may be src. A count below 1 runs the pipeline about 2^32 times on the
// console (the software routine would do nothing): stopped here.
void m_mulmanyvec3mat33(ArmCpu& c) {
    uint32_t dest = c.r[0], src = c.r[1], mat = c.r[2];
    int32_t count = (int32_t)c.r[3];
    if (count < 1) pf_stop(c, "MulManyVec3Mat33_F16 of no vectors (the matrix engine's routine runs on): not yet");
    int32_t m[9], v[3], next[3] = {}, out[3];
    for (int i = 0; i < 9; ++i) m[i] = (int32_t)pf_r32(mat + 4 * i);
    read3(src, v);
    for (int32_t k = 0; k < count; ++k) {
        if (k + 1 < count) read3(src + 12 * (uint32_t)(k + 1), next);
        mul3(m, v, out);
        write3(dest + 12 * (uint32_t)k, out);
        for (int i = 0; i < 3; ++i) v[i] = next[i];
    }
}

// The engine's 4x4 product (23.10's operamath, Immercenary's disc: 0x1a54 one vector, 0x1b1c the
// pipeline): v times m as operamath.h lays out a mat44f16, out[j] the sum over i of v[i] * m[i][j]
// -- the folio loads m's columns into the engine's rows -- each the 64-bit sum shifted down 16, as
// the 3x3 one.
void mul4(const int32_t m[16], const int32_t v[4], int32_t out[4]) {
    for (int j = 0; j < 4; ++j) {
        int64_t s = 0;
        for (int i = 0; i < 4; ++i) s += (int64_t)v[i] * m[4 * i + j];
        out[j] = (int32_t)(s >> 16);
    }
}

// count vectors of four through the engine, src to dest: one (0x1a54), or the pipeline (0x1b1c),
// which reads vector k + 1 before it writes result k, so dest may be src. Below 1 the pipeline
// runs about 2^32 times on the console: stopped here.
void mul_many4(ArmCpu& c, uint32_t dest, uint32_t src, uint32_t mat, int32_t count, const char* who) {
    if (count < 1) {
        char why[96];
        std::snprintf(why, sizeof why, "%s of no vectors (the matrix engine's routine runs on): not yet", who);
        pf_stop(c, why);
    }
    int32_t m[16], v[4], next[4] = {}, out[4];
    for (int i = 0; i < 16; ++i) m[i] = (int32_t)pf_r32(mat + 4 * i);
    for (int i = 0; i < 4; ++i) v[i] = (int32_t)pf_r32(src + 4 * i);
    for (int32_t k = 0; k < count; ++k) {
        if (k + 1 < count)
            for (int i = 0; i < 4; ++i) next[i] = (int32_t)pf_r32(src + 16 * (uint32_t)(k + 1) + 4 * i);
        mul4(m, v, out);
        for (int i = 0; i < 4; ++i) pf_w32(dest + 16 * (uint32_t)k + 4 * i, (uint32_t)out[i]);
        for (int i = 0; i < 4; ++i) v[i] = next[i];
    }
}

// swi 0x50007: void MulVec4Mat44_F16(vec4f16 dest, vec4f16 vec, mat44f16 mat) -- 0x1a54.
void m_mulvec4mat44(ArmCpu& c) { mul_many4(c, c.r[0], c.r[1], c.r[2], 1, "MulVec4Mat44_F16"); }
// swi 0x50008: void MulMat44Mat44_F16(mat44f16 dest, mat44f16 src1, mat44f16 src2) -- 0x1b18:
// the pipeline over src1's four rows.
void m_mulmat44mat44(ArmCpu& c) { mul_many4(c, c.r[0], c.r[1], c.r[2], 4, "MulMat44Mat44_F16"); }
// swi 0x50009: void MulManyVec4Mat44_F16(vec4f16* dest, vec4f16* src, mat44f16 mat, int32 count).
void m_mulmanyvec4mat44(ArmCpu& c) {
    mul_many4(c, c.r[0], c.r[1], c.r[2], (int32_t)c.r[3], "MulManyVec4Mat44_F16");
}

// ---- the folio's vectors: code that runs in the caller (23.10's operamath, Immercenary's disc;
// its eight user functions at 0x25a0, -4 the last) ----------------------------------------
// DivUF16 (0x1c48): n / d in unsigned 16.16, a restoring division -- the integer part's bits
// from bit 15 (from bit 7 when d > n >> 8, none when d > n), then sixteen fraction bits: the
// quotient floor(n * 65536 / d) in r0, what is left in r1. When d <= n >> 16 (d 0 among them) the
// quotient would not fit: r0 and r1 both -1.
void div_uf16(ArmCpu& c, uint32_t n, uint32_t d) {
    if (d <= n >> 16) {
        c.r[0] = c.r[1] = 0xFFFFFFFFu;
        return;
    }
    uint64_t num = (uint64_t)n << 16;
    c.r[0] = (uint32_t)(num / d);
    c.r[1] = (uint32_t)(num % d);
}

// Operamath -12: ufrac16 DivUF16(ufrac16 n, ufrac16 d).
void m_divuf16(ArmCpu& c) { div_uf16(c, c.r[0], c.r[1]); }

// Operamath -28: ufrac16 RecipUF16(ufrac16 d) -- 0x1c40: DivUF16 of 1.0 by d.
void m_recipuf16(ArmCpu& c) { div_uf16(c, 0x10000u, c.r[0]); }

// Operamath -16: ufrac16 DivRemUF16(ufrac16* rem, ufrac16 n, ufrac16 d) -- 0x1c24: DivUF16, what is
// left stored at rem.
void m_divremuf16(ArmCpu& c) {
    uint32_t rem = c.r[0];
    div_uf16(c, c.r[1], c.r[2]);
    pf_w32(rem, c.r[1]);
}

// DivSF16 (0x1e5c): the same on the magnitudes, the integer part's bits from bit 14 -- so when
// |d| <= |n| >> 15 (d 0 among them) r0 and r1 are both 0x7FFFFFFF, whatever the signs; otherwise
// the quotient negated when the signs differ, what is left negated when n is negative.
void div_sf16(ArmCpu& c, uint32_t n, uint32_t d) {
    bool nneg = n >> 31, qneg = (n ^ d) >> 31;
    uint32_t an = nneg ? 0u - n : n, ad = d >> 31 ? 0u - d : d;
    if (ad <= an >> 15) {
        c.r[0] = c.r[1] = 0x7FFFFFFFu;
        return;
    }
    uint64_t num = (uint64_t)an << 16;
    uint32_t q = (uint32_t)(num / ad), r = (uint32_t)(num % ad);
    c.r[0] = qneg ? 0u - q : q;
    c.r[1] = nneg ? 0u - r : r;
}

// MulUF16 (0x2068): a * b >> 16 from the halves, al * bl >> 16 plus a * bh plus ah * bl, in 32
// bits -- floor(a * b / 65536), its low 32 bits.
uint32_t mul_uf16(uint32_t a, uint32_t b) {
    uint32_t ah = a >> 16, bh = b >> 16, al = a & 0xFFFF, bl = b & 0xFFFF;
    return bh * a + ((al * bl) >> 16) + ah * bl;
}

void m_divsf16(ArmCpu& c) { div_sf16(c, c.r[0], c.r[1]); }     // Operamath -20
void m_recipsf16(ArmCpu& c) { div_sf16(c, 0x10000u, c.r[0]); } // -32, 0x1e54

// Operamath -24: frac16 DivRemSF16(frac16* rem, frac16 n, frac16 d) -- 0x1e38.
void m_divremsf16(ArmCpu& c) {
    uint32_t rem = c.r[0];
    div_sf16(c, c.r[1], c.r[2]);
    pf_w32(rem, c.r[1]);
}

// Operamath -4: ufrac16 MulUF16(ufrac16 a, ufrac16 b).
void m_muluf16(ArmCpu& c) { c.r[0] = mul_uf16(c.r[0], c.r[1]); }

// Operamath -8: frac16 MulSF16(frac16 a, frac16 b) -- 0x208c: MulUF16 of the magnitudes, negated
// when the signs differ (toward zero).
void m_mulsf16(ArmCpu& c) {
    uint32_t a = c.r[0], b = c.r[1];
    uint32_t p = mul_uf16(a >> 31 ? 0u - a : a, b >> 31 ? 0u - b : b);
    c.r[0] = (a ^ b) >> 31 ? 0u - p : p;
}

} // namespace

void pf_math_init() {
    pf_on_swi(0x50002, m_mulmanyvec3mat33);
    pf_on_swi(0x50007, m_mulvec4mat44);
    pf_on_swi(0x50008, m_mulmat44mat44);
    pf_on_swi(0x50009, m_mulmanyvec4mat44);
    pf_on_slot(PF_MATH, -4, m_muluf16);
    pf_on_slot(PF_MATH, -8, m_mulsf16);
    pf_on_slot(PF_MATH, -12, m_divuf16);
    pf_on_slot(PF_MATH, -16, m_divremuf16);
    pf_on_slot(PF_MATH, -20, m_divsf16);
    pf_on_slot(PF_MATH, -24, m_divremsf16);
    pf_on_slot(PF_MATH, -28, m_recipuf16);
    pf_on_slot(PF_MATH, -32, m_recipsf16);
}
