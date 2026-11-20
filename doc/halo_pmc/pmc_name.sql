-- HaloPMC counter catalog
-- Run this script in the database that will execute the profiled SQL.

DROP TABLE IF EXISTS pmc_name;

CREATE TABLE pmc_name
(
    pmc_name  text PRIMARY KEY,
    pmtype    bigint,
    pe_config bigint,
    config1   bigint,
    config2   bigint
);

-- pmtype: 0 = PERF_TYPE_HARDWARE, 1 = PERF_TYPE_SOFTWARE,
--         3 = PERF_TYPE_HW_CACHE, 4 = PERF_TYPE_RAW
INSERT INTO pmc_name (pmc_name, pmtype, pe_config, config1, config2) VALUES
    ('Instructions',                    0,     1, 0, 0),
    ('cycles',                          0,     0, 0, 0),
    ('task_clock',                      1,     1, 0, 0),
    ('PERF_COUNT_HW_CACHE_L1D_RD',      3,     0, 0, 0),
    ('PERF_COUNT_HW_CACHE_L1D_MISS_RD', 3, 65536, 0, 0),
    ('MEM_INST_RETIRED_ALL_STORES',     4, 33488, 0, 0),
    ('BR_INST_RETIRED_ALL_BRANCHES',    4,   196, 0, 0),
    ('BR_MISP_RETIRED_ALL_BRANCHES',    4,   197, 0, 0),
    ('RESOURCE_STALLS_ANY',              4,   418, 0, 0);

-- Example:
--   CREATE TABLE t1 (id int, name text, balance int);
--   CREATE TABLE t2 (id int, name text, balance int);
--   INSERT INTO t1 SELECT g % 1000, 'n' || g, g FROM generate_series(1,200000) g;
--   INSERT INTO t2 SELECT g % 2000, 'm' || g, g FROM generate_series(1,200000) g;
--   SET max_parallel_workers_per_gather = 0;   -- 让计划走普通 Hash Join
--   SET halo_pmc_list = 'Instructions,cycles,PERF_COUNT_HW_CACHE_L1D_RD,MEM_INST_RETIRED_ALL_STORES';
--   SELECT count(*) FROM t1 JOIN t2 ON t1.id = t2.id;
