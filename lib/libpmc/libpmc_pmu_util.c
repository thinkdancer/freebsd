/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2018, Matthew Macy
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 *
 */

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <pmc.h>
#include <pmclog.h>
#include <libpmcstat.h>
#include "pmu-events/pmu-events.h"

#if defined(__amd64__) || defined(__i386__)
struct pmu_alias {
	const char *pa_alias;
	const char *pa_name;
};
static struct pmu_alias pmu_alias_table[] = {
	{"UNHALTED_CORE_CYCLES", "CPU_CLK_UNHALTED.THREAD_P_ANY"},
	{"UNHALTED-CORE-CYCLES", "CPU_CLK_UNHALTED.THREAD_P_ANY"},
	{"LLC_MISSES", "LONGEST_LAT_CACHE.MISS"},
	{"LLC-MISSES", "LONGEST_LAT_CACHE.MISS"},
	{"LLC_REFERENCE", "LONGEST_LAT_CACHE.REFERENCE"},
	{"LLC-REFERENCE", "LONGEST_LAT_CACHE.REFERENCE"},
	{"LLC_MISS_RHITM", "mem_load_l3_miss_retired.remote_hitm"},
	{"LLC-MISS-RHITM", "mem_load_l3_miss_retired.remote_hitm"},
	{"RESOURCE_STALL", "RESOURCE_STALLS.ANY"},
	{"RESOURCE_STALLS_ANY", "RESOURCE_STALLS.ANY"},
	{"BRANCH_INSTRUCTION_RETIRED", "BR_INST_RETIRED.ALL_BRANCHES"},
	{"BRANCH-INSTRUCTION-RETIRED", "BR_INST_RETIRED.ALL_BRANCHES"},
	{"BRANCH_MISSES_RETIRED", "BR_MISP_RETIRED.ALL_BRANCHES"},
	{"BRANCH-MISSES-RETIRED", "BR_MISP_RETIRED.ALL_BRANCHES"},
	{"cycles", "tsc-tsc"},
	{"instructions", "inst-retired.any_p"},
	{"branch-mispredicts", "br_misp_retired.all_branches" },
	{"branches", "br_inst_retired.all_branches" },
	{"interrupts", "hw_interrupts.received"},
	{"ic-misses", "frontend_retired.l1i_miss"},
	{NULL, NULL},
};

static const char *fixed_mode_cntrs[] = {
	"inst_retired.any",
	"cpu_clk_unhalted.thread",
	"cpu_clk_unhalted.thread_any",
	"cpu_clk_unhalted.ref_tsc",
	NULL
};

static const char *
pmu_alias_get(const char *name)
{
	struct pmu_alias *pa;

	for (pa = pmu_alias_table; pa->pa_alias != NULL; pa++)
		if (strcasecmp(name, pa->pa_alias) == 0)
			return (pa->pa_name);
	return (name);
}

struct pmu_event_desc {
	uint64_t ped_period;
	uint64_t ped_offcore_rsp;
	uint32_t ped_event;
	uint32_t ped_frontend;
	uint32_t ped_ldlat;
	uint32_t ped_config1;
	uint8_t	ped_umask;
	uint8_t	ped_cmask;
	uint8_t	ped_any;
	uint8_t	ped_inv;
	uint8_t	ped_edge;
	uint8_t	ped_fc_mask;
	uint8_t	ped_ch_mask;
};

static const struct pmu_events_map *
pmu_events_map_get(void)
{
	size_t s;
	char buf[64];
	const struct pmu_events_map *pme;

	if (sysctlbyname("kern.hwpmc.cpuid", (void *)NULL, &s,
	    (void *)NULL, 0) == -1)
		return (NULL);
	if (sysctlbyname("kern.hwpmc.cpuid", buf, &s,
	    (void *)NULL, 0) == -1)
		return (NULL);
	for (pme = pmu_events_map; pme->cpuid != NULL; pme++)
		if (strcmp(buf, pme->cpuid) == 0)
			return (pme);
	return (NULL);
}

static const struct pmu_event *
pmu_event_get(const char *event_name, int *idx)
{
	const struct pmu_events_map *pme;
	const struct pmu_event *pe;
	int i;

	if ((pme = pmu_events_map_get()) == NULL)
		return (NULL);
	for (i = 0, pe = pme->table; pe->name || pe->desc || pe->event; pe++, i++) {
		if (pe->name == NULL)
			continue;
		if (strcasecmp(pe->name, event_name) == 0) {
			if (idx)
				*idx = i;
			return (pe);
		}
	}
	return (NULL);
}

const char *
pmc_pmu_event_get_by_idx(int idx)
{
	const struct pmu_events_map *pme;
	const struct pmu_event *pe;
	int i;

	if ((pme = pmu_events_map_get()) == NULL)
		return (NULL);
	for (i = 0, pe = pme->table; (pe->name || pe->desc || pe->event) && i < idx; pe++, i++);
	return (pe->name);
}

static int
pmu_parse_event(struct pmu_event_desc *ped, const char *eventin)
{
	char *event;
	char *kvp, *key, *value, *r;
	char *debug;

	if ((event = strdup(eventin)) == NULL)
		return (ENOMEM);
	r = event;
	bzero(ped, sizeof(*ped));
	while ((kvp = strsep(&event, ",")) != NULL) {
		key = strsep(&kvp, "=");
		if (key == NULL)
			abort();
		value = kvp;
		if (strcmp(key, "umask") == 0)
			ped->ped_umask = strtol(value, NULL, 16);
		else if (strcmp(key, "event") == 0)
			ped->ped_event = strtol(value, NULL, 16);
		else if (strcmp(key, "period") == 0)
			ped->ped_period = strtol(value, NULL, 10);
		else if (strcmp(key, "offcore_rsp") == 0)
			ped->ped_offcore_rsp = strtol(value, NULL, 16);
		else if (strcmp(key, "any") == 0)
			ped->ped_any = strtol(value, NULL, 10);
		else if (strcmp(key, "cmask") == 0)
			ped->ped_cmask = strtol(value, NULL, 10);
		else if (strcmp(key, "inv") == 0)
			ped->ped_inv = strtol(value, NULL, 10);
		else if (strcmp(key, "edge") == 0)
			ped->ped_edge = strtol(value, NULL, 10);
		else if (strcmp(key, "frontend") == 0)
			ped->ped_frontend = strtol(value, NULL, 16);
		else if (strcmp(key, "ldlat") == 0)
			ped->ped_ldlat = strtol(value, NULL, 16);
		else if (strcmp(key, "fc_mask") == 0)
			ped->ped_fc_mask = strtol(value, NULL, 16);
		else if (strcmp(key, "ch_mask") == 0)
			ped->ped_ch_mask = strtol(value, NULL, 16);
		else if (strcmp(key, "config1") == 0)
			ped->ped_config1 = strtol(value, NULL, 16);
		else {
			debug = getenv("PMUDEBUG");
			if (debug != NULL && strcmp(debug, "true") == 0 && value != NULL)
				printf("unrecognized kvpair: %s:%s\n", key, value);
		}
	}
	free(r);
	return (0);
}

uint64_t
pmc_pmu_sample_rate_get(const char *event_name)
{
	const struct pmu_event *pe;
	struct pmu_event_desc ped;

	event_name = pmu_alias_get(event_name);
	if ((pe = pmu_event_get(event_name, NULL)) == NULL)
		return (DEFAULT_SAMPLE_COUNT);
	if (pe->alias && (pe = pmu_event_get(pe->alias, NULL)) == NULL)
		return (DEFAULT_SAMPLE_COUNT);
	if (pe->event == NULL)
		return (DEFAULT_SAMPLE_COUNT);
	if (pmu_parse_event(&ped, pe->event))
		return (DEFAULT_SAMPLE_COUNT);
	return (ped.ped_period);
}

int
pmc_pmu_enabled(void)
{

	return (pmu_events_map_get() != NULL);
}

void
pmc_pmu_print_counters(const char *event_name)
{
	const struct pmu_events_map *pme;
	const struct pmu_event *pe;
	struct pmu_event_desc ped;
	char *debug;
	int do_debug;

	debug = getenv("PMUDEBUG");
	do_debug = 0;

	if (debug != NULL && strcmp(debug, "true") == 0)
		do_debug = 1;
	if ((pme = pmu_events_map_get()) == NULL)
		return;
	for (pe = pme->table; pe->name || pe->desc || pe->event; pe++) {
		if (pe->name == NULL)
			continue;
		if (event_name != NULL && strcasestr(pe->name, event_name) == NULL)
			continue;
		printf("\t%s\n", pe->name);
		if (do_debug)
			pmu_parse_event(&ped, pe->event);
	}
}

void
pmc_pmu_print_counter_desc(const char *ev)
{
	const struct pmu_events_map *pme;
	const struct pmu_event *pe;

	if ((pme = pmu_events_map_get()) == NULL)
		return;
	for (pe = pme->table; pe->name || pe->desc || pe->event; pe++) {
		if (pe->name == NULL)
			continue;
		if (strcasestr(pe->name, ev) != NULL &&
		    pe->desc != NULL)
			printf("%s:\t%s\n", pe->name, pe->desc);
	}
}

void
pmc_pmu_print_counter_desc_long(const char *ev)
{
	const struct pmu_events_map *pme;
	const struct pmu_event *pe;

	if ((pme = pmu_events_map_get()) == NULL)
		return;
	for (pe = pme->table; pe->name || pe->desc || pe->event; pe++) {
		if (pe->name == NULL)
			continue;
		if (strcasestr(pe->name, ev) != NULL) {
			if (pe->long_desc != NULL)
				printf("%s:\n%s\n", pe->name, pe->long_desc);
			else if (pe->desc != NULL)
				printf("%s:\t%s\n", pe->name, pe->desc);
		}
	}
}

void
pmc_pmu_print_counter_full(const char *ev)
{
	const struct pmu_events_map *pme;
	const struct pmu_event *pe;

	if ((pme = pmu_events_map_get()) == NULL)
		return;
	for (pe = pme->table; pe->name || pe->desc || pe->event; pe++) {
		if (pe->name == NULL)
			continue;
		if (strcasestr(pe->name, ev) == NULL)
			continue;
		printf("name: %s\n", pe->name);
		if (pe->long_desc != NULL)
			printf("desc: %s\n", pe->long_desc);
		else if (pe->desc != NULL)
			printf("desc: %s\n", pe->desc);
		if (pe->event != NULL)
			printf("event: %s\n", pe->event);
		if (pe->topic != NULL)
			printf("topic: %s\n", pe->topic);
		if (pe->pmu != NULL)
			printf("pmu: %s\n", pe->pmu);
		if (pe->unit != NULL)
			printf("unit: %s\n", pe->unit);
		if (pe->perpkg != NULL)
			printf("perpkg: %s\n", pe->perpkg);
		if (pe->metric_expr != NULL)
			printf("metric_expr: %s\n", pe->metric_expr);
		if (pe->metric_name != NULL)
			printf("metric_name: %s\n", pe->metric_name);
		if (pe->metric_group != NULL)
			printf("metric_group: %s\n", pe->metric_group);
	}
}

int
pmc_pmu_pmcallocate(const char *event_name, struct pmc_op_pmcallocate *pm)
{
	const struct pmu_event *pe;
	struct pmu_event_desc ped;
	struct pmc_md_iap_op_pmcallocate *iap;
	struct pmc_md_iaf_op_pmcallocate *iaf;
	int idx, isfixed;

	iap = &pm->pm_md.pm_iap;
	isfixed = 0;
	bzero(iap, sizeof(*iap));
	event_name = pmu_alias_get(event_name);
	pm->pm_caps |= (PMC_CAP_READ | PMC_CAP_WRITE);
	if ((pe = pmu_event_get(event_name, &idx)) == NULL)
		return (ENOENT);
	if (pe->alias && (pe = pmu_event_get(pe->alias, &idx)) == NULL)
		return (ENOENT);
	if (pe->event == NULL)
		return (ENOENT);
	if (pmu_parse_event(&ped, pe->event))
		return (ENOENT);

	for (idx = 0; fixed_mode_cntrs[idx] != NULL; idx++)
		if (strcmp(fixed_mode_cntrs[idx], event_name) == 0)
			isfixed = 1;
	if (isfixed) {
		iaf = &pm->pm_md.pm_iaf;
		pm->pm_class = PMC_CLASS_IAF;
		if (strcasestr(pe->desc, "retired") != NULL)
			pm->pm_ev = PMC_EV_IAF_INSTR_RETIRED_ANY;
		else if (strcasestr(pe->desc, "core") != NULL ||
		    strcasestr(pe->desc, "unhalted"))
			pm->pm_ev = PMC_EV_IAF_CPU_CLK_UNHALTED_CORE;
		else if (strcasestr(pe->desc, "ref") != NULL)
			pm->pm_ev = PMC_EV_IAF_CPU_CLK_UNHALTED_REF;
		iaf->pm_iaf_flags |= (IAF_USR | IAF_OS);
		if (ped.ped_any)
			iaf->pm_iaf_flags |= IAF_ANY;
		if (pm->pm_caps & PMC_CAP_INTERRUPT)
			iaf->pm_iaf_flags |= IAF_PMI;
		return (0);
	} else if (strcasestr(event_name, "UNC_") == event_name ||
			   strcasestr(event_name, "uncore") != NULL) {
		pm->pm_class = PMC_CLASS_UCP;
	} else {
		pm->pm_caps |= PMC_CAP_QUALIFIER;
		pm->pm_class = PMC_CLASS_IAP;
	}
	pm->pm_ev = idx;
	iap->pm_iap_config |= IAP_EVSEL(ped.ped_event);
	iap->pm_iap_config |= IAP_UMASK(ped.ped_umask);
	iap->pm_iap_config |= IAP_CMASK(ped.ped_cmask);
	iap->pm_iap_rsp = ped.ped_offcore_rsp;

	iap->pm_iap_config |= (IAP_USR | IAP_OS);
	if (ped.ped_edge)
		iap->pm_iap_config |= IAP_EDGE;
	if (ped.ped_any)
		iap->pm_iap_config |= IAP_ANY;
	if (ped.ped_inv)
		iap->pm_iap_config |= IAP_EDGE;
	if (pm->pm_caps & PMC_CAP_INTERRUPT)
		iap->pm_iap_config |= IAP_INT;
	return (0);
}

/*
 * Ultimately rely on AMD calling theirs the same
 */
static const char *stat_mode_cntrs[] = {
	"cpu_clk_unhalted.thread_any",
	"inst_retired.any",
	"br_inst_retired.all_branches",
	"br_misp_retired.all_branches",
	"longest_lat_cache.reference",
	"longest_lat_cache.miss",
};

int
pmc_pmu_stat_mode(const char ***cntrs)
{
	if (pmc_pmu_enabled()) {
		*cntrs = stat_mode_cntrs;
		return (0);
	}
	return (EOPNOTSUPP);
}

#else

uint64_t
pmc_pmu_sample_rate_get(const char *event_name __unused)
{
	return (DEFAULT_SAMPLE_COUNT);
}

void
pmc_pmu_print_counters(const char *event_name __unused)
{
}

void
pmc_pmu_print_counter_desc(const char *e __unused)
{
}

void
pmc_pmu_print_counter_desc_long(const char *e __unused)
{
}

void
pmc_pmu_print_counter_full(const char *e __unused)
{

}

int
pmc_pmu_enabled(void)
{
	return (0);
}

int
pmc_pmu_pmcallocate(const char *e __unused, struct pmc_op_pmcallocate *p __unused)
{
	return (EOPNOTSUPP);
}

const char *
pmc_pmu_event_get_by_idx(int idx __unused)
{
	return (NULL);
}
int
pmc_pmu_stat_mode(const char ***a __unused)
{
	return (EOPNOTSUPP);
}

#endif
