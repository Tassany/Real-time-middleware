#include <annot.h>

static int data[100];

void source_fn(int n) {
    int i;
    for (i = 0; i < n; i++) {
        ANNOT_MAXITER(100);
        data[i] = data[i] * 2;
    }
}

int main(void) {
    source_fn(100);
    return 0;
}
