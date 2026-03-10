#include "chs/assembler.h"

#include <stdio.h>
#include <string.h>

static void chs_print_usage(FILE *stream) {
    fprintf(stream, "usage: chs --arch <arm64|x86_64> --format <macho|elf64> --output <path> <input> [-d|-vd]\n");
}

static void chs_print_version(FILE *stream) {
    fprintf(stream, "chs: CHance Assembler version 0.5.0\n");
    fprintf(stream, "chs: Assembly support: arm64, x86_64\n");
    fprintf(stream, "chs: License: OpenAzure License\n");
    fprintf(stream, "chs: Compiled on %s %s\n", __DATE__, __TIME__);
    fprintf(stream, "chs: Created by Nathan Hornby (AzureianGH)\n");
}

static void chs_print_debug_header(void) {
    fprintf(stderr, "\n[debug] Configuration\n");
}

static void chs_print_debug_row(const char *key, const char *value) {
    fprintf(stderr, "  %-16s : %s\n", key, value ? value : "-");
}


static bool chs_parse_arch(const char *text, ChsArchKind *arch) {
    if (strcmp(text, "arm64") == 0) {
        *arch = CHS_ARCH_ARM64;
        return true;
    }
    if (strcmp(text, "x86_64") == 0) {
        *arch = CHS_ARCH_X86_64;
        return true;
    }
    return false;
}

static bool chs_parse_format(const char *text, ChsOutputKind *output_kind) {
    if (strcmp(text, "macho") == 0) {
        *output_kind = CHS_OUTPUT_MACHO;
        return true;
    }
    if (strcmp(text, "elf64") == 0) {
        *output_kind = CHS_OUTPUT_ELF64;
        return true;
    }
    return false;
}

int main(int argc, char **argv) {
    ChsAssembleOptions options;
    ChsError error;
    bool debug_enabled;
    bool debug_deep;
    int index;

    memset(&options, 0, sizeof(options));
    memset(&error, 0, sizeof(error));
    debug_enabled = false;
    debug_deep = false;

    if (argc < 2) {
        chs_print_usage(stderr);
        return 1;
    }

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--arch") == 0) {
            if (index + 1 >= argc || !chs_parse_arch(argv[++index], &options.arch)) {
                chs_print_usage(stderr);
                return 1;
            }
        } else if (strcmp(argv[index], "--format") == 0) {
            if (index + 1 >= argc || !chs_parse_format(argv[++index], &options.output_kind)) {
                chs_print_usage(stderr);
                return 1;
            }
        } else if (strcmp(argv[index], "--output") == 0 || strcmp(argv[index], "-o") == 0) {
            if (index + 1 >= argc) {
                chs_print_usage(stderr);
                return 1;
            }
            options.output_path = argv[++index];
        } else if (strcmp(argv[index], "--version") == 0) {
            chs_print_version(stdout);
            return 0;
        } else if (strcmp(argv[index], "-d") == 0 || strcmp(argv[index], "--debug") == 0) {
            debug_enabled = true;
        } else if (strcmp(argv[index], "-vd") == 0 || strcmp(argv[index], "--verbose-deep") == 0) {
            debug_enabled = true;
            debug_deep = true;
        } else if (argv[index][0] == '-') {
            chs_print_usage(stderr);
            return 1;
        } else {
            options.input_path = argv[index];
        }
    }

    if (options.input_path == NULL || options.output_path == NULL) {
        chs_print_usage(stderr);
        return 1;
    }

    if (debug_enabled) {
        const char *arch_name = options.arch == CHS_ARCH_ARM64 ? "arm64" : "x86_64";
        const char *format_name = options.output_kind == CHS_OUTPUT_MACHO ? "macho" : "elf64";
        chs_print_debug_header();
        chs_print_debug_row("Input", options.input_path);
        chs_print_debug_row("Output", options.output_path);
        chs_print_debug_row("Arch", arch_name);
        chs_print_debug_row("Format", format_name);
        if (debug_deep) {
            chs_print_debug_row("Deep mode", "enabled");
        }
    }

    if (!chs_assemble_file(&options, &error)) {
        fprintf(stderr, "chs: %s\n", error.message);
        return 1;
    }

    return 0;
}