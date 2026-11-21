-- HaloPMC counter catalog
-- Run this script in the database that will execute the profiled SQL.
--
-- The layout matches the lecture's "创建PMC表.txt":
--   pmc_name(pcm_name, pmutype, pe_config, config1, config2)
-- so either this script or the lecture file can be imported.

DROP TABLE IF EXISTS pmc_name;

CREATE TABLE pmc_name
(
    pcm_name  varchar(40) PRIMARY KEY,
    pmutype   int,
    pe_config int,
    config1   int,
    config2   int
);

-- pmutype: 0 = PERF_TYPE_HARDWARE, 1 = PERF_TYPE_SOFTWARE,
--          3 = PERF_TYPE_HW_CACHE, 4 = PERF_TYPE_RAW
INSERT INTO pmc_name (pcm_name, pmutype, pe_config, config1, config2) VALUES
    ('cycles',                          0,         0, 0, 1),
    ('Instructions',                    0,         1, 0, 1),
    ('PERF_COUNT_HW_CACHE_L1D_RD',      3,         0, 0, 1),
    ('PERF_COUNT_HW_CACHE_L1D_MISS_RD', 3,     65536, 0, 1),
    ('MEM_INST_RETIRED_ALL_STORES',     4,     33488, 0, 1),
    ('cpu_clk_unhalted_cycles',         4,        60, 0, 1),
    ('mem_load_retired_l1_miss',        4,      2257, 0, 1),
    ('mem_load_retired_l2_miss',        4,      4305, 0, 1),
    ('mem_load_retired_l3_miss',        4,      8401, 0, 1),
    ('cycle_activity_stalls_mem_any',   4, 335549603, 0, 1),
    ('cycle_activity_stalls_total',     4,  67110051, 0, 1),
    ('cycle_activity_statlls_l1d_miss', 4, 201329827, 0, 1),
    ('cycle_activity_statlls_l2d_miss', 4,  83887523, 0, 1),
    ('cycle_activity_statlls_l3d_miss', 4, 100664995, 0, 1),
    ('UOPS_EXECUTED_1_UOP_EXEC',        4,  16777649, 0, 1),
    ('UOPS_EXECUTED_2_UOP_EXEC',        4,  33554865, 0, 1),
    ('UOPS_EXECUTED_3_UOP_EXEC',        4,  50332081, 0, 1),
    ('UOPS_EXECUTED_4_UOP_EXEC',        4,  67109297, 0, 1),
    ('ICache_Reads',                    4,       128, 0, 1),
    ('ICACHE_16B_IFDATA_ACCESS',        4,       896, 0, 1),
    ('ICACHE_64B_IFTAG_HIT',            4,       387, 0, 1),
    ('ICACHE_64B_IFTAG_MISS',           4,       643, 0, 1),
    ('ITLB_MISSES_WALK_COMPLETED',      4,      3717, 0, 1),
    ('ITLB_MISSES_STLB_HIT',            4,      8325, 0, 1),
    ('BR_INST_RETIRED_ALL_BRANCHES',    4,       196, 0, 1),
    ('BR_MISP_RETIRED_ALL_BRANCHES',    4,       197, 0, 1),
    ('RESOURCE_STALLS_ANY',             4,       418, 0, 1),
    -- software event, usable on WSL2/VMware without PMU passthrough
    ('task_clock',                      1,         1, 0, 1);

-- Example:
--   CREATE TABLE t1 (id int, name text, balance int);
--   CREATE TABLE t2 (id int, name text, balance int);
--   INSERT INTO t1 SELECT g % 1000, 'n' || g, g FROM generate_series(1,200000) g;
--   INSERT INTO t2 SELECT g % 2000, 'm' || g, g FROM generate_series(1,200000) g;
--   SET max_parallel_workers_per_gather = 0;   -- 让计划走普通 Hash Join
--   SET halo_pmc_list = 'Instructions,cycles,PERF_COUNT_HW_CACHE_L1D_RD,MEM_INST_RETIRED_ALL_STORES';
--   SELECT count(*) FROM t1 JOIN t2 ON t1.id = t2.id;
