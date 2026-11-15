#include "wiz_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char line[8192];
    unsigned count = 0, failures = 0;
    while (fgets(line, sizeof(line), stdin)) {
        ++count;
        char *after = NULL;
        unsigned long method = strtoul(line, &after, 10);
        if (!after || *after++ != '\t' || method > WIZ_GET_USER) return 2;
        char *separator = strchr(after, '\t');
        if (!separator) return 2;
        *separator++ = 0;
        uint8_t mac[6];
        if (wiz_parse_mac(after, mac) != WIZ_OK) return 2;
        size_t size = strlen(separator);
        if (!size || separator[size - 1] != '\n') return 2;
        --size;
        wiz_reply_t reply;
        wiz_error_t result = wiz_parse_reply((const uint8_t *)separator, size, (wiz_method_t)method, mac, &reply);
        if (result != WIZ_OK) {
            ++failures;
            fprintf(stderr, "Upstream fixture %u (%s): %s\n", count, wiz_method_name((wiz_method_t)method), wiz_error_name(result));
        }
    }
    printf("Pinned upstream configuration replay: %u fixtures, %u rejected\n", count, failures);
    return failures ? 1 : 0;
}
