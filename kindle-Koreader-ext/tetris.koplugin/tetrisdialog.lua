--[[--
方块战争 (俄罗斯方块) - Kindle 墨水屏深度定制极简手势版
针对 Kindle 600x800 分辨率与触摸交互全新重构
支持全屏划屏手势盲操、落点投影预判、AI 自动观战与底部极简触控提示区。
--]]--

local Device = require("device")
local Font = require("ui/font")
local Geom = require("ui/geometry")
local GestureRange = require("ui/gesturerange")
local InputContainer = require("ui/widget/container/inputcontainer")
local UIManager = require("ui/uimanager")
local Screen = Device.screen
local Blitbuffer = require("ffi/blitbuffer")
local DataStorage = require("datastorage")
local RenderText = require("ui/rendertext")
local logger = require("logger")

local FW = 10
local FH = 20
local CELL_SZ = 27 -- 10 * 27 = 270px 宽, 20 * 27 = 540px 高
local BOARD_X = 25
local BOARD_Y = 65

local STATE_RUNNING = 0
local STATE_PAUSED = 1
local STATE_OVER = 2

-- 7 种经典俄罗斯方块定义 (0-indexed x, y)
local SHAPES = {
    -- 1: I
    {
        { {0,1},{1,1},{2,1},{3,1} },
        { {2,0},{2,1},{2,2},{2,3} },
        { {0,2},{1,2},{2,2},{3,2} },
        { {1,0},{1,1},{1,2},{1,3} },
    },
    -- 2: O
    {
        { {1,0},{2,0},{1,1},{2,1} },
        { {1,0},{2,0},{1,1},{2,1} },
        { {1,0},{2,0},{1,1},{2,1} },
        { {1,0},{2,0},{1,1},{2,1} },
    },
    -- 3: T
    {
        { {1,0},{0,1},{1,1},{2,1} },
        { {1,0},{1,1},{2,1},{1,2} },
        { {0,1},{1,1},{2,1},{1,2} },
        { {1,0},{0,1},{1,1},{1,2} },
    },
    -- 4: S
    {
        { {1,0},{2,0},{0,1},{1,1} },
        { {1,0},{1,1},{2,1},{2,2} },
        { {1,1},{2,1},{0,2},{1,2} },
        { {0,0},{0,1},{1,1},{1,2} },
    },
    -- 5: Z
    {
        { {0,0},{1,0},{1,1},{2,1} },
        { {2,0},{1,1},{2,1},{1,2} },
        { {0,1},{1,1},{1,2},{2,2} },
        { {1,0},{0,1},{1,1},{0,2} },
    },
    -- 6: L
    {
        { {2,0},{0,1},{1,1},{2,1} },
        { {1,0},{1,1},{1,2},{2,2} },
        { {0,1},{1,1},{2,1},{0,2} },
        { {0,0},{1,0},{1,1},{1,2} },
    },
    -- 7: J
    {
        { {0,0},{0,1},{1,1},{2,1} },
        { {1,0},{2,0},{1,1},{1,2} },
        { {0,1},{1,1},{2,1},{2,2} },
        { {1,0},{1,1},{0,2},{1,2} },
    },
}

local TetrisDialog = InputContainer:extend{
    name = "tetris_dialog",
    dimen = Geom:new{ x = 0, y = 0, w = 600, h = 800 },
    modal = true,
}

function TetrisDialog:init()
    self.show_parent = self
    self.dimen = Geom:new{ x = 0, y = 0, w = Screen:getWidth(), h = Screen:getHeight() }

    self.board = {}
    for y = 1, FH do
        self.board[y] = {}
        for x = 1, FW do
            self.board[y][x] = 0
        end
    end

    self.hiscore_file = DataStorage:getSettingsDir() .. "/tetris_hiscore.txt"
    self.hiscore = self:loadHiscore()

    self.state = STATE_OVER
    self.score = 0
    self.lines = 0
    self.level = 1
    self.pieces = 0
    self.auto_mode = false

    self.type_ = 1
    self.rot_ = 1
    self.px_ = 3
    self.py_ = 1
    self.next_ = math.random(1, 7)
    self.hold_ = 0
    self.can_hold_ = true

    -- AI 自动规划缓存
    self.ai_target_rot = 1
    self.ai_target_x = 3

    self.timer_action = nil
    self.is_closed = false
    self.last_swipe_clock = 0

    -- 注册全屏手势支持
    local full_range = Geom:new{ x = 0, y = 0, w = Screen:getWidth(), h = Screen:getHeight() }
    self.ges_events = {
        Tap = { GestureRange:new{ ges = "tap", range = full_range } },
        Swipe = { GestureRange:new{ ges = "swipe", range = full_range } },
        DoubleTap = { GestureRange:new{ ges = "double_tap", range = full_range } },
        Hold = { GestureRange:new{ ges = "hold", range = full_range } },
    }

    self:registerTouchZones({
        {
            id = "tetris_all_tap",
            ges = "tap",
            screen_zone = { ratio_x = 0, ratio_y = 0, ratio_w = 1, ratio_h = 1 },
            handler = function(ges) return self:onTap(nil, ges) end,
        },
        {
            id = "tetris_all_swipe",
            ges = "swipe",
            screen_zone = { ratio_x = 0, ratio_y = 0, ratio_w = 1, ratio_h = 1 },
            handler = function(ges) return self:onSwipe(nil, ges) end,
        },
        {
            id = "tetris_all_double_tap",
            ges = "double_tap",
            screen_zone = { ratio_x = 0, ratio_y = 0, ratio_w = 1, ratio_h = 1 },
            handler = function(ges) return self:onDoubleTap(nil, ges) end,
        },
        {
            id = "tetris_all_hold",
            ges = "hold",
            screen_zone = { ratio_x = 0, ratio_y = 0, ratio_w = 1, ratio_h = 1 },
            handler = function(ges) return self:onHold(nil, ges) end,
        },
    })

    self:startNewGame()
end

function TetrisDialog:loadHiscore()
    local f = io.open(self.hiscore_file, "r")
    if f then
        local s = f:read("*number")
        f:close()
        if s then return s end
    end
    return 0
end

function TetrisDialog:saveHiscore()
    if self.score > self.hiscore then
        self.hiscore = self.score
        local f = io.open(self.hiscore_file, "w")
        if f then
            f:write(tostring(self.hiscore))
            f:close()
        end
    end
end

function TetrisDialog:startNewGame()
    for y = 1, FH do
        for x = 1, FW do
            self.board[y][x] = 0
        end
    end
    self.score = 0
    self.lines = 0
    self.level = 1
    self.pieces = 0
    self.hold_ = 0
    self.can_hold_ = true
    self.state = STATE_RUNNING
    self.next_ = math.random(1, 7)
    self:spawn()
    self:scheduleNextDrop()
end

function TetrisDialog:fits(t, r, x, y)
    local shape = SHAPES[t][r]
    for i = 1, 4 do
        local cx = x + shape[i][1]
        local cy = y + shape[i][2]
        if cx < 0 or cx >= FW or cy >= FH then
            return false
        end
        if cy >= 0 and self.board[cy + 1][cx + 1] ~= 0 then
            return false
        end
    end
    return true
end

function TetrisDialog:getGhostY()
    local y = self.py_
    while self:fits(self.type_, self.rot_, self.px_, y + 1) do
        y = y + 1
    end
    return y
end

function TetrisDialog:spawn()
    self.type_ = self.next_
    self.next_ = math.random(1, 7)
    self.rot_ = 1
    self.px_ = 3
    self.py_ = 0
    self.can_hold_ = true
    self.pieces = self.pieces + 1

    if not self:fits(self.type_, self.rot_, self.px_, self.py_) then
        self.state = STATE_OVER
        self:saveHiscore()
    end

    if self.auto_mode and self.state == STATE_RUNNING then
        self:computeAiPlan()
    end
end

function TetrisDialog:lockAndClear()
    local shape = SHAPES[self.type_][self.rot_]
    for i = 1, 4 do
        local cx = self.px_ + shape[i][1]
        local cy = self.py_ + shape[i][2]
        if cy >= 0 and cy < FH and cx >= 0 and cx < FW then
            self.board[cy + 1][cx + 1] = self.type_
        end
    end

    local cleared = 0
    local y = FH
    while y >= 1 do
        local full = true
        for x = 1, FW do
            if self.board[y][x] == 0 then
                full = false
                break
            end
        end
        if full then
            cleared = cleared + 1
            for ty = y, 2, -1 do
                for tx = 1, FW do
                    self.board[ty][tx] = self.board[ty - 1][tx]
                end
            end
            for tx = 1, FW do
                self.board[1][tx] = 0
            end
        else
            y = y - 1
        end
    end

    if cleared > 0 then
        self.lines = self.lines + cleared
        local pts = { 100, 300, 500, 800 }
        self.score = self.score + (pts[cleared] or 1000) * self.level
        self.level = math.floor(self.lines / 10) + 1
        self:saveHiscore()
    end

    self:spawn()
end

function TetrisDialog:moveLeft()
    if self.state ~= STATE_RUNNING then return end
    if self:fits(self.type_, self.rot_, self.px_ - 1, self.py_) then
        self.px_ = self.px_ - 1
        self:refreshScreen()
    end
end

function TetrisDialog:moveRight()
    if self.state ~= STATE_RUNNING then return end
    if self:fits(self.type_, self.rot_, self.px_ + 1, self.py_) then
        self.px_ = self.px_ + 1
        self:refreshScreen()
    end
end

function TetrisDialog:rotatePiece()
    if self.state ~= STATE_RUNNING then return end
    local next_rot = (self.rot_ % 4) + 1
    if self:fits(self.type_, next_rot, self.px_, self.py_) then
        self.rot_ = next_rot
        self:refreshScreen()
    elseif self:fits(self.type_, next_rot, self.px_ - 1, self.py_) then
        self.px_ = self.px_ - 1
        self.rot_ = next_rot
        self:refreshScreen()
    elseif self:fits(self.type_, next_rot, self.px_ + 1, self.py_) then
        self.px_ = self.px_ + 1
        self.rot_ = next_rot
        self:refreshScreen()
    end
end

function TetrisDialog:softDrop()
    if self.state ~= STATE_RUNNING then return end
    if self:fits(self.type_, self.rot_, self.px_, self.py_ + 1) then
        self.py_ = self.py_ + 1
        self.score = self.score + 1
        self:refreshScreen()
    else
        self:lockAndClear()
        self:refreshScreen()
    end
end

function TetrisDialog:hardDrop()
    if self.state ~= STATE_RUNNING then return end
    local gy = self:getGhostY()
    local drop_dist = gy - self.py_
    self.score = self.score + drop_dist * 2
    self.py_ = gy
    self:lockAndClear()
    self:refreshScreen()
end

function TetrisDialog:holdPiece()
    if self.state ~= STATE_RUNNING or not self.can_hold_ then return end
    self.can_hold_ = false

    if self.hold_ == 0 then
        self.hold_ = self.type_
        self:spawn()
    else
        local tmp = self.type_
        self.type_ = self.hold_
        self.hold_ = tmp
        self.rot_ = 1
        self.px_ = 3
        self.py_ = 0
        if not self:fits(self.type_, self.rot_, self.px_, self.py_) then
            self.state = STATE_OVER
            self:saveHiscore()
        end
    end
    if self.auto_mode and self.state == STATE_RUNNING then
        self:computeAiPlan()
    end
    self:refreshScreen()
end

-- ======================= AI 观战决策算法 =======================
function TetrisDialog:computeAiPlan()
    local best_score = -9999999
    local best_rot = 1
    local best_x = 3

    for r = 1, 4 do
        local shape = SHAPES[self.type_][r]
        local min_x = 0
        local max_x = 0
        for i = 1, 4 do
            if shape[i][1] < min_x then min_x = shape[i][1] end
            if shape[i][1] > max_x then max_x = shape[i][1] end
        end

        for x = -min_x, FW - 1 - max_x do
            if self:fits(self.type_, r, x, 0) then
                local y = 0
                while self:fits(self.type_, r, x, y + 1) do
                    y = y + 1
                end

                local tmp = {}
                for ty = 1, FH do
                    tmp[ty] = {}
                    for tx = 1, FW do
                        tmp[ty][tx] = self.board[ty][tx]
                    end
                end

                for i = 1, 4 do
                    local cx = x + shape[i][1]
                    local cy = y + shape[i][2]
                    if cy >= 0 and cy < FH then
                        tmp[cy + 1][cx + 1] = 1
                    end
                end

                local cleared = 0
                for ty = 1, FH do
                    local full = true
                    for tx = 1, FW do
                        if tmp[ty][tx] == 0 then full = false; break end
                    end
                    if full then cleared = cleared + 1 end
                end

                local agg_height = 0
                local col_heights = {}
                for tx = 1, FW do
                    local h = 0
                    for ty = 1, FH do
                        if tmp[ty][tx] ~= 0 then
                            h = FH - ty + 1
                            break
                        end
                    end
                    col_heights[tx] = h
                    agg_height = agg_height + h
                end

                local holes = 0
                for tx = 1, FW do
                    local block_found = false
                    for ty = 1, FH do
                        if tmp[ty][tx] ~= 0 then
                            block_found = true
                        elseif block_found and tmp[ty][tx] == 0 then
                            holes = holes + 1
                        end
                    end
                end

                local bumpiness = 0
                for tx = 1, FW - 1 do
                    bumpiness = bumpiness + math.abs(col_heights[tx] - col_heights[tx + 1])
                end

                local eval_score = (-0.51 * agg_height) + (0.76 * cleared) - (0.36 * holes) - (0.18 * bumpiness)
                if eval_score > best_score then
                    best_score = eval_score
                    best_rot = r
                    best_x = x
                end
            end
        end
    end

    self.ai_target_rot = best_rot
    self.ai_target_x = best_x
end

function TetrisDialog:aiTick()
    if self.state ~= STATE_RUNNING or not self.auto_mode then return end

    if self.rot_ ~= self.ai_target_rot then
        self:rotatePiece()
    elseif self.px_ < self.ai_target_x then
        self:moveRight()
    elseif self.px_ > self.ai_target_x then
        self:moveLeft()
    else
        self:hardDrop()
    end
end

-- ======================= 定时器与刷新 =======================
function TetrisDialog:scheduleNextDrop()
    if self.is_closed then return end

    local delay = math.max(0.18, 0.75 - (self.level - 1) * 0.06)
    if self.auto_mode then
        delay = 0.20 -- AI 观战模式
    end

    self.timer_action = UIManager:scheduleIn(delay, function()
        if self.is_closed then return end
        if self.state == STATE_RUNNING then
            if self.auto_mode then
                self:aiTick()
            else
                self:softDrop()
            end
        elseif self.state == STATE_OVER and self.auto_mode then
            self:startNewGame()
        end
        self:scheduleNextDrop()
    end)
end

function TetrisDialog:refreshScreen()
    UIManager:setDirty(self, "ui")
end

-- ======================= 绘制主渲染逻辑 =======================
function TetrisDialog:paintTo(bb, x, y)
    bb:paintRect(0, 0, 600, 800, Blitbuffer.COLOR_WHITE)

    local font_title = Font:getFace("tfont", 20)
    local font_num = Font:getFace("cfont", 16)
    local font_small = Font:getFace("cfont", 14)
    local font_btn = Font:getFace("cfont", 16)

    -- 1. 顶部标题与功能快捷键 (Y: 0 ~ 55)
    bb:paintRect(0, 0, 600, 52, Blitbuffer.COLOR_BLACK)
    RenderText:renderUtf8Text(bb, 20, 36, font_title, "方块战争 · 极简手势触控版", false, true, Blitbuffer.COLOR_WHITE)

    -- 顶部按钮: [暂停/继续] [新开局] [退出]
    local btn_p_text = (self.state == STATE_PAUSED) and "▶ 继续" or "⏸ 暂停"
    bb:paintRoundedRect(330, 10, 75, 32, Blitbuffer.COLOR_WHITE, 4)
    RenderText:renderUtf8Text(bb, 342, 33, font_btn, btn_p_text, false, true, Blitbuffer.COLOR_BLACK)

    bb:paintRoundedRect(420, 10, 75, 32, Blitbuffer.COLOR_WHITE, 4)
    RenderText:renderUtf8Text(bb, 432, 33, font_btn, "🔄 重开", false, true, Blitbuffer.COLOR_BLACK)

    bb:paintRoundedRect(510, 10, 75, 32, Blitbuffer.COLOR_WHITE, 4)
    RenderText:renderUtf8Text(bb, 525, 33, font_btn, "✕ 退出", false, true, Blitbuffer.COLOR_BLACK)

    -- 2. 左侧主棋盘渲染 (BOARD_X=25, BOARD_Y=65, W=270, H=540)
    bb:paintRect(BOARD_X - 3, BOARD_Y - 3, FW * CELL_SZ + 6, FH * CELL_SZ + 6, Blitbuffer.COLOR_BLACK)
    bb:paintRect(BOARD_X, BOARD_Y, FW * CELL_SZ, FH * CELL_SZ, Blitbuffer.COLOR_WHITE)

    -- 网格底纹
    for gy = 1, FH do
        for gx = 1, FW do
            local bx = BOARD_X + (gx - 1) * CELL_SZ
            local by = BOARD_Y + (gy - 1) * CELL_SZ
            bb:paintRect(bx + CELL_SZ - 1, by + CELL_SZ - 1, 1, 1, Blitbuffer.COLOR_GRAY_7)
        end
    end

    -- 绘制已固定的方块
    for gy = 1, FH do
        for gx = 1, FW do
            if self.board[gy][gx] ~= 0 then
                local bx = BOARD_X + (gx - 1) * CELL_SZ
                local by = BOARD_Y + (gy - 1) * CELL_SZ
                bb:paintRect(bx + 1, by + 1, CELL_SZ - 2, CELL_SZ - 2, Blitbuffer.COLOR_BLACK)
                bb:paintRect(bx + 2, by + 2, CELL_SZ - 4, 1, Blitbuffer.COLOR_WHITE)
            end
        end
    end

    -- 绘制落点虚线影子 (Ghost Piece)
    if self.state == STATE_RUNNING then
        local gy = self:getGhostY()
        local shape = SHAPES[self.type_][self.rot_]
        for i = 1, 4 do
            local cx = self.px_ + shape[i][1]
            local cy = gy + shape[i][2]
            if cy >= 0 and cy < FH and cx >= 0 and cx < FW then
                local bx = BOARD_X + cx * CELL_SZ
                local by = BOARD_Y + cy * CELL_SZ
                bb:paintRect(bx + 2, by + 2, CELL_SZ - 4, CELL_SZ - 4, Blitbuffer.COLOR_GRAY_3)
                bb:paintRect(bx + 3, by + 3, CELL_SZ - 6, CELL_SZ - 6, Blitbuffer.COLOR_WHITE)
            end
        end

        -- 绘制当前活动下落的方块
        for i = 1, 4 do
            local cx = self.px_ + shape[i][1]
            local cy = self.py_ + shape[i][2]
            if cy >= 0 and cy < FH and cx >= 0 and cx < FW then
                local bx = BOARD_X + cx * CELL_SZ
                local by = BOARD_Y + cy * CELL_SZ
                bb:paintRect(bx + 1, by + 1, CELL_SZ - 2, CELL_SZ - 2, Blitbuffer.COLOR_BLACK)
                bb:paintRect(bx + 2, by + 2, CELL_SZ - 4, 1, Blitbuffer.COLOR_WHITE)
            end
        end
    end

    -- 3. 右侧信息看板 (Next / Hold / Stats / AI)
    local side_x = 315

    -- 下一个预览框 (NEXT)
    bb:paintRoundedRect(side_x, BOARD_Y, 260, 95, Blitbuffer.COLOR_GRAY_9, 6)
    bb:paintRect(side_x, BOARD_Y, 260, 1, Blitbuffer.COLOR_BLACK)
    RenderText:renderUtf8Text(bb, side_x + 12, BOARD_Y + 22, font_small, "下一个 (NEXT)", false, true, Blitbuffer.COLOR_BLACK)
    if self.next_ > 0 then
        local n_shape = SHAPES[self.next_][1]
        for i = 1, 4 do
            local nbx = side_x + 110 + n_shape[i][1] * 18
            local nby = BOARD_Y + 28 + n_shape[i][2] * 18
            bb:paintRect(nbx, nby, 16, 16, Blitbuffer.COLOR_BLACK)
        end
    end

    -- 暂存框 (HOLD) - 支持轻触直接换块
    local hold_y = BOARD_Y + 110
    bb:paintRoundedRect(side_x, hold_y, 260, 95, Blitbuffer.COLOR_GRAY_9, 6)
    bb:paintRect(side_x, hold_y, 260, 1, Blitbuffer.COLOR_BLACK)
    RenderText:renderUtf8Text(bb, side_x + 12, hold_y + 22, font_small, "暂存 (HOLD · 点此换块)", false, true, Blitbuffer.COLOR_BLACK)
    if self.hold_ > 0 then
        local h_shape = SHAPES[self.hold_][1]
        for i = 1, 4 do
            local hbx = side_x + 110 + h_shape[i][1] * 18
            local hby = hold_y + 28 + h_shape[i][2] * 18
            bb:paintRect(hbx, hby, 16, 16, Blitbuffer.COLOR_BLACK)
        end
    end

    -- 统计指标卡
    local stat_y = hold_y + 110
    bb:paintRoundedRect(side_x, stat_y, 260, 175, Blitbuffer.COLOR_WHITE, 6)
    bb:paintRect(side_x, stat_y, 260, 175, Blitbuffer.COLOR_BLACK)
    bb:paintRect(side_x + 1, stat_y + 1, 258, 173, Blitbuffer.COLOR_WHITE)

    RenderText:renderUtf8Text(bb, side_x + 15, stat_y + 35, font_num, "当前得分:", false, false, Blitbuffer.COLOR_GRAY_3)
    RenderText:renderUtf8Text(bb, side_x + 110, stat_y + 35, font_title, tostring(self.score), false, true, Blitbuffer.COLOR_BLACK)

    RenderText:renderUtf8Text(bb, side_x + 15, stat_y + 75, font_num, "最高纪录:", false, false, Blitbuffer.COLOR_GRAY_3)
    RenderText:renderUtf8Text(bb, side_x + 110, stat_y + 75, font_title, tostring(math.max(self.score, self.hiscore)), false, true, Blitbuffer.COLOR_BLACK)

    RenderText:renderUtf8Text(bb, side_x + 15, stat_y + 115, font_num, "消除行数:", false, false, Blitbuffer.COLOR_GRAY_3)
    RenderText:renderUtf8Text(bb, side_x + 110, stat_y + 115, font_title, tostring(self.lines), false, true, Blitbuffer.COLOR_BLACK)

    RenderText:renderUtf8Text(bb, side_x + 15, stat_y + 155, font_num, "游戏级别:", false, false, Blitbuffer.COLOR_GRAY_3)
    RenderText:renderUtf8Text(bb, side_x + 110, stat_y + 155, font_title, "Lv " .. tostring(self.level), false, true, Blitbuffer.COLOR_BLACK)

    -- AI 自动演示开关按键 (右侧下方)
    local ai_btn_y = stat_y + 190
    local ai_bg = self.auto_mode and Blitbuffer.COLOR_BLACK or Blitbuffer.COLOR_WHITE
    local ai_fg = self.auto_mode and Blitbuffer.COLOR_WHITE or Blitbuffer.COLOR_BLACK
    bb:paintRoundedRect(side_x, ai_btn_y, 260, 42, ai_bg, 6)
    bb:paintRect(side_x, ai_btn_y, 260, 42, Blitbuffer.COLOR_BLACK)
    if not self.auto_mode then
        bb:paintRect(side_x + 1, ai_btn_y + 1, 258, 40, Blitbuffer.COLOR_WHITE)
    end
    local ai_text = self.auto_mode and "★ AI 自动观战中 (点此手动)" or "☆ 开启 AI 自动观战"
    RenderText:renderUtf8Text(bb, side_x + 20, ai_btn_y + 28, font_small, ai_text, false, true, ai_fg)

    -- 游戏状态浮层
    if self.state == STATE_PAUSED then
        bb:paintRoundedRect(50, 240, 200, 80, Blitbuffer.COLOR_BLACK, 8)
        bb:paintRoundedRect(52, 242, 196, 76, Blitbuffer.COLOR_WHITE, 6)
        RenderText:renderUtf8Text(bb, 95, 288, font_title, "游戏已暂停", false, true, Blitbuffer.COLOR_BLACK)
    elseif self.state == STATE_OVER then
        bb:paintRoundedRect(40, 230, 220, 100, Blitbuffer.COLOR_BLACK, 8)
        bb:paintRoundedRect(42, 232, 216, 96, Blitbuffer.COLOR_WHITE, 6)
        RenderText:renderUtf8Text(bb, 85, 275, font_title, "游戏结束!", false, true, Blitbuffer.COLOR_BLACK)
        RenderText:renderUtf8Text(bb, 75, 310, font_num, "轻触屏幕重开新局", false, false, Blitbuffer.COLOR_GRAY_3)
    end

    -- ======================= 4. 底部极简手势指南与辅助触控面板 =======================
    -- Y: 620 ~ 785 (高度 165px)
    local guide_y = 620
    local guide_h = 165

    bb:paintRoundedRect(25, guide_y, 550, guide_h, Blitbuffer.COLOR_GRAY_9, 8)
    bb:paintRect(25, guide_y, 550, guide_h, Blitbuffer.COLOR_BLACK)
    bb:paintRect(27, guide_y + 2, 546, guide_h - 4, Blitbuffer.COLOR_WHITE)

    -- 标题指示
    RenderText:renderUtf8Text(bb, 185, guide_y + 28, font_title, "✦ 全屏手势交互指南 ✦", false, true, Blitbuffer.COLOR_BLACK)
    bb:paintRect(40, guide_y + 36, 520, 1, Blitbuffer.COLOR_GRAY_7)

    -- 4 组手势图解与说明
    -- Row 1: 左移与右移
    RenderText:renderUtf8Text(bb, 45, guide_y + 68, font_btn, "◄ 向左滑动: 方块左移", false, true, Blitbuffer.COLOR_BLACK)
    RenderText:renderUtf8Text(bb, 320, guide_y + 68, font_btn, "► 向右滑动: 方块右移", false, true, Blitbuffer.COLOR_BLACK)

    -- Row 2: 旋转与直坠
    RenderText:renderUtf8Text(bb, 45, guide_y + 105, font_btn, "▲ 向上滑动 / 点棋盘: 旋转", false, true, Blitbuffer.COLOR_BLACK)
    RenderText:renderUtf8Text(bb, 320, guide_y + 105, font_btn, "▼ 向下滑动 / 双击: 直坠", false, true, Blitbuffer.COLOR_BLACK)

    -- Row 3: 底部长按与辅助点按提示
    bb:paintRect(40, guide_y + 120, 520, 1, Blitbuffer.COLOR_GRAY_7)
    RenderText:renderUtf8Text(bb, 55, guide_y + 148, font_small, "★ 长按屏幕或轻触暂存框换块 | 亦可点按此区域左/右微调，点中央旋转", false, false, Blitbuffer.COLOR_GRAY_3)
end

-- ======================= 手势与触控事件分发 =======================

-- 顶级 onGesture 统一入口 (直接由 UIManager 派发)
function TetrisDialog:onGesture(ev)
    if not ev then return true end
    local ges = ev.ges
    if ges == "tap" then
        return self:onTap(nil, ev)
    elseif ges == "swipe" then
        return self:onSwipe(nil, ev)
    elseif ges == "hold" then
        return self:onHold(nil, ev)
    elseif ges == "double_tap" then
        return self:onDoubleTap(nil, ev)
    end
    return true
end

function TetrisDialog:onTap(arg, ges)
    ges = ges or arg
    if not ges or not ges.pos then return true end
    local x = ges.pos.x
    local y = ges.pos.y

    local now = os.clock()
    -- 核心防误判：如果刚刚发生过滑动 (0.45 秒内)，完全忽略本次点击事件！
    -- 彻底消除用户在下滑、划屏结束抬指离开屏幕时，电容屏瞬间产生的微小抖动 tap！
    if (now - self.last_swipe_clock) < 0.45 then
        logger.warn("TetrisDialog:onTap ignored (anti-ghost tap after swipe, dt=" .. tostring(now - self.last_swipe_clock) .. ")")
        return true
    end

    logger.warn("TetrisDialog:onTap (" .. tostring(x) .. ", " .. tostring(y) .. ")")

    -- 顶部按钮点击 (Y: 5 ~ 50)
    if y >= 5 and y <= 50 then
        if x >= 320 and x <= 410 then
            -- 暂停 / 继续
            if self.state == STATE_RUNNING then
                self.state = STATE_PAUSED
            elseif self.state == STATE_PAUSED then
                self.state = STATE_RUNNING
            end
            self:refreshScreen()
            return true
        elseif x >= 415 and x <= 500 then
            -- 重开新局
            self:startNewGame()
            self:refreshScreen()
            return true
        elseif x >= 505 and x <= 590 then
            -- 退出
            self:onClose()
            return true
        end
    end

    -- 游戏结束状态下轻点任意处开新局
    if self.state == STATE_OVER then
        self:startNewGame()
        self:refreshScreen()
        return true
    end

    -- 点击 AI 模式开关 (315, 470 ~ 575, 530)
    if x >= 315 and x <= 575 and y >= 470 and y <= 530 then
        self.auto_mode = not self.auto_mode
        if self.auto_mode and self.state == STATE_RUNNING then
            self:computeAiPlan()
        end
        self:refreshScreen()
        return true
    end

    -- 点击右侧暂存区 HOLD (315, 170 ~ 575, 270)
    if x >= 315 and x <= 575 and y >= 170 and y <= 270 then
        self:holdPiece()
        return true
    end

    -- 底部触控指示区点击辅助微调 (Y: 620 ~ 785)
    if y >= 620 and y <= 785 then
        if x < 200 then
            self:moveLeft()
        elseif x > 400 then
            self:moveRight()
        else
            self:rotatePiece()
        end
        return true
    end

    -- 主棋盘区域智能分区点击:
    -- 左侧 30% 左移，右侧 30% 右移，中间 40% 旋转
    if x >= BOARD_X and x <= BOARD_X + FW * CELL_SZ and y >= BOARD_Y and y <= BOARD_Y + FH * CELL_SZ then
        local rel_x = x - BOARD_X
        if rel_x < FW * CELL_SZ * 0.30 then
            self:moveLeft()
        elseif rel_x > FW * CELL_SZ * 0.70 then
            self:moveRight()
        else
            self:rotatePiece()
        end
        return true
    end

    return true
end

-- 全屏滑动盲操 (深度优化误判容差)
function TetrisDialog:onSwipe(arg, ges)
    ges = ges or arg
    if not ges then return true end
    self.last_swipe_clock = os.clock()

    local dir = ges.direction or ""
    local dx = (ges.end_pos and ges.pos) and (ges.end_pos.x - ges.pos.x) or 0
    local dy = (ges.end_pos and ges.pos) and (ges.end_pos.y - ges.pos.y) or 0
    local dist = ges.distance or 0

    logger.warn("TetrisDialog:onSwipe raw dir=" .. tostring(dir) .. " dx=" .. tostring(dx) .. " dy=" .. tostring(dy) .. " dist=" .. tostring(dist))

    -- ========================================================
    -- 1. 下滑 (直坠速降锁定) —— 第一优先级，极致放宽判定！
    -- 用户在墨水屏设备上下滑时，手指/拇指很容易划出斜线或弧度，不可能正上正下。
    -- 满足以下任一条件立即判定为【下滑直坠】：
    --   a) 纵向下移量 dy >= 18 且 dy >= |dx| * 0.35 (即使倾斜达 70 度也判定为下滑!)
    --   b) 系统方向为 "south", "southwest", "southeast"
    -- ========================================================
    local is_down = false
    if dy >= 20 and dy >= math.abs(dx) * 0.55 then
        is_down = true
    elseif dir:find("south") then
        is_down = true
    end

    if is_down then
        logger.warn("TetrisDialog: Classified as DOWNWARD (hardDrop)")
        self:hardDrop()
        return true
    end

    -- ========================================================
    -- 2. 上滑 (顺时针旋转)
    -- 必须具有明确的向上位移趋势，绝不与任何下滑趋势冲突
    -- ========================================================
    local is_up = false
    if dy <= -20 and math.abs(dy) >= math.abs(dx) * 0.60 then
        is_up = true
    elseif dir == "north" or dir == "northwest" or dir == "northeast" then
        is_up = true
    end

    if is_up then
        logger.warn("TetrisDialog: Classified as UPWARD (rotate)")
        self:rotatePiece()
        return true
    end

    -- ========================================================
    -- 3. 左右平移 (必须是纯横向位移为主，严禁带下偏分量)
    -- ========================================================
    if dx < -18 and math.abs(dx) > math.abs(dy) * 0.85 then
        logger.warn("TetrisDialog: Classified as LEFT")
        self:moveLeft()
        return true
    elseif dx > 18 and dx > math.abs(dy) * 0.85 then
        logger.warn("TetrisDialog: Classified as RIGHT")
        self:moveRight()
        return true
    elseif dir == "west" then
        self:moveLeft()
        return true
    elseif dir == "east" then
        self:moveRight()
        return true
    end

    return true
end

-- 双击屏幕: 直坠到底
function TetrisDialog:onDoubleTap(arg, ges)
    logger.warn("TetrisDialog:onDoubleTap")
    self:hardDrop()
    return true
end

-- 长按屏幕: 暂存方块 (HOLD)
function TetrisDialog:onHold(arg, ges)
    logger.warn("TetrisDialog:onHold")
    self:holdPiece()
    return true
end

function TetrisDialog:onClose()
    self.is_closed = true
    if self.timer_action then
        UIManager:unschedule(self.timer_action)
        self.timer_action = nil
    end
    UIManager:close(self)
end

return TetrisDialog
