--- Module to call vshard.storage.bucket_ref / vshard.storage.bucket_unref
--- on storage requests. The refs protect the buckets from being moved by the
--- rebalancer while a request is in progress.

--- bucket_ref/bucket_unref must be called in one transaction in order to
--- guarantee the unref matches the ref.

local vshard = require('vshard')
local errors = require('errors')

local bucket_ref_unref = {
    BucketRefError = errors.new_class('bucket_ref_error', {capture_stack = false})
}

local function make_bucket_ref_err(bucket_id, vshard_ref_err)
    local err = bucket_ref_unref.BucketRefError:new(
        "failed bucket_ref: %s, bucket_id: %s",
        vshard_ref_err.name,
        bucket_id
    )
    err.bucket_ref_errs = {
        {
            bucket_id = bucket_id,
            vshard_err = vshard_ref_err,
        }
    }
    return err
end

--- bucket_refrw implementation that calls vshard.storage.bucket_refrw.
--- must be called with bucket_unrefrw in transaction
function bucket_ref_unref.bucket_refrw(bucket_id)
    local ref_ok, vshard_ref_err = vshard.storage.bucket_refrw(bucket_id)
    if not ref_ok then
        return nil, make_bucket_ref_err(bucket_id, vshard_ref_err)
    end

    return true, nil, bucket_ref_unref.bucket_unrefrw
end

--- bucket_unrefrw implementation that calls vshard.storage.bucket_unrefrw.
--- must be called with bucket_refrw in transaction
function bucket_ref_unref.bucket_unrefrw(bucket_id)
    return vshard.storage.bucket_unrefrw(bucket_id)
end

--- bucket_refro implementation that calls vshard.storage.bucket_refro.
function bucket_ref_unref.bucket_refro(bucket_id)
    local ref_ok, vshard_ref_err = vshard.storage.bucket_refro(bucket_id)
    if not ref_ok then
        return nil, make_bucket_ref_err(bucket_id, vshard_ref_err)
    end

    return true, nil, bucket_ref_unref.bucket_unrefro
end

--- bucket_unrefro implementation that calls vshard.storage.bucket_unrefro.
--- must be called in one transaction with bucket_refro
function bucket_ref_unref.bucket_unrefro(bucket_id)
    return vshard.storage.bucket_unrefro(bucket_id)
end

--- bucket_refrw_many that calls bucket_refrw for every bucket and aggregates
--- errors
--- @param bucket_ids table<number, boolean>
function bucket_ref_unref.bucket_refrw_many(bucket_ids)
    local bucket_ref_errs = {}
    local reffed_bucket_ids = {}
    for bucket_id in pairs(bucket_ids) do
        local ref_ok, bucket_refrw_err = bucket_ref_unref.bucket_refrw(bucket_id)
        if not ref_ok then
            table.insert(bucket_ref_errs, bucket_refrw_err.bucket_ref_errs[1])
            break
        end
        reffed_bucket_ids[bucket_id] = true
    end

    if next(bucket_ref_errs) ~= nil then
        local err = bucket_ref_unref.BucketRefError:new(bucket_ref_unref.BucketRefError:new("failed bucket_ref"))
        err.bucket_ref_errs = bucket_ref_errs
        bucket_ref_unref.bucket_unrefrw_many(reffed_bucket_ids)
        return nil, err
    end

    return true, nil, bucket_ref_unref.bucket_unrefrw_many
end

--- bucket_unrefrw_many that calls vshard.storage.bucket_unrefrw for every
--- bucket.
--- must be called in one transaction with bucket_refrw_many
--- @param bucket_ids table<number, boolean>
function bucket_ref_unref.bucket_unrefrw_many(bucket_ids)
    local unref_all_ok = true
    local unref_last_err
    for reffed_bucket_id in pairs(bucket_ids) do
        local unref_ok, unref_err = bucket_ref_unref.bucket_unrefrw(reffed_bucket_id)
        if not unref_ok then
            unref_all_ok = false
            unref_last_err = unref_err
        end
    end

    if not unref_all_ok then
        return nil, unref_last_err
    end
    return true
end

return bucket_ref_unref
