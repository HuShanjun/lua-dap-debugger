#ifndef DAP_JSON_H
#define DAP_JSON_H

#include <stddef.h>
#include <cJSON.h>

cJSON *dap_json_parse(const char *s, size_t n);
char *dap_json_print_unformatted(const cJSON *root); /* malloc'd; caller free */

#endif
