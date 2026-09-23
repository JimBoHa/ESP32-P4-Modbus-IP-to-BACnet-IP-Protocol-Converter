#ifndef TEST_OTA_PK_H
#define TEST_OTA_PK_H
#include <stddef.h>
typedef struct { int unused; } mbedtls_pk_context;
void mbedtls_pk_init(mbedtls_pk_context *);
void mbedtls_pk_free(mbedtls_pk_context *);
int mbedtls_pk_parse_key(mbedtls_pk_context *, const unsigned char *, size_t, const unsigned char *, size_t, int (*)(void *,unsigned char *,size_t), void *);
int mbedtls_pk_check_pair(const mbedtls_pk_context *, const mbedtls_pk_context *, int (*)(void *,unsigned char *,size_t), void *);
#endif
