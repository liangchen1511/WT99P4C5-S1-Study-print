/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soti_print_sections.hpp"

#include "cJSON.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

struct Section {
    std::string name;
    std::string body;
};

static void copy_trunc(char *dst, size_t cap, const std::string &src)
{
    if (dst == nullptr || cap == 0) {
        return;
    }
    size_t n = src.size();
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

static std::vector<Section> parse_section_map(const char *text)
{
    std::vector<Section> out;
    if (text == nullptr) {
        return out;
    }
    const char *p = text;
    while (*p != '\0') {
        if (p[0] == 0xE3 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x90) {
            const char *name_start = p + 3;
            const char *name_end = name_start;
            while (*name_end != '\0') {
                if (name_end[0] == 0xE3 && (unsigned char)name_end[1] == 0x80 && (unsigned char)name_end[2] == 0x91) {
                    break;
                }
                name_end++;
            }
            if (*name_end == '\0') {
                break;
            }
            Section sec;
            sec.name.assign(name_start, (size_t)(name_end - name_start));
            p = name_end + 3;
            while (*p == '\n' || *p == '\r') {
                p++;
            }
            const char *body_start = p;
            while (*p != '\0') {
                if (p[0] == 0xE3 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x90) {
                    break;
                }
                p++;
            }
            sec.body.assign(body_start, (size_t)(p - body_start));
            while (!sec.body.empty() && (sec.body.back() == '\n' || sec.body.back() == '\r' || sec.body.back() == ' ')) {
                sec.body.pop_back();
            }
            if (!sec.name.empty()) {
                out.push_back(std::move(sec));
            }
            continue;
        }
        p++;
    }
    return out;
}

static std::string join_sections(const std::vector<Section> &sm, const char *const *names, size_t n_names)
{
    std::string out;
    for (size_t i = 0; i < n_names; i++) {
        for (const auto &sec : sm) {
            if (sec.name != names[i] || sec.body.empty()) {
                continue;
            }
            if (!out.empty()) {
                out += "\n\n";
            }
            out += "【";
            out += sec.name;
            out += "】\n";
            out += sec.body;
            break;
        }
    }
    return out;
}

static void fallback_split(const char *text, std::string &q, std::string &wa)
{
    q.clear();
    wa.clear();
    if (text == nullptr) {
        return;
    }
    const char *split = strstr(text, "\n\n");
    if (split == nullptr) {
        q = text;
        return;
    }
    q.assign(text, (size_t)(split - text));
    wa = split + 2;
    while (!q.empty() && (q.back() == '\n' || q.back() == '\r')) {
        q.pop_back();
    }
    while (!wa.empty() && (wa.front() == '\n' || wa.front() == '\r')) {
        wa.erase(wa.begin());
    }
}

static void assign_sections(soti_print_sections_t *out, const std::string &q, const std::string &wa)
{
    if (out == nullptr) {
        return;
    }
    if (!q.empty()) {
        copy_trunc(out->question, sizeof(out->question), q);
        out->has_question = true;
    }
    if (!wa.empty()) {
        copy_trunc(out->with_answer, sizeof(out->with_answer), wa);
        out->has_with_answer = true;
    }
}

} // namespace

extern "C" void soti_print_sections_clear(soti_print_sections_t *out)
{
    if (out == nullptr) {
        return;
    }
    memset(out, 0, sizeof(*out));
}

void soti_print_sections_from_json(const cJSON *print_obj, soti_print_sections_t *out)
{
    soti_print_sections_clear(out);
    if (out == nullptr || print_obj == nullptr || !cJSON_IsObject(print_obj)) {
        return;
    }
    const cJSON *q = cJSON_GetObjectItemCaseSensitive(print_obj, "question");
    const cJSON *wa = cJSON_GetObjectItemCaseSensitive(print_obj, "with_answer");
    if (cJSON_IsString(q) && q->valuestring != nullptr && q->valuestring[0] != '\0') {
        copy_trunc(out->question, sizeof(out->question), q->valuestring);
        out->has_question = true;
    }
    if (cJSON_IsString(wa) && wa->valuestring != nullptr && wa->valuestring[0] != '\0') {
        copy_trunc(out->with_answer, sizeof(out->with_answer), wa->valuestring);
        out->has_with_answer = true;
    }
}

void soti_split_print_sections(const char *display_utf8, soti_mode_t mode, soti_print_sections_t *out)
{
    soti_print_sections_clear(out);
    if (out == nullptr || display_utf8 == nullptr || display_utf8[0] == '\0') {
        return;
    }
    if (strncmp(display_utf8, "（演示）", strlen("（演示）")) == 0 ||
        strncmp(display_utf8, "搜题失败", strlen("搜题失败")) == 0) {
        return;
    }

    const auto sm = parse_section_map(display_utf8);
    std::string q;
    std::string wa;

    switch (mode) {
    case SOTI_MODE_DAILY: {
        static const char *k[] = {"每日一句"};
        std::string block = join_sections(sm, k, 1);
        if (block.empty()) {
            block = display_utf8;
        }
        assign_sections(out, block, block);
        return;
    }
    case SOTI_MODE_TRANSLATE: {
        static const char *kq[] = {"原文"};
        static const char *kwa[] = {"译文", "说明"};
        q = join_sections(sm, kq, 1);
        wa = join_sections(sm, kwa, 2);
        break;
    }
    case SOTI_MODE_TUTOR: {
        static const char *kq[] = {"题目"};
        static const char *kwa[] = {"考点", "分步讲解", "答案", "易错提醒"};
        q = join_sections(sm, kq, 1);
        wa = join_sections(sm, kwa, 4);
        break;
    }
    case SOTI_MODE_GRADE: {
        static const char *kq[] = {"题目"};
        static const char *kwa[] = {"批改结果", "说明"};
        q = join_sections(sm, kq, 1);
        wa = join_sections(sm, kwa, 2);
        if (wa.empty() && sm.empty()) {
            fallback_split(display_utf8, q, wa);
        }
        if (wa.empty()) {
            wa = display_utf8;
        }
        break;
    }
    case SOTI_MODE_SUMMARY: {
        static const char *kq[] = {"标题"};
        static const char *kwa[] = {"要点", "关键词"};
        q = join_sections(sm, kq, 1);
        wa = join_sections(sm, kwa, 2);
        break;
    }
    case SOTI_MODE_SOLVE:
    default: {
        static const char *kq[] = {"题目"};
        static const char *kwa[] = {"解题步骤", "答案"};
        q = join_sections(sm, kq, 1);
        wa = join_sections(sm, kwa, 2);
        break;
    }
    }

    if (q.empty() && wa.empty() && sm.empty()) {
        fallback_split(display_utf8, q, wa);
    }
    assign_sections(out, q, wa);
}
