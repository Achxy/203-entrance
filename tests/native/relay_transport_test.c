#include "relay_transport.h"
#include "esp_transport_ws.h"
#include "esp_transport_ssl.h"
#include "esp_crt_bundle.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct test_transport {
    bool ws, bundle;
    const char *common_name;
    esp_transport_handle_t parent;
    char headers[128];
};
static unsigned allocations, calls, tls_calls, dns_frees;
static int allocation_failure, config_failure, dns_failure, tls_result, ws_result;
static const char *answers[12];
static unsigned answers_n;
static char dialed[INET_ADDRSTRLEN];
static const char *expected_host = "relay.example.com";
static struct test_transport unrelated;

int __wrap_esp_transport_connect(esp_transport_handle_t, const char *, int, int);
int test_getaddrinfo(const char *host, const char *port, const struct addrinfo *hint,
                     struct addrinfo **result)
{
    assert(!strcmp(host, expected_host) && !strcmp(port, "443"));
    assert(hint->ai_family == AF_INET && hint->ai_socktype == SOCK_STREAM);
    *result = NULL;
    if (dns_failure)
        return EAI_FAIL;
    struct addrinfo **next = result;
    for (unsigned i = 0; i < answers_n; i++) {
        struct addrinfo *p = calloc(1, sizeof(*p));
        struct sockaddr_in *ip = calloc(1, sizeof(*ip));
        assert(p && ip);
        ip->sin_family = AF_INET;
        assert(inet_pton(AF_INET, answers[i], &ip->sin_addr) == 1);
        p->ai_family = AF_INET;
        p->ai_addrlen = sizeof(*ip);
        p->ai_addr = (struct sockaddr *)ip;
        *next = p;
        next = &p->ai_next;
    }
    return 0;
}
void test_freeaddrinfo(struct addrinfo *p)
{
    dns_frees++;
    while (p) {
        struct addrinfo *next = p->ai_next;
        free(p->ai_addr);
        free(p);
        p = next;
    }
}
static esp_transport_handle_t allocate(bool ws)
{
    if (allocation_failure == (ws ? 2 : 1))
        return NULL;
    esp_transport_handle_t t = calloc(1, sizeof(*t));
    assert(t);
    t->ws = ws;
    allocations++;
    return t;
}
esp_transport_handle_t esp_transport_ssl_init(void) { return allocate(false); }
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent)
{
    esp_transport_handle_t t = allocate(true);
    if (t)
        t->parent = parent;
    return t;
}
esp_err_t esp_transport_destroy(esp_transport_handle_t t)
{
    assert(t && allocations);
    allocations--;
    free(t);
    return ESP_OK;
}
esp_err_t esp_transport_set_default_port(esp_transport_handle_t t, int port)
{
    assert(t->ws && port == 443);
    return ESP_OK;
}
esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t t, esp_err_t (*attach)(void *))
{
    assert(!t->ws && attach == esp_crt_bundle_attach);
    t->bundle = true;
}
void esp_transport_ssl_set_common_name(esp_transport_handle_t t, const char *name)
{
    assert(!t->ws && !strcmp(name, expected_host));
    t->common_name = name;
}
void esp_transport_ssl_set_keep_alive(esp_transport_handle_t t, esp_transport_keep_alive_t *cfg)
{
    assert(!t->ws && cfg->keep_alive_enable && cfg->keep_alive_count > 0);
}
esp_err_t esp_transport_ws_set_config(esp_transport_handle_t t, const esp_transport_ws_config_t *cfg)
{
    assert(t->ws && !strcmp(cfg->ws_path, "/v1/device"));
    assert(!strcmp(cfg->user_agent, "203-Arcade/1") && cfg->propagate_control_frames);
    assert(strlen(cfg->headers) < sizeof(t->headers));
    strcpy(t->headers, cfg->headers);
    return config_failure ? ESP_ERR_NO_MEM : ESP_OK;
}
int __real_esp_transport_connect(esp_transport_handle_t t, const char *host, int port, int timeout)
{
    calls++;
    if (!t) {
        assert(!strcmp(host, "unrelated.example") && port == 8443 && timeout == 17);
        return 19;
    }
    if (t == &unrelated) {
        assert(!strcmp(host, expected_host) && port == 443 && timeout == 17);
        return 23;
    }
    assert(port == 443 && timeout == 5000);
    if (t->ws) {
        /* The actual SDK WS layer uses this hostname in the HTTP Host header. */
        assert(!strcmp(host, expected_host));
        assert(!strcmp(t->headers, "X-Test-Proof: temporary\r\n"));
        int ret = __wrap_esp_transport_connect(t->parent, host, port, timeout);
        return ret < 0 ? ret : ws_result;
    }
    tls_calls++;
    assert(t->bundle && !strcmp(t->common_name, expected_host));
    struct in_addr ip;
    assert(inet_pton(AF_INET, host, &ip) == 1);
    strcpy(dialed, host);
    return tls_result;
}
static esp_transport_handle_t make(void)
{
    assert(relay_transport_resolve(expected_host));
    char headers[] = "X-Test-Proof: temporary\r\n";
    esp_transport_handle_t t = relay_transport_create(headers);
    assert(t && allocations == 2);
    memset(headers, 'x', sizeof(headers) - 1);
    return t;
}
static void connect_result(int expected)
{
    esp_transport_handle_t t = make();
    unsigned before = tls_calls;
    assert(__wrap_esp_transport_connect(t, expected_host, 443, 5000) == expected);
    assert(tls_calls == before + 1); /* one IP, no multiplied timeout */
    relay_transport_destroy();
    relay_transport_destroy();
    assert(allocations == 0);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    answers[0] = "192.0.2.2"; answers[1] = "192.0.2.1"; answers_n = 2;
    if (!strcmp(argv[1], "failover")) {
        tls_result = -1;
        connect_result(-1);
        assert(!strcmp(dialed, "192.0.2.1"));
        answers[0] = "192.0.2.1"; answers[1] = "192.0.2.2";
        tls_result = 0;
        connect_result(0);
        assert(!strcmp(dialed, "192.0.2.2"));
        connect_result(0);
        assert(!strcmp(dialed, "192.0.2.2")); /* sticky success */
        tls_result = -1;
        connect_result(-1);
        connect_result(-1);
        assert(!strcmp(dialed, "192.0.2.1")); /* wraps */
        answers[0] = "192.0.2.9"; answers_n = 1;
        tls_result = 0;
        connect_result(0);
        assert(!strcmp(dialed, "192.0.2.9")); /* no stale IP */
        expected_host = "changed.example.com";
        answers[0] = "192.0.2.1"; answers[1] = "192.0.2.9"; answers_n = 2;
        connect_result(0);
        assert(!strcmp(dialed, "192.0.2.1")); /* host change resets preference */
    } else if (!strcmp(argv[1], "redirects")) {
        ws_result = 302;
        connect_result(-1);
        assert(relay_transport_redirected());
        ws_result = 0;
        esp_transport_handle_t t = make();
        assert(!relay_transport_redirected());
        unsigned before = calls;
        assert(__wrap_esp_transport_connect(t, "evil.example", 443, 5000) == -1);
        assert(calls == before && errno == EACCES);
        assert(__wrap_esp_transport_connect(t, expected_host, 443, 5000) == -1);
        assert(calls == before);
        relay_transport_destroy();
        t = make();
        before = calls;
        assert(__wrap_esp_transport_connect(t, expected_host, 80, 5000) == -1);
        assert(calls == before);
        relay_transport_destroy();
    } else if (!strcmp(argv[1], "resources")) {
        assert(!relay_transport_create("unused"));
        for (int i = 1; i <= 2; i++) {
            assert(relay_transport_resolve(expected_host));
            allocation_failure = i;
            assert(!relay_transport_create("X-Test-Proof: temporary\r\n"));
            assert(allocations == 0);
        }
        allocation_failure = 0;
        config_failure = 1;
        assert(!relay_transport_create("X-Test-Proof: temporary\r\n"));
        assert(allocations == 0);
        config_failure = 0;
        esp_transport_handle_t t = make();
        assert(!relay_transport_resolve("changed.example.com"));
        assert(!relay_transport_create("duplicate"));
        assert(__wrap_esp_transport_connect(t, expected_host, 443, 5000) == 0);
        /* Unrelated transport arguments and return value remain intact. */
        assert(__wrap_esp_transport_connect(NULL, "unrelated.example", 8443, 17) == 19);
        assert(__wrap_esp_transport_connect(&unrelated, expected_host, 443, 17) == 23);
        relay_transport_destroy();
        assert(allocations == 0);
    } else if (!strcmp(argv[1], "dns")) {
        assert(!relay_transport_resolve(NULL));
        assert(!relay_transport_resolve(""));
        char long_host[130]; memset(long_host, 'x', 129); long_host[129] = 0;
        assert(!relay_transport_resolve(long_host));
        dns_failure = 1;
        assert(!relay_transport_resolve(expected_host));
        assert(!relay_transport_create("unused"));
        dns_failure = 0;
        answers_n = 0;
        assert(!relay_transport_resolve(expected_host));
        answers[0] = "0.0.0.0"; answers[1] = "255.255.255.255"; answers_n = 2;
        assert(!relay_transport_resolve(expected_host));
        for (unsigned i = 0; i < 6; i++) answers[i] = "192.0.2.5";
        answers_n = 6;
        assert(relay_transport_resolve(expected_host));
        assert(relay_transport_address_count() == 1);
        answers[1] = "192.0.2.2"; answers[2] = "192.0.2.3";
        answers[3] = "192.0.2.4"; answers[4] = "192.0.2.9";
        assert(relay_transport_resolve(expected_host));
        assert(relay_transport_address_count() == 4 && dns_frees == 4);
        connect_result(0);
        assert(!strcmp(dialed, "192.0.2.2"));
        dns_failure = 1;
        assert(!relay_transport_resolve(expected_host));
        assert(!relay_transport_create("unused"));
    } else {
        assert(!"unknown test");
    }
    puts("relay transport check passed");
    return 0;
}
