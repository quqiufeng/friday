-- app.lua - Friday AI 桌面端业务逻辑
-- 通过 FFI 调用 Rust GPUI (libfriday_gui.so)
-- 热更新：修改此文件后即时生效，无需重启 GUI

package.cpath = "/opt/friday/chat/gui_gpui/?.so;/usr/local/lualib/?.so;" .. package.cpath
local ffi = require("ffi")
local cjson = require("cjson")

ffi.cdef[[
    void* gui_app_create(const char *config_json);
    void gui_app_free(void *app);
    int  gui_run(void *app);
    void gui_stop(void *app);
    void gui_on_user_message(void *app, void (*cb)(const char*, void*), void *userdata);
    void gui_stream_delta(void *app, const char *delta);
    void gui_set_status(void *app, const char *text);
    void gui_free_string(char *s);
]]

local gui = ffi.load("/opt/friday/chat/gui_gpui/target/release/libfriday_gui.so")

-- ── 状态 ───
local state = {
    model = "MiniCPM-o-4.5",
    tokens_used = 0,
    context_tokens = 8192,
}

-- ── 创建应用 ───
local config = cjson.encode({ model = state.model, title = "Friday AI" })
local app = gui.gui_app_create(config)

-- ── 事件回调 ───

local on_user_msg = ffi.cast("void (*)(const char*, void*)", function(text)
    local msg = ffi.string(text)
    print("[Lua] 用户消息: " .. msg)

    gui.gui_set_status(app, "推理中...")

    -- 模拟 AI 回复（实际应由推理引擎回调 gui_stream_delta）
    local response = "你好！我是 Friday，你的 AI 助手。你说的是：" .. msg
    for i = 1, #response, 3 do
        local chunk = response:sub(i, i + 2)
        gui.gui_stream_delta(app, chunk)
    end

    state.tokens_used = state.tokens_used + #msg + #response
    gui.gui_set_status(app, "就绪")
end)

gui.gui_on_user_message(app, on_user_msg, nil)

-- ── 初始化 GUI 状态 ───
gui.gui_set_status(app, "就绪")
gui.gui_stream_delta(app, "你好！我是 Friday AI。看着摄像头画面，随时为你服务。")

-- ── 启动 GUI（阻塞） ───
print("[Lua] Friday AI 启动...")

gui.gui_run(app)

-- 清理
on_user_msg:free()
gui.gui_app_free(app)
