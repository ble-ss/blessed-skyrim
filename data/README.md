# skybench run export

one row per run folder in runs.csv. campaigns.json lists each campaign's plan name and its run ids per configuration row, in repeat order. the harness writes no paired results into campaign.json or result.json, so paired_results is null; compare the run rows instead.

only whitelisted fields are copied. no paths, command lines, environment values, hostnames, user names, hashes or error text. a failed run carries a fixed fail_reason label only. times are utc. blank means the harness had no value.

## columns

- `run_id`: run folder name
- `campaign_id`: campaign this run belongs to, blank if none
- `date`: run start date, utc, yyyy-mm-dd
- `time_utc`: run start time, utc, hh:mm:ss
- `scene`: scene name passed to skybench (--scene)
- `backend`: native, stock dxvk, or fork
- `build`: dxvk build name (--dxvk-build) for dxvk runs, blank for native
- `config`: campaign plan row name, blank for runs outside a campaign
- `preset`: graphics preset (--preset)
- `res`: capture resolution, widthxheight
- `status`: ok or failed, from result ok
- `fail_reason`: the gate that failed: insufficient-capture, device-lost, short-capture, stall, restore-incomplete, other
- `contaminated`: yes if a build tool ran during the run, else no
- `avg_fps`: frames / capture seconds
- `median_fps`: 1000 / ft_p50_ms (same rule the harness uses for low1_fps)
- `low1_fps`: 1000 / ft_p99_ms, the harness 1% low
- `low0_1_fps`: 0.1% low fps as the harness stored it (1000 / 99.9th percentile frame time)
- `ft_mean_ms`: frame interval, mean, ms
- `ft_p50_ms`: frame interval, 50th percentile, ms
- `ft_p95_ms`: frame interval, 95th percentile, ms
- `ft_p99_ms`: frame interval, 99th percentile, ms
- `ft_max_ms`: frame interval, worst frame, ms
- `display_lat_p50_ms`: presentmon DisplayLatency over the game's frames, 50th percentile, ms. each backend is timed on its own clock
- `display_lat_mean_ms`: presentmon DisplayLatency, mean, ms
- `gpu_lat_p50_ms`: presentmon GPULatency, 50th percentile, ms
- `gpu_time_p50_ms`: gpu time per frame (MsGPUTime), 50th percentile, ms
- `itp_p50_ms`: input to photon latency, 50th percentile, ms (blank unless input taps were on)
- `itp_n`: number of frames that had an input to photon value
- `gpu_busy_p50_ms`: gpu busy per frame (MsGPUBusy), 50th percentile, ms
- `gpu_busy_mean_ms`: gpu busy per frame, mean, ms
- `gpu_bound_frac`: share of frames where gpu busy was at least 90 percent of the frame interval
- `frames`: frames captured after settle
- `capture_s`: capture span in seconds after settle
