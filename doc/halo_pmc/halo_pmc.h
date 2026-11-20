#ifndef HALO_PMC_H
#define HALO_PMC_H

#include "postgres.h"

#include <linux/perf_event.h>

#include "utils/hsearch.h"

struct read_format
{
	uint64_t	nr;
	uint64_t	time_enabled;
	uint64_t	time_running;
	struct
	{
		uint64_t	value;
		uint64_t	id;
	}			values[FLEXIBLE_ARRAY_MEMBER];
};

#define PRM_DATA_SIZE 40
#define PRM_IDX 10

struct pmc_data
{
	int			fd;
	uint64_t	id;
	char		name[64];
	uint32_t	pmtype;
	uint64_t	pe_config;
	uint64_t	config1;
	uint64_t	config2;
	int			pid;
	int			num;
	unsigned long value;
	unsigned long time_enabled;
	unsigned long time_running;
	double		scale;
	double		scale_value;
	struct perf_event_mmap_page *mmap;
	unsigned long long value2;
	unsigned long long valx;
	HTAB	   *pmcHashTab;
};

typedef struct PMCHashItem
{
	char		pmcName[40];
	uint64_t	pmtype;
	uint64_t	pe_config;
	uint64_t	config1;
	uint64_t	config2;
} PMCHashItem;

extern char *halo_pmc_list;
extern struct pmc_data _PmcData[PRM_IDX];

extern void PmcInit(void);
extern int	PmcOpen(int mmapflag);
extern void PmcOpenInternal(struct pmc_data *pmc, int mmapflag);
extern void SetPmcData(struct pmc_data *pmc, const char *name,
					   uint32_t pmtype, uint64_t pe_config,
					   uint64_t config1, uint64_t config2);
extern void PmcClose(struct pmc_data *pmc);
extern void PmcReadData(int cumulation);
extern void PmcPrint(void);
extern void PmcEnable(void);
extern void PmcDisable(void);
extern void PmcSampleBegin(void);
extern void PmcSampleEnd(void);
extern void PmcPrintSample(void);

#endif							/* HALO_PMC_H */
