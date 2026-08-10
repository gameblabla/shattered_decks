/*
 * Fixed-point 32-band synthesis transform, derived from kjmp2 1.1's
 * factorisation (Martin J. Fiedler) and adjusted only for this decoder's
 * signed 32-bit sample bank.  Keep this separate from the parser so the
 * Towns path remains limited to ordinary 32-bit and 16x16 products.
 */
#define DCT_COS0_0 4101
#define DCT_COS0_1 4141
#define DCT_COS0_2 4223
#define DCT_COS0_3 4350
#define DCT_COS0_4 4531
#define DCT_COS0_5 4775
#define DCT_COS0_6 5100
#define DCT_COS0_7 5528
#define DCT_COS0_8 6099
#define DCT_COS0_9 6876
#define DCT_COS0_10 7967
#define DCT_COS0_11 4790
#define DCT_COS0_12 6079
#define DCT_COS0_13 4214
#define DCT_COS0_14 6979
#define DCT_COS0_15 5217
#define DCT_COS1_0 4116
#define DCT_COS1_1 4280
#define DCT_COS1_2 4644
#define DCT_COS1_3 5299
#define DCT_COS1_4 6457
#define DCT_COS1_5 4345
#define DCT_COS1_6 7055
#define DCT_COS1_7 5224
#define DCT_COS2_0 4176
#define DCT_COS2_1 4926
#define DCT_COS2_2 7373
#define DCT_COS2_3 5249
#define DCT_COS3_0 4433
#define DCT_COS3_1 5352
#define DCT_COS4_0 5793

#define DCT_BF(t,a,b,sign,cosc,sh) do { \
    int _aa = (t)[(a)]; \
    int _bb = (t)[(b)]; \
    int _df; \
    (t)[(a)] = _aa + _bb; \
    _df = (sign) ? (_bb - _aa) : (_aa - _bb); \
    (t)[(b)] = (((_df << (sh)) * (cosc)) + 8192) >> 14; \
} while (0)
#define DCT_ADD(t,a,b) do { (t)[(a)] += (t)[(b)]; } while (0)
#define DCT_BF1(t,aa,bb,cc,dd) do { \
    DCT_BF(t,aa,bb,0,DCT_COS4_0,1); \
    DCT_BF(t,cc,dd,1,DCT_COS4_0,1); \
    DCT_ADD(t,cc,dd); \
} while (0)
#define DCT_BF2(t,aa,bb,cc,dd) do { \
    DCT_BF(t,aa,bb,0,DCT_COS4_0,1); \
    DCT_BF(t,cc,dd,1,DCT_COS4_0,1); \
    DCT_ADD(t,cc,dd); \
    DCT_ADD(t,aa,cc); \
    DCT_ADD(t,cc,bb); \
    DCT_ADD(t,bb,dd); \
} while (0)

static inline __attribute__((always_inline)) void FASTCALL synth_dct32_factored(int *t) {
    DCT_BF(t,0,31,0,DCT_COS0_0,1);   DCT_BF(t,15,16,0,DCT_COS0_15,5);
    DCT_BF(t,0,15,0,DCT_COS1_0,1);   DCT_BF(t,16,31,1,DCT_COS1_0,1);
    DCT_BF(t,7,24,0,DCT_COS0_7,1);   DCT_BF(t,8,23,0,DCT_COS0_8,1);
    DCT_BF(t,7,8,0,DCT_COS1_7,4);    DCT_BF(t,23,24,1,DCT_COS1_7,4);
    DCT_BF(t,0,7,0,DCT_COS2_0,1);    DCT_BF(t,16,23,0,DCT_COS2_0,1);
    DCT_BF(t,8,15,1,DCT_COS2_0,1);   DCT_BF(t,24,31,1,DCT_COS2_0,1);
    DCT_BF(t,3,28,0,DCT_COS0_3,1);   DCT_BF(t,12,19,0,DCT_COS0_12,2);
    DCT_BF(t,3,12,0,DCT_COS1_3,1);   DCT_BF(t,19,28,1,DCT_COS1_3,1);
    DCT_BF(t,4,27,0,DCT_COS0_4,1);   DCT_BF(t,11,20,0,DCT_COS0_11,2);
    DCT_BF(t,4,11,0,DCT_COS1_4,1);   DCT_BF(t,20,27,1,DCT_COS1_4,1);
    DCT_BF(t,3,4,0,DCT_COS2_3,3);    DCT_BF(t,19,20,0,DCT_COS2_3,3);
    DCT_BF(t,11,12,1,DCT_COS2_3,3);  DCT_BF(t,27,28,1,DCT_COS2_3,3);
    DCT_BF(t,0,3,0,DCT_COS3_0,1);    DCT_BF(t,8,11,0,DCT_COS3_0,1);
    DCT_BF(t,16,19,0,DCT_COS3_0,1);  DCT_BF(t,24,27,0,DCT_COS3_0,1);
    DCT_BF(t,4,7,1,DCT_COS3_0,1);    DCT_BF(t,12,15,1,DCT_COS3_0,1);
    DCT_BF(t,20,23,1,DCT_COS3_0,1);  DCT_BF(t,28,31,1,DCT_COS3_0,1);
    DCT_BF(t,1,30,0,DCT_COS0_1,1);   DCT_BF(t,14,17,0,DCT_COS0_14,3);
    DCT_BF(t,1,14,0,DCT_COS1_1,1);   DCT_BF(t,17,30,1,DCT_COS1_1,1);
    DCT_BF(t,6,25,0,DCT_COS0_6,1);   DCT_BF(t,9,22,0,DCT_COS0_9,1);
    DCT_BF(t,6,9,0,DCT_COS1_6,2);    DCT_BF(t,22,25,1,DCT_COS1_6,2);
    DCT_BF(t,1,6,0,DCT_COS2_1,1);    DCT_BF(t,17,22,0,DCT_COS2_1,1);
    DCT_BF(t,9,14,1,DCT_COS2_1,1);   DCT_BF(t,25,30,1,DCT_COS2_1,1);
    DCT_BF(t,2,29,0,DCT_COS0_2,1);   DCT_BF(t,13,18,0,DCT_COS0_13,3);
    DCT_BF(t,2,13,0,DCT_COS1_2,1);   DCT_BF(t,18,29,1,DCT_COS1_2,1);
    DCT_BF(t,5,26,0,DCT_COS0_5,1);   DCT_BF(t,10,21,0,DCT_COS0_10,1);
    DCT_BF(t,5,10,0,DCT_COS1_5,2);   DCT_BF(t,21,26,1,DCT_COS1_5,2);
    DCT_BF(t,2,5,0,DCT_COS2_2,1);    DCT_BF(t,18,21,0,DCT_COS2_2,1);
    DCT_BF(t,10,13,1,DCT_COS2_2,1);  DCT_BF(t,26,29,1,DCT_COS2_2,1);
    DCT_BF(t,1,2,0,DCT_COS3_1,2);    DCT_BF(t,9,10,0,DCT_COS3_1,2);
    DCT_BF(t,17,18,0,DCT_COS3_1,2);  DCT_BF(t,25,26,0,DCT_COS3_1,2);
    DCT_BF(t,5,6,1,DCT_COS3_1,2);    DCT_BF(t,13,14,1,DCT_COS3_1,2);
    DCT_BF(t,21,22,1,DCT_COS3_1,2);  DCT_BF(t,29,30,1,DCT_COS3_1,2);
    DCT_BF1(t,0,1,2,3);              DCT_BF2(t,4,5,6,7);
    DCT_BF1(t,8,9,10,11);            DCT_BF2(t,12,13,14,15);
    DCT_BF1(t,16,17,18,19);          DCT_BF2(t,20,21,22,23);
    DCT_BF1(t,24,25,26,27);          DCT_BF2(t,28,29,30,31);
}

static inline __attribute__((always_inline)) void FASTCALL synth_dct32_output_order(int *t) {
    int o[32];
    int k;
#define DCT_OUT(dst,src0,src1,src2,src3,src2a,src13a,src13b) do { \
    int r1 = t[(src0)]; \
    int r4 = t[(src13a)]; \
    int r2 = t[(src1)]; \
    int r3 = t[(src2)]; \
    if ((src13b) >= 0) r4 += t[(src13b)]; \
    r2 += r4; \
    if ((src3) >= 0) r4 += t[(src3)]; \
    if ((src2a) >= 0) r3 += t[(src2a)]; \
    o[(dst)+0] = r1; o[(dst)+1] = r2; o[(dst)+2] = r3; o[(dst)+3] = r4; \
} while (0)
    DCT_OUT(0,0,16,8,20,12,24,28);
    DCT_OUT(4,4,20,12,18,10,28,26);
    DCT_OUT(8,2,18,10,22,14,26,30);
    DCT_OUT(12,6,22,14,17,9,30,25);
    DCT_OUT(16,1,17,9,21,13,25,29);
    DCT_OUT(20,5,21,13,19,11,29,27);
    DCT_OUT(24,3,19,11,23,15,27,31);
    DCT_OUT(28,7,23,15,-1,-1,31,-1);
#undef DCT_OUT
    for (k = 0; k < 32; ++k) t[k] = o[k];
}

static void FASTCALL synth_dct32_to_v(int *vbase, int vpos, const int *sx) {
    int d[32];
    int i, a, b, sumx;

    d[0]  = sx[0];   d[1]  = sx[3];   d[2]  = sx[6];   d[3]  = sx[9];
    d[4]  = sx[12];  d[5]  = sx[15];  d[6]  = sx[18];  d[7]  = sx[21];
    d[8]  = sx[24];  d[9]  = sx[27];  d[10] = sx[30];  d[11] = sx[33];
    d[12] = sx[36];  d[13] = sx[39];  d[14] = sx[42];  d[15] = sx[45];
    d[16] = sx[48];
    d[17] = d[18] = d[19] = d[20] = d[21] = d[22] = d[23] = d[24] = 0;
    d[25] = d[26] = d[27] = d[28] = d[29] = d[30] = d[31] = 0;

    sumx = d[0] + d[1] + d[2] + d[3] + d[4] + d[5] + d[6] + d[7]
         + d[8] + d[9] + d[10] + d[11] + d[12] + d[13] + d[14] + d[15] + d[16];

    synth_dct32_factored(d);
    synth_dct32_output_order(d);

    for (i = 0; i < 16; ++i) {
        a = ( d[16 + i] + 32) >> 6;
        b = (-d[16 + i] + 32) >> 6;
        vbase[vpos + i] = a;
        vbase[vpos + i + 1024] = a;
        vbase[vpos + 32 - i] = b;
        vbase[vpos + 32 - i + 1024] = b;
    }
    vbase[vpos + 16] = 0;
    vbase[vpos + 16 + 1024] = 0;

    for (i = 0; i < 15; ++i) {
        a = (-d[15 - i] + 32) >> 6;
        vbase[vpos + 33 + i] = a;
        vbase[vpos + 33 + i + 1024] = a;
        vbase[vpos + 63 - i] = a;
        vbase[vpos + 63 - i + 1024] = a;
    }

    a = ((-sumx) + 32) >> 6;
    vbase[vpos + 48] = a;
    vbase[vpos + 48 + 1024] = a;
}
