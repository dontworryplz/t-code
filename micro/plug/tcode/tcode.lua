-- t-code: reports the open file and whether it has unsaved changes through the terminal title,
-- as "t-code <0|1> <absolute path>". t-code reads it to label the tab ("●" when modified).
VERSION = "1.0.0"

local micro = import("micro")
local goos = import("os")

local last = nil

local function report()
    -- CurPane fails (in Go) before the first pane exists.
    local ok, bp = pcall(micro.CurPane)
    if not ok or bp == nil or bp.Buf == nil then
        return
    end
    local path = bp.Buf.Path == "" and "" or bp.Buf.AbsPath
    local state = (bp.Buf:Modified() and "1 " or "0 ") .. path
    if state ~= last then
        last = state
        goos.Stdout:WriteString("\027]0;t-code " .. state .. "\007")
    end
end

function onBufPaneOpen(bp) report() end
function onSetActive(bp) report() end
function onAnyEvent() report() end
