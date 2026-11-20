/*-------------------------------------------------------------------------
 *
 * halo_pmc.c
 *	  CPU performance monitoring counter (PMC) instrumentation for openHalo/PG
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include "access/xact.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/halo_pmc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/varlena.h"

char	   *halo_pmc_list = NULL;

struct pmc_data _PmcData[PRM_IDX];

#define PMC_HASH_KEYSIZE 40

static inline long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
				int cpu, int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

/*
 * Read a counter without a syscall when the kernel allows user-space rdpmc.
 * Returns 0 if the event page is not usable.
 */
static uint64_t
pmc_read_mmap(struct perf_event_mmap_page *pc)
{
	uint64_t	count = 0;
	uint32_t	seq;
	uint32_t	index;

	if (pc == NULL)
		return 0;

	for (;;)
	{
		seq = pc->lock;
		__sync_synchronize();

		index = pc->index;
		if (index != 0 && pc->cap_user_rdpmc)
		{
#if defined(__x86_64__) || defined(__i386__)
			uint32_t	lo;
			uint32_t	hi;

			__asm__ __volatile__("rdpmc" : "=a"(lo), "=d"(hi) : "c"(index - 1));
			count = ((uint64_t) hi << 32) | lo;
#else
			count = 0;
#endif
		}

		__sync_synchronize();
		if (pc->lock == seq && !(seq & 1))
			break;
	}

	if (!pc->cap_user_rdpmc)
		return 0;

	if (pc->pmc_width < 64)
	{
		count <<= (64 - pc->pmc_width);
		count = (uint64_t) ((int64_t) count >> (64 - pc->pmc_width));
	}
	count += pc->offset;

	return count;
}

/*
 * Read one counter opened without PERF_FORMAT_GROUP.  The kernel returns
 * time_enabled, time_running, value (no nr for single-event reads).
 */
static bool
pmc_read_event(int fd, uint64_t *time_enabled, uint64_t *time_running,
			   uint64_t *value)
{
	uint64_t	buf[3];

	if (fd < 0)
		return false;
	if (read(fd, buf, sizeof(buf)) != (ssize_t) sizeof(buf))
		return false;

	*time_enabled = buf[0];
	*time_running = buf[1];
	*value = buf[2];
	return true;
}

static uint64_t
pmc_read_fd(int fd)
{
	uint64_t	te;
	uint64_t	tr;
	uint64_t	v;

	if (!pmc_read_event(fd, &te, &tr, &v))
		return 0;
	return v;
}

/*
 * Initialize (once per backend) the name -> PMU configuration hash from the
 * pmc_name table.  The table is created by pmc_name.sql.
 */
void
PmcInit(void)
{
	HASHCTL		hctl;
	HTAB	   *hTable;
	MemoryContext cxt;
	MemoryContext old_ctx;
	char	   *sql = "SELECT lower(pmc_name), pmtype, pe_config, config1, config2 FROM pmc_name";
	int			i;
	bool		pushed = false;
	bool		started = false;

	if (_PmcData[0].pmcHashTab != NULL)
		hash_destroy(_PmcData[0].pmcHashTab);

	cxt = AllocSetContextCreate(TopMemoryContext,
								"Halo PMC hash memctx",
								ALLOCSET_DEFAULT_SIZES);

	old_ctx = MemoryContextSwitchTo(cxt);
	memset(&hctl, 0, sizeof(hctl));
	hctl.keysize = PMC_HASH_KEYSIZE;
	hctl.entrysize = sizeof(PMCHashItem);
	hctl.hcxt = cxt;
	hTable = hash_create("Halo PMC hash", 1024, &hctl,
						 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	MemoryContextSwitchTo(old_ctx);

	_PmcData[0].pmcHashTab = hTable;

	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started = true;
	}
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}

	if (SPI_connect() != SPI_OK_CONNECT)
	{
		elog(WARNING, "Halo PMC: SPI_connect failed");
		goto done;
	}

	if (SPI_exec(sql, 0) != SPI_OK_SELECT)
	{
		elog(WARNING, "Halo PMC: relation pmc_name is not ready");
		SPI_finish();
		goto done;
	}

	for (i = 0; i < SPI_processed; i++)
	{
		HeapTuple	tuple = SPI_tuptable->vals[i];
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		char	   *tPmcName;
		char		hashKey[PMC_HASH_KEYSIZE];
		PMCHashItem *item;
		bool		isnull;
		bool		found;

		tPmcName = SPI_getvalue(tuple, tupdesc, 1);
		if (tPmcName == NULL)
			continue;

		memset(hashKey, 0, sizeof(hashKey));
		strlcpy(hashKey, tPmcName, sizeof(hashKey));
		item = (PMCHashItem *) hash_search(hTable, hashKey,
										   HASH_ENTER, &found);

		item->pmtype = (uint64_t) DatumGetInt64(SPI_getbinval(tuple, tupdesc, 2, &isnull));
		item->pe_config = (uint64_t) DatumGetInt64(SPI_getbinval(tuple, tupdesc, 3, &isnull));
		item->config1 = (uint64_t) DatumGetInt64(SPI_getbinval(tuple, tupdesc, 4, &isnull));
		item->config2 = (uint64_t) DatumGetInt64(SPI_getbinval(tuple, tupdesc, 5, &isnull));
	}

	SPI_finish();

done:
	if (pushed)
		PopActiveSnapshot();
	if (started)
		CommitTransactionCommand();
}

/*
 * Fill one _PmcData slot.
 */
void
SetPmcData(struct pmc_data *pmc, const char *name, uint32_t pmtype,
		   uint64_t pe_config, uint64_t config1, uint64_t config2)
{
	struct pmc_data *p = &pmc[pmc[0].num];

	p->fd = -1;
	p->id = 0;
	strlcpy(p->name, name, sizeof(p->name));
	p->pmtype = pmtype;
	p->pe_config = pe_config;
	p->config1 = config1;
	p->config2 = config2;
	p->pid = 0;
	p->num = 0;
	p->value = 0;
	p->time_enabled = 0;
	p->time_running = 0;
	p->scale = 1.0;
	p->scale_value = 0;
	p->mmap = NULL;
	p->value2 = 0;
	p->valx = 0;

	pmc[0].num++;
}

/*
 * Open one perf event for every configured counter.  mmapflag != 0 also
 * maps the perf event page, which enables the low-overhead rdpmc path.
 */
void
PmcOpenInternal(struct pmc_data *pmc, int mmapflag)
{
	int			i;
	long		pagesize;

	pagesize = sysconf(_SC_PAGESIZE);

	for (i = 0; i < pmc[0].num; i++)
	{
		struct pmc_data *p = &pmc[i];
		struct perf_event_attr pe;
		long		fd;

		memset(&pe, 0, sizeof(pe));
		pe.type = (uint32_t) p->pmtype;
		pe.size = sizeof(pe);
		pe.config = p->pe_config;
		pe.disabled = 1;
		pe.exclude_kernel = 1;
		pe.exclude_hv = 1;
		pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
			PERF_FORMAT_TOTAL_TIME_RUNNING;

		fd = perf_event_open(&pe, 0, -1, -1, 0);
		if (fd < 0)
		{
			int			save_errno = errno;

			elog(INFO, "Halo PMC: open %s failed: %s",
				 p->name, strerror(save_errno));
			p->fd = -1;
			continue;
		}
		p->fd = (int) fd;

		if (mmapflag)
		{
			void	   *m;

			m = mmap(NULL, (size_t) pagesize, PROT_READ, MAP_SHARED,
					 p->fd, 0);
			if (m != MAP_FAILED)
				p->mmap = (struct perf_event_mmap_page *) m;
		}
	}
}

/*
 * Entry point: parse halo_pmc_list, create the events, return the number of
 * configured counters.
 */
int
PmcOpen(int mmapflag)
{
	int			i;
	int			rawlen;
	List	   *elemList = NIL;
	ListCell   *l;
	char	   *rawstr;
	HTAB	   *hTable;

	_PmcData[0].num = 0;

	if (halo_pmc_list == NULL || halo_pmc_list[0] == '\0')
		return 0;

	if (_PmcData[0].pmcHashTab == NULL)
		PmcInit();

	hTable = _PmcData[0].pmcHashTab;
	if (hTable == NULL)
		return 0;

	rawstr = pstrdup(halo_pmc_list);
	rawlen = strlen(rawstr);
	for (i = 0; i < rawlen; i++)
		rawstr[i] = (char) pg_ascii_tolower((unsigned char) rawstr[i]);

	if (!SplitGUCList(rawstr, ',', &elemList))
		elog(ERROR, "Halo PMC: invalid list syntax in parameter \"%s\"",
			 halo_pmc_list);

	foreach(l, elemList)
	{
		char	   *pmcName = (char *) lfirst(l);
		char		hashKey[PMC_HASH_KEYSIZE];
		PMCHashItem *item;
		bool		found;

		memset(hashKey, 0, sizeof(hashKey));
		strlcpy(hashKey, pmcName, sizeof(hashKey));
		item = (PMCHashItem *) hash_search(hTable, hashKey,
										   HASH_FIND, &found);
		if (!found)
		{
			elog(INFO, "Halo PMC: not found %s in Hash.", pmcName);
			continue;
		}

		if (_PmcData[0].num >= PRM_IDX)
			break;

		SetPmcData(_PmcData, pmcName, (uint32_t) item->pmtype,
				   item->pe_config, item->config1, item->config2);
	}

	PmcOpenInternal(_PmcData, mmapflag);

	return _PmcData[0].num;
}

/*
 * Close all events and release the mapped pages.  The name hash is kept for
 * the lifetime of the backend.
 */
void
PmcClose(struct pmc_data *pmc)
{
	int			i;

	if (pmc == NULL)
		return;

	for (i = 0; i < pmc[0].num; i++)
	{
		struct pmc_data *p = &pmc[i];

		if (p->mmap != NULL)
		{
			munmap(p->mmap, (size_t) sysconf(_SC_PAGESIZE));
			p->mmap = NULL;
		}
		if (p->fd >= 0)
		{
			close(p->fd);
			p->fd = -1;
		}
	}
	pmc[0].num = 0;
}

/*
 * Read the accumulated values with a syscall on every fd.  cumulation != 0
 * adds the read value to the running total instead of replacing it.
 */
void
PmcReadData(int cumulation)
{
	int			i;

	for (i = 0; i < _PmcData[0].num; i++)
	{
		struct pmc_data *p = &_PmcData[i];
		uint64_t	te;
		uint64_t	tr;
		uint64_t	v;

		if (!pmc_read_event(p->fd, &te, &tr, &v))
			continue;

		p->value = (unsigned long) v;
		p->time_enabled = (unsigned long) te;
		p->time_running = (unsigned long) tr;
		p->scale = (tr > 0) ? ((double) te / (double) tr) : 1.0;
		p->scale_value = (double) p->value * p->scale;

		if (cumulation)
			p->valx += p->value;
	}
}

void
PmcPrint(void)
{
	int			i;

	for (i = 0; i < _PmcData[0].num; i++)
	{
		struct pmc_data *p = &_PmcData[i];
		double		pct;

		if (p->fd < 0)
			continue;

		pct = (p->time_enabled > 0) ?
			(100.0 * (double) p->time_running / (double) p->time_enabled) : 100.0;

		elog(INFO, "%40s : %.2f (%.2f%%) %lu",
			 p->name, p->scale_value, pct, p->value);
	}
}

void
PmcEnable(void)
{
	prctl(PR_TASK_PERF_EVENTS_ENABLE);
}

void
PmcDisable(void)
{
	prctl(PR_TASK_PERF_EVENTS_DISABLE);
}

/*
 * Low-overhead in-loop sampling: snapshot at the start of the measured code,
 * snapshot again at the end and accumulate the delta.  Uses rdpmc via the
 * mmap'ed event page, so the loop body pays no syscall.
 */
void
PmcSampleBegin(void)
{
	int			i;

	for (i = 0; i < _PmcData[0].num; i++)
	{
		struct pmc_data *p = &_PmcData[i];

		if (p->fd < 0)
			continue;

		if (p->mmap != NULL && p->mmap->cap_user_rdpmc)
			p->valx = pmc_read_mmap(p->mmap);
		else
			p->valx = pmc_read_fd(p->fd);
	}
}

void
PmcSampleEnd(void)
{
	int			i;

	for (i = 0; i < _PmcData[0].num; i++)
	{
		struct pmc_data *p = &_PmcData[i];
		uint64_t	cur;

		if (p->fd < 0)
			continue;

		if (p->mmap != NULL && p->mmap->cap_user_rdpmc)
			cur = pmc_read_mmap(p->mmap);
		else
			cur = pmc_read_fd(p->fd);

		p->value2 += (cur >= p->valx) ? (cur - p->valx) :
			((uint64_t) -1 - p->valx + cur + 1);
	}
}

void
PmcPrintSample(void)
{
	int			i;

	for (i = 0; i < _PmcData[0].num; i++)
	{
		struct pmc_data *p = &_PmcData[i];

		if (p->fd < 0)
			continue;

		elog(INFO, "%40s : %.2f (loop sample)",
			 p->name, (double) p->value2 * p->scale);
	}
}
