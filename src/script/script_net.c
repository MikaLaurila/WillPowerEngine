/*
 * Luau bindings for multiplayer games (wpe.net_*, docs/scripting.md and docs/networking.md), and the
 * conversion of script values to and from the network value format (network/value.h).
 *
 * Bindings follow the error-safety rule of script_internal.h: arguments are validated (may raise)
 * before the application is asked to do anything. Encoding writes into the host's scratch buffer,
 * which the host owns for its whole life, so raising half-way leaks nothing.
 */
#define WPE_LOG_MODULE "script"
#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "network/transport.h"
#include "network/value.h"
#include "script/script_internal.h"

#define WPE__NET_NAME_MAX 32u
/* Room for the message header the application adds (target, sender, name). */
#define WPE__NET_VALUE_MAX (WPE_NET_MAX_MESSAGE_SIZE - 64u)
#define WPE__NET_MAX_LISTED 32u

/* ---- Helpers --------------------------------------------------------------------------------- */

static const WPE_ScriptNet* wpe__net(lua_State* L, bool available, const char* function) {
    if (!available) luaL_errorL(L, "wpe.%s is not available in this application", function);
    return &wpe__script_host_from(L)->app.net;
}

static void* wpe__net_user(lua_State* L) {
    return wpe__script_host_from(L)->app.user_data;
}

static WPE_Entity wpe__net_entity(lua_State* L, int index) {
    const double value = luaL_checknumber(L, index);
    if (!(value >= 0.0 && value <= (double)UINT32_MAX && value == floor(value)))
        luaL_errorL(L, "argument #%d is not an entity handle", index);
    const WPE_Entity entity = (WPE_Entity)value;
    if (!wpe_ecs_is_alive(wpe__script_host_from(L)->ecs, entity))
        luaL_errorL(L, "argument #%d: entity 0x%08x is not alive", index, entity);
    return entity;
}

static uint32_t wpe__net_player(lua_State* L, int index) {
    const double value = luaL_checknumber(L, index);
    if (!(value >= 0.0 && value < 8.0 && value == floor(value)))
        luaL_errorL(L, "argument #%d: players are numbered 0 (the host) to 7", index);
    return (uint32_t)value;
}

/* The `name` field of an options table, or NULL. */
static const char* wpe__option_string(lua_State* L, int index, const char* key) {
    if (lua_isnoneornil(L, index)) return NULL;
    luaL_checktype(L, index, LUA_TTABLE);
    lua_getfield(L, index, key);
    const char* text = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
    lua_pop(L, 1); /* The string stays alive: the table holds it. */
    return text;
}

static bool wpe__option_bool(lua_State* L, int index, const char* key, bool fallback) {
    if (lua_isnoneornil(L, index)) return fallback;
    luaL_checktype(L, index, LUA_TTABLE);
    lua_getfield(L, index, key);
    const bool value = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

/* ---- Values: Luau -> wire ---------------------------------------------------------------------- */

static void wpe__encode(lua_State* L, int index, WPE_BitWriter* w, int depth, bool key);

static void wpe__encode_table(lua_State* L, int index, WPE_BitWriter* w, int depth) {
    if (depth >= (int)WPE_NETVALUE_MAX_DEPTH)
        luaL_errorL(L, "wpe.net_send: the value nests tables more than %u deep (or contains itself)",
                    WPE_NETVALUE_MAX_DEPTH);
    luaL_checkstack(L, 4, "wpe.net_send");
    uint32_t entries = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        lua_pop(L, 1);
        ++entries;
    }
    wpe_netvalue_write_table(w, entries);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        const int top = lua_gettop(L);
        wpe__encode(L, top - 1, w, depth + 1, true);
        wpe__encode(L, top, w, depth + 1, false);
        lua_pop(L, 1);
    }
}

static void wpe__encode(lua_State* L, int index, WPE_BitWriter* w, int depth, bool key) {
    switch (lua_type(L, index)) {
    case LUA_TNIL:
        wpe_netvalue_write_nil(w);
        break;
    case LUA_TBOOLEAN:
        wpe_netvalue_write_bool(w, lua_toboolean(L, index));
        break;
    case LUA_TNUMBER:
        wpe_netvalue_write_number(w, lua_tonumber(L, index));
        break;
    case LUA_TSTRING: {
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        wpe_netvalue_write_string(w, text, length);
        break;
    }
    case LUA_TVECTOR: {
        if (key) luaL_errorL(L, "wpe.net_send: table keys must be strings, numbers or booleans");
        const float* v = lua_tovector(L, index);
        wpe_netvalue_write_vector(w, wpe_vec3(v[0], v[1], v[2]));
        break;
    }
    case LUA_TTABLE:
        if (key) luaL_errorL(L, "wpe.net_send: table keys must be strings, numbers or booleans");
        wpe__encode_table(L, index < 0 ? lua_gettop(L) + index + 1 : index, w, depth);
        break;
    default:
        luaL_errorL(L,
                    "wpe.net_send: a %s can't be sent (only nil, booleans, numbers, strings, vectors and "
                    "tables of them)",
                    luaL_typename(L, index));
    }
    if (w->error)
        luaL_errorL(L, "wpe.net_send: the value is too large (at most %u bytes encoded)", WPE__NET_VALUE_MAX);
}

/* ---- Values: wire -> Luau ---------------------------------------------------------------------- */

/* Pushes the next value (with everything in it). The reader has checked depth, counts and keys. */
static bool wpe__push_decoded(lua_State* L, WPE_NetValueReader* r) {
    WPE_NetValue v;
    if (!wpe_netvalue_read(r, &v)) return false;
    luaL_checkstack(L, 4, "network value");
    switch (v.type) {
    case WPE_NETVALUE_NIL:
        lua_pushnil(L);
        return true;
    case WPE_NETVALUE_BOOL:
        lua_pushboolean(L, v.boolean);
        return true;
    case WPE_NETVALUE_NUMBER:
        lua_pushnumber(L, v.number);
        return true;
    case WPE_NETVALUE_STRING:
        lua_pushlstring(L, v.string, v.length);
        return true;
    case WPE_NETVALUE_VECTOR:
        lua_pushvector(L, v.vector.x, v.vector.y, v.vector.z);
        return true;
    case WPE_NETVALUE_TABLE: {
        lua_createtable(L, 0, (int)v.entries); /* Bounded by the message's size. */
        const int table = lua_gettop(L);
        for (uint32_t i = 0; i < v.entries; ++i) {
            const bool key = wpe__push_decoded(L, r);
            if (!key) return false;
            const bool value = wpe__push_decoded(L, r);
            if (!value) return false;
            if (lua_isnil(L, -1)) {
                lua_pop(L, 2); /* A nil value: the key stays absent, as in Luau. */
                continue;
            }
            lua_rawset(L, table);
        }
        return true;
    }
    }
    return false;
}

/* ---- Bindings -------------------------------------------------------------------------------- */

static int wpe__api_net_host(lua_State* L) {
    const WPE_ScriptNet* net = wpe__net(L, wpe__script_host_from(L)->app.net.host != NULL, "net_host");
    const char* name = wpe__option_string(L, 1, "name");
    const bool hidden = wpe__option_bool(L, 1, "hidden", false);
    lua_pushboolean(L, net->host(wpe__net_user(L), name, hidden) == WPE_RESULT_OK);
    return 1;
}

static int wpe__api_net_join(lua_State* L) {
    const WPE_ScriptNet* net = wpe__net(L, wpe__script_host_from(L)->app.net.join != NULL, "net_join");
    const char* address = luaL_checkstring(L, 1);
    const char* name = wpe__option_string(L, 2, "name");
    lua_pushboolean(L, net->join(wpe__net_user(L), address, name) == WPE_RESULT_OK);
    return 1;
}

static int wpe__api_net_leave(lua_State* L) {
    const WPE_ScriptNet* net = wpe__net(L, wpe__script_host_from(L)->app.net.leave != NULL, "net_leave");
    net->leave(wpe__net_user(L));
    return 0;
}

static int wpe__api_net_status(lua_State* L) {
    const WPE_ScriptNet* net = wpe__net(L, wpe__script_host_from(L)->app.net.status != NULL, "net_status");
    static const char* const names[] = {"offline", "connecting", "hosting", "joined", "ended"};
    const char* reason = NULL;
    const WPE_ScriptNetStatus status = net->status(wpe__net_user(L), &reason);
    lua_pushstring(L, (unsigned)status < 5 ? names[status] : "offline");
    if (status == WPE_SCRIPT_NET_ENDED && reason) {
        lua_pushstring(L, reason);
        return 2;
    }
    return 1;
}

static int wpe__api_net_is_host(lua_State* L) {
    const WPE_ScriptNet* net = wpe__net(L, wpe__script_host_from(L)->app.net.status != NULL, "net_is_host");
    lua_pushboolean(L, net->status(wpe__net_user(L), NULL) == WPE_SCRIPT_NET_HOSTING);
    return 1;
}

static int wpe__api_net_player(lua_State* L) {
    const WPE_ScriptNet* net =
        wpe__net(L, wpe__script_host_from(L)->app.net.local_player != NULL, "net_player");
    const uint32_t player = net->local_player(wpe__net_user(L));
    if (player == UINT32_MAX) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, player);
    }
    return 1;
}

static int wpe__api_net_players(lua_State* L) {
    const WPE_ScriptNet* net = wpe__net(L, wpe__script_host_from(L)->app.net.players != NULL, "net_players");
    WPE_ScriptNetPlayer players[8];
    const uint32_t count = net->players(wpe__net_user(L), players, 8);
    lua_createtable(L, (int)count, 0);
    for (uint32_t i = 0; i < count && i < 8; ++i) {
        lua_createtable(L, 0, 3);
        lua_pushnumber(L, players[i].id);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, players[i].name);
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, players[i].ping_ms);
        lua_setfield(L, -2, "ping");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

/* wpe.net_send(to, name, [value], [reliable = true]) -> sent */
static int wpe__api_net_send(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_ScriptNet* net = wpe__net(L, host->app.net.send != NULL, "net_send");
    int64_t target = 0;
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char* to = lua_tostring(L, 1);
        if (strcmp(to, "all") == 0) {
            target = WPE_SCRIPT_NET_ALL;
        } else if (strcmp(to, "host") == 0) {
            target = 0;
        } else {
            luaL_errorL(L, "wpe.net_send: send to a player id, \"host\" or \"all\" (not \"%s\")", to);
        }
    } else {
        target = wpe__net_player(L, 1);
    }
    size_t name_length = 0;
    const char* name = luaL_checklstring(L, 2, &name_length);
    if (name_length == 0 || name_length >= WPE__NET_NAME_MAX)
        luaL_errorL(L, "wpe.net_send: message names are 1 to %u bytes", WPE__NET_NAME_MAX - 1);
    const bool reliable = lua_isnoneornil(L, 4) ? true : lua_toboolean(L, 4);
    if (!host->net_scratch) {
        host->net_scratch = wpe_alloc(WPE__NET_VALUE_MAX);
        if (!host->net_scratch) luaL_errorL(L, "wpe.net_send: out of memory");
    }
    WPE_BitWriter w = wpe_bits_writer(host->net_scratch, WPE__NET_VALUE_MAX);
    lua_settop(L, 3); /* The value (or nil) on top: encoding pushes above it. */
    wpe__encode(L, 3, &w, 0, false);
    lua_pushboolean(
        L, net->send(wpe__net_user(L), target, name, host->net_scratch, wpe_bits_writer_size(&w), reliable));
    return 1;
}

static int wpe__api_net_is_local(lua_State* L) {
    const WPE_ScriptNet* net =
        wpe__net(L, wpe__script_host_from(L)->app.net.is_local != NULL, "net_is_local");
    const WPE_Entity entity = wpe__net_entity(L, 1);
    lua_pushboolean(L, net->is_local(wpe__net_user(L), entity));
    return 1;
}

static int wpe__api_net_owner(lua_State* L) {
    const WPE_ScriptNet* net = wpe__net(L, wpe__script_host_from(L)->app.net.owner != NULL, "net_owner");
    const WPE_Entity entity = wpe__net_entity(L, 1);
    uint32_t player = 0;
    if (net->owner(wpe__net_user(L), entity, &player)) {
        lua_pushnumber(L, player);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int wpe__api_net_set_owner(lua_State* L) {
    const WPE_ScriptNet* net =
        wpe__net(L, wpe__script_host_from(L)->app.net.set_owner != NULL, "net_set_owner");
    const WPE_Entity entity = wpe__net_entity(L, 1);
    const uint32_t player = wpe__net_player(L, 2);
    const WPE_Result result = net->set_owner(wpe__net_user(L), entity, player);
    if (result == WPE_RESULT_ERROR_NOT_FOUND)
        luaL_errorL(L, "wpe.net_set_owner: the entity is not networked");
    if (result == WPE_RESULT_ERROR_INVALID_STATE)
        luaL_errorL(L, "wpe.net_set_owner: only the host gives entities");
    if (result == WPE_RESULT_ERROR_INVALID_ARG)
        luaL_errorL(L, "wpe.net_set_owner: player %u is not in the game", player);
    return 0;
}

static int wpe__api_net_kick(lua_State* L) {
    const WPE_ScriptNet* net = wpe__net(L, wpe__script_host_from(L)->app.net.kick != NULL, "net_kick");
    const uint32_t player = wpe__net_player(L, 1);
    lua_pushboolean(L, net->kick(wpe__net_user(L), player) == WPE_RESULT_OK);
    return 1;
}

static int wpe__api_net_find_games(lua_State* L) {
    const WPE_ScriptNet* net =
        wpe__net(L, wpe__script_host_from(L)->app.net.find_games != NULL, "net_find_games");
    WPE_ScriptNetGame games[WPE__NET_MAX_LISTED];
    const uint32_t count = net->find_games(wpe__net_user(L), games, WPE__NET_MAX_LISTED);
    lua_createtable(L, (int)count, 0);
    for (uint32_t i = 0; i < count && i < WPE__NET_MAX_LISTED; ++i) {
        lua_createtable(L, 0, 6);
        lua_pushstring(L, games[i].address);
        lua_setfield(L, -2, "address");
        lua_pushstring(L, games[i].game_name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, games[i].host_name);
        lua_setfield(L, -2, "host");
        lua_pushnumber(L, games[i].players);
        lua_setfield(L, -2, "players");
        lua_pushnumber(L, games[i].max_players);
        lua_setfield(L, -2, "max_players");
        lua_pushboolean(L, games[i].accepting);
        lua_setfield(L, -2, "accepting");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

static int wpe__api_net_stop_finding(lua_State* L) {
    const WPE_ScriptNet* net =
        wpe__net(L, wpe__script_host_from(L)->app.net.stop_finding != NULL, "net_stop_finding");
    net->stop_finding(wpe__net_user(L));
    return 0;
}

static const luaL_Reg wpe__net_api[] = {
    {"net_host", wpe__api_net_host},
    {"net_join", wpe__api_net_join},
    {"net_leave", wpe__api_net_leave},
    {"net_status", wpe__api_net_status},
    {"net_is_host", wpe__api_net_is_host},
    {"net_player", wpe__api_net_player},
    {"net_players", wpe__api_net_players},
    {"net_send", wpe__api_net_send},
    {"net_is_local", wpe__api_net_is_local},
    {"net_owner", wpe__api_net_owner},
    {"net_set_owner", wpe__api_net_set_owner},
    {"net_kick", wpe__api_net_kick},
    {"net_find_games", wpe__api_net_find_games},
    {"net_stop_finding", wpe__api_net_stop_finding},
    {NULL, NULL},
};

void wpe__script_net_register(lua_State* L) {
    luaL_register(L, NULL, wpe__net_api);
}

/* ---- Events from the application ------------------------------------------------------------- */

typedef struct wpe__NetEventOp {
    WPE_ScriptHost* host;
    const WPE_ScriptNetEvent* event;
    bool malformed;
} wpe__NetEventOp;

/* Runs inside lua_cpcall: pushes the callback's arguments and broadcasts it. */
static int wpe__net_event_call(lua_State* L) {
    wpe__NetEventOp* op = lua_touserdata(L, 1);
    const WPE_ScriptNetEvent* e = op->event;
    static const char* const methods[] = {"on_net_joined", "on_player_joined", "on_player_left",
                                          "on_net_ended", "on_net_message"};
    lua_pushcfunction(L, wpe__script_broadcast, "broadcast");
    lua_pushstring(L, methods[e->type]);
    int arguments = 0;
    switch (e->type) {
    case WPE_SCRIPT_NET_EVENT_JOINED:
        lua_pushnumber(L, e->player);
        arguments = 1;
        break;
    case WPE_SCRIPT_NET_EVENT_PLAYER_JOINED:
        lua_pushnumber(L, e->player);
        lua_pushstring(L, e->text ? e->text : "");
        arguments = 2;
        break;
    case WPE_SCRIPT_NET_EVENT_PLAYER_LEFT:
        lua_pushnumber(L, e->player);
        lua_pushstring(L, e->text ? e->text : "");
        lua_pushstring(L, e->reason ? e->reason : "");
        arguments = 3;
        break;
    case WPE_SCRIPT_NET_EVENT_ENDED:
        lua_pushstring(L, e->reason ? e->reason : "");
        arguments = 1;
        break;
    case WPE_SCRIPT_NET_EVENT_MESSAGE: {
        lua_pushnumber(L, e->player);
        lua_pushstring(L, e->text ? e->text : "");
        if (e->value_size == 0) {
            lua_pushnil(L);
        } else {
            WPE_BitReader bits = wpe_bits_reader(e->value, e->value_size);
            WPE_NetValueReader reader = wpe_netvalue_reader(&bits);
            const int before = lua_gettop(L);
            /* One whole value, and nothing after it but padding. */
            if (!wpe__push_decoded(L, &reader) || reader.depth != 0 ||
                wpe_bits_reader_remaining(&bits) >= 8) {
                lua_settop(L, before);
                op->malformed = true;
                lua_settop(L, 0);
                return 0;
            }
        }
        arguments = 3;
        break;
    }
    }
    lua_call(L, arguments + 1, 0);
    return 0;
}

WPE_Result wpe_script_host_net_event(WPE_ScriptHost* host, const WPE_ScriptNetEvent* event) {
    if (!host || !event) return WPE_RESULT_ERROR_NULL_ARG;
    if ((unsigned)event->type > WPE_SCRIPT_NET_EVENT_MESSAGE) return WPE_RESULT_OK;
    wpe__NetEventOp op = {.host = host, .event = event};
    if (lua_cpcall(host->L, wpe__net_event_call, &op) != 0) {
        wpe_log_error("a network event could not reach the scripts: %s",
                      lua_tostring(host->L, -1) ? lua_tostring(host->L, -1) : "VM error");
        lua_pop(host->L, 1);
    }
    if (op.malformed)
        wpe_log_warn("dropped a malformed network message '%s' from player %u",
                     event->text ? event->text : "", event->player);
    return WPE_RESULT_OK;
}
