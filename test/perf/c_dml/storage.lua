-- Storage instance for the crud simple-DML benchmark. Launched
-- by run.sh with LUA_PATH/LUA_CPATH pointing at the crud
-- revision under test and at our vshard.
local fio = require('fio')

local RS_UUID = 'cbf06940-0790-498b-948d-042b62cf3d29'
local INST_UUID = '8a274925-a26d-47fc-9e1b-af88ce939412'
local BUCKET_COUNT = 3000

local cfg = {
    bucket_count = BUCKET_COUNT,
    sharding = {
        [RS_UUID] = {
            replicas = {
                [INST_UUID] = {
                    uri = 'storage:storage@127.0.0.1:3321',
                    name = 'storage_1_a',
                    master = true,
                },
            },
        },
    },
}

local vshard = require('vshard')
rawset(_G, 'vshard', vshard)
vshard.storage.cfg(cfg, INST_UUID)
vshard.storage.bucket_force_create(1, BUCKET_COUNT)

box.schema.space.create('bench', {if_not_exists = true})
box.space.bench:format({
    {'id', 'unsigned'},
    {'bucket_id', 'unsigned'},
    {'payload', 'string'},
})
box.space.bench:create_index('pk', {parts = {'id'}, if_not_exists = true})
box.space.bench:create_index('bucket_id', {parts = {'bucket_id'},
                                           unique = false,
                                           if_not_exists = true})

require('crud').init_storage()

local f = fio.open('storage.pid', {'O_CREAT', 'O_WRONLY', 'O_TRUNC'},
                   tonumber('644', 8))
f:write(tostring(box.info.pid))
f:close()
print('storage is ready')
