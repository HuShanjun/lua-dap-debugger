#include "dap_json.h"

cJSON *dap_json_parse(const char *s, size_t n) {
    if (!s) return NULL;
    return cJSON_ParseWithLength(s, n);
}

char *dap_json_print_unformatted(const cJSON *root) {
    if (!root) return NULL;
    return cJSON_PrintUnformatted(root);
}
