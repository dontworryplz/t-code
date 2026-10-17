-- t-code: reports the editor's state to t-code through the terminal title, as
--   "t-code <modified 0|1> <line> <column> <errors> <warnings> <top> <height> <lines> <tabsize>
--    <startcol> <visualcol> <absolute path>"
-- (errors and warnings are clangd's; top is the first line shown, height the lines that fit and
-- lines the lines in the file, for t-code's scrollbar and minimap; tabsize, startcol (the first
-- column shown) and visualcol (the cursor's column on the screen, tabs expanded) for its indent
-- guides).
-- With TCODE_DETECT_INDENT set, a file's own indentation (tabs, or spaces in steps of 2, 4, ...)
-- overrides the tabsize and tabstospaces it was started with, like VS Code's "Detect Indentation".
-- With TCODE_CONTROL set, it reads lines from that named pipe:
--   scroll <line>       show the file from that line (0-based) on
--   command <command>   run a micro command, as if typed after Ctrl+E
--   prompt <text>       open the command bar with text typed in (Ctrl+G: "goto ")
-- It adds the tcodebase command that sets what the changed lines in the gutter are compared with:
--   tcodebase rev <commit>   that commit's version of the file
--   tcodebase file <path>    the contents of another file
--   tcodebase                back to the last commit (HEAD), following new commits
-- An editor started with TCODE_BASE_REV or TCODE_BASE_FILE set begins with that comparison.
VERSION = "1.1.0"

local micro = import("micro")
local config = import("micro/config")
local shell = import("micro/shell")
local buffer = import("micro/buffer")
local filepath = import("path/filepath")
local ioutil = import("io/ioutil")
local time = import("time")
local goos = import("os")

local last = nil
local report_pending = false
local ticks = 0

-- Per file: head (the commit the gutter follows), pinned (comparing with something other than
-- HEAD), reflog and stamp (where HEAD's reflog is and its size and time when last looked at).
local files = {}

local function file_state(buf)
    local state = files[buf.AbsPath]
    if state == nil then
        state = { pinned = false }
        files[buf.AbsPath] = state
    end
    return state
end

local function current()
    -- CurPane fails (in Go) before the first pane exists.
    local ok, bp = pcall(micro.CurPane)
    if ok and bp ~= nil and bp.Buf ~= nil then
        return bp
    end
    return nil
end

local function diagnostics(buf)
    local errors, warnings = 0, 0
    -- Go slices iterate when called; an empty one cannot, hence pcall.
    pcall(function()
        for _, m in buf.Messages() do
            if m.Owner == "lsp" then
                if m.Kind == buffer.MTError then
                    errors = errors + 1
                elseif m.Kind == buffer.MTWarning then
                    warnings = warnings + 1
                end
            end
        end
    end)
    return errors, warnings
end

local function write_osc(text)
    local _, err = goos.Stdout:WriteString(text)
    if err ~= nil then
        goos.Exit(0)
    end
end

local function report()
    local bp = current()
    if bp == nil then
        return
    end
    local path = bp.Buf.Path == "" and "" or bp.Buf.AbsPath
    local errors, warnings = diagnostics(bp.Buf)
    local cursor = bp.Buf:GetActiveCursor()
    local view = bp:BufView()
    local state = string.format("%d %d %d %d %d %d %d %d %d %d %d %s", bp.Buf:Modified() and 1 or 0,
                                cursor.Y + 1, cursor.X + 1, errors, warnings, view.StartLine.Line,
                                view.Height, bp.Buf:LinesNum(), math.floor(bp.Buf.Settings["tabsize"]),
                                view.StartCol, cursor:GetVisualX(false), path)
    if state ~= last then
        last = state
        write_osc("\027]0;t-code " .. state .. "\007")
    end
end

-- Indentation ---------------------------------------------------------------------------------

-- Tabs when more lines start with a tab than with spaces; else spaces, in the most common step
-- between the indentation of neighboring lines. Single spaces (" * " in block comments) and blank
-- lines do not count; without any indented line the settings stay.
local function detect_indent(buf)
    local tabs, spaces, steps, previous = 0, 0, {}, nil
    for i = 0, math.min(buf:LinesNum(), 10000) - 1 do
        local line = buf:Line(i)
        local lead = line:match("^[ \t]*")
        if #lead < #line then
            if lead:find("\t", 1, true) then
                tabs = tabs + 1
                previous = nil
            else
                if #lead > 1 then
                    spaces = spaces + 1
                end
                if previous ~= nil then
                    local step = math.abs(#lead - previous)
                    if step >= 2 and step <= 8 then
                        steps[step] = (steps[step] or 0) + 1
                    end
                end
                previous = #lead
            end
        end
    end
    if tabs > spaces then
        buf:SetOptionNative("tabstospaces", false)
    elseif spaces > 0 then
        local best, count = nil, 0
        for step = 2, 8 do
            if (steps[step] or 0) > count then
                best, count = step, steps[step]
            end
        end
        buf:SetOptionNative("tabstospaces", true)
        if best ~= nil then
            buf:SetOptionNative("tabsize", best)
        end
    end
end

local function git(buf, ...)
    local out, err = shell.ExecCommand("git", "-C", filepath.Dir(buf.AbsPath), ...)
    if err ~= nil then
        return nil
    end
    return out
end

local function show(buf, rev)
    return git(buf, "show", rev .. ":./" .. filepath.Base(buf.AbsPath))
end

-- Size and time of HEAD's reflog, which git appends to whenever HEAD moves (commit, checkout,
-- reset). Watching it costs a stat instead of a git process.
local function reflog_stamp(state)
    if not state.reflog then
        return nil
    end
    local info, err = goos.Stat(state.reflog)
    if err ~= nil then
        return nil
    end
    return info:Size() .. ":" .. info:ModTime():UnixNano()
end

-- After a commit, compare with the new HEAD. Without a reflog to watch (no commits yet), git is
-- asked every 10 seconds; force asks now.
local function follow_head(buf, force)
    local state = file_state(buf)
    if state.pinned or buf.Path == "" or not buf.Settings["diffgutter"] then
        return
    end
    if state.reflog == nil then
        local dir = git(buf, "rev-parse", "--absolute-git-dir")
        state.reflog = dir ~= nil and filepath.Join((dir:gsub("%s+$", "")), "logs", "HEAD") or false
    end
    if state.reflog == false then
        return -- not in a repository
    end
    local stamp = reflog_stamp(state)
    if stamp ~= nil and stamp == state.stamp then
        return
    end
    if stamp == nil and not force and ticks % 10 ~= 0 then
        return
    end
    state.stamp = stamp
    local commit = git(buf, "rev-parse", "HEAD")
    if commit ~= nil and commit ~= state.head then
        state.head = commit
        local base = show(buf, "HEAD")
        if base ~= nil then
            buf:SetDiffBase(base)
        end
    end
end

local function set_base(bp, kind, value)
    local buf = bp.Buf
    local state = file_state(buf)
    if kind == "file" and value ~= nil then
        local data, err = ioutil.ReadFile(value)
        if err == nil then
            buf:SetDiffBase(data)
            state.pinned = true
        end
    elseif kind == "rev" and value ~= nil then
        local base = show(buf, value)
        if base ~= nil then
            buf:SetDiffBase(base)
            state.pinned = value ~= "HEAD"
        end
    else
        state.pinned = false
        state.head = nil
        state.stamp = nil
        follow_head(buf, true)
    end
end

local function tick()
    report()
    ticks = ticks + 1
    if ticks % 2 == 0 then
        local bp = current()
        if bp ~= nil then
            follow_head(bp.Buf)
        end
    end
    micro.After(time.Second, tick)
end

-- Commands from t-code ----------------------------------------------------------------------

local pending_input = ""

local function control(line)
    local bp = current()
    if bp == nil then
        return
    end
    local verb, arg = line:match("^(%S+) ?(.*)$")
    if verb == "scroll" and tonumber(arg) ~= nil then
        local wanted = math.max(0, math.min(tonumber(arg), bp.Buf:LinesNum() - 1))
        local top = bp:BufView().StartLine.Line
        if wanted > top then
            bp:ScrollDown(wanted - top)
        elseif wanted < top then
            bp:ScrollUp(top - wanted)
        end
    elseif verb == "command" and arg ~= "" then
        bp:HandleCommand(arg)
    elseif verb == "prompt" then
        micro.InfoBar():Prompt("> ", arg, "Command", nil, function(input, canceled)
            if not canceled and input ~= "" then
                bp:HandleCommand(input)
            end
        end)
    end
    report()
end

local function on_control(text)
    pending_input = pending_input .. text
    while true do
        local line, rest = pending_input:match("^([^\n]*)\n(.*)$")
        if line == nil then
            break
        end
        pending_input = rest
        control(line)
    end
end

local function listen()
    local fifo = goos.Getenv("TCODE_CONTROL")
    if fifo == "" then
        return
    end
    -- cat ends when t-code closes the pipe (the tab went away, the pipe is removed). If it ends
    -- while the pipe is still there, listen again: t-code keeps writing to it either way.
    local function on_exit()
        micro.After(time.Second, function()
            local _, err = goos.Stat(fifo)
            if err == nil then
                listen()
            end
        end)
    end
    shell.JobSpawn("cat", {fifo}, on_control, function() end, on_exit)
end

local function base_command(bp, args)
    local list = {}
    pcall(function() -- Go slices iterate when called; an empty one cannot
        for _, arg in args() do
            list[#list + 1] = arg
        end
    end)
    set_base(bp, list[1], list[2])
end

function init()
    config.MakeCommand("tcodebase", base_command, config.NoComplete)
    micro.After(time.Second, tick)
    listen()
end

function onBufPaneOpen(bp)
    if goos.Getenv("TCODE_DETECT_INDENT") ~= "" then
        pcall(detect_indent, bp.Buf)
    end
    report()
    local rev, file = goos.Getenv("TCODE_BASE_REV"), goos.Getenv("TCODE_BASE_FILE")
    if rev ~= "" or file ~= "" then
        -- After the built-in diff plugin has set its own (HEAD) base.
        micro.After(300 * time.Millisecond, function()
            if file ~= "" then
                set_base(bp, "file", file)
            else
                set_base(bp, "rev", rev)
            end
        end)
    end
end
function onSetActive(bp) report() end
function onSave(bp) report() end
-- Events come in bursts (typing, scrolling, mouse drags): report once the burst has passed.
function onAnyEvent()
    if report_pending then
        return
    end
    report_pending = true
    micro.After(30 * time.Millisecond, function()
        report_pending = false
        report()
    end)
end
