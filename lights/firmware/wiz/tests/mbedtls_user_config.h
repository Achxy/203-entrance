/* Keep the pinned ESP-IDF's host MPI wrappers usable. Production configuration
 * still comes from sdkconfig; this is only the host cryptographic test build. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
