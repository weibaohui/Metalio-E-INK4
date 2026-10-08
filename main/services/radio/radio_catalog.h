#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// main/services/radio/radio_catalog.h —— 内置电台目录(339 台,只读)。
#include <stdbool.h>

#define RADIO_NAME_MAX 32
#define RADIO_URL_MAX  256

typedef struct {
    char name[RADIO_NAME_MAX];
    char url[RADIO_URL_MAX];
} radio_station_t;

int radio_catalog_count(void);
bool radio_catalog_get(int idx, radio_station_t *out);
const char *radio_catalog_url(int idx);

#ifdef __cplusplus
}  // extern "C"
#endif
