/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soti_answer_format.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

static void append_ch(char *dst, size_t cap, size_t *len, char c)
{
    if (dst == nullptr || cap == 0 || len == nullptr || *len + 1 >= cap) {
        return;
    }
    dst[*len] = c;
    (*len)++;
    dst[*len] = '\0';
}

static void append_str(char *dst, size_t cap, size_t *len, const char *s)
{
    if (s == nullptr) {
        return;
    }
    while (*s != '\0' && *len + 1 < cap) {
        dst[*len] = *s++;
        (*len)++;
    }
    if (*len < cap) {
        dst[*len] = '\0';
    }
}

static void append_utf8_char(char *dst, size_t cap, size_t *len, const char *utf8, size_t n)
{
    if (utf8 == nullptr || n == 0 || *len + n >= cap) {
        return;
    }
    memcpy(dst + *len, utf8, n);
    *len += n;
    dst[*len] = '\0';
}

static void normalize_char(char *dst, size_t cap, size_t *len, const unsigned char *p, size_t char_len)
{
    if (char_len == 3 && p[0] == 0xE2) {
        if (p[1] == 0x80 && p[2] == 0x9C) {
            append_ch(dst, cap, len, '"');
            return;
        }
        if (p[1] == 0x80 && p[2] == 0x9D) {
            append_ch(dst, cap, len, '"');
            return;
        }
        if (p[1] == 0x80 && p[2] == 0x98) {
            append_ch(dst, cap, len, '\'');
            return;
        }
        if (p[1] == 0x80 && p[2] == 0x99) {
            append_ch(dst, cap, len, '\'');
            return;
        }
        if (p[1] == 0x80 && p[2] == 0x94) {
            append_str(dst, cap, len, "——");
            return;
        }
        if (p[1] == 0x80 && p[2] == 0xA6) {
            append_utf8_char(dst, cap, len, "\xe2\x80\xa6", 3);
            return;
        }
    }
    if (char_len <= cap - *len - 1) {
        memcpy(dst + *len, p, char_len);
        *len += char_len;
        dst[*len] = '\0';
    }
}

static size_t utf8_char_len(unsigned char c)
{
    if ((c & 0x80) == 0) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

static void skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t') {
        (*p)++;
    }
}

/** 豆包 OCR 常在数字间插空格：6 8 → 68（仅 ASCII 数字链）。 */
static void collapse_digit_spaces(char *s)
{
    if (s == nullptr) {
        return;
    }
    char *w = s;
    for (const char *r = s; *r != '\0';) {
        if (isdigit((unsigned char)*r)) {
            while (isdigit((unsigned char)*r)) {
                *w++ = *r++;
            }
            while (*r == ' ' && isdigit((unsigned char)r[1])) {
                r++;
            }
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static void copy_brace_content(char *dst, size_t cap, size_t *len, const char **p)
{
    if (**p != '{') {
        return;
    }
    (*p)++;
    int depth = 1;
    while (**p != '\0' && depth > 0) {
        if (**p == '{') {
            depth++;
        } else if (**p == '}') {
            depth--;
            if (depth == 0) {
                (*p)++;
                break;
            }
        }
        unsigned char c = (unsigned char)**p;
        size_t cl = utf8_char_len(c);
        normalize_char(dst, cap, len, (const unsigned char *)*p, cl);
        *p += cl;
    }
}

/** \boldsymbol68 或 \boldsymbol{68} */
static void copy_cmd_payload(char *dst, size_t cap, size_t *len, const char **p)
{
    skip_ws(p);
    if (**p == '{') {
        copy_brace_content(dst, cap, len, p);
        return;
    }
    while (**p != '\0' &&
           (isdigit((unsigned char)**p) || **p == '.' || **p == '-' || **p == '+' || **p == ' ')) {
        if (**p != ' ') {
            append_ch(dst, cap, len, **p);
        }
        (*p)++;
    }
}

static void handle_latex_command(char *dst, size_t cap, size_t *len, const char **p);

/** LaTeX 行内：\( ... \) 或 \[ ... \]（豆包常用，非 ( \ 顺序）。 */
static void strip_latex_delim_math(char *dst, size_t cap, size_t *len, const char **p)
{
    if (**p != '\\') {
        return;
    }
    char open = (*p)[1];
    if (open != '(' && open != '[') {
        return;
    }
    char close = (open == '(') ? ')' : ']';
    *p += 2;
    while (**p != '\0') {
        if (**p == '\\' && (*p)[1] == close) {
            *p += 2;
            break;
        }
        if (**p == '\\') {
            handle_latex_command(dst, cap, len, p);
            continue;
        }
        if (**p == '{' || **p == '}') {
            (*p)++;
            continue;
        }
        if (**p == '*') {
            append_ch(dst, cap, len, '*');
            (*p)++;
            continue;
        }
        unsigned char c = (unsigned char)**p;
        size_t cl = utf8_char_len(c);
        normalize_char(dst, cap, len, (const unsigned char *)*p, cl);
        *p += cl;
    }
}

static void strip_dollar_math(char *dst, size_t cap, size_t *len, const char **p)
{
    if (**p != '$') {
        return;
    }
    bool dbl = (*p)[1] == '$';
    *p += dbl ? 2 : 1;
    while (**p != '\0') {
        if (dbl && **p == '$' && (*p)[1] == '$') {
            *p += 2;
            break;
        }
        if (!dbl && **p == '$') {
            (*p)++;
            break;
        }
        if (**p == '\\') {
            handle_latex_command(dst, cap, len, p);
            continue;
        }
        if (**p == '{' || **p == '}') {
            (*p)++;
            continue;
        }
        unsigned char c = (unsigned char)**p;
        size_t cl = utf8_char_len(c);
        normalize_char(dst, cap, len, (const unsigned char *)*p, cl);
        *p += cl;
    }
}

static void handle_latex_command(char *dst, size_t cap, size_t *len, const char **p)
{
    if (**p != '\\') {
        return;
    }
    (*p)++;
    skip_ws(p);
    char cmd[32];
    size_t ci = 0;
    while (**p != '\0' && ci + 1 < sizeof(cmd) &&
           (isalpha((unsigned char)**p) || **p == '@' || **p == '*')) {
        cmd[ci++] = **p;
        (*p)++;
    }
    cmd[ci] = '\0';
    skip_ws(p);

    if (strcmp(cmd, "boldsymbol") == 0 || strcmp(cmd, "mathbf") == 0 || strcmp(cmd, "mathrm") == 0 ||
        strcmp(cmd, "text") == 0 || strcmp(cmd, "textbf") == 0 || strcmp(cmd, "textit") == 0) {
        copy_cmd_payload(dst, cap, len, p);
        return;
    }
    if (strcmp(cmd, "frac") == 0) {
        copy_cmd_payload(dst, cap, len, p);
        append_ch(dst, cap, len, '/');
        skip_ws(p);
        copy_cmd_payload(dst, cap, len, p);
        return;
    }
    if (strcmp(cmd, "times") == 0 || strcmp(cmd, "cdot") == 0) {
        append_ch(dst, cap, len, '*');
        return;
    }
    if (strcmp(cmd, "div") == 0) {
        append_ch(dst, cap, len, '/');
        return;
    }
    if (strcmp(cmd, "left") == 0 || strcmp(cmd, "right") == 0) {
        if (**p == '(' || **p == ')' || **p == '[' || **p == ']') {
            (*p)++;
        }
        return;
    }
    if (cmd[0] == '\0') {
        append_ch(dst, cap, len, '\\');
        return;
    }
    if (**p == '{') {
        copy_brace_content(dst, cap, len, p);
    }
}

static void strip_inline_math(char *dst, size_t cap, size_t *len, const char **p)
{
    if ((**p == '(' || **p == '[') && (*p)[1] == '\\') {
        *p += 2;
        while (**p != '\0') {
            if (**p == '\\' && ((*p)[1] == ')' || (*p)[1] == ']')) {
                *p += 2;
                break;
            }
            if (**p == '\\') {
                handle_latex_command(dst, cap, len, p);
                continue;
            }
            if (**p == '{' || **p == '}') {
                (*p)++;
                continue;
            }
            if (**p == '*') {
                append_ch(dst, cap, len, '*');
                (*p)++;
                continue;
            }
            unsigned char c = (unsigned char)**p;
            size_t cl = utf8_char_len(c);
            normalize_char(dst, cap, len, (const unsigned char *)*p, cl);
            *p += cl;
        }
    }
}

static void format_heading_line(char *dst, size_t cap, size_t *len, const char *line)
{
    const char *p = line;
    while (*p == '#') {
        p++;
    }
    while (*p == ' ') {
        p++;
    }
    if (*p == '\0') {
        return;
    }
    append_ch(dst, cap, len, '\n');
    append_utf8_char(dst, cap, len, "【", 3);
    while (*p != '\0' && *len + 1 < cap) {
        if (*p == '*' && p[1] == '*') {
            p += 2;
            continue;
        }
        unsigned char c = (unsigned char)*p;
        size_t cl = utf8_char_len(c);
        normalize_char(dst, cap, len, (const unsigned char *)p, cl);
        p += cl;
    }
    append_utf8_char(dst, cap, len, "】", 3);
    append_ch(dst, cap, len, '\n');
}

char *soti_format_answer_for_display(const char *raw)
{
    if (raw == nullptr) {
        return nullptr;
    }
    size_t cap = strlen(raw) * 2 + 256;
    if (cap < 2048) {
        cap = 2048;
    }
    if (cap > 16384) {
        cap = 16384;
    }
    char *dst = (char *)malloc(cap);
    if (dst == nullptr) {
        return nullptr;
    }
    dst[0] = '\0';
    size_t len = 0;

    const char *p = raw;
    while (*p != '\0' && len + 1 < cap) {
        if (*p == '\r') {
            p++;
            continue;
        }
        if (*p == '\n') {
            append_ch(dst, cap, &len, '\n');
            p++;
            continue;
        }
        if (*p == '#') {
            const char *line_end = strchr(p, '\n');
            if (line_end == nullptr) {
                format_heading_line(dst, cap, &len, p);
                break;
            }
            char line_buf[256];
            size_t n = (size_t)(line_end - p);
            if (n >= sizeof(line_buf)) {
                n = sizeof(line_buf) - 1;
            }
            memcpy(line_buf, p, n);
            line_buf[n] = '\0';
            format_heading_line(dst, cap, &len, line_buf);
            p = line_end + 1;
            continue;
        }
        if (*p == '*' && p[1] == '*') {
            p += 2;
            continue;
        }
        if (*p == '*' && p[1] != '*') {
            p++;
            continue;
        }
        if (*p == '\\' && (p[1] == '(' || p[1] == '[')) {
            strip_latex_delim_math(dst, cap, &len, &p);
            continue;
        }
        if (*p == '$') {
            strip_dollar_math(dst, cap, &len, &p);
            continue;
        }
        if (*p == '\\') {
            handle_latex_command(dst, cap, &len, &p);
            continue;
        }
        if ((*p == '(' || *p == '[') && p[1] == '\\') {
            strip_inline_math(dst, cap, &len, &p);
            continue;
        }
        if (*p == '{' || *p == '}') {
            p++;
            continue;
        }
        unsigned char c = (unsigned char)*p;
        size_t cl = utf8_char_len(c);
        normalize_char(dst, cap, &len, (const unsigned char *)p, cl);
        p += cl;
    }

    collapse_digit_spaces(dst);

    char *compact = (char *)malloc(cap);
    if (compact == nullptr) {
        return dst;
    }
    size_t j = 0;
    int nl = 0;
    for (size_t i = 0; dst[i] != '\0' && j + 1 < cap; i++) {
        if (dst[i] == '\n') {
            nl++;
            if (nl <= 2) {
                compact[j++] = '\n';
            }
        } else {
            nl = 0;
            compact[j++] = dst[i];
        }
    }
    compact[j] = '\0';
    free(dst);
    while (compact[0] == '\n' || compact[0] == ' ') {
        memmove(compact, compact + 1, strlen(compact));
    }
    return compact;
}
