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

static void fse_encode_with_stats(const void* src, size_t srcSize)
{
    unsigned count[256];
    short normalizedCounter[256];
    unsigned maxSymbolValue = 255;
    size_t ret;

    /* Step 1: count symbol frequencies */
    ret = HIST_count(count, &maxSymbolValue, src, srcSize);
    if (HIST_isError(ret)) {
        fprintf(stderr, "HIST_count error: %s\n", FSE_getErrorName(ret));
        return;
    }
    printf("=== Symbol statistics ===\n");
    printf("Source size        : %zu bytes\n", srcSize);
    printf("Max symbol value   : %u\n", maxSymbolValue);
    printf("Distinct symbols   : ");
    {   unsigned distinct = 0, s;
        for (s = 0; s <= maxSymbolValue; s++)
            if (count[s]) distinct++;
        printf("%u\n", distinct);
    }

    /* Step 2: find optimal tableLog */
    unsigned tableLog = FSE_optimalTableLog(0, srcSize, maxSymbolValue);
    printf("\n=== Table properties ===\n");
    printf("Optimal tableLog   : %u\n", tableLog);
    printf("Table size (states): %u\n", 1u << tableLog);

    /* Step 3: normalize counts */
    ret = FSE_normalizeCount(normalizedCounter, tableLog, count, srcSize, maxSymbolValue);
    if (FSE_isError(ret)) {
        fprintf(stderr, "FSE_normalizeCount error: %s\n", FSE_getErrorName(ret));
        return;
    }
    tableLog = (unsigned)ret;
    printf("Actual tableLog    : %u\n", tableLog);

    /* Step 4: measure header (normalized counter serialization) */
    size_t headerBound = FSE_NCountWriteBound(maxSymbolValue, tableLog);
    void* headerBuf = malloc(headerBound);
    if (!headerBuf) { fprintf(stderr, "malloc failed\n"); return; }
    size_t headerSize = FSE_writeNCount(headerBuf, headerBound,
                                        normalizedCounter, maxSymbolValue, tableLog);
    if (FSE_isError(headerSize)) {
        fprintf(stderr, "FSE_writeNCount error: %s\n", FSE_getErrorName(headerSize));
        free(headerBuf);
        return;
    }
    printf("\n=== Sizes ===\n");
    printf("Header size        : %zu bytes (bound: %zu)\n", headerSize, headerBound);
    printf("CTable size        : %zu bytes\n", FSE_CTABLE_SIZE(tableLog, maxSymbolValue));
    printf("DTable size        : %zu bytes\n", FSE_DTABLE_SIZE(tableLog));

    /* Step 5: build CTable and compress */
    FSE_CTable* ct = FSE_createCTable(maxSymbolValue, tableLog);
    if (!ct) { fprintf(stderr, "FSE_createCTable failed\n"); free(headerBuf); return; }
    ret = FSE_buildCTable(ct, normalizedCounter, maxSymbolValue, tableLog);
    if (FSE_isError(ret)) {
        fprintf(stderr, "FSE_buildCTable error: %s\n", FSE_getErrorName(ret));
        FSE_freeCTable(ct); free(headerBuf);
        return;
    }

    size_t dstCapacity = FSE_COMPRESSBOUND(srcSize);
    void* dst = malloc(dstCapacity);
    if (!dst) { fprintf(stderr, "malloc failed\n"); FSE_freeCTable(ct); free(headerBuf); return; }

    size_t compressedSize = FSE_compress_usingCTable(dst, dstCapacity, src, srcSize, ct);
    if (FSE_isError(compressedSize)) {
        fprintf(stderr, "FSE_compress_usingCTable error: %s\n", FSE_getErrorName(compressedSize));
        free(dst); FSE_freeCTable(ct); free(headerBuf);
        return;
    }

    size_t totalSize = headerSize + compressedSize;
    printf("Compressed data    : %zu bytes\n", compressedSize);
    printf("Total (hdr + data) : %zu bytes\n", totalSize);
    if (srcSize > 0) {
        printf("Ratio              : %.3f%%\n", (double)totalSize * 100.0 / (double)srcSize);
        printf("Bits per byte      : %.3f\n", (double)totalSize * 8.0 / (double)srcSize);
    }

    if (compressedSize == 0)
        printf("\nNote: data was not compressible (compressed >= source)\n");

    /* Step 6: verify by decompressing */
    if (compressedSize > 0) {
        FSE_DTable* dt = FSE_createDTable(tableLog);
        if (dt) {
            ret = FSE_buildDTable(dt, normalizedCounter, maxSymbolValue, tableLog);
            if (!FSE_isError(ret)) {
                void* check = malloc(srcSize);
                if (check) {
                    size_t decSize = FSE_decompress_usingDTable(check, srcSize, dst, compressedSize, dt);
                    if (!FSE_isError(decSize) && decSize == srcSize && memcmp(src, check, srcSize) == 0)
                        printf("\nRoundtrip verified OK\n");
                    else
                        printf("\nRoundtrip FAILED\n");
                    free(check);
                }
            }
            FSE_freeDTable(dt);
        }
    }

    free(dst);
    free(headerBuf);
    FSE_freeCTable(ct);
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
