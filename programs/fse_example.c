#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FSE_STATIC_LINKING_ONLY
#include "../lib/fse.h"
#include "../lib/hist.h"

/* Declare U16 functions - can't include fseU16.h because it redefines FSE_MAX_SYMBOL_VALUE */
size_t FSE_compressU16(void* dst, size_t dstCapacity,
       const unsigned short* src, size_t srcSize,
       unsigned maxSymbolValue, unsigned tableLog);

/* U16 constants (from fseU16.c) */
#define FSEU16_MAX_MEMORY_USAGE    15
#define FSEU16_DEFAULT_MEMORY_USAGE 14
#define FSEU16_MAX_TABLELOG        (FSEU16_MAX_MEMORY_USAGE - 2)
#define FSEU16_DEFAULT_TABLELOG    (FSEU16_DEFAULT_MEMORY_USAGE - 2)
#define FSEU16_SYMBOLVALUE_ABSOLUTEMAX 4095

#define MAX_SRC_SIZE (64 << 20)  /* 64 MB max */

static size_t readStdin(void* buf, size_t bufSize)
{
    size_t totalRead = 0;
    while (totalRead < bufSize) {
        size_t n = fread((char*)buf + totalRead, 1, bufSize - totalRead, stdin);
        if (n == 0) break;
        totalRead += n;
    }
    return totalRead;
}

/* Compress bytes with a specific tableLog and print stats.
 * Returns total compressed size (header + data), or 0 on failure/incompressible. */
static size_t compress_and_report(const char* label,
                                  const void* src, size_t srcSize,
                                  const unsigned* count,
                                  unsigned maxSymbolValue,
                                  unsigned tableLog)
{
    short normalizedCounter[FSE_MAX_SYMBOL_VALUE + 1];
    size_t ret;

    ret = FSE_normalizeCount(normalizedCounter, tableLog, count, srcSize, maxSymbolValue);
    if (FSE_isError(ret)) return 0;
    tableLog = (unsigned)ret;

    size_t headerBound = FSE_NCountWriteBound(maxSymbolValue, tableLog);
    void* headerBuf = malloc(headerBound);
    if (!headerBuf) return 0;
    size_t headerSize = FSE_writeNCount(headerBuf, headerBound,
                                        normalizedCounter, maxSymbolValue, tableLog);
    free(headerBuf);
    if (FSE_isError(headerSize)) return 0;

    FSE_CTable* ct = FSE_createCTable(maxSymbolValue, tableLog);
    if (!ct) return 0;
    ret = FSE_buildCTable(ct, normalizedCounter, maxSymbolValue, tableLog);
    if (FSE_isError(ret)) { FSE_freeCTable(ct); return 0; }

    size_t dstCapacity = FSE_COMPRESSBOUND(srcSize);
    void* dst = malloc(dstCapacity);
    if (!dst) { FSE_freeCTable(ct); return 0; }

    size_t compressedSize = FSE_compress_usingCTable(dst, dstCapacity, src, srcSize, ct);
    free(dst);
    FSE_freeCTable(ct);
    if (FSE_isError(compressedSize) || compressedSize == 0) return 0;

    size_t totalSize = headerSize + compressedSize;
    printf("  %-30s tableLog=%2u  header=%4zu  data=%6zu  total=%6zu  ratio=%6.2f%%  bpb=%.3f\n",
           label, tableLog, headerSize, compressedSize, totalSize,
           (double)totalSize * 100.0 / (double)srcSize,
           (double)totalSize * 8.0 / (double)srcSize);

    return totalSize;
}

static void byte_fse_stats(const void* src, size_t srcSize)
{
    unsigned count[FSE_MAX_SYMBOL_VALUE + 1];
    unsigned maxSymbolValue = 255;
    size_t ret;

    ret = HIST_count(count, &maxSymbolValue, src, srcSize);
    if (HIST_isError(ret)) {
        fprintf(stderr, "HIST_count error: %s\n", FSE_getErrorName(ret));
        return;
    }
    unsigned distinct = 0;
    {   unsigned s;
        for (s = 0; s <= maxSymbolValue; s++)
            if (count[s]) distinct++;
    }

    printf("=== Byte FSE ===\n");
    printf("Source size      : %zu bytes\n", srcSize);
    printf("Max symbol value : %u\n", maxSymbolValue);
    printf("Distinct symbols : %u\n", distinct);

    /* FSE_compress result for baseline */
    size_t dstCapacity = FSE_COMPRESSBOUND(srcSize);
    void* dst = malloc(dstCapacity);
    size_t fseCompressSize = 0;
    if (dst) {
        fseCompressSize = FSE_compress(dst, dstCapacity, src, srcSize);
        free(dst);
    }

    printf("\n  FSE_compress() baseline: ");
    if (fseCompressSize <= 1) {
        printf("returned %zu (not compressible or RLE)\n", fseCompressSize);
    } else {
        printf("total=%zu  ratio=%.2f%%  bpb=%.3f\n",
               fseCompressSize,
               (double)fseCompressSize * 100.0 / (double)srcSize,
               (double)fseCompressSize * 8.0 / (double)srcSize);
    }

    /* Detailed comparison at different tableLog values */
    printf("\n  Per-tableLog breakdown:\n");

    unsigned optDefault = FSE_optimalTableLog(FSE_DEFAULT_TABLELOG, srcSize, maxSymbolValue);
    unsigned optMax     = FSE_optimalTableLog(FSE_MAX_TABLELOG, srcSize, maxSymbolValue);

    printf("  FSE_DEFAULT_TABLELOG = %u,  FSE_MAX_TABLELOG = %u\n", FSE_DEFAULT_TABLELOG, FSE_MAX_TABLELOG);
    printf("  optimalTableLog(default=%u) = %u\n", FSE_DEFAULT_TABLELOG, optDefault);
    printf("  optimalTableLog(max=%u)     = %u\n\n", FSE_MAX_TABLELOG, optMax);

    unsigned tl;
    for (tl = FSE_MIN_TABLELOG; tl <= FSE_MAX_TABLELOG; tl++) {
        char label[64];
        const char* tag = "";
        if (tl == optDefault && tl == optMax) tag = " (optimal) <--";
        else if (tl == optDefault)            tag = " (opt-default) <--";
        else if (tl == optMax)                tag = " (opt-max) <--";
        snprintf(label, sizeof(label), "tableLog %u%s", tl, tag);
        size_t r = compress_and_report(label, src, srcSize, count, maxSymbolValue, tl);
        if (r == 0)
            printf("  %-30s tableLog=%2u  (skipped: too small for %u symbols)\n", label, tl, distinct);
    }

    printf("\n  Table memory sizes:\n");
    for (tl = FSE_MIN_TABLELOG; tl <= FSE_MAX_TABLELOG; tl++) {
        printf("  tableLog=%2u : CTable=%5zu bytes, DTable=%5zu bytes, states=%5u\n",
               tl,
               (size_t)FSE_CTABLE_SIZE(tl, maxSymbolValue),
               (size_t)FSE_DTABLE_SIZE(tl),
               1u << tl);
    }
}

static void u16_fse_stats(const unsigned short* src, size_t u16Count)
{
    size_t rawBytes = u16Count * 2;

    /* Count U16 symbol frequencies manually (FSE_countU16 is not easily
     * callable without pulling in the U16 redefinitions) */
    unsigned maxSymbolValue = 0;
    {   size_t i;
        for (i = 0; i < u16Count; i++)
            if (src[i] > maxSymbolValue) maxSymbolValue = src[i];
    }

    printf("=== U16 FSE (native) ===\n");
    printf("U16 count        : %zu elements (%zu bytes)\n", u16Count, rawBytes);
    printf("Max symbol value : %u\n", maxSymbolValue);

    if (maxSymbolValue > FSEU16_SYMBOLVALUE_ABSOLUTEMAX) {
        printf("  SKIPPED: max symbol value %u exceeds U16 FSE absolute limit (%u)\n",
               maxSymbolValue, FSEU16_SYMBOLVALUE_ABSOLUTEMAX);
        return;
    }
    printf("  Note: FSEU16_MAX_SYMBOL_VALUE compiled as %u (redefine to increase, max %u)\n",
           286u, FSEU16_SYMBOLVALUE_ABSOLUTEMAX);

    /* Try FSE_compressU16 at different tableLogs */
    size_t dstCapacity = rawBytes + 1024;
    void* dst = malloc(dstCapacity);
    if (!dst) { fprintf(stderr, "malloc failed\n"); return; }

    /* Default (pass actual maxSymbolValue, tableLog=0 means auto) */
    size_t baselineSize = FSE_compressU16(dst, dstCapacity, src, u16Count, maxSymbolValue, 0);
    printf("\n  FSE_compressU16() baseline (auto): ");
    if (FSE_isError(baselineSize)) {
        printf("error: %s\n", FSE_getErrorName(baselineSize));
    } else if (baselineSize <= 1) {
        printf("returned %zu (not compressible or RLE)\n", baselineSize);
    } else {
        printf("total=%zu  ratio=%.2f%%  bpb=%.3f\n",
               baselineSize,
               (double)baselineSize * 100.0 / (double)rawBytes,
               (double)baselineSize * 8.0 / (double)rawBytes);
    }

    printf("\n  Per-tableLog breakdown:\n");
    unsigned optDefault = FSE_optimalTableLog(FSEU16_DEFAULT_TABLELOG, u16Count, maxSymbolValue);
    unsigned optMax     = FSE_optimalTableLog(FSEU16_MAX_TABLELOG, u16Count, maxSymbolValue);
    printf("  FSEU16_DEFAULT_TABLELOG = %u,  FSEU16_MAX_TABLELOG = %u\n", FSEU16_DEFAULT_TABLELOG, FSEU16_MAX_TABLELOG);
    printf("  optimalTableLog(default=%u) = %u\n", FSEU16_DEFAULT_TABLELOG, optDefault);
    printf("  optimalTableLog(max=%u)     = %u\n\n", FSEU16_MAX_TABLELOG, optMax);

    unsigned tl;
    for (tl = FSE_MIN_TABLELOG; tl <= FSEU16_MAX_TABLELOG; tl++) {
        char label[64];
        const char* tag = "";
        if (tl == optDefault && tl == optMax) tag = " (optimal) <--";
        else if (tl == optDefault)            tag = " (opt-default) <--";
        else if (tl == optMax)                tag = " (opt-max) <--";
        snprintf(label, sizeof(label), "tableLog %u%s", tl, tag);

        size_t cSize = FSE_compressU16(dst, dstCapacity, src, u16Count, maxSymbolValue, tl);
        if (FSE_isError(cSize)) {
            printf("  %-30s tableLog=%2u  error: %s\n", label, tl, FSE_getErrorName(cSize));
        } else if (cSize <= 1) {
            printf("  %-30s tableLog=%2u  returned %zu (not compressible or RLE)\n", label, tl, cSize);
        } else {
            printf("  %-30s tableLog=%2u  total=%6zu  ratio=%6.2f%%  bpb=%.3f\n",
                   label, tl, cSize,
                   (double)cSize * 100.0 / (double)rawBytes,
                   (double)cSize * 8.0 / (double)rawBytes);
        }
    }

    free(dst);
}

static void byte_lane_stats(const unsigned short* src, size_t u16Count)
{
    size_t rawBytes = u16Count * 2;
    unsigned char* lo = malloc(u16Count);
    unsigned char* hi = malloc(u16Count);
    if (!lo || !hi) { fprintf(stderr, "malloc failed\n"); free(lo); free(hi); return; }

    /* Split into byte lanes */
    {   size_t i;
        for (i = 0; i < u16Count; i++) {
            lo[i] = (unsigned char)(src[i] & 0xFF);
            hi[i] = (unsigned char)(src[i] >> 8);
        }
    }

    printf("=== Byte-lane split (low byte + high byte, each FSE-compressed independently) ===\n");
    printf("Each lane: %zu bytes, total raw: %zu bytes\n\n", u16Count, rawBytes);

    size_t dstCapacity = FSE_COMPRESSBOUND(u16Count);
    void* dst = malloc(dstCapacity);
    if (!dst) { free(lo); free(hi); return; }

    /* Compress low lane */
    size_t loSize = FSE_compress(dst, dstCapacity, lo, u16Count);
    printf("  Low byte lane:  ");
    if (FSE_isError(loSize)) {
        printf("error: %s\n", FSE_getErrorName(loSize));
        loSize = u16Count;
    } else if (loSize == 1) {
        printf("RLE (single byte repeated) => 1 byte\n");
    } else if (loSize == 0) {
        printf("not compressible => storing raw %zu bytes\n", u16Count);
        loSize = u16Count;
    } else {
        printf("compressed=%zu  (%.2f%% of lane)\n", loSize, (double)loSize * 100.0 / (double)u16Count);
    }

    /* Compress high lane */
    size_t hiSize = FSE_compress(dst, dstCapacity, hi, u16Count);
    printf("  High byte lane: ");
    if (FSE_isError(hiSize)) {
        printf("error: %s\n", FSE_getErrorName(hiSize));
        hiSize = u16Count;
    } else if (hiSize == 1) {
        printf("RLE (single byte repeated) => 1 byte\n");
    } else if (hiSize == 0) {
        printf("not compressible => storing raw %zu bytes\n", u16Count);
        hiSize = u16Count;
    } else {
        printf("compressed=%zu  (%.2f%% of lane)\n", hiSize, (double)hiSize * 100.0 / (double)u16Count);
    }

    size_t totalLanes = loSize + hiSize;
    printf("\n  Combined        : total=%zu  ratio=%.2f%%  bpb=%.3f\n",
           totalLanes,
           (double)totalLanes * 100.0 / (double)rawBytes,
           (double)totalLanes * 8.0 / (double)rawBytes);

    free(dst);
    free(lo);
    free(hi);
}

int main(int argc, char** argv)
{
    int u16_mode = 0;
    if (argc > 1 && strcmp(argv[1], "-u16") == 0)
        u16_mode = 1;

    void* src = malloc(MAX_SRC_SIZE);
    if (!src) { fprintf(stderr, "malloc failed\n"); return 1; }

    size_t srcSize = readStdin(src, MAX_SRC_SIZE);
    if (srcSize == 0) { fprintf(stderr, "Empty input\n"); free(src); return 1; }

    if (!u16_mode) {
        /* Byte mode: just like before */
        byte_fse_stats(src, srcSize);
    } else {
        /* U16 mode: interpret input as U16 array */
        if (srcSize < 2) { fprintf(stderr, "Need at least 2 bytes for U16 mode\n"); free(src); return 1; }
        size_t u16Count = srcSize / 2;

        printf("Input: %zu bytes => %zu U16 elements", srcSize, u16Count);
        if (srcSize & 1) printf(" (last byte ignored)");
        printf("\n\n");

        const unsigned short* u16src = (const unsigned short*)src;

        u16_fse_stats(u16src, u16Count);
        printf("\n");
        byte_lane_stats(u16src, u16Count);

        /* Also show what plain byte FSE gets on the raw bytes for completeness */
        printf("\n");
        byte_fse_stats(src, u16Count * 2);
    }

    free(src);
    return 0;
}
