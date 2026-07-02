-- Friday AI 回复处理脚本 (可热更新)
-- on_reply(reply_text, log_path) 每次 AI 说完话时调用
function on_reply(reply_text, log_path)
    print("[Friday] 收到回复: " .. reply_text:sub(1, 80) .. "...")
    
    -- TODO: 转发到 opencode 或其他模型处理
    -- local cjson = require("cjson")
    -- 调用本地 API...
end
