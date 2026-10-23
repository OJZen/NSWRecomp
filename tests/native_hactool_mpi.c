// Synthetic vectors from Python's modular exponentiation, run during tool preparation.
#include <mbedtls/bignum.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(void) {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) < 0) return 1;
#endif
    unsigned char data[768], out[256], exponent[3] = {1, 0, 1};
    unsigned count = 0; size_t size;
    while ((size = fread(data, 1, sizeof data, stdin))) {
        if (size != sizeof data) return 1;
        mbedtls_mpi value, modulus, e, result;
        mbedtls_mpi_init(&value); mbedtls_mpi_init(&modulus);
        mbedtls_mpi_init(&e); mbedtls_mpi_init(&result);
        int error = mbedtls_mpi_read_binary(&modulus, data, 256);
        if (!error) error = mbedtls_mpi_read_binary(&value, data + 256, 256);
        if (!error) error = mbedtls_mpi_read_binary(&e, exponent, sizeof exponent);
        if (!error) error = mbedtls_mpi_exp_mod(&result, &value, &e, &modulus, NULL);
        if (!error) error = mbedtls_mpi_write_binary(&result, out, sizeof out);
        if (!error) error = memcmp(out, data + 512, 256) != 0;
        mbedtls_mpi_free(&value); mbedtls_mpi_free(&modulus);
        mbedtls_mpi_free(&e); mbedtls_mpi_free(&result);
        if (error) return 2;
        count++;
    }
    if (ferror(stdin) || !count) return 1;
    printf("Synthetic MPI checks passed: %u\n", count);
    return 0;
}
