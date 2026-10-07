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

} // namespace

void pf_math_init() {
    pf_on_swi(0x50002, m_mulmanyvec3mat33);
}
