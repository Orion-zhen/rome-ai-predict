local available, native = pcall(require, "rome_ai_predict_native")
local P, active = {}, nil
local tag = "rome_ai_predict"
local function status(env, value) env.engine.context:set_property(tag .. "/status", value) end
local function owns(env)
  local ctx = env.engine.context
  if not env.showing or ctx.input ~= "" or ctx.composition:empty() then return false end
  return ctx.composition:toSegmentation():get_at(0):has_tag(tag)
end
local function watch(env)
  local ctx = env.engine.context
  if env.host and not ctx:is_composing() and not ctx:get_option("ascii_mode") then
    native.watch(env.host, ctx:get_property("client_app"), ctx:get_property("client_type"))
  end
end
local function clear(env)
  local ctx, owned = env.engine.context, owns(env)
  env.internal, env.showing, env.count = true, false, 0
  if owned then ctx:clear() end
  if env.options then
    ctx:set_option("soft_cursor", env.options.soft_cursor)
    ctx:set_option("_auto_commit", env.options.auto_commit)
    env.options = nil
  end
  env.internal = false
  -- 选词交接期间仍使用已授权的菜单投影，直到提交或取消。
  if env.host and not env.choosing then native.updated(env.host, ctx:is_composing()) end
end
local function stop(env)
  env.running, env.choosing = false, false
  if env.host then native.cancel(env.host) end
  clear(env)
end
local function open(env, enabled)
  if not available then status(env, "unavailable"); return end
  local reason
  env.host, reason = native.open(rime_api.get_user_data_dir(), enabled)
  status(env, env.host and "idle" or reason)
end
local function reload(env, enabled)
  stop(env)
  if env.host then native.close(env.host); env.host = nil end
  open(env, enabled)
  env.internal = true
  env.engine.context:set_option(tag, env.host ~= nil)
  env.internal = false
  watch(env)
end
local function choose(env, index)
  if not owns(env) or index < 1 or index > env.count then return false end
  env.choosing = true
  native.select(env.host, index)
  clear(env)
  return true
end
function P.init(env)
  env.running, env.showing, env.choosing, env.internal = false, false, false, false
  env.initialized, env.count = false, 0
  open(env)
  local ctx = env.engine.context
  env.commit_connection = ctx.commit_notifier:connect(function(context)
    if env.internal or active ~= env or not env.running or not env.host then return end
    -- formatter 若改写上屏内容，UIA 核对会失败，不以提交历史替代应用正文。
    local text = context:get_commit_text()
    if text ~= "" then native.committed(env.host, text) else stop(env) end
  end)
  env.update_connection = ctx.update_notifier:connect(function(context)
    if env.internal or active ~= env or not env.host or owns(env) then return end
    if env.choosing and not context:is_composing() then return end
    native.updated(env.host, context:is_composing())
  end)
  env.property_connection = ctx.property_update_notifier:connect(function(_, name)
    if name == "client_app" or name == "client_type" then watch(env) end
  end)
  env.selection = ctx.select_notifier:connect(function(context)
    if owns(env) then choose(env, context.composition:toSegmentation():get_at(0).selected_index + 1) end
  end)
  env.option_connection = ctx.option_update_notifier:connect(function(context, name)
    if env.internal then return end
    if name == tag then
      if env.initialized then reload(env, context:get_option(tag)) end
    else
      stop(env)
      if not env.initialized then watch(env) end
    end
  end)
  watch(env)
end
function P.fini(env)
  if active == env then active = nil end
  env.commit_connection:disconnect()
  env.update_connection:disconnect()
  env.property_connection:disconnect()
  env.selection:disconnect()
  env.option_connection:disconnect()
  if env.host then native.close(env.host); env.host = nil end
end
local function receive(env)
  local event = native.poll(env.host)
  if not event then return end
  local ctx = env.engine.context
  status(env, event.state)
  if event.state == "showing" then
    if active ~= env or not env.running or ctx:is_composing() or #event.tokens == 0 then stop(env); return end
    env.internal = true
    env.options = {soft_cursor = ctx:get_option("soft_cursor"), auto_commit = ctx:get_option("_auto_commit")}
    ctx:set_option("soft_cursor", false)
    ctx:set_option("_auto_commit", false)
    local menu = Menu()
    menu:add_translation(Translation(function()
      for _, text in ipairs(event.tokens) do yield(Candidate(tag, 0, 0, text, "AI")) end
    end))
    menu:prepare(#event.tokens)
    local segment = Segment(0, 0)
    segment.status, segment.tags, segment.menu = "kGuess", Set({tag, "placeholder", "phony"}), menu
    ctx.composition:push_back(segment)
    env.count, env.showing, env.internal = #event.tokens, true, false
    native.displayed(env.host)
  elseif event.state == "commit" then
    if active ~= env or not env.running or not env.choosing or ctx:is_composing() then stop(env); return end
    env.choosing, env.internal = false, true
    env.engine:commit_text(event.commit)
    env.internal = false
    native.committed(env.host, event.commit)
    native.updated(env.host, ctx:is_composing())
  elseif event.state == "cancelled" or event.state == "timeout" or event.state == "unavailable" or
         event.state == "api-error" or event.state == "error" then
    env.running, env.choosing = false, false
    clear(env)
  end
end
function P.func(key, env)
  local repr = key:repr()
  local ctx = env.engine.context
  if key:release() then
    -- 方向键等不一定触发 update_notifier，松键时恢复可预采集的空闲状态。
    if env.host and active == env and not env.choosing and not owns(env) then
      native.updated(env.host, ctx:is_composing())
    end
    return (env.host and repr == "Release+F24") and 1 or 2
  end
  if not env.initialized then
    -- Schema 的 switches/reset 已完成，再应用配置的启动状态。
    env.initialized, env.internal = true, true
    ctx:set_option(tag, env.host ~= nil)
    env.internal = false
  end
  if repr == "F24" and env.host then receive(env); return 1 end
  if active and active ~= env then stop(active) end
  active = env
  if not env.host then return 2 end
  if ctx:get_option("ascii_mode") or key:ctrl() or key:alt() or key:super() then stop(env); return 2 end
  if owns(env) and not key:shift() then
    if repr == "Tab" and choose(env, 1) then return 1 end
    local index = tonumber(repr) or tonumber(repr:match("^KP_([1-9])$"))
    if index and choose(env, index) then return 1 end
    if repr == "Escape" then stop(env); return 1 end
  end
  if env.showing or env.choosing then stop(env) end
  if not ctx:is_composing() then
    native.arm(env.host, ctx:get_property("client_app"), ctx:get_property("client_type"))
    env.running = true
  end
  return 2
end
return P
