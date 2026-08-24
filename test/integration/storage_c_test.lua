-- Checks that the C implementation of the storage DML functions
-- (CRUD_C_DML=1) actually runs its fast path and returns results
-- identical to the Lua one. Skipped unless the flag is set and
-- crud/storage_c.so is built.
local t = require('luatest')
local helpers = require('test.helper')

local pgroup = t.group('storage_c', helpers.backend_matrix({
    {engine = 'memtx'},
}))

pgroup.before_all(function(g)
    helpers.start_default_cluster(g, 'srv_simple_operations')
    g.is_c_enabled = g.cluster:server('s1-master'):exec(function()
        local storage_c = require('crud.common.storage_c')
        return storage_c.enabled and storage_c.api ~= nil
    end)
end)

pgroup.after_all(function(g)
    helpers.stop_cluster(g.cluster, g.params.backend)
end)

pgroup.before_each(function(g)
    t.skip_if(not g.is_c_enabled,
              'the C DML path is disabled: set CRUD_C_DML=1 and build ' ..
              'crud/storage_c/build.sh')
    helpers.truncate_space_on_cluster(g.cluster, 'customers')
end)

local function stats_on_storages(g)
    local total = {fastpath = 0, fallback = 0}
    helpers.call_on_storages(g.cluster, function(server)
        local s = server:exec(function()
            return require('crud.common.storage_c').stats()
        end)
        if s ~= nil then
            total.fastpath = total.fastpath + s.fastpath
            total.fallback = total.fallback + s.fallback
        end
    end)
    return total
end

--- get of an existing tuple must go through the C fast path and
--- return exactly what the Lua implementation returns.
pgroup.test_get_uses_fast_path = function(g)
    local tuple = {1, box.NULL, 'Elizabeth', 23}
    local _, err = g.router:call('crud.insert', {'customers', tuple})
    t.assert_equals(err, nil)

    local before = stats_on_storages(g)

    local result, err = g.router:call('crud.get', {'customers', 1})
    t.assert_equals(err, nil)
    t.assert_equals(#result.rows, 1)
    t.assert_equals(result.rows[1][1], 1)
    t.assert_equals(result.rows[1][3], 'Elizabeth')

    local after = stats_on_storages(g)
    t.assert_gt(after.fastpath, before.fastpath,
                'the C fast path was not used')
    t.assert_equals(after.fallback, before.fallback,
                    'the request unexpectedly fell back to Lua')
end

--- get of a missing tuple: an empty result, still the fast path.
pgroup.test_get_missing_uses_fast_path = function(g)
    local before = stats_on_storages(g)

    local result, err = g.router:call('crud.get', {'customers', 777})
    t.assert_equals(err, nil)
    t.assert_equals(result.rows, {})

    local after = stats_on_storages(g)
    t.assert_gt(after.fastpath, before.fastpath)
    t.assert_equals(after.fallback, before.fallback)
end

--- Options the C code does not support must fall back to the Lua
--- implementation and still produce the correct result.
pgroup.test_unsupported_opts_fall_back = function(g)
    local _, err = g.router:call('crud.insert',
                                 {'customers', {2, box.NULL, 'John', 25}})
    t.assert_equals(err, nil)

    local before = stats_on_storages(g)

    -- Field filtering is done in Lua only.
    local result, err = g.router:call('crud.get',
                                      {'customers', 2, {fields = {'name'}}})
    t.assert_equals(err, nil)
    t.assert_equals(#result.rows, 1)
    t.assert_equals(result.rows[1][1], 'John')

    local after = stats_on_storages(g)
    t.assert_gt(after.fallback, before.fallback,
                'the request did not fall back to Lua')
end

--- An unknown space is reported exactly like on the Lua path.
pgroup.test_unknown_space_error = function(g)
    local _, err = g.router:call('crud.get', {'no_such_space', 1})
    t.assert_not_equals(err, nil)
    t.assert_str_contains(err.str, 'no_such_space')
end
