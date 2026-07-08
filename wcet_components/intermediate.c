#include <annot.h>

static double input[100];
static double output[100];

void intermediate_fn(int n) {
    int i;
    for (i = 0; i < n; i++) {
        ANNOT_MAXITER(100);
        output[i] = input[i] * 2.0;
    }
}

int main(void) {
    intermediate_fn(100);
    return 0;
}
