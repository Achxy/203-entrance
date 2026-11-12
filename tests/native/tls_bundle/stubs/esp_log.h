#include <stdio.h>
#define ESP_LOGE(tag,fmt,...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
