#include "core.h"
#include <ctype.h>
#include <limits.h>
#include <string.h>

#ifdef ESP_PLATFORM
#define APP_DESCRIPTOR __attribute__((section(".rodata_custom_desc"), used))
#else
#define APP_DESCRIPTOR
#endif
const APP_DESCRIPTOR entrance_descriptor_t entrance_descriptor = {
    .magic = ENTRANCE_MAGIC,
    .layout = ENTRANCE_LAYOUT,
    .schema_min = ENTRANCE_SCHEMA,
    .schema_max = ENTRANCE_SCHEMA,
    .board = ENTRANCE_BOARD,
    .descriptor_version = 1,
};

bool constant_equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= x[i] ^ y[i];
    return diff == 0;
}

bool hex_decode(const char *s, uint8_t *out, size_t n)
{
    if (!s || strlen(s) != n * 2)
        return false;
    for (size_t i = 0; i < n; i++) {
        int a = tolower((unsigned char)s[2 * i]), b = tolower((unsigned char)s[2 * i + 1]);
        a = a >= '0' && a <= '9' ? a - '0' : a >= 'a' && a <= 'f' ? a - 'a' + 10 : -1;
        b = b >= '0' && b <= '9' ? b - '0' : b >= 'a' && b <= 'f' ? b - 'a' + 10 : -1;
        if (a < 0 || b < 0)
            return false;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return true;
}

void hex_encode(const void *data, size_t n, char *out)
{
    const uint8_t *bytes = data;
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hex[bytes[i] >> 4];
        out[2 * i + 1] = hex[bytes[i] & 15];
    }
    out[2 * n] = 0;
}

uint32_t record_crc(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t crc = ~0u;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

typedef struct {
    unsigned major, minor, patch;
    const char *pre;
} version_t;
static bool parse_version(const char *s, version_t *v)
{
    if (!s || !*s || strlen(s) > 31)
        return false;
    unsigned *parts[] = {&v->major, &v->minor, &v->patch};
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s))
            return false;
        if (*s == '0' && isdigit((unsigned char)s[1]))
            return false;
        *parts[i] = 0;
        while (isdigit((unsigned char)*s)) {
            unsigned d = (unsigned)(*s++ - '0');
            if (*parts[i] > (UINT_MAX - d) / 10)
                return false;
            *parts[i] = *parts[i] * 10 + d;
        }
        if (i < 2 && *s++ != '.')
            return false;
    }
    v->pre = NULL;
    if (*s == '-') {
        v->pre = ++s;
        if (!*s || *s == '+')
            return false;
        while (*s && *s != '+') {
            const char *start = s;
            bool numeric = true;
            while (*s && *s != '.' && *s != '+') {
                if (!isalnum((unsigned char)*s) && *s != '-')
                    return false;
                if (!isdigit((unsigned char)*s))
                    numeric = false;
                s++;
            }
            if (s == start || (numeric && s - start > 1 && *start == '0'))
                return false;
            if (*s == '.') {
                s++;
                if (!*s || *s == '+')
                    return false;
            }
        }
    }
    if (*s == '+') {
        s++;
        if (!*s)
            return false;
        while (*s) {
            const char *start = s;
            while (*s && *s != '.') {
                if (!isalnum((unsigned char)*s) && *s != '-')
                    return false;
                s++;
            }
            if (s == start)
                return false;
            if (*s == '.') {
                s++;
                if (!*s)
                    return false;
            }
        }
    }
    return !*s;
}

static int pre_compare(const char *a, const char *b)
{
    if (!a || !b)
        return !a && !b ? 0 : !a ? 1 : -1;
    while (*a && *a != '+' && *b && *b != '+') {
        size_t na = 0, nb = 0;
        bool an = true, bn = true;
        while (a[na] && a[na] != '.' && a[na] != '+') {
            if (!isdigit((unsigned char)a[na]))
                an = false;
            na++;
        }
        while (b[nb] && b[nb] != '.' && b[nb] != '+') {
            if (!isdigit((unsigned char)b[nb]))
                bn = false;
            nb++;
        }
        if (an != bn)
            return an ? -1 : 1;
        if (an && na != nb)
            return na > nb ? 1 : -1;
        int c = strncmp(a, b, na < nb ? na : nb);
        if (c)
            return c > 0 ? 1 : -1;
        if (na != nb)
            return na > nb ? 1 : -1;
        a += na;
        b += nb;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
    }
    bool ae = !*a || *a == '+', be = !*b || *b == '+';
    return ae == be ? 0 : ae ? -1 : 1;
}

int version_compare(const char *a, const char *b, bool *valid)
{
    version_t x = {0}, y = {0};
    *valid = parse_version(a, &x) && parse_version(b, &y);
    if (!*valid)
        return 0;
    if (x.major != y.major)
        return x.major > y.major ? 1 : -1;
    if (x.minor != y.minor)
        return x.minor > y.minor ? 1 : -1;
    if (x.patch != y.patch)
        return x.patch > y.patch ? 1 : -1;
    return pre_compare(x.pre, y.pre);
}

bool descriptor_compatible(const entrance_descriptor_t *d)
{
    return d->magic == ENTRANCE_MAGIC && d->descriptor_version == 1 &&
           d->layout == ENTRANCE_LAYOUT && d->schema_min <= ENTRANCE_SCHEMA &&
           d->schema_max >= ENTRANCE_SCHEMA && memchr(d->board, 0, sizeof(d->board)) &&
           strcmp(d->board, ENTRANCE_BOARD) == 0;
}
