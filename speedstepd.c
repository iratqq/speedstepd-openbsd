/*
 * Copyright (c) 2003 Iwata <iratqq@gmail.com>
 * All rights reserved.
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
 */

/*
  ref.
  a-gota@bokutou.jp, FreeBSD PRESS, 19, 207(2003)
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/dkstat.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <machine/apmvar.h>
#include "pathnames.h"
#include "apm-proto.h"

extern char *__progname;

#define DPRINTF(__args__) { if (verbose_f) printf __args__; }

#define	DEFAULT_INTERVAL 2
#define	MIN_INTERVAL     1

#define	MAX_NUM_OF_DATA     10
#define	DEFAULT_NUM_OF_DATA 3
#define	MIN_NUM_OF_DATA	    1

#define	MAX_LOAD_LEVEL   100
#define	DEFAULT_VH_LEVEL 80
#define	DEFAULT_H_LEVEL  66
#define	DEFAULT_L_LEVEL  33
#define	DEFAULT_VL_LEVEL 15
#define	MIN_LOAD_LEVEL   0

#define	DEFAULT_LARGE_STEP	  2
#define	MIN_LARGE_STEP		  1

#define	MIN_SPEED        0
#define	MAX_SPEED        100

#define MIN_CPUSTEP      1
#define MAX_CPUSTEP      100
#define DEFAULT_CPUSTEP  5

int interval = 2;
static long load[MAX_NUM_OF_DATA], sum;
static int curr_pos, data_cnt;
static int num_of_data;
static int ignore_ac_check = 0;

int verbose_f = 0;

static void
usage(void)
{
        fprintf(stderr, "usage: %s -hHilLsv\n",
		__progname);
        exit(1);
}

static int
param_check(int value, int min, int max, int default_value,
	    const char *name)
{
	int ret = value;

	if (value < min) {
		warnx("%s (%d) must be `=> %d'; ignored", name, value, min);
		ret = default_value;
	} else if (max < value) {
		warnx("%s (%d) must be `<= %d'; ignored", name, value, max);
		ret = default_value;
	}

	return ret;
}

static int
get_cpu_load(void)
{
	static long cp_time[CPUSTATES];
	static int mib[2] = { CTL_KERN, KERN_CPTIME };
	long idle_diff, total, total_diff, load;
	static long old_total = 0, old_idle = 0;
	size_t len = sizeof(cp_time);

	if (sysctl(mib, 2, (void*)cp_time, &len, NULL, 0) == -1)
		err(1, "get_cpu_load");
	idle_diff = cp_time[CP_IDLE] - old_idle;
	total = cp_time[CP_USER] + cp_time[CP_NICE] + cp_time[CP_SYS] +
		cp_time[CP_INTR] + cp_time[CP_IDLE];
	total_diff = total - old_total;
	load = (total_diff - idle_diff) * 100 / total_diff;
	old_total = total;
	old_idle = cp_time[CP_IDLE];
	return load;
}

static int
get_cpuspeed(void)
{
        int mib[2] = { CTL_HW, HW_CPUSPEED };
	int hw_cpuspeed;
        size_t len = sizeof(int);

        if (sysctl(mib, 2, &hw_cpuspeed, &len, NULL, 0) == -1)
		err(1, "get_cpuspeed");
	return hw_cpuspeed;
}

static int
get_setperf(void)
{
	int mib[2] = { CTL_HW, HW_SETPERF };
	int hw_setperf;
	size_t len = sizeof(hw_setperf);

	if (sysctl(mib, 2, &hw_setperf, &len, NULL, 0) == -1)
		err(1, "get_setperf");
	return hw_setperf;
}

static void
set_setperf(int perf)
{
	int mib[2] = { CTL_HW, HW_SETPERF };
	int hw_setperf = perf;
	size_t len = sizeof(hw_setperf);

	if (hw_setperf < 0)
		hw_setperf = 0;
	if (100 < hw_setperf)
		hw_setperf = 100;
	if (sysctl(mib, 2, NULL, 0, &hw_setperf, len) == -1)
		err(1, "set_setperf");
}

static int
get_acstate(void)
{
	struct apm_power_info bstate;
	int fd;

	if ((fd = open(_PATH_APM_NORMAL, O_RDONLY)) == -1) {
		syslog(LOG_ERR, "cannot open apm device: %m");
		return 0;
	}
	if (ioctl(fd, APM_IOC_GETPOWER, &bstate) == 0) {
		close(fd);
		if (bstate.ac_state == APM_AC_ON)
			return 1;
		else
			return 0;
	}
	close(fd);
	syslog(LOG_ERR, "cannot fetch power status: %m");
	return 0;

}

static void
init_loads(void)
{
	int cnt;

	if (10 < num_of_data)
		err(1, "init_loads");
	for (cnt = 0; cnt < num_of_data; cnt++)
		load[cnt] = 0;
	curr_pos = 0;
	data_cnt = 0;
	sum = 0;
}


int
main(int argc, char *argv[])
{
	int ch;
	int cpu_speed, prev_cpuspeed = -1;
	long avg;
	int very_low_level, low_level, high_level, very_high_level, large_step;
	int max_speed, cpu_step;

	interval = DEFAULT_INTERVAL;
	num_of_data = DEFAULT_NUM_OF_DATA;
	very_high_level = DEFAULT_VH_LEVEL;
	high_level = DEFAULT_H_LEVEL;
	low_level = DEFAULT_L_LEVEL;
	very_low_level = DEFAULT_VL_LEVEL;
	large_step = DEFAULT_LARGE_STEP;
	max_speed = 100;
	verbose_f = 0;
	cpu_step = DEFAULT_CPUSTEP;

	while ((ch = getopt(argc, argv, "Ah:H:i:l:m:L:s:v")) != -1) {
		switch (ch) {
		case 'A':
			ignore_ac_check = 1;
			break;
		case 'h':
			high_level = param_check(atoi(optarg),
						 MIN_LOAD_LEVEL, MAX_LOAD_LEVEL,
						 high_level,
						 "high level value");
			break;
		case 'H':
			very_high_level = param_check(atoi(optarg),
						      MIN_LOAD_LEVEL, MAX_LOAD_LEVEL,
						      very_high_level,
						      "very high level value");
			break;
		case 'i':
			interval = param_check(atoi(optarg),
					       MIN_INTERVAL, INT_MAX,
					       interval,
					       "interval value");
			break;
		case 'l':
			low_level = param_check(atoi(optarg),
						MIN_LOAD_LEVEL, MAX_LOAD_LEVEL,
						low_level,
						"low level value");
			break;
		case 'n':
			num_of_data = param_check(atoi(optarg),
						  MIN_NUM_OF_DATA, MAX_NUM_OF_DATA,
						  num_of_data,
						  "number of data to calculate average");
			break;
		case 'L':
			very_low_level = param_check(atoi(optarg),
						     MIN_LOAD_LEVEL, MAX_LOAD_LEVEL,
						     very_low_level,
						     "very low level value");
			break;
		case 'm':
			max_speed = param_check(atoi(optarg),
						MIN_SPEED, 100,
						max_speed,
						"maximum speed");
			break;
		case 'S':
			cpu_step = param_check(atoi(optarg),
					       MIN_CPUSTEP, MAX_CPUSTEP,
					       cpu_step,
					       "cpu step value");
			break;
		case 's':
			large_step = param_check(atoi(optarg),
						 MIN_LARGE_STEP, INT_MAX,
						 large_step,
						 "large step value");
			break;
		case 'v':
			verbose_f = 1;
			break;
                default:
                        usage();
		}
	}
	if (!(very_low_level < low_level
	      && low_level < high_level && high_level < very_high_level)) {
		warnx("threshold levels must be the following relation: "
		      "%d <= very_low(%d) < low(%d)"
		      " < high(%d) < very_high(%d) <= %d"
		      "; abort.",
		      MIN_LOAD_LEVEL, very_low_level, low_level,
		      high_level, very_high_level, MAX_LOAD_LEVEL);
	}
	if (!verbose_f) {
		if (daemon(0, 1)) {
			errx(2, "fork failed");
		}
	}
	if (MAX_SPEED <= large_step) {
		errx(1, "large step value(%d) must be"
		     " between %d to max speed(%d) - 1; abort.",
		     large_step, MIN_LARGE_STEP, MAX_SPEED);
	}
	get_cpu_load();
	while(1) {
		sleep(interval);
		if ((cpu_speed = get_setperf()) != prev_cpuspeed)
			init_loads();
		prev_cpuspeed = cpu_speed;
		sum -= load[curr_pos];
		load[curr_pos] = get_cpu_load();
		sum += load[curr_pos];
		if (data_cnt < num_of_data)
			++data_cnt;
		avg = sum / data_cnt;
		if (num_of_data <= ++curr_pos)
			curr_pos = 0;
		DPRINTF(("load %ld (avg by %d)\n", avg, data_cnt));
		if (!ignore_ac_check && get_acstate()) {
			DPRINTF(("AC line is online now\n"));
			set_setperf(MAX_CPUSTEP);
			continue;
		}
		if (data_cnt == num_of_data) {
			if (avg < very_low_level) {
				int curr_speed = cpu_speed - 100 / cpu_step * large_step;
				if (curr_speed < 0)
					curr_speed = 0;
				DPRINTF(("VERY LOW, hw.setperf = %d\n", curr_speed));
				set_setperf(curr_speed);
			}
			else if (avg < low_level) {
				int curr_speed = cpu_speed - 100 / cpu_step;
				if (curr_speed < 0)
					curr_speed = 0;
				DPRINTF(("LOW, hw.setperf = %d\n", curr_speed));
				set_setperf(curr_speed);
			}
			if (very_high_level < avg) {
				int curr_speed = cpu_speed + 100 / cpu_step * large_step;
				if (max_speed < curr_speed)
					curr_speed = max_speed;
				DPRINTF(("VERY HIGH, hw.setperf = %d\n", curr_speed));
				set_setperf(curr_speed);
			}
			else if (high_level < avg) {
				int curr_speed = cpu_speed + 100 / cpu_step;
				if (max_speed < curr_speed)
					curr_speed = max_speed;
				DPRINTF(("HIGH, hw.setperf = %d\n", curr_speed));
				set_setperf(curr_speed);
			}
		}
		DPRINTF(("speed:%dMhz pref:%d\n", get_cpuspeed(), get_setperf()));
	}
	/* NOTREACHED */
	return 0;
}

