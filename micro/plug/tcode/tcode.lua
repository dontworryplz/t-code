-- t-code: reports the editor's state to t-code through the terminal title, as
--   "t-code <modified 0|1> <line> <column> <errors> <warnings> <top> <height> <lines> <absolute path>"
-- (errors and warnings are clangd's; top is the first line shown, height the lines that fit and
-- lines the lines in the file, for t-code's scrollbar and minimap).
-- With TCODE_CONTROL set, it reads lines from that named pipe:
--   scroll <line>       show the file from that line (0-based) on
--   command <command>   run a micro command, as if typed after Ctrl+E
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
    local state = string.format("%d %d %d %d %d %d %d %d %s", bp.Buf:Modified() and 1 or 0,
                                cursor.Y + 1, cursor.X + 1, errors, warnings, view.StartLine.Line,
                                view.Height, bp.Buf:LinesNum(), path)
    if state ~= last then
        last = state
        write_osc("\027]0;t-code " .. state .. "\007")
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
    if fifo ~= "" then
        -- cat ends when t-code closes the pipe (the tab went away).
        local ignore = function() end
        shell.JobSpawn("cat", {fifo}, on_control, ignore, ignore)
    end
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
