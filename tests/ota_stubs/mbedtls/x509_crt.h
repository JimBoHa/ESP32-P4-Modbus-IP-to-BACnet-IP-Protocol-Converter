#ifndef TEST_OTA_X509_H
#define TEST_OTA_X509_H
#include "mbedtls/pk.h"
typedef struct { mbedtls_pk_context pk; } mbedtls_x509_crt;
void mbedtls_x509_crt_init(mbedtls_x509_crt *);
void mbedtls_x509_crt_free(mbedtls_x509_crt *);
int mbedtls_x509_crt_parse(mbedtls_x509_crt *, const unsigned char *, size_t);
#endif
