/**
 * C implementation of the crud simple DML storage functions
 * (get, insert, replace, update, upsert, delete), registered as
 * C stored procedures 'crud.storage_c.<op>' and dispatched by
 * vshard's C storage.call without entering Lua on the happy
 * path. The bucket ref is taken by the vshard wrapper for the
 * whole call - these functions do no ref logic at all.
 *
 * Anything unusual - malformed args, unknown space, sharding
 * hash mismatch or cache miss, unsupported option, or a FAILED
 * box operation - is delegated to the Lua op implementation
 * in-process (a failed single-statement DML changes nothing, so
 * re-execution is safe; on vinyl a failed attempt and the Lua
 * re-execution are separated by possible yields, which is
 * indistinguishable from the request arriving later - still
 * linearizable). A SUCCEEDED box operation is never re-executed:
 * its reply is fully encodable here.
 */
#include <stdlib.h>
#include <string.h>

#include <msgpuck.h>
#include "module.h"
#include <lauxlib.h>

#define EXPORT __attribute__((visibility("default")))

/* From src/box/errcode.h, not exposed in module.h. */
enum {
	ER_PROC_C_CODE = 102,
};

/** Names of the Lua helpers in the stashed helpers table. */
#define HELPER_CALL_OP "call_op"
#define HELPER_GET_SHARDING_HASHES "get_sharding_hashes"

enum {
	/* Both caches are tiny linear-scan arrays. */
	SPACE_CACHE_CAP = 64,
	HASH_CACHE_CAP = 64,
	VSC_NAME_MAX = 128,
};

struct space_cache_entry {
	char name[VSC_NAME_MAX];
	uint32_t name_len;
	uint32_t space_id;
};

struct hash_cache_entry {
	char name[VSC_NAME_MAX];
	uint32_t name_len;
	bool has_func_hash;
	uint64_t func_hash;
	bool has_key_hash;
	uint64_t key_hash;
};

static struct {
	int helpers_ref;
	/* Space name -> id cache, valid for one schema version. */
	uint64_t schema_version;
	struct space_cache_entry spaces[SPACE_CACHE_CAP];
	int space_count;
	/* Space name -> sharding hashes, invalidated from Lua. */
	struct hash_cache_entry hashes[HASH_CACHE_CAP];
	int hash_count;
	/* Fast path/fallback counters, see stat_get(). */
	uint64_t stat_fastpath;
	uint64_t stat_fallback;
} crud_c = {
	.helpers_ref = LUA_NOREF,
};

/*
 * Coroutine pool for the Lua helpers: the delegated Lua op can
 * yield, so each in-flight call needs its own coro.
 */
struct coro_slot {
	lua_State *co;
	int ref;
};

enum { CORO_POOL_CAP = 16 };
static struct coro_slot coro_pool[CORO_POOL_CAP];
static int coro_pool_size = 0;

static int
coro_take(struct coro_slot *out)
{
	if (coro_pool_size > 0) {
		*out = coro_pool[--coro_pool_size];
		return 0;
	}
	lua_State *L = luaT_state();
	out->co = lua_newthread(L);
	if (out->co == NULL) {
		box_error_set(__FILE__, __LINE__, ER_PROC_C_CODE,
			      "can not create a Lua coroutine");
		return -1;
	}
	out->ref = luaL_ref(L, LUA_REGISTRYINDEX);
	return 0;
}

static void
coro_release(struct coro_slot *slot)
{
	lua_settop(slot->co, 0);
	if (coro_pool_size < CORO_POOL_CAP) {
		coro_pool[coro_pool_size++] = *slot;
	} else {
		luaL_unref(luaT_state(), LUA_REGISTRYINDEX, slot->ref);
	}
}

/**
 * Call a Lua helper from the stashed helpers table. The helper
 * returns one msgpack-encoded string, which is copied onto the
 * box region. Returns -1 with the diag set when the helper
 * itself fails.
 */
typedef void (*push_args_f)(lua_State *co, void *ctx);

static int
helper_call(const char *helper, push_args_f push_args, void *ctx, int nargs,
	    const char **out, const char **out_end)
{
	if (crud_c.helpers_ref == LUA_NOREF) {
		box_error_set(__FILE__, __LINE__, ER_PROC_C_CODE,
			      "crud C helpers are not initialized");
		return -1;
	}
	struct coro_slot slot;
	if (coro_take(&slot) != 0)
		return -1;
	lua_State *co = slot.co;
	lua_rawgeti(co, LUA_REGISTRYINDEX, crud_c.helpers_ref);
	lua_getfield(co, -1, helper);
	lua_remove(co, -2);
	if (push_args != NULL)
		push_args(co, ctx);
	if (luaT_call(co, nargs, 1) != 0) {
		coro_release(&slot);
		return -1;
	}
	size_t len;
	const char *s = lua_tolstring(co, -1, &len);
	int rc = -1;
	if (s == NULL || len == 0) {
		box_error_set(__FILE__, __LINE__, ER_PROC_C_CODE,
			      "crud C helper '%s' must return "
			      "a msgpack string", helper);
	} else {
		char *copy = box_region_alloc(len);
		if (copy != NULL) {
			memcpy(copy, s, len);
			*out = copy;
			*out_end = copy + len;
			rc = 0;
		}
	}
	coro_release(&slot);
	return rc;
}

/** Emit a helper reply: one port entry per array element. */
static int
reply_emit(box_function_ctx_t *ctx, const char *reply, const char *reply_end)
{
	(void)reply_end;
	if (mp_typeof(*reply) != MP_ARRAY) {
		box_error_set(__FILE__, __LINE__, ER_PROC_C_CODE,
			      "malformed crud C helper reply");
		return -1;
	}
	const char *q = reply;
	uint32_t cnt = mp_decode_array(&q);
	for (uint32_t i = 0; i < cnt; i++) {
		const char *e = q;
		mp_next(&q);
		box_return_mp(ctx, e, q);
	}
	return 0;
}

struct op_args {
	const char *op;
	const char *args;
	const char *args_end;
};

static void
push_op_args(lua_State *co, void *vctx)
{
	struct op_args *a = vctx;
	lua_pushstring(co, a->op);
	lua_pushlstring(co, a->args, a->args_end - a->args);
}

/**
 * Delegate the whole request to the Lua '<op>_on_storage'
 * implementation, which reproduces the outcome and encodes the
 * complete reply.
 */
static int
fallback(box_function_ctx_t *ctx, const char *op, const char *args,
	 const char *args_end)
{
	crud_c.stat_fallback++;
	struct op_args a = {op, args, args_end};
	const char *reply, *reply_end;
	if (helper_call(HELPER_CALL_OP, push_op_args, &a, 2, &reply,
			&reply_end) != 0)
		return -1;
	return reply_emit(ctx, reply, reply_end);
}

/*
 * Space name -> id cache. Flushed whenever the schema version
 * changes.
 */
static int64_t
space_id_resolve(const char *name, uint32_t name_len)
{
	uint64_t version = box_schema_version();
	if (version != crud_c.schema_version) {
		crud_c.space_count = 0;
		crud_c.schema_version = version;
	}
	if (name_len >= VSC_NAME_MAX)
		return -1;
	for (int i = 0; i < crud_c.space_count; i++) {
		struct space_cache_entry *e = &crud_c.spaces[i];
		if (e->name_len == name_len &&
		    memcmp(e->name, name, name_len) == 0)
			return e->space_id;
	}
	uint32_t space_id = box_space_id_by_name(name, name_len);
	if (space_id == BOX_ID_NIL)
		return -1;
	if (crud_c.space_count < SPACE_CACHE_CAP) {
		struct space_cache_entry *e =
			&crud_c.spaces[crud_c.space_count++];
		memcpy(e->name, name, name_len);
		e->name_len = name_len;
		e->space_id = space_id;
	}
	return space_id;
}

/*
 * Sharding hash cache, mirroring the Lua storage_metadata_cache.
 * Entries are added on demand through the get_sharding_hashes
 * helper (whose Lua side installs the _ddl_* on_replace triggers
 * lazily, so whenever C holds a value, the invalidation hooks
 * below are already wired).
 */
static struct hash_cache_entry *
hash_cache_find(const char *name, uint32_t name_len)
{
	for (int i = 0; i < crud_c.hash_count; i++) {
		struct hash_cache_entry *e = &crud_c.hashes[i];
		if (e->name_len == name_len &&
		    memcmp(e->name, name, name_len) == 0)
			return e;
	}
	return NULL;
}

static void
push_space_name(lua_State *co, void *vctx)
{
	struct op_args *a = vctx;
	lua_pushlstring(co, a->op, a->args_end - a->args);
}

/**
 * Fill the cache entry for the space through the Lua helper.
 * The helper returns msgpack [func_hash|nil, key_hash|nil].
 * Returns NULL when the helper failed or the cache is full -
 * the caller falls back to Lua.
 */
static struct hash_cache_entry *
hash_cache_fill(const char *name, uint32_t name_len)
{
	if (name_len >= VSC_NAME_MAX || crud_c.hash_count >= HASH_CACHE_CAP)
		return NULL;
	struct op_args a = {name, name, name + name_len};
	const char *reply, *reply_end;
	if (helper_call(HELPER_GET_SHARDING_HASHES, push_space_name, &a, 1,
			&reply, &reply_end) != 0) {
		/* Clear the diag - the caller falls back. */
		box_error_clear();
		return NULL;
	}
	const char *q = reply;
	if (mp_typeof(*q) != MP_ARRAY || mp_decode_array(&q) < 2)
		return NULL;
	struct hash_cache_entry e;
	memset(&e, 0, sizeof(e));
	memcpy(e.name, name, name_len);
	e.name_len = name_len;
	if (mp_typeof(*q) == MP_UINT) {
		e.has_func_hash = true;
		e.func_hash = mp_decode_uint(&q);
	} else {
		mp_next(&q);
	}
	if (mp_typeof(*q) == MP_UINT) {
		e.has_key_hash = true;
		e.key_hash = mp_decode_uint(&q);
	} else {
		mp_next(&q);
	}
	crud_c.hashes[crud_c.hash_count] = e;
	return &crud_c.hashes[crud_c.hash_count++];
}

/** The vtable handed to Lua by setup(), see c_api in Lua. */
struct crud_storage_c_api {
	void (*sharding_cache_invalidate)(const char *name, uint32_t len);
	void (*sharding_cache_clear)(void);
	/* which: 0 - fastpath counter, 1 - fallback counter. */
	uint64_t (*stat_get)(int which);
};

static void
api_sharding_cache_invalidate(const char *name, uint32_t len)
{
	struct hash_cache_entry *e = hash_cache_find(name, len);
	if (e != NULL) {
		*e = crud_c.hashes[crud_c.hash_count - 1];
		crud_c.hash_count--;
	}
}

static void
api_sharding_cache_clear(void)
{
	crud_c.hash_count = 0;
}

static uint64_t
api_stat_get(int which)
{
	return which == 0 ? crud_c.stat_fastpath : crud_c.stat_fallback;
}

static struct crud_storage_c_api crud_c_api = {
	.sharding_cache_invalidate = api_sharding_cache_invalidate,
	.sharding_cache_clear = api_sharding_cache_clear,
	.stat_get = api_stat_get,
};

/**
 * crud.storage_c.setup() - the handshake. Grabs the Lua helpers
 * table stashed in _G by crud/common/storage_c.lua and returns
 * the api vtable address for Lua to ffi.cast.
 */
EXPORT int
setup(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	(void)args;
	(void)args_end;
	lua_State *L = luaT_state();
	lua_getglobal(L, "__crud_storage_c_helpers");
	if (lua_istable(L, -1)) {
		if (crud_c.helpers_ref != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, crud_c.helpers_ref);
		crud_c.helpers_ref = luaL_ref(L, LUA_REGISTRYINDEX);
		lua_pushnil(L);
		lua_setglobal(L, "__crud_storage_c_helpers");
	} else {
		lua_pop(L, 1);
	}
	char buf[16];
	char *e = mp_encode_uint(buf, (uintptr_t)&crud_c_api);
	box_return_mp(ctx, buf, e);
	return 0;
}


/*
 * Common request pipeline pieces for the DML ops.
 */

/**
 * Decode the leading space_name argument and resolve the space
 * id. Returns -1 when the caller must fall back.
 */
static int64_t
decode_space(const char **p, const char **name, uint32_t *name_len)
{
	if (mp_typeof(**p) != MP_STR)
		return -1;
	*name = mp_decode_str(p, name_len);
	return space_id_resolve(*name, *name_len);
}

/**
 * Check the sharding hashes against the C cache. 0 - check
 * passed, -1 - fall back (mismatch or cache problem; the Lua op
 * re-checks authoritatively and builds the exact
 * ShardingHashMismatchError).
 */
static int
check_sharding_hashes(const char *space, uint32_t space_len,
		      bool has_func_hash, uint64_t func_hash,
		      bool has_key_hash, uint64_t key_hash)
{
	struct hash_cache_entry *e = hash_cache_find(space, space_len);
	if (e == NULL)
		e = hash_cache_fill(space, space_len);
	if (e == NULL)
		return -1;
	if (e->has_func_hash != has_func_hash ||
	    (has_func_hash && e->func_hash != func_hash))
		return -1;
	if (e->has_key_hash != has_key_hash ||
	    (has_key_hash && e->key_hash != key_hash))
		return -1;
	return 0;
}

/** Parsed subset of the per-op opts map. */
struct dml_opts {
	bool skip_sharding_hash_check;
	bool noreturn;
	bool has_func_hash;
	uint64_t func_hash;
	bool has_key_hash;
	uint64_t key_hash;
};

/**
 * Parse the opts map. Whitelisted keys only; any unknown key or
 * an unsupported value (fetch_latest_metadata=true, non-nil
 * fields) means fallback -> returns -1. opts may be MP_NIL or
 * absent (*p >= args_end).
 */
static int
parse_opts(const char **p, const char *args_end, struct dml_opts *o)
{
	memset(o, 0, sizeof(*o));
	if (*p >= args_end)
		return 0;
	if (mp_typeof(**p) == MP_NIL) {
		mp_decode_nil(p);
		return 0;
	}
	if (mp_typeof(**p) != MP_MAP)
		return -1;
	uint32_t n = mp_decode_map(p);
	for (uint32_t i = 0; i < n; i++) {
		if (mp_typeof(**p) != MP_STR)
			return -1;
		uint32_t klen;
		const char *k = mp_decode_str(p, &klen);
		if (klen == 9 && memcmp(k, "bucket_id", 9) == 0) {
			/* The vshard wrapper holds the ref. */
			mp_next(p);
		} else if (klen == 18 &&
			   memcmp(k, "sharding_func_hash", 18) == 0) {
			if (mp_typeof(**p) == MP_UINT) {
				o->has_func_hash = true;
				o->func_hash = mp_decode_uint(p);
			} else if (mp_typeof(**p) == MP_NIL) {
				mp_decode_nil(p);
			} else {
				return -1;
			}
		} else if (klen == 17 &&
			   memcmp(k, "sharding_key_hash", 17) == 0) {
			if (mp_typeof(**p) == MP_UINT) {
				o->has_key_hash = true;
				o->key_hash = mp_decode_uint(p);
			} else if (mp_typeof(**p) == MP_NIL) {
				mp_decode_nil(p);
			} else {
				return -1;
			}
		} else if (klen == 24 &&
			   memcmp(k, "skip_sharding_hash_check", 24) == 0) {
			if (mp_typeof(**p) != MP_BOOL)
				return -1;
			o->skip_sharding_hash_check = mp_decode_bool(p);
		} else if (klen == 8 && memcmp(k, "noreturn", 8) == 0) {
			if (mp_typeof(**p) != MP_BOOL)
				return -1;
			o->noreturn = mp_decode_bool(p);
		} else if (klen == 21 &&
			   memcmp(k, "fetch_latest_metadata", 21) == 0) {
			if (mp_typeof(**p) == MP_BOOL && !mp_decode_bool(p))
				continue;
			/* true or unexpected type -> fallback. */
			return -1;
		} else if (klen == 21 &&
			   memcmp(k, "add_space_schema_hash", 21) == 0) {
			/*
			 * Only affects the error path, which falls
			 * back to Lua anyway.
			 */
			mp_next(p);
		} else if (klen == 6 && memcmp(k, "fields", 6) == 0) {
			/* Field filtering is not done in C. */
			if (mp_typeof(**p) != MP_NIL)
				return -1;
			mp_decode_nil(p);
		} else {
			return -1;
		}
	}
	return 0;
}

/**
 * Normalize a key argument: MP_ARRAY passes through, a scalar is
 * wrapped into a one-element array on the region, MP_NIL and the
 * rest mean fallback (returns -1).
 */
static int
normalize_key(const char **p, const char **key, const char **key_end)
{
	enum mp_type t = mp_typeof(**p);
	if (t == MP_ARRAY) {
		*key = *p;
		mp_next(p);
		*key_end = *p;
		return 0;
	}
	if (t == MP_NIL || t == MP_MAP)
		return -1;
	const char *part = *p;
	mp_next(p);
	size_t part_size = *p - part;
	char *buf = box_region_alloc(mp_sizeof_array(1) + part_size);
	if (buf == NULL)
		return -1;
	char *e = mp_encode_array(buf, 1);
	memcpy(e, part, part_size);
	*key = buf;
	*key_end = e + part_size;
	return 0;
}

/**
 * Emit the success reply: {res = tuple} or an empty map when
 * there is no tuple to return (miss / noreturn / upsert).
 */
static int
reply_result(box_function_ctx_t *ctx, box_tuple_t *tuple, bool noreturn)
{
	crud_c.stat_fastpath++;
	if (noreturn || tuple == NULL) {
		static const char empty_map[] = {(char)0x80};
		box_return_mp(ctx, empty_map, empty_map + 1);
		return 0;
	}
	size_t bsize = box_tuple_bsize(tuple);
	size_t size = mp_sizeof_map(1) + mp_sizeof_str(3) + bsize;
	char *buf = box_region_alloc(size);
	if (buf == NULL)
		return -1;
	char *e = mp_encode_map(buf, 1);
	e = mp_encode_str(e, "res", 3);
	ssize_t rc = box_tuple_to_buf(tuple, e, bsize);
	if (rc < 0 || (size_t)rc != bsize) {
		box_error_set(__FILE__, __LINE__, ER_PROC_C_CODE,
			      "box_tuple_to_buf failed");
		return -1;
	}
	box_return_mp(ctx, buf, e + bsize);
	return 0;
}

/**
 * crud.storage_c.get(space_name, key, field_names, opts)
 */
EXPORT int
get(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	const char *p = args;
	if (mp_typeof(*p) != MP_ARRAY || mp_decode_array(&p) < 2)
		return fallback(ctx, "get", args, args_end);
	const char *space;
	uint32_t space_len;
	int64_t space_id = decode_space(&p, &space, &space_len);
	if (space_id < 0)
		return fallback(ctx, "get", args, args_end);
	const char *key, *key_end;
	if (normalize_key(&p, &key, &key_end) != 0)
		return fallback(ctx, "get", args, args_end);
	/* field_names must be absent - filtering stays in Lua. */
	if (p < args_end) {
		if (mp_typeof(*p) != MP_NIL)
			return fallback(ctx, "get", args, args_end);
		mp_decode_nil(&p);
	}
	struct dml_opts o;
	if (parse_opts(&p, args_end, &o) != 0 || o.noreturn)
		return fallback(ctx, "get", args, args_end);
	if (!o.skip_sharding_hash_check &&
	    check_sharding_hashes(space, space_len, o.has_func_hash,
				  o.func_hash, o.has_key_hash,
				  o.key_hash) != 0)
		return fallback(ctx, "get", args, args_end);
	box_tuple_t *tuple;
	if (box_index_get(space_id, 0, key, key_end, &tuple) != 0) {
		/* The Lua op reproduces the error properly. */
		box_error_clear();
		return fallback(ctx, "get", args, args_end);
	}
	return reply_result(ctx, tuple, false);
}

/**
 * Common part of the tuple-taking ops (insert, replace).
 */
static int
tuple_op(box_function_ctx_t *ctx, const char *op, const char *args,
	 const char *args_end,
	 int (*box_op)(uint32_t, const char *, const char *, box_tuple_t **))
{
	const char *p = args;
	if (mp_typeof(*p) != MP_ARRAY || mp_decode_array(&p) < 2)
		return fallback(ctx, op, args, args_end);
	const char *space;
	uint32_t space_len;
	int64_t space_id = decode_space(&p, &space, &space_len);
	if (space_id < 0)
		return fallback(ctx, op, args, args_end);
	if (mp_typeof(*p) != MP_ARRAY)
		return fallback(ctx, op, args, args_end);
	const char *tuple = p;
	mp_next(&p);
	const char *tuple_end = p;
	struct dml_opts o;
	if (parse_opts(&p, args_end, &o) != 0)
		return fallback(ctx, op, args, args_end);
	if (!o.skip_sharding_hash_check &&
	    check_sharding_hashes(space, space_len, o.has_func_hash,
				  o.func_hash, o.has_key_hash,
				  o.key_hash) != 0)
		return fallback(ctx, op, args, args_end);
	box_tuple_t *result;
	if (box_op(space_id, tuple, tuple_end, &result) != 0) {
		box_error_clear();
		return fallback(ctx, op, args, args_end);
	}
	return reply_result(ctx, result, o.noreturn);
}

/**
 * crud.storage_c.replace(space_name, tuple, opts)
 */
EXPORT int
replace(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	return tuple_op(ctx, "replace", args, args_end, box_replace);
}

/**
 * crud.storage_c.insert(space_name, tuple, opts)
 */
EXPORT int
insert(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	return tuple_op(ctx, "insert", args, args_end, box_insert);
}

/**
 * crud.storage_c.update(space_name, key, operations, field_names,
 *                       opts)
 */
EXPORT int
update(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	const char *p = args;
	if (mp_typeof(*p) != MP_ARRAY || mp_decode_array(&p) < 3)
		return fallback(ctx, "update", args, args_end);
	const char *space;
	uint32_t space_len;
	int64_t space_id = decode_space(&p, &space, &space_len);
	if (space_id < 0)
		return fallback(ctx, "update", args, args_end);
	const char *key, *key_end;
	if (normalize_key(&p, &key, &key_end) != 0)
		return fallback(ctx, "update", args, args_end);
	if (mp_typeof(*p) != MP_ARRAY)
		return fallback(ctx, "update", args, args_end);
	const char *ops = p;
	mp_next(&p);
	const char *ops_end = p;
	/* field_names must be absent - filtering stays in Lua. */
	if (p < args_end) {
		if (mp_typeof(*p) != MP_NIL)
			return fallback(ctx, "update", args, args_end);
		mp_decode_nil(&p);
	}
	struct dml_opts o;
	if (parse_opts(&p, args_end, &o) != 0)
		return fallback(ctx, "update", args, args_end);
	if (!o.skip_sharding_hash_check &&
	    check_sharding_hashes(space, space_len, o.has_func_hash,
				  o.func_hash, o.has_key_hash,
				  o.key_hash) != 0)
		return fallback(ctx, "update", args, args_end);
	box_tuple_t *result;
	/*
	 * index_base = 1: crud passes the operations in the Lua
	 * convention. A failed update also covers the legacy
	 * field-not-found retry, which lives in the Lua op.
	 */
	if (box_update(space_id, 0, key, key_end, ops, ops_end, 1,
		       &result) != 0) {
		box_error_clear();
		return fallback(ctx, "update", args, args_end);
	}
	return reply_result(ctx, result, o.noreturn);
}

/**
 * crud.storage_c.upsert(space_name, tuple, operations, opts)
 */
EXPORT int
upsert(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	const char *p = args;
	if (mp_typeof(*p) != MP_ARRAY || mp_decode_array(&p) < 3)
		return fallback(ctx, "upsert", args, args_end);
	const char *space;
	uint32_t space_len;
	int64_t space_id = decode_space(&p, &space, &space_len);
	if (space_id < 0)
		return fallback(ctx, "upsert", args, args_end);
	if (mp_typeof(*p) != MP_ARRAY)
		return fallback(ctx, "upsert", args, args_end);
	const char *tuple = p;
	mp_next(&p);
	const char *tuple_end = p;
	if (mp_typeof(*p) != MP_ARRAY)
		return fallback(ctx, "upsert", args, args_end);
	const char *ops = p;
	mp_next(&p);
	const char *ops_end = p;
	struct dml_opts o;
	if (parse_opts(&p, args_end, &o) != 0)
		return fallback(ctx, "upsert", args, args_end);
	if (!o.skip_sharding_hash_check &&
	    check_sharding_hashes(space, space_len, o.has_func_hash,
				  o.func_hash, o.has_key_hash,
				  o.key_hash) != 0)
		return fallback(ctx, "upsert", args, args_end);
	box_tuple_t *result;
	if (box_upsert(space_id, 0, tuple, tuple_end, ops, ops_end, 1,
		       &result) != 0) {
		box_error_clear();
		return fallback(ctx, "upsert", args, args_end);
	}
	/* upsert never returns a tuple. */
	return reply_result(ctx, NULL, true);
}

/**
 * crud.storage_c.delete(space_name, key, field_names, opts)
 *
 * The symbol is 'delete_' - 'delete' is a C++ keyword and some
 * toolchains dislike it; the Lua side registers the function
 * under the name 'crud.storage_c.delete_'.
 */
EXPORT int
delete_(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	const char *p = args;
	if (mp_typeof(*p) != MP_ARRAY || mp_decode_array(&p) < 2)
		return fallback(ctx, "delete", args, args_end);
	const char *space;
	uint32_t space_len;
	int64_t space_id = decode_space(&p, &space, &space_len);
	if (space_id < 0)
		return fallback(ctx, "delete", args, args_end);
	const char *key, *key_end;
	if (normalize_key(&p, &key, &key_end) != 0)
		return fallback(ctx, "delete", args, args_end);
	if (p < args_end) {
		if (mp_typeof(*p) != MP_NIL)
			return fallback(ctx, "delete", args, args_end);
		mp_decode_nil(&p);
	}
	struct dml_opts o;
	if (parse_opts(&p, args_end, &o) != 0)
		return fallback(ctx, "delete", args, args_end);
	if (!o.skip_sharding_hash_check &&
	    check_sharding_hashes(space, space_len, o.has_func_hash,
				  o.func_hash, o.has_key_hash,
				  o.key_hash) != 0)
		return fallback(ctx, "delete", args, args_end);
	box_tuple_t *result;
	if (box_delete(space_id, 0, key, key_end, &result) != 0) {
		box_error_clear();
		return fallback(ctx, "delete", args, args_end);
	}
	return reply_result(ctx, result, o.noreturn);
}
