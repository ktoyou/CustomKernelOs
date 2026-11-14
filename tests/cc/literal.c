#include <stdio.h>

struct LitPair { int a; int b; };
static struct LitPair *lit_pair = &(struct LitPair){ .b = 9, .a = 7 };
static int *lit_eleven = &(int){11};

static unsigned fbits(float f) {
    union { float f; unsigned i; } u;
    u.f = f;
    return ((union { float _f; unsigned _i; }){f})._i;
}

int main(void) {
    struct { int a; int b; } s = (struct { int a; int b; }){2, 3};
    double pi = 0x1.921fb54442d18p+1;
    double three = 0x1.8p1;
    float half = 0x1p-1f;
    if (three != 3) return 1;
    if (half != 0.5f) return 2;
    if (pi < 3.141592653589792 || pi > 3.141592653589794) return 3;
    if (fbits(1.0f) != 0x3f800000u) return 4;
    if (s.a != 2 || s.b != 3) return 5;
    if (!lit_pair || lit_pair->a != 7 || lit_pair->b != 9) return 9;
    if (!lit_eleven || *lit_eleven != 11) return 10;
    if (0x1p10 != 1024) return 6;
    if (0x1p-10 != 1.0 / 1024) return 7;
    if (0x0.8p1 != 1) return 8;
    printf("literals: ok\n");
    return 0;
}
