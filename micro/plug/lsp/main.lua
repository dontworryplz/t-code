VERSION = "0.6.2"


local micro = import("micro")
local config = import("micro/config")
local shell = import("micro/shell")
local util = import("micro/util")
local buffer = import("micro/buffer")
local fmt = import("fmt")
local go_os = import("os")
local path = import("path")
local filepath = import("path/filepath")

local cmd = {}
local id = {}
local version = {}
local currentAction = {}
local capabilities = {}
local filetype = ''
local rootUri = ''
local message = ''
local completionCursor = 0
local lastCompletion = {}
local splitBP = nil
local tabCount = 0

local json = {}

function toBytes(str)
	local result = {}
	for i=1,#str do 
		local b = str:byte(i)
		if b < 32 then 
			table.insert(result, b)
		end
	end
	return result
end

-- The patterns below spell out byte ranges: gopher-lua's %w and %c follow Unicode and would
-- take single bytes of UTF-8 text for letters or control characters.

-- Percent-encodes a path the way clangd does, so URIs compare equal.
function pathToUri(file)
	return "file://" .. file:gsub("[^A-Za-z0-9%-%_%.~/:]", function (c)
		return string.format("%%%02X", c:byte())
	end)
end

function uriToPath(uri)
	local file = uri:gsub("^file://", "")
	return (file:gsub("%%(%x%x)", function (h) return string.char(tonumber(h, 16)) end))
end

local jsonEscapes = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
local jsonSpecial = "[" .. string.char(1) .. "-" .. string.char(31) .. '"\\]'

-- Escapes text for a JSON string, control characters included (clangd rejects them raw).
function jsonEscape(text)
	return (text:gsub(jsonSpecial, function (c)
		return jsonEscapes[c] or string.format("\\u%04x", c:byte())
	end))
end

function getUriFromBuf(buf)
	if buf == nil then return; end
	return pathToUri(buf.AbsPath)
end

function mysplit (inputstr, sep)
        if sep == nil then
                sep = "%s"
        end
        local t={}
        for str in string.gmatch(inputstr, "([^"..sep.."]+)") do
                table.insert(t, str)
        end
        return t
end

function parseOptions(inputstr)
	local t = {}
	for str in inputstr:gmatch("[^,]+") do
		table.insert(t, str)
	end
	return t
end

function startServer(filetype, callback)
	local wd, _ = go_os.Getwd()
	rootUri = pathToUri(wd)
	local envSettings, _ = go_os.Getenv("MICRO_LSP")
	local settings = config.GetGlobalOption("lsp.server")
	local fallback = "python=pylsp,go=gopls,typescript=deno lsp,javascript=deno lsp,markdown=deno lsp,json=deno lsp,jsonc=deno lsp,rust=rls,lua=lua-lsp,c++=clangd"
	if envSettings ~= nil and #envSettings > 0 then
		settings = envSettings
	end
	if settings ~= nil and #settings > 0 then
		settings = settings .. "," .. fallback
	else
		settings = fallback
	end
	local server = parseOptions(settings)
	micro.Log("Server Options", server)
	for i in pairs(server) do
		local lang, rest = server[i]:match("^([%w+_-]+)=(.+)$")
		if lang == filetype and rest then
			local runStr, initOpts = rest:match("^(.-)=({.*})$")
			local run = mysplit(runStr or rest, "%s")
			local initOptions = initOpts or '{}'
			local runCmd = table.remove(run, 1)
			local args = run
			local send = withSend(lang)
			if cmd[lang] ~= nil then return; end
			id[lang] = 0
			micro.Log("Starting server", lang)
			cmd[lang] = shell.JobSpawn(runCmd, args, onStdout(lang), onStderr, onExit(lang), {})
			currentAction[lang] = { method = "initialize", response = function (bp, data)
			    send("initialized", "{}", true)
				capabilities[filetype] = data.result and data.result.capabilities or {}
			    callback(bp.Buf, filetype)
			end }
			send(currentAction[lang].method, fmt.Sprintf('{"processId": %.0f, "rootUri": "%s", "workspaceFolders": [{"name": "root", "uri": "%s"}], "initializationOptions": %s, "capabilities": {"textDocument": {"hover": {"contentFormat": ["plaintext", "markdown"]}, "publishDiagnostics": {"relatedInformation": false, "versionSupport": false, "codeDescriptionSupport": true, "dataSupport": true}, "signatureHelp": {"signatureInformation": {"documentationFormat": ["plaintext", "markdown"]}}}}}', go_os.Getpid(), rootUri, rootUri, initOptions))
			return
		end
	end
end

function init()
	config.RegisterCommonOption("lsp", "server", "python=pylsp,go=gopls,typescript=deno lsp,javascript=deno lsp,markdown=deno lsp,json=deno lsp,jsonc=deno lsp,rust=rls,lua=lua-lsp,c++=clangd")
	config.RegisterCommonOption("lsp", "formatOnSave", true)
	config.RegisterCommonOption("lsp", "autocompleteDetails", false)
	config.RegisterCommonOption("lsp", "ignoreMessages", "")
	config.RegisterCommonOption("lsp", "tabcompletion", true)
	config.RegisterCommonOption("lsp", "ignoreTriggerCharacters", "completion")
	-- example to ignore all LSP server message starting with these strings:
	-- "lsp.ignoreMessages": "Skipping analyzing |See https://"
	
	config.MakeCommand("hover", hoverAction, config.NoComplete)
	config.MakeCommand("definition", definitionAction, config.NoComplete)
	config.MakeCommand("lspcompletion", completionAction, config.NoComplete)
	config.MakeCommand("format", formatAction, config.NoComplete)
	config.MakeCommand("references", referencesAction, config.NoComplete)

	config.TryBindKey("Alt-k", "command:hover", false)
	config.TryBindKey("Alt-d", "command:definition", false)
	config.TryBindKey("Alt-f", "command:format", false)
	config.TryBindKey("Alt-r", "command:references", false)
	config.TryBindKey("CtrlSpace", "command:lspcompletion", false)

	config.AddRuntimeFile("lsp", config.RTHelp, "help/lsp.md")
		
	-- @TODO register additional actions here
end

function withSend(filetype)
	return function (method, params, isNotification) 
	    if cmd[filetype] == nil then
	    	return
	    end
	    
		local msg = fmt.Sprintf('{"jsonrpc": "2.0", %s"method": "%s", "params": %s}', not isNotification and fmt.Sprintf('"id": %.0f, ', id[filetype]) or "", method, params)
		id[filetype] = id[filetype] + 1
		msg = fmt.Sprintf("Content-Length: %.0f\r\n\r\n%s", #msg, msg)
		--micro.Log("send", filetype, "sending", method or msg, msg)
		shell.JobSend(cmd[filetype], msg)
	end
end

function preRune(bp, r)
	if splitBP ~= nil then
		pcall(function () splitBP:Unsplit(); end)
		splitBP = nil
		local cur = bp.Buf:GetActiveCursor()
		cur:Deselect(false);
		cur:GotoLoc(buffer.Loc(cur.X + 1, cur.Y))
	end
end

-- when a new character is types, the document changes
function onRune(bp, r)
	local filetype = bp.Buf:FileType()
	if cmd[filetype] == nil then
		return
	end
	if splitBP ~= nil then
		pcall(function () splitBP:Unsplit(); end)
		splitBP = nil
	end

	local send = withSend(filetype)
	local uri = getUriFromBuf(bp.Buf)
	if r ~= nil then
		lastCompletion = {}
	end
	-- allow the document contents to be escaped properly for the JSON string
	local content = jsonEscape(util.String(bp.Buf:Bytes()))
	-- increase change version
	version[uri] = (version[uri] or 0) + 1
	send("textDocument/didChange", fmt.Sprintf('{"textDocument": {"version": %.0f, "uri": "%s"}, "contentChanges": [{"text": "%s"}]}', version[uri], uri, content), true)
	local ignored = mysplit(config.GetGlobalOption("lsp.ignoreTriggerCharacters") or '', ",")
	if r and capabilities[filetype] then
		if not contains(ignored, "completion") and capabilities[filetype].completionProvider and capabilities[filetype].completionProvider.triggerCharacters and contains(capabilities[filetype].completionProvider.triggerCharacters, r) then
			completionAction(bp)
		elseif not contains(ignored, "signature") and capabilities[filetype].signatureHelpProvider and capabilities[filetype].signatureHelpProvider.triggerCharacters and contains(capabilities[filetype].signatureHelpProvider.triggerCharacters, r) then
			hoverAction(bp)
		end
	end
end

-- alias functions for any kind of change to the document
-- @TODO: add missing ones
function onBackspace(bp) onRune(bp); end
function onCut(bp) onRune(bp); end
function onCutLine(bp) onRune(bp); end
function onDuplicateLine(bp) onRune(bp); end
function onDeleteLine(bp) onRune(bp); end
function onDelete(bp) onRune(bp); end
function onUndo(bp) onRune(bp); end
function onRedo(bp) onRune(bp); end
function onIndent(bp) onRune(bp); end
function onIndentSelection(bp) onRune(bp); end
function onPaste(bp) onRune(bp); end

function onEscape(bp) 
	if splitBP ~= nil then
		pcall(function () splitBP:Unsplit(); end)
		splitBP = nil
	end
end

function preInsertNewline(bp)
	if bp.Buf.Path == "References found" then
		local cur = bp.Buf:GetActiveCursor()
		cur:SelectLine()
		local data = util.String(cur:GetSelection())
		local doc, line, character = data:match("^(.*):(%d+):(%d+)")
		if doc == nil then return false end
		if go_os.Getenv("TCODE_SOCKET") ~= "" then
			local abs, err = filepath.Abs(doc)
			if err == nil then doc = abs end
			pcall(function () bp:Unsplit(); end)
			splitBP = nil
			go_os.Stdout:WriteString(fmt.Sprintf("\027]0;t-code-open %d %d %s\007", (line * 1) + 1, (character * 1) + 1, doc))
			return false
		end
		buf, _ = buffer.NewBufferFromFile(doc)
		bp:AddTab()
		micro.CurPane():OpenBuffer(buf)
		buf:GetActiveCursor():GotoLoc(buffer.Loc(character * 1, line * 1))
		micro.CurPane():Center()
		return false
	end
end

function preSave(bp)
	if config.GetGlobalOption("lsp.formatOnSave") then
		onRune(bp)
		formatAction(bp, function ()
			bp:Save()
		end)
	end
end

function handleInitialized(buf, filetype)
	if cmd[filetype] == nil then return; end
	micro.Log("Found running lsp server for ", filetype, "firing textDocument/didOpen...")
	local send = withSend(filetype)
	local uri = getUriFromBuf(buf)
	local content = jsonEscape(util.String(buf:Bytes()))
	send("textDocument/didOpen", fmt.Sprintf('{"textDocument": {"uri": "%s", "languageId": "%s", "version": 1, "text": "%s"}}', uri, filetype, content), true)
end

function onBufferOpen(buf)
	local filetype = buf:FileType()
	micro.Log("ONBUFFEROPEN", filetype)
	if filetype ~= "unknown" and rootUri == "" and not cmd[filetype] then return startServer(filetype, handleInitialized); end
	if cmd[filetype] then
	    handleInitialized(buf, filetype)
	end
end

function contains(list, x)
	for _, v in pairs(list) do
		if v == x then return true; end
	end
	return false
end

function string.starts(String, Start)
	return string.sub(String, 1, #Start) == Start
end

function string.ends(String, End)
	return string.sub(String, #String - (#End - 1), #String) == End
end

function string.random(CharSet, Length, prefix)

   local _CharSet = CharSet or '.'

   if _CharSet == '' then
      return ''
   else
      local Result = prefix or ""
      math.randomseed(os.time())
      for Loop = 1,Length do
	      local char = math.random(1, #CharSet)
         Result = Result .. CharSet:sub(char,char)
      end

      return Result
   end
end

function string.parse(text)
	if not text:find('"jsonrpc":') then return {}; end
	local start,fin = text:find("\n%s*\n")
	local cleanedText = text
	if fin ~= nil then
		cleanedText = text:sub(fin)
	end
	local status, res = pcall(json.parse, cleanedText)
	if status then
		return res
	end
	return false
end

function isIgnoredMessage(msg)
	-- Return true if msg matches one of the ignored starts of messages
	-- Useful for linters that show spurious, hard to disable warnings
	local ignoreList = mysplit(config.GetGlobalOption("lsp.ignoreMessages"), "|")
	for i, ignore in pairs(ignoreList) do
		if string.match(msg, ignore) then -- match from start of string
			micro.Log("Ignore message: '", msg, "', because it matched: '", ignore, "'.")
			return true -- ignore this message, dont show to user
		end
	end
	return false -- show this message to user
end

local function handleMessage(filetype, raw)
	local status, data = pcall(json.parse, raw)
	if not status or type(data) ~= "table" then
		return
	end

	if data.method == "workspace/configuration" then
		local resp = fmt.Sprintf('{"jsonrpc": "2.0", "id": %.0f, "result": [{"enable": true}]}', data.id)
		shell.JobSend(cmd[filetype], fmt.Sprintf('Content-Length: %.0f\r\n\r\n%s', #resp, resp))
	elseif data.method == "textDocument/publishDiagnostics" or data.method == "textDocument\\/publishDiagnostics" then
		local pane = micro.CurPane()
		if not pane or not pane.Buf then return end
		local bp = pane.Buf
		
		local uri = getUriFromBuf(bp)
		if data.params and data.params.uri == uri then
			bp:ClearMessages("lsp")
			bp:AddMessage(buffer.NewMessage("lsp", "", buffer.Loc(0, 10000000), buffer.Loc(0, 10000000), buffer.MTInfo))
			for _, diagnostic in ipairs(data.params.diagnostics or {}) do
				local type = buffer.MTInfo
				if diagnostic.severity == 1 then
					type = buffer.MTError
				elseif diagnostic.severity == 2 then
					type = buffer.MTWarning
				end
				local mstart = buffer.Loc(diagnostic.range.start.character, diagnostic.range.start.line)
				local mend = buffer.Loc(diagnostic.range["end"].character, diagnostic.range["end"].line)
				if not isIgnoredMessage(diagnostic.message) then
					local msg = buffer.NewMessage("lsp", diagnostic.message, mstart, mend, type)
					bp:AddMessage(msg)
				end
			end
		end
	elseif currentAction[filetype] and currentAction[filetype].method and not data.method and currentAction[filetype].response and data.jsonrpc then
		local bp = micro.CurPane()
		local action = currentAction[filetype]
		currentAction[filetype] = {}
		action.response(bp, data)
	elseif data.method == "window/showMessage" or data.method == "window\\/showMessage" then
		if filetype == micro.CurPane().Buf:FileType() then
			micro.InfoBar():Message(data.params.message)
		else
			micro.Log(filetype .. " message " .. data.params.message)
		end
	elseif data.method == "window/logMessage" or data.method == "window\\/logMessage" then
		micro.Log(data.params.message)
	end
end

function onStdout(filetype)
	return function (text)
		message = message .. text
		while true do
			local hstart = message:find("Content%-Length:")
			if not hstart then
				if #message > 64 then message = "" end
				break
			end
			local _, body_start = message:find("\r?\n\r?\n", hstart)
			if not body_start then
				break
			end
			local len_str = message:sub(hstart, body_start):match("Content%-Length:%s*(%d+)")
			local content_len = tonumber(len_str)
			if not content_len then
				message = message:sub(body_start + 1)
			elseif #message < body_start + content_len then
				break
			else
				local body = message:sub(body_start + 1, body_start + content_len)
				message = message:sub(body_start + content_len + 1)
				handleMessage(filetype, body)
			end
		end
	end
end

function onStderr(text)
	micro.Log("ONSTDERR", text)
	--micro.InfoBar():Message(text)
end

function onExit(filetype)
	return function (str)
		currentAction[filetype] = nil
		cmd[filetype] = nil
		micro.Log("ONEXIT", filetype, str)
	end
end

-- the actual hover action request and response
-- the hoverActionResponse is hooked up in 
function hoverAction(bp)
	local filetype = bp.Buf:FileType()
	if cmd[filetype] ~= nil then
		local send = withSend(filetype)
		local file = getUriFromBuf(bp.Buf)
		local line = bp.Buf:GetActiveCursor().Y
		local char = bp.Buf:GetActiveCursor().X
		currentAction[filetype] = { method = "textDocument/hover", response = hoverActionResponse }
		send(currentAction[filetype].method, fmt.Sprintf('{"textDocument": {"uri": "%s"}, "position": {"line": %.0f, "character": %.0f}}', file, line, char))
	end
end

function hoverActionResponse(buf, data)
	if data.result and data.result.contents ~= nil and data.result.contents ~= "" then
		if data.result.contents.value then
			micro.InfoBar():Message(data.result.contents.value)
		elseif #data.result.contents > 0 then
			micro.InfoBar():Message(data.result.contents[1].value)
		end
	end
end

-- the definition action request and response
function definitionAction(bp)
	local filetype = bp.Buf:FileType()	
	if cmd[filetype] == nil then return; end
	
	local send = withSend(filetype)
	local file = bp.Buf.AbsPath
	local line = bp.Buf:GetActiveCursor().Y
	local char = bp.Buf:GetActiveCursor().X
	currentAction[filetype] = { method = "textDocument/definition", response = definitionActionResponse }
	send(currentAction[filetype].method, fmt.Sprintf('{"textDocument": {"uri": "%s"}, "position": {"line": %.0f, "character": %.0f}}', pathToUri(file), line, char))
end

function definitionActionResponse(bp, data)
	local results = data.result or data.partialResult
	if results == nil then return; end
	local file = bp.Buf.AbsPath
	if results.uri ~= nil then
		-- single result
		results = { results }
	end
	if #results <= 0 then return; end
	local uri = (results[1].uri or results[1].targetUri)
	local doc = uriToPath(uri)
	local buf = bp.Buf
	local range = results[1].range or results[1].targetSelectionRange
	if file ~= doc then
		if go_os.Getenv("TCODE_SOCKET") ~= "" then
			go_os.Stdout:WriteString(fmt.Sprintf("\027]0;t-code-open %d %d %s\007", range.start.line + 1, range.start.character + 1, doc))
			return
		end
		-- it's from a different file, so open it as a new tab
		buf, _ = buffer.NewBufferFromFile(doc)
		bp:AddTab()
		micro.CurPane():OpenBuffer(buf)
		-- shorten the displayed name in status bar
		name = buf:GetName()
    	local wd, _ = go_os.Getwd()
		if name:starts(wd) then
    		buf:SetName("." .. name:sub(#wd + 1, #name + 1))
		else 
		  if #name > 30 then
		     buf:SetName("..." .. name:sub(-30, #name + 1))
		  end
		end
	end
	buf:GetActiveCursor():GotoLoc(buffer.Loc(range.start.character, range.start.line))
	bp:Center()
end

function completionAction(bp)
	local filetype = bp.Buf:FileType()
	local send = withSend(filetype)
	local file = bp.Buf.AbsPath
	local line = bp.Buf:GetActiveCursor().Y
	local char = bp.Buf:GetActiveCursor().X

	if lastCompletion[1] == file and lastCompletion[2] == line and lastCompletion[3] == char then 
		completionCursor = completionCursor + 1
	else
		completionCursor = 0
		if bp.Cursor:HasSelection() then
			-- we have a selection
			-- assume we want to indent the selection
			bp:IndentSelection()
			return
		end
		if char == 0 then
			-- we are at the very first character of a line
			-- assume we want to indent
			bp:IndentLine()
			return
		end
		local cur = bp.Buf:GetActiveCursor()
		cur:SelectLine()
		local lineContent = util.String(cur:GetSelection())
		cur:ResetSelection()
		cur:GotoLoc(buffer.Loc(char, line))
		local startOfLine = "" .. lineContent:sub(1, char)
		if startOfLine:match("^%s+$") then
			-- we are at the beginning of a line
			-- assume we want to indent the line
			bp:IndentLine()
			return
		end
	end
	if cmd[filetype] == nil then return; end
	lastCompletion = {file, line, char}
	currentAction[filetype] = { method = "textDocument/completion", response = completionActionResponse }
	send(currentAction[filetype].method, fmt.Sprintf('{"textDocument": {"uri": "%s"}, "position": {"line": %.0f, "character": %.0f}}', pathToUri(file), line, char))
end

table.filter = function(t, filterIter)
  local out = {}

  for k, v in pairs(t) do
    if filterIter(v, k, t) then table.insert(out, v) end
  end

  return out
end

function findCommon(input, list)
	local commonLen = 0
	local prefixList = {}
	local str = input.textEdit and input.textEdit.newText or input.label
	for i = 1,#str,1 do
		local prefix = str:sub(1, i)
		prefixList[prefix] = 0
		for idx, entry in ipairs(list) do
			local currentEntry = entry.textEdit and entry.textEdit.newText or entry.label
			if currentEntry:starts(prefix) then
				prefixList[prefix] = prefixList[prefix] + 1
			end
		end
	end
	local longest = ""
	for idx, entry in pairs(prefixList) do
		if entry >= #list then
			if #longest < #idx then
				longest = idx
			end
		end
	end
	if #list == 1 then
		return list[1].textEdit and list[1].textEdit.newText or list[1].label
	end
	return longest
end

function completionActionResponse(bp, data)
	local results = data.result
	if results == nil then 
		return
	end
	if results.items then
		results = results.items
	end
	
	local xy = buffer.Loc(bp.Cursor.X, bp.Cursor.Y)
	local start = xy
	if bp.Cursor:HasSelection() then
		bp.Cursor:DeleteSelection()
	end

	local found = false
	local prefix = ""
	local reversed = ""
	-- if we have no defined ranges in the result
	-- try to find out what our prefix is we want to filter against
	if not results[1] or not results[1].textEdit or not results[1].textEdit.range then
		if capabilities[bp.Buf:FileType()] and capabilities[bp.Buf:FileType()].completionProvider and capabilities[bp.Buf:FileType()].completionProvider.triggerCharacters then
			local cur = bp.Buf:GetActiveCursor()
			cur:SelectLine()
			local lineContent = util.String(cur:GetSelection())
			reversed = string.reverse(lineContent:gsub("\r?\n$", ""):sub(1, xy.X))
			local triggerChars = capabilities[bp.Buf:FileType()].completionProvider.triggerCharacters
			for i = 1,#reversed,1 do
				local char = reversed:sub(i,i)
				-- try to find a trigger character or any other non-word character
				if contains(triggerChars, char) or contains({" ", ":", "/", "-", "\t", ";"}, char) then
					found = true
					start = buffer.Loc(#reversed - (i - 1), bp.Cursor.Y)
					bp.Cursor:SetSelectionStart(start)
					bp.Cursor:SetSelectionEnd(xy)
					prefix = util.String(cur:GetSelection())
					bp.Cursor:DeleteSelection()
					bp.Cursor:ResetSelection()
					break
				end
			end
			if not found then
				prefix = lineContent:gsub("\r?\n$", '')
			end
		end
		-- if we have found a prefix
		if prefix ~= "" then
		    -- filter it down to what is suggested by the prefix
			results = table.filter(results, function (entry)
				return entry.label:starts(prefix)
			end)
		end
	end

	table.sort(results, function (left, right)
		return (left.sortText or left.label) < (right.sortText or right.label)
	end)
	
	entry = results[(completionCursor % #results) + 1]
	-- if no matching results are found
	if entry == nil then 
	    -- reposition cursor and stop
		bp.Cursor:GotoLoc(xy)
		return
	end
	local commonStart = ''
	local toInsert = entry.textEdit and entry.textEdit.newText or entry.label
	local isTabCompletion = config.GetGlobalOption("lsp.tabcompletion")
	if isTabCompletion and not entry.textEdit then
		commonStart = findCommon(entry, results)
		bp.Buf:Insert(start, commonStart)
		if prefix ~= commonStart then
			return
		end
		start = buffer.Loc(start.X + #prefix, start.Y)
	else
		prefix = ''
	end

	if entry.textEdit and entry.textEdit.range then
		start = buffer.Loc(entry.textEdit.range.start.character, entry.textEdit.range.start.line)
		bp.Cursor:SetSelectionStart(start)
		bp.Cursor:SetSelectionEnd(xy)
		bp.Cursor:DeleteSelection()
		bp.Cursor:ResetSelection()
	elseif capabilities[bp.Buf:FileType()] and capabilities[bp.Buf:FileType()].completionProvider and capabilities[bp.Buf:FileType()].completionProvider.triggerCharacters then
		if not found then
			-- we found nothing - so assume we need the beginning of the line
			if reversed:starts(" ") or reversed:starts("\t") then
				-- if we end with some indentation, keep it
				start = buffer.Loc(#reversed, bp.Cursor.Y)
			else
				start = buffer.Loc(0, bp.Cursor.Y)
			end
			bp.Cursor:SetSelectionStart(start)
			bp.Cursor:SetSelectionEnd(xy)
			bp.Cursor:DeleteSelection()
			bp.Cursor:ResetSelection()
		end
	end
	local inserting = "" .. toInsert:gsub(prefix, "")
	bp.Buf:Insert(start, inserting)
	
	if #results > 1 then
		if entry.textEdit then
			bp.Cursor:GotoLoc(start)
			bp.Cursor:SetSelectionStart(start)
		else
			-- if we had to calculate everything outselves
			-- go back to the original location
			bp.Cursor:GotoLoc(xy)
			bp.Cursor:SetSelectionStart(xy)
		end
		bp.Cursor:SetSelectionEnd(buffer.Loc(start.X + #toInsert, start.Y))
	else
		bp.Cursor:GotoLoc(buffer.Loc(start.X + #inserting, start.Y))
	end
	
	local startLoc = buffer.Loc(0, 0)
	local endLoc = buffer.Loc(0, 0)	
	local msg = ''
	local insertion = ''
	if entry.detail or entry.documentation then
		insertion = fmt.Sprintf("%s", entry.detail or entry.documentation or '')
		for idx, result in ipairs(results) do
			if #msg > 0 then
				msg = msg .. "\n"
			end
			local insertion = fmt.Sprintf("%s %s", result.detail or '', result.documentation or '')
			if idx == (completionCursor % #results) + 1 then
				local msglines = mysplit(msg, "\n")
				startLoc = buffer.Loc(0, #msglines)
				endLoc = buffer.Loc(#insertion - 1, #msglines)
			end
			msg = msg .. insertion
		end
	else
		insertion = entry.label
		for idx, result in ipairs(results) do
			if #msg > 0 then
				local msglines = mysplit(msg, "\n")
				local lastLine = msglines[#msglines]
				local len = #result.label + 4
				if #lastLine + len >= bp:GetView().Width then
					msg = msg .. "\n  "
				else 
					msg = msg .. '  '
				end
			else
				msg = "  "
			end
			if idx == (completionCursor % #results) + 1 then
				local msglines = mysplit(msg, "\n")
				local prefixLen = 0
				if #msglines > 0 then
		    		prefixLen = #msglines[#msglines]
		    	else
		    		prefixLen = #msg
		    	end
				startLoc = buffer.Loc(prefixLen or 0, #msglines - 1)
				endLoc = buffer.Loc(prefixLen + #result.label, #msglines - 1)
			end
			msg = msg .. result.label
		end
	end
	if config.GetGlobalOption("lsp.autocompleteDetails") then
		if not splitBP then
			local tmpName = ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"):random(32)
			local logBuf = buffer.NewBuffer(msg, tmpName)
			splitBP = bp:HSplitBuf(logBuf)
			bp:NextSplit()
		else
			splitBP:SelectAll()
			splitBP.Cursor:DeleteSelection()
			splitBP.Cursor:ResetSelection()
			splitBP.Buf:insert(buffer.Loc(1, 1), msg)
		end
		splitBP.Cursor:ResetSelection()
		splitBP.Cursor:SetSelectionStart(startLoc)
		splitBP.Cursor:SetSelectionEnd(endLoc)
	else
		if entry.detail or entry.documentation then
			micro.InfoBar():Message(insertion)
		else
			local cleaned = " " .. msg:gsub("%s+", "  ")
			local replaced, _ = cleaned:gsub(".*%s" .. insertion .. "%s?", " [" .. insertion .. "] ")
			micro.InfoBar():Message(replaced)
		end
	end
end

function formatAction(bp, callback)
	local filetype = bp.Buf:FileType()
	if cmd[filetype] == nil then return; end
	local send = withSend(filetype)
	local file = bp.Buf.AbsPath

	currentAction[filetype] = { method = "textDocument/formatting", response = formatActionResponse(callback) }
	send(currentAction[filetype].method, fmt.Sprintf('{"textDocument": {"uri": "%s"}, "options": {"tabSize": 4, "insertSpaces": true}}', pathToUri(file)))
end

function formatActionResponse(callback)
	return function (bp, data)
		if data.result == nil then return; end
		local edits = data.result
		-- make sure we apply the changes from back to front
		-- this allows for changes to not need position updates
		table.sort(edits, function (left, right)
			-- go by lines first
			return left.range['end'].line > right.range['end'].line or 
				-- if lines match, go by end character
				left.range['end'].line == right.range['end'].line and left.range['end'].character > right.range['end'].character or
				-- if they match too, go by start character
				left.range['end'].line == right.range['end'].line and left.range['end'].character == right.range['end'].character and left.range.start.line == left.range['end'].line and left.range.start.character > right.range.start.character
		end)

		-- save original cursor position
		local xy = buffer.Loc(bp.Cursor.X, bp.Cursor.Y)
		for _idx, edit in ipairs(edits) do
			rangeStart = buffer.Loc(edit.range.start.character, edit.range.start.line)
			rangeEnd = buffer.Loc(edit.range['end'].character, edit.range['end'].line)
			-- apply each change
			bp.Cursor:GotoLoc(rangeStart)
			bp.Cursor:SetSelectionStart(rangeStart)
			bp.Cursor:SetSelectionEnd(rangeEnd)
			bp.Cursor:DeleteSelection()
			bp.Cursor:ResetSelection()
			
			if edit.newText ~= "" then
				bp.Buf:insert(rangeStart, edit.newText)
			end
		end
		-- put the cursor back where it was
		bp.Cursor:GotoLoc(xy)
		-- if any changes were applied
		if #edits > 0 then
			-- tell the server about the changed document
			onRune(bp)
		end

		if callback ~= nil then
			callback(bp)
		end
	end
end

-- the references action request and response
function referencesAction(bp)
	local filetype = bp.Buf:FileType()	
	if cmd[filetype] == nil then return; end
	
	local send = withSend(filetype)
	local file = bp.Buf.AbsPath
	local line = bp.Buf:GetActiveCursor().Y
	local char = bp.Buf:GetActiveCursor().X
	currentAction[filetype] = { method = "textDocument/references", response = referencesActionResponse }
	send(currentAction[filetype].method, fmt.Sprintf('{"textDocument": {"uri": "%s"}, "position": {"line": %.0f, "character": %.0f}, "context": {"includeDeclaration":true}}', pathToUri(file), line, char))
end

function referencesActionResponse(bp, data)
	if data.result == nil then return; end
	local results = data.result or data.partialResult
	if results == nil or #results <= 0 then return; end

	local file = bp.Buf.AbsPath
	
	local msg = ''
	for _idx, ref in ipairs(results) do
		if msg ~= '' then msg = msg .. '\n'; end
		local doc = uriToPath(ref.uri or ref.targetUri)
		local root = uriToPath(rootUri) .. "/"
		if doc:sub(1, #root) == root then
			doc = "./" .. doc:sub(#root + 1)
		end
		msg = msg .. doc .. ":" .. ref.range.start.line .. ":" .. ref.range.start.character
	end

	local logBuf = buffer.NewBuffer(msg, "References found")
	local splitBP = bp:HSplitBuf(logBuf)
end

--
-- @TODO implement additional functions here...
--



--
-- JSON
--
-- Internal functions.

local function kind_of(obj)
  if type(obj) ~= 'table' then return type(obj) end
  local i = 1
  for _ in pairs(obj) do
    if obj[i] ~= nil then i = i + 1 else return 'table' end
  end
  if i == 1 then return 'table' else return 'array' end
end

local function escape_str(s)
  local in_char  = {'\\', '"', '/', '\b', '\f', '\n', '\r', '\t'}
  local out_char = {'\\', '"', '/',  'b',  'f',  'n',  'r',  't'}
  for i, c in ipairs(in_char) do
    s = s:gsub(c, '\\' .. out_char[i])
  end
  return s
end

-- Returns pos, did_find; there are two cases:
-- 1. Delimiter found: pos = pos after leading space + delim; did_find = true.
-- 2. Delimiter not found: pos = pos after leading space;     did_find = false.
-- This throws an error if err_if_missing is true and the delim is not found.
local function skip_delim(str, pos, delim, err_if_missing)
  pos = pos + #str:match('^%s*', pos)
  if str:sub(pos, pos) ~= delim then
    if err_if_missing then
      error('Expected ' .. delim .. ' near position ' .. pos)
    end
    return pos, false
  end
  return pos + 1, true
end

-- Expects the given pos to be the first character after the opening quote.
-- Returns val, pos; the returned pos is after the closing quote character.
local esc_map = {b = '\b', f = '\f', n = '\n', r = '\r', t = '\t'}

local function utf8_char(code)
  if code < 0x80 then return string.char(code) end
  if code < 0x800 then
    return string.char(0xC0 + math.floor(code / 0x40), 0x80 + code % 0x40)
  end
  if code < 0x10000 then
    return string.char(0xE0 + math.floor(code / 0x1000), 0x80 + math.floor(code / 0x40) % 0x40,
                       0x80 + code % 0x40)
  end
  return string.char(0xF0 + math.floor(code / 0x40000), 0x80 + math.floor(code / 0x1000) % 0x40,
                     0x80 + math.floor(code / 0x40) % 0x40, 0x80 + code % 0x40)
end

local function parse_str_val(str, pos)
  local early_end_error = 'End of input found while parsing string.'
  local parts = {}
  local len = #str
  while pos <= len do
    local c = str:sub(pos, pos)
    if c == '"' then
      return table.concat(parts), pos + 1
    elseif c == '\\' then
      local nextc = str:sub(pos + 1, pos + 1)
      if nextc == '' then error(early_end_error) end
      local code = nextc == 'u' and tonumber(str:sub(pos + 2, pos + 5), 16)
      if code then
        pos = pos + 6
        -- A surrogate pair: \uD83D\uDE00
        local low = code >= 0xD800 and code < 0xDC00 and str:sub(pos, pos + 1) == '\\u' and
                    tonumber(str:sub(pos + 2, pos + 5), 16)
        if low and low >= 0xDC00 and low < 0xE000 then
          code = 0x10000 + (code - 0xD800) * 0x400 + (low - 0xDC00)
          pos = pos + 6
        end
        parts[#parts + 1] = utf8_char(code)
      else
        parts[#parts + 1] = esc_map[nextc] or nextc
        pos = pos + 2
      end
    else
      local next_special = str:find('["\\]', pos)
      if not next_special then error(early_end_error) end
      parts[#parts + 1] = str:sub(pos, next_special - 1)
      pos = next_special
    end
  end
  error(early_end_error)
end

-- Returns val, pos; the returned pos is after the number's final character.
local function parse_num_val(str, pos)
  local num_str = str:match('^-?%d+%.?%d*[eE]?[+-]?%d*', pos)
  local val = tonumber(num_str)
  if not val then error('Error parsing number at position ' .. pos .. '.') end
  return val, pos + #num_str
end

json.null = {}  -- This is a one-off table to represent the null value.

function json.parse(str, pos, end_delim)
  pos = pos or 1
  if pos > #str then error('Reached unexpected end of input.' .. str) end
  local pos = pos + #str:match('^%s*', pos)  -- Skip whitespace.
  local first = str:sub(pos, pos)
  if first == '{' then  -- Parse an object.
    local obj, key, delim_found = {}, true, true
    pos = pos + 1
    while true do
      key, pos = json.parse(str, pos, '}')
      if key == nil then return obj, pos end
      if not delim_found then error('Comma missing between object items.') end
      pos = skip_delim(str, pos, ':', true)  -- true -> error if missing.
      obj[key], pos = json.parse(str, pos)
      pos, delim_found = skip_delim(str, pos, ',')
    end
  elseif first == '[' then  -- Parse an array.
    local arr, val, delim_found = {}, true, true
    pos = pos + 1
    while true do
      val, pos = json.parse(str, pos, ']')
      if val == nil then return arr, pos end
      if not delim_found then error('Comma missing between array items.') end
      arr[#arr + 1] = val
      pos, delim_found = skip_delim(str, pos, ',')
    end
  elseif first == '"' then  -- Parse a string.
    return parse_str_val(str, pos + 1)
  elseif first == '-' or first:match('%d') then  -- Parse a number.
    return parse_num_val(str, pos)
  elseif first == end_delim then  -- End of an object or array.
    return nil, pos + 1
  else  -- Parse true, false, or null.
    local literals = {['true'] = true, ['false'] = false, ['null'] = json.null}
    for lit_str, lit_val in pairs(literals) do
      local lit_end = pos + #lit_str - 1
      if str:sub(pos, lit_end) == lit_str then return lit_val, lit_end + 1 end
    end
    local pos_info_str = 'position ' .. pos .. ': ' .. str:sub(pos, pos + 10)
    error('Invalid json syntax starting at ' .. pos_info_str .. ': ' .. str)
  end
end
