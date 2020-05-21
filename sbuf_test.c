//
// Created by fourstring on 2020/5/19.
//

#include <stdlib.h>
#include "sbuf.h"
#include "csapp.h"

sbuf_t buf;

void *producer_routine(void *varg) {
    int m[] = {1, 3, 2, 4, 5, 6, 7, 8, 9, 10};
    for (int i = 0; i < 10; i++) {
        sbuf_insert(&buf, m[i]);
    }
    return NULL;
}

void *consumer_routine(void *varg) {
    int t;
    for (int i = 0; i < 10; i++) {
        printf("%d \n", sbuf_remove(&buf));
    }
    return NULL;
}


int main() {
    char a[10000], b[10000];
    sscanf("Content-Length: 114514", "%s: %s", a, b);
    return 0;
}