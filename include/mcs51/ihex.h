/*
 * ihex.h - Intel HEX (I8HEX/I16HEX/I32HEX) loader.
 */
#ifndef MCS51_IHEX_H
#define MCS51_IHEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcs51_ihex_info {
    uint32_t bytes;   /* data bytes stored */
    uint32_t lowest;  /* lowest address written (valid if bytes > 0) */
    uint32_t highest; /* highest address written (valid if bytes > 0) */
    unsigned line;    /* line of the first error, 0 if the error has no line */
    char error[96];   /* empty on success */
} mcs51_ihex_info;

/* Parse Intel HEX text into mem[0 .. mem_size). Record types: 00 data,
 * 01 end of file (required), 02 extended segment address, 04 extended linear
 * address, 03/05 start address (ignored). Every record's checksum is checked.
 * Returns 0 on success, -1 on error (details in info). Bytes before the
 * failing record have already been stored. */
int mcs51_ihex_parse(const char *text, size_t len, uint8_t *mem, size_t mem_size,
                     mcs51_ihex_info *info);

/* Read a file and parse it with mcs51_ihex_parse. */
int mcs51_ihex_load_file(const char *path, uint8_t *mem, size_t mem_size,
                         mcs51_ihex_info *info);

#ifdef __cplusplus
}
#endif

#endif /* MCS51_IHEX_H */
