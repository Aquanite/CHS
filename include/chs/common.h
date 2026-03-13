#ifndef CHS_COMMON_H
#define CHS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <stdlib.h>
#include <string.h>
#ifndef strdup
#define strdup _strdup
#endif
#ifndef HAVE_STRNLEN
static inline size_t chs_strnlen_fallback(const char *value, size_t max_len) {
    size_t len = 0;
    while (len < max_len && value[len] != '\0') {
        ++len;
    }
    return len;
}
#define strnlen chs_strnlen_fallback
#endif
#endif

typedef struct {
    char message[512];
} ChsError;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} ChsString;

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
} ChsBuffer;

void chs_set_error(ChsError *error, const char *format, ...);
bool chs_buffer_append(ChsBuffer *buffer, const void *data, size_t size, ChsError *error);
bool chs_buffer_append_u8(ChsBuffer *buffer, uint8_t value, ChsError *error);
bool chs_buffer_append_u16le(ChsBuffer *buffer, uint16_t value, ChsError *error);
bool chs_buffer_append_u32le(ChsBuffer *buffer, uint32_t value, ChsError *error);
bool chs_buffer_append_u64le(ChsBuffer *buffer, uint64_t value, ChsError *error);
void chs_buffer_free(ChsBuffer *buffer);
bool chs_string_assign(ChsString *string, const char *value, ChsError *error);
void chs_string_free(ChsString *string);
char *chs_strdup(const char *value, ChsError *error);
char *chs_trim(char *text);
void chs_strip_line_comment(char *text);
bool chs_parse_u64(const char *text, uint64_t *value);
bool chs_parse_i64(const char *text, int64_t *value);
bool chs_read_entire_file(const char *path, char **data, size_t *size, ChsError *error);
bool chs_write_entire_file(const char *path, const uint8_t *data, size_t size, ChsError *error);
bool chs_make_temp_path(const char *suffix, ChsString *path, ChsError *error);
bool chs_run_process(const char *program, char *const argv[], ChsError *error);
uint64_t chs_align_up_u64(uint64_t value, uint64_t alignment);
uint32_t chs_version_triplet(unsigned major, unsigned minor, unsigned patch);

#endif
