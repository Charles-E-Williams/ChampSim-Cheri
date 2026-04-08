import json
import glob

base_config = {
  "block_size": 64,
  "page_size": 2097152,
  "heartbeat_frequency": 10000000,
  "num_cores": 1,
  "ooo_cpu": [
    {
      "frequency": 4000,
      "ifetch_buffer_size": 150,
      "decode_buffer_size": 75,
      "dispatch_buffer_size": 144,
      "register_file_size": 288,
      "rob_size": 512,
      "lq_size": 192,
      "sq_size": 114,
      "fetch_width": 10,
      "decode_width": 6,
      "dispatch_width": 6,
      "execute_width": 5,
      "lq_width": 3,
      "sq_width": 4,
      "retire_width": 8,
      "mispredict_penalty": 1,
      "scheduler_size": 205,
      "decode_latency": 5,
      "dispatch_latency": 2,
      "schedule_latency": 5,
      "execute_latency": 1,
      "branch_predictor": "tage_sc",
      "btb": "basic_btb"
    }
  ],
  "DIB": {
    "window_size": 16,
    "sets": 256,
    "ways": 8
  },
  "L1I": {
    "sets": 64,
    "ways": 8,
    "rq_size": 64,
    "wq_size": 64,
    "pq_size": 32,
    "mshr_size": 16,
    "latency": 4,
    "max_tag_check": 2,
    "max_fill": 2,
    "prefetch_as_load": False,
    "virtual_prefetch": True,
    "prefetch_activate": "LOAD,PREFETCH",
    "prefetcher": "no"
  },
  "L1D": {
    "sets": 64,
    "ways": 12,
    "rq_size": 192,
    "wq_size": 114,
    "pq_size": 192,
    "mshr_size": 32,
    "latency": 5,
    "max_tag_check": 2,
    "max_fill": 2,
    "prefetch_as_load": False,
    "virtual_prefetch": True,
    "prefetch_activate": "LOAD,PREFETCH",
    "prefetcher": "no"
  },
  "L2C": {
    "sets": 2048,
    "ways": 10,
    "rq_size": 64,
    "wq_size": 48,
    "pq_size": 64,
    "mshr_size": 64,
    "latency": 15,
    "max_tag_check": 1,
    "max_fill": 1,
    "prefetch_as_load": False,
    "virtual_prefetch": False,
    "prefetch_activate": "LOAD,PREFETCH",
    "prefetcher": "no"
  },
  "ITLB": {
    "sets": 32,
    "ways": 8,
    "rq_size": 16,
    "wq_size": 16,
    "pq_size": 0,
    "mshr_size": 16,
    "latency": 1,
    "max_tag_check": 2,
    "max_fill": 2,
    "prefetch_as_load": False
  },
  "DTLB": {
    "sets": 16,
    "ways": 6,
    "rq_size": 16,
    "wq_size": 16,
    "pq_size": 0,
    "mshr_size": 16,
    "latency": 1,
    "max_tag_check": 2,
    "max_fill": 2,
    "prefetch_as_load": False
  },
  "STLB": {
    "sets": 128,
    "ways": 16,
    "rq_size": 32,
    "wq_size": 32,
    "pq_size": 0,
    "mshr_size": 8,
    "latency": 7,
    "max_tag_check": 1,
    "max_fill": 1,
    "prefetch_as_load": False
  },
  "PTW": {
    "pscl5_set": 1,
    "pscl5_way": 2,
    "pscl4_set": 1,
    "pscl4_way": 8,
    "pscl3_set": 2,
    "pscl3_way": 16,
    "pscl2_set": 4,
    "pscl2_way": 16,
    "rq_size": 16,
    "mshr_size": 8,
    "max_read": 2,
    "max_write": 2
  },
  "LLC": {
    "frequency": 4000,
    "sets":  4096,
    "ways": 12,
    "rq_size": 192,
    "wq_size": 192,
    "pq_size": 64,
    "mshr_size": 48,
    "latency": 45,
    "max_tag_check": 1,
    "max_fill": 1,
    "prefetch_as_load": False,
    "virtual_prefetch": False,
    "prefetch_activate": "LOAD,PREFETCH",
    "prefetcher": "no",
    "replacement": "lru"
  },
  "physical_memory": {
    "data_rate": 3200,
    "channels": 1,
    "ranks": 1,
    "bankgroups": 8,
    "banks": 4,
    "bank_rows": 65536,
    "bank_columns": 1024,
    "channel_width": 8,
    "wq_size": 64,
    "rq_size": 64,
    "tCAS": 24,
    "tRCD": 24,
    "tRP": 24,
    "tRAS": 52,
    "refresh_period": 32,
    "refreshes_per_period": 8192
  },
  "virtual_memory": {
    "pte_page_size": 4096,
    "num_levels": 4,
    "minor_fault_penalty": 200,
    "randomization": 1
  }
}

for filename in glob.glob("*.json"):
    if filename == "vcpkg.json":
        continue

    try:
        with open(filename, 'r') as f:
            json_input_data = json.load(f)
    except Exception:
        continue

    json_output_data = json.loads(json.dumps(base_config))

    if "executable_name" in json_input_data:
        json_output_data["executable_name"] = json_input_data["executable_name"]

    for cache_level in ["L1I", "L1D", "L2C", "LLC"]:
        if cache_level in json_input_data and cache_level in json_output_data:
            if "prefetcher" in json_input_data[cache_level]:
                json_output_data[cache_level]["prefetcher"] = json_input_data[cache_level]["prefetcher"]
            if "replacement" in json_input_data[cache_level]:
                json_output_data[cache_level]["replacement"] = json_input_data[cache_level]["replacement"]

    with open(filename, 'w') as f:
        json.dump(json_output_data, f, indent=2)