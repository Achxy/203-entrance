#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

/* ESP-IDF's dual-stack HTTP listener returns IPv4-mapped IPv6 endpoints.
   Return only a real IPv4 endpoint, in network byte order. */
static inline bool arcade_address_ipv4(const struct sockaddr_storage *address, socklen_t length,
                                       uint32_t *out)
{
    if (address->ss_family == AF_INET && length >= sizeof(struct sockaddr_in)) {
        *out = ((const struct sockaddr_in *)address)->sin_addr.s_addr;
        return true;
    }
    if (address->ss_family == AF_INET6 && length >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address;
        static const uint8_t mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255};
        if (!memcmp(v6->sin6_addr.s6_addr, mapped_prefix, sizeof(mapped_prefix))) {
            memcpy(out, v6->sin6_addr.s6_addr + 12, sizeof(*out));
            return true;
        }
    }
    return false;
}
