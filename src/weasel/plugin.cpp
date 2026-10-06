#include "session.h"
#include <lua.hpp>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace {
using namespace rome::weasel;
constexpr auto sessionType = "rome.weasel.session";
using Holder = std::unique_ptr<Session>;
Holder& holder(lua_State* state) { return *static_cast<Holder*>(luaL_checkudata(state, 1, sessionType)); }
Session& session(lua_State* state) {
    auto& value = holder(state);
    if (!value) luaL_error(state, "closed rome-ai-predict session");
    return *value;
}
std::filesystem::path defaultsPath() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&defaultsPath), &module))
        throw std::runtime_error("cannot locate module");
    wchar_t path[32768];
    const auto length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!length || length == std::size(path)) throw std::runtime_error("cannot locate module defaults");
    return std::filesystem::path(std::wstring(path, length)).parent_path() / L"rome-ai-predict.defaults.yaml";
}
int open(lua_State* state) {
    const auto* userDirectory = luaL_checkstring(state, 1);
    try {
        // librime GetUserDataDir 返回 path::string()，采用宿主 ACP，而非固定 UTF-8。
        auto settings = rome::loadSettings(defaultsPath(), std::filesystem::path(userDirectory) / L"rome-ai-predict.yaml");
        if (lua_type(state, 2) != LUA_TNONE && lua_type(state, 2) != LUA_TNIL)
            settings.enabled = lua_toboolean(state, 2) != 0;
        if (!settings.enabled) { lua_pushnil(state); lua_pushliteral(state, "disabled"); return 2; }
        rome::validateSettings(settings);
        auto value = static_cast<Holder*>(lua_newuserdatauv(state, sizeof(Holder), 0));
        std::construct_at(value);
        luaL_setmetatable(state, sessionType);
        *value = std::make_unique<Session>(std::move(settings), createWindowsHost());
        return 1;
    } catch (const std::exception&) {
        lua_pushnil(state); lua_pushliteral(state, "configuration-error"); return 2;
    }
}
int close(lua_State* state) { holder(state).reset(); return 0; }
int destroy(lua_State* state) { std::destroy_at(&holder(state)); return 0; }
int arm(lua_State* state) {
    auto& value = session(state);
    const auto* app = luaL_optstring(state, 2, "");
    const auto* type = luaL_optstring(state, 3, "");
    value.arm(app, type); return 0;
}
int watch(lua_State* state) {
    auto& value = session(state);
    const auto* app = luaL_optstring(state, 2, "");
    const auto* type = luaL_optstring(state, 3, "");
    value.watch(app, type); return 0;
}
int updated(lua_State* state) {
    session(state).send({lua_toboolean(state, 2) ? Command::Composing : Command::Idle, {}});
    return 0;
}
int displayed(lua_State* state) { session(state).send({Command::Displayed, {}}); return 0; }
int cancel(lua_State* state) { session(state).send({Command::Cancel, {}}); return 0; }
int committed(lua_State* state) {
    auto& value = session(state);
    size_t length = 0;
    const char* text = luaL_checklstring(state, 2, &length);
    value.send({Command::Commit, std::string(text, length)}); return 0;
}
int select(lua_State* state) {
    auto& value = session(state);
    const auto index = luaL_checkinteger(state, 2);
    if (index >= 1 && index <= 9) value.send({Command::Select, {}, static_cast<size_t>(index - 1)});
    return 0;
}
int poll(lua_State* state) {
    auto event = session(state).take();
    if (!event) return 0;
    lua_createtable(state, 0, 3);
    lua_pushlstring(state, event->state.data(), event->state.size()); lua_setfield(state, -2, "state");
    lua_pushlstring(state, event->commit.data(), event->commit.size()); lua_setfield(state, -2, "commit");
    lua_createtable(state, static_cast<int>(event->tokens.size()), 0);
    for (size_t i = 0; i < event->tokens.size(); ++i) {
        lua_pushlstring(state, event->tokens[i].data(), event->tokens[i].size());
        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(state, -2, "tokens"); return 1;
}
}
extern "C" __declspec(dllexport) int luaopen_rome_ai_predict(lua_State* state) {
    luaL_newmetatable(state, sessionType);
    lua_pushcfunction(state, destroy); lua_setfield(state, -2, "__gc");
    const luaL_Reg functions[] = {{"open", open}, {"close", close}, {"arm", arm}, {"cancel", cancel},
        {"committed", committed}, {"select", select}, {"poll", poll}, {"displayed", displayed},
        {"watch", watch}, {"updated", updated}, {nullptr, nullptr}};
    luaL_newlib(state, functions);
    return 1;
}
