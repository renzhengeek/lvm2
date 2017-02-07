/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <sys/ioctl.h>
#define _LINUX_AUDIT_H_
#include <sys/mount.h>
#undef MAJOR
#undef MINOR
#undef MKDEV

#include "lib.h"
#include "dev-cache.h"
#include "filter.h"
#include "lvm-string.h"
#include "config.h"
#include "metadata.h"
#include "activate.h"

#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>

#include "device-types.h"

#define NUMBER_OF_MAJORS 4096

#define PARTITION_SCSI_DEVICE (1 << 0)
static struct {
	int max_partitions; /* 0 means LVM won't use this major number. */
	int flags;
} _partitions[NUMBER_OF_MAJORS];

static int _md_major = -1;
static int _blkext_major = -1;
static int _drbd_major = -1;
static int _device_mapper_major = -1;
static int _emcpower_major = -1;

int dm_major(void)
{
	return _device_mapper_major;
}

int md_major(void)
{
	return _md_major;
}

int blkext_major(void)
{
	return _blkext_major;
}

int dev_subsystem_part_major(const struct device *dev)
{
	dev_t primary_dev;

	if (MAJOR(dev->dev) == _md_major)
		return 1;

	if (MAJOR(dev->dev) == _drbd_major)
		return 1;

	if (MAJOR(dev->dev) == _emcpower_major)
		return 1;

	if ((MAJOR(dev->dev) == _blkext_major) &&
	    (get_primary_dev(sysfs_dir_path(), dev, &primary_dev)) &&
	    (MAJOR(primary_dev) == _md_major))
		return 1;

	return 0;
}

const char *dev_subsystem_name(const struct device *dev)
{
	if (MAJOR(dev->dev) == _md_major)
		return "MD";

	if (MAJOR(dev->dev) == _drbd_major)
		return "DRBD";

	if (MAJOR(dev->dev) == _emcpower_major)
		return "EMCPOWER";

	if (MAJOR(dev->dev) == _blkext_major)
		return "BLKEXT";

	return "";
}

static int _passes_lvm_type_device_filter(struct dev_filter *f __attribute__((unused)),
					  struct device *dev)
{
	const char *name = dev_name(dev);
	int ret = 0;
	uint64_t size;

	/* Is this a recognised device type? */
	if (!_partitions[MAJOR(dev->dev)].max_partitions) {
		log_debug("%s: Skipping: Unrecognised LVM device type %"
			  PRIu64, name, (uint64_t) MAJOR(dev->dev));
		return 0;
	}

	/* Check it's accessible */
	if (!dev_open_flags(dev, O_RDONLY|O_NONBLOCK, 1, 1)) {
		log_debug("%s: Skipping: open failed", name);
		return 0;
	}

	/* Skip cdrom device */
	#define CDROM_GET_CAPABILITY   0x5331
	if (ioctl(dev->fd, CDROM_GET_CAPABILITY) >= 0) {
		log_debug("%s: Skipping: cdrom device", name );
		goto out;
	}


	/* Check it's not too small */
	if (!dev_get_size(dev, &size)) {
		log_debug("%s: Skipping: dev_get_size failed", name);
		goto out;
	}

	if (size < pv_min_size()) {
		log_debug("%s: Skipping: Too small to hold a PV", name);
		goto out;
	}

	if (is_partitioned_dev(dev)) {
		log_debug("%s: Skipping: Partition table signature found",
			  name);
		goto out;
	}

	ret = 1;

      out:
	if (!dev_close(dev))
		stack;

	return ret;
}

static int _scan_proc_dev(const char *proc, const struct dm_config_node *cn)
{
	char line[80];
	char proc_devices[PATH_MAX];
	FILE *pd = NULL;
	int i, j = 0;
	int line_maj = 0;
	int blocksection = 0;
	size_t dev_len = 0;
	const struct dm_config_value *cv;
	const char *name;
	char *nl;

	if (!*proc) {
		log_verbose("No proc filesystem found: using all block device "
			    "types");
		for (i = 0; i < NUMBER_OF_MAJORS; i++)
			_partitions[i].max_partitions = 1;
		return 1;
	}

	/* All types unrecognised initially */
	memset(_partitions, 0, sizeof(_partitions));

	/* Extended devt will use MAJOR 259
	   Consider it an non-partitionable block device */
	_partitions[259].max_partitions = 1;

	if (dm_snprintf(proc_devices, sizeof(proc_devices),
			 "%s/devices", proc) < 0) {
		log_error("Failed to create /proc/devices string");
		return 0;
	}

	if (!(pd = fopen(proc_devices, "r"))) {
		log_sys_error("fopen", proc_devices);
		return 0;
	}

	while (fgets(line, sizeof(line), pd) != NULL) {
		i = 0;
		while (line[i] == ' ')
			i++;

		/* If it's not a number it may be name of section */
		line_maj = atoi(((char *) (line + i)));

		if (line_maj < 0 || line_maj >= NUMBER_OF_MAJORS) {
			/*
			 * Device numbers shown in /proc/devices are actually direct
			 * numbers passed to registering function, however the kernel
			 * uses only 12 bits, so use just 12 bits for major.
			 */
			if ((nl = strchr(line, '\n'))) *nl = '\0';
			log_warn("WARNING: /proc/devices line: %s, replacing major with %d.",
				 line, line_maj & (NUMBER_OF_MAJORS - 1));
			line_maj &= (NUMBER_OF_MAJORS - 1);
		}

		if (!line_maj) {
			blocksection = (line[i] == 'B') ? 1 : 0;
			continue;
		}

		/* We only want block devices ... */
		if (!blocksection)
			continue;

		/* Find the start of the device major name */
		while (line[i] != ' ' && line[i] != '\0')
			i++;
		while (line[i] == ' ')
			i++;

		/* Look for md device */
		if (!strncmp("md", line + i, 2) && isspace(*(line + i + 2)))
			_md_major = line_maj;

		/* Look for blkext device */
		if (!strncmp("blkext", line + i, 6) && isspace(*(line + i + 6)))
			_blkext_major = line_maj;

		/* Look for drbd device */
		if (!strncmp("drbd", line + i, 4) && isspace(*(line + i + 4)))
			_drbd_major = line_maj;

		/* Look for EMC powerpath */
		if (!strncmp("emcpower", line + i, 8) && isspace(*(line + i + 8)))
			_emcpower_major = line_maj;

		/* Look for device-mapper device */
		/* FIXME Cope with multiple majors */
		if (!strncmp("device-mapper", line + i, 13) && isspace(*(line + i + 13)))
			_device_mapper_major = line_maj;

		/* Major is SCSI device */
		if (!strncmp("sd", line + i, 2) && isspace(*(line + i + 2)))
			_partitions[line_maj].flags |= PARTITION_SCSI_DEVICE;

		/* Go through the valid device names and if there is a
		   match store max number of partitions */
		for (j = 0; _device_info[j].name[0]; j++) {
			dev_len = strlen(_device_info[j].name);
			if (dev_len <= strlen(line + i) &&
			    !strncmp(_device_info[j].name, line + i, dev_len) &&
			    (line_maj < NUMBER_OF_MAJORS)) {
				_partitions[line_maj].max_partitions =
				    _device_info[j].max_partitions;
				break;
			}
		}

		if (!cn)
			continue;

		/* Check devices/types for local variations */
		for (cv = cn->v; cv; cv = cv->next) {
			if (cv->type != DM_CFG_STRING) {
				log_error("Expecting string in devices/types "
					  "in config file");
				if (fclose(pd))
					log_sys_error("fclose", proc_devices);
				return 0;
			}
			dev_len = strlen(cv->v.str);
			name = cv->v.str;
			cv = cv->next;
			if (!cv || cv->type != DM_CFG_INT) {
				log_error("Max partition count missing for %s "
					  "in devices/types in config file",
					  name);
				if (fclose(pd))
					log_sys_error("fclose", proc_devices);
				return 0;
			}
			if (!cv->v.i) {
				log_error("Zero partition count invalid for "
					  "%s in devices/types in config file",
					  name);
				if (fclose(pd))
					log_sys_error("fclose", proc_devices);
				return 0;
			}
			if (dev_len <= strlen(line + i) &&
			    !strncmp(name, line + i, dev_len) &&
			    (line_maj < NUMBER_OF_MAJORS)) {
				_partitions[line_maj].max_partitions = cv->v.i;
				break;
			}
		}
	}

	if (fclose(pd))
		log_sys_error("fclose", proc_devices);

	return 1;
}

int max_partitions(int major)
{
	if (major >= NUMBER_OF_MAJORS)
		return 0;

	return _partitions[major].max_partitions;
}

int major_is_scsi_device(int major)
{
	if (major >= NUMBER_OF_MAJORS)
		return 0;

	return (_partitions[major].flags & PARTITION_SCSI_DEVICE) ? 1 : 0;
}

static void _lvm_type_filter_destroy(struct dev_filter *f)
{
	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying lvm_type filter while in use %u times.", f->use_count);

	dm_free(f);
}

struct dev_filter *lvm_type_filter_create(const char *proc,
					  const struct dm_config_node *cn)
{
	struct dev_filter *f;

	if (!(f = dm_zalloc(sizeof(struct dev_filter)))) {
		log_error("LVM type filter allocation failed");
		return NULL;
	}

	f->passes_filter = _passes_lvm_type_device_filter;
	f->destroy = _lvm_type_filter_destroy;
	f->use_count = 0;
	f->private = NULL;

	if (!_scan_proc_dev(proc, cn)) {
		dm_free(f);
		return_NULL;
	}

	return f;
}
