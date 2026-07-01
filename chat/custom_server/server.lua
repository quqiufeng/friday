-- server.lua - MiniCPM-o 全双工监控 (纯 LuaJIT)
-- 摄像头 → 模型推理 → AI 语音播报

package.path = "/usr/local/lualib/?.lua;" .. package.path
package.cpath = "/usr/local/lualib/?.so;" .. package.cpath

local ffi = require("ffi")
local bit = require("bit")
local cjson = require("cjson")

ffi.cdef[[
    typedef struct { int _; } FILE;
    struct sockaddr_in { unsigned short sin_family; unsigned short sin_port;
        unsigned int sin_addr; char sin_zero[8]; };
    int system(const char *cmd);
    int usleep(unsigned int usec);
    int remove(const char *path);
    int socket(int domain, int type, int protocol);
    int connect(int fd, const struct sockaddr *addr, int addrlen);
    int send(int fd, const void *buf, size_t len, int flags);
    int recv(int fd, void *buf, size_t len, int flags);
    int close(int fd);
    unsigned int inet_addr(const char *cp);
    unsigned short htons(unsigned short hostshort);
    FILE *popen(const char *cmd, const char *mode);
    int pclose(FILE *stream);
    size_t fread(void *ptr, size_t size, size_t n, FILE *s);
    FILE *fopen(const char *path, const char *mode);
    size_t fwrite(const void *p, size_t s, size_t n, FILE *f);
    int fclose(FILE *f);
]]

local C = ffi.load("c")

-- ─── 工具函数 ──────────────────────────────────────────────────
local function le32(v)
    return string.char(bit.band(v, 0xFF), bit.band(bit.rshift(v, 8), 0xFF),
                       bit.band(bit.rshift(v, 16), 0xFF), bit.band(bit.rshift(v, 24), 0xFF))
end

local function le16(v)
    return string.char(bit.band(v, 0xFF), bit.band(bit.rshift(v, 8), 0xFF))
end

-- ─── 摄像头 ──────────────────────────────────────────────────
local function camera_capture(dev)
    dev = dev or "/dev/video0"
    local cmd = "ffmpeg -f v4l2 -video_size 640x480 -i " .. dev
        .. " -vframes 1 -f image2pipe - 2>/dev/null"
    local f = C.popen(cmd, "r")
    if f == nil then return nil end
    local buf = ffi.new("char[?]", 256 * 1024)
    local n = C.fread(buf, 1, 256 * 1024, f)
    C.pclose(f)
    if n <= 0 then return nil end
    return ffi.string(buf, n)
end

-- ─── HTTP (raw socket) ───────────────────────────────────────
local function http(method, host, port, path, body, timeout)
    timeout = timeout or 30
    local fd = C.socket(2, 1, 0)
    if fd < 0 then return nil, "socket failed" end

    local addr = ffi.new("struct sockaddr_in")
    addr.sin_family = 2
    addr.sin_port = C.htons(port)
    addr.sin_addr = C.inet_addr(host)

    if C.connect(fd, ffi.cast("const struct sockaddr *", addr), ffi.sizeof(addr)) < 0 then
        C.close(fd)
        return nil, "connect failed"
    end

    local req = method .. " " .. path .. " HTTP/1.1\r\n"
        .. "Host: " .. host .. ":" .. port .. "\r\n"
        .. "Content-Type: application/json\r\n"
        .. "Content-Length: " .. #body .. "\r\n"
        .. "Connection: close\r\n\r\n" .. body

    local p = ffi.new("char[?]", #req)
    ffi.copy(p, req, #req)
    C.send(fd, p, #req, 0)

    local buf = ffi.new("char[?]", 1048576)
    local total = 0
    while true do
        local r = C.recv(fd, buf + total, 1048576 - total, 0)
        if r <= 0 then break end
        total = total + r
    end
    C.close(fd)

    if total <= 0 then return nil, "recv failed" end

    local raw = ffi.string(buf, total)
    local _, e = raw:find("\r\n\r\n")
    if not e then return nil, "invalid response" end
    return raw:sub(e + 1)
end

-- ─── 播放 WAV ───────────────────────────────────────────────
local function play_wav(path)
    C.system("ffplay -nodisp -autoexit -loglevel quiet '" .. path .. "' &")
end

-- ─── 保存文件 ───────────────────────────────────────────────
local function save_file(path, data)
    local f = C.fopen(path, "wb")
    if f ~= nil then
        C.fwrite(data, 1, #data, f)
        C.fclose(f)
        return true
    end
    return false
end

-- ─── 生成静音 WAV (16-bit PCM, 16kHz, 1s) ─────────────────
local function make_silence_wav(path, duration)
    duration = duration or 1.0
    local sr = 16000
    local samples = math.floor(sr * duration)
    local data_size = samples * 2  -- 16-bit mono
    local header = "RIFF" .. le32(36 + data_size) .. "WAVE"
        .. "fmt " .. le32(16) .. le16(1) .. le16(1)
        .. le32(sr) .. le32(sr * 2) .. le16(2) .. le16(16)
        .. "data" .. le32(data_size)
    save_file(path, header .. string.rep(string.char(0), data_size))
end

-- ─── 配置 ─────────────────────────────────────────────────────
local CFG = {
    port = 19080,
    fps = 1,
    model = "/data/models/MiniCPM-o-4_5-gguf/MiniCPM-o-4_5-Q4_K_M.gguf",
    dir = "/data/models/MiniCPM-o-4_5-gguf",
    output = "/tmp/omni_out",
}
local HOST = "127.0.0.1"

-- ─── 启动 llama-server ───────────────────────────────────────
local function start_llama()
    C.system("pkill -9 llama-server 2>/dev/null")
    C.system("sleep 1")

    local cmd = string.format(
        "/opt/llama.cpp-omni/build/bin/llama-server --host 127.0.0.1 --port %d"
        .. " --model '%s' --ctx-size 8192 --n-gpu-layers 99"
        .. " --repeat-penalty 1.05 --temp 0.7 > /tmp/llama-server.log 2>&1 &",
        CFG.port, CFG.model)
    C.system(cmd)

    print("[模型] 启动中...")
    for i = 1, 90 do
        local r, err = http("GET", HOST, CFG.port, "/health", "", 1)
        if r then
            print("[模型] 就绪 (" .. i .. "s)")
            return true
        end
        io.write(".")
        io.flush()
        C.usleep(1000000)
    end
    print("\n[错误] llama-server 启动超时")
    return false
end

-- ─── omni_init ───────────────────────────────────────────────
local function omni_init()
    local body = cjson.encode({
        media_type = 2,
        use_tts = true,
        duplex_mode = true,
        model_dir = CFG.dir,
        tts_bin_dir = CFG.dir .. "/tts",
        tts_gpu_layers = 100,
        token2wav_device = "gpu:0",
        output_dir = CFG.output,
        voice_clone_prompt = '<|im_start|>system\n你是一个监控管理员，持续观察摄像头画面。正常情况下保持静默观察，不要主动说话。发现异常情况时简洁描述当前画面。不要主动中断对话。\n<|audio_start|>',
        assistant_prompt = '<|audio_end|><|im_end|>\n',
    })

    print("[模型] omni_init ...")
    local r, err = http("POST", HOST, CFG.port, "/v1/stream/omni_init", body, 300)
    if not r then
        print("[错误] omni_init 失败: " .. (err or "unknown"))
        return false
    end
    print("[模型] 完成 (" .. #r .. " bytes)")
    return true
end

-- ─── 播放最新 TTS 音频 ──────────────────────────────────────
local function play_latest_tts()
    local cmd = "ls -t " .. CFG.output .. "/round_*/tts_wav/wav_*.wav 2>/dev/null | head -1"
    local f = C.popen(cmd, "r")
    if f == nil then return end

    local buf = ffi.new("char[?]", 1024)
    local n = C.fread(buf, 1, 1024, f)
    C.pclose(f)

    if n > 0 then
        local latest = ffi.string(buf, n):gsub("\n", "")
        if latest ~= "" then play_wav(latest) end
    end
end

-- ─── 主程序 ───────────────────────────────────────────────
print("========================================")
print("  MiniCPM-o 监控 (语音输出)")
print("========================================")

if not start_llama() then return end
if not omni_init() then return end

print("[摄像头] 打开 /dev/video0 ...")
local test_jpeg = camera_capture()
if not test_jpeg then
    print("[错误] 摄像头不可用")
    return
end
print("[摄像头] 就绪 (" .. #test_jpeg .. " bytes)")

print("[循环] 开始 (Ctrl+C 停止)\n")

local idx = 0
while true do
    idx = idx + 1
    local jpeg = camera_capture()

    if jpeg then
        local ts = tostring(os.time())
        local img = "/tmp/f_" .. ts .. ".jpg"
        local wav = "/tmp/s_" .. ts .. ".wav"

        save_file(img, jpeg)
        make_silence_wav(wav, 1.0)

        local ok, err = http("POST", HOST, CFG.port, "/v1/stream/prefill",
            '{"audio_path_prefix":"' .. wav .. '","img_path_prefix":"' .. img .. '","cnt":' .. idx .. '}', 10)

        if ok then
            local resp, dec_err = http("POST", HOST, CFG.port, "/v1/stream/decode",
                '{"debug_dir":"' .. CFG.output .. '","stream":true}', 120)

            if resp then
                for line in resp:gmatch("data: ([^\r\n]+)") do
                    if line ~= "[DONE]" then
                        local parse_ok, msg = pcall(cjson.decode, line)
                        if parse_ok and msg then
                            if msg.content and #msg.content > 0 then
                                print("[AI] " .. msg.content)
                            end
                            if msg.stop then break end
                        end
                    end
                end
            end
        end

        play_latest_tts()
        C.remove(img)
        C.remove(wav)
    else
        print("[摄像头] 抓帧失败，等待...")
        C.usleep(500000)
    end

    C.usleep(1000000 / CFG.fps)
end
