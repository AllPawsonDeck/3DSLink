/*
 * Code-size savings for the Old 3DS memory budget.
 *
 * libjpeg only calls sscanf() to parse the JPEGMEM environment variable and
 * fprintf() to print warnings to stderr. A system module has neither, but
 * linking the real functions drags in ~45 KB of newlib's scanf/printf
 * machinery, which comes out of the same small memory region as our heap.
 */
#include <stdarg.h>
#include <stdio.h>

int sscanf(const char *restrict str, const char *restrict format, ...)
{
    (void)str;
    (void)format;
    return 0;
}

int fprintf(FILE *restrict stream, const char *restrict format, ...)
{
    (void)stream;
    (void)format;
    return 0;
}
