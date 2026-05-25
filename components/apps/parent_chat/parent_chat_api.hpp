/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ParentChatMsg {
    int id = 0;
    double created_at = 0;
    std::string sender;
    std::string body;
};

struct ParentChatUnread {
    int parent_unread = 0;
    int child_unread = 0;
    int latest_id = 0;
};

bool parent_chat_build_api_url(char *dst, size_t cap, const char *path);

bool parent_chat_http_get(const char *path_with_query, std::string &out_body, int *out_status);

bool parent_chat_http_post_json(const char *path, const char *json_body, std::string &out_body, int *out_status);

bool parent_chat_fetch_recent(std::vector<ParentChatMsg> &out, ParentChatUnread *unread);

bool parent_chat_poll(int since_id, std::vector<ParentChatMsg> &out, ParentChatUnread *unread);

bool parent_chat_send(const char *body, const char *client_msg_id, ParentChatMsg *out_msg);

bool parent_chat_mark_read_child(int up_to_id);

bool parent_chat_fetch_unread(ParentChatUnread *unread);

void parent_chat_bg_poll_start(void);

void parent_chat_bg_poll_stop(void);

/** 搜题上传等占用 SDIO/堆时暂停后台 poll，避免 std::bad_alloc → abort。 */
void parent_chat_bg_pause(bool paused);

bool parent_chat_bg_poll_is_running(void);

// Wait for bg poll task to exit (max_wait_ms total). Task may take up to ~12s to exit.
void parent_chat_bg_poll_wait_stop(int max_wait_ms);
