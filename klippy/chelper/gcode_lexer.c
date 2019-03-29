// G-code lexer implementation
//
// Design notes:
//   - Lexical state is a property of the lexer.  No state is encoded in call
//     stack or stack variables
//   - Of necessity, the lexer has a significant amount of semantic awareness.
//     This includes statement position (line number/command name/arguments)
//     and limited nested context (command/expression/string)
//   - Stack frames are kept to an absolute minimum via use of switch
//     statements, inline functions and local character classification
//   - The parser is not UTF-8 aware except in the implementation of Unicode
//     string escapes, but surrogates should pass cleanly through for strings
//   - Arguments parse in three different modes, "traditional" where keys are a
//     single character followed immediately by the value, "extended"
//     (key=value) and "raw" (argument is a single string).  The lexer inspects
//     the command name to determine which mode is appropriate.
//   - Errors terminate scanner output until the next statement
//   - Lexer uses a single buffer for capturing, growing as necessary.  This is
//     the only allocation that occurs during scanning
//   - We parse integer values directly.  Float values scan for lexical
//     correctness but actual double value is computed via the standard
//     library to keep complexity low and accuracy high
//   - A special "bridge" is emitted when words and expressions connect without
//     intervening whitespace (e.g. foo{1}); the parser uses to generate a
//     concatenation expression
//   - String and numeric literals parse according to C standards.  This
//     includes all string escapes, binary/octal/decimal/hex integers, and
//     decimal/hex floats
//
// Copyright (C) 2019 Greg Lauckhart <greg@lauckhart.com>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "gcode_lexer.h"
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#define ENTER_EXPR '{'
#define EXIT_EXPR '}'

#define TOKEN_STOP { \
    if (lexer->location) { \
        lexer->location->last_line = lexer->line; \
        lexer->location->last_column = lexer->column + 1; \
    } \
}

#define TOKEN_START { \
    if (lexer->location) { \
        lexer->location->first_line = lexer->line; \
        lexer->location->first_column = lexer->column; \
    } \
    TOKEN_STOP; \
}

#define ERROR(args...) { \
    emit_error(lexer, args); \
    lexer->state = SCAN_ERROR; \
}

typedef enum state_t {
    SCAN_NEWLINE,
    SCAN_ERROR,
    SCAN_LINENO,
    SCAN_AFTER_LINENO,
    SCAN_COMMAND_NAME,
    SCAN_ARGS,
    SCAN_EXTENDED_KEY,
    SCAN_AFTER_EXTENDED_KEY,
    SCAN_AFTER_EXTENDED_SEPARATOR,
    SCAN_AFTER_TRADITIONAL_KEY,
    SCAN_ARG_VALUE,
    SCAN_COMMENT,
    SCAN_EMPTY_LINE_COMMENT,
    SCAN_EXPR,
    SCAN_AFTER_EXPR,
    SCAN_SYMBOL,
    SCAN_IDENTIFIER,
    SCAN_STR,
    SCAN_STR_ESCAPE,
    SCAN_STR_OCTAL,
    SCAN_STR_HEX,
    SCAN_STR_LOW_UNICODE,
    SCAN_STR_HIGH_UNICODE,
    SCAN_NUMBER_BASE,
    SCAN_DECIMAL,
    SCAN_HEX,
    SCAN_BINARY,
    SCAN_OCTAL,
    SCAN_DOT,
    SCAN_DECIMAL_FLOAT,
    SCAN_DECIMAL_FRACTION,
    SCAN_DECIMAL_EXPONENT_SIGN,
    SCAN_DECIMAL_EXPONENT,
    SCAN_HEX_FLOAT,
    SCAN_HEX_FRACTION,
    SCAN_HEX_EXPONENT_SIGN,
    SCAN_HEX_EXPONENT
} state_t;

typedef enum arg_mode_t {
    ARG_TRADITIONAL,
    ARG_EXTENDED,
    ARG_RAW
} arg_mode_t;

struct GCodeLexer {
    void*       context;
    state_t     state;
    char*       token_str;
    size_t      token_length;
    size_t      token_limit;
    char*       error_str;
    size_t      error_limit;
    size_t      expr_nesting;
    int64_t     int_value;
    int8_t      digit_count;
    uint32_t    line;
    uint32_t    column;
    arg_mode_t  arg_mode;
    state_t     after_str;
    bool        in_arg_value;
    GCodeLocation* location;
};

GCodeLexer* gcode_lexer_new(
    void* context,
    GCodeLocation* location
) {
    GCodeLexer* lexer = malloc(sizeof(GCodeLexer));
    if (!lexer)
        return NULL;

    lexer->context = context;
    lexer->location = location;

    lexer->token_str = NULL;
    lexer->token_length = lexer->token_limit = 0;

    lexer->error_str = NULL;
    lexer->error_limit = 0;

    gcode_lexer_reset(lexer);

    return lexer;
}

// vsnprintf with error handling and argp management
static int error_printf(GCodeLexer* lexer, size_t limit, const char* format, va_list argp) {
    if (lexer->error_limit < limit) {
        if (!lexer->error_limit)
            lexer->error_limit = 256;
        while (lexer->error_limit < limit)
            lexer->error_limit *= 2;
        lexer->error_str = realloc(lexer->error_str, lexer->error_limit);
        if (!lexer->error_str) {
            lex_error(lexer->context, "Out of memory (gcode error_printf)");
            va_end(argp);
            return -1;
        }
    }

    int rv = vsnprintf(lexer->error_str, lexer->error_limit, format, argp);
    va_end(argp);
    if (rv < 0) {
        lex_error(lexer->context, "Internal: Failed to format error");
        return -1;
    }

    return rv;
}

static void emit_error(GCodeLexer* lexer, const char* format, ...) {
    va_list argp;
    va_start(argp, format);
    int rv = error_printf(lexer, 256, format, argp);
    if (rv < 0)
        return;

    if (rv > lexer->error_limit) {
        va_start(argp, format);
        error_printf(lexer, rv, format, argp);
    }
}

static bool token_alloc(GCodeLexer* lexer) {
    if (lexer->token_str == NULL) {
        lexer->token_limit = 128;
        lexer->token_str = malloc(lexer->token_limit);
        if (!lexer->token_str) {
            TOKEN_STOP;
            ERROR("Out of memory (token_alloc)");
            return false;
        }
    } else {
        lexer->token_limit *= 2;
        lexer->token_str = realloc(lexer->token_str, lexer->token_limit);
        if (!lexer->token_str) {
            TOKEN_STOP;
            ERROR("Out of memory (token_alloc)");
            return false;
        }
    }
    return true;
}

static inline bool token_char(GCodeLexer* lexer, const char ch) {
    if (lexer->token_length == lexer->token_limit
        && !token_alloc
    (lexer)
    )
        return false;
    lexer->token_str[lexer->token_length++] = ch;
    return true;
}

static inline bool add_str_wchar(GCodeLexer* lexer) {
    char buf[MB_CUR_MAX];

    int len = wctomb(buf, lexer->int_value);
    if (len == -1) {
        token_char(lexer, '?');
        return true;
    }

    for (int i = 0; i < len; i++)
        if (!token_char(lexer, buf[i]))
            return false;
    return true;
}

static inline bool terminate_token(GCodeLexer* lexer) {
    return token_char(lexer, '\0');
}

static inline char hex_digit_to_int(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'Z')
        return 10 + ch - 'A';
    return -1;
}

static inline void add_safe_digit(GCodeLexer* lexer, int8_t value,
                           int8_t base)
 {
    lexer->int_value = lexer->int_value * base + value;
    lexer->digit_count++;
}

static inline bool is_ident_char(char ch) {
    return (ch >= 'a' && ch <= 'z')
        || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9')
        || ch == '_'
        || ch == '$';
}

static inline bool is_symbol_char(char ch) {
    switch (ch) {
    case '`':
    case '~':
    case '!':
    case '@':
    case '#':
    case '%':
    case '^':
    case '&':
    case '*':
    case '(':
    case ')':
    case '-':
    case '+':
    case '=':
    case '{':
    case '[':
    case '}':
    case ']':
    case '|':
    case '\\':
    case ':':
    case ',':
    case '<':
    case '.':
    case '>':
    case '?':
    case '/':
        return true;
    }

    return false;
}

static inline bool continue_symbol(char c1, char c2) {
    switch (c1) {
    case '*':
        return c2 == '*';

    case '<':
    case '>':
    case '=':
        return c2 == '=';
    }
    return false;
}

static inline void free_token(GCodeLexer* lexer) {
    lexer->token_length = 0;
}

static inline bool emit_symbol(GCodeLexer* lexer) {
    TOKEN_STOP;

    if (!lex_keyword(lexer->context, lexer->token_str)) {
        lexer->state = SCAN_ERROR;
        free_token(lexer);
        return false;
    }

    free_token(lexer);
    return true;
}

static inline bool emit_char_symbol(GCodeLexer* lexer, char ch) {
    TOKEN_START;
    TOKEN_STOP;
    token_char(lexer, ch);
    terminate_token(lexer);

    if (!lex_keyword(lexer->context, lexer->token_str)) {
        lexer->state = SCAN_ERROR;
        free_token(lexer);
        return false;
    }

    free_token(lexer);
    return true;
}

static inline bool emit_bridge(GCodeLexer* lexer) {
    TOKEN_START;
    TOKEN_STOP;
    if (!lex_bridge(lexer->context)) {
        lexer->state = SCAN_ERROR;
        return false;
    }
    return true;
}

static inline bool emit_end_of_statement(GCodeLexer* lexer) {
    TOKEN_START;
    TOKEN_STOP;
    lex_end_statement(lexer->context);
    lexer->state = SCAN_NEWLINE;
    return true;
}

#define EMIT_CHAR_SYMBOL() emit_char_symbol(lexer, ch)

static inline bool emit_str(GCodeLexer* lexer) {
    TOKEN_STOP;
    if (!terminate_token(lexer)) {
        free_token(lexer);
        return false;
    }
    if (!lex_str_literal(lexer->context, lexer->token_str)) {
        lexer->state = SCAN_ERROR;
        free_token(lexer);
        return false;
    }
    free_token(lexer);
    return true;
}

static inline bool emit_possible_str(GCodeLexer* lexer) {
    if (lexer->token_length)
        return emit_str(lexer);
    return true;
}

static inline bool emit_ident(GCodeLexer* lexer) {
    TOKEN_STOP;
    if (!terminate_token(lexer)) {
        free_token(lexer);
        return false;
    }
    if (!lex_identifier(lexer->context, lexer->token_str)) {
        lexer->state = SCAN_ERROR;
        free_token(lexer);
        return false;
    }
    free_token(lexer);
    return true;
}

static inline bool enter_args(GCodeLexer* lexer) {
    lexer->state = SCAN_ARGS;

    const char* name = lexer->token_str;
    size_t length = lexer->token_length;

    if (!strcmp(lexer->token_str, "M117")
        || !strcmp(lexer->token_str, "ECHO"))
    {
        lexer->arg_mode = ARG_RAW;
        return emit_ident(lexer);
    }

    if (length > 1 && name[0] >= 'A' && name[0] <= 'Z') {
        for (size_t i = 1; i < length; i++)
            if (name[i] < '0' || name[i] > '9') {
                lexer->arg_mode = ARG_EXTENDED;
                return emit_ident(lexer);
            }
        lexer->arg_mode = ARG_TRADITIONAL;
        return emit_ident(lexer);
    }

    lexer->arg_mode = ARG_EXTENDED;

    return emit_ident(lexer);
}

static inline bool emit_int(GCodeLexer* lexer, int value) {
    TOKEN_STOP;
    if (!lex_int_literal(lexer->context, value)) {
        lexer->state = SCAN_ERROR;
        return false;
    }
    return true;
}

#define EMIT_INT() emit_int(lexer, lexer->int_value)

static inline bool emit_float(GCodeLexer* lexer) {
    TOKEN_STOP;
    terminate_token(lexer);
    char* end;
    double value = strtod(lexer->token_str, &end);
    free_token(lexer);
    if (*end) {
        ERROR("Invalid float %s", lexer->token_str);
        return false;
    }
    if (!lex_float_literal(lexer->context, value)) {
        lexer->state = SCAN_ERROR;
        return false;
    }
    return true;
}

static inline bool emit_keyword_or_identifier(GCodeLexer* lexer) {
    TOKEN_STOP;

    bool result;
    result = lex_identifier(lexer->context, lexer->token_str);

    free_token(lexer);
    return result;
}

static inline bool digit_exceeds(GCodeLexer* lexer, uint8_t value,
                                 int16_t base, int64_t max)
{
    return lexer->int_value > (max - value) / base;
}

static inline bool add_digit(GCodeLexer* lexer, uint8_t value, int16_t base,
    int64_t max, const char* err)
{
    if (digit_exceeds(lexer, value, base, max)) {
        ERROR(err);
        free_token(lexer);
        return false;
    }
    add_safe_digit(lexer, value, base);
    return true;
}

static inline bool enter_expr(GCodeLexer* lexer) {
    if (emit_char_symbol(lexer, ENTER_EXPR)) {
        lexer->state = SCAN_EXPR;
        lexer->expr_nesting = 1;
        return true;
    }
    return false;
}

static inline void end_arg_segment(GCodeLexer* lexer, char ch) {
    switch (lexer->arg_mode) {
        case ARG_TRADITIONAL:
            if (!lexer->in_arg_value) {
                TOKEN_START;
                TOKEN_STOP;

                // Empty value
                emit_str(lexer);
            }
            break;

        case ARG_EXTENDED:
            if (!lexer->in_arg_value) {
                TOKEN_START;
                TOKEN_STOP;
                ERROR("Expected '=' after parameter name");
                if (ch == '\n')
                    emit_end_of_statement(lexer);
                return;
            }
            break;

        case ARG_RAW:
            if (ch != '\r' && ch != '\n') {
                if (emit_bridge(lexer) && token_char(lexer, ch))
                    lexer->state = SCAN_ARG_VALUE;
                return;
            }
            break;
    }
    switch (ch) {
        case ';':
            lexer->state = SCAN_COMMENT;
            break;

        case '\n':
            emit_end_of_statement(lexer);
            lexer->state = SCAN_NEWLINE;
            break;

        default:
            lexer->state = SCAN_ARGS;
            break;
    }
}

#define TOKEN_CHAR(ch) token_char(lexer, ch)

#define TOKEN_CHAR_UPPER() \
    TOKEN_CHAR(ch >= 'a' && ch <= 'z' ? ch - 32 : ch)

#define TOKEN_CHAR_LOWER() \
    TOKEN_CHAR(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch)

#define CASE_SPACE case ' ': case '\t': case '\v': case '\r':

#define BACK_UP { \
    buffer--; \
    ch = 0; \
}

#define CASE_STR_ESC(esc_ch, ch) \
    case esc_ch: \
        if (TOKEN_CHAR(ch)) \
            lexer->state = SCAN_STR; \
        break;

static const int UNICODE_MAX = 0x10ffff;

// Get ready for monster switch statement.  Two reasons for this:
//   - Performance (no function call overhead)
//   - Incremental scanning (buffer may terminate anywhere in a statement)
void gcode_lexer_scan(GCodeLexer* lexer, const char* buffer, size_t length) {
    const char* end = buffer + length;
    int8_t digit_value;
    for (char ch = *buffer; buffer < end; ch = (*++buffer)) {
        switch (lexer->state) {
        case SCAN_NEWLINE:
            lexer->expr_nesting = 0;
            switch (ch) {
            case 'N':
            case 'n':
                lexer->state = SCAN_LINENO;
                break;

            case ';':
                lexer->state = SCAN_EMPTY_LINE_COMMENT;
                break;

            case '\n':
            CASE_SPACE
                break;

            default:
                BACK_UP;
                lexer->state = SCAN_COMMAND_NAME;
                break;
            }
            break;

        case SCAN_ERROR:
            if (ch == '\n')
                lexer->state = SCAN_NEWLINE;
            break;

        case SCAN_LINENO:
            switch (ch) {
            case '\n':
                lexer->state = SCAN_NEWLINE;
                break;

            CASE_SPACE
                lexer->state = SCAN_AFTER_LINENO;
                break;

            case ';':
                lexer->state = SCAN_EMPTY_LINE_COMMENT;
                break;

            case '"':
                ERROR("String not allowed in line number");
                break;

            case ENTER_EXPR:
                ERROR("Expression not allowed in line number");
                break;
            }
            break;

        case SCAN_AFTER_LINENO:
            switch (ch) {
            case '\n':
                lexer->state = SCAN_NEWLINE;
                break;

            CASE_SPACE
                break;

            case ';':
                lexer->state = SCAN_EMPTY_LINE_COMMENT;
                break;

            default:
                BACK_UP;
                lexer->state = SCAN_COMMAND_NAME;
                break;
            }
            break;

        case SCAN_COMMAND_NAME:
            switch (ch) {
            case ENTER_EXPR:
                TOKEN_START;
                free_token(lexer);
                ERROR("Expressions not allowed in command name");
                break;

            case '"':
                TOKEN_START;
                free_token(lexer);
                ERROR("Strings not allowed in command name");
                break;

            case '\n':
                emit_ident(lexer);
                emit_end_of_statement(lexer);
                break;

            CASE_SPACE
                enter_args(lexer);
                break;

            case ';':
                if (emit_ident(lexer))
                    lexer->state = SCAN_COMMENT;
                break;

            default:
                TOKEN_CHAR_UPPER();
                break;
            }
            break;

        case SCAN_ARGS:
            switch (ch) {
            case ENTER_EXPR:
                lexer->in_arg_value = false;
                if (EMIT_CHAR_SYMBOL())
                    lexer->state = SCAN_EXPR;
                break;

            case '"':
                switch (lexer->arg_mode) {
                case ARG_TRADITIONAL:
                    lexer->after_str = SCAN_AFTER_TRADITIONAL_KEY;
                    lexer->state = SCAN_STR;
                    break;

                case ARG_EXTENDED:
                    lexer->in_arg_value = false;
                    lexer->after_str = SCAN_AFTER_EXPR;
                    lexer->state = SCAN_STR;
                    break;

                case ARG_RAW:
                    TOKEN_CHAR(ch);
                    break;
                }
                break;

            case '\n':
                emit_end_of_statement(lexer);
                break;

            case ';':
                lexer->state = SCAN_COMMENT;
                break;

            case '=':
                ERROR("Expected parameter name before '='");
                break;

            CASE_SPACE
                break;

            default:
                TOKEN_START;
                switch (lexer->arg_mode) {
                case ARG_TRADITIONAL:
                    if (TOKEN_CHAR_UPPER() && emit_str(lexer)) {
                        lexer->in_arg_value = false;
                        lexer->state = SCAN_AFTER_TRADITIONAL_KEY;
                    }
                    break;

                case ARG_EXTENDED:
                    if (TOKEN_CHAR_UPPER()) {
                        lexer->in_arg_value = false;
                        lexer->state = SCAN_EXTENDED_KEY;
                    }
                    break;

                case ARG_RAW:
                    if (TOKEN_CHAR(ch))
                        lexer->state = SCAN_ARG_VALUE;
                    break;
                }
                break;
            }
            break;

        case SCAN_EXTENDED_KEY:
            switch (ch) {
            case '\n':
            case ';':
                free_token(lexer);
                end_arg_segment(lexer, ch);
                break;

            CASE_SPACE
                if (emit_possible_str(lexer))
                    lexer->state = SCAN_AFTER_EXTENDED_KEY;
                break;

            case '=':
                if (emit_possible_str(lexer))
                    lexer->state = SCAN_AFTER_EXTENDED_SEPARATOR;
                break;

            case ENTER_EXPR:
                if (emit_possible_str(lexer) && emit_bridge(lexer))
                    enter_expr(lexer);
                break;

            case '"':
                if (emit_possible_str(lexer) && emit_bridge(lexer)) {
                    lexer->after_str = SCAN_AFTER_EXPR;
                    lexer->state = SCAN_STR;
                }
                break;

            default:
                TOKEN_CHAR_UPPER();
                break;
            }
            break;

        case SCAN_AFTER_EXTENDED_KEY:
            switch (ch) {
            case '=':
                lexer->state = SCAN_AFTER_EXTENDED_SEPARATOR;
                break;

            CASE_SPACE
                break;

            case ';':
            case '\n':
                end_arg_segment(lexer, ch);
                break;

            default:
                lexer->in_arg_value = true;
                end_arg_segment(lexer, ch);
                break;
            }
            break;

        case SCAN_AFTER_EXTENDED_SEPARATOR:
            switch (ch) {
            case '\n':
            case ';':
                end_arg_segment(lexer, ch);
                break;

            CASE_SPACE
                break;

            case '"':
                lexer->in_arg_value = true;
                lexer->after_str = SCAN_AFTER_EXPR;
                lexer->state = SCAN_STR;
                break;

            case ENTER_EXPR:
                lexer->in_arg_value = true;
                enter_expr(lexer);
                break;

            default:
                BACK_UP;
                lexer->in_arg_value = true;
                lexer->state = SCAN_ARG_VALUE;
            }
            break;

        case SCAN_AFTER_TRADITIONAL_KEY:
            switch (ch) {
                case '=':
                    // Allow optional "=" to keep things uniform
                    break;

                case '\n':
                case ';':
                CASE_SPACE
                    end_arg_segment(lexer, ch);
                    break;

                case '"':
                    lexer->after_str = SCAN_ARG_VALUE;
                    lexer->state = SCAN_STR;
                    break;

                case ENTER_EXPR:
                    lexer->in_arg_value = true;
                    enter_expr(lexer);
                    break;

                default:
                    lexer->state = SCAN_ARG_VALUE;
                    lexer->in_arg_value = true;
                    BACK_UP;
                    break;
            }
            break;

        case SCAN_ARG_VALUE:
            switch (ch) {
            case '\n':
                emit_possible_str(lexer);
                emit_end_of_statement(lexer);
                break;

            case ';':
                if (lexer->arg_mode == ARG_RAW)
                    TOKEN_CHAR(ch);
                else if (emit_possible_str(lexer))
                    lexer->state = SCAN_COMMENT;
                break;

            CASE_SPACE
                if (lexer->arg_mode == ARG_RAW)
                    TOKEN_CHAR(ch);
                else if (emit_possible_str(lexer))
                    lexer->state = SCAN_ARGS;
                break;

            case '"':
                if (emit_possible_str(lexer) && emit_bridge(lexer)) {
                    lexer->after_str = SCAN_AFTER_EXPR;
                    lexer->state = SCAN_STR;
                }
                break;

            case ENTER_EXPR:
                if (emit_possible_str(lexer) && emit_bridge(lexer))
                    enter_expr(lexer);
                break;

            default:
                TOKEN_CHAR(ch);
                break;
            }
            break;

        case SCAN_COMMENT:
            if (ch == '\n')
                emit_end_of_statement(lexer);
            break;

        case SCAN_EMPTY_LINE_COMMENT:
            if (ch == '\n')
                lexer->state = SCAN_NEWLINE;
            break;

        case SCAN_EXPR:
            switch (ch) {
            case '\n':
                TOKEN_START;
                TOKEN_STOP;
                ERROR("Unterminated expression");
                lexer->state = SCAN_NEWLINE;
                break;

            CASE_SPACE
                break;

            case '(':
                lexer->expr_nesting++;
                EMIT_CHAR_SYMBOL();
                break;

            case ')':
                if (lexer->expr_nesting)
                    lexer->expr_nesting--;
                EMIT_CHAR_SYMBOL();
                break;

            case EXIT_EXPR:
                lexer->expr_nesting = 0;
                if (EMIT_CHAR_SYMBOL())
                    lexer->state = SCAN_AFTER_EXPR;
                break;

            case '0':
                TOKEN_START;
                TOKEN_CHAR(ch);
                lexer->state = SCAN_NUMBER_BASE;
                break;

            case '\'':
            case '`':
                TOKEN_START;
                ERROR("Unexpected character %c", ch);
                break;

            case '.':
                TOKEN_START;
                TOKEN_CHAR(ch);
                lexer->state = SCAN_DOT;
                break;

            case '"':
                TOKEN_START;
                lexer->after_str = SCAN_EXPR;
                lexer->state = SCAN_STR;
                break;

            default:
                TOKEN_START;
                if (ch >= '1' && ch <= '9') {
                    TOKEN_CHAR(ch);
                    lexer->int_value = ch - '0';
                    lexer->digit_count = 1;
                    lexer->state = SCAN_DECIMAL;
                } else if (is_symbol_char(ch)) {
                    lexer->state = SCAN_SYMBOL;
                    TOKEN_CHAR(ch);
                } else {
                    lexer->state = SCAN_IDENTIFIER;
                    TOKEN_CHAR_LOWER();
                }
                break;
            }
            break;

        case SCAN_AFTER_EXPR:
            if (lexer->arg_mode == ARG_RAW)
                end_arg_segment(lexer, ch);
            else {
                switch (ch) {
                case '\n':
                case ';':
                    end_arg_segment(lexer, ch);
                    break;

                CASE_SPACE
                    if (lexer->arg_mode == ARG_EXTENDED && !lexer->in_arg_value)
                        lexer->state = SCAN_AFTER_EXTENDED_KEY;
                    else
                        end_arg_segment(lexer, ch);
                    break;

                default:
                    if (lexer->arg_mode == ARG_TRADITIONAL) {
                        if (lexer->in_arg_value) {
                            if (ch != '"' && ch != ENTER_EXPR)
                                emit_bridge(lexer);
                            lexer->state = SCAN_ARG_VALUE;
                        } else
                            lexer->state = SCAN_AFTER_TRADITIONAL_KEY;
                    } else {
                        if (ch != '"' && ch != ENTER_EXPR)
                            emit_bridge(lexer);
                        if (lexer->in_arg_value)
                            lexer->state = SCAN_ARG_VALUE;
                        else
                            lexer->state = SCAN_EXTENDED_KEY;
                    }
                    BACK_UP;
                    break;
                }
            }
            break;

        case SCAN_SYMBOL:
            if (!lexer->token_length
                || (lexer->token_length == 1
                    && continue_symbol(*lexer->token_str, ch)))
            {
                TOKEN_CHAR(ch);
            } else {
                if (!emit_symbol(lexer))
                    break;
                lexer->state = SCAN_EXPR;
                BACK_UP;
            }
            break;

        case SCAN_IDENTIFIER:
            if (is_ident_char(ch))
                TOKEN_CHAR_LOWER();
            else {
                if (!emit_keyword_or_identifier(lexer))
                    break;
                if (ch == '.') {
                    TOKEN_CHAR(ch);
                    lexer->state = SCAN_DOT;
                    break;
                } else
                    lexer->state = SCAN_EXPR;
                BACK_UP;
            }
            break;

        case SCAN_STR:
            switch (ch) {
            case '\\':
                lexer->state = SCAN_STR_ESCAPE;
                break;

            case '"':
                if (emit_str(lexer))
                    lexer->state = lexer->after_str;
                break;

            case '\n':
                ERROR("Unterminated string");
                free_token(lexer);
                lexer->state = SCAN_NEWLINE;
                break;

            default:
                TOKEN_CHAR(ch);
                break;
            }
            break;

        case SCAN_STR_ESCAPE:
            switch (ch) {
            CASE_STR_ESC('a', 0x07);
            CASE_STR_ESC('b', 0x08);
            CASE_STR_ESC('e', 0x1b);
            CASE_STR_ESC('f', 0x0c);
            CASE_STR_ESC('n', 0x0a);
            CASE_STR_ESC('r', 0x0d);
            CASE_STR_ESC('t', 0x09);
            CASE_STR_ESC('v', 0x0b);
            CASE_STR_ESC('\\', 0x5c);
            CASE_STR_ESC('\'', 0x27);
            CASE_STR_ESC('"', 0x22);
            CASE_STR_ESC('?', 0x3f);

            case 'x':
                lexer->int_value = 0;
                lexer->digit_count = 0;
                lexer->state = SCAN_STR_HEX;
                break;

            case 'u':
                lexer->int_value = 0;
                lexer->digit_count = 0;
                lexer->state = SCAN_STR_LOW_UNICODE;
                break;

            case 'U':
                lexer->int_value = 0;
                lexer->digit_count = 0;
                lexer->state = SCAN_STR_HIGH_UNICODE;
                break;

            case '\n':
                ERROR("Unterminated string");
                free_token(lexer);
                lexer->state = SCAN_NEWLINE;
                break;

            default:
                if (ch >= '0' && ch <= '9') {
                    lexer->int_value = 0;
                    lexer->digit_count = 0;
                    lexer->state = SCAN_STR_OCTAL;
                    BACK_UP;
                } else {
                    ERROR("Illegal string escape \\%c", ch);
                    free_token(lexer);
                }
                break;
            }
            break;

        case SCAN_STR_OCTAL:
            if (ch >= '0' && ch <= '7') {
                if (add_digit(lexer, ch - '0', 8, 255,
                              "Octal escape (\\nnn) exceeds byte value")
                    && lexer->digit_count == 3
                    && TOKEN_CHAR(lexer->int_value)
                )
                    lexer->state = SCAN_STR;
            } else if (ch == '8' || ch == '9') {
                ERROR("Illegal digit in octal escape (\\nnn)");
                free_token(lexer);
            } else {
                if (TOKEN_CHAR(lexer->int_value))
                    lexer->state = SCAN_STR;
                BACK_UP;
            }
            break;

        case SCAN_STR_HEX:
            digit_value = hex_digit_to_int(ch);
            if (digit_value == -1) {
                if (!lexer->digit_count) {
                    ERROR("Hex string escape (\\x) requires at least one "
                          "digit");
                    free_token(lexer);
                    break;
                }
                if (TOKEN_CHAR(lexer->int_value))
                    lexer->state = SCAN_STR;
                BACK_UP;
            }else if (!add_digit(lexer, digit_value, 16, 255,
                                 "Hex escape exceeds byte value"))
                lexer->state = SCAN_ERROR;
            break;

        case SCAN_STR_LOW_UNICODE:
            digit_value = hex_digit_to_int(ch);
            if (digit_value == -1) {
                ERROR("Low unicode escape (\\u) requires exactly four "
                      "digits");
                free_token(lexer);
                break;
            }
            add_safe_digit(lexer, digit_value, 16);
            if (lexer->digit_count == 4
                && add_str_wchar(lexer)
            )
                lexer->state = SCAN_STR;
            break;

        case SCAN_STR_HIGH_UNICODE:
            digit_value = hex_digit_to_int(ch);
            if (digit_value == -1) {
                ERROR("High unicode escape (\\U) requires exactly eight "
                      "digits");
                free_token(lexer);
                break;
            }
            if (add_digit(lexer, digit_value, 16, UNICODE_MAX,
                      "High unicode escape (\\U) exceeds unicode value")
                && lexer->digit_count == 8
                && add_str_wchar(lexer)
            )
                lexer->state = SCAN_STR;
            break;

        case SCAN_NUMBER_BASE:
            switch (ch) {
            case 'b':
            case 'B':
                free_token(lexer);
                lexer->int_value = 0;
                lexer->digit_count = 0;
                lexer->state = SCAN_BINARY;
                break;

            case 'x':
            case 'X':
                TOKEN_CHAR(ch);
                lexer->int_value = 0;
                lexer->digit_count = 0;
                lexer->state = SCAN_HEX;
                break;

            case '.':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_DECIMAL_FRACTION;
                break;

            case 'e':
            case 'E':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_DECIMAL_EXPONENT_SIGN;
                break;

            default:
                if (ch >= '0' && ch <= '9') {
                    free_token(lexer);
                    lexer->int_value = 0;
                    lexer->state = SCAN_OCTAL;
                    BACK_UP;
                } else {
                    free_token(lexer);
                    if (emit_int(lexer, 0))
                        lexer->state = SCAN_EXPR;
                    BACK_UP;
                }
                break;
            }
            break;

        case SCAN_DECIMAL:
            switch (ch) {
            case '.':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_DECIMAL_FRACTION;
                break;

            case 'e':
            case 'E':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_DECIMAL_EXPONENT_SIGN;
                break;

            default:
                if (ch >= '0' && ch <= '9') {
                    TOKEN_CHAR(ch);
                    if (digit_exceeds(lexer, ch - '0', 10, INT64_MAX)) {
                        lexer->state = SCAN_DECIMAL_FLOAT;
                    } else
                        add_safe_digit(lexer, ch - '0', 10);
                } else {
                    free_token(lexer);
                    if (emit_int(lexer, lexer->int_value))
                        lexer->state = SCAN_EXPR;
                    BACK_UP;
                }
                break;
            }
            break;

        case SCAN_HEX:
            switch (ch) {
            case '.':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_HEX_FRACTION;
                break;

            case 'p':
            case 'P':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_HEX_EXPONENT_SIGN;
                break;

            default:
                digit_value = hex_digit_to_int(ch);
                if (digit_value != -1) {
                    TOKEN_CHAR(ch);
                    if (digit_exceeds(lexer, digit_value, 16, INT64_MAX)) {
                        lexer->state = SCAN_HEX_FLOAT;
                    } else
                        add_safe_digit(lexer, digit_value, 16);
                } else {
                    free_token(lexer);
                    if (EMIT_INT())
                        lexer->state = SCAN_EXPR;
                    BACK_UP;
                }
                break;
            }
            break;

        case SCAN_BINARY:
            if (ch == '0' || ch == '1') {
                add_digit(lexer, ch - '0', 2, INT64_MAX,
                    "Binary literal exceeds maximum value");
            } else if (ch == '.') {
                ERROR("Fractional binary literals not allowed");
            } else if (ch >= '2' && ch <= '9') {
                ERROR("Illegal binary digit %c", ch);
            } else {
                if (EMIT_INT())
                    lexer->state = SCAN_EXPR;
                BACK_UP;
            }
            break;

        case SCAN_OCTAL:
            if (ch >= '0' && ch <= '7') {
                add_digit(lexer, ch - '0', 8, INT64_MAX,
                    "Octal literal exceeds maximum value");
            } else if (ch == '.') {
                ERROR("Fractional octal literals not allowed");
            } else if (ch == '8' || ch == '9') {
                ERROR("Illegal octal digit %c", ch);
            } else {
                if (EMIT_INT())
                    lexer->state = SCAN_EXPR;
                BACK_UP;
            }
            break;

        case SCAN_DOT:
            if (ch >= '0' && ch <= '9') {
                TOKEN_CHAR(ch);
                lexer->state = SCAN_DECIMAL_FRACTION;
            } else {
                if (!emit_symbol(lexer))
                    break;
                lexer->state = SCAN_EXPR;
                BACK_UP;
            }
            break;

        case SCAN_DECIMAL_FLOAT:
            switch (ch) {
            case '.':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_DECIMAL_FRACTION;
                break;

            case 'e':
            case 'E':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_DECIMAL_EXPONENT_SIGN;
                break;

            default:
                if (ch >= '0' && ch <= '9')
                    TOKEN_CHAR(ch);
                else {
                    if (emit_float(lexer))
                        lexer->state = SCAN_EXPR;
                    BACK_UP;
                }
                break;
            }
            break;

        case SCAN_DECIMAL_FRACTION:
            switch (ch) {
            case 'e':
            case 'E':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_DECIMAL_EXPONENT_SIGN;
                break;

            default:
                if (ch >= '0' && ch <= '9')
                    TOKEN_CHAR(ch);
                else {
                    if (emit_float(lexer))
                        lexer->state = SCAN_EXPR;
                    BACK_UP;
                }
                break;
            }
            break;

        case SCAN_DECIMAL_EXPONENT_SIGN:
            if (ch == '-')
                TOKEN_CHAR(ch);
            else
                BACK_UP;
            lexer->state = SCAN_DECIMAL_EXPONENT;
            break;

        case SCAN_DECIMAL_EXPONENT:
            if (ch >= '0' && ch <= '9')
                TOKEN_CHAR(ch);
            else if (!lexer->digit_count) {
                free_token(lexer);
                ERROR("No digits after decimal exponent delimiter");
            } else {
                if (emit_float(lexer))
                    lexer->state = SCAN_EXPR;
                BACK_UP;
            }
            break;

        case SCAN_HEX_FLOAT:
            switch (ch) {
            case '.':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_HEX_FRACTION;
                break;

            case 'p':
            case 'P':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_HEX_EXPONENT_SIGN;
                break;

            default:
                digit_value = hex_digit_to_int(ch);
                if (digit_value != -1)
                    TOKEN_CHAR(ch);
                else {
                    if (emit_float(lexer))
                        lexer->state = SCAN_EXPR;
                    BACK_UP;
                }
                break;
            }
            break;

        case SCAN_HEX_FRACTION:
            switch (ch) {
            case 'p':
            case 'P':
                TOKEN_CHAR(ch);
                lexer->state = SCAN_HEX_EXPONENT_SIGN;
                break;

            default:
                digit_value = hex_digit_to_int(ch);
                if (digit_value  != -1)
                    TOKEN_CHAR(ch);
                else {
                    if (emit_float(lexer))
                        lexer->state = SCAN_EXPR;
                    BACK_UP;
                }
                break;
            }
            break;

        case SCAN_HEX_EXPONENT_SIGN:
            if (ch == '-')
                TOKEN_CHAR(ch);
            else
                BACK_UP;
            lexer->state = SCAN_HEX_EXPONENT;
            break;

        case SCAN_HEX_EXPONENT:
            digit_value = hex_digit_to_int(ch);
            if (digit_value != -1)
                TOKEN_CHAR(ch);
            else if (!lexer->digit_count) {
                free_token(lexer);
                ERROR("No digits after hex exponent delimiter");
            } else {
                emit_float(lexer);
                lexer->state = SCAN_EXPR;
                BACK_UP;
            }
            break;

        default:
            ERROR("Internal: Unknown lexer state %d", lexer->state);
            break;
        }
        if (ch == '\n') {
            lexer->line++;
            lexer->column = 1;
        } else
            lexer->column++;
    }
}

void gcode_lexer_finish(GCodeLexer* lexer) {
    // A final newline will flush any dangling statement and have no effect
    // otherwise
    if (lexer->state != SCAN_NEWLINE)
        gcode_lexer_scan(lexer, "\n", 1);
}

void gcode_lexer_reset(GCodeLexer* lexer) {
    lexer->state = SCAN_NEWLINE;
    lexer->token_length = 0;
    lexer->line = 1;
    lexer->column = 1;
}

void gcode_lexer_delete(GCodeLexer* lexer) {
    if (!lexer)
        return;
    free(lexer->token_str);
    free(lexer->error_str);
    free(lexer);
}
