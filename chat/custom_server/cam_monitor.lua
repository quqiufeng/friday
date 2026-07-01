-- cam_monitor.lua - 局域网摄像头扫描 + MiniCPM-o 视频双工推流 (纯 LuaJIT)
-- 零 Python 依赖，纯 LuaJIT FFI 实现
--
-- 用法:
--   luajit cam_monitor.lua                            # 自动扫描
--   luajit cam_monitor.lua -p '密码'                  # 自动扫描+密码
--   luajit cam_monitor.lua 192.168.1.53 -p '密码'     # 指定IP

package.path = "/usr/local/lualib/?.lua;" .. package.path
package.cpath = "/usr/local/lualib/?.so;" .. package.cpath

local ffi = require("ffi")
local bit = require("bit")
local cjson = require("cjson")
local crypto = ffi.load("crypto")

ffi.cdef[[
    // socket
    int socket(int domain, int type, int protocol);
    int connect(int fd, const struct sockaddr *addr, int addrlen);
    int close(int fd);
    ssize_t send(int fd, const void *buf, size_t len, int flags);
    ssize_t recv(int fd, void *buf, size_t len, int flags);
    unsigned int inet_addr(const char *cp);
    unsigned short htons(unsigned short hostshort);
    struct sockaddr_in { unsigned short sin_family; unsigned short sin_port;
        unsigned int sin_addr; char sin_zero[8]; };
    int select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout);
    int fcntl(int fd, int cmd, ...);

    // stdio
    typedef struct { int _; } FILE;
    FILE *popen(const char *cmd, const char *mode);
    int pclose(FILE *stream);
    size_t fread(void *ptr, size_t size, size_t n, FILE *s);

    // time
    long time(long *t);
    int usleep(unsigned int usec);
    int system(const char *cmd);

    // SHA1
    int SHA1(const unsigned char *d, size_t n, unsigned char *md);
]]

-- ─── 配置 ──────────────────────────────────────────────────────
local CONFIG = {
    gw_host = "127.0.0.1",
    gw_port = 8040,
    gw_path = "/v1/realtime?mode=video",
    subnet  = "192.168.1",
    ports   = {554, 80, 8080, 8000},
    rtsp_paths = {
        "/h264/ch1/main/av_stream",
        "/h264/ch1/sub/av_stream",
        "/h265/ch1/main/av_stream",
        "/h265/ch1/sub/av_stream",
        "/Streaming/Channels/101",
        "/Streaming/Channels/102",
        "/live/ch00_0",
        "/live/ch00_1",
    },
    frame_interval  = 1.0,
    max_width       = 640,
    reconnect_delay = 2,
}

-- ─── Base64 ────────────────────────────────────────────────────
local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function b64enc(data)
    local r, j = {}, 1
    for i = 1, #data - 2, 3 do
        local a, b, c = data:byte(i), data:byte(i+1), data:byte(i+2)
        local n = bit.lshift(a, 16) + bit.lshift(b, 8) + c
        r[j] = B64:sub(bit.rshift(n, 18) + 1); j = j + 1
        r[j] = B64:sub(bit.band(bit.rshift(n, 12), 0x3F) + 1); j = j + 1
        r[j] = B64:sub(bit.band(bit.rshift(n, 6), 0x3F) + 1); j = j + 1
        r[j] = B64:sub(bit.band(n, 0x3F) + 1); j = j + 1
    end
    local rem = #data % 3
    if rem == 1 then
        local n = bit.lshift(data:byte(#data), 16)
        r[j] = B64:sub(bit.rshift(n, 18) + 1); j = j + 1
        r[j] = B64:sub(bit.band(bit.rshift(n, 12), 0x3F) + 1); j = j + 1
        r[j] = "="; r[j+1] = "="
    elseif rem == 2 then
        local a, b = data:byte(#data-1), data:byte(#data)
        local n = bit.lshift(a, 16) + bit.lshift(b, 8)
        r[j] = B64:sub(bit.rshift(n, 18) + 1); j = j + 1
        r[j] = B64:sub(bit.band(bit.rshift(n, 12), 0x3F) + 1); j = j + 1
        r[j] = B64:sub(bit.band(bit.rshift(n, 6), 0x3F) + 1); j = j + 1
        r[j] = "="
    end
    return table.concat(r)
end

-- ─── 端口扫描 ──────────────────────────────────────────────────
local function scan_ip(ip)
    for _, port in ipairs(CONFIG.ports) do
        local fd = ffi.C.socket(2, 1, 0)
        if fd >= 0 then
            local addr = ffi.new("struct sockaddr_in")
            addr.sin_family = 2
            addr.sin_port = ffi.C.htons(port)
            addr.sin_addr = ffi.C.inet_addr(ip)

            -- 非阻塞 connect
            local flags = ffi.C.fcntl(fd, 3, 0)
            ffi.C.fcntl(fd, 4, bit.bor(flags, 2048))

            ffi.C.connect(fd, ffi.cast("const struct sockaddr *", addr), ffi.sizeof(addr))

            -- select 等待
            local tv = ffi.new("struct timeval[1]")
            tv[0].tv_sec = 0
            tv[0].tv_usec = 500000

            local wfds = ffi.new("fd_set[1]")
            -- FD_SET(fd, wfds)
            local fd_bits = bit.lshift(1, fd % 64)
            wfds[0].fds_bits[math.floor(fd / 64)] = fd_bits

            local ret = ffi.C.select(fd + 1, nil, wfds, nil, tv)
            ffi.C.close(fd)

            if ret > 0 then
                return port
            end
        end
    end
    return nil
end

local function scan_lan()
    print(string.format("[扫描] %s.1-254 ...", CONFIG.subnet))
    local found = {}
    local ffi_null = ffi.null

    for i = 1, 254 do
        local ip = string.format("%s.%d", CONFIG.subnet, i)
        local port = scan_ip(ip)
        if port then
            table.insert(found, {ip = ip, port = port})
            print(string.format("  [+] %s:%d", ip, port))
        end
    end
    return found
end

-- ─── RTSP 探测 ─────────────────────────────────────────────────
local function probe_rtsp(ip, port, pwd)
    for _, path in ipairs(CONFIG.rtsp_paths) do
        local url = string.format("rtsp://admin:%s@%s:%d%s", pwd, ip, port, path)
        local cmd = string.format(
            "timeout 5 ffmpeg -rtsp_transport tcp -t 2 -i '%s' -vframes 1 "
            .. "-f rawvideo -pix_fmt rgb24 - 2>/dev/null | wc -c", url)
        local f = ffi.C.popen(cmd, "r")
        if f ~= nil then
            local buf = ffi.new("char[?]", 64)
            local n = ffi.C.fread(buf, 1, 64, f)
            ffi.C.pclose(f)
            if n > 0 then
                local size = tonumber(ffi.string(buf, n):match("%d+"))
                if size and size > 1000 then
                    return path
                end
            end
        end
    end
    return nil
end

-- ─── WebSocket 客户端 ──────────────────────────────────────────
local function ws_sha1(data)
    local buf = ffi.new("unsigned char[20]")
    ffi.C.SHA1(data, #data, buf)
    return ffi.string(buf, 20)
end

local function ws_connect(host, port, path)
    local fd = ffi.C.socket(2, 1, 0)
    if fd < 0 then return nil, "socket failed" end

    local addr = ffi.new("struct sockaddr_in")
    addr.sin_family = 2
    addr.sin_port = ffi.C.htons(port)
    addr.sin_addr = ffi.C.inet_addr(host)

    if ffi.C.connect(fd, ffi.cast("const struct sockaddr *", addr), ffi.sizeof(addr)) < 0 then
        ffi.C.close(fd)
        return nil, "connect failed"
    end

    -- 握手
    local key = b64enc(ws_sha1(tostring(ffi.C.time(nil))))  -- 简单随机 key
    local hs = "GET " .. path .. " HTTP/1.1\r\n"
        .. "Host: " .. host .. ":" .. port .. "\r\n"
        .. "Upgrade: websocket\r\n"
        .. "Connection: Upgrade\r\n"
        .. "Sec-WebSocket-Key: " .. key .. "\r\n"
        .. "Sec-WebSocket-Version: 13\r\n"
        .. "Sec-WebSocket-Protocol: binary\r\n\r\n"

    local p = ffi.new("char[?]", #hs)
    ffi.copy(p, hs, #hs)
    ffi.C.send(fd, p, #hs, 0)

    -- 读响应
    local resp = ffi.new("char[?]", 4096)
    local n = ffi.C.recv(fd, resp, 4096, 0)
    if n <= 0 then
        ffi.C.close(fd)
        return nil, "handshake recv failed"
    end

    local resp_str = ffi.string(resp, n)
    if not resp_str:find("101") then
        ffi.C.close(fd)
        return nil, "handshake rejected: " .. resp_str:sub(1, 100)
    end

    return fd
end

local function ws_send(fd, data)
    local fin = 0x80
    local len = #data
    local header

    if len < 126 then
        header = string.char(bit.bor(fin, 0x1), len)
    elseif len < 65536 then
        header = string.char(bit.bor(fin, 0x1), 126,
            bit.band(bit.rshift(len, 8), 0xFF), bit.band(len, 0xFF))
    else
        header = string.char(bit.bor(fin, 0x1), 127)
        for i = 7, 0, -1 do
            header = header .. string.char(bit.band(bit.rshift(len, i * 8), 0xFF))
        end
    end

    local p = ffi.new("char[?]", #header + len)
    ffi.copy(p, header, #header)
    ffi.copy(p + #header, data, len)
    ffi.C.send(fd, p, #header + len, 0)
end

local function ws_recv(fd, timeout_ms)
    timeout_ms = timeout_ms or 50

    -- select 等待数据
    local tv = ffi.new("struct timeval[1]")
    tv[0].tv_sec = math.floor(timeout_ms / 1000)
    tv[0].tv_usec = (timeout_ms % 1000) * 1000

    local rfds = ffi.new("fd_set[1]")
    local FD_SETSIZE = 1024
    local fd_idx = math.floor(fd / 64)
    local fd_bit = bit.lshift(1, fd % 64)
    rfds[0].fds_bits[fd_idx] = fd_bit

    local ret = ffi.C.select(fd + 1, rfds, nil, nil, tv)
    if ret <= 0 then return nil, "timeout" end

    -- 读帧头
    local h2 = ffi.new("char[2]")
    local n = ffi.C.recv(fd, h2, 2, 0)
    if n <= 0 then return nil, "closed" end

    local b0 = h2[0]:byte()
    local b1 = h2[1]:byte()
    local opcode = bit.band(b0, 0x0F)
    local len = bit.band(b1, 0x7F)

    if len == 126 then
        local ext = ffi.new("char[2]")
        ffi.C.recv(fd, ext, 2, 0)
        len = bit.lshift(ext[0]:byte(), 8) + ext[1]:byte()
    elseif len == 127 then
        local ext = ffi.new("char[8]")
        ffi.C.recv(fd, ext, 8, 0)
        len = 0
        for i = 0, 7 do
            len = bit.bor(bit.lshift(len, 8), ext[i]:byte())
        end
    end

    if len == 0 then return opcode, "" end

    -- 读 payload
    local payload = ffi.new("char[?]", len)
    local pos = 0
    while pos < len do
        local r = ffi.C.recv(fd, payload + pos, len - pos, 0)
        if r <= 0 then return nil, "recv payload failed" end
        pos = pos + r
    end

    return opcode, ffi.string(payload, len)
end

local function ws_close(fd)
    -- 发送 close 帧
    ffi.C.send(fd, ffi.cast("const char *", "\x88\x00"), 2, 0)
    ffi.C.close(fd)
end

-- ─── RTSP 推流主逻辑 ───────────────────────────────────────────
local function stream_camera(ip, port, rtsp_path, password)
    local url = string.format("rtsp://admin:%s@%s:%d%s", password, ip, port, rtsp_path)
    print("[推流] " .. url)

    local function make_silence()
        return string.rep(string.char(0), 16000 * 4)  -- 1s float32 16kHz
    end

    local running = true
    while running do
        -- 连接 WS
        local ws_fd, err = ws_connect(CONFIG.gw_host, CONFIG.gw_port, CONFIG.gw_path)
        if not ws_fd then
            print("[错误] WS 连接失败: " .. (err or "unknown"))
            goto continue
        end

        -- 启动 ffmpeg 拉流
        local ffmpeg_cmd = string.format(
            "ffmpeg -rtsp_transport tcp -i '%s' "
            .. "-vf 'fps=1,scale='min(%d,iw)':-1' "
            .. "-f image2pipe -vcodec mjpeg -q:v 5 - 2>/dev/null",
            url, CONFIG.max_width)

        local fp = ffi.C.popen(ffmpeg_cmd, "r")
        if fp == nil then
            print("[错误] 无法打开 RTSP 流")
            ws_close(ws_fd)
            goto continue
        end

        print("[推流] 连接成功，开始推流...")

        -- 等待 session.queue_done
        local opcode, data = ws_recv(ws_fd, 5000)
        if not opcode then
            print("[错误] 等待就绪超时")
            goto cleanup
        end

        -- 发送 session.init
        ws_send(ws_fd, cjson.encode({
            type = "session.init",
            payload = {
                system_prompt = "你是一个监控管理员，持续观察摄像头画面。"
                    .. "正常情况下保持静默观察，不要主动说话。"
                    .. "发现异常情况时简洁描述当前画面。"
                    .. "不要主动中断对话。",
            },
        }))

        -- 等待 session.created
        opcode, data = ws_recv(ws_fd, 5000)
        if opcode then
            local ok, msg = pcall(cjson.decode, data)
            if ok and msg and msg.type == "session.created" then
                print("[推流] 会话已创建: " .. (msg.session_id or "?"))
            end
        end

        -- 主推流循环
        local buf_size = 512 * 1024
        local buf = ffi.new("char[?]", buf_size)
        local frame_buf = ""
        local last_frame = 0

        while running do
            -- 读 ffmpeg 输出
            local n = ffi.C.fread(buf, 1, buf_size, fp)
            if n > 0 then
                frame_buf = frame_buf .. ffi.string(buf, n)

                -- 提取 JPEG 帧
                local s, e = frame_buf:find("\xFF\xD8"), frame_buf:find("\xFF\xD9", (frame_buf:find("\xFF\xD8") or 0) + 2)
                if s and e then
                    local jpeg = frame_buf:sub(s, e + 1)
                    frame_buf = frame_buf:sub(e + 2)

                    local now = ffi.C.time(nil)
                    if now - last_frame >= CONFIG.frame_interval then
                        last_frame = now

                        -- 发送帧
                        local msg = cjson.encode({
                            type = "input.append",
                            input = {
                                audio = b64enc(make_silence()),
                                video_frames = {b64enc(jpeg)},
                            },
                        })
                        ws_send(ws_fd, msg)
                    end
                end
            else
                -- ffmpeg 断了
                print("[推流] ffmpeg 管道断开")
                break
            end

            -- 非阻塞读 WS 响应
            local r_ok, r_data = pcall(ws_recv, ws_fd, 10)
            if r_ok and r_data then
                local ok2, r_msg = pcall(cjson.decode, r_data)
                if ok2 and r_msg then
                    if r_msg.type == "response.output.delta" then
                        if r_msg.kind == "text" then
                            io.write("[AI] " .. (r_msg.text or ""))
                            io.flush()
                        elseif r_msg.kind == "listen" then
                            print()
                        end
                    elseif r_msg.type == "session.closed" then
                        print("\n[会话结束] " .. (r_msg.reason or "?"))
                        running = false
                        break
                    end
                end
            end
        end

        ::cleanup::
        ffi.C.pclose(fp)
        ws_close(ws_fd)

        ::continue::
        if running then
            print(string.format("[推流] %d秒后重连...", CONFIG.reconnect_delay))
            ffi.C.usleep(CONFIG.reconnect_delay * 1000000)
        end
    end
end

-- ─── 主程序 ────────────────────────────────────────────────────
local function main()
    print(string.rep("=", 50))
    print("  MiniCPM-o 局域网摄像头监控推流 (LuaJIT)")
    print(string.rep("=", 50))

    -- 解析参数
    local password = ""
    local target_ip = nil
    local target_port = 554

    local args = {...}
    local i = 1
    while i <= #args do
        if args[i] == "-p" and i + 1 <= #args then
            password = args[i + 1]
            i = i + 2
        elseif not args[i]:match("^%-") then
            if not target_ip then
                target_ip = args[i]
            else
                target_port = tonumber(args[i]) or 554
            end
            i = i + 1
        else
            print("未知参数: " .. args[i])
            os.exit(1)
        end
    end

    -- 发现摄像头
    local cams
    if target_ip then
        cams = {{ip = target_ip, port = target_port}}
    else
        cams = scan_lan()
    end

    if #cams == 0 then
        print("[!] 未发现摄像头")
        print("用法: luajit cam_monitor.lua [IP] [port] [-p 密码]")
        os.exit(1)
    end

    -- 分类
    local rtsp_cams, http_cams = {}, {}
    for _, cam in ipairs(cams) do
        if cam.port == 554 then
            table.insert(rtsp_cams, cam)
        else
            table.insert(http_cams, cam)
        end
    end

    if #rtsp_cams > 0 then
        print(string.format("\n发现 %d 个 RTSP 摄像头:", #rtsp_cams))
        for idx, cam in ipairs(rtsp_cams) do
            print(string.format("  [%d] %s:%d", idx, cam.ip, cam.port))
        end
    end
    if #http_cams > 0 then
        print("\n其他设备:")
        for _, cam in ipairs(http_cams) do
            print(string.format("      %s:%d", cam.ip, cam.port))
        end
    end

    if #rtsp_cams == 0 then
        print("\n[!] 未发现 RTSP 摄像头")
        os.exit(1)
    end

    -- 选择
    local selected
    if #rtsp_cams == 1 then
        selected = rtsp_cams[1]
    else
        io.write("\n选择摄像头编号: ")
        local idx = tonumber(io:read("*l")) or 1
        selected = rtsp_cams[idx]
    end

    -- 密码
    if password == "" then
        io.write(string.format("输入 %s 的密码: ", selected.ip))
        password = io:read("*l"):gsub("%s+$", "")
    end

    -- 探测 RTSP
    print(string.format("\n[探测] %s ...", selected.ip))
    local path = probe_rtsp(selected.ip, selected.port, password)
    if not path then
        print("[!] 密码错误或摄像头不兼容")
        os.exit(1)
    end
    print("  ✓ " .. path)

    -- 推流
    print(string.format("\n开始监控 %s ... (Ctrl+C 停止)\n", selected.ip))
    stream_camera(selected.ip, selected.port, path, password)
end

main()
