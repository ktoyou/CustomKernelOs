/* date — печать текущего времени. Шаблоны Git только проверяют, что команда есть. */
#include "myos.h"
#include "time.h"

int main(void) {
    char buf[64];
    time_t now = time(0);
    struct tm *tm = localtime(&now);
    if (!tm || !strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", tm)) {
        printf("%ld\n", (long)now);
        return 0;
    }
    printf("%s\n", buf);
    return 0;
}
