-- Benchmark of the crud simple DML operations through the
-- router. Uses the public crud API only, so the very same script
-- runs against any crud revision (see run.sh, which drives the
-- old/new comparison).
local clock = require('clock')
local fiber = require('fiber')

local RS_UUID = 'cbf06940-0790-498b-948d-042b62cf3d29'
local INST_UUID = '8a274925-a26d-47fc-9e1b-af88ce939412'
local BUCKET_COUNT = 3000

local FIBERS = tonumber(os.getenv('BENCH_FIBERS')) or 10
local DURATION = tonumber(os.getenv('BENCH_DURATION')) or 10
local WARMUP = tonumber(os.getenv('BENCH_WARMUP')) or 2
local LABEL = os.getenv('BENCH_LABEL') or 'crud'
-- Sample every Nth request latency to keep the overhead low.
local SAMPLE = 16

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
vshard.router.cfg(cfg)
vshard.router.bootstrap_first = nil

local crud = require('crud')
crud.init_router()

local payload = string.rep('x', 32)

-- Wait for the storage.
local deadline = clock.monotonic() + 60
while true do
    local _, err = crud.replace('bench', {1, box.NULL, payload},
                                {timeout = 5})
    if err == nil then
        break
    end
    if clock.monotonic() > deadline then
        io.stderr:write('storage is not available: '..tostring(err)..'\n')
        os.exit(1)
    end
    fiber.sleep(0.1)
end

local workloads = {
    {
        name = 'replace',
        call = function(i)
            return crud.replace('bench', {i % 100000, box.NULL, payload},
                                {timeout = 10})
        end,
    },
    {
        name = 'get',
        call = function(i)
            return crud.get('bench', i % 100000, {timeout = 10})
        end,
    },
    {
        name = 'update',
        call = function(i)
            return crud.update('bench', i % 100000,
                               {{'=', 'payload', payload}}, {timeout = 10})
        end,
    },
    {
        name = 'insert+delete',
        call = function(i)
            local id = 200000 + i % 100000
            crud.insert('bench', {id, box.NULL, payload}, {timeout = 10})
            return crud.delete('bench', id, {timeout = 10})
        end,
    },
}

local function percentile(sorted, p)
    if #sorted == 0 then
        return 0
    end
    local idx = math.max(1, math.ceil(#sorted * p))
    return sorted[idx]
end

local function run_workload(w)
    local stop = false
    local measuring = false
    local counts = {}
    local errors = 0
    local lats = {}
    local fibers = {}
    for f = 1, FIBERS do
        counts[f] = 0
        fibers[f] = fiber.new(function()
            local i = f * 1000003
            while not stop do
                i = i + 1
                local sample = measuring and i % SAMPLE == 0
                local t0
                if sample then
                    t0 = clock.monotonic64()
                end
                local _, err = w.call(i)
                if err ~= nil then
                    errors = errors + 1
                elseif sample then
                    table.insert(lats,
                                 tonumber(clock.monotonic64() - t0) / 1e3)
                end
                counts[f] = counts[f] + 1
            end
        end)
        fibers[f]:set_joinable(true)
    end
    local function total()
        local s = 0
        for f = 1, FIBERS do
            s = s + counts[f]
        end
        return s
    end
    fiber.sleep(WARMUP)
    local c0 = total()
    lats = {}
    measuring = true
    local t0 = clock.monotonic()
    fiber.sleep(DURATION)
    local c1 = total()
    local elapsed = clock.monotonic() - t0
    stop = true
    for f = 1, FIBERS do
        fibers[f]:join()
    end
    table.sort(lats)
    return {
        rps = (c1 - c0) / elapsed,
        p50 = percentile(lats, 0.50),
        p95 = percentile(lats, 0.95),
        p99 = percentile(lats, 0.99),
        errors = errors,
    }
end

print(('# %s, fibers: %d, duration: %ds'):format(LABEL, FIBERS, DURATION))
print(('%-14s %10s %9s %9s %9s %7s'):format(
      'workload', 'RPS', 'p50(us)', 'p95(us)', 'p99(us)', 'errors'))
for _, w in ipairs(workloads) do
    local r = run_workload(w)
    print(('%-14s %10.0f %9.1f %9.1f %9.1f %7d'):format(
          w.name, r.rps, r.p50, r.p95, r.p99, r.errors))
end
os.exit(0)
