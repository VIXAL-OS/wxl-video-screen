local players = {
  cinema = {action = WXLFilmCinemaAction, state = WXLFilmCinemaState},
  plex = {action = WXLFilmPlexAction, state = WXLFilmPlexState}
}
local active,kind,offset=nil,"server",0
local function filmAction(op,text,value)
  if players[op] then
    local nextPlayer=players[op]
    if not nextPlayer.action or not nextPlayer.state then return false,"This player mod is not installed or failed to load." end
    local ok,msg=nextPlayer.action("open","",0)
    if ok then
      if active and active~=op then players[active].action("close","",0) end
      active=op
    end
    return ok,msg
  end
  if not active then return false,"Choose a player first." end
  local ok,msg=players[active].action(op,text,value)
  if op=="close" and ok then active=nil end
  return ok,msg
end
local function filmState()
  if not active then return {player="closed",rows={},total=0} end
  local alive,placed,status,screen,err,partyError,room,total,a,b,c,d,e=players[active].state(kind,offset)
  return {player=alive and active or "closed",placed=placed,status=status,screen=screen,error=err,
          partyError=partyError,room=room,total=total or 0,rows={a,b,c,d,e}}
end
local panel = CreateFrame("Frame", "WXLFilmClubPanel", UIParent)
panel:SetSize(610, 446)
panel:SetPoint("CENTER")
panel:SetFrameStrata("DIALOG")
panel:EnableMouse(true)
panel:SetMovable(true)
panel:RegisterForDrag("LeftButton")
panel:SetScript("OnDragStart", panel.StartMoving)
panel:SetScript("OnDragStop", panel.StopMovingOrSizing)
panel:SetBackdrop({bgFile="Interface\\DialogFrame\\UI-DialogBox-Background",
  edgeFile="Interface\\DialogFrame\\UI-DialogBox-Border", tile=true, tileSize=32, edgeSize=24,
  insets={left=5,right=5,top=5,bottom=5}})
panel:Hide()
local function label(x,y,font)
  local t=panel:CreateFontString(nil,"OVERLAY",font or "GameFontHighlight")
  t:SetPoint("TOPLEFT",x,-y)
  t:SetJustifyH("LEFT")
  return t
end
label(18,14,"GameFontNormalLarge"):SetText("Film Club")
local feedback=label(18,401,"GameFontHighlightSmall")
feedback:SetWidth(570)
local status=label(18,367)
status:SetWidth(570)
local function button(name,x,y,w,fn)
  local b=CreateFrame("Button",nil,panel,"UIPanelButtonTemplate")
  b:SetPoint("TOPLEFT",x,-y)
  b:SetSize(w,23)
  b:SetText(name)
  b:SetScript("OnClick",fn)
  return b
end
local entry=CreateFrame("EditBox",nil,panel,"InputBoxTemplate")
entry:SetPoint("TOPLEFT",23,-78)
entry:SetSize(416,25)
entry:SetAutoFocus(false)
entry:SetMaxLetters(1023)
entry:SetScript("OnEscapePressed",function(self) self:ClearFocus() end)
local function run(op,text,value)
  local ok,msg=filmAction(op,text or "",value or 0)
  feedback:SetText(msg and msg~="" and msg or (ok and "" or "Command unavailable."))
  entry:ClearFocus()
end
local function pick(k) kind,offset=k,0 end
local cinemaButton=button("Video Cinema",16,44,108,function() run("cinema") end)
local plexButton=button("Azeroth Plex",128,44,108,function() pick("server") run("plex") end)
if not players.cinema.action then cinemaButton:Disable() end
if not players.plex.action then plexButton:Disable() end
button("Place screen",252,44,104,function() run("place") end)
button("Hide screen",360,44,98,function() run("hide") end)
button("End session",474,44,112,function() run("close") end)
button("Hide panel",492,9,94,function() panel:Hide() end)
local function submit()
  local s=filmState()
  if s.player=="plex" then pick("item") run("search",entry:GetText()) else run("load",entry:GetText()) end
end
entry:SetScript("OnEnterPressed",submit)
local load=button("Load video",448,79,138,submit)
button("Play",16,110,62,function() run("play") end)
button("Pause",82,110,62,function() run("pause") end)
button("Stop",148,110,62,function() run("stop") end)
local vol=80
local level=label(294,116)
level:SetText("Volume 80%")
local function volume(delta)
  vol=math.max(0,math.min(100,vol+delta))
  run("volume","",vol)
  level:SetText("Volume "..vol.."%")
end
button("-",250,110,30,function() volume(-10) end)
button("+",382,110,30,function() volume(10) end)
local plex={}
local function pb(...)
  local b=button(...)
  plex[#plex+1]=b
  return b
end
pb("Sign in",16,149,74,function() run("login") end)
pb("Servers",94,149,78,function() pick("server") end)
pb("Libraries",176,149,78,function() pick("library") end)
pb("Titles",258,149,68,function() pick("item") end)
pb("Back",330,149,62,function() run("back") pick("item") end)
pb("Prev page",398,149,92,function() run("prev") pick("item") end)
pb("Next page",494,149,92,function() run("next") pick("item") end)
local rows={}
for i=1,5 do
  local row=pb("",16,177+(i-1)*24,482,function(self)
    if self.command then
      run(self.command,"",self.index)
      if self.command=="server" then pick("library") elseif self.command=="library" then pick("item") end
    end
  end)
  rows[i]=row
end
pb("Up",506,177,80,function() offset=math.max(0,offset-5) end)
pb("Down",506,201,80,function()
  local total=filmState().total
  if offset+5<total then offset=offset+5 end
end)
pb("Create party",16,307,104,function() run("create") end)
pb("Join code",124,307,92,function() run("join",entry:GetText()) end)
pb("Leave party",220,307,98,function() run("leave") end)
pb("Join CyTube",330,307,122,function() run("channel",entry:GetText()) end)
pb("Leave channel",456,307,130,function() run("leavechannel") end)
local hint=label(18,340,"GameFontHighlightSmall")
local function refresh()
  local s=filmState()
  local isPlex=s.player=="plex"
  for _,b in ipairs(plex) do if isPlex then b:Show() else b:Hide() end end
  load:SetText(isPlex and "Search Plex" or "Load video")
  hint:SetText(isPlex and "Enter a search, party code, or CyTube channel above." or
    "Choose Video Cinema, enter a YouTube link, then use Load video and Place screen.")
  if offset>0 and offset>=s.total then offset=0 s=filmState() end
  local list=s.rows
  if isPlex then
    for i,row in ipairs(rows) do
      local index=i+offset
      if list[i] then
        row.command,row.index=kind,index
        row:SetText(list[i]:gsub("|","||"))
        row:Show()
      else row.command=nil row:Hide() end
    end
  end
  local detail=(s.error and s.error~="" and s.error) or (s.partyError and s.partyError~="" and s.partyError) or s.status or ""
  if s.room and s.room~="" then detail=detail.."  Party "..s.room end
  if s.screen and s.screen~="" then detail=detail.."  "..s.screen end
  status:SetText(detail:gsub("|","||"))
end
if not players.cinema.action and not players.plex.action then feedback:SetText("Install a WarcraftXL x64 Film Club mod to enable playback.") end
local elapsed=0
panel:SetScript("OnUpdate",function(_,dt)
  elapsed=elapsed+dt
  if elapsed>=0.35 then elapsed=0 refresh() end
end)
panel:SetScript("OnShow",refresh)
SLASH_WXLFILMCLUB1="/film"
SlashCmdList.WXLFILMCLUB=function()
  if panel:IsShown() then panel:Hide() else panel:Show() end
end
