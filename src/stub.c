#include <string.h>
#include <stdarg.h>
#include <pthread.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/custom.h>
#include <caml/fail.h>
#include <caml/callback.h>
#include <caml/signals.h>

#include "stub.h"


/******************************************************************************/
/*****                           DEBUG FUNCTION                           *****/
/******************************************************************************/
#ifdef ENABLE_DEBUG
static int msglevel = 10; /* the higher, the more messages... */
#endif

#if defined(ENABLE_DEBUG)
void debug(int level, char* format, ...)
{
    va_list args;

    if (level > msglevel)
        return;

    va_start(args, format);
    vfprintf(stderr, format, args);
    fflush(stderr);
    va_end(args);
#ifdef ENABLE_DEBUG
#else
    /* Empty body, so a good compiler will optimise calls
       to debug away */
#endif /* ENABLE_DEBUG */
}
#else
/* Nothing */
#endif /* ENABLE_DEBUG */


/******************************************************************************/
/*****                          DATA STRUCTURES                           *****/
/******************************************************************************/
typedef struct reader_data
{
    value state_value;
    value reader_function;
    value reader_data;
} reader_data;

typedef struct writer_data
{
    value writer_function;
    value state_value;
    value writer_data;
} writer_data;

static void finalize_thread(value L);     /* Forward declaration */

static struct custom_operations thread_lua_State_ops =
{
  THREADS_OPS_UUID,
  finalize_thread,
  custom_compare_default,
  custom_hash_default,
  custom_serialize_default,
  custom_deserialize_default
};


/******************************************************************************/
/*****                         UTILITY FUNCTIONS                          *****/
/******************************************************************************/
/*
 * Pushes on the stack of L the array used to track the threads created via
 * lua_newthread
 */
static void push_threads_array(lua_State *L)
{
    debug(3, "push_threads_array(%p)\n", (void*)L);

    lua_pushstring(L, UUID);
    lua_gettable(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "threads_array");
    lua_gettable(L, -2);
    lua_insert(L, -2);
    lua_pop(L, 1);

    debug(4, "push_threads_array: RETURN\n");
}


/*
 * Pushes on the stack of L the array used to track the light userdata created via
 * lua_pushlightuserdata
 */
void push_lud_array(lua_State *L)
{
    debug(3, "push_lud_array(%p)\n", (void*)L);

    lua_pushstring(L, UUID);
    lua_gettable(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "light_userdata_array");
    lua_gettable(L, -2);
    lua_insert(L, -2);
    lua_pop(L, 1);

    debug(4, "push_lud_array: RETURN\n");
}


ocaml_data * get_ocaml_data(lua_State *L)
{
    lua_pushstring(L, UUID);
    lua_gettable(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "ocaml_data");
    lua_gettable(L, -2);
    ocaml_data *info = (ocaml_data*)lua_touserdata(L, -1);
    lua_pop(L, 2);
    return info;
}


static int panic_wrapper(lua_State *L)
{
    ocaml_data *data = get_ocaml_data(L);
    return Int_val(caml_callback(data->panic_callback,  // callback
                                 data->state_value));   // Lua state
}

/* This function is taken from the Lua source code, file ltablib.c line 118 */
static int tremove (lua_State *L)
{
    int e = aux_getn(L, 1);
    int pos = luaL_optint(L, 2, e);
    if (!(1 <= pos && pos <= e))  /* position is outside bounds? */
        return 0;  /* nothing to remove */
    luaL_setn(L, 1, e - 1);  /* t.n = n-1 */
    lua_rawgeti(L, 1, pos);  /* result = t[pos] */
    for ( ;pos<e; pos++) {
        lua_rawgeti(L, 1, pos+1);
        lua_rawseti(L, 1, pos);  /* t[pos] = t[pos+1] */
    }
    lua_pushnil(L);
    lua_rawseti(L, 1, e);  /* t[e] = nil */
    return 1;
}

static void finalize_thread(value L)
{
    debug(3, "finalize_thread(value L)\n");

    lua_State *thread = lua_State_val(L);
    push_threads_array(thread);
    int table_pos = lua_gettop(thread);

    lua_pushnil(thread);  /* first key */

    lua_State *el = NULL;
    int found = 0;

    /* Find the thread element to be removed */
    while (lua_next(thread, table_pos) != 0)
    {
        el = lua_tothread(thread, -1);
        if (el == thread)
        {
            found = 1;
            break;
        }
        lua_pop(thread, 1);
    }

    /* If found, remove it from the table */
    if (found)
    {
        lua_pop(thread, 1);
        if (tremove(thread) == 1)
        {
            lua_pop(thread, 1);
        }
        lua_pop(thread, 2);
    }
    debug(4, "finalize_thread: RETURN\n");
    return;
}

static int execute_ocaml_closure(lua_State *L)
{
    value *ocaml_closure = (value*)lua_touserdata(L, lua_upvalueindex(1));
    ocaml_data *data = get_ocaml_data(L);
    return Int_val(caml_callback(*ocaml_closure, data->state_value));
}

/******************************************************************************/
/*****                           LUA API STUBS                            *****/
/******************************************************************************/
CAMLprim
value lua_atpanic__stub(value L, value panicf)
{
    CAMLparam2(L, panicf);
    CAMLlocal1(old_panicf);

    lua_State *state = lua_State_val(L);

    ocaml_data *data = get_ocaml_data(state);

    old_panicf = data->panic_callback;
    caml_remove_global_root(&(data->panic_callback));
    caml_register_global_root(&(data->panic_callback));
    data->panic_callback = panicf;
    lua_atpanic(state, panic_wrapper);

    CAMLreturn(old_panicf);
}

STUB_STATE_INT_INT_VOID(lua_call, nargs, nresults)

STUB_STATE_INT_BOOL(lua_checkstack, extra)

STUB_STATE_INT_VOID(lua_concat, n)

STUB_STATE_INT_INT_VOID(lua_createtable, narr, nrec)

static int writer_function(lua_State *L, const void *p, size_t sz, void* ud)
{
    value writer_status_value, buffer;

    debug(3, "writer_function(%p, %p, %d, %p)\n", L, p, sz, ud);

    writer_data *internal_data = (writer_data*)ud;
    buffer = caml_alloc_string(sz);
    memcpy(String_val(buffer), p, sz);

    writer_status_value =
        caml_callback3( internal_data->writer_function,
                        internal_data->state_value,
                        buffer,
                        internal_data->writer_data );

    if (writer_status_value == Val_int(0))
    {
        debug(4, "writer_function: RETURN 0\n");
        return 0;
    }
    else
    {
        debug(4, "writer_function: RETURN 1\n");
        return 1;
    }
}

CAMLprim
value lua_dump__stub(value L, value writer, value data)
{
    CAMLparam3(L, writer, data);

    debug(3, "lua_dump__stub(value L, value writer, value data)\n");

    writer_data *internal_data = (writer_data*)caml_stat_alloc(sizeof(writer_data));

    caml_register_global_root(&(internal_data->writer_function));
    caml_register_global_root(&(internal_data->state_value));
    caml_register_global_root(&(internal_data->writer_data));

    internal_data->writer_function = writer;
    internal_data->state_value = L;
    internal_data->writer_data = data;

    int result = lua_dump(  lua_State_val(L),
                            writer_function,
                            (void*)internal_data  );

    caml_remove_global_root(&(internal_data->writer_function));
    caml_remove_global_root(&(internal_data->state_value));
    caml_remove_global_root(&(internal_data->writer_data));

    caml_stat_free(internal_data);

    debug(4, "lua_dump__stub: RETURN %d\n", result);
    CAMLreturn(Val_int(result));
}

STUB_STATE_INT_INT_BOOL(lua_equal, index1, index2)

STUB_STATE_VOID(lua_error)

STUB_STATE_INT_INT_INT(lua_gc, what, data)

STUB_STATE_INT_VOID(lua_getfenv, index)

CAMLprim
value lua_getfield__stub(value L, value index, value k)
{
    CAMLparam3(L, index, k);
    lua_getfield(lua_State_val(L), Int_val(index), String_val(k));
    CAMLreturn(Val_unit);
}

STUB_STATE_INT_BOOL(lua_getmetatable, index)

STUB_STATE_INT_VOID(lua_gettable, index)

STUB_STATE_INT(lua_gettop)

STUB_STATE_INT_VOID(lua_insert, index)

STUB_STATE_INT_BOOL(lua_isboolean, index)

STUB_STATE_INT_BOOL(lua_iscfunction, index)

STUB_STATE_INT_BOOL(lua_isfunction, index)

STUB_STATE_INT_BOOL(lua_islightuserdata, index)

STUB_STATE_INT_BOOL(lua_isnil, index)

STUB_STATE_INT_BOOL(lua_isnone, index)

STUB_STATE_INT_BOOL(lua_isnoneornil, index)

STUB_STATE_INT_BOOL(lua_isnumber, index)

STUB_STATE_INT_BOOL(lua_isstring, index)

STUB_STATE_INT_BOOL(lua_istable, index)

STUB_STATE_INT_BOOL(lua_isthread, index)

STUB_STATE_INT_BOOL(lua_isuserdata, index)

STUB_STATE_INT_INT_BOOL(lua_lessthan, index1, index2)

static const char* reader_function(lua_State *L, void *data, size_t *size)
{
    value string_option_res;

    debug(3, "reader_function(%p, %p, %p)\n", L, data, size);

    reader_data *internal_data = (reader_data*)data;
    string_option_res = caml_callback2( internal_data->reader_function,
                                        internal_data->state_value,
                                        internal_data->reader_data );
    if (string_option_res == Val_int(0))
    {
        // string_option_res = None
        *size = 0;
        debug(4, "reader_function: RETURN NULL\n");
        return NULL;
    }
    else
    {
        // string_option_res = (Some "string")
        value str = Field(string_option_res, 0);
        *size = caml_string_length(str);
        debug(4, "reader_function: RETURN \"%s\", len = %d\n", String_val(str), *size);
        return String_val(str);
    }
}

CAMLprim
value lua_load__stub(value L, value reader, value data, value chunkname)
{
    CAMLparam4(L, reader, data, chunkname);

    reader_data *internal_data = (reader_data*)caml_stat_alloc(sizeof(reader_data));

    caml_register_global_root(&(internal_data->state_value));
    caml_register_global_root(&(internal_data->reader_function));
    caml_register_global_root(&(internal_data->reader_data));

    internal_data->state_value = L;
    internal_data->reader_function = reader;
    internal_data->reader_data = data;

    int result = lua_load(  lua_State_val(L),
                            reader_function,
                            (void*)internal_data,
                            String_val(chunkname)  );

    caml_remove_global_root(&(internal_data->state_value));
    caml_remove_global_root(&(internal_data->reader_function));
    caml_remove_global_root(&(internal_data->reader_data));

    caml_stat_free(internal_data);

    CAMLreturn(Val_int(result));
}

STUB_STATE_VOID(lua_newtable)

CAMLprim
value lua_newthread__stub(value L)
{
    CAMLparam1(L);
    CAMLlocal1(thread_value);
    lua_State *LL = lua_State_val(L);

    push_threads_array(LL);
    lua_State *thread = lua_newthread(LL);
    lua_pushvalue(LL, -1);

    /* The stack here is:
     *  -----+-------------------------------------
     *  | -1 | the new thread just created (COPY) |
     *  -----+-------------------------------------
     *  | -2 | the new thread just created        |
     *  -----+------------------------------------|
     *  | -3 | the threads array (n elements)     |
     *  -----+-------------------------------------
     */
    int n = lua_objlen(LL, -3);
    lua_pushinteger(LL, n + 1);
    lua_insert(LL, -2);
    lua_settable(LL, -4); /* a copy of the thread inserted in our registry */
    lua_insert(LL, -2);
    lua_pop(LL, 1);

    /* Here the stack contains only the new thread on its top */

    /* wrap the new thread lua_State *thread in a custom object */
    thread_value = caml_alloc_custom(&thread_lua_State_ops, sizeof(lua_State *), 1, 10);
    lua_State_val(thread_value) = thread;

    /* Return the thread value */
    CAMLreturn(thread_value);
}

CAMLprim
value lua_newuserdata__stub(value L, value ud)
{
    debug(3, "lua_newuserdata__stub(%p, %p)\n", (void*)L, (void*)ud);
    CAMLparam2(L, ud);

    lua_State *LL = lua_State_val(L);

    /* Create the new userdatum containing the OCaml value ud */
    value *lua_ud = (value*)lua_newuserdata(LL, sizeof(value));
    debug(5, "lua_newuserdata__stub: calling lua_newuserdata(%p, %d) -> %p\n",
             (void*)LL, sizeof(value), (void*)lua_ud);
    caml_register_global_root(lua_ud);
    *lua_ud = ud;

    /* retrieve the metatable for this kind of userdata */
    lua_pushstring(LL, UUID);
    lua_gettable(LL, LUA_REGISTRYINDEX);
    lua_pushstring(LL, "userdata_metatable");
    lua_gettable(LL, -2);
    lua_setmetatable(LL, -3);
    lua_pop(LL, 1);

    debug(4, "lua_newuserdata__stub: RETURNS\n");
    CAMLreturn(Val_unit);
}

STUB_STATE_INT_INT(lua_next, index)

STUB_STATE_INT_INT(lua_objlen, index)

CAMLprim
value lua_pcall__stub(value L, value nargs, value nresults, value errfunc)
{
  CAMLparam4(L, nargs, nresults, errfunc);
  CAMLlocal1(status);

  status = Val_int(lua_pcall( lua_State_val(L),
                              Int_val(nargs),
                              Int_val(nresults),
                              Int_val(errfunc)) );
  CAMLreturn(status);
}

STUB_STATE_INT_VOID(lua_pop, n)

STUB_STATE_BOOL_VOID(lua_pushboolean, b)

CAMLprim
value lua_pushcfunction__stub(value L, value f)
{
    CAMLparam2(L, f);

    debug(3, "lua_pushcfunction__stub(%p, %p)\n", (void*)L, (void*)f);

    /* Create the new userdatum containing the OCaml value of the closure */
    lua_State *LL = lua_State_val(L);
    value *ocaml_closure = (value*)lua_newuserdata(LL, sizeof(value));
    debug(5, "lua_pushcfunction__stub: calling lua_newuserdata(%p, %d) -> %p\n",
             (void*)LL, sizeof(value), (void*)ocaml_closure);

    caml_register_global_root(ocaml_closure);
    *ocaml_closure = f;

    /* retrieve the metatable for this kind of userdata */
    lua_pushstring(LL, UUID);
    lua_gettable(LL, LUA_REGISTRYINDEX);
    lua_pushstring(LL, "closure_metatable");
    lua_gettable(LL, -2);
    lua_setmetatable(LL, -3);
    lua_pop(LL, 1);

    /* at this point the stack has a userdatum on its top, with the correct metatable */

    lua_pushcclosure(LL, execute_ocaml_closure, 1);

    debug(4, "lua_pushcfunction__stub: RETURN\n");
    CAMLreturn(Val_unit);
}

STUB_STATE_INT_VOID(lua_pushinteger, n)

CAMLprim
value lua_pushlightuserdata__stub(value L, value p)
{
    debug(3, "lua_pushlightuserdata__stub(%p, %p)\n", (void*)L, (void*)p);

    CAMLparam2(L, p);

    lua_State *LL = lua_State_val(L);

    if (Is_block(p))
    {
        /* the p value is an OCaml block */

        /* Create the new userdatum containing the OCaml value ud */
        value *lua_light_ud = (value*)caml_stat_alloc(sizeof(value));
        debug(5, "lua_pushlightuserdata__stub: caml_stat_alloc -> %p\n", (void*)(lua_light_ud));
        caml_register_global_root(lua_light_ud);
        *lua_light_ud = p;

        push_lud_array(LL);
        lua_pushlightuserdata(LL, (void *)lua_light_ud);
        lua_pushvalue(LL, -1);
        int n = lua_objlen(LL, -3);
        lua_pushinteger(LL, n + 1);
        lua_insert(LL, -2);
        lua_settable(LL, -4); /* the light user data inserted in our registry */
        lua_insert(LL, -2);
        lua_pop(LL, 1);
    }
    else
    {
        /* the p value is an immediate integer: in this case calling
         * pushlightuserdata is a nonsense, I prefer to raise an exception */
        caml_raise_constant(*caml_named_value("Not_a_block_value"));
    }

    debug(4, "lua_pushlightuserdata__stub: RETURN\n");
    CAMLreturn(Val_unit);
}

CAMLprim
value lua_pushlstring__stub(value L, value s)
{
    CAMLparam2(L, s);
    lua_pushlstring(lua_State_val(L), String_val(s), caml_string_length(s));
    CAMLreturn(Val_unit);
}

STUB_STATE_VOID(lua_pushnil)

STUB_STATE_DOUBLE_VOID(lua_pushnumber, n)

STUB_STATE_BOOL(lua_pushthread)

STUB_STATE_INT_VOID(lua_pushvalue, index)

STUB_STATE_INT_INT_BOOL(lua_rawequal, index1, index2)

STUB_STATE_INT_VOID(lua_rawget, index)

STUB_STATE_INT_INT_VOID(lua_rawgeti, index, n)

STUB_STATE_INT_VOID(lua_rawset, index)

STUB_STATE_INT_INT_VOID(lua_rawseti, index, n)

STUB_STATE_INT_VOID(lua_remove, index)

STUB_STATE_INT_VOID(lua_replace, index)

STUB_STATE_INT_INT(lua_resume, narg)

STUB_STATE_INT_BOOL(lua_setfenv, index)

CAMLprim
value lua_setfield__stub(value L, value index, value k)
{
    CAMLparam3(L, index, k);
    lua_setfield(lua_State_val(L), Int_val(index), String_val(k));
    CAMLreturn(Val_unit);
}

CAMLprim
value lua_setglobal__stub(value L, value name)
{
    CAMLparam2(L, name);
    lua_setglobal(lua_State_val(L), String_val(name));
    CAMLreturn(Val_unit);
}

STUB_STATE_INT_INT(lua_setmetatable, index)

STUB_STATE_INT_VOID(lua_settable, index)

STUB_STATE_INT_VOID(lua_settop, index)

STUB_STATE_INT(lua_status)

STUB_STATE_INT_BOOL(lua_toboolean, index)

CAMLprim
value lua_tocfunction__stub(value L, value index)
{
    CAMLparam2(L, index);

    lua_State *LL = lua_State_val(L);

    /* 1ST: get the C function pointer */
    lua_CFunction f = lua_tocfunction(LL, Int_val(index));

    /* If the Lua object at position "index" of the stack is not a C function,
       raise an exception (will be catched on the OCaml side */
    if (f == NULL) caml_raise_constant(*caml_named_value("Not_a_C_function"));

    /* 2ND: if the function is an OCaml closure, pushed in the Lua world using
       Lua.pushcfunction, the pointer returned here is actually
       execute_ocaml_closure. This is a static function defined in this file and
       without a context it's useless. Instead of returning a "value" version of
       execute_ocaml_closure we get the upvalue, which is the original value of
       the OCaml closure, and return it. */

    /* lua_getupvalue (from the Lua debugging interface) pushes the upvalue on
       the stack (+1) */
    const char *name = lua_getupvalue(LL, Int_val(index), 1);
    if (name == NULL)
    {
        /* In case of error, raise an exception */
        caml_raise_constant(*caml_named_value("Not_a_C_function"));
    }
    else
    {
        /* Convert the userdatum to an OCaml value */
        value *ocaml_closure = (value*)lua_touserdata(LL, -1);

        /* remove the userdatum from the stack (-1) */
        lua_pop(LL, 1);

        /* return it */
        CAMLreturn (*ocaml_closure);
    }
}

STUB_STATE_INT_INT(lua_tointeger, index)

void raise_type_error(char *msg)
{
  caml_raise_with_string(*caml_named_value("Lua_type_error"), msg);
}

CAMLprim
value lua_tolstring__stub(value L, value index)
{
  size_t len = 0;
  const char *value_from_lua;
  CAMLparam2(L, index);
  CAMLlocal1(ret_val);

  value_from_lua = lua_tolstring( lua_State_val(L),
                                  Int_val(index),
                                  &len );
  if (value_from_lua != NULL)
  {
    ret_val = caml_alloc_string(len);
    char *s = String_val(ret_val);
    memcpy(s, value_from_lua, len);
  }
  else
  {
    raise_type_error("lua_tolstring: not a string value!");
  }

  CAMLreturn(ret_val);
}

STUB_STATE_INT_DOUBLE(lua_tonumber, index)

CAMLprim
value lua_tothread__stub(value L, value index)
{
    CAMLparam2(L, index);
    CAMLlocal1(thread_value);

    lua_State *LL = lua_State_val(L);
    int int_index = Int_val(index);

    lua_State *thread = lua_tothread(LL, int_index);
    if (thread != NULL)
    {
        push_threads_array(LL);
        lua_pushvalue(LL, int_index);

        int n = lua_objlen(LL, -2);
        lua_pushinteger(LL, n + 1);
        lua_insert(LL, -2);
        lua_settable(LL, -3); /* a copy of the thread inserted in our registry */
        lua_pop(LL, 1);

        thread_value = caml_alloc_custom(&thread_lua_State_ops, sizeof(lua_State *), 1, 10);
        lua_State_val(thread_value) = thread;
    }
    else
    {
        caml_raise_constant(*caml_named_value("Not_a_Lua_thread"));
    }

    CAMLreturn(thread_value);
}

CAMLprim
value lua_touserdata__stub(value L, value index)
{
    CAMLparam2(L, index);
    CAMLlocal1(ret_val);

    debug(3, "lua_touserdata__stub(%p, %p)\n", (void*)L, (void*)index);

    lua_State *LL = lua_State_val(L);
    int int_index = Int_val(index);

    value *lua_ud = (value*)lua_touserdata(LL, int_index);
    debug(5, "lua_touserdata__stub: calling lua_touserdata(%p, %d) -> %p\n",
             (void*)LL, int_index, (void*)lua_ud);
    ret_val = *lua_ud;

    debug(4, "lua_touserdata__stub: RETURN %p\n", (void*)ret_val);
    CAMLreturn(ret_val);
}

STUB_STATE_INT_INT(lua_type, index)

CAMLprim
value lua_xmove__stub(value from, value to, value n)
{
    CAMLparam3(from, to, n);
    lua_xmove(lua_State_val(from), lua_State_val(to), Int_val(n));
    CAMLreturn(Val_unit);
}

STUB_STATE_INT_INT(lua_yield, nresults)

