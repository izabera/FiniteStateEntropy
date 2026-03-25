#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FSE_STATIC_LINKING_ONLY
#include "../lib/fse.h"
#include "../lib/hist.h"

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

/* Compress with a specific tableLog and print stats.
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

static void fse_encode_with_stats(const void* src, size_t srcSize)
{
    unsigned count[FSE_MAX_SYMBOL_VALUE + 1];
    unsigned maxSymbolValue = 255;
    size_t ret;

    /* Count symbol frequencies */
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

    printf("=== Input ===\n");
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

    printf("\n=== FSE_compress() baseline ===\n");
    if (fseCompressSize <= 1) {
        printf("  FSE_compress returned %zu (not compressible or RLE)\n", fseCompressSize);
    } else {
        printf("  total=%zu  ratio=%.2f%%  bpb=%.3f\n",
               fseCompressSize,
               (double)fseCompressSize * 100.0 / (double)srcSize,
               (double)fseCompressSize * 8.0 / (double)srcSize);
    }

    /* Detailed comparison at different tableLog values */
    printf("\n=== Detailed comparison (header + data, manual workflow) ===\n");

    unsigned optDefault = FSE_optimalTableLog(FSE_DEFAULT_TABLELOG, srcSize, maxSymbolValue);
    unsigned optMax     = FSE_optimalTableLog(FSE_MAX_TABLELOG, srcSize, maxSymbolValue);

    printf("  FSE_DEFAULT_TABLELOG = %u,  FSE_MAX_TABLELOG = %u\n", FSE_DEFAULT_TABLELOG, FSE_MAX_TABLELOG);
    printf("  optimalTableLog(default=%u) = %u\n", FSE_DEFAULT_TABLELOG, optDefault);
    printf("  optimalTableLog(max=%u)     = %u\n", FSE_MAX_TABLELOG, optMax);
    printf("\n");

    /* Try every valid tableLog from MIN to MAX */
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

    /* Table memory sizes for reference */
    printf("\n=== Table memory sizes ===\n");
    for (tl = FSE_MIN_TABLELOG; tl <= FSE_MAX_TABLELOG; tl++) {
        printf("  tableLog=%2u : CTable=%5zu bytes, DTable=%5zu bytes, states=%5u\n",
               tl,
               (size_t)FSE_CTABLE_SIZE(tl, maxSymbolValue),
               (size_t)FSE_DTABLE_SIZE(tl),
               1u << tl);
    }
}

int main(void)
{
    void* src = malloc(MAX_SRC_SIZE);
    if (!src) { fprintf(stderr, "malloc failed\n"); return 1; }

    size_t srcSize = readStdin(src, MAX_SRC_SIZE);
    if (srcSize == 0) { fprintf(stderr, "Empty input\n"); free(src); return 1; }

    fse_encode_with_stats(src, srcSize);

    free(src);
    return 0;
}
