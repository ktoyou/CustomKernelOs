/* cmp file1 file2 — код 0, если байты совпали, 1 если нет, 2 при ошибке. */
#include "myos.h"
#include "stdio.h"

int main(int argc, char **argv) {
    FILE *a, *b;
    int ca, cb;
    if (argc != 3) {
        fprintf(stderr, "usage: cmp file1 file2\n");
        return 2;
    }
    a = fopen(argv[1], "rb");
    b = fopen(argv[2], "rb");
    if (!a || !b) {
        fprintf(stderr, "cmp: cannot open\n");
        if (a) fclose(a);
        if (b) fclose(b);
        return 2;
    }
    do {
        ca = fgetc(a);
        cb = fgetc(b);
        if (ca != cb) {
            fclose(a);
            fclose(b);
            return 1;
        }
    } while (ca != EOF);
    fclose(a);
    fclose(b);
    return 0;
}
