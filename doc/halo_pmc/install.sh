#!/bin/bash
# Install HaloPMC instrumentation into a PostgreSQL/openHalo source tree.
#
#   ./install.sh /opt/src/postgresql-16.16
#
# Copies halo_pmc.{c,h} into the tree, wires up the halo_pmc_list GUC, and
# adds the instrumentation calls to MultiExecPrivateHash() in nodeHash.c.
# Re-running is safe: each edit is guarded by a marker check.
set -e

SRC=${1:-/opt/src/postgresql-16.16}
HERE=$(cd "$(dirname "$0")" && pwd)

[ -f "$SRC/src/backend/executor/nodeHash.c" ] || {
	echo "not a PostgreSQL source tree: $SRC" >&2
	exit 1
}

cp "$HERE/halo_pmc.h" "$SRC/src/include/utils/halo_pmc.h"
cp "$HERE/halo_pmc.c" "$SRC/src/backend/utils/misc/halo_pmc.c"

python3 - "$SRC" <<'EOF'
import sys

SRC = sys.argv[1]

p = SRC + '/src/backend/utils/misc/Makefile'
s = open(p).read()
if 'halo_pmc.o' not in s:
    a = '\tguc_tables.o \\\n'
    assert a in s
    s = s.replace(a, a + '\thalo_pmc.o \\\n', 1)
    open(p, 'w').write(s)

p = SRC + '/src/backend/utils/misc/guc_tables.c'
s = open(p).read()
if 'halo_pmc_list' not in s:
    a = 'extern char *temp_tablespaces;\n'
    assert a in s
    s = s.replace(a, a + '\nextern char *halo_pmc_list;\n', 1)
    a = 'struct config_string ConfigureNamesString[] =\n{\n'
    assert a in s
    entry = '''{
        {"halo_pmc_list", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("Sets the CPU PMC counter."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &halo_pmc_list,
        "",
        NULL, NULL, NULL
    },

'''
    s = s.replace(a, a + entry, 1)
    open(p, 'w').write(s)

p = SRC + '/src/backend/executor/nodeHash.c'
s = open(p).read()
if 'halo_pmc.h' not in s:
    a = '#include "utils/guc.h"\n'
    assert a in s
    s = s.replace(a, a + '#include "utils/halo_pmc.h"\n', 1)

    a = '''\thashkeys = node->hashkeys;
\tecontext = node->ps.ps_ExprContext;

\t/*
\t * Get all tuples from the node below the Hash node and insert into the
\t * hash table (or temp files).
\t */
\tfor (;;)
\t{
\t\tslot = ExecProcNode(outerNode);
'''
    assert a in s
    b = '''\thashkeys = node->hashkeys;
\tecontext = node->ps.ps_ExprContext;

\t/* Halo PMC 采样开始 */
\t(void) PmcOpen(1);
\tPmcEnable();

\t/*
\t * Get all tuples from the node below the Hash node and insert into the
\t * hash table (or temp files).
\t */
\tfor (;;)
\t{
\t\tPmcSampleBegin();
\t\tslot = ExecProcNode(outerNode);
'''
    s = s.replace(a, b, 1)

    a = '''\t\t\thashtable->totalTuples += 1;
\t\t}
\t}

\t/* resize the hash table if needed (NTUP_PER_BUCKET exceeded) */
'''
    assert a in s
    b = '''\t\t\thashtable->totalTuples += 1;
\t\t}
\t\tPmcSampleEnd();
\t}

\t/* Halo PMC 采样结束 */
\tPmcDisable();
\tPmcReadData(0);
\tPmcPrint();
\tPmcPrintSample();
\tPmcClose(_PmcData);

\t/* resize the hash table if needed (NTUP_PER_BUCKET exceeded) */
'''
    s = s.replace(a, b, 1)
    open(p, 'w').write(s)

print('patched ok')
EOF

cd "$SRC"
make -j"$(nproc)"
make install
echo "HaloPMC installed into $SRC"
