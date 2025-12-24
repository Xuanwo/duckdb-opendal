#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct duckdb_opendal_operator duckdb_opendal_operator;
typedef struct duckdb_opendal_lister duckdb_opendal_lister;

duckdb_opendal_operator *duckdb_opendal_operator_new(const char *service,
                                                     const char *const *keys,
                                                     const char *const *values,
                                                     size_t len,
                                                     char **out_error);
void duckdb_opendal_operator_free(duckdb_opendal_operator *op);

bool duckdb_opendal_stat(duckdb_opendal_operator *op,
                         const char *path,
                         bool *out_exists,
                         bool *out_is_dir,
                         uint64_t *out_size,
                         char **out_error);

bool duckdb_opendal_read(duckdb_opendal_operator *op,
                         const char *path,
                         uint64_t offset,
                         uint8_t *buffer,
                         size_t buffer_len,
                         size_t *out_bytes_read,
                         char **out_error);

duckdb_opendal_lister *duckdb_opendal_list(duckdb_opendal_operator *op,
                                           const char *path,
                                           bool recursive,
                                           char **out_error);
bool duckdb_opendal_lister_next(duckdb_opendal_lister *lister, char **out_path, bool *out_is_dir, uint64_t *out_size);
void duckdb_opendal_lister_free(duckdb_opendal_lister *lister);

void duckdb_opendal_string_free(char *s);

#ifdef __cplusplus
}
#endif

