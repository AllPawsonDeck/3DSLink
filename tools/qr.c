/*
 * Prints a QR code for TEXT as SVG (default) or PBM. Builds the install QR
 * code in the README:
 *
 *   cc -O2 -Itools/vendor tools/qr.c tools/vendor/qrcodegen.c -o build/qr
 *   build/qr https://github.com/.../3DSLink.cia > docs/install-qr.svg
 */
#include <stdio.h>
#include <string.h>

#include "qrcodegen.h"

int main(int argc, char **argv)
{
    int pbm = argc > 2 && !strcmp(argv[1], "-pbm");
    const char *text = argv[argc - 1];
    if (argc < 2)
        return fprintf(stderr, "usage: qr [-pbm] TEXT\n"), 2;

    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        return fprintf(stderr, "text too long\n"), 1;

    const int border = 4, size = qrcodegen_getSize(qr), full = size + 2 * border;
    if (pbm) {
        printf("P1\n%d %d\n", full, full);
        for (int y = -border; y < size + border; y++) {
            for (int x = -border; x < size + border; x++)
                putchar(qrcodegen_getModule(qr, x, y) ? '1' : '0');
            putchar('\n');
        }
        return 0;
    }
    printf("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" width=\"%d\" height=\"%d\" "
           "shape-rendering=\"crispEdges\">\n<rect width=\"100%%\" height=\"100%%\" fill=\"#fff\"/>\n<path d=\"",
           full, full, full * 8, full * 8);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            if (qrcodegen_getModule(qr, x, y))
                printf("M%d,%dh1v1h-1z", x + border, y + border);
    printf("\" fill=\"#000\"/>\n</svg>\n");
    return 0;
}
