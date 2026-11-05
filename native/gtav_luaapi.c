#include "gtav_luaapi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct gtav_lua OW;

static void seterr(char *err, size_t errlen, const char *fmt, ...) {
	va_list ap;
	if (!err || errlen == 0) return;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

int gtav_lua_bind(char *err, size_t errlen) {
	static int done = 0;
	HMODULE h;

	if (done) return 1;

	h = GetModuleHandleA("lua_shared.dll");
	if (!h) {
		h = LoadLibraryA("lua_shared.dll");
		if (!h) {
			seterr(err, errlen, "lua_shared.dll is not loaded");
			return 0;
		}
	}

// out of lua_shared.dll, no import lib
#define BIND(field, name)                                                     \
	do {                                                                      \
		*(void **)&OW.field = (void *)GetProcAddress(h, name);                \
		if (!OW.field) {                                                      \
			seterr(err, errlen, "lua_shared.dll does not export %s", name);   \
			return 0;                                                         \
		}                                                                     \
	} while (0)

	BIND(gettop,       "lua_gettop");
	BIND(settop,       "lua_settop");
	BIND(pushvalue,    "lua_pushvalue");
	BIND(remove,       "lua_remove");
	BIND(insert,       "lua_insert");
	BIND(replace,      "lua_replace");
	BIND(checkstack,   "lua_checkstack");
	BIND(type,         "lua_type");
	BIND(typename,     "lua_typename");

	BIND(isnumber,     "lua_isnumber");
	BIND(isstring,     "lua_isstring");
	BIND(isuserdata,   "lua_isuserdata");
	BIND(tonumber,     "lua_tonumber");
	BIND(tointeger,    "lua_tointeger");
	BIND(toboolean,    "lua_toboolean");
	BIND(tolstring,    "lua_tolstring");
	BIND(touserdata,   "lua_touserdata");
	BIND(topointer,    "lua_topointer");

	BIND(pushnil,      "lua_pushnil");
	BIND(pushnumber,   "lua_pushnumber");
	BIND(pushinteger,  "lua_pushinteger");
	BIND(pushlstring,  "lua_pushlstring");
	BIND(pushstring,   "lua_pushstring");
	BIND(pushfstring,  "lua_pushfstring");
	BIND(pushboolean,  "lua_pushboolean");
	BIND(pushlightuserdata, "lua_pushlightuserdata");
	BIND(pushcclosure, "lua_pushcclosure");

	BIND(getfield,     "lua_getfield");
	BIND(setfield,     "lua_setfield");
	BIND(gettable,     "lua_gettable");
	BIND(settable,     "lua_settable");
	BIND(rawget,       "lua_rawget");
	BIND(rawset,       "lua_rawset");
	BIND(rawgeti,      "lua_rawgeti");
	BIND(rawseti,      "lua_rawseti");
	BIND(createtable,  "lua_createtable");
	BIND(next,         "lua_next");
	BIND(objlen,       "lua_objlen");

	BIND(newuserdata,  "lua_newuserdata");
	BIND(getmetatable, "lua_getmetatable");
	BIND(setmetatable, "lua_setmetatable");
	BIND(checkudata,   "luaL_checkudata");
	BIND(newmetatable, "luaL_newmetatable");

	BIND(call,         "lua_call");
	BIND(pcall,        "lua_pcall");
	BIND(error,        "lua_error");
	BIND(ref,          "luaL_ref");
	BIND(unref,        "luaL_unref");
	BIND(concat,       "lua_concat");
	BIND(gc,           "lua_gc");
	BIND(checklstring, "luaL_checklstring");
	BIND(checknumber,  "luaL_checknumber");
	BIND(checkinteger, "luaL_checkinteger");
	BIND(checktype,    "luaL_checktype");
	BIND(argerror,     "luaL_argerror");
	BIND(register_,    "luaL_register");

#undef BIND

	done = 1;
	return 1;
}
