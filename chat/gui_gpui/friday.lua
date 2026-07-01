package.cpath = "/usr/local/lualib/?.so;" .. package.cpath
local cjson = require("cjson")
local llama_url = "http://127.0.0.1:19080"
local tmpfile = os.tmpname()

local function http_post(path, body, timeout)
    timeout = timeout or 30
    local cmd = string.format(
        'curl -s -m %d -X POST "%s%s" -H "Content-Type: application/json" -d %q -o %s 2>/dev/null',
        timeout, llama_url, path, body, tmpfile)
    os.execute(cmd)
    local f = io.open(tmpfile, "r")
    local content = f and f:read("*a") or ""
    if f then f:close() end
    return content
end

local function extract_text(text)
    local results = {}
    for line in text:gmatch("[^\r\n]+") do
        if line:find('^data: ') then
            local s = line:sub(7)
            if s ~= "[DONE]" then
                local c = s:match('"content"%s*:%s*"([^"]*)"')
                if c and c ~= "" and c ~= "__IS_LISTEN__" and c ~= "__END_OF_TURN__" then
                    table.insert(results, c)
                end
            end
        end
    end
    return results
end

io.stderr:write("[friday] 开始\n"); io.stderr:flush()

local idx = 0
while true do
    idx = idx + 1
    io.stderr:write("[friday] frame " .. idx .. "\n"); io.stderr:flush()

    -- 抓帧
    os.execute("ffmpeg -f v4l2 -i /dev/video0 -vframes 1 -y /tmp/f.jpg 2>/dev/null")
    -- 录音
    os.execute("ffmpeg -f alsa -i default -t 1 -y /tmp/m.wav 2>/dev/null")

    -- prefill
    local prefill_json = cjson.encode({
        audio_path_prefix = "/tmp/m.wav",
        img_path_prefix = "/tmp/f.jpg",
        cnt = idx,
    }):gsub("\\/", "/")
    http_post("/v1/stream/prefill", prefill_json, 10)

    -- decode
    local resp = http_post("/v1/stream/decode", '{"debug_dir":"/tmp/omni_out2","stream":true}', 5)

    for _, c in ipairs(extract_text(resp)) do
        io.stderr:write("[AI] " .. c .. "\n"); io.stderr:flush()
    end

    os.execute("sleep 0.3")
end
