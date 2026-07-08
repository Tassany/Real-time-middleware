#include <annot.h>

static double input[100];
static double result;

void sink_fn(int n) {
    int i;
    result = 0.0;
    for (i = 0; i < n; i++) {
        ANNOT_MAXITER(100);
        result = result + input[i];
    }
}

int main(void) {
    sink_fn(100);
    return 0;
}
