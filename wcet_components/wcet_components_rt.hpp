#pragma once

/**
 * @file wcet_components_rt.hpp
 *
 * Versões de runtime dos benchmarks analisados pelo Heptane em
 * wcet_components/{source,intermediate,sink}.c (MDH WCET Benchmark Suite:
 * bs.c/binary_search, jfdctint.c/jpeg_fdct_islow, sqrt.c/my_sqrt).
 *
 * Os .c originais não são incluídos diretamente porque:
 *   - cada um tem seu próprio main() (conflitaria com o main() do harness);
 *   - dependem de <annot.h>, que só existe na árvore do Heptane
 *     (ANNOT_MAXITER é um hint de análise estática, sem efeito em runtime);
 *   - jpeg_fdct_islow() opera sobre um buffer `static` global — instâncias
 *     concorrentes em cores diferentes corromperiam esse estado compartilhado.
 *
 * As funções abaixo são cópias fiéis da lógica original (mesmo número de
 * iterações/instruções — nenhum laço aqui depende dos dados de entrada, então
 * a contagem de ciclos medida pelo Heptane não muda). A única alteração real é
 * em dct_run(): o buffer que era `static DCTELEM dataptr[64]` (global,
 * file-scope) virou parâmetro fornecido pelo chamador, para que cada subtask
 * "intermediate" tenha seu próprio estado.
 */

#include <cstdint>

namespace wcet_rt {

// ---------------------------------------------------------------------------
//  source: bs.c / binary_search — busca binária num array de 15 elementos
// ---------------------------------------------------------------------------
struct BsData { int key; int value; };

inline int binary_search_run(int x) {
    static const BsData data[15] = {
        {1, 100}, {5, 200}, {6, 300}, {7, 700}, {8, 900}, {9, 250}, {10, 400},
        {11, 600}, {12, 800}, {13, 1500}, {14, 1200}, {15, 110}, {16, 140},
        {17, 133}, {18, 10}
    };

    int fvalue, mid, up, low;
    low = 0;
    up = 14;
    fvalue = -1; // all data are positive
    while (low <= up) {
        mid = (low + up) >> 1;
        if (data[mid].key == x) {
            up = low - 1;
            fvalue = data[mid].value;
        } else if (data[mid].key > x) {
            up = mid - 1;
        } else {
            low = mid + 1;
        }
    }
    return fvalue;
}

// ---------------------------------------------------------------------------
//  sink: sqrt.c / my_sqrt — raiz quadrada por série de Taylor
// ---------------------------------------------------------------------------
inline float my_fabs_run(float x) {
    if (x < 0) return -x;
    return x;
}

inline float sqrt_run(float val) {
    float  x = val / 10;
    float  dx;
    double diff;
    double min_tol = 0.00001;
    int    i, flag;

    flag = 0;
    if (val == 0) {
        x = 0;
    } else {
        for (i = 1; i < 20; i++) {
            if (!flag) {
                dx = (val - (x * x)) / (2.0f * x);
                x  = x + dx;
                diff = val - (x * x);
                if (my_fabs_run(static_cast<float>(diff)) <= min_tol)
                    flag = 1;
            } else {
                x = x;
            }
        }
    }
    return x;
}

// ---------------------------------------------------------------------------
//  intermediate: jfdctint.c / jpeg_fdct_islow — DCT direta do JPEG (8x8)
// ---------------------------------------------------------------------------
#define WCET_RT_DCTSIZE 8
#define WCET_RT_CONST_BITS 13
#define WCET_RT_PASS1_BITS 2
#define WCET_RT_FIX_0_298631336  ((std::int32_t)  2446)
#define WCET_RT_FIX_0_390180644  ((std::int32_t)  3196)
#define WCET_RT_FIX_0_541196100  ((std::int32_t)  4433)
#define WCET_RT_FIX_0_765366865  ((std::int32_t)  6270)
#define WCET_RT_FIX_0_899976223  ((std::int32_t)  7373)
#define WCET_RT_FIX_1_175875602  ((std::int32_t)  9633)
#define WCET_RT_FIX_1_501321110  ((std::int32_t) 12299)
#define WCET_RT_FIX_1_847759065  ((std::int32_t) 15137)
#define WCET_RT_FIX_1_961570560  ((std::int32_t) 16069)
#define WCET_RT_FIX_2_053119869  ((std::int32_t) 16819)
#define WCET_RT_FIX_2_562915447  ((std::int32_t) 20995)
#define WCET_RT_FIX_3_072711026  ((std::int32_t) 25172)
#define WCET_RT_MULTIPLY(var, cst) ((var) * (cst))
#define WCET_RT_RIGHT_SHIFT(x, shft) ((x) >> (shft))
#define WCET_RT_DESCALE(x, n) WCET_RT_RIGHT_SHIFT((x) + (((std::int32_t)1) << ((n)-1)), n)

inline void dct_run(int dataptr[64]) {
    std::int32_t tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    std::int32_t tmp10, tmp11, tmp12, tmp13;
    std::int32_t z1, z2, z3, z4, z5;
    int ctr;

    // Pass 1: process rows.
    for (ctr = WCET_RT_DCTSIZE - 1; ctr >= 0; ctr--) {
        tmp0 = dataptr[0] + dataptr[7];
        tmp7 = dataptr[0] - dataptr[7];
        tmp1 = dataptr[1] + dataptr[6];
        tmp6 = dataptr[1] - dataptr[6];
        tmp2 = dataptr[2] + dataptr[5];
        tmp5 = dataptr[2] - dataptr[5];
        tmp3 = dataptr[3] + dataptr[4];
        tmp4 = dataptr[3] - dataptr[4];

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        dataptr[0] = static_cast<int>((tmp10 + tmp11) << WCET_RT_PASS1_BITS);
        dataptr[4] = static_cast<int>((tmp10 - tmp11) << WCET_RT_PASS1_BITS);

        z1 = WCET_RT_MULTIPLY(tmp12 + tmp13, WCET_RT_FIX_0_541196100);
        dataptr[2] = static_cast<int>(WCET_RT_DESCALE(
            z1 + WCET_RT_MULTIPLY(tmp13, WCET_RT_FIX_0_765366865),
            WCET_RT_CONST_BITS - WCET_RT_PASS1_BITS));
        dataptr[6] = static_cast<int>(WCET_RT_DESCALE(
            z1 + WCET_RT_MULTIPLY(tmp12, -WCET_RT_FIX_1_847759065),
            WCET_RT_CONST_BITS - WCET_RT_PASS1_BITS));

        z1 = tmp4 + tmp7;
        z2 = tmp5 + tmp6;
        z3 = tmp4 + tmp6;
        z4 = tmp5 + tmp7;
        z5 = WCET_RT_MULTIPLY(z3 + z4, WCET_RT_FIX_1_175875602);

        tmp4 = WCET_RT_MULTIPLY(tmp4, WCET_RT_FIX_0_298631336);
        tmp5 = WCET_RT_MULTIPLY(tmp5, WCET_RT_FIX_2_053119869);
        tmp6 = WCET_RT_MULTIPLY(tmp6, WCET_RT_FIX_3_072711026);
        tmp7 = WCET_RT_MULTIPLY(tmp7, WCET_RT_FIX_1_501321110);
        z1 = WCET_RT_MULTIPLY(z1, -WCET_RT_FIX_0_899976223);
        z2 = WCET_RT_MULTIPLY(z2, -WCET_RT_FIX_2_562915447);
        z3 = WCET_RT_MULTIPLY(z3, -WCET_RT_FIX_1_961570560);
        z4 = WCET_RT_MULTIPLY(z4, -WCET_RT_FIX_0_390180644);

        z3 += z5;
        z4 += z5;

        dataptr[7] = static_cast<int>(WCET_RT_DESCALE(tmp4 + z1 + z3, WCET_RT_CONST_BITS - WCET_RT_PASS1_BITS));
        dataptr[5] = static_cast<int>(WCET_RT_DESCALE(tmp5 + z2 + z4, WCET_RT_CONST_BITS - WCET_RT_PASS1_BITS));
        dataptr[3] = static_cast<int>(WCET_RT_DESCALE(tmp6 + z2 + z3, WCET_RT_CONST_BITS - WCET_RT_PASS1_BITS));
        dataptr[1] = static_cast<int>(WCET_RT_DESCALE(tmp7 + z1 + z4, WCET_RT_CONST_BITS - WCET_RT_PASS1_BITS));
    }

    // Pass 2: process columns.
    for (ctr = WCET_RT_DCTSIZE - 1; ctr >= 0; ctr--) {
        tmp0 = dataptr[WCET_RT_DCTSIZE * 0] + dataptr[WCET_RT_DCTSIZE * 7];
        tmp7 = dataptr[WCET_RT_DCTSIZE * 0] - dataptr[WCET_RT_DCTSIZE * 7];
        tmp1 = dataptr[WCET_RT_DCTSIZE * 1] + dataptr[WCET_RT_DCTSIZE * 6];
        tmp6 = dataptr[WCET_RT_DCTSIZE * 1] - dataptr[WCET_RT_DCTSIZE * 6];
        tmp2 = dataptr[WCET_RT_DCTSIZE * 2] + dataptr[WCET_RT_DCTSIZE * 5];
        tmp5 = dataptr[WCET_RT_DCTSIZE * 2] - dataptr[WCET_RT_DCTSIZE * 5];
        tmp3 = dataptr[WCET_RT_DCTSIZE * 3] + dataptr[WCET_RT_DCTSIZE * 4];
        tmp4 = dataptr[WCET_RT_DCTSIZE * 3] - dataptr[WCET_RT_DCTSIZE * 4];

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        dataptr[WCET_RT_DCTSIZE * 0] = static_cast<int>(WCET_RT_DESCALE(tmp10 + tmp11, WCET_RT_PASS1_BITS));
        dataptr[WCET_RT_DCTSIZE * 4] = static_cast<int>(WCET_RT_DESCALE(tmp10 - tmp11, WCET_RT_PASS1_BITS));

        z1 = WCET_RT_MULTIPLY(tmp12 + tmp13, WCET_RT_FIX_0_541196100);
        dataptr[WCET_RT_DCTSIZE * 2] = static_cast<int>(WCET_RT_DESCALE(
            z1 + WCET_RT_MULTIPLY(tmp13, WCET_RT_FIX_0_765366865),
            WCET_RT_CONST_BITS + WCET_RT_PASS1_BITS));
        dataptr[WCET_RT_DCTSIZE * 6] = static_cast<int>(WCET_RT_DESCALE(
            z1 + WCET_RT_MULTIPLY(tmp12, -WCET_RT_FIX_1_847759065),
            WCET_RT_CONST_BITS + WCET_RT_PASS1_BITS));

        z1 = tmp4 + tmp7;
        z2 = tmp5 + tmp6;
        z3 = tmp4 + tmp6;
        z4 = tmp5 + tmp7;
        z5 = WCET_RT_MULTIPLY(z3 + z4, WCET_RT_FIX_1_175875602);

        tmp4 = WCET_RT_MULTIPLY(tmp4, WCET_RT_FIX_0_298631336);
        tmp5 = WCET_RT_MULTIPLY(tmp5, WCET_RT_FIX_2_053119869);
        tmp6 = WCET_RT_MULTIPLY(tmp6, WCET_RT_FIX_3_072711026);
        tmp7 = WCET_RT_MULTIPLY(tmp7, WCET_RT_FIX_1_501321110);
        z1 = WCET_RT_MULTIPLY(z1, -WCET_RT_FIX_0_899976223);
        z2 = WCET_RT_MULTIPLY(z2, -WCET_RT_FIX_2_562915447);
        z3 = WCET_RT_MULTIPLY(z3, -WCET_RT_FIX_1_961570560);
        z4 = WCET_RT_MULTIPLY(z4, -WCET_RT_FIX_0_390180644);

        z3 += z5;
        z4 += z5;

        dataptr[WCET_RT_DCTSIZE * 7] = static_cast<int>(WCET_RT_DESCALE(tmp4 + z1 + z3, WCET_RT_CONST_BITS + WCET_RT_PASS1_BITS));
        dataptr[WCET_RT_DCTSIZE * 5] = static_cast<int>(WCET_RT_DESCALE(tmp5 + z2 + z4, WCET_RT_CONST_BITS + WCET_RT_PASS1_BITS));
        dataptr[WCET_RT_DCTSIZE * 3] = static_cast<int>(WCET_RT_DESCALE(tmp6 + z2 + z3, WCET_RT_CONST_BITS + WCET_RT_PASS1_BITS));
        dataptr[WCET_RT_DCTSIZE * 1] = static_cast<int>(WCET_RT_DESCALE(tmp7 + z1 + z4, WCET_RT_CONST_BITS + WCET_RT_PASS1_BITS));
    }
}

#undef WCET_RT_DCTSIZE
#undef WCET_RT_CONST_BITS
#undef WCET_RT_PASS1_BITS
#undef WCET_RT_FIX_0_298631336
#undef WCET_RT_FIX_0_390180644
#undef WCET_RT_FIX_0_541196100
#undef WCET_RT_FIX_0_765366865
#undef WCET_RT_FIX_0_899976223
#undef WCET_RT_FIX_1_175875602
#undef WCET_RT_FIX_1_501321110
#undef WCET_RT_FIX_1_847759065
#undef WCET_RT_FIX_1_961570560
#undef WCET_RT_FIX_2_053119869
#undef WCET_RT_FIX_2_562915447
#undef WCET_RT_FIX_3_072711026
#undef WCET_RT_MULTIPLY
#undef WCET_RT_RIGHT_SHIFT
#undef WCET_RT_DESCALE

} // namespace wcet_rt
