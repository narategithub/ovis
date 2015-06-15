/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2012-2015 Open Grid Computing, Inc. All rights reserved.
 * Copyright (c) 2012-2015 Sandia Corporation. All rights reserved.
 * Under the terms of Contract DE-AC04-94AL85000, there is a non-exclusive
 * license for use of this work by or on behalf of the U.S. Government.
 * Export of this program may require a license from the United States
 * Government.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the BSD-type
 * license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *      Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *      Neither the name of Sandia nor the names of any contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 *      Neither the name of Open Grid Computing nor the names of any
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *      Modified source versions must be plainly marked as such, and
 *      must not be misrepresented as being the original software.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <inttypes.h>
#include <unistd.h>
#include <sys/errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>
#include <sys/queue.h>
#include <assert.h>
#include "ldms.h"
#include "ldmsd.h"

#define PROC_FILE "/proc/diskstats"
#define SECTOR_SIZE_FILE_FMT "/sys/block/%s/queue/hw_sector_size"

static char *procfile = PROC_FILE;
#define NRAW_FIELD 11
#define NDERIVED_FIELD 2
#define NFIELD (NRAW_FIELD + NDERIVED_FIELD)
static char *fieldname[NFIELD] = {
	/* raw */
	"reads_comp",
	"reads_merg",
	"sect_read",	/* SECT_READ_IDX points here */
	"time_read",
	"writes_comp",
	"writes_merg",
	"sect_written",	/* SECT_WRITTEN_IDX points here */
	"time_write",
	"ios_in_progress",
	"time_ios",
	"weighted_time",

	/* derived */
	"disk.byte_read",	/* number of sectors read * sector_size */
	"disk.byte_written",	/* number of sectors write * sector_size */
};

#define SECT_READ_IDX 2
#define SECT_WRITTEN_IDX 6
#define SECT_READ_BYTES_IDX 11
#define SECT_WRITTEN_BYTES_IDX 12

static ldms_set_t set;
static FILE *mf = NULL;
static ldmsd_msg_log_f msglog;
static char *producer_name;

static long USER_HZ; /* initialized in get_plugin() */
static struct timeval _tv[2] = {0};
static struct timeval *curr_tv = &_tv[0];
static struct timeval *prev_tv = &_tv[1];

struct proc_disk_s {
	char *name;
	size_t sect_sz;
	int monitored;
	int midx[NFIELD * 2]; /* raw + rate metrics */
	uint64_t prev_value[NFIELD];
	TAILQ_ENTRY(proc_disk_s) entry;
};
TAILQ_HEAD(proc_disk_list, proc_disk_s) disk_list =
	TAILQ_HEAD_INITIALIZER(disk_list);

static int add_disk_metrics(ldms_schema_t schema, struct proc_disk_s *disk)
{
	char metric_name[128];
	int i, rc;
	for (i = 0; i < NFIELD; i++) {
		/* raw metric */
		snprintf(metric_name, 128, "%s#%s", fieldname[i], disk->name);
		rc = ldms_schema_metric_add(schema, metric_name, LDMS_V_U64);
		if (rc < 0)
			return ENOMEM;
		disk->midx[2 * i] = rc;

		/* rate metric */
		snprintf(metric_name, 128, "%s.rate#%s", fieldname[i], disk->name);
		rc = ldms_schema_metric_add(schema, metric_name, LDMS_V_F32);
		if (rc < 0)
			return ENOMEM;
		disk->midx[2 * i + 1] = rc;
	}
	return 0;
}

#define DEFAULT_SECTOR_SZ 512
static int get_sector_sz(char *device)
{
	int rc = 0;
	int result;
	FILE *f = NULL;
	char filename[FILENAME_MAX];
	sprintf(filename, SECTOR_SIZE_FILE_FMT, device);

	f = fopen(filename, "r");
	if (!f) {
		msglog(LDMSD_LERROR, "Failed to open %s\n", filename);
		return DEFAULT_SECTOR_SZ;
	}

	fseek(f, 0, SEEK_SET);
	char *s;
	do {
		s = fgets(filename, sizeof(filename), f);
		if (!s)
			break;
		rc = sscanf(filename, "%d", &result);

		if (rc != 1) {
			msglog(LDMSD_LINFO, "Failed to get the sector size of %s. "
					"The size is set to 512.\n", device);
			result = DEFAULT_SECTOR_SZ;
		}
	} while (s);
	fclose(f);
	return result;
}

static int scan_line(char *lbuf, char *name, uint64_t *v)
{
	int rc;
	int junk1, junk2;
	rc = sscanf(lbuf, "%d %d %s %" PRIu64 " %" PRIu64
		    " %" PRIu64 " %" PRIu64 " %" PRIu64 " %"
		    PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
		    " %" PRIu64 " %" PRIu64 "\n", &junk1, &junk2, name,
		    &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
		    &v[8], &v[9], &v[10]);
	return (rc == 14);
}

static struct proc_disk_s *add_disk(char *name)
{
	struct proc_disk_s *disk = calloc(1, sizeof *disk);
	if (!disk)
		goto out;
	disk->name = strdup(name);
	disk->sect_sz = get_sector_sz(disk->name);
	TAILQ_INSERT_TAIL(&disk_list, disk, entry);
 out:
	return disk;
}

/*
 * Parse the /proc/diskstats file and collect all the devices names
 */
static int get_disks()
{
	uint64_t v[NFIELD];
	int rc;
	char *s;
	char lbuf[256];
	char name[64];
	struct proc_disk_s *disk;
	FILE *pf;

	pf = fopen(procfile, "r");
	if (!pf)
		return ENOENT;

	fseek(pf, 0, SEEK_SET);
	do {
		s = fgets(lbuf, sizeof(lbuf), pf);
		if (!s)
			break;
		rc = scan_line(s, name, v);
		if (!rc)
			break;
		disk = add_disk(name);
	} while (1);

	fclose(pf);
	return 0;
}

static int config_add_disks(struct attr_value_list *avl, ldms_schema_t schema)
{
	int rc = 0;
	int next_comp_id = 0;
	struct proc_disk_s *disk;
	char *value = av_value(avl, "device");

	rc = get_disks();
	if (rc)
		goto err;

	if (value) {
		/* Mark selected disks as monitored */
		char *value_tmp = strdup(value);
		if (!value_tmp) {
			rc = ENOMEM;
			goto err;
		}
		char *ptr, *name;
		for (name = strtok_r(value_tmp, ",", &ptr);
		     name; name = strtok_r(NULL, ",", &ptr)) {
			TAILQ_FOREACH(disk, &disk_list, entry) {
				if (0 == strcmp(name, disk->name))
					disk->monitored = 1;
			}
		}
		free(value_tmp);
	} else {
		/* Mark all the disks as monitored */
		TAILQ_FOREACH(disk, &disk_list, entry)
			disk->monitored = 1;
	}

	/* Add metrics for monitored disks */
	TAILQ_FOREACH(disk, &disk_list, entry) {
		if (!disk->monitored)
			continue;
		rc = add_disk_metrics(schema, disk);
		if (rc)
			goto err;
	}
	return rc;

err:
	msglog(LDMSD_LERROR, "%s Error %d adding metrics.\n", __FILE__, rc);
	return rc;
}

static int config(struct attr_value_list *kwl, struct attr_value_list *avl)
{
	char *value;
	char *attr;
	int rc = 0;
	ldms_schema_t schema = ldms_schema_new("procdiskstats");
	if (!schema)
		return ENOMEM;

	attr = "producer";
	producer_name = av_value(avl, attr);
	if (!producer_name)
		goto enoent;

	rc = config_add_disks(avl, schema);
	if (rc)
		return rc;

	attr = "instance";
	value = av_value(avl, attr);
	if (!value)
		goto enoent;

	if (set) {
		msg(LDMSD_LERROR, "procdiskstat: Set already created.\n");
		return EINVAL;
	}
	set = ldms_set_new(value, schema);
	if (!set) {
		rc = errno;
		msglog(LDMSD_LERROR, "procdiskstats: failed to create "
				"the metric set.\n");
		return rc;
	}
	ldms_set_producer_name_set(set, producer_name);
	ldms_schema_delete(schema);
	return 0;
enoent:
	msglog(LDMSD_LERROR, "procdiskstat: requires '%s'\n", attr);
	ldms_schema_delete(schema);
	return ENOENT;
}

static float calculate_rate(uint64_t prev_value, uint64_t curr_v, float dt)
{
	if ((prev_value == 0) || (prev_value > curr_v))
		return 0.0;

	return ((float)(curr_v - prev_value) / USER_HZ) / dt * 100.0;
}

static void set_disk_metrics(struct proc_disk_s *disk,
			     uint64_t *values, float dt)
{
	float f, rate;
	int i, idx;
	for (i = 0; i < NFIELD; i++) {
		idx = 2 * i;
		if (i == SECT_READ_BYTES_IDX) {
			/* Calculate sect_read in bytes */
			/* sect_read's been updated already. */
			f = values[SECT_READ_IDX] * disk->sect_sz;
			ldms_metric_set_float(set, disk->midx[idx], f);

			rate = calculate_rate(disk->prev_value[i], f, dt);
			ldms_metric_set_float(set, disk->midx[idx + 1], rate);

			disk->prev_value[SECT_READ_BYTES_IDX] = (uint64_t)f;
		} else if (i == SECT_WRITTEN_BYTES_IDX) {
			/* Calculate sect_written in bytes */
			/* sect_written's been updated already */
			f = values[SECT_WRITTEN_IDX] * disk->sect_sz;
			ldms_metric_set_float(set, disk->midx[idx], f);

			rate = calculate_rate(disk->prev_value[i], f, dt);
			ldms_metric_set_float(set, disk->midx[idx + 1], rate);

			disk->prev_value[SECT_WRITTEN_BYTES_IDX] = (uint64_t)f;
		} else {
			/* raw */
			ldms_metric_set_u64(set, disk->midx[idx], values[i]);

			/* rate */
			rate = calculate_rate(disk->prev_value[i], values[i], dt);
			ldms_metric_set_float(set, disk->midx[idx + 1], rate);

			disk->prev_value[i] = values[i];
		}
	}
}

static int sample(void)
{
	int rc = 0;
	char *s;
	char name[64];
	char lbuf[256];
	uint64_t v[NFIELD];
	struct timeval diff_tv;
	struct timeval *tmp_tv;
	float dt;
	struct proc_disk_s *disk;

	if (!set) {
		msglog(LDMSD_LDEBUG, "diskstats: plugin not initialized\n");
		return EINVAL;
	}

	if (!mf)
		mf = fopen(procfile, "r");
	if (!mf)
		return ENOENT;
	ldms_transaction_end(set);
	gettimeofday(curr_tv, NULL);
	timersub(curr_tv, prev_tv, &diff_tv);
	dt = diff_tv.tv_sec + diff_tv.tv_usec / 1e06;

	fseek(mf, 0, SEEK_SET);
	disk = TAILQ_FIRST(&disk_list);
	assert(disk);
	do {
		s = fgets(lbuf, sizeof(lbuf), mf);
		if (!s)
			break;
		if (!disk->monitored) {
			disk = TAILQ_NEXT(disk, entry);
			continue;
		}

		rc = scan_line(s, name, v);
		if (!rc) {
			rc = EINVAL;
			goto out;
		}
		set_disk_metrics(disk, v, dt);
		disk = TAILQ_NEXT(disk, entry);
	} while (disk);
out:
	tmp_tv = curr_tv;
	curr_tv = prev_tv;
	prev_tv = tmp_tv;
	ldms_transaction_end(set);
	return rc;
}

static ldms_set_t get_set()
{
	return set;
}

static void term(void)
{
	if (mf)
		fclose(mf);
	mf = 0;

	if (set)
		ldms_set_delete(set);
	set = NULL;
	while (!TAILQ_EMPTY(&disk_list)) {
		struct proc_disk_s *disk = TAILQ_FIRST(&disk_list);
		TAILQ_REMOVE(&disk_list, disk, entry);
		free(disk);
	}
}

static const char *usage(void)
{
        return  "config name=procdiskstats producer=<prod_name> instance=<inst_name> device=<devices>\n"
                "    <prod_name>     The producer name value\n"
                "    <inst_name>     The instance name\n"
                "    <devices>       A comma-separated list of devices\n";
}

static struct ldmsd_sampler procdiskstats_plugin = {
        .base = {
                .name = "procdiskstats",
                .term = term,
                .config = config,
                .usage = usage,
        },
        .get_set = get_set,
        .sample = sample,
};

struct ldmsd_plugin *get_plugin(ldmsd_msg_log_f pf)
{
	msglog = pf;
	set = NULL;
	USER_HZ = sysconf(_SC_CLK_TCK);
	return &procdiskstats_plugin.base;
}
