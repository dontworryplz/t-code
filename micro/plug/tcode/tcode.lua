-- t-code: reports the editor's state to t-code through the terminal title, as
--   "t-code <modified 0|1> <line> <column> <errors> <warnings> <absolute path>"
-- (errors and warnings are clangd's), and adds the tcodebase command that sets what the changed
-- lines in the gutter are compared with:
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
local head = nil     -- the commit the gutter currently follows
local pinned = false -- comparing with something other than HEAD
local ticks = 0

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
    local state = string.format("%d %d %d %d %d %s", bp.Buf:Modified() and 1 or 0,
                                cursor.Y + 1, cursor.X + 1, errors, warnings, path)
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

-- After a commit, compare with the new HEAD.
local function follow_head(buf)
    if pinned or buf.Path == "" or not buf.Settings["diffgutter"] then
        return
    end
    local commit = git(buf, "rev-parse", "HEAD")
    if commit ~= nil and commit ~= head then
        head = commit
        local base = show(buf, "HEAD")
        if base ~= nil then
            buf:SetDiffBase(base)
        end
    end
end

local function set_base(bp, kind, value)
    local buf = bp.Buf
    if kind == "file" and value ~= nil then
        local data, err = ioutil.ReadFile(value)
        if err == nil then
            buf:SetDiffBase(data)
            pinned = true
        end
    elseif kind == "rev" and value ~= nil then
        local base = show(buf, value)
        if base ~= nil then
            buf:SetDiffBase(base)
            pinned = value ~= "HEAD"
        end
    else
        pinned = false
        head = nil
        follow_head(buf)
    end
end

local function tick()
    report()
    ticks = ticks + 1
    if ticks % 10 == 0 then
        local bp = current()
        if bp ~= nil then
            follow_head(bp.Buf)
        end
    end
    micro.After(time.Second, tick)
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
function onAnyEvent() report() end
