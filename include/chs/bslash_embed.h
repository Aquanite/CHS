#ifndef CHS_BSLASH_EMBED_H
#define CHS_BSLASH_EMBED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int bas_assemble_file(const char *input_path,
                      const char *output_path,
                      bool emit_object,
                      bool has_origin,
                      uint32_t origin,
                      char *error_buffer,
                      size_t error_buffer_size);

#endif
