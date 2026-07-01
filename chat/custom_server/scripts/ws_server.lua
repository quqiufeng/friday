-- ws_server.lua - WebSocket 服务端（纯 LuaJIT FFI）
-- C++ accept TCP 连接后传入 fd，Lua 处理 WS 握手 + 帧协议

local ffi = require("ffi")
local bit = require("bit")
local cjson = require("cjson")

-- OpenSSL
local crypto = ffi.load("crypto")

ffi.cdef[[
    int SHA1(const unsigned char *d, size_t n, unsigned char *md);
    ssize_t recv(int sockfd, void *buf, size_t len, int flags);
    ssize_t send(int sockfd, const void *buf, size_t len, int flags);
    int close(int fd);
]]

-- ─── SHA1 + Base64 ────────────────────────────────────────────────
local b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function sha1(data)
    local buf = ffi.new("unsigned char[20]")
    crypto.SHA1(data, #data, buf)
    return ffi.string(buf, 20)
end

local function base64_encode(data)
    local b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    local r = {}
    local i, j = 1, 1
    for i = 1, #data - 2, 3 do
        local a, b, c = data:byte(i), data:byte(i+1), data:byte(i+2)
        local n = bit.lshift(a, 16) + bit.lshift(b, 8) + c
        r[j] = b64chars:sub(bit.rshift(n, 18) + 1, bit.rshift(n, 18) + 1); j = j + 1
        r[j] = b64chars:sub(bit.band(bit.rshift(n, 12), 0x3F) + 1, bit.band(bit.rshift(n, 12), 0x3F) + 1); j = j + 1
        r[j] = b64chars:sub(bit.band(bit.rshift(n, 6), 0x3F) + 1, bit.band(bit.rshift(n, 6), 0x3F) + 1); j = j + 1
        r[j] = b64chars:sub(bit.band(n, 0x3F) + 1, bit.band(n, 0x3F) + 1); j = j + 1
    end
    local remain = #data % 3
    if remain == 1 then
        local a = data:byte(#data)
        local n = bit.lshift(a, 16)
        r[j] = b64chars:sub(bit.rshift(n, 18) + 1, bit.rshift(n, 18) + 1); j = j + 1
        r[j] = b64chars:sub(bit.band(bit.rshift(n, 12), 0x3F) + 1, bit.band(bit.rshift(n, 12), 0x3F) + 1); j = j + 1
        r[j] = "="; j = j + 1
        r[j] = "="
    elseif remain == 2 then
        local a, b = data:byte(#data-1), data:byte(#data)
        local n = bit.lshift(a, 16) + bit.lshift(b, 8)
        r[j] = b64chars:sub(bit.rshift(n, 18) + 1, bit.rshift(n, 18) + 1); j = j + 1
        r[j] = b64chars:sub(bit.band(bit.rshift(n, 12), 0x3F) + 1, bit.band(bit.rshift(n, 12), 0x3F) + 1); j = j + 1
        r[j] = b64chars:sub(bit.band(bit.rshift(n, 6), 0x3F) + 1, bit.band(bit.rshift(n, 6), 0x3F) + 1); j = j + 1
        r[j] = "="
    end
    return table.concat(r)
end

-- ─── WS 握手 ──────────────────────────────────────────────────────
local function ws_handshake(fd)
    local buf = ffi.new("char[?]", 4096)
    local n = ffi.C.recv(fd, buf, 4096, 0)
    if n <= 0 then return nil, "recv failed" end

    local req = ffi.string(buf, n)
    local key = req:match("Sec%-WebSocket%-Key:%s*(%S+)")
    if not key then return nil, "missing key" end

    local accept = base64_encode(sha1(key .. "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
    local resp = "HTTP/1.1 101 Switching Protocols\r\n"
        .. "Upgrade: websocket\r\n"
        .. "Connection: Upgrade\r\n"
        .. "Sec-WebSocket-Accept: " .. accept .. "\r\n\r\n"

    local p = ffi.new("char[?]", #resp)
    ffi.copy(p, resp, #resp)
    ffi.C.send(fd, p, #resp, 0)
    return true
end

-- ─── 读 WS 帧 ────────────────────────────────────────────────────
local function ws_recv(fd)
    local function recv_n(n)
        local buf = ffi.new("char[?]", n)
        local pos = 0
        while pos < n do
            local r = ffi.C.recv(fd, buf + pos, n - pos, 0)
            if r <= 0 then return nil end
            pos = pos + r
        end
        return ffi.string(buf, n)
    end

    local h2 = recv_n(2)
    if not h2 then return nil end
    local b0, b1 = h2:byte(1), h2:byte(2)
    local opcode = bit.band(b0, 0x0F)
    local masked = bit.band(b1, 0x80) ~= 0
    local len = bit.band(b1, 0x7F)

    if len == 126 then
        local ext = recv_n(2)
        if not ext then return nil end
        len = bit.bor(bit.lshift(ext:byte(1), 8), ext:byte(2))
    elseif len == 127 then
        local ext = recv_n(8)
        if not ext then return nil end
        len = 0
        for i = 1, 8 do len = bit.bor(bit.lshift(len, 8), ext:byte(i)) end
    end

    local mask
    if masked then
        mask = recv_n(4)
        if not mask then return nil end
    end

    local payload = recv_n(len)
    if not payload then return nil end

    if masked then
        local unmasked = {}
        for i = 1, #payload do
            unmasked[i] = string.char(bit.bxor(payload:byte(i), mask:byte((i - 1) % 4 + 1)))
        end
        payload = table.concat(unmasked)
    end

    return opcode, payload
end

-- ─── 发 WS 帧 ────────────────────────────────────────────────────
local function ws_send(fd, opcode, data)
    local fin = 0x80
    local h = string.char(bit.bor(fin, opcode))
    local len = #data

    if len < 126 then
        h = h .. string.char(len)
    elseif len < 65536 then
        h = h .. string.char(126, bit.band(bit.rshift(len, 8), 0xFF), bit.band(len, 0xFF))
    else
        h = h .. string.char(127)
        for i = 7, 0, -1 do h = h .. string.char(bit.band(bit.rshift(len, i * 8), 0xFF)) end
    end

    local p = ffi.new("char[?]", #h + len)
    ffi.copy(p, h, #h)
    ffi.copy(p + #h, data, len)
    ffi.C.send(fd, p, #h + len, 0)
end

-- ─── 协议处理 ────────────────────────────────────────────────────
function handle_client(fd)
    local ok, err = ws_handshake(fd)
    if not ok then print("[WS] 握手失败:", err); return end
    print("[WS] 连接建立 fd=" .. fd)

    ws_send(fd, 0x1, '{"type":"session.created"}')

    while true do
        local opcode, data = ws_recv(fd)
        if not opcode then break end

        if opcode == 0x8 then break end       -- close
        if opcode == 0x9 then ws_send(fd, 0xA, "") end  -- ping/pong
        if opcode ~= 0x1 then break end        -- 非 text 忽略

        local ok, msg = pcall(cjson.decode, data)
        if ok and msg then
            local t = msg.type
            if t == "session.init" then
                ws_send(fd, 0x1, '{"type":"session.created","session_id":"sess_' .. fd .. '"}')
            elseif t == "input.append" then
                ws_send(fd, 0x1, '{"type":"response.output.delta","kind":"text","text":"收到"}')
            elseif t == "session.close" then
                break
            end
        end
    end

    ffi.C.close(fd)
    print("[WS] 连接关闭 fd=" .. fd)
end