#include "chs/common.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void chs_set_error(ChsError *error, const char *format, ...) {
    va_list arguments;

    if (error == NULL) {
        return;
    }

    va_start(arguments, format);
    vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}


static bool chs_buffer_reserve(ChsBuffer *buffer, size_t additional_size, ChsError *error) {
    size_t required_size;
    size_t new_capacity;
    uint8_t *new_data;

    required_size = buffer->length + additional_size;
    if (required_size <= buffer->capacity) {
        return true;
    }

    new_capacity = buffer->capacity == 0 ? 64 : buffer->capacity;
    while (new_capacity < required_size) {
        new_capacity *= 2;
    }

    new_data = realloc(buffer->data, new_capacity);
    if (new_data == NULL) {
        chs_set_error(error, "out of memory allocating %zu bytes", new_capacity);
        return false;
    }

    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return true;
}

bool chs_buffer_append(ChsBuffer *buffer, const void *data, size_t size, ChsError *error) {
    if (!chs_buffer_reserve(buffer, size, error)) {
        return false;
    }

    memcpy(buffer->data + buffer->length, data, size);
    buffer->length += size;
    return true;
}

bool chs_buffer_append_u8(ChsBuffer *buffer, uint8_t value, ChsError *error) {
    return chs_buffer_append(buffer, &value, sizeof(value), error);
}

bool chs_buffer_append_u16le(ChsBuffer *buffer, uint16_t value, ChsError *error) {
    uint8_t bytes[2];

    bytes[0] = (uint8_t) (value & 0xffu);
    bytes[1] = (uint8_t) ((value >> 8) & 0xffu);
    return chs_buffer_append(buffer, bytes, sizeof(bytes), error);
}

bool chs_buffer_append_u32le(ChsBuffer *buffer, uint32_t value, ChsError *error) {
    uint8_t bytes[4];

    bytes[0] = (uint8_t) (value & 0xffu);
    bytes[1] = (uint8_t) ((value >> 8) & 0xffu);
    bytes[2] = (uint8_t) ((value >> 16) & 0xffu);
    bytes[3] = (uint8_t) ((value >> 24) & 0xffu);
    return chs_buffer_append(buffer, bytes, sizeof(bytes), error);
}

bool chs_buffer_append_u64le(ChsBuffer *buffer, uint64_t value, ChsError *error) {
    uint8_t bytes[8];

    bytes[0] = (uint8_t) (value & 0xffu);
    bytes[1] = (uint8_t) ((value >> 8) & 0xffu);
    bytes[2] = (uint8_t) ((value >> 16) & 0xffu);
    bytes[3] = (uint8_t) ((value >> 24) & 0xffu);
    bytes[4] = (uint8_t) ((value >> 32) & 0xffu);
    bytes[5] = (uint8_t) ((value >> 40) & 0xffu);
    bytes[6] = (uint8_t) ((value >> 48) & 0xffu);
    bytes[7] = (uint8_t) ((value >> 56) & 0xffu);
    return chs_buffer_append(buffer, bytes, sizeof(bytes), error);
}

void chs_buffer_free(ChsBuffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

bool chs_string_assign(ChsString *string, const char *value, ChsError *error) {
    size_t size;
    char *copy;

    size = strlen(value) + 1;
    copy = realloc(string->data, size);
    if (copy == NULL) {
        chs_set_error(error, "out of memory duplicating string");
        return false;
    }

    memcpy(copy, value, size);
    string->data = copy;
    string->length = size - 1;
    string->capacity = size;
    return true;
}

void chs_string_free(ChsString *string) {
    free(string->data);
    string->data = NULL;
    string->length = 0;
    string->capacity = 0;
}

char *chs_strdup(const char *value, ChsError *error) {
    size_t size;
    char *copy;

    size = strlen(value) + 1;
    copy = malloc(size);
    if (copy == NULL) {
        chs_set_error(error, "out of memory duplicating string");
        return NULL;
    }

    memcpy(copy, value, size);
    return copy;
}

char *chs_trim(char *text) {
    char *end;

    while (*text != '\0' && isspace((unsigned char) *text)) {
        ++text;
    }

    end = text + strlen(text);
    while (end > text && isspace((unsigned char) end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

void chs_strip_line_comment(char *text) {
    while (*text != '\0') {
        if (text[0] == '/' && text[1] == '/') {
            *text = '\0';
            return;
        }
        ++text;
    }
}

bool chs_parse_u64(const char *text, uint64_t *value) {
    char *end_pointer;
    unsigned long long parsed;

    parsed = strtoull(text, &end_pointer, 0);
    if (*text == '\0' || *end_pointer != '\0') {
        return false;
    }

    *value = (uint64_t) parsed;
    return true;
}

bool chs_parse_i64(const char *text, int64_t *value) {
    char *end_pointer;
    long long parsed;

    parsed = strtoll(text, &end_pointer, 0);
    if (*text == '\0' || *end_pointer != '\0') {
        return false;
    }

    *value = (int64_t) parsed;
    return true;
}

bool chs_read_entire_file(const char *path, char **data, size_t *size, ChsError *error) {
    FILE *file;
    long file_size;
    char *buffer;

    file = fopen(path, "rb");
    if (file == NULL) {
        chs_set_error(error, "failed to open %s", path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        chs_set_error(error, "failed to seek %s", path);
        return false;
    }

    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        chs_set_error(error, "failed to determine size of %s", path);
        return false;
    }

    rewind(file);
    buffer = malloc((size_t) file_size + 1);
    if (buffer == NULL) {
        fclose(file);
        chs_set_error(error, "out of memory reading %s", path);
        return false;
    }

    if (fread(buffer, 1, (size_t) file_size, file) != (size_t) file_size) {
        free(buffer);
        fclose(file);
        chs_set_error(error, "failed to read %s", path);
        return false;
    }

    buffer[file_size] = '\0';
    fclose(file);
    *data = buffer;
    *size = (size_t) file_size;
    return true;
}

bool chs_write_entire_file(const char *path, const uint8_t *data, size_t size, ChsError *error) {
    FILE *file;

    file = fopen(path, "wb");
    if (file == NULL) {
        chs_set_error(error, "failed to create %s", path);
        return false;
    }

    if (size != 0 && fwrite(data, 1, size, file) != size) {
        fclose(file);
        chs_set_error(error, "failed to write %s", path);
        return false;
    }

    fclose(file);
    return true;
}

bool chs_make_temp_path(const char *suffix, ChsString *path, ChsError *error) {
    size_t suffix_length;
    size_t template_length;
    char *template;
    int fd;
    bool success;

    suffix_length = strlen(suffix);
    template_length = strlen("/tmp/chsXXXXXX") + suffix_length + 1;
    template = malloc(template_length);
    if (template == NULL) {
        chs_set_error(error, "out of memory creating temp path");
        return false;
    }

    snprintf(template, template_length, "/tmp/chsXXXXXX%s", suffix);
    fd = mkstemps(template, (int) suffix_length);
    if (fd < 0) {
        chs_set_error(error, "failed to create temp file: %s", strerror(errno));
        free(template);
        return false;
    }

    close(fd);
    success = chs_string_assign(path, template, error);
    free(template);
    return success;
}

bool chs_run_process(const char *program, char *const argv[], ChsError *error) {
    pid_t child;
    int status;

    child = fork();
    if (child < 0) {
        chs_set_error(error, "failed to fork %s: %s", program, strerror(errno));
        return false;
    }

    if (child == 0) {
        execvp(program, argv);
        _exit(127);
    }

    if (waitpid(child, &status, 0) < 0) {
        chs_set_error(error, "failed waiting for %s: %s", program, strerror(errno));
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status)) {
            chs_set_error(error, "%s exited with status %d", program, WEXITSTATUS(status));
        } else {
            chs_set_error(error, "%s terminated abnormally", program);
        }
        return false;
    }

    return true;
}

uint64_t chs_align_up_u64(uint64_t value, uint64_t alignment) {
    if (alignment <= 1) {
        return value;
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t chs_version_triplet(unsigned major, unsigned minor, unsigned patch) {
    return ((uint32_t) major << 16) | ((uint32_t) minor << 8) | (uint32_t) patch;
}
