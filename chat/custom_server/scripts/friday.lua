-- friday.lua - Friday AI 主逻辑 (LuaJIT FFI)
-- C++ 提供底层接口，Lua 控制全部业务流程
-- 修改此文件无需编译，保存后自动热更新

local VOLUME_THRESHOLD = 3000
local COOLDOWN_SEC = 8

-- ─── 主循环 (C++ 每帧调用) ────────────────────────────────────
function on_tick(idx)
    -- 录音，返回峰值
    local peak = mic_record(string.format("/tmp/m_%d.wav", idx))

    -- 存帧
    frame_save(string.format("/tmp/f_%d.jpg", idx))

    -- 决策: 首帧或说话才推理
    if idx == 1 or peak > VOLUME_THRESHOLD then
        ui_set_status("推理中...")
        local text = model_infer(
            string.format("/tmp/m_%d.wav", idx),
            string.format("/tmp/f_%d.jpg", idx),
            idx
        )
        if text ~= "" then
            print("[AI] " .. text)
            ui_add_text("🤖 " .. text)
        end
        tts_play()
        ui_set_status("运行中")
        sleep(COOLDOWN_SEC * 1000)
    else
        tts_play()
        ui_set_status("等待语音输入...")
    end
end

-- ─── 初始化 ────────────────────────────────────────────────────
print("[lua] Friday AI 脚本加载完成")
