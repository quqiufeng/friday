#pragma once
#include <string>

bool lua_init();
void lua_close();
bool lua_load_script(const std::string &path);
void lua_call_tick();
void lua_call_on_frame(const unsigned char *jpeg, size_t len, double timestamp);
void lua_call_on_ai_response(const std::string &text);
void lua_call_on_connect(int client_fd);
