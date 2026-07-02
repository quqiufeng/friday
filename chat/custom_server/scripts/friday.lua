-- friday.lua - Friday AI 主逻辑
-- C++ 提供底层接口，Lua 控制全部业务流程
-- 修改此文件无需编译，保存后自动热更新

local COOLDOWN = 6       -- 回复后冷却秒数
local last_infer = 0

-- ─── 主循环 ────────────────────────────────────────────────────
function on_tick(idx)
    local audio = string.format("/tmp/m_%d.wav", idx)
    local img   = string.format("/tmp/f_%d.jpg", idx)

    -- 从环形缓冲取音频写 WAV + 存帧
    local peak = mic_record(audio)
    frame_save(img)

    -- 冷却检查
    local now = os.time()
    local voice = has_speech()
    local can_infer = (idx == 1) or (voice and now - last_infer > COOLDOWN)

    if can_infer then
        last_infer = now
        ui_set_status("推理中...")
        local text = model_infer(audio, img, idx)
        if text ~= "" then
            print("[AI] " .. text)
            ui_add_text("🤖 " .. text)
        end
        tts_play()
        ui_set_status("运行中")
    else
        if idx % 10 == 0 then
            print("[dbg] idx=" .. idx .. " voice=" .. tostring(voice) .. " peak=" .. peak)
        end
        tts_play()
        ui_set_status("等待语音输入...")
    end
end

print("[lua] Friday AI 加载完成 (冷却=" .. COOLDOWN .. "s)")
