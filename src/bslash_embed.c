#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stddef.h>
#include <setjmp.h>

#include "chs/bslash_embed.h"
#include "chs/bso_format.h"

#if defined(_WIN32) && !defined(strdup)
#define strdup _strdup
#endif

#if defined(_WIN32) && !defined(_MAX_PATH)
#define _MAX_PATH 260
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_OPERANDS 4
#define BSLASH_DEFAULT_ORIGIN_BASE 0xA0000000u

static uint32_t g_origin_base = BSLASH_DEFAULT_ORIGIN_BASE;
typedef enum {
	ORIGIN_SOURCE_DEFAULT,
	ORIGIN_SOURCE_CMDLINE,
	ORIGIN_SOURCE_DIRECTIVE
} origin_source_t;
static origin_source_t g_origin_source = ORIGIN_SOURCE_DEFAULT;
static const char *g_current_source = NULL;
static jmp_buf *g_fatal_jmp = NULL;
static char *g_error_buffer = NULL;
static size_t g_error_buffer_size = 0;

static void fatal(size_t line, const char *fmt, ...);

static void reset_assembler_state(void) {
	g_origin_base = BSLASH_DEFAULT_ORIGIN_BASE;
	g_origin_source = ORIGIN_SOURCE_DEFAULT;
	g_current_source = NULL;
}

static void format_error_message(char *buffer, size_t buffer_size, size_t line, const char *fmt, va_list args) {
	int written = 0;
	if (!buffer || buffer_size == 0) {
		return;
	}
	buffer[0] = '\0';
	written += snprintf(buffer + written, written < (int)buffer_size ? buffer_size - (size_t)written : 0,
	                   "bas: error");
	if (line) {
		if (g_current_source) {
			written += snprintf(buffer + written, written < (int)buffer_size ? buffer_size - (size_t)written : 0,
			                    " (%s:%zu)", g_current_source, line);
		} else {
			written += snprintf(buffer + written, written < (int)buffer_size ? buffer_size - (size_t)written : 0,
			                    " (line %zu)", line);
		}
	} else if (g_current_source) {
		written += snprintf(buffer + written, written < (int)buffer_size ? buffer_size - (size_t)written : 0,
		                    " (%s)", g_current_source);
	}
	written += snprintf(buffer + written, written < (int)buffer_size ? buffer_size - (size_t)written : 0,
	                   ": ");
	if ((size_t)written < buffer_size) {
		vsnprintf(buffer + written, buffer_size - (size_t)written, fmt, args);
	}
}

static void set_origin_base(uint32_t value, origin_source_t source, size_t line) {
	if (source == ORIGIN_SOURCE_CMDLINE) {
		if (g_origin_source == ORIGIN_SOURCE_CMDLINE) {
			fatal(line, "--origin specified multiple times");
		}
		if (g_origin_source == ORIGIN_SOURCE_DIRECTIVE) {
			fatal(line, "--origin cannot override %%origin directive");
		}
	} else if (source == ORIGIN_SOURCE_DIRECTIVE) {
		if (g_origin_source == ORIGIN_SOURCE_CMDLINE) {
			fatal(line, "%%origin cannot override --origin");
		}
		if (g_origin_source == ORIGIN_SOURCE_DIRECTIVE) {
			fatal(line, "%%origin may only be specified once");
		}
	}
	g_origin_base = value;
	g_origin_source = source;
}

static bool keyword_equals_ci(const char *text, size_t len, const char *keyword) {
	size_t key_len = strlen(keyword);
	if (len != key_len) {
		return false;
	}
	for (size_t i = 0; i < len; ++i) {
		unsigned char a = (unsigned char)text[i];
		unsigned char b = (unsigned char)keyword[i];
		if (tolower(a) != tolower(b)) {
			return false;
		}
	}
	return true;
}

typedef enum {
	FMT_NONE,
	FMT_REG_REG,
	FMT_REG_CTL,
	FMT_CTL_REG,
	FMT_REG_REG_REG,
	FMT_REG_IMM32,
	FMT_REG_IMM16,
	FMT_REG_IMM8,
	FMT_BRANCH_REL8,
	FMT_BRANCH_REL32,
	FMT_ABS32_ONLY,
	FMT_REG_HIGH,
	FMT_IMM8,
	FMT_IMM16,
	FMT_MEM_LOAD,
	FMT_MEM_STORE,
	FMT_REG_MEM_IMM8,
	FMT_REG_REL32,
	FMT_EXT,
	FMT_CTL_IDX,
	FMT_IMM32_ONLY,
	FMT_RRPIN,
	FMT_RRPIN_R,
	FMT_RRPIN_V,
		FMT_RRPIN_VR,
		FMT_PORT_OUT,
		FMT_PORT_IN,
	FMT_REG_MEM
} instr_format_t;

typedef struct {
	const char *mnemonic;
	uint8_t opcode;
	instr_format_t format;
	uint8_t size_bytes;
} instr_def_t;

static const instr_def_t g_instructions[] = {
	{"NOP", 0x36, FMT_NONE, 1},
	{"BRK", 0x37, FMT_NONE, 1},
	{"RET", 0x26, FMT_NONE, 1},
	{"LEAVE", 0x2E, FMT_NONE, 1},
	{"PSTAT", 0x30, FMT_NONE, 1},
	{"POPSTAT", 0x31, FMT_NONE, 1},
	{"PCTL", 0x32, FMT_CTL_IDX, 2},
	{"POPCTL", 0x33, FMT_CTL_IDX, 2},
	{"PFR", 0x34, FMT_NONE, 1},
	{"POPFR", 0x35, FMT_NONE, 1},
	{"ROR", 0x40, FMT_REG_REG, 2},
	{"RORI8", 0x41, FMT_REG_IMM8, 3},
	{"POPC", 0x42, FMT_REG_REG, 2},
	{"TXBEGIN", 0xB0, FMT_NONE, 1},
	{"TXEND", 0xB1, FMT_NONE, 1},
	{"TXABORT", 0xB2, FMT_IMM8, 2},
	{"SPAWN", 0xC0, FMT_REG_REG_REG, 3},
	{"YIELD", 0xC1, FMT_REG_HIGH, 2},
	{"TLOG", 0xD0, FMT_REG_IMM8, 3},
	{"PERF", 0xD1, FMT_REG_IMM8, 3},
	{"PEEKPTE", 0xD2, FMT_REG_MEM, 2},
	{"RDBAD", 0xD3, FMT_REG_HIGH, 2},
	{"RDFLSH", 0xD4, FMT_REG_REG, 2},
	{"WRFLSH", 0xD5, FMT_REG_REG, 2},
	{"CICPY", 0xD6, FMT_REG_REG_REG, 3},

	{"ADC", 0x00, FMT_REG_REG, 2},
	{"ADCI32", 0x01, FMT_REG_IMM32, 6},
	{"ADCI16", 0x02, FMT_REG_IMM16, 4},
	{"ADCI8", 0x03, FMT_REG_IMM8, 3},
	{"ADD", 0x04, FMT_REG_REG, 2},
	{"ADDI32", 0x05, FMT_REG_IMM32, 6},
	{"ADDI16", 0x06, FMT_REG_IMM16, 4},
	{"ADDI8", 0x07, FMT_REG_IMM8, 3},
	{"SUB", 0x0E, FMT_REG_REG, 2},
	{"SUBI32", 0x1D, FMT_REG_IMM32, 6},
	{"SUBI16", 0x1E, FMT_REG_IMM16, 4},
	{"SUBI8", 0x1F, FMT_REG_IMM8, 3},
	{"ADC.F", 0x50, FMT_REG_REG, 2},
	{"ADCI32.F", 0x51, FMT_REG_IMM32, 6},
	{"ADCI16.F", 0x52, FMT_REG_IMM16, 4},
	{"ADCI8.F", 0x53, FMT_REG_IMM8, 3},
	{"ADD.F", 0x54, FMT_REG_REG, 2},
	{"ADDI32.F", 0x55, FMT_REG_IMM32, 6},
	{"ADDI16.F", 0x56, FMT_REG_IMM16, 4},
	{"ADDI8.F", 0x57, FMT_REG_IMM8, 3},
	{"AND.F", 0x58, FMT_REG_REG, 2},
	{"ANDI32.F", 0x59, FMT_REG_IMM32, 6},
	{"ANDI16.F", 0x5A, FMT_REG_IMM16, 4},
	{"ANDI8.F", 0x5B, FMT_REG_IMM8, 3},
	{"SUB.F", 0x5C, FMT_REG_REG, 2},
	{"OR.F", 0x5D, FMT_REG_REG, 2},
	{"XOR.F", 0x5E, FMT_REG_REG, 2},
	{"SUBI32.F", 0x5F, FMT_REG_IMM32, 6},
	{"SUBI16.F", 0x60, FMT_REG_IMM16, 4},
	{"SUBI8.F", 0x61, FMT_REG_IMM8, 3},
	{"FADD", 0x80, FMT_REG_REG, 2},
	{"FSUB", 0x81, FMT_REG_REG, 2},
	{"FMUL", 0x82, FMT_REG_REG, 2},
	{"FDIV", 0x83, FMT_REG_REG, 2},
	{"FFMA", 0x84, FMT_REG_REG_REG, 3},
	{"FINV", 0x85, FMT_REG_REG, 2},
	{"FSQRT", 0x86, FMT_REG_REG, 2},
	{"FCMP", 0x87, FMT_REG_REG, 2},
	{"FCMPI", 0x88, FMT_REG_IMM32, 6},
	{"ITOF", 0x89, FMT_REG_REG, 2},
	{"FTOI", 0x8A, FMT_REG_REG, 2},
	{"FMOV", 0x8B, FMT_REG_REG, 2},
	{"FMOVI32", 0x8C, FMT_REG_IMM32, 6},
	{"FADD.F", 0x90, FMT_REG_REG, 2},
	{"FSUB.F", 0x91, FMT_REG_REG, 2},
	{"FMUL.F", 0x92, FMT_REG_REG, 2},
	{"FDIV.F", 0x93, FMT_REG_REG, 2},
	{"FINV.F", 0x94, FMT_REG_REG, 2},
	{"FSQRT.F", 0x95, FMT_REG_REG, 2},
	{"AND", 0x08, FMT_REG_REG, 2},
	{"ANDI32", 0x09, FMT_REG_IMM32, 6},
	{"ANDI16", 0x0A, FMT_REG_IMM16, 4},
	{"ANDI8", 0x0B, FMT_REG_IMM8, 3},
	{"OR", 0x0F, FMT_REG_REG, 2},
	{"ORI32", 0x17, FMT_REG_IMM32, 6},
	{"ORI16", 0x18, FMT_REG_IMM16, 4},
	{"ORI8", 0x19, FMT_REG_IMM8, 3},
	{"XOR", 0x10, FMT_REG_REG, 2},
	{"XORI32", 0x1A, FMT_REG_IMM32, 6},
	{"XORI16", 0x1B, FMT_REG_IMM16, 4},
	{"XORI8", 0x1C, FMT_REG_IMM8, 3},
	{"NOT", 0x11, FMT_REG_REG, 2},
	{"MOV", 0x15, FMT_REG_REG, 2},
	{"MOVI32", 0x16, FMT_REG_IMM32, 6},
	{"MUL", 0x12, FMT_REG_REG_REG, 3},
	{"DIV", 0x13, FMT_REG_REG_REG, 3},
	{"UDIV", 0x14, FMT_REG_REG_REG, 3},

	{"CMP", 0x0C, FMT_REG_REG, 2},
	{"CMPI32", 0x0D, FMT_REG_IMM32, 6},

	{"BZ", 0x20, FMT_BRANCH_REL8, 2},
	{"BNZ", 0x21, FMT_BRANCH_REL8, 2},
	{"BS", 0x22, FMT_BRANCH_REL8, 2},
	{"BNS", 0x23, FMT_BRANCH_REL8, 2},
	{"BO", 0x24, FMT_BRANCH_REL8, 2},
	{"BNO", 0x25, FMT_BRANCH_REL8, 2},
	{"BGE", 0xED, FMT_BRANCH_REL8, 2},
	{"BLT", 0xEE, FMT_BRANCH_REL8, 2},
	{"BGT", 0xEF, FMT_BRANCH_REL8, 2},
	{"BLE", 0xF0, FMT_BRANCH_REL8, 2},
	{"BGEU", 0xF1, FMT_BRANCH_REL8, 2},
	{"BLTU", 0xF2, FMT_BRANCH_REL8, 2},
	{"BGTU", 0xF3, FMT_BRANCH_REL8, 2},
	{"BLEU", 0xF4, FMT_BRANCH_REL8, 2},
	{"PUSHR", 0xF5, FMT_REG_HIGH, 2},
	{"POPR", 0xF6, FMT_REG_HIGH, 2},
	{"PUSHI32", 0xF7, FMT_IMM32_ONLY, 6},
	{"RRPIN", 0xF8, FMT_RRPIN, 0},
	{"RRPIN.R", 0xF9, FMT_RRPIN_R, 0},
	{"RRPIN.V", 0xFA, FMT_RRPIN_V, 0},
	{"RRPIN.VR", 0xFB, FMT_RRPIN_VR, 0},
	{"J", 0x27, FMT_BRANCH_REL8, 2},
	{"J32", 0x28, FMT_BRANCH_REL32, 5},
	{"CALL", 0x29, FMT_BRANCH_REL32, 5},
	{"CALLR", 0x2A, FMT_REG_HIGH, 2},
	{"JR", 0x2B, FMT_REG_HIGH, 2},
	{"JA", 0x2C, FMT_ABS32_ONLY, 5},
	{"CALLA", 0x2D, FMT_ABS32_ONLY, 5},

	{"ENTERI16", 0x2F, FMT_IMM16, 3},

	{"BTST", 0x45, FMT_REG_IMM8, 3},
	{"CLZ", 0x43, FMT_REG_REG, 2},
	{"CTZ", 0x44, FMT_REG_REG, 2},
	{"SHL", 0x46, FMT_REG_REG, 2},
	{"SHLI8", 0x47, FMT_REG_IMM8, 3},
	{"LSR", 0x48, FMT_REG_REG, 2},
	{"LSRI8", 0x49, FMT_REG_IMM8, 3},
	{"ASR", 0x4A, FMT_REG_REG, 2},
	{"ASRI8", 0x4B, FMT_REG_IMM8, 3},
	{"BEXT", 0x70, FMT_REG_REG_REG, 3},
	{"BDEP", 0x71, FMT_REG_REG_REG, 3},

	{"RDSTK", 0x38, FMT_REG_HIGH, 2},
	{"WRSTK", 0x39, FMT_REG_HIGH, 2},
	{"RDFR", 0x3A, FMT_REG_HIGH, 2},
	{"WRFR", 0x3B, FMT_REG_HIGH, 2},

	{"LD.K", 0xA0, FMT_MEM_LOAD, 3},
	{"ST.K", 0xA1, FMT_MEM_STORE, 3},
	{"LD", 0xA2, FMT_MEM_LOAD, 3},
	{"ST", 0xA3, FMT_MEM_STORE, 3},
	{"LDRGN", 0xA4, FMT_REG_MEM_IMM8, 3},
	{"LDBU", 0xA5, FMT_MEM_LOAD, 3},
	{"LDBS", 0xA6, FMT_MEM_LOAD, 3},
	{"LDHU", 0xA7, FMT_MEM_LOAD, 3},
	{"LDHS", 0xA8, FMT_MEM_LOAD, 3},
	{"STB", 0xA9, FMT_MEM_STORE, 3},
	{"STH", 0xAA, FMT_MEM_STORE, 3},
	{"LDRIP", 0xAB, FMT_REG_REL32, 6},
	{"LL", 0xAC, FMT_MEM_LOAD, 3},
	{"SC", 0xAD, FMT_MEM_STORE, 3},

	{"SYSRD", 0xE0, FMT_REG_CTL, 2},
	{"SYSWR", 0xE1, FMT_CTL_REG, 2},
	{"SYSCALL", 0xE2, FMT_NONE, 1},
	{"WFI", 0xE3, FMT_NONE, 1},
	{"IRET", 0xE4, FMT_NONE, 1},
	{"ENI", 0xE5, FMT_NONE, 1},
	{"DIS", 0xE6, FMT_NONE, 1},
	{"FENCE", 0xE7, FMT_NONE, 1},
	{"RDTSC", 0xE8, FMT_REG_HIGH, 2},
	{"CPUID", 0xE9, FMT_IMM8, 2},
	{"EXT", 0xCB, FMT_EXT, 3},
	{"MTCC", 0xCB, FMT_EXT, 3},
	{"MTID", 0xCB, FMT_EXT, 3},
	{"MTSTS", 0xCB, FMT_EXT, 3},
	{"MTSETIP", 0xCB, FMT_EXT, 3},
	{"MTI", 0xCB, FMT_EXT, 3},
	{"FENCE.R", 0xEA, FMT_NONE, 1},
	{"FENCE.W", 0xEB, FMT_NONE, 1},
	{"FENCE.I", 0xEC, FMT_NONE, 1},
	{"SICF", 0xFC, FMT_IMM8, 2},
	{"OUTPRT.S", 0xFD, FMT_PORT_OUT, 4},
	{"INPRT.S", 0xFE, FMT_PORT_IN, 4},
};

typedef struct {
	char *name;
	uint32_t address;
	size_t line;
} symbol_t;

typedef struct {
	symbol_t *data;
	size_t count;
	size_t capacity;
} symbol_vec_t;

typedef struct {
	uint8_t *data;
	size_t count;
	size_t capacity;
} byte_buf_t;

typedef struct {
	char **data;
	size_t count;
	size_t capacity;
} path_vec_t;

typedef struct {
	char *label;
	uint8_t *bytes;
	size_t count;
	size_t line;
} string_lit_t;

typedef struct {
	string_lit_t *data;
	size_t count;
	size_t capacity;
} string_lit_vec_t;

static void ensure_path_capacity(path_vec_t *vec) {
	if (vec->count >= vec->capacity) {
		size_t new_cap = vec->capacity ? vec->capacity * 2u : 8u;
		char **new_data = (char **)realloc(vec->data, new_cap * sizeof(char *));
		if (!new_data) {
			fatal(0, "out of memory while growing include list");
		}
		vec->data = new_data;
		vec->capacity = new_cap;
	}
}

static void ensure_string_lit_capacity(string_lit_vec_t *vec) {
	if (vec->count >= vec->capacity) {
		size_t new_cap = vec->capacity ? vec->capacity * 2u : 8u;
		string_lit_t *new_data = (string_lit_t *)realloc(vec->data, new_cap * sizeof(string_lit_t));
		if (!new_data) {
			fatal(0, "out of memory while growing string literal table");
		}
		vec->data = new_data;
		vec->capacity = new_cap;
	}
}

static void free_path_vec(path_vec_t *vec) {
	for (size_t i = 0; i < vec->count; ++i) {
		free(vec->data[i]);
	}
	free(vec->data);
	vec->data = NULL;
	vec->count = 0;
	vec->capacity = 0;
}

static const char *path_vec_intern(path_vec_t *vec, const char *path, bool *added) {
	for (size_t i = 0; i < vec->count; ++i) {
		if (strcmp(vec->data[i], path) == 0) {
			if (added) {
				*added = false;
			}
			return vec->data[i];
		}
	}
	ensure_path_capacity(vec);
	char *copy = strdup(path);
	if (!copy) {
		fatal(0, "out of memory while storing include path");
	}
	vec->data[vec->count++] = copy;
	if (added) {
		*added = true;
	}
	return copy;
}

#if defined(_WIN32)
static bool is_path_separator(char c) {
	return c == '/' || c == '\\';
}
static char preferred_path_separator(void) {
	return '\\';
}
#else
static bool is_path_separator(char c) {
	return c == '/';
}
static char preferred_path_separator(void) {
	return '/';
}
#endif

static bool path_is_absolute(const char *path) {
	if (!path || !*path) {
		return false;
	}
#if defined(_WIN32)
	if (is_path_separator(path[0])) {
		return true;
	}
	if (isalpha((unsigned char)path[0]) && path[1] == ':' && is_path_separator(path[2])) {
		return true;
	}
	return false;
#else
	return path[0] == '/';
#endif
}

static char *duplicate_dirname(const char *path) {
	if (!path || !*path) {
		char *dot = strdup(".");
		if (!dot) {
			fatal(0, "out of memory duplicating dirname");
		}
		return dot;
	}
	size_t len = strlen(path);
	ptrdiff_t last_sep = -1;
	for (size_t i = 0; i < len; ++i) {
		if (is_path_separator(path[i])) {
			last_sep = (ptrdiff_t)i;
		}
	}
	if (last_sep < 0) {
		char *dot = strdup(".");
		if (!dot) {
			fatal(0, "out of memory duplicating dirname");
		}
		return dot;
	}
	if (last_sep == 0) {
		char *root = (char *)malloc(2);
		if (!root) {
			fatal(0, "out of memory duplicating root dirname");
		}
		root[0] = path[0];
		root[1] = '\0';
		return root;
	}
	size_t length = (size_t)last_sep;
	char *dir = (char *)malloc(length + 1);
	if (!dir) {
		fatal(0, "out of memory duplicating dirname");
	}
	memcpy(dir, path, length);
	dir[length] = '\0';
	return dir;
}

static char *join_paths(const char *base_dir, const char *relative) {
	if (!relative) {
		return NULL;
	}
	if (path_is_absolute(relative)) {
		char *copy = strdup(relative);
		if (!copy) {
			fatal(0, "out of memory joining paths");
		}
		return copy;
	}
	const char *base = (base_dir && *base_dir) ? base_dir : ".";
	size_t base_len = strlen(base);
	size_t rel_len = strlen(relative);
	bool need_sep = base_len > 0 && !is_path_separator(base[base_len - 1]);
	size_t total = base_len + (need_sep ? 1 : 0) + rel_len + 1;
	char *result = (char *)malloc(total);
	if (!result) {
		fatal(0, "out of memory joining paths");
	}
	memcpy(result, base, base_len);
	size_t pos = base_len;
	if (need_sep) {
		result[pos++] = preferred_path_separator();
	}
	memcpy(result + pos, relative, rel_len);
	result[pos + rel_len] = '\0';
	return result;
}

static char *canonicalize_path(const char *path) {
	if (!path) {
		return NULL;
	}
#if defined(_WIN32)
	char buffer[_MAX_PATH];
	if (!_fullpath(buffer, path, _MAX_PATH)) {
		return NULL;
	}
	return strdup(buffer);
#else
	char buffer[PATH_MAX];
	if (!realpath(path, buffer)) {
		return NULL;
	}
	return strdup(buffer);
#endif
}

typedef struct parsed_instr_t parsed_instr_t;
struct parsed_instr_t {
	const instr_def_t *def;
	char *operands[MAX_OPERANDS];
	size_t operand_count;
	parsed_instr_t *rrpin_inner;
	int rrpin_reg;
	bool rrpin_has_reg;
	int rrpin_loop_reg;
	bool rrpin_has_loop_reg;
};

typedef struct data_value_entry data_value_entry_t;

typedef enum {
	ITEM_INSTR,
	ITEM_DATA_BYTES,
	ITEM_DATA_VALUES
} item_kind_t;

typedef struct {
	item_kind_t kind;
	uint32_t offset;
	uint32_t size_bytes;
	size_t line;
	union {
		parsed_instr_t instr;
		struct {
			uint8_t *bytes;
			size_t count;
		} data;
		struct {
			data_value_entry_t *entries;
			size_t count;
			size_t elem_size;
		} data_values;
	} u;
} program_item_t;

typedef struct {
	program_item_t *data;
	size_t count;
	size_t capacity;
} program_vec_t;

typedef struct {
	uint32_t offset;
	int32_t addend;
	bso_reloc_kind_t kind;
	char *symbol;
} relocation_entry_t;

typedef struct {
	relocation_entry_t *data;
	size_t count;
	size_t capacity;
} relocation_vec_t;

typedef struct {
	const char *name;
	uint32_t value;
	bool defined;
	uint32_t name_offset;
} object_symbol_info_t;

static int find_object_symbol_index(const object_symbol_info_t *symbols, size_t count, const char *name) {
	for (size_t i = 0; i < count; ++i) {
		if (strcmp(symbols[i].name, name) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static char *trim(char *s) {
	if (!s) {
		return s;
	}
	while (*s && isspace((unsigned char)*s)) {
		++s;
	}
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) {
		--end;
	}
	*end = '\0';
	return s;
}

static void ensure_symbol_capacity(symbol_vec_t *vec) {
	if (vec->count >= vec->capacity) {
		size_t new_cap = vec->capacity ? vec->capacity * 2u : 32u;
		symbol_t *new_data = (symbol_t *)realloc(vec->data, new_cap * sizeof(symbol_t));
		if (!new_data) {
			fatal(0, "out of memory while growing symbol table");
		}
		vec->data = new_data;
		vec->capacity = new_cap;
	}
}

static void ensure_program_capacity(program_vec_t *vec) {
	if (vec->count >= vec->capacity) {
		size_t new_cap = vec->capacity ? vec->capacity * 2u : 64u;
		program_item_t *new_data = (program_item_t *)realloc(vec->data, new_cap * sizeof(program_item_t));
		if (!new_data) {
			fatal(0, "out of memory while growing program");
		}
		vec->data = new_data;
		vec->capacity = new_cap;
	}
}

static void ensure_relocation_capacity(relocation_vec_t *vec) {
	if (vec->count >= vec->capacity) {
		size_t new_cap = vec->capacity ? vec->capacity * 2u : 32u;
		relocation_entry_t *new_data = (relocation_entry_t *)realloc(vec->data, new_cap * sizeof(relocation_entry_t));
		if (!new_data) {
			fatal(0, "out of memory while growing relocation table");
		}
		vec->data = new_data;
		vec->capacity = new_cap;
	}
}

static void add_relocation(relocation_vec_t *vec, uint32_t offset, bso_reloc_kind_t kind, int32_t addend, const char *symbol) {
	if (!vec) {
		return;
	}
	ensure_relocation_capacity(vec);
	vec->data[vec->count].offset = offset;
	vec->data[vec->count].addend = addend;
	vec->data[vec->count].kind = kind;
	vec->data[vec->count].symbol = strdup(symbol);
	if (!vec->data[vec->count].symbol) {
		fatal(0, "out of memory while copying relocation symbol name");
	}
	vec->count += 1;
}

static void byte_buf_append(byte_buf_t *buf, uint8_t value) {
	if (buf->count >= buf->capacity) {
		size_t new_cap = buf->capacity ? buf->capacity * 2u : 256u;
		uint8_t *new_data = (uint8_t *)realloc(buf->data, new_cap);
		if (!new_data) {
			fatal(0, "out of memory while growing output buffer");
		}
		buf->data = new_data;
		buf->capacity = new_cap;
	}
	buf->data[buf->count++] = value;
}

static void byte_buf_append16(byte_buf_t *buf, uint16_t value) {
	byte_buf_append(buf, (uint8_t)(value & 0xFFu));
	byte_buf_append(buf, (uint8_t)((value >> 8) & 0xFFu));
}

static void byte_buf_append32(byte_buf_t *buf, uint32_t value) {
	byte_buf_append(buf, (uint8_t)(value & 0xFFu));
	byte_buf_append(buf, (uint8_t)((value >> 8) & 0xFFu));
	byte_buf_append(buf, (uint8_t)((value >> 16) & 0xFFu));
	byte_buf_append(buf, (uint8_t)((value >> 24) & 0xFFu));
}

static const instr_def_t *lookup_instruction(const char *mnemonic) {
	char upper[32];
	size_t len = strlen(mnemonic);
	if (len >= sizeof(upper)) {
		return NULL;
	}
	for (size_t i = 0; i < len; ++i) {
		upper[i] = (char)toupper((unsigned char)mnemonic[i]);
	}
	upper[len] = '\0';
	for (size_t i = 0; i < sizeof(g_instructions) / sizeof(g_instructions[0]); ++i) {
		if (strcmp(upper, g_instructions[i].mnemonic) == 0) {
			return &g_instructions[i];
		}
	}
	return NULL;
}

static void add_symbol(symbol_vec_t *symbols, const char *name, uint32_t address, size_t line) {
	for (size_t i = 0; i < symbols->count; ++i) {
		if (strcmp(symbols->data[i].name, name) == 0) {
			fatal(line, "duplicate label '%s'", name);
		}
	}
	ensure_symbol_capacity(symbols);
	symbols->data[symbols->count].name = strdup(name);
	if (!symbols->data[symbols->count].name) {
		fatal(line, "out of memory copying label");
	}
	symbols->data[symbols->count].address = address;
	symbols->data[symbols->count].line = line;
	symbols->count += 1;
}

static const symbol_t *find_symbol(const symbol_vec_t *symbols, const char *name) {
	for (size_t i = 0; i < symbols->count; ++i) {
		if (strcmp(symbols->data[i].name, name) == 0) {
			return &symbols->data[i];
		}
	}
	return NULL;
}

static uint32_t absolute_symbol_address(const symbol_t *sym) {
	return sym->address + g_origin_base;
}

static uint32_t symbol_value_for_mode(const symbol_t *sym, bool object_mode) {
	return object_mode ? sym->address : absolute_symbol_address(sym);
}

static int parse_register(const char *text, size_t line) {
	if (!text || !*text) {
		fatal(line, "expected register, got empty token");
	}
	if (text[0] != 'B' && text[0] != 'b') {
		fatal(line, "expected register, got '%s'", text);
	}
	char *end = NULL;
	long value = strtol(text + 1, &end, 10);
	if (!end || *end != '\0') {
		fatal(line, "invalid register '%s'", text);
	}
	if (value < 0 || value > 15) {
		fatal(line, "register index out of range in '%s'", text);
	}
	return (int)value;
}

typedef struct {
	bool is_label;
	char *label;
	int64_t value;
} value_t;

struct data_value_entry {
	value_t value;
	size_t line;
};

typedef struct {
	data_value_entry_t *data;
	size_t count;
	size_t capacity;
	size_t elem_size;
} data_value_vec_t;

static void free_value_contents(value_t *val) {
	if (val && val->is_label && val->label) {
		free(val->label);
		val->label = NULL;
		val->is_label = false;
	}
}

static void data_value_vec_append(data_value_vec_t *vec, value_t val, size_t line) {
	if (vec->count == vec->capacity) {
		size_t new_capacity = vec->capacity ? vec->capacity * 2 : 8;
		data_value_entry_t *new_data = realloc(vec->data, new_capacity * sizeof(*new_data));
		if (!new_data) {
			fatal(line, "out of memory while parsing data directive");
		}
		vec->data = new_data;
		vec->capacity = new_capacity;
	}
	vec->data[vec->count].value = val;
	vec->data[vec->count].line = line;
	vec->count += 1;
}

static long long evaluate_const_expression(const char *text, size_t line);

static value_t parse_value(const char *text, size_t line) {
	value_t v = {0};
	if (!text) {
		fatal(line, "missing immediate value");
	}
	char *copy = strdup(text);
	if (!copy) {
		fatal(line, "out of memory while parsing immediate");
	}
	char *s = trim(copy);
	if (*s == '#') {
		s = trim(s + 1);
	}
	if (*s == '\0') {
		free(copy);
		fatal(line, "empty immediate value");
	}
	if (isalpha((unsigned char)*s) || *s == '_') {
		v.is_label = true;
		v.label = strdup(s);
		if (!v.label) {
			free(copy);
			fatal(line, "out of memory while copying label");
		}
	} else {
		long long temp = evaluate_const_expression(s, line);
		v.value = temp;
	}
	free(copy);
	return v;
}

typedef struct {
	int reg;
	int32_t offset;
} mem_operand_t;

static mem_operand_t parse_mem_operand(const char *text, size_t line) {
	if (!text) {
		fatal(line, "missing memory operand");
	}
	char *copy = strdup(text);
	if (!copy) {
		fatal(line, "out of memory while parsing memory operand");
	}
	char *s = trim(copy);
	if (*s != '[') {
		free(copy);
		fatal(line, "expected '[' to start memory operand '%s'", text);
	}
	char *close = strrchr(s, ']');
	if (!close) {
		free(copy);
		fatal(line, "expected ']' in memory operand '%s'", text);
	}
	*close = '\0';
	char *inner = trim(s + 1);
	if (*inner == '\0') {
		free(copy);
		fatal(line, "empty memory operand");
	}
	char *offset_part = NULL;
	int sign = +1;
	for (char *p = inner; *p; ++p) {
		if (*p == '+' || *p == '-') {
			sign = (*p == '+') ? +1 : -1;
			offset_part = p + 1;
			*p = '\0';
			break;
		}
	}
	char *base_str = trim(inner);
	int reg = parse_register(base_str, line);
	int32_t offset = 0;
	if (offset_part) {
		char *off = trim(offset_part);
		value_t val = parse_value(off, line);
		if (val.is_label) {
			free(val.label);
			free(copy);
			fatal(line, "labels not supported as memory offsets");
		}
		long long total = sign * val.value;
		if (total < -128 || total > 127) {
			free(copy);
			fatal(line, "memory offset %lld out of 8-bit range", total);
		}
		offset = (int32_t)total;
	}
	free(copy);
	mem_operand_t mem = { reg, offset };
	return mem;
}

static int parse_ctl_index(const char *text, size_t line) {
	if (!text) {
		fatal(line, "missing control register operand");
	}
	char *copy = strdup(text);
	if (!copy) {
		fatal(line, "out of memory while parsing control register");
	}
	char *s = trim(copy);
	size_t len = strlen(s);
	for (size_t i = 0; i < len; ++i) {
		s[i] = (char)toupper((unsigned char)s[i]);
	}
	if (strncmp(s, "CTL", 3) != 0) {
		free(copy);
		fatal(line, "expected CTL[n], got '%s'", text);
	}
	char *p = s + 3;
	while (*p == ' ') {
		++p;
	}
	bool bracketed = false;
	if (*p == '[') {
		bracketed = true;
		++p;
		while (*p == ' ') {
			++p;
		}
	}
	char *digits = p;
	errno = 0;
	char *end = NULL;
	long value = strtol(digits, &end, 0);
	if (errno != 0 || digits == end) {
		free(copy);
		fatal(line, "invalid control register index '%s'", text);
	}
	while (*end == ' ') {
		++end;
	}
	if (bracketed) {
		if (*end != ']') {
			free(copy);
			fatal(line, "expected closing ']' in '%s'", text);
		}
		++end;
		while (*end == ' ') {
			++end;
		}
	}
	if (*end != '\0') {
		free(copy);
		fatal(line, "unexpected trailing characters in '%s'", text);
	}
	if (value < 0 || value > 7) {
		free(copy);
		fatal(line, "control register index out of range in '%s'", text);
	}
	free(copy);
	return (int)value;
}

typedef struct {
	const char *p;
	size_t line;
} expr_parser_t;

static void expr_skip_ws(expr_parser_t *ctx) {
	while (isspace((unsigned char)*ctx->p)) {
		ctx->p++;
	}
}

static long long parse_expr_add(expr_parser_t *ctx);

static long long parse_expr_number(expr_parser_t *ctx) {
	expr_skip_ws(ctx);
	const char *start = ctx->p;
	if (*start == '\0') {
		fatal(ctx->line, "unexpected end of expression");
	}
	errno = 0;
	char *end = NULL;
	long long value = strtoll(start, &end, 0);
	if (errno != 0 || end == start) {
		fatal(ctx->line, "invalid numeric literal in expression");
	}
	ctx->p = end;
	return value;
}

static long long parse_expr_unary(expr_parser_t *ctx) {
	expr_skip_ws(ctx);
	char c = *ctx->p;
	if (c == '+' || c == '-' || c == '~') {
		ctx->p++;
		long long value = parse_expr_unary(ctx);
		if (c == '+') {
			return value;
		} else if (c == '-') {
			return -value;
		}
		return ~value;
	}
	if (c == '(') {
		ctx->p++;
		long long inner = parse_expr_add(ctx);
		expr_skip_ws(ctx);
		if (*ctx->p != ')') {
			fatal(ctx->line, "expected ')' in expression");
		}
		ctx->p++;
		return inner;
	}
	return parse_expr_number(ctx);
}

static long long parse_expr_mul(expr_parser_t *ctx) {
	long long value = parse_expr_unary(ctx);
	while (true) {
		expr_skip_ws(ctx);
		char op = *ctx->p;
		if (op != '*' && op != '/' && op != '%') {
			break;
		}
		ctx->p++;
		long long rhs = parse_expr_unary(ctx);
		if (op == '*') {
			value *= rhs;
		} else if (op == '/') {
			if (rhs == 0) {
				fatal(ctx->line, "division by zero in expression");
			}
			value /= rhs;
		} else {
			if (rhs == 0) {
				fatal(ctx->line, "modulo by zero in expression");
			}
			value %= rhs;
		}
	}
	return value;
}

static long long parse_expr_add(expr_parser_t *ctx) {
	long long value = parse_expr_mul(ctx);
	while (true) {
		expr_skip_ws(ctx);
		char op = *ctx->p;
		if (op != '+' && op != '-') {
			break;
		}
		ctx->p++;
		long long rhs = parse_expr_mul(ctx);
		if (op == '+') {
			value += rhs;
		} else {
			value -= rhs;
		}
	}
	return value;
}

static long long evaluate_const_expression(const char *text, size_t line) {
	expr_parser_t ctx = { .p = text, .line = line };
	long long value = parse_expr_add(&ctx);
	expr_skip_ws(&ctx);
	if (*ctx.p != '\0') {
		fatal(line, "unexpected trailing characters in expression");
	}
	return value;
}

static int parse_hex_digit(int c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	c = tolower(c);
	if (c >= 'a' && c <= 'f') {
		return 10 + (c - 'a');
	}
	return -1;
}

static const char *parse_string_literal_bytes(const char *text, size_t line, byte_buf_t *out) {
	const char *p = text;
	if (*p != '"') {
		fatal(line, "expected string literal");
	}
	++p;
	while (true) {
		char c = *p;
		if (c == '\0') {
			fatal(line, "unterminated string literal");
		}
		if (c == '"') {
			++p;
			break;
		}
		if (c == '\\') {
			++p;
			char esc = *p;
			if (esc == '\0') {
				fatal(line, "unterminated escape sequence");
			}
			if (esc == 'x' || esc == 'X') {
				++p;
				int digits = 0;
				int value = 0;
				while (digits < 2) {
					int hv = parse_hex_digit(*p);
					if (hv < 0) {
						break;
					}
					value = (value << 4) | hv;
					++p;
					digits += 1;
				}
				if (digits == 0) {
					fatal(line, "\\x escape requires at least one hex digit");
				}
				byte_buf_append(out, (uint8_t)value);
				continue;
			}
			int value = 0;
			switch (esc) {
				case '\\':
				case '"':
				case '\'':
					value = esc;
					break;
				case 'n':
					value = '\n';
					break;
				case 'r':
					value = '\r';
					break;
				case 't':
					value = '\t';
					break;
				case '0':
					value = 0;
					break;
				case 'b':
					value = '\b';
					break;
				case 'f':
					value = '\f';
					break;
				case 'v':
					value = '\v';
					break;
				default:
					value = esc;
					break;
			}
			++p;
			byte_buf_append(out, (uint8_t)value);
			continue;
		}
		byte_buf_append(out, (uint8_t)c);
		++p;
	}
	return p;
}

static void parse_string_directive(char *text, size_t line, byte_buf_t *output) {
	char *p = text;
	while (*p) {
		while (isspace((unsigned char)*p)) {
			++p;
		}
		if (*p == '\0') {
			break;
		}
		if (*p == ',') {
			++p;
			continue;
		}
		if (*p == '"') {
			const char *next = parse_string_literal_bytes(p, line, output);
			p = (char *)next;
			continue;
		}
		char *start = p;
		while (*p && *p != ',') {
			++p;
		}
		char saved = *p;
		*p = '\0';
		char *token = trim(start);
		if (*token != '\0') {
			value_t val = parse_value(token, line);
			if (val.is_label) {
				free(val.label);
				fatal(line, "labels not allowed in .string directives");
			}
			if (val.value < 0 || val.value > 255) {
				fatal(line, "byte value %lld out of range", val.value);
			}
			byte_buf_append(output, (uint8_t)(val.value & 0xFFu));
		}
		*p = saved;
		if (saved == ',') {
			++p;
		}
	}
}

static void maybe_convert_string_operand(parsed_instr_t *instr, const instr_def_t *def, size_t line, string_lit_vec_t *strings) {
	if (!strings || !instr || !def) {
		return;
	}
	if (def->opcode != 0x16u) { /* MOVI32 only */
		return;
	}
	if (instr->operand_count != 2) {
		return;
	}
	char *operand = trim(instr->operands[1]);
	if (*operand != '"') {
		return;
	}
	byte_buf_t data = {0};
	const char *next = parse_string_literal_bytes(operand, line, &data);
	char *rest = trim((char *)next);
	if (*rest != '\0') {
		free(data.data);
		fatal(line, "unexpected tokens after string literal");
	}
	byte_buf_append(&data, 0u); /* implicit .stringz */

	char label_buf[32];
	snprintf(label_buf, sizeof(label_buf), "__str_%zu", strings->count);
	ensure_string_lit_capacity(strings);
	string_lit_t *entry = &strings->data[strings->count++];
	entry->label = strdup(label_buf);
	entry->bytes = data.data;
	entry->count = data.count;
	entry->line = line;
	if (!entry->label) {
		fatal(line, "out of memory while storing string literal label");
	}

	free(instr->operands[1]);
	instr->operands[1] = strdup(label_buf);
	if (!instr->operands[1]) {
		fatal(line, "out of memory while rewriting string literal operand");
	}
}

static void parse_data_values(char *text, size_t line, bool allow_strings, data_value_vec_t *out_vec) {
	char *p = text;
	while (*p) {
		while (isspace((unsigned char)*p)) {
			++p;
		}
		if (*p == '\0') {
			break;
		}
		if (*p == ',') {
			++p;
			continue;
		}
		if (*p == '"') {
			if (!allow_strings) {
				fatal(line, "string literals are only allowed in .byte directives");
			}
			byte_buf_t tmp = {0};
			const char *next = parse_string_literal_bytes(p, line, &tmp);
			for (size_t i = 0; i < tmp.count; ++i) {
				value_t val = { .is_label = false, .label = NULL, .value = (int64_t)tmp.data[i] };
				data_value_vec_append(out_vec, val, line);
			}
			free(tmp.data);
			p = (char *)next;
			continue;
		}
		char *start = p;
		while (*p && *p != ',') {
			++p;
		}
		char saved = *p;
		*p = '\0';
		char *token = trim(start);
		if (*token != '\0') {
			value_t val = parse_value(token, line);
			data_value_vec_append(out_vec, val, line);
		}
		*p = saved;
		if (saved == ',') {
			++p;
		}
	}
	if (out_vec->count == 0) {
		fatal(line, "data directive requires at least one value");
	}
}

static size_t split_operands(char *text, char **out_operands, size_t max_operands) {
	size_t count = 0;
	char *start = text;
	int depth = 0;
	bool in_string = false;
	bool escape = false;
	for (char *p = text; ; ++p) {
		char c = *p;
		if (in_string) {
			if (escape) {
				escape = false;
			} else if (c == '\\') {
				escape = true;
			} else if (c == '"') {
				in_string = false;
			}
			if (c == '\0') {
				break;
			}
			continue;
		}
		if (c == '"') {
			in_string = true;
			continue;
		}
		if (c == '[') {
			depth += 1;
		} else if (c == ']') {
			if (depth > 0) {
				depth -= 1;
			}
		}
		if ((c == ',' && depth == 0) || c == '\0' || (c == ';' && depth == 0)) {
			if (c == ',' || c == ';') {
				*p = '\0';
			}
			char *segment = trim(start);
			if (*segment) {
				if (count >= max_operands) {
					fatal(0, "too many operands (max %zu)", max_operands);
				}
				out_operands[count] = strdup(segment);
				if (!out_operands[count]) {
					fatal(0, "out of memory copying operand");
				}
				count += 1;
			}
			if (c == '\0' || c == ';') {
				break;
			}
			start = p + 1;
		}
		if (c == '\0') {
			break;
		}
	}
	return count;
}

static char *find_top_level_comma(char *text) {
	bool in_string = false;
	bool escape = false;
	int bracket_depth = 0;
	for (char *p = text; *p; ++p) {
		char c = *p;
		if (in_string) {
			if (escape) {
				escape = false;
				continue;
			}
			if (c == '\\') {
				escape = true;
				continue;
			}
			if (c == '"') {
				in_string = false;
			}
			continue;
		}
		if (c == '"') {
			in_string = true;
			continue;
		}
		if (c == '[') {
			bracket_depth += 1;
			continue;
		}
		if (c == ']') {
			if (bracket_depth > 0) {
				bracket_depth -= 1;
			}
			continue;
		}
		if (c == ',' && bracket_depth == 0) {
			return p;
		}
	}
	return NULL;
}

static parsed_instr_t *parse_inline_instruction(const char *text, size_t line) {
	char *copy = strdup(text);
	if (!copy) {
		fatal(line, "out of memory parsing inline instruction");
	}
	char *cursor = trim(copy);
	if (*cursor == '\0') {
		free(copy);
		fatal(line, "empty instruction in RRPIN payload");
	}
	char mnemonic[64];
	size_t idx = 0;
	while (cursor[idx] && !isspace((unsigned char)cursor[idx])) {
		if (idx + 1 >= sizeof(mnemonic)) {
			free(copy);
			fatal(line, "mnemonic too long in RRPIN payload");
		}
		mnemonic[idx] = cursor[idx];
		idx += 1;
	}
	mnemonic[idx] = '\0';
	cursor = trim(cursor + idx);
	const instr_def_t *def = lookup_instruction(mnemonic);
	if (!def) {
		free(copy);
		fatal(line, "unknown instruction '%s' in RRPIN payload", mnemonic);
	}
	parsed_instr_t *instr = (parsed_instr_t *)calloc(1, sizeof(parsed_instr_t));
	if (!instr) {
		free(copy);
		fatal(line, "out of memory allocating RRPIN payload instruction");
	}
	instr->def = def;
	if (*cursor != '\0') {
		instr->operand_count = split_operands(cursor, instr->operands, MAX_OPERANDS);
	}
	free(copy);
	return instr;
}

static bool rrpin_instruction_allowed(const instr_def_t *def) {
	switch (def->opcode) {
		case 0x00: /* ADC */
		case 0x01: /* ADCI32 */
		case 0x02: /* ADCI16 */
		case 0x03: /* ADCI8 */
		case 0x04: /* ADD */
		case 0x05: /* ADDI32 */
		case 0x06: /* ADDI16 */
		case 0x07: /* ADDI8 */
		case 0x08: /* AND */
		case 0x09: /* ANDI32 */
		case 0x0A: /* ANDI16 */
		case 0x0B: /* ANDI8 */
		case 0x0C: /* CMP */
		case 0x0D: /* CMPI32 */
		case 0x0E: /* SUB */
		case 0x0F: /* OR */
		case 0x10: /* XOR */
		case 0x12: /* MUL */
		case 0x13: /* DIV */
		case 0x14: /* UDIV */
		case 0x15: /* MOV */
		case 0x16: /* MOVI32 */
		case 0x17: /* ORI32 */
		case 0x18: /* ORI16 */
		case 0x19: /* ORI8 */
		case 0x1A: /* XORI32 */
		case 0x1B: /* XORI16 */
		case 0x1C: /* XORI8 */
		case 0x1D: /* SUBI32 */
		case 0x1E: /* SUBI16 */
		case 0x1F: /* SUBI8 */
		case 0x46: /* SHL */
		case 0x47: /* SHLI8 */
		case 0x48: /* LSR */
		case 0x49: /* LSRI8 */
		case 0x4A: /* ASR */
		case 0x4B: /* ASRI8 */
		case 0xA2: /* LD */
		case 0xA3: /* ST */
		case 0xA5: /* LDBU */
		case 0xA6: /* LDBS */
		case 0xA7: /* LDHU */
		case 0xA8: /* LDHS */
		case 0xA9: /* STB */
		case 0xAA: /* STH */
		case 0xAB: /* LDRIP */
			return true;
		default:
			return false;
	}
}

static void free_operands(parsed_instr_t *instr) {
	for (size_t i = 0; i < instr->operand_count; ++i) {
		free(instr->operands[i]);
		instr->operands[i] = NULL;
	}
	instr->operand_count = 0;
	if (instr->rrpin_inner) {
		free_operands(instr->rrpin_inner);
		free(instr->rrpin_inner);
		instr->rrpin_inner = NULL;
	}
	instr->rrpin_has_reg = false;
	instr->rrpin_reg = 0;
	instr->rrpin_has_loop_reg = false;
	instr->rrpin_loop_reg = 0;
}

static void free_program(program_vec_t *program) {
	for (size_t i = 0; i < program->count; ++i) {
		program_item_t *item = &program->data[i];
		if (item->kind == ITEM_INSTR) {
			free_operands(&item->u.instr);
		} else if (item->kind == ITEM_DATA_BYTES) {
			free(item->u.data.bytes);
		} else if (item->kind == ITEM_DATA_VALUES) {
			for (size_t j = 0; j < item->u.data_values.count; ++j) {
				free_value_contents(&item->u.data_values.entries[j].value);
			}
			free(item->u.data_values.entries);
		}
	}
	free(program->data);
	program->data = NULL;
	program->count = 0;
	program->capacity = 0;
}

static void free_symbols(symbol_vec_t *symbols) {
	for (size_t i = 0; i < symbols->count; ++i) {
		free(symbols->data[i].name);
	}
	free(symbols->data);
	symbols->data = NULL;
	symbols->count = 0;
	symbols->capacity = 0;
}

static void free_relocations(relocation_vec_t *relocs) {
	for (size_t i = 0; i < relocs->count; ++i) {
		free(relocs->data[i].symbol);
	}
	free(relocs->data);
	relocs->data = NULL;
	relocs->count = 0;
	relocs->capacity = 0;
}

static void free_string_literals(string_lit_vec_t *vec) {
	for (size_t i = 0; i < vec->count; ++i) {
		free(vec->data[i].label);
		if (vec->data[i].bytes) {
			free(vec->data[i].bytes);
		}
	}
	free(vec->data);
	vec->data = NULL;
	vec->count = 0;
	vec->capacity = 0;
}

static uint64_t max_unsigned_for_size(size_t elem_size) {
	if (elem_size >= 8) {
		return UINT64_MAX;
	}
	return (1ULL << (elem_size * 8)) - 1ULL;
}

static int64_t min_signed_for_size(size_t elem_size) {
	if (elem_size >= 8) {
		return INT64_MIN;
	}
	return -((int64_t)1 << ((elem_size * 8) - 1));
}

static void append_little_endian(byte_buf_t *output, uint64_t value, size_t elem_size) {
	for (size_t i = 0; i < elem_size; ++i) {
		byte_buf_append(output, (uint8_t)(value & 0xFFu));
		value >>= 8;
	}
}

static uint8_t encode_bytesize2(const value_t *val, size_t line, const char *mnemonic) {
	if (val->is_label) {
		if (val->label) {
			free(val->label);
		}
		fatal(line, "labels not allowed for byte-size operand in '%s'", mnemonic);
	}
	long long bytes = val->value;
	switch (bytes) {
		case 1:
			return 0u;
		case 2:
			return 1u;
		case 4:
			return 2u;
		default:
			fatal(line, "invalid byte-size %lld in '%s' (must be 1, 2, or 4)", bytes, mnemonic);
	}
	return 0u;
}

static void encode_data_values(const program_item_t *item, const symbol_vec_t *symbols, byte_buf_t *output, relocation_vec_t *relocs, bool object_mode) {
	const size_t elem_size = item->u.data_values.elem_size;
	const uint64_t max_unsigned = max_unsigned_for_size(elem_size);
	const int64_t min_signed = min_signed_for_size(elem_size);
	for (size_t i = 0; i < item->u.data_values.count; ++i) {
		const data_value_entry_t *entry = &item->u.data_values.entries[i];
		uint64_t resolved = 0;
		const symbol_t *sym = NULL;
		if (entry->value.is_label) {
			sym = find_symbol(symbols, entry->value.label);
			if (!sym && !object_mode) {
				fatal(entry->line, "undefined label '%s' in data directive", entry->value.label);
			}
			if (sym) {
				resolved = symbol_value_for_mode(sym, object_mode);
			} else {
				resolved = 0;
			}
			if (!object_mode && resolved > max_unsigned) {
				fatal(entry->line, "label '%s' value 0x%llx out of range for %zu-byte directive", entry->value.label, (unsigned long long)resolved, elem_size);
			}
		} else {
			int64_t signed_value = entry->value.value;
			if (signed_value < min_signed) {
				fatal(entry->line, "value %lld out of range for %zu-byte directive", signed_value, elem_size);
			}
			if (signed_value >= 0 && (uint64_t)signed_value > max_unsigned) {
				fatal(entry->line, "value %lld out of range for %zu-byte directive", signed_value, elem_size);
			}
			resolved = (uint64_t)signed_value;
		}
		uint32_t reloc_offset = output->count;
		append_little_endian(output, resolved, elem_size);
		if (object_mode && entry->value.is_label) {
			bso_reloc_kind_t kind = BSO_RELOC_ABS32;
			switch (elem_size) {
				case 1:
					kind = BSO_RELOC_ABS8;
					break;
				case 2:
					kind = BSO_RELOC_ABS16;
					break;
				case 4:
					kind = BSO_RELOC_ABS32;
					break;
				case 8:
					kind = BSO_RELOC_ABS64;
					break;
				default:
					fatal(entry->line, "unsupported data relocation size %zu", elem_size);
			}
			const char *target_name = sym ? sym->name : entry->value.label;
			add_relocation(relocs, reloc_offset, kind, 0, target_name);
		}
	}
}

static void encode_instruction(const program_item_t *item, const symbol_vec_t *symbols, byte_buf_t *output, relocation_vec_t *relocs, bool object_mode) {
	const parsed_instr_t *instr = &item->u.instr;
	const instr_def_t *def = instr->def;
	size_t line = item->line;
	byte_buf_append(output, def->opcode);
	uint32_t next_ip = item->offset + item->size_bytes;

	switch (def->format) {
		case FMT_NONE:
			if (instr->operand_count != 0) {
				fatal(line, "instruction '%s' takes no operands", def->mnemonic);
			}
			break;

		case FMT_REG_REG: {
			if (instr->operand_count != 2) {
				fatal(line, "instruction '%s' expects two registers", def->mnemonic);
			}
			int rd = parse_register(trim(instr->operands[0]), line);
			int rs = parse_register(trim(instr->operands[1]), line);
			byte_buf_append(output, (uint8_t)((rd << 4) | (rs & 0x0F)));
			break;
		}

		case FMT_REG_CTL: {
			if (instr->operand_count != 2) {
				fatal(line, "instruction '%s' expects register and CTL[n]", def->mnemonic);
			}
			int rd = parse_register(trim(instr->operands[0]), line);
			int ctl = parse_ctl_index(trim(instr->operands[1]), line);
			byte_buf_append(output, (uint8_t)(((rd & 0x0F) << 4) | (ctl & 0x07)));
			break;
		}

		case FMT_CTL_REG: {
			if (instr->operand_count != 2) {
				fatal(line, "instruction '%s' expects CTL[n] and register", def->mnemonic);
			}
			int ctl = parse_ctl_index(trim(instr->operands[0]), line);
			int rs = parse_register(trim(instr->operands[1]), line);
			byte_buf_append(output, (uint8_t)(((rs & 0x0F) << 4) | (ctl & 0x07)));
			break;
		}

		case FMT_REG_REG_REG: {
			if (instr->operand_count != 3) {
				fatal(line, "instruction '%s' expects three registers", def->mnemonic);
			}
			int rd = parse_register(trim(instr->operands[0]), line);
			int ra = parse_register(trim(instr->operands[1]), line);
			int rb = parse_register(trim(instr->operands[2]), line);
			byte_buf_append(output, (uint8_t)(((rd & 0x0F) << 4) | (ra & 0x0F)));
			byte_buf_append(output, (uint8_t)((rb & 0x0F) << 4));
			break;
		}

		case FMT_REG_IMM32: {
			if (instr->operand_count != 2) {
				fatal(line, "instruction '%s' expects register and imm32", def->mnemonic);
			}
			int rd = parse_register(trim(instr->operands[0]), line);
			value_t val = parse_value(instr->operands[1], line);
			uint32_t imm = 0;
			bool needs_reloc = false;
			const char *target_name = NULL;
			if (val.is_label) {
				const symbol_t *sym = find_symbol(symbols, val.label);
				if (!sym && !object_mode) {
					fatal(line, "undefined label '%s'", val.label);
				}
				if (sym) {
					imm = symbol_value_for_mode(sym, object_mode);
					target_name = sym->name;
				} else {
					imm = 0;
					target_name = val.label;
				}
				needs_reloc = object_mode;
			} else {
				if (val.value < INT32_MIN || val.value > UINT32_MAX) {
					fatal(line, "immediate %lld out of 32-bit range", val.value);
				}
				imm = (uint32_t)val.value;
			}
			byte_buf_append(output, (uint8_t)((rd & 0x0F) << 4));
			uint32_t imm_offset = output->count;
			byte_buf_append32(output, imm);
			if (needs_reloc) {
				add_relocation(relocs, imm_offset, BSO_RELOC_ABS32, 0, target_name);
				free(val.label);
			} else if (val.is_label) {
				free(val.label);
			}
			break;
		}

		case FMT_REG_IMM16: {
			if (instr->operand_count != 2) {
				fatal(line, "instruction '%s' expects register and imm16", def->mnemonic);
			}
			int rd = parse_register(trim(instr->operands[0]), line);
			value_t val = parse_value(instr->operands[1], line);
			if (val.is_label) {
				free(val.label);
				fatal(line, "labels not allowed for 16-bit immediates in '%s'", def->mnemonic);
			}
			if (val.value < -32768 || val.value > 32767) {
				fatal(line, "immediate %lld out of 16-bit signed range", val.value);
			}
			byte_buf_append(output, (uint8_t)((rd & 0x0F) << 4));
			byte_buf_append16(output, (uint16_t)((int16_t)val.value));
			break;
		}

		case FMT_REG_IMM8: {
			if (instr->operand_count != 2) {
				fatal(line, "instruction '%s' expects register and imm8", def->mnemonic);
			}
			int rd = parse_register(trim(instr->operands[0]), line);
			value_t val = parse_value(instr->operands[1], line);
			if (val.is_label) {
				free(val.label);
				fatal(line, "labels not allowed for 8-bit immediates in '%s'", def->mnemonic);
			}
			if (val.value < -128 || val.value > 127) {
				fatal(line, "immediate %lld out of 8-bit signed range", val.value);
			}
			byte_buf_append(output, (uint8_t)((rd & 0x0F) << 4));
			byte_buf_append(output, (uint8_t)((int8_t)val.value));
			break;
		}

		case FMT_BRANCH_REL8: {
			if (instr->operand_count != 1) {
				fatal(line, "instruction '%s' expects one operand", def->mnemonic);
			}
			value_t val = parse_value(instr->operands[0], line);
			int64_t displacement = 0;
			bool needs_reloc = false;
			const char *target_name = NULL;
			if (val.is_label) {
				const symbol_t *sym = find_symbol(symbols, val.label);
				if (!sym) {
					if (!object_mode) {
						fatal(line, "undefined label '%s'", val.label);
					}
					needs_reloc = true;
					target_name = val.label;
					displacement = 0;
				} else {
					displacement = (int64_t)(int32_t)sym->address - (int64_t)(int32_t)next_ip;
					free(val.label);
				}
			} else {
				displacement = val.value;
			}
			if (!needs_reloc && (displacement < -128 || displacement > 127)) {
				fatal(line, "branch displacement %lld out of range", displacement);
			}
			uint32_t disp_offset = output->count;
			byte_buf_append(output, (uint8_t)((int8_t)displacement));
			if (needs_reloc) {
				add_relocation(relocs, disp_offset, BSO_RELOC_REL8, -((int32_t)next_ip), target_name);
				free(val.label);
			}
			break;
		}

		case FMT_BRANCH_REL32: {
			if (instr->operand_count != 1) {
				fatal(line, "instruction '%s' expects one operand", def->mnemonic);
			}
			value_t val = parse_value(instr->operands[0], line);
			int64_t displacement = 0;
			bool needs_reloc = false;
			const char *target_name = NULL;
			if (val.is_label) {
				const symbol_t *sym = find_symbol(symbols, val.label);
				if (!sym) {
					if (!object_mode) {
						fatal(line, "undefined label '%s'", val.label);
					}
					needs_reloc = true;
					target_name = val.label;
					displacement = 0;
				} else {
					displacement = (int64_t)(int32_t)sym->address - (int64_t)(int32_t)next_ip;
					free(val.label);
				}
			} else {
				displacement = val.value;
			}
			if (!needs_reloc && (displacement < INT32_MIN || displacement > INT32_MAX)) {
				fatal(line, "branch displacement %lld out of range", displacement);
			}
			uint32_t disp_offset = output->count;
			byte_buf_append32(output, (uint32_t)((int32_t)displacement));
			if (needs_reloc) {
				add_relocation(relocs, disp_offset, BSO_RELOC_REL32, -((int32_t)next_ip), target_name);
				free(val.label);
			}
			break;
		}

		case FMT_ABS32_ONLY: {
			if (instr->operand_count != 1) {
				fatal(line, "instruction '%s' expects one abs32 operand", def->mnemonic);
			}
			value_t val = parse_value(instr->operands[0], line);
			uint32_t imm = 0;
			bool needs_reloc = false;
			const char *target_name = NULL;
			if (val.is_label) {
				const symbol_t *sym = find_symbol(symbols, val.label);
				if (!sym && !object_mode) {
					fatal(line, "undefined label '%s'", val.label);
				}
				if (sym) {
					imm = symbol_value_for_mode(sym, object_mode);
					target_name = sym->name;
				} else {
					imm = 0;
					target_name = val.label;
				}
				needs_reloc = object_mode;
			} else {
				if (val.value < INT32_MIN || val.value > UINT32_MAX) {
					fatal(line, "absolute target %lld out of 32-bit range", val.value);
				}
				imm = (uint32_t)val.value;
			}
			uint32_t imm_offset = output->count;
			byte_buf_append32(output, imm);
			if (needs_reloc) {
				add_relocation(relocs, imm_offset, BSO_RELOC_ABS32, 0, target_name);
				free(val.label);
			} else if (val.is_label) {
				free(val.label);
			}
			break;
		}

		case FMT_REG_HIGH: {
			if (instr->operand_count != 1) {
				fatal(line, "instruction '%s' expects one register", def->mnemonic);
			}
			int reg = parse_register(trim(instr->operands[0]), line);
			byte_buf_append(output, (uint8_t)((reg & 0x0F) << 4));
			break;
		}

		case FMT_IMM8: {
			if (instr->operand_count != 1) {
				fatal(line, "instruction '%s' expects one imm8", def->mnemonic);
			}
			value_t val = parse_value(instr->operands[0], line);
			if (val.is_label) {
				free(val.label);
				fatal(line, "labels not allowed for 8-bit immediates in '%s'", def->mnemonic);
			}
			if (val.value < 0 || val.value > 255) {
				fatal(line, "immediate %lld out of 8-bit unsigned range", val.value);
			}
			byte_buf_append(output, (uint8_t)val.value);
			break;
		}

		case FMT_IMM16: {
			if (instr->operand_count != 1) {
				fatal(line, "instruction '%s' expects one imm16", def->mnemonic);
			}
			value_t val = parse_value(instr->operands[0], line);
			if (val.is_label) {
				free(val.label);
				fatal(line, "labels not allowed for 16-bit immediates in '%s'", def->mnemonic);
			}
			if (val.value < 0 || val.value > 65535) {
				fatal(line, "immediate %lld out of 16-bit unsigned range", val.value);
			}
			byte_buf_append16(output, (uint16_t)val.value);
			break;
		}

		case FMT_MEM_LOAD: {
			if (instr->operand_count != 2) {
				fatal(line, "instruction '%s' expects reg, [reg+imm]", def->mnemonic);
			}
			int rd = parse_register(trim(instr->operands[0]), line);
			mem_operand_t mem = parse_mem_operand(instr->operands[1], line);
			byte_buf_append(output, (uint8_t)(((rd & 0x0F) << 4) | (mem.reg & 0x0F)));
			byte_buf_append(output, (uint8_t)((int8_t)mem.offset));
			break;
		}

		case FMT_MEM_STORE: {
			if (instr->operand_count != 2) {
				fatal(line, "instruction '%s' expects [reg+imm], reg", def->mnemonic);
			}
			mem_operand_t mem = parse_mem_operand(instr->operands[0], line);
			int rs = parse_register(trim(instr->operands[1]), line);
			byte_buf_append(output, (uint8_t)(((rs & 0x0F) << 4) | (mem.reg & 0x0F)));
			byte_buf_append(output, (uint8_t)((int8_t)mem.offset));
			break;
		}

		case FMT_REG_MEM_IMM8: {
			if (instr->operand_count != 3) {
				fatal(line, "instruction '%s' expects reg, [reg], imm8", def->mnemonic);
			}
			int rd = parse_register(trim(instr->operands[0]), line);
			mem_operand_t mem = parse_mem_operand(instr->operands[1], line);
			if (mem.offset != 0) {
				fatal(line, "instruction '%s' requires zero offset in memory operand", def->mnemonic);
			}
			value_t len = parse_value(instr->operands[2], line);
			if (len.is_label) {
				free(len.label);
				fatal(line, "instruction '%s' requires an immediate length", def->mnemonic);
			}
			if (len.value < 0 || len.value > 255) {
				fatal(line, "length %lld out of 8-bit unsigned range", len.value);
			}
			byte_buf_append(output, (uint8_t)(((rd & 0x0F) << 4) | (mem.reg & 0x0F)));
			byte_buf_append(output, (uint8_t)len.value);
			break;
		}

		case FMT_REG_MEM: {
			if (instr->operand_count != 2) {
				fatal(line, "instruction '%s' expects reg, [reg]", def->mnemonic);
			}
			int rd = parse_register(trim(instr->operands[0]), line);
			mem_operand_t mem = parse_mem_operand(instr->operands[1], line);
			if (mem.offset != 0) {
				fatal(line, "instruction '%s' requires zero offset in memory operand", def->mnemonic);
			}
			byte_buf_append(output, (uint8_t)(((rd & 0x0F) << 4) | (mem.reg & 0x0F)));
			break;
		}

		case FMT_REG_REL32: {
			if (instr->operand_count != 2) {
				fatal(line, "instruction '%s' expects register and relative imm32", def->mnemonic);
			}
			int rd = parse_register(trim(instr->operands[0]), line);
			value_t val = parse_value(instr->operands[1], line);
			int64_t displacement = 0;
			bool needs_reloc = false;
			const char *target_name = NULL;
			if (val.is_label) {
				const symbol_t *sym = find_symbol(symbols, val.label);
				if (!sym) {
					if (!object_mode) {
						fatal(line, "undefined label '%s'", val.label);
					}
					needs_reloc = true;
					target_name = val.label;
					displacement = 0;
				} else {
					displacement = (int64_t)(int32_t)sym->address - (int64_t)(int32_t)next_ip;
					free(val.label);
				}
			} else {
				displacement = val.value;
			}
			if (!needs_reloc && (displacement < INT32_MIN || displacement > INT32_MAX)) {
				fatal(line, "relative displacement %lld out of range", displacement);
			}
			byte_buf_append(output, (uint8_t)((rd & 0x0F) << 4));
			uint32_t disp_offset = output->count;
			byte_buf_append32(output, (uint32_t)((int32_t)displacement));
			if (needs_reloc) {
				add_relocation(relocs, disp_offset, BSO_RELOC_REL32, -((int32_t)next_ip), target_name);
				free(val.label);
			}
			break;
		}

		case FMT_EXT: {
			const char *name = def->mnemonic;
			bool is_ext = (strcmp(name, "EXT") == 0);
			uint8_t subop = 0;
			int rd = 0;
			int rs = 0;
			if (is_ext) {
				if (instr->operand_count != 3) {
					fatal(line, "instruction '%s' expects rd, rs, imm8", def->mnemonic);
				}
				rd = parse_register(trim(instr->operands[0]), line);
				rs = parse_register(trim(instr->operands[1]), line);
				value_t val = parse_value(instr->operands[2], line);
				if (val.is_label) {
					free(val.label);
					fatal(line, "labels not allowed for EXT subopcode");
				}
				if (val.value < 0 || val.value > 255) {
					fatal(line, "EXT subopcode %lld out of 8-bit unsigned range", val.value);
				}
				subop = (uint8_t)val.value;
			} else if (strcmp(name, "MTCC") == 0) {
				if (instr->operand_count != 1) {
					fatal(line, "MTCC expects rd");
				}
				rd = parse_register(trim(instr->operands[0]), line);
				rs = 0;
				subop = 0x00;
			} else if (strcmp(name, "MTID") == 0) {
				if (instr->operand_count != 1) {
					fatal(line, "MTID expects rd");
				}
				rd = parse_register(trim(instr->operands[0]), line);
				rs = 0;
				subop = 0x01;
			} else if (strcmp(name, "MTSTS") == 0) {
				if (instr->operand_count != 2) {
					fatal(line, "MTSTS expects rd, rs (target core)");
				}
				rd = parse_register(trim(instr->operands[0]), line);
				rs = parse_register(trim(instr->operands[1]), line);
				subop = 0x02;
			} else if (strcmp(name, "MTSETIP") == 0) {
				if (instr->operand_count != 2) {
					fatal(line, "MTSETIP expects rd (addr), rs (target core)");
				}
				rd = parse_register(trim(instr->operands[0]), line);
				rs = parse_register(trim(instr->operands[1]), line);
				subop = 0x03;
			} else if (strcmp(name, "MTI") == 0) {
				if (instr->operand_count != 2) {
					fatal(line, "MTI expects rd (vector), rs (target core)");
				}
				rd = parse_register(trim(instr->operands[0]), line);
				rs = parse_register(trim(instr->operands[1]), line);
				subop = 0x04;
			} else {
				fatal(line, "unknown EXT alias '%s'", name);
			}

			byte_buf_append(output, (uint8_t)(((rd & 0x0F) << 4) | (rs & 0x0F)));
			byte_buf_append(output, subop);
			break;
		}

		case FMT_CTL_IDX: {
			if (instr->operand_count != 1) {
				fatal(line, "instruction '%s' expects a control register operand", def->mnemonic);
			}
			int ctl = parse_ctl_index(trim(instr->operands[0]), line);
			byte_buf_append(output, (uint8_t)(ctl & 0x07));
			break;
		}

		case FMT_IMM32_ONLY: {
			if (instr->operand_count != 1) {
				fatal(line, "instruction '%s' expects one imm32", def->mnemonic);
			}
			value_t val = parse_value(instr->operands[0], line);
			uint32_t imm = 0;
			bool needs_reloc = false;
			const char *target_name = NULL;
			if (val.is_label) {
				const symbol_t *sym = find_symbol(symbols, val.label);
				if (!sym && !object_mode) {
					fatal(line, "undefined label '%s'", val.label);
				}
				if (sym) {
					imm = symbol_value_for_mode(sym, object_mode);
					target_name = sym->name;
				} else {
					imm = 0;
					target_name = val.label;
				}
				needs_reloc = object_mode;
			} else {
				if (val.value < INT32_MIN || val.value > UINT32_MAX) {
					fatal(line, "immediate %lld out of 32-bit range", val.value);
				}
				imm = (uint32_t)val.value;
			}
			byte_buf_append(output, 0); // padding byte
			uint32_t imm_offset = output->count;
			byte_buf_append32(output, imm);
			if (needs_reloc) {
				add_relocation(relocs, imm_offset, BSO_RELOC_ABS32, 0, target_name);
				free(val.label);
			} else if (val.is_label) {
				free(val.label);
			}
			break;
		}

		case FMT_RRPIN: {
			if (instr->operand_count != 1) {
				fatal(line, "RRPIN expects a single loop count operand");
			}
			if (!instr->rrpin_inner) {
				fatal(line, "missing RRPIN payload instruction");
			}
			value_t val = parse_value(instr->operands[0], line);
			uint32_t loop_count = 0;
			bool needs_reloc = false;
			const char *target_name = NULL;
			if (val.is_label) {
				const symbol_t *sym = find_symbol(symbols, val.label);
				if (!sym && !object_mode) {
					fatal(line, "undefined label '%s'", val.label);
				}
				if (sym) {
					loop_count = symbol_value_for_mode(sym, object_mode);
					target_name = sym->name;
				} else {
					loop_count = 0;
					target_name = val.label;
				}
				needs_reloc = object_mode;
			} else {
				if (val.value < 0 || val.value > UINT32_MAX) {
					fatal(line, "loop count %lld out of 32-bit unsigned range", val.value);
				}
				loop_count = (uint32_t)val.value;
			}
			uint32_t loop_offset = output->count;
			byte_buf_append32(output, loop_count);
			if (needs_reloc) {
				add_relocation(relocs, loop_offset, BSO_RELOC_ABS32, 0, target_name);
				free(val.label);
			} else if (val.is_label) {
				free(val.label);
			}
			if (item->size_bytes < 5u) {
				fatal(line, "invalid RRPIN size encountered");
			}
			program_item_t inner_item = {0};
			inner_item.kind = ITEM_INSTR;
			inner_item.offset = item->offset + 5u;
			inner_item.size_bytes = item->size_bytes - 5u;
			inner_item.line = item->line;
			inner_item.u.instr = *(instr->rrpin_inner);
			encode_instruction(&inner_item, symbols, output, relocs, object_mode);
			break;
		}

		case FMT_RRPIN_R: {
			if (instr->operand_count != 1) {
				fatal(line, "RRPIN.R expects a single loop count operand");
			}
			if (!instr->rrpin_inner || !instr->rrpin_has_reg) {
				fatal(line, "missing RRPIN.R payload instruction or register");
			}
			value_t val = parse_value(instr->operands[0], line);
			uint32_t loop_count = 0;
			bool needs_reloc = false;
			const char *target_name = NULL;
			if (val.is_label) {
				const symbol_t *sym = find_symbol(symbols, val.label);
				if (!sym && !object_mode) {
					fatal(line, "undefined label '%s'", val.label);
				}
				if (sym) {
					loop_count = symbol_value_for_mode(sym, object_mode);
					target_name = sym->name;
				} else {
					loop_count = 0;
					target_name = val.label;
				}
				needs_reloc = object_mode;
			} else {
				if (val.value < 0 || val.value > UINT32_MAX) {
					fatal(line, "loop count %lld out of 32-bit unsigned range", val.value);
				}
				loop_count = (uint32_t)val.value;
			}
			uint32_t loop_offset = output->count;
			byte_buf_append32(output, loop_count);
			if (needs_reloc) {
				add_relocation(relocs, loop_offset, BSO_RELOC_ABS32, 0, target_name);
				free(val.label);
			} else if (val.is_label) {
				free(val.label);
			}
			if (item->size_bytes < 6u) {
				fatal(line, "invalid RRPIN.R size encountered");
			}
			byte_buf_append(output, (uint8_t)((instr->rrpin_reg & 0x0F) << 4));
			program_item_t inner_item = {0};
			inner_item.kind = ITEM_INSTR;
			inner_item.offset = item->offset + 6u;
			inner_item.size_bytes = item->size_bytes - 6u;
			inner_item.line = item->line;
			inner_item.u.instr = *(instr->rrpin_inner);
			encode_instruction(&inner_item, symbols, output, relocs, object_mode);
			break;
		}

		case FMT_RRPIN_V: {
			if (!instr->rrpin_inner || !instr->rrpin_has_loop_reg) {
				fatal(line, "missing RRPIN.V payload instruction or loop register");
			}
			if (item->size_bytes < 2u) {
				fatal(line, "invalid RRPIN.V size encountered");
			}
			byte_buf_append(output, (uint8_t)((instr->rrpin_loop_reg & 0x0F) << 4));
			program_item_t inner_item = {0};
			inner_item.kind = ITEM_INSTR;
			inner_item.offset = item->offset + 2u;
			inner_item.size_bytes = item->size_bytes - 2u;
			inner_item.line = item->line;
			inner_item.u.instr = *(instr->rrpin_inner);
			encode_instruction(&inner_item, symbols, output, relocs, object_mode);
			break;
		}

		case FMT_PORT_OUT: {
			if (instr->operand_count != 3) {
				fatal(line, "instruction '%s' expects #bytesize2, #port16, rA", def->mnemonic);
			}
			value_t size_val = parse_value(instr->operands[0], line);
			uint8_t size_code = encode_bytesize2(&size_val, line, def->mnemonic);
			value_t port_val = parse_value(instr->operands[1], line);
			if (port_val.is_label) {
				free(port_val.label);
				fatal(line, "labels not allowed for port immediates in '%s'", def->mnemonic);
			}
			if (port_val.value < 0 || port_val.value > 0xFFFF) {
				fatal(line, "port value %lld out of 16-bit range in '%s'", port_val.value, def->mnemonic);
			}
			int reg = parse_register(trim(instr->operands[2]), line);
			byte_buf_append16(output, (uint16_t)port_val.value);
			uint8_t operand = (uint8_t)(((size_code & 0x03u) << 6) | ((reg & 0x0Fu) << 2));
			byte_buf_append(output, operand);
			break;
		}

		case FMT_PORT_IN: {
			if (instr->operand_count != 3) {
				fatal(line, "instruction '%s' expects #bytesize2, rD, #port16", def->mnemonic);
			}
			value_t size_val = parse_value(instr->operands[0], line);
			uint8_t size_code = encode_bytesize2(&size_val, line, def->mnemonic);
			int reg = parse_register(trim(instr->operands[1]), line);
			value_t port_val = parse_value(instr->operands[2], line);
			if (port_val.is_label) {
				free(port_val.label);
				fatal(line, "labels not allowed for port immediates in '%s'", def->mnemonic);
			}
			if (port_val.value < 0 || port_val.value > 0xFFFF) {
				fatal(line, "port value %lld out of 16-bit range in '%s'", port_val.value, def->mnemonic);
			}
			byte_buf_append16(output, (uint16_t)port_val.value);
			uint8_t operand = (uint8_t)(((size_code & 0x03u) << 6) | ((reg & 0x0Fu) << 2));
			byte_buf_append(output, operand);
			break;
		}

		case FMT_RRPIN_VR: {
			if (!instr->rrpin_inner || !instr->rrpin_has_loop_reg || !instr->rrpin_has_reg) {
				fatal(line, "missing RRPIN.VR payload instruction or registers");
			}
			if (item->size_bytes < 3u) {
				fatal(line, "invalid RRPIN.VR size encountered");
			}
			byte_buf_append(output, (uint8_t)(((instr->rrpin_loop_reg & 0x0F) << 4) | (instr->rrpin_reg & 0x0F)));
			byte_buf_append(output, 0u);
			program_item_t inner_item = {0};
			inner_item.kind = ITEM_INSTR;
			inner_item.offset = item->offset + 3u;
			inner_item.size_bytes = item->size_bytes - 3u;
			inner_item.line = item->line;
			inner_item.u.instr = *(instr->rrpin_inner);
			encode_instruction(&inner_item, symbols, output, relocs, object_mode);
			break;
		}

		default:
			fatal(line, "unsupported format for '%s'", def->mnemonic);
	}
}

static void write_flat_binary(const char *output_path, const byte_buf_t *output) {
	FILE *out = fopen(output_path, "wb");
	if (!out) {
		fatal(0, "failed to open '%s' for writing: %s", output_path, strerror(errno));
	}
	if (output->count > 0) {
		size_t written = fwrite(output->data, 1, output->count, out);
		if (written != output->count) {
			fatal(0, "short write to '%s'", output_path);
		}
	}
	fclose(out);
}

static void write_bso_object(const char *output_path, const byte_buf_t *output, const symbol_vec_t *symbols, const relocation_vec_t *relocs) {
	if (!relocs) {
		fatal(0, "internal error: missing relocation table for object emission");
	}
	if (output->count > UINT32_MAX) {
		fatal(0, "output too large (%zu bytes) for BSO object", output->count);
	}
	size_t max_symbols = symbols->count + relocs->count;
	object_symbol_info_t *obj_syms = NULL;
	if (max_symbols > 0) {
		obj_syms = (object_symbol_info_t *)calloc(max_symbols, sizeof(object_symbol_info_t));
		if (!obj_syms) {
			fatal(0, "out of memory while preparing BSO symbols");
		}
	}
	size_t obj_count = 0;
	for (size_t i = 0; i < symbols->count; ++i) {
		obj_syms[obj_count].name = symbols->data[i].name;
		obj_syms[obj_count].value = symbols->data[i].address;
		obj_syms[obj_count].defined = true;
		obj_syms[obj_count].name_offset = 0;
		obj_count += 1;
	}
	for (size_t i = 0; i < relocs->count; ++i) {
		const relocation_entry_t *rel = &relocs->data[i];
		if (find_object_symbol_index(obj_syms, obj_count, rel->symbol) >= 0) {
			continue;
		}
		if (find_symbol(symbols, rel->symbol)) {
			continue;
		}
		if (obj_count >= max_symbols) {
			object_symbol_info_t *new_syms = (object_symbol_info_t *)realloc(obj_syms, (max_symbols + relocs->count) * sizeof(object_symbol_info_t));
			if (!new_syms) {
				free(obj_syms);
				fatal(0, "out of memory while expanding BSO symbol table");
			}
			obj_syms = new_syms;
			max_symbols += relocs->count;
		}
		obj_syms[obj_count].name = rel->symbol;
		obj_syms[obj_count].value = 0;
		obj_syms[obj_count].defined = false;
		obj_syms[obj_count].name_offset = 0;
		obj_count += 1;
	}
	byte_buf_t strtab = {0};
	byte_buf_append(&strtab, 0);
	for (size_t i = 0; i < obj_count; ++i) {
		obj_syms[i].name_offset = (uint32_t)strtab.count;
		const char *name = obj_syms[i].name ? obj_syms[i].name : "";
		size_t len = strlen(name) + 1;
		for (size_t j = 0; j < len; ++j) {
			byte_buf_append(&strtab, (uint8_t)name[j]);
		}
	}
	if (strtab.count > UINT32_MAX) {
		free(strtab.data);
		free(obj_syms);
		fatal(0, "string table too large for BSO object");
	}
	bso_symbol_record_t *symbol_records = NULL;
	if (obj_count > 0) {
		symbol_records = (bso_symbol_record_t *)malloc(obj_count * sizeof(bso_symbol_record_t));
		if (!symbol_records) {
			free(strtab.data);
			free(obj_syms);
			fatal(0, "out of memory while emitting BSO symbols");
		}
		for (size_t i = 0; i < obj_count; ++i) {
			symbol_records[i].name_offset = obj_syms[i].name_offset;
			symbol_records[i].value = obj_syms[i].defined ? obj_syms[i].value : 0;
			uint32_t flags = BSO_SYMBOL_GLOBAL;
			if (obj_syms[i].defined) {
				flags |= BSO_SYMBOL_DEFINED;
			}
			symbol_records[i].flags = flags;
		}
	}
	uint32_t reloc_count = (uint32_t)relocs->count;
	bso_relocation_record_t *reloc_records = NULL;
	if (reloc_count > 0) {
		reloc_records = (bso_relocation_record_t *)malloc(reloc_count * sizeof(bso_relocation_record_t));
		if (!reloc_records) {
			free(symbol_records);
			free(strtab.data);
			free(obj_syms);
			fatal(0, "out of memory while emitting BSO relocations");
		}
		for (uint32_t i = 0; i < reloc_count; ++i) {
			const relocation_entry_t *src = &relocs->data[i];
			if (src->offset > output->count) {
				free(reloc_records);
				free(symbol_records);
				free(strtab.data);
				free(obj_syms);
				fatal(0, "relocation offset %u out of range", src->offset);
			}
			int sym_index = find_object_symbol_index(obj_syms, obj_count, src->symbol);
			if (sym_index < 0) {
				free(reloc_records);
				free(symbol_records);
				free(strtab.data);
				free(obj_syms);
				fatal(0, "relocation references unknown symbol '%s'", src->symbol);
			}
			reloc_records[i].offset = src->offset;
			reloc_records[i].addend = src->addend;
			reloc_records[i].symbol_index = (uint32_t)sym_index;
			reloc_records[i].kind = (uint32_t)src->kind;
		}
	}
	bso_header_t header = {0};
	memcpy(header.magic, BSO_MAGIC, BSO_MAGIC_SIZE);
	header.version = BSO_VERSION;
	header.code_size = (uint32_t)output->count;
	header.symbol_count = (uint32_t)obj_count;
	header.relocation_count = reloc_count;
	header.string_table_size = (uint32_t)strtab.count;
	FILE *out = fopen(output_path, "wb");
	if (!out) {
		free(reloc_records);
		free(symbol_records);
		free(strtab.data);
		free(obj_syms);
		fatal(0, "failed to open '%s' for writing: %s", output_path, strerror(errno));
	}
	if (fwrite(&header, sizeof(header), 1, out) != 1) {
		fclose(out);
		free(reloc_records);
		free(symbol_records);
		free(strtab.data);
		free(obj_syms);
		fatal(0, "failed to write BSO header to '%s'", output_path);
	}
	if (output->count > 0 && fwrite(output->data, 1, output->count, out) != output->count) {
		fclose(out);
		free(reloc_records);
		free(symbol_records);
		free(strtab.data);
		free(obj_syms);
		fatal(0, "failed to write BSO code to '%s'", output_path);
	}
	if (obj_count > 0 && fwrite(symbol_records, sizeof(bso_symbol_record_t), obj_count, out) != obj_count) {
		fclose(out);
		free(reloc_records);
		free(symbol_records);
		free(strtab.data);
		free(obj_syms);
		fatal(0, "failed to write BSO symbols to '%s'", output_path);
	}
	if (reloc_count > 0 && fwrite(reloc_records, sizeof(bso_relocation_record_t), reloc_count, out) != reloc_count) {
		fclose(out);
		free(reloc_records);
		free(symbol_records);
		free(strtab.data);
		free(obj_syms);
		fatal(0, "failed to write BSO relocations to '%s'", output_path);
	}
	if (strtab.count > 0 && fwrite(strtab.data, 1, strtab.count, out) != strtab.count) {
		fclose(out);
		free(reloc_records);
		free(symbol_records);
		free(strtab.data);
		free(obj_syms);
		fatal(0, "failed to write BSO string table to '%s'", output_path);
	}
	fclose(out);
	free(reloc_records);
	free(symbol_records);
	free(strtab.data);
	free(obj_syms);
}

static void process_source_file(const char *path, symbol_vec_t *symbols, program_vec_t *program, uint32_t *pc, path_vec_t *included_files, string_lit_vec_t *string_literals) {
	FILE *fp = fopen(path, "r");
	if (!fp) {
		fatal(0, "failed to open '%s': %s", path, strerror(errno));
	}

	const char *prev_source = g_current_source;
	g_current_source = path;
	char *base_dir = duplicate_dirname(path);
	uint32_t local_pc = *pc;

	char linebuf[4096];
	size_t line_no = 0;
	while (fgets(linebuf, sizeof(linebuf), fp)) {
		line_no += 1;
		char *line = linebuf;
		char *newline = strchr(line, '\n');
		if (newline) {
			*newline = '\0';
		}

		char *comment = strstr(line, "//");
		if (comment) {
			*comment = '\0';
		}

		char *semi = strchr(line, ';');
		if (semi) {
			*semi = '\0';
		}

		char *cursor = trim(line);
		if (*cursor == '\0') {
			continue;
		}

		while (true) {
			char *colon = strchr(cursor, ':');
			if (!colon) {
				break;
			}
			bool has_space = false;
			for (char *p = cursor; p < colon; ++p) {
				if (isspace((unsigned char)*p)) {
					has_space = true;
					break;
				}
			}
			if (has_space) {
				break;
			}
			*colon = '\0';
			char *label = trim(cursor);
			if (*label == '\0') {
				fatal(line_no, "empty label");
			}
			add_symbol(symbols, label, local_pc, line_no);
			cursor = trim(colon + 1);
			if (*cursor == '\0') {
				break;
			}
		}

		if (*cursor == '\0') {
			continue;
		}

		bool force_include = false;
		char *directive_cursor = cursor;
		if (*directive_cursor) {
			char *scan = directive_cursor;
			if (*scan && !isspace((unsigned char)*scan)) {
				size_t token_len = 0;
				while (scan[token_len] && !isspace((unsigned char)scan[token_len]) && scan[token_len] != '%') {
					token_len += 1;
				}
				if (token_len > 0 && keyword_equals_ci(scan, token_len, "force")) {
					force_include = true;
					directive_cursor = trim(scan + token_len);
				}
			}
		}

		if (*directive_cursor == '%' || *directive_cursor == '.') {
			char directive[64];
			size_t dir_len = 0;
			while (directive_cursor[dir_len] && !isspace((unsigned char)directive_cursor[dir_len])) {
				if (dir_len + 1 >= sizeof(directive)) {
					fatal(line_no, "directive too long");
				}
				directive[dir_len] = (char)toupper((unsigned char)directive_cursor[dir_len]);
				dir_len += 1;
			}
			directive[dir_len] = '\0';
			char *rest = trim(directive_cursor + dir_len);
			if (strcmp(directive, "%INCLUDEFILE") == 0) {
				if (*rest == '\0') {
					fatal(line_no, "%%includefile requires a quoted path");
				}
				if (*rest != '"') {
					fatal(line_no, "%%includefile expects a quoted path");
				}
				byte_buf_t include_bytes = {0};
				const char *after_literal = parse_string_literal_bytes(rest, line_no, &include_bytes);
				char *include_arg = (char *)malloc(include_bytes.count + 1);
				if (!include_arg) {
					free(include_bytes.data);
					fatal(line_no, "out of memory while parsing include path");
				}
				memcpy(include_arg, include_bytes.data, include_bytes.count);
				include_arg[include_bytes.count] = '\0';
				free(include_bytes.data);
				char *extra = trim((char *)after_literal);
				if (*extra != '\0') {
					free(include_arg);
					fatal(line_no, "unexpected tokens after %%includefile path");
				}
				char *joined = join_paths(base_dir, include_arg);
				if (!joined) {
					free(include_arg);
					fatal(line_no, "failed to resolve include path");
				}
				char *canon = canonicalize_path(joined);
				if (!canon) {
					int saved_errno = errno;
					fatal(line_no, "failed to resolve include '%s': %s", include_arg, strerror(saved_errno));
				}
				if (force_include) {
					*pc = local_pc;
					process_source_file(canon, symbols, program, pc, included_files, string_literals);
					local_pc = *pc;
					free(canon);
				} else {
					bool added = false;
					const char *interned = path_vec_intern(included_files, canon, &added);
					free(canon);
					if (added) {
						*pc = local_pc;
						process_source_file(interned, symbols, program, pc, included_files, string_literals);
						local_pc = *pc;
					}
				}
				free(joined);
				free(include_arg);
				continue;
			}
			bool handled = false;
			if (strcmp(directive, "%ORIGIN") == 0) {
				char *expr = trim(rest);
				if (*expr == '\0') {
					fatal(line_no, "%%origin requires an address expression");
				}
				if (*expr == '#') {
					expr = trim(expr + 1);
					if (*expr == '\0') {
						fatal(line_no, "%%origin requires an address expression");
					}
				}
				long long origin_value = evaluate_const_expression(expr, line_no);
				if (origin_value < 0 || origin_value > (long long)UINT32_MAX) {
					fatal(line_no, "%%origin value %lld out of 32-bit range", origin_value);
				}
				set_origin_base((uint32_t)origin_value, ORIGIN_SOURCE_DIRECTIVE, line_no);
				handled = true;
			} else if (strcmp(directive, "%ALIGN") == 0) {
				char *expr = trim(rest);
				if (*expr == '\0') {
					fatal(line_no, "%%align requires an alignment value");
				}
				if (*expr == '#') {
					expr = trim(expr + 1);
					if (*expr == '\0') {
						fatal(line_no, "%%align requires an alignment value");
					}
				}
				long long align_value = evaluate_const_expression(expr, line_no);
				if (align_value <= 0) {
					fatal(line_no, "%%align value %lld must be positive", align_value);
				}
				if ((unsigned long long)align_value > UINT32_MAX) {
					fatal(line_no, "%%align value %lld out of range", align_value);
				}
				uint32_t align_u32 = (uint32_t)align_value;
				uint32_t padding = 0;
				uint32_t mod = align_u32 ? (local_pc % align_u32) : 0;
				if (mod != 0) {
					padding = align_u32 - mod;
				}
				if (padding > 0) {
					uint8_t *pad_bytes = (uint8_t *)calloc(padding, 1);
					if (!pad_bytes) {
						fatal(line_no, "out of memory while applying %%align");
					}
					program_item_t item = (program_item_t){0};
					item.kind = ITEM_DATA_BYTES;
					item.offset = local_pc;
					item.size_bytes = padding;
					item.line = line_no;
					item.u.data.bytes = pad_bytes;
					item.u.data.count = padding;
					ensure_program_capacity(program);
					program->data[program->count++] = item;
					local_pc += padding;
				}
				handled = true;
			} else if (strcmp(directive, ".STRING") == 0 || strcmp(directive, ".STRINGZ") == 0) {
				bool zero_terminate = (strcmp(directive, ".STRINGZ") == 0);
				byte_buf_t data_bytes = {0};
				parse_string_directive(rest, line_no, &data_bytes);
				if (zero_terminate) {
					byte_buf_append(&data_bytes, 0u);
				}
				program_item_t item = (program_item_t){0};
				item.kind = ITEM_DATA_BYTES;
				item.offset = local_pc;
				item.size_bytes = (uint32_t)data_bytes.count;
				item.line = line_no;
				item.u.data.bytes = data_bytes.data;
				item.u.data.count = data_bytes.count;
				ensure_program_capacity(program);
				program->data[program->count++] = item;
				local_pc += item.size_bytes;
				handled = true;
			} else if (strcmp(directive, ".BYTE") == 0 || strcmp(directive, ".WORD") == 0 ||
					strcmp(directive, ".DWORD") == 0 || strcmp(directive, ".QWORD") == 0) {
				size_t elem_size = 1;
				if (strcmp(directive, ".WORD") == 0) {
					elem_size = 2;
				} else if (strcmp(directive, ".DWORD") == 0) {
					elem_size = 4;
				} else if (strcmp(directive, ".QWORD") == 0) {
					elem_size = 8;
				}
				bool allow_strings = (strcmp(directive, ".BYTE") == 0);
				data_value_vec_t values = { .elem_size = elem_size };
				parse_data_values(rest, line_no, allow_strings, &values);
				program_item_t item = (program_item_t){0};
				item.kind = ITEM_DATA_VALUES;
				item.offset = local_pc;
				item.size_bytes = (uint32_t)(elem_size * values.count);
				item.line = line_no;
				item.u.data_values.entries = values.data;
				item.u.data_values.count = values.count;
				item.u.data_values.elem_size = elem_size;
				ensure_program_capacity(program);
				program->data[program->count++] = item;
				local_pc += item.size_bytes;
				handled = true;
			}
			if (!handled) {
				if (force_include) {
					fatal(line_no, "'force' modifier is only valid with %%includefile");
				}
				fatal(line_no, "unknown directive '%s'", cursor);
			}
			continue;
		}

		char mnemonic[64];
		size_t idx = 0;
		while (cursor[idx] && !isspace((unsigned char)cursor[idx])) {
			if (idx + 1 >= sizeof(mnemonic)) {
				fatal(line_no, "mnemonic too long");
			}
			mnemonic[idx] = cursor[idx];
			idx += 1;
		}
		mnemonic[idx] = '\0';
		cursor = trim(cursor + idx);

		const instr_def_t *def = lookup_instruction(mnemonic);
		if (!def) {
			fatal(line_no, "unknown instruction '%s'", mnemonic);
		}

		program_item_t item;
		memset(&item, 0, sizeof(item));
		item.kind = ITEM_INSTR;
		item.offset = local_pc;
		item.size_bytes = def->size_bytes;
		item.line = line_no;
		item.u.instr.def = def;
		item.u.instr.operand_count = 0;

		if (def->format == FMT_RRPIN) {
			if (*cursor == '\0') {
				fatal(line_no, "RRPIN expects '#loop32, INSTRUCTION'");
			}
			char *rr_copy = strdup(cursor);
			if (!rr_copy) {
				fatal(line_no, "out of memory while parsing RRPIN operands");
			}
			char *comma = find_top_level_comma(rr_copy);
			if (!comma) {
				free(rr_copy);
				fatal(line_no, "RRPIN requires a comma between loop count and instruction");
			}
			*comma = '\0';
			char *loop_part = trim(rr_copy);
			char *inner_part = trim(comma + 1);
			if (*loop_part == '\0' || *inner_part == '\0') {
				free(rr_copy);
				fatal(line_no, "RRPIN requires both a loop count and an instruction");
			}
			item.u.instr.operand_count = 1;
			item.u.instr.operands[0] = strdup(loop_part);
			if (!item.u.instr.operands[0]) {
				free(rr_copy);
				fatal(line_no, "out of memory copying RRPIN loop operand");
			}
			parsed_instr_t *inner = parse_inline_instruction(inner_part, line_no);
			if (!rrpin_instruction_allowed(inner->def)) {
				free(rr_copy);
				fatal(line_no, "instruction '%s' is not allowed inside RRPIN", inner->def->mnemonic);
			}
			if (inner->def->opcode == 0xF8u || inner->def->format == FMT_RRPIN) {
				free(rr_copy);
				fatal(line_no, "RRPIN cannot contain another RRPIN instruction");
			}
			if (inner->def->size_bytes == 0u) {
				free(rr_copy);
				fatal(line_no, "instruction '%s' has unsupported size for RRPIN", inner->def->mnemonic);
			}
			item.u.instr.rrpin_inner = inner;
			item.size_bytes = 5u + inner->def->size_bytes;
			free(rr_copy);
		} else if (def->format == FMT_RRPIN_R) {
			if (*cursor == '\0') {
				fatal(line_no, "RRPIN.R expects '#loop32, rD, INSTRUCTION'");
			}
			char *rr_copy = strdup(cursor);
			if (!rr_copy) {
				fatal(line_no, "out of memory while parsing RRPIN.R operands");
			}
			char *first_comma = find_top_level_comma(rr_copy);
			if (!first_comma) {
				free(rr_copy);
				fatal(line_no, "RRPIN.R requires commas between loop, register, and instruction");
			}
			*first_comma = '\0';
			char *loop_part = trim(rr_copy);
			char *reg_and_instr = trim(first_comma + 1);
			if (*loop_part == '\0' || *reg_and_instr == '\0') {
				free(rr_copy);
				fatal(line_no, "RRPIN.R requires loop count, register, and instruction");
			}
			char *second_comma = strchr(reg_and_instr, ',');
			if (!second_comma) {
				free(rr_copy);
				fatal(line_no, "RRPIN.R requires a second comma between register and instruction");
			}
			*second_comma = '\0';
			char *reg_part = trim(reg_and_instr);
			char *inner_part = trim(second_comma + 1);
			if (*reg_part == '\0' || *inner_part == '\0') {
				free(rr_copy);
				fatal(line_no, "RRPIN.R requires non-empty register and instruction operands");
			}
			item.u.instr.operand_count = 1;
			item.u.instr.operands[0] = strdup(loop_part);
			if (!item.u.instr.operands[0]) {
				free(rr_copy);
				fatal(line_no, "out of memory copying RRPIN.R loop operand");
			}
			int rr_reg = parse_register(reg_part, line_no);
			item.u.instr.rrpin_reg = rr_reg;
			item.u.instr.rrpin_has_reg = true;
			parsed_instr_t *inner = parse_inline_instruction(inner_part, line_no);
			if (!rrpin_instruction_allowed(inner->def)) {
				free(rr_copy);
				fatal(line_no, "instruction '%s' is not allowed inside RRPIN.R", inner->def->mnemonic);
			}
			if (inner->def->opcode == 0xF8u || inner->def->format == FMT_RRPIN || inner->def->format == FMT_RRPIN_R) {
				free(rr_copy);
				fatal(line_no, "RRPIN.R cannot contain another RRPIN or RRPIN.R instruction");
			}
			if (inner->def->size_bytes == 0u) {
				free(rr_copy);
				fatal(line_no, "instruction '%s' has unsupported size for RRPIN.R", inner->def->mnemonic);
			}
			item.u.instr.rrpin_inner = inner;
			item.size_bytes = 6u + inner->def->size_bytes;
			free(rr_copy);
		} else if (def->format == FMT_RRPIN_V) {
			if (*cursor == '\0') {
				fatal(line_no, "RRPIN.V expects 'rT, INSTRUCTION'");
			}
			char *rr_copy = strdup(cursor);
			if (!rr_copy) {
				fatal(line_no, "out of memory while parsing RRPIN.V operands");
			}
			char *comma = find_top_level_comma(rr_copy);
			if (!comma) {
				free(rr_copy);
				fatal(line_no, "RRPIN.V requires a comma between register and instruction");
			}
			*comma = '\0';
			char *reg_part = trim(rr_copy);
			char *inner_part = trim(comma + 1);
			if (*reg_part == '\0' || *inner_part == '\0') {
				free(rr_copy);
				fatal(line_no, "RRPIN.V requires both register and instruction operands");
			}
			int loop_reg = parse_register(reg_part, line_no);
			item.u.instr.rrpin_loop_reg = loop_reg;
			item.u.instr.rrpin_has_loop_reg = true;
			parsed_instr_t *inner = parse_inline_instruction(inner_part, line_no);
			if (!rrpin_instruction_allowed(inner->def)) {
				free(rr_copy);
				fatal(line_no, "instruction '%s' is not allowed inside RRPIN.V", inner->def->mnemonic);
			}
			if (inner->def->opcode == 0xF8u || inner->def->format == FMT_RRPIN || inner->def->format == FMT_RRPIN_R || inner->def->format == FMT_RRPIN_V || inner->def->format == FMT_RRPIN_VR) {
				free(rr_copy);
				fatal(line_no, "RRPIN.V cannot contain another RRPIN variant");
			}
			if (inner->def->size_bytes == 0u) {
				free(rr_copy);
				fatal(line_no, "instruction '%s' has unsupported size for RRPIN.V", inner->def->mnemonic);
			}
			item.u.instr.rrpin_inner = inner;
			item.size_bytes = 2u + inner->def->size_bytes;
			free(rr_copy);
		} else if (def->format == FMT_RRPIN_VR) {
			if (*cursor == '\0') {
				fatal(line_no, "RRPIN.VR expects 'rT, rS, INSTRUCTION'");
			}
			char *rr_copy = strdup(cursor);
			if (!rr_copy) {
				fatal(line_no, "out of memory while parsing RRPIN.VR operands");
			}
			char *first_comma = find_top_level_comma(rr_copy);
			if (!first_comma) {
				free(rr_copy);
				fatal(line_no, "RRPIN.VR requires commas between register operands and instruction");
			}
			*first_comma = '\0';
			char *loop_part = trim(rr_copy);
			char *reg_and_instr = trim(first_comma + 1);
			if (*loop_part == '\0' || *reg_and_instr == '\0') {
				free(rr_copy);
				fatal(line_no, "RRPIN.VR requires loop register, increment register, and instruction");
			}
			char *second_comma = find_top_level_comma(reg_and_instr);
			if (!second_comma) {
				free(rr_copy);
				fatal(line_no, "RRPIN.VR requires a second comma between register and instruction");
			}
			*second_comma = '\0';
			char *inc_part = trim(reg_and_instr);
			char *inner_part = trim(second_comma + 1);
			if (*inc_part == '\0' || *inner_part == '\0') {
				free(rr_copy);
				fatal(line_no, "RRPIN.VR requires non-empty increment register and instruction operands");
			}
			int loop_reg = parse_register(loop_part, line_no);
			int inc_reg = parse_register(inc_part, line_no);
			if (loop_reg == inc_reg) {
				free(rr_copy);
				fatal(line_no, "RRPIN.VR loop and increment registers must differ");
			}
			item.u.instr.rrpin_loop_reg = loop_reg;
			item.u.instr.rrpin_has_loop_reg = true;
			item.u.instr.rrpin_reg = inc_reg;
			item.u.instr.rrpin_has_reg = true;
			parsed_instr_t *inner = parse_inline_instruction(inner_part, line_no);
			if (!rrpin_instruction_allowed(inner->def)) {
				free(rr_copy);
				fatal(line_no, "instruction '%s' is not allowed inside RRPIN.VR", inner->def->mnemonic);
			}
			if (inner->def->opcode == 0xF8u || inner->def->format == FMT_RRPIN || inner->def->format == FMT_RRPIN_R || inner->def->format == FMT_RRPIN_V || inner->def->format == FMT_RRPIN_VR) {
				free(rr_copy);
				fatal(line_no, "RRPIN.VR cannot contain another RRPIN variant");
			}
			if (inner->def->size_bytes == 0u) {
				free(rr_copy);
				fatal(line_no, "instruction '%s' has unsupported size for RRPIN.VR", inner->def->mnemonic);
			}
			item.u.instr.rrpin_inner = inner;
			item.size_bytes = 3u + inner->def->size_bytes;
			free(rr_copy);
		} else if (*cursor != '\0') {
			item.u.instr.operand_count = split_operands(cursor, item.u.instr.operands, MAX_OPERANDS);
			maybe_convert_string_operand(&item.u.instr, def, line_no, string_literals);
		}

		ensure_program_capacity(program);
		program->data[program->count++] = item;
		local_pc += item.size_bytes;
	}

	free(base_dir);
	fclose(fp);
	g_current_source = prev_source;
	*pc = local_pc;
}

static void assemble(const char *input_path, const char *output_path, bool emit_object) {
	symbol_vec_t symbols = {0};
	program_vec_t program = {0};
	path_vec_t included_files = {0};
	string_lit_vec_t string_literals = {0};
	uint32_t pc = 0;

	char *canonical = canonicalize_path(input_path);
	if (!canonical) {
		int saved_errno = errno;
		fatal(0, "failed to resolve '%s': %s", input_path, strerror(saved_errno));
	}
	bool added = false;
	const char *root_path = path_vec_intern(&included_files, canonical, &added);
	free(canonical);
	process_source_file(root_path, &symbols, &program, &pc, &included_files, &string_literals);

	for (size_t i = 0; i < string_literals.count; ++i) {
		string_lit_t *lit = &string_literals.data[i];
		add_symbol(&symbols, lit->label, pc, lit->line);
		program_item_t item = (program_item_t){0};
		item.kind = ITEM_DATA_BYTES;
		item.offset = pc;
		item.size_bytes = (uint32_t)lit->count;
		item.line = lit->line;
		item.u.data.bytes = lit->bytes;
		item.u.data.count = lit->count;
		ensure_program_capacity(&program);
		program.data[program.count++] = item;
		pc += item.size_bytes;
		lit->bytes = NULL;
		lit->count = 0;
	}

	byte_buf_t output = {0};
	relocation_vec_t relocs = {0};
	relocation_vec_t *relocs_ptr = emit_object ? &relocs : NULL;
	for (size_t i = 0; i < program.count; ++i) {
		if (program.data[i].kind == ITEM_INSTR) {
			encode_instruction(&program.data[i], &symbols, &output, relocs_ptr, emit_object);
		} else if (program.data[i].kind == ITEM_DATA_BYTES) {
			for (size_t j = 0; j < program.data[i].u.data.count; ++j) {
				byte_buf_append(&output, program.data[i].u.data.bytes[j]);
			}
		} else if (program.data[i].kind == ITEM_DATA_VALUES) {
			encode_data_values(&program.data[i], &symbols, &output, relocs_ptr, emit_object);
		}
	}

	if (emit_object) {
		write_bso_object(output_path, &output, &symbols, relocs_ptr);
	} else {
		write_flat_binary(output_path, &output);
	}

	free(output.data);
	free_program(&program);
	free_symbols(&symbols);
	free_path_vec(&included_files);
	free_relocations(&relocs);
	free_string_literals(&string_literals);
}

static void fatal(size_t line, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	if (g_fatal_jmp) {
		format_error_message(g_error_buffer, g_error_buffer_size, line, fmt, args);
		va_end(args);
		longjmp(*g_fatal_jmp, 1);
	}
	fprintf(stderr, "bas: error");
	if (line) {
		if (g_current_source) {
			fprintf(stderr, " (%s:%zu)", g_current_source, line);
		} else {
			fprintf(stderr, " (line %zu)", line);
		}
	} else if (g_current_source) {
		fprintf(stderr, " (%s)", g_current_source);
	}
	fprintf(stderr, ": ");
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

int bas_assemble_file(const char *input_path,
			      const char *output_path,
			      bool emit_object,
			      bool has_origin,
			      uint32_t origin,
			      char *error_buffer,
			      size_t error_buffer_size) {
	jmp_buf fatal_jmp;
	jmp_buf *prev_fatal_jmp = g_fatal_jmp;
	char *prev_error_buffer = g_error_buffer;
	size_t prev_error_buffer_size = g_error_buffer_size;
	reset_assembler_state();
	g_fatal_jmp = &fatal_jmp;
	g_error_buffer = error_buffer;
	g_error_buffer_size = error_buffer_size;
	if (g_error_buffer && g_error_buffer_size > 0) {
		g_error_buffer[0] = '\0';
	}
	if (setjmp(fatal_jmp) != 0) {
		g_fatal_jmp = prev_fatal_jmp;
		g_error_buffer = prev_error_buffer;
		g_error_buffer_size = prev_error_buffer_size;
		reset_assembler_state();
		return -1;
	}
	if (has_origin) {
		set_origin_base(origin, ORIGIN_SOURCE_CMDLINE, 0);
	}
	assemble(input_path, output_path, emit_object);
	g_fatal_jmp = prev_fatal_jmp;
	g_error_buffer = prev_error_buffer;
	g_error_buffer_size = prev_error_buffer_size;
	reset_assembler_state();
	return 0;
}

