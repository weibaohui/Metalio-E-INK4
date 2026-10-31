package.path = "/mnt/us/koreader/common/?.lua;" .. package.path
package.cpath = "/mnt/us/koreader/common/?.so;" .. package.cpath
local rj = require("rapidjson")

local function read_file(path)
    local f = io.open(path, "r")
    if not f then return "{}" end
    local s = f:read("*a")
    f:close()
    return s
end

local quota = rj.decode(read_file("/tmp/quota.json"))
local resets = rj.decode(read_file("/tmp/resets.json"))

if not quota or quota.code ~= 200 then
    local err_msg = (quota and quota.msg) or "网络请求超时"
    local f = io.open("/tmp/dashboard.txt", "w")
    f:write(string.format("\n\n【 智谱 GLM 看板 】\n\n查询失败：%s\n\n请检查网络连接。", err_msg))
    f:close()
    os.exit(0)
end

local limits = (quota.data and quota.data.limits) or {}
local f_limit, w_limit
for _, lim in ipairs(limits) do
    if lim.unit == 3 and lim.number == 5 then
        f_limit = lim
    elseif lim.unit == 6 and lim.number == 1 then
        w_limit = lim
    end
end

local f_pct = f_limit and f_limit.percentage or 0
local f_used = f_limit and f_limit.currentValue or 0
local f_total = f_limit and f_limit.usage or 15000
local f_rem = f_limit and f_limit.remaining or math.max(0, f_total - f_used)

local w_pct = w_limit and w_limit.percentage or 0
local w_used = w_limit and w_limit.currentValue or 0
local w_total = w_limit and w_limit.usage or 66000
local w_rem = w_limit and w_limit.remaining or math.max(0, w_total - w_used)

local resets_avail = 0
if resets and resets.data and resets.data.weekResets then
    for _, r in ipairs(resets.data.weekResets) do
        if r.available == true then
            resets_avail = resets_avail + 1
        end
    end
end

local date_str = os.date("%H:%M:%S")

local function make_bar(pct)
    local total_len = 5
    local filled = math.floor(pct / 100 * total_len + 0.5)
    if filled > total_len then filled = total_len end
    local s = "["
    for i = 1, filled do s = s .. "■" end
    for i = filled + 1, total_len do s = s .. "□" end
    s = s .. string.format("] %d%%", pct)
    return s
end

local f_bar = make_bar(f_pct)
local w_bar = make_bar(w_pct)

local f_status = (f_pct < 80) and "充足健康" or "额度已满"
local w_status = (w_pct >= 100) and "已用完 (达上限)" or "正常"

local lines = {
    "   【 智谱 GLM 看板 】",
    "──────────────",
    "套餐：团队编程套餐",
    "",
    "【 5小时滑动配额 】",
    "状态：" .. f_status,
    "进度：" .. f_bar,
    string.format("已用：%d / %d", f_used, f_total),
    "",
    "【 每周自然配额 】",
    "状态：" .. w_status,
    "进度：" .. w_bar,
    string.format("已用：%d / %d", w_used, w_total),
    string.format("礼券：剩余 %d 张重置券", resets_avail),
    "",
    "──────────────",
    "更新：" .. date_str .. " (KUAL刷新)"
}

local out_text = table.concat(lines, "\n")
local f = io.open("/tmp/dashboard.txt", "w")
f:write(out_text)
f:close()
