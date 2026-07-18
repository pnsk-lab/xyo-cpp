#include "sjit_host_api.h"

#include <stdio.h>

void sjit_host_log(const char *message) {
    fprintf(stderr, "%s\n", message ? message : "");
}

