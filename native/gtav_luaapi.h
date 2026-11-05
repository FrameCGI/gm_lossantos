#ifndef GTAV_LUAAPI_H
#define GTAV_LUAAPI_H

#include <stddef.h>

typedef struct lua_State lua_State;
typedef double           lua_Number;
typedef ptrdiff_t        lua_Integer;
typedef int (*lua_CFunction)(lua_State *L);

typedef struct luaL_Reg {
	const char  *name;
	lua_CFunction func;
} luaL_Reg;

// stack indices
#define GTAV_LUA_REGISTRYINDEX (-10000)
#define GTAV_LUA_ENVIRONINDEX  (-10001)
#define GTAV_LUA_GLOBALSINDEX  (-10002)
#define GTAV_LUA_MULTRET       (-1)

// lua_type() results
#define GTAV_LUA_TNIL           0
#define GTAV_LUA_TBOOLEAN       1
#define GTAV_LUA_TLIGHTUSERDATA 2
#define GTAV_LUA_TNUMBER        3
#define GTAV_LUA_TSTRING        4
#define GTAV_LUA_TTABLE         5
#define GTAV_LUA_TFUNCTION      6
#define GTAV_LUA_TUSERDATA      7
#define GTAV_LUA_TTHREAD        8

struct gtav_lua {
	// stack inspection
	int         (*gettop)(lua_State *L);
	void        (*settop)(lua_State *L, int idx);
	void        (*pushvalue)(lua_State *L, int idx);
	void        (*remove)(lua_State *L, int idx);
	void        (*insert)(lua_State *L, int idx);
	void        (*replace)(lua_State *L, int idx);
	int         (*checkstack)(lua_State *L, int sz);
	int         (*type)(lua_State *L, int idx);
	const char *(*typename)(lua_State *L, int tp);

	// reading
	int         (*isnumber)(lua_State *L, int idx);
	int         (*isstring)(lua_State *L, int idx);
	int         (*isuserdata)(lua_State *L, int idx);
	lua_Number  (*tonumber)(lua_State *L, int idx);
	lua_Integer (*tointeger)(lua_State *L, int idx);
	int         (*toboolean)(lua_State *L, int idx);
	const char *(*tolstring)(lua_State *L, int idx, size_t *len);
	void       *(*touserdata)(lua_State *L, int idx);
	const void *(*topointer)(lua_State *L, int idx);

	// pushing
	void        (*pushnil)(lua_State *L);
	void        (*pushnumber)(lua_State *L, lua_Number n);
	void        (*pushinteger)(lua_State *L, lua_Integer n);
	const char *(*pushlstring)(lua_State *L, const char *s, size_t len);
	const char *(*pushstring)(lua_State *L, const char *s);
	const char *(*pushfstring)(lua_State *L, const char *fmt, ...);
	void        (*pushboolean)(lua_State *L, int b);
	void        (*pushlightuserdata)(lua_State *L, void *p);
	void        (*pushcclosure)(lua_State *L, lua_CFunction fn, int n);

	// tables, no getglobal export so getfield + GLOBALSINDEX
	void        (*getfield)(lua_State *L, int idx, const char *k);
	void        (*setfield)(lua_State *L, int idx, const char *k);
	void        (*gettable)(lua_State *L, int idx);
	void        (*settable)(lua_State *L, int idx);
	void        (*rawget)(lua_State *L, int idx);
	void        (*rawset)(lua_State *L, int idx);
	void        (*rawgeti)(lua_State *L, int idx, int n);
	void        (*rawseti)(lua_State *L, int idx, int n);
	void        (*createtable)(lua_State *L, int narr, int nrec);
	int         (*next)(lua_State *L, int idx);
	size_t      (*objlen)(lua_State *L, int idx);

	// userdata + metatables
	void       *(*newuserdata)(lua_State *L, size_t sz);
	int         (*getmetatable)(lua_State *L, int objindex);
	int         (*setmetatable)(lua_State *L, int objindex);
	void       *(*checkudata)(lua_State *L, int ud, const char *tname);
	int         (*newmetatable)(lua_State *L, const char *tname);

	// calls
	void        (*call)(lua_State *L, int nargs, int nresults);
	int         (*pcall)(lua_State *L, int nargs, int nresults, int errfunc);
	int         (*error)(lua_State *L);
	int         (*ref)(lua_State *L, int t);
	void        (*unref)(lua_State *L, int t, int ref);
	void        (*concat)(lua_State *L, int n);
	int         (*gc)(lua_State *L, int what, int data);
	const char *(*checklstring)(lua_State *L, int narg, size_t *len);
	lua_Number  (*checknumber)(lua_State *L, int narg);
	lua_Integer (*checkinteger)(lua_State *L, int narg);
	void        (*checktype)(lua_State *L, int narg, int t);
	int         (*argerror)(lua_State *L, int narg, const char *extramsg);
	void        (*register_)(lua_State *L, const char *libname,
							 const luaL_Reg *l);
};

extern struct gtav_lua OW;

int gtav_lua_bind(char *err, size_t errlen);

#endif /* GTAV_LUAAPI_H */
