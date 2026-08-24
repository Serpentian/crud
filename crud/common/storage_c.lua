--- Lua side of the crud.storage_c C module (prototype).
---
--- The C module implements the happy path of the simple DML
--- storage functions; whenever a request goes off it, the C code
--- delegates the whole request back to the Lua
--- '<op>_on_storage' implementation through the helpers table
--- stashed here. The .so must NOT be ffi.load()ed: tarantool's
--- module cache copies the file before dlopen, so all the
--- sharing goes through the vtable pointer returned by the
--- 'crud.storage_c.setup' stored function.
local ffi = require('ffi')
local log = require('log')
local msgpack = require('msgpack')
local fiber = require('fiber')

local utils = require('crud.common.utils')
local vshard_utils = require('crud.common.vshard_utils')
local storage_metadata_cache = require('crud.common.sharding.storage_metadata_cache')

pcall(ffi.cdef, [[
struct crud_storage_c_api {
    void (*sharding_cache_invalidate)(const char *name, uint32_t len);
    void (*sharding_cache_clear)(void);
    uint64_t (*stat_get)(int which);
};
]])

local M = {
    -- The feature flag, read once. Router side it only selects
    -- the storage function NAME (C or Lua) - the transport is
    -- vshard.router.call either way.
    enabled = (function()
        local v = os.getenv('CRUD_C_DML')
        return v ~= nil and v ~= '' and v ~= '0'
    end)(),
    -- Casted 'struct crud_storage_c_api *' after setup.
    api = nil,
    -- Ops ported to C, appended one per commit.
    ops = {
        get = true,
    },
}

--- Storage function name for the op: the C one when the flag is
--- on and the op is ported, the Lua one otherwise.
function M.func_name(op)
    if M.enabled and M.ops[op] then
        return 'crud.storage_c.' .. op
    end
    return utils.get_storage_call(op .. '_on_storage')
end

local function encode_reply(ok, ret1, ret2, ret3)
    local NULL = msgpack.NULL
    if ret3 ~= nil then
        return msgpack.encode({ok == nil and NULL or ok,
                               ret1 == nil and NULL or ret1,
                               ret2 == nil and NULL or ret2, ret3})
    end
    if ret2 ~= nil then
        return msgpack.encode({ok == nil and NULL or ok,
                               ret1 == nil and NULL or ret1, ret2})
    end
    if ret1 ~= nil then
        return msgpack.encode({ok == nil and NULL or ok, ret1})
    end
    return msgpack.encode({ok})
end

--- The helpers the C module delegates to.
local helpers = {}

--- Re-execute the whole request through the Lua op. Errors (if
--- the op raises) propagate to the C caller as a Lua error -
--- identical to what the Lua op would produce.
function helpers.call_op(op, args_mp)
    local func = rawget(_G, utils.STORAGE_NAMESPACE)[op .. '_on_storage']
    if func == nil then
        error(('crud C fallback: %s_on_storage is not registered'):format(op))
    end
    local args = msgpack.decode(args_mp)
    return encode_reply(func(args[1], args[2], args[3], args[4], args[5]))
end

--- Sharding hashes of the space for the C-side cache. The Lua
--- cache lazily installs the _ddl_* on_replace triggers, so
--- whenever C holds a value, the invalidation hooks are wired.
function helpers.get_sharding_hashes(space_name)
    local func_hash = storage_metadata_cache.get_sharding_func_hash(space_name)
    local key_hash = storage_metadata_cache.get_sharding_key_hash(space_name)
    return msgpack.encode({func_hash or msgpack.NULL,
                           key_hash or msgpack.NULL})
end

local function cache_invalidate(space_name)
    if M.api ~= nil then
        M.api.sharding_cache_invalidate(space_name, #space_name)
    end
end

local function cache_clear()
    if M.api ~= nil then
        M.api.sharding_cache_clear()
    end
end

--- Fast path / fallback counters, for tests and benchmarks.
function M.stats()
    if M.api == nil then
        return nil
    end
    return {
        fastpath = tonumber(M.api.stat_get(0)),
        fallback = tonumber(M.api.stat_get(1)),
    }
end

local function setup()
    local func = box.func and box.func['crud.storage_c.setup']
    if func == nil then
        return false
    end
    rawset(_G, '__crud_storage_c_helpers', helpers)
    local ok, ptr = pcall(func.call, func, {})
    rawset(_G, '__crud_storage_c_helpers', nil)
    if not ok then
        log.warn('crud.storage_c: setup failed - %s', ptr)
        return false
    end
    M.api = ffi.cast('struct crud_storage_c_api *', ptr)
    storage_metadata_cache.set_c_cache_hooks(cache_invalidate, cache_clear)
    log.info('crud.storage_c: the C DML implementation is enabled')
    return true
end

local function func_names()
    local names = {'crud.storage_c.setup'}
    for op in pairs(M.ops) do
        table.insert(names, 'crud.storage_c.' .. op)
    end
    return names
end

local function try_init()
    if not box.info.ro then
        local user = vshard_utils.get_this_replica_user() or 'guest'
        for _, name in ipairs(func_names()) do
            box.schema.func.create(name, {language = 'C',
                                          if_not_exists = true})
            box.schema.user.grant(user, 'execute', 'function', name,
                                  {if_not_exists = true})
        end
    end
    if box.func == nil or box.func['crud.storage_c.setup'] == nil then
        return false
    end
    return setup()
end

--- Register the C functions (on a writable master) and perform
--- the handshake. Never fails: when the instance can not do it
--- right now (read-only and the _func rows are not replicated
--- yet, the .so is missing), a background fiber keeps retrying.
function M.init_storage()
    if not M.enabled then
        return
    end
    local ok, res = pcall(try_init)
    if ok and res then
        return
    end
    if not ok then
        log.warn('crud.storage_c: init failed - %s, retrying in background',
                 res)
    end
    fiber.new(function()
        fiber.name('crud.storage_c_init', {truncate = true})
        for _ = 1, 120 do
            fiber.sleep(1)
            if M.api ~= nil then
                return
            end
            local f_ok, f_res = pcall(try_init)
            if f_ok and f_res then
                return
            end
        end
        log.warn('crud.storage_c: gave up enabling the C DML path')
    end)
end

return M
