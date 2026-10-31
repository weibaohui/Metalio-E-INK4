local UIManager = require("ui/uimanager")
local WidgetContainer = require("ui/widget/container/widgetcontainer")
local reader_order = require("ui/elements/reader_menu_order")
local filemanager_order = require("ui/elements/filemanager_menu_order")

-- 插入到工具栏菜单
table.insert(reader_order.tools, 2, "tetris_game")
table.insert(filemanager_order.tools, 2, "tetris_game")

local TetrisPlugin = WidgetContainer:extend{
    name = "tetris",
    is_doc_only = false,
}

function TetrisPlugin:init()
    self.ui.menu:registerToMainMenu(self)
end

function TetrisPlugin:addToMainMenu(menu_items)
    menu_items.tetris_game = {
        text = "方块战争 (俄罗斯方块)",
        sorting_hint = "tools",
        callback = function()
            package.loaded["tetrisdialog"] = nil
            local TetrisDialog = require("tetrisdialog")
            local dlg = TetrisDialog:new()
            UIManager:show(dlg)
        end,
    }
end

return TetrisPlugin
