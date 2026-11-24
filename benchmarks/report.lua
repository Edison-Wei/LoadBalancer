-- Same response-hook overhead in every trial, including direct baselines.
local threads = {}
function setup(thread)
  table.insert(threads, thread)
end
function init(args)
  statuses, backends, invalid = {}, {}, 0
end
function response(status, headers, body)
  statuses[status] = (statuses[status] or 0) + 1
  if status == 200 then
    local id = body:match('"server_id"%s*:%s*"([%w_.-]+)"')
    if id then backends[id] = (backends[id] or 0) + 1
    else invalid = invalid + 1 end
  end
end
local function encode(values)
  local entries = {}
  for k,v in pairs(values) do table.insert(entries, string.format('"%s":%d', k, v)) end
  table.sort(entries)
  return '{' .. table.concat(entries, ',') .. '}'
end
function done(summary, latency, requests)
  local statuses, backends, invalid = {}, {}, 0
  for _, thread in ipairs(threads) do
    for k,v in pairs(thread:get('statuses')) do statuses[k] = (statuses[k] or 0) + v end
    for k,v in pairs(thread:get('backends')) do backends[k] = (backends[k] or 0) + v end
    invalid = invalid + thread:get('invalid')
  end
  io.write(string.format('LB_WRK_JSON {"requests":%d,"duration_seconds":%.6f,"rps":%.6f,' ..
    '"p50_ms":%.6f,"p95_ms":%.6f,"p99_ms":%.6f,"statuses":%s,"backends":%s,' ..
    '"invalid_identities":%d,"errors":%s}\n', summary.requests, summary.duration / 1e6,
    summary.requests / (summary.duration / 1e6), latency:percentile(50) / 1000,
    latency:percentile(95) / 1000, latency:percentile(99) / 1000,
    encode(statuses), encode(backends), invalid, encode(summary.errors)))
end
