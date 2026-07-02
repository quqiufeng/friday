-- friday.lua - Friday AI 业务逻辑
-- 修改此文件后重启程序即可生效，无需重新编译

local VOLUME_THRESHOLD = 3000      -- 语音检测阈值
local INIT_SILENCE_MS = 100         -- 初始化用静音时长(毫秒)
local PROMPT = "Streaming Omni Conversation."

-- ─── 初始化回调 ────────────────────────────────────────────────
function on_init()
    print("[lua] 脚本加载完成")
    return PROMPT, INIT_SILENCE_MS
end

-- ─── 是否推理 ──────────────────────────────────────────────────
function on_should_infer(idx, peak)
    if idx == 1 then return true end
    if peak > VOLUME_THRESHOLD then return true end
    return false
end

-- ─── AI 文字格式化 ─────────────────────────────────────────────
function on_ai_format(text)
    return "🤖 " .. text
end

-- ─── 状态文字 ──────────────────────────────────────────────────
function on_status_idle()
    return "等待语音输入..."
end

function on_status_infer()
    return "推理中..."
end

function on_status_ready()
    return "运行中"
end
