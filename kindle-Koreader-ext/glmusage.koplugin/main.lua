local InfoMessage = require("ui/widget/infomessage")
local TextViewer = require("ui/widget/textviewer")
local UIManager = require("ui/uimanager")
local WidgetContainer = require("ui/widget/container/widgetcontainer")

local GlmUsage = WidgetContainer:extend{
    name = "glmusage",
    is_doc_only = false,
}

function GlmUsage:init()
    self.ui.menu:registerToMainMenu(self)
end

function GlmUsage:addToMainMenu(menu_items)
    menu_items.glmusage = {
        text = "智谱 GLM 用量查询",
        sorting_hint = "more_tools",
        callback = function()
            local info = InfoMessage:new{
                text = "正在联网查询智谱 GLM 额度，请稍候...",
            }
            UIManager:show(info)
            UIManager:nextTick(function()
                os.execute("/mnt/us/extensions/glm_usage/bin/query.sh")
                UIManager:close(info)
                local f = io.open("/tmp/dashboard.txt", "r")
                local content = "获取数据失败，请检查网络。"
                if f then
                    content = f:read("*a")
                    f:close()
                end
                UIManager:show(TextViewer:new{
                    title = "智谱 GLM 用量看板",
                    text = content,
                })
            end)
        end,
    }
end

return GlmUsage
