-- monitor.lua - 默认监控脚本
-- 用户可自由修改，热更新：lua_load_script("scripts/monitor.lua")

function on_frame()
    -- 每帧回调，返回 false 跳过 AI 分析
    return true
end

function on_ai_response(text)
    if text == "" then
        return
    end

    print("[AI] " .. text)

    -- 示例: 检测关键词触发动作
    if text:find("异常") or text:find("报警") then
        local ts = os.date("%Y%m%d_%H%M%S")
        alert_save_snapshot("/tmp/alert_" .. ts .. ".jpg")
        notify_send("发现异常: " .. text)
    end

    if text:find("温度") then
        -- 用户可在此扩展：解析温度数值，控制空调等
        notify_send("温度异常")
    end

    if text:find("有人") or text:find("人员") then
        notify_send("检测到人员活动")
    end
end

function on_tick()
    -- 每秒触发，可用于健康检查
end
