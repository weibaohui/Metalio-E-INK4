--[[--
方块战争 (俄罗斯方块) - Kindle 墨水屏全屏大视野定制版
针对 Kindle 600x800 分辨率深度重构：
- 棋盘空间最大化拓展 (单元格 35px，视野拓展 77% 以上)
- 纯净去按键化设计，右侧看板微型化，底部精简为单行操作小提示
- 点棋盘任意处直接顺时针旋转方块
- 支持双击直坠到底、下滑直坠、划屏平移、长按暂存
- 下落速度严格恒定 0.90s，完美匹配墨水屏物理刷新，杜绝重影与拖影
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
local CELL_W = 48 -- 10 * 48 = 480px 宽 (占屏幕 600px 的整整 80.0%!)
local CELL_H = 36 -- 20 * 36 = 720px 高 (占屏幕 800px 的 90.0%!)
local BOARD_X = 6
local BOARD_Y = 48

local STATE_RUNNING = 0
local STATE_PAUSED = 1
local STATE_OVER = 2

-- 恒定下落速度：0.90 秒 / 格。慢于墨水屏物理刷新周期，杜绝重影与动画拖影
local CONSTANT_DROP_DELAY = 0.90

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
    disable_double_tap = false,
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
    self.last_hard_drop_clock = 0
    self.last_tap_clock = 0
    self.last_tap_x = 0
    self.last_tap_y = 0

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

    if self.auto_mode and self.state == STATE_RUNNING then
        self:computeAiPlan()
        -- AI 模式：在顶部生成时立即对齐目标形态与列坐标，随后匀速匀步下落
        self.rot_ = self.ai_target_rot
        self.px_ = self.ai_target_x
    end

    if not self:fits(self.type_, self.rot_, self.px_, self.py_) then
        self.state = STATE_OVER
        self:saveHiscore()
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

        -- 消行时若多行消除或满 5 行，执行一次全局全刷彻底清除底纹残影
        if cleared >= 2 or self.lines % 5 == 0 then
            UIManager:setDirty(self, "full")
        end
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
    self.last_hard_drop_clock = os.clock()
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
        self.rot_ = self.ai_target_rot
        self.px_ = self.ai_target_x
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

-- ======================= 定时器与刷新 =======================
function TetrisDialog:scheduleNextDrop()
    if self.is_closed then return end

    -- 无论手动还是 AI 观战模式，下落速度均严格保持恒定 0.90s，绝不随关卡变快。
    -- 必须慢于墨水屏粒子物理刷新周期，彻底杜绝重影与动画拖影异形。
    local delay = CONSTANT_DROP_DELAY

    self.timer_action = UIManager:scheduleIn(delay, function()
        if self.is_closed then return end
        if self.state == STATE_RUNNING then
            self:softDrop()
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
    local font_small = Font:getFace("cfont", 13)
    local font_mini = Font:getFace("cfont", 11)
    local font_btn = Font:getFace("cfont", 14)

    -- 1. 顶部紧凑标题与功能按键 (Y: 0 ~ 44)
    bb:paintRect(0, 0, 600, 44, Blitbuffer.COLOR_BLACK)
    RenderText:renderUtf8Text(bb, 10, 31, font_title, "方块战争", false, true, Blitbuffer.COLOR_WHITE)

    -- 顶部按钮: [暂停/继续] [重开] [退出]
    local btn_p_text = (self.state == STATE_PAUSED) and "▶ 继续" or "⏸ 暂停"
    bb:paintRoundedRect(330, 7, 70, 30, Blitbuffer.COLOR_WHITE, 4)
    RenderText:renderUtf8Text(bb, 340, 28, font_btn, btn_p_text, false, true, Blitbuffer.COLOR_BLACK)

    bb:paintRoundedRect(410, 7, 70, 30, Blitbuffer.COLOR_WHITE, 4)
    RenderText:renderUtf8Text(bb, 420, 28, font_btn, "🔄 重开", false, true, Blitbuffer.COLOR_BLACK)

    bb:paintRoundedRect(490, 7, 102, 30, Blitbuffer.COLOR_WHITE, 4)
    RenderText:renderUtf8Text(bb, 520, 28, font_btn, "✕ 退出", false, true, Blitbuffer.COLOR_BLACK)

    -- 2. 80% 宽度极限大视野主棋盘 (BOARD_X=6, BOARD_Y=48, CELL_W=48, CELL_H=36, W=480, H=720)
    -- 占据整个屏幕 600px 宽度的整整 80.0%！
    bb:paintRect(BOARD_X - 2, BOARD_Y - 2, FW * CELL_W + 4, FH * CELL_H + 4, Blitbuffer.COLOR_BLACK)
    bb:paintRect(BOARD_X, BOARD_Y, FW * CELL_W, FH * CELL_H, Blitbuffer.COLOR_WHITE)

    -- 网格底纹
    for gy = 1, FH do
        for gx = 1, FW do
            local bx = BOARD_X + (gx - 1) * CELL_W
            local by = BOARD_Y + (gy - 1) * CELL_H
            bb:paintRect(bx + CELL_W - 1, by + CELL_H - 1, 1, 1, Blitbuffer.COLOR_GRAY_7)
        end
    end

    -- 绘制已固定的方块
    for gy = 1, FH do
        for gx = 1, FW do
            if self.board[gy][gx] ~= 0 then
                local bx = BOARD_X + (gx - 1) * CELL_W
                local by = BOARD_Y + (gy - 1) * CELL_H
                bb:paintRect(bx + 1, by + 1, CELL_W - 2, CELL_H - 2, Blitbuffer.COLOR_BLACK)
                bb:paintRect(bx + 2, by + 2, CELL_W - 4, 2, Blitbuffer.COLOR_WHITE)
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
                local bx = BOARD_X + cx * CELL_W
                local by = BOARD_Y + cy * CELL_H
                bb:paintRect(bx + 2, by + 2, CELL_W - 4, CELL_H - 4, Blitbuffer.COLOR_GRAY_3)
                bb:paintRect(bx + 4, by + 4, CELL_W - 8, CELL_H - 8, Blitbuffer.COLOR_WHITE)
            end
        end

        -- 绘制当前活动下落的方块
        for i = 1, 4 do
            local cx = self.px_ + shape[i][1]
            local cy = self.py_ + shape[i][2]
            if cy >= 0 and cy < FH and cx >= 0 and cx < FW then
                local bx = BOARD_X + cx * CELL_W
                local by = BOARD_Y + cy * CELL_H
                bb:paintRect(bx + 1, by + 1, CELL_W - 2, CELL_H - 2, Blitbuffer.COLOR_BLACK)
                bb:paintRect(bx + 2, by + 2, CELL_W - 4, 2, Blitbuffer.COLOR_WHITE)
            end
        end
    end

    -- 3. 右侧极简微型看板 (NEXT / HOLD / STATS / AI)
    -- X: 492 ~ 596 (宽 104px, 仅占 17% 宽度, 彻底让出 80% 视野给主游戏区!)
    local side_x = 492
    local side_w = 104

    -- 下一个预览框 (NEXT)
    bb:paintRoundedRect(side_x, BOARD_Y, side_w, 80, Blitbuffer.COLOR_GRAY_9, 4)
    bb:paintRect(side_x, BOARD_Y, side_w, 1, Blitbuffer.COLOR_BLACK)
    RenderText:renderUtf8Text(bb, side_x + 30, BOARD_Y + 16, font_small, "下一个", false, true, Blitbuffer.COLOR_BLACK)
    if self.next_ > 0 then
        local n_shape = SHAPES[self.next_][1]
        for i = 1, 4 do
            local nbx = side_x + 22 + n_shape[i][1] * 15
            local nby = BOARD_Y + 26 + n_shape[i][2] * 13
            bb:paintRect(nbx, nby, 13, 11, Blitbuffer.COLOR_BLACK)
        end
    end

    -- 暂存框 (HOLD) - 支持轻触直接换块
    local hold_y = BOARD_Y + 88
    bb:paintRoundedRect(side_x, hold_y, side_w, 80, Blitbuffer.COLOR_GRAY_9, 4)
    bb:paintRect(side_x, hold_y, side_w, 1, Blitbuffer.COLOR_BLACK)
    RenderText:renderUtf8Text(bb, side_x + 24, hold_y + 16, font_small, "暂存换块", false, true, Blitbuffer.COLOR_BLACK)
    if self.hold_ > 0 then
        local h_shape = SHAPES[self.hold_][1]
        for i = 1, 4 do
            local hbx = side_x + 22 + h_shape[i][1] * 15
            local hby = hold_y + 26 + h_shape[i][2] * 13
            bb:paintRect(hbx, hby, 13, 11, Blitbuffer.COLOR_BLACK)
        end
    end

    -- 统计指标卡 (垂直紧凑排列)
    local stat_y = hold_y + 88
    bb:paintRoundedRect(side_x, stat_y, side_w, 236, Blitbuffer.COLOR_WHITE, 4)
    bb:paintRect(side_x, stat_y, side_w, 236, Blitbuffer.COLOR_BLACK)
    bb:paintRect(side_x + 1, stat_y + 1, side_w - 2, 234, Blitbuffer.COLOR_WHITE)

    -- 分数
    RenderText:renderUtf8Text(bb, side_x + 36, stat_y + 20, font_mini, "得分", false, false, Blitbuffer.COLOR_GRAY_3)
    RenderText:renderUtf8Text(bb, side_x + 14, stat_y + 44, font_num, tostring(self.score), false, true, Blitbuffer.COLOR_BLACK)

    -- 最高
    RenderText:renderUtf8Text(bb, side_x + 36, stat_y + 78, font_mini, "最高", false, false, Blitbuffer.COLOR_GRAY_3)
    RenderText:renderUtf8Text(bb, side_x + 14, stat_y + 102, font_num, tostring(math.max(self.score, self.hiscore)), false, true, Blitbuffer.COLOR_BLACK)

    -- 行数
    RenderText:renderUtf8Text(bb, side_x + 36, stat_y + 136, font_mini, "消行", false, false, Blitbuffer.COLOR_GRAY_3)
    RenderText:renderUtf8Text(bb, side_x + 14, stat_y + 160, font_num, tostring(self.lines), false, true, Blitbuffer.COLOR_BLACK)

    -- 级别
    RenderText:renderUtf8Text(bb, side_x + 36, stat_y + 194, font_mini, "难度", false, false, Blitbuffer.COLOR_GRAY_3)
    RenderText:renderUtf8Text(bb, side_x + 14, stat_y + 218, font_num, "Lv " .. tostring(self.level), false, true, Blitbuffer.COLOR_BLACK)

    -- AI 自动演示开关按键 (右侧下方)
    local ai_btn_y = stat_y + 244
    local ai_bg = self.auto_mode and Blitbuffer.COLOR_BLACK or Blitbuffer.COLOR_WHITE
    local ai_fg = self.auto_mode and Blitbuffer.COLOR_WHITE or Blitbuffer.COLOR_BLACK
    bb:paintRoundedRect(side_x, ai_btn_y, side_w, 46, ai_bg, 4)
    bb:paintRect(side_x, ai_btn_y, side_w, 46, Blitbuffer.COLOR_BLACK)
    if not self.auto_mode then
        bb:paintRect(side_x + 1, ai_btn_y + 1, side_w - 2, 44, Blitbuffer.COLOR_WHITE)
    end
    local ai_text1 = self.auto_mode and "★ AI演示中" or "☆ AI观战"
    local ai_text2 = self.auto_mode and "(点此手动)" or "(点击开启)"
    RenderText:renderUtf8Text(bb, side_x + 16, ai_btn_y + 18, font_small, ai_text1, false, true, ai_fg)
    RenderText:renderUtf8Text(bb, side_x + 22, ai_btn_y + 36, font_mini, ai_text2, false, false, ai_fg)

    -- 游戏状态浮层 (居中在主棋盘上)
    local center_x = BOARD_X + math.floor((FW * CELL_W - 220) / 2)
    if self.state == STATE_PAUSED then
        bb:paintRoundedRect(center_x, 320, 220, 80, Blitbuffer.COLOR_BLACK, 8)
        bb:paintRoundedRect(center_x + 2, 322, 216, 76, Blitbuffer.COLOR_WHITE, 6)
        RenderText:renderUtf8Text(bb, center_x + 50, 368, font_title, "游戏已暂停", false, true, Blitbuffer.COLOR_BLACK)
    elseif self.state == STATE_OVER then
        bb:paintRoundedRect(center_x, 310, 220, 100, Blitbuffer.COLOR_BLACK, 8)
        bb:paintRoundedRect(center_x + 2, 312, 216, 96, Blitbuffer.COLOR_WHITE, 6)
        RenderText:renderUtf8Text(bb, center_x + 45, 355, font_title, "游戏结束!", false, true, Blitbuffer.COLOR_BLACK)
        RenderText:renderUtf8Text(bb, center_x + 30, 390, font_num, "轻触屏幕重开新局", false, false, Blitbuffer.COLOR_GRAY_3)
    end

    -- ======================= 4. 底部极简单行微型提示 =======================
    -- Y: 772 ~ 798 (单行精炼小字，彻底释放底部空间)
    bb:paintRect(6, 772, 588, 1, Blitbuffer.COLOR_GRAY_7)
    RenderText:renderUtf8Text(bb, 20, 790, font_small, "操作: 划屏平移 · 点棋盘/上划旋转 · 下滑/双击直降 · 点暂存换块", false, false, Blitbuffer.COLOR_GRAY_3)
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

    -- 1. 顶部操作按钮优先处理 (Y: 5 ~ 44)
    if y >= 5 and y <= 44 then
        if x >= 325 and x <= 405 then
            -- 暂停 / 继续
            if self.state == STATE_RUNNING then
                self.state = STATE_PAUSED
            elseif self.state == STATE_PAUSED then
                self.state = STATE_RUNNING
            end
            self:refreshScreen()
            return true
        elseif x >= 410 and x <= 485 then
            -- 重开新局
            self:startNewGame()
            self:refreshScreen()
            return true
        elseif x >= 490 and x <= 595 then
            -- 退出
            self:onClose()
            return true
        end
    end

    -- 2. 游戏结束状态下轻点任意处开新局
    if self.state == STATE_OVER then
        self:startNewGame()
        self:refreshScreen()
        return true
    end

    -- 3. 点击 AI 模式开关 (492, 468 ~ 596, 520)
    if x >= 492 and x <= 596 and y >= 468 and y <= 520 then
        self.auto_mode = not self.auto_mode
        if self.auto_mode and self.state == STATE_RUNNING then
            self:computeAiPlan()
            self.rot_ = self.ai_target_rot
            self.px_ = self.ai_target_x
        end
        self:refreshScreen()
        return true
    end

    -- 4. 点击右侧暂存区 HOLD (492, 136 ~ 596, 216)
    if x >= 492 and x <= 596 and y >= 136 and y <= 216 then
        self:holdPiece()
        return true
    end

    -- 5. 仅在直坠硬降 (hardDrop) 刚刚发生的 0.20 秒内过滤残余抬手点击，防止刚落底的新方块被误转
    if self.last_hard_drop_clock and (now - self.last_hard_drop_clock) < 0.20 then
        logger.warn("TetrisDialog: Ignored tap right after hardDrop")
        return true
    end

    -- 6. 双击直降核心判定 (连续两次点击在 60ms ~ 380ms 内，且两点相距 < 90px)
    if self.last_tap_clock and (now - self.last_tap_clock) >= 0.06 and (now - self.last_tap_clock) < 0.38 then
        local tap_dist = math.sqrt((x - self.last_tap_x)^2 + (y - self.last_tap_y)^2)
        if tap_dist < 90 then
            logger.warn("TetrisDialog: Double-tap hardDrop! dist=" .. tostring(tap_dist))
            self.last_tap_clock = 0
            self:hardDrop()
            return true
        end
    end
    self.last_tap_clock = now
    self.last_tap_x = x
    self.last_tap_y = y

    -- 7. 点棋盘任意区域直接旋转方块！(占屏幕 80% 宽度，极易盲按)
    if x >= BOARD_X and x <= BOARD_X + FW * CELL_W and y >= BOARD_Y and y <= BOARD_Y + FH * CELL_H then
        logger.warn("TetrisDialog: Tap on board (" .. tostring(x) .. ", " .. tostring(y) .. ") -> rotatePiece!")
        self:rotatePiece()
        return true
    end

    return true
end

-- 全屏滑动盲操 (深度优化误判容差)
function TetrisDialog:onSwipe(arg, ges)
    ges = ges or arg
    if not ges then return true end

    local dir = ges.direction or ""
    local dx = (ges.end_pos and ges.pos) and (ges.end_pos.x - ges.pos.x) or 0
    local dy = (ges.end_pos and ges.pos) and (ges.end_pos.y - ges.pos.y) or 0
    local dist = ges.distance or math.sqrt(dx*dx + dy*dy)

    -- 如果位移非常微小 (小于 20px)，属于手指触控抖动而非真正划屏，直接忽略放行给 onTap 去旋转
    if dist < 20 and math.abs(dx) < 20 and math.abs(dy) < 20 then
        logger.warn("TetrisDialog:onSwipe ignored tiny jitter dist=" .. tostring(dist))
        return false
    end

    self.last_swipe_clock = os.clock()
    logger.warn("TetrisDialog:onSwipe raw dir=" .. tostring(dir) .. " dx=" .. tostring(dx) .. " dy=" .. tostring(dy) .. " dist=" .. tostring(dist))

    -- ========================================================
    -- 1. 下滑 (直坠速降锁定) —— 第一优先级，极致放宽判定！
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
    -- 3. 左右平移 (必须是纯横向位移为主)
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
    logger.warn("TetrisDialog:onDoubleTap event -> hardDrop")
    self:hardDrop()
    return true
end

-- 长按屏幕: 暂存方块 (HOLD)
function TetrisDialog:onHold(arg, ges)
    logger.warn("TetrisDialog:onHold -> holdPiece")
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
