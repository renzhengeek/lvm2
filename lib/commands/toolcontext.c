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

#include "lib.h"
#include "toolcontext.h"
#include "metadata.h"
#include "defaults.h"
#include "lvm-string.h"
#include "activate.h"
#include "filter.h"
#include "filter-composite.h"
#include "filter-md.h"
#include "filter-mpath.h"
#include "filter-persistent.h"
#include "filter-regex.h"
#include "filter-sysfs.h"
#include "label.h"
#include "lvm-file.h"
#include "format-text.h"
#include "display.h"
#include "memlock.h"
#include "str_list.h"
#include "segtype.h"
#include "lvmcache.h"
#include "lvmetad.h"
#include "dev-cache.h"
#include "archiver.h"

#ifdef HAVE_LIBDL
#include "sharedlib.h"
#endif

#ifdef LVM1_INTERNAL
#include "format1.h"
#endif

#ifdef POOL_INTERNAL
#include "format_pool.h"
#endif

#include <locale.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>

#ifdef linux
#  include <malloc.h>
#endif

static const size_t linebuffer_size = 4096;

static int _get_env_vars(struct cmd_context *cmd)
{
	const char *e;

	/* Set to "" to avoid using any system directory */
	if ((e = getenv("LVM_SYSTEM_DIR"))) {
		if (dm_snprintf(cmd->system_dir, sizeof(cmd->system_dir),
				 "%s", e) < 0) {
			log_error("LVM_SYSTEM_DIR environment variable "
				  "is too long.");
			return 0;
		}
	}

	return 1;
}

static void _get_sysfs_dir(struct cmd_context *cmd)
{
	static char proc_mounts[PATH_MAX];
	static char *split[4], buffer[PATH_MAX + 16];
	FILE *fp;
	char *sys_mnt = NULL;

	cmd->sysfs_dir[0] = '\0';
	if (!*cmd->proc_dir) {
		log_debug("No proc filesystem found: skipping sysfs detection");
		return;
	}

	if (dm_snprintf(proc_mounts, sizeof(proc_mounts),
			 "%s/mounts", cmd->proc_dir) < 0) {
		log_error("Failed to create /proc/mounts string for sysfs detection");
		return;
	}

	if (!(fp = fopen(proc_mounts, "r"))) {
		log_sys_error("_get_sysfs_dir fopen", proc_mounts);
		return;
	}

	while (fgets(buffer, sizeof(buffer), fp)) {
		if (dm_split_words(buffer, 4, 0, split) == 4 &&
		    !strcmp(split[2], "sysfs")) {
			sys_mnt = split[1];
			break;
		}
	}

	if (fclose(fp))
		log_sys_error("fclose", proc_mounts);

	if (!sys_mnt) {
		log_error("Failed to find sysfs mount point");
		return;
	}

	strncpy(cmd->sysfs_dir, sys_mnt, sizeof(cmd->sysfs_dir));
}

static void _init_logging(struct cmd_context *cmd)
{
	int append = 1;
	time_t t;

	const char *log_file;
	char timebuf[26];

	/* Syslog */
	cmd->default_settings.syslog =
	    find_config_tree_int(cmd, "log/syslog", DEFAULT_SYSLOG);
	if (cmd->default_settings.syslog != 1)
		fin_syslog();

	if (cmd->default_settings.syslog > 1)
		init_syslog(cmd->default_settings.syslog);

	/* Debug level for log file output */
	cmd->default_settings.debug =
	    find_config_tree_int(cmd, "log/level", DEFAULT_LOGLEVEL);
	init_debug(cmd->default_settings.debug);

	/*
	 * Suppress all non-essential stdout?
	 * -qq can override the default of 0 to 1 later.
	 * Once set to 1, there is no facility to change it back to 0.
	 */
	cmd->default_settings.silent = silent_mode() ? :
	    find_config_tree_int(cmd, "log/silent", DEFAULT_SILENT);
	init_silent(cmd->default_settings.silent);

	/* Verbose level for tty output */
	cmd->default_settings.verbose =
	    find_config_tree_int(cmd, "log/verbose", DEFAULT_VERBOSE);
	init_verbose(cmd->default_settings.verbose + VERBOSE_BASE_LEVEL);

	/* Log message formatting */
	init_indent(find_config_tree_int(cmd, "log/indent",
					 DEFAULT_INDENT));
	init_abort_on_internal_errors(find_config_tree_int(cmd, "global/abort_on_internal_errors",
							   DEFAULT_ABORT_ON_INTERNAL_ERRORS));

	cmd->default_settings.msg_prefix =
		find_config_tree_str_allow_empty(cmd, "log/prefix", DEFAULT_MSG_PREFIX);

	init_msg_prefix(cmd->default_settings.msg_prefix);

	cmd->default_settings.cmd_name = find_config_tree_int(cmd,
							 "log/command_names",
							 DEFAULT_CMD_NAME);
	init_cmd_name(cmd->default_settings.cmd_name);

	/* Test mode */
	cmd->default_settings.test =
	    find_config_tree_int(cmd, "global/test", 0);
	init_test(cmd->default_settings.test);

	/* Settings for logging to file */
	if (find_config_tree_int(cmd, "log/overwrite", DEFAULT_OVERWRITE))
		append = 0;

	log_file = find_config_tree_str(cmd, "log/file", 0);

	if (log_file) {
		release_log_memory();
		fin_log();
		init_log_file(log_file, append);
	}

	log_file = find_config_tree_str(cmd, "log/activate_file", 0);
	if (log_file)
		init_log_direct(log_file, append);

	init_log_while_suspended(find_config_tree_int(cmd,
						 "log/activation", 0));

	t = time(NULL);
	ctime_r(&t, &timebuf[0]);
	timebuf[24] = '\0';
	log_verbose("Logging initialised at %s", timebuf);

	/* Tell device-mapper about our logging */
#ifdef DEVMAPPER_SUPPORT
	dm_log_with_errno_init(print_log);
#endif
	reset_log_duplicated();
	reset_lvm_errno(1);
}

#ifdef UDEV_SYNC_SUPPORT
/*
 * Until the DM_UEVENT_GENERATED_FLAG was introduced in kernel patch 
 * 856a6f1dbd8940e72755af145ebcd806408ecedd
 * some operations could not be performed by udev, requiring our fallback code.
 */
static int _dm_driver_has_stable_udev_support(void)
{
	char vsn[80];
	unsigned maj, min, patchlevel;

	return driver_version(vsn, sizeof(vsn)) &&
	       (sscanf(vsn, "%u.%u.%u", &maj, &min, &patchlevel) == 3) &&
	       (maj == 4 ? min >= 18 : maj > 4);
}
#endif

static int _process_config(struct cmd_context *cmd)
{
	mode_t old_umask;
	const char *read_ahead;
	struct stat st;
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	int64_t pv_min_kb;

	/* umask */
	cmd->default_settings.umask = find_config_tree_int(cmd,
						      "global/umask",
						      DEFAULT_UMASK);

	if ((old_umask = umask((mode_t) cmd->default_settings.umask)) !=
	    (mode_t) cmd->default_settings.umask)
		log_verbose("Set umask from %04o to %04o",
                            old_umask, cmd->default_settings.umask);

	/* dev dir */
	if (dm_snprintf(cmd->dev_dir, sizeof(cmd->dev_dir), "%s/",
			 find_config_tree_str(cmd, "devices/dir",
					 DEFAULT_DEV_DIR)) < 0) {
		log_error("Device directory given in config file too long");
		return 0;
	}
#ifdef DEVMAPPER_SUPPORT
	dm_set_dev_dir(cmd->dev_dir);

	if (!dm_set_uuid_prefix("LVM-"))
		return_0;
#endif

	/* proc dir */
	if (dm_snprintf(cmd->proc_dir, sizeof(cmd->proc_dir), "%s",
			 find_config_tree_str(cmd, "global/proc",
					 DEFAULT_PROC_DIR)) < 0) {
		log_error("Device directory given in config file too long");
		return 0;
	}

	if (*cmd->proc_dir && !dir_exists(cmd->proc_dir)) {
		log_warn("WARNING: proc dir %s not found - some checks will be bypassed",
			 cmd->proc_dir);
		cmd->proc_dir[0] = '\0';
	}

	/* FIXME Use global value of sysfs_dir everywhere instead cmd->sysfs_dir. */
	_get_sysfs_dir(cmd);
	set_sysfs_dir_path(cmd->sysfs_dir);
	dm_set_sysfs_dir(cmd->sysfs_dir);

	/* activation? */
	cmd->default_settings.activation = find_config_tree_int(cmd,
							   "global/activation",
							   DEFAULT_ACTIVATION);
	set_activation(cmd->default_settings.activation);

	cmd->default_settings.suffix = find_config_tree_int(cmd,
						       "global/suffix",
						       DEFAULT_SUFFIX);

	if (!(cmd->default_settings.unit_factor =
	      units_to_bytes(find_config_tree_str(cmd,
					     "global/units",
					     DEFAULT_UNITS),
			     &cmd->default_settings.unit_type))) {
		log_error("Invalid units specification");
		return 0;
	}

	read_ahead = find_config_tree_str(cmd, "activation/readahead", DEFAULT_READ_AHEAD);
	if (!strcasecmp(read_ahead, "auto"))
		cmd->default_settings.read_ahead = DM_READ_AHEAD_AUTO;
	else if (!strcasecmp(read_ahead, "none"))
		cmd->default_settings.read_ahead = DM_READ_AHEAD_NONE;
	else {
		log_error("Invalid readahead specification");
		return 0;
	}

	cmd->default_settings.udev_rules = find_config_tree_int(cmd,
								"activation/udev_rules",
								DEFAULT_UDEV_RULES);

	cmd->default_settings.udev_sync = find_config_tree_int(cmd,
								"activation/udev_sync",
								DEFAULT_UDEV_SYNC);

	init_retry_deactivation(find_config_tree_int(cmd, "activation/retry_deactivation",
							DEFAULT_RETRY_DEACTIVATION));

	init_activation_checks(find_config_tree_int(cmd, "activation/checks",
						      DEFAULT_ACTIVATION_CHECKS));

#ifdef UDEV_SYNC_SUPPORT
	/*
	 * We need udev rules to be applied, otherwise we would end up with no
	 * nodes and symlinks! However, we can disable the synchronization itself
	 * in runtime and still have only udev to create the nodes and symlinks
	 * without any fallback.
	 */
	cmd->default_settings.udev_fallback = cmd->default_settings.udev_rules ?
		find_config_tree_int(cmd, "activation/verify_udev_operations",
				     DEFAULT_VERIFY_UDEV_OPERATIONS) : 1;

	/* Do not rely fully on udev if the udev support is known to be incomplete. */
	if (!cmd->default_settings.udev_fallback && !_dm_driver_has_stable_udev_support()) {
		log_very_verbose("Kernel driver has incomplete udev support so "
				 "LVM will check and perform some operations itself.");
		cmd->default_settings.udev_fallback = 1;
	}

#else
	/* We must use old node/symlink creation code if not compiled with udev support at all! */
	cmd->default_settings.udev_fallback = 1;
#endif

	cmd->use_linear_target = find_config_tree_int(cmd,
						      "activation/use_linear_target",
						       DEFAULT_USE_LINEAR_TARGET);

	cmd->stripe_filler = find_config_tree_str(cmd,
						  "activation/missing_stripe_filler",
						  DEFAULT_STRIPE_FILLER);

	/* FIXME Missing error code checks from the stats, not log_warn?, notify if setting overridden, delay message/check till it is actually used (eg consider if lvm shell - file could appear later after this check)? */
	if (!strcmp(cmd->stripe_filler, "/dev/ioerror") &&
	    stat(cmd->stripe_filler, &st))
		cmd->stripe_filler = "error";

	if (strcmp(cmd->stripe_filler, "error")) {
		if (stat(cmd->stripe_filler, &st)) {
			log_warn("WARNING: activation/missing_stripe_filler = \"%s\" "
				 "is invalid,", cmd->stripe_filler);
			log_warn("         stat failed: %s", strerror(errno));
			log_warn("Falling back to \"error\" missing_stripe_filler.");
			cmd->stripe_filler = "error";
		} else if (!S_ISBLK(st.st_mode)) {
			log_warn("WARNING: activation/missing_stripe_filler = \"%s\" "
				 "is not a block device.", cmd->stripe_filler);
			log_warn("Falling back to \"error\" missing_stripe_filler.");
			cmd->stripe_filler = "error";
		}
	}

	cmd->si_unit_consistency = find_config_tree_int(cmd,
						  "global/si_unit_consistency",
						  DEFAULT_SI_UNIT_CONSISTENCY);

	if ((cn = find_config_tree_node(cmd, "activation/mlock_filter")))
		for (cv = cn->v; cv; cv = cv->next) 
			if ((cv->type != DM_CFG_STRING) || !cv->v.str[0]) 
				log_error("Ignoring invalid activation/mlock_filter entry in config file");

	cmd->metadata_read_only = find_config_tree_int(cmd, "global/metadata_read_only",
						       DEFAULT_METADATA_READ_ONLY);

	pv_min_kb = find_config_tree_int64(cmd, "devices/pv_min_size", DEFAULT_PV_MIN_SIZE_KB);
	if (pv_min_kb < PV_MIN_SIZE_KB) {
		log_warn("Ignoring too small pv_min_size %" PRId64 "KB, using default %dKB.",
			 pv_min_kb, PV_MIN_SIZE_KB);
		pv_min_kb = PV_MIN_SIZE_KB;
	}
	/* LVM stores sizes internally in units of 512-byte sectors. */
	init_pv_min_size((uint64_t)pv_min_kb * (1024 >> SECTOR_SHIFT));

	init_detect_internal_vg_cache_corruption
		(find_config_tree_int(cmd, "global/detect_internal_vg_cache_corruption",
				      DEFAULT_DETECT_INTERNAL_VG_CACHE_CORRUPTION));

	lvmetad_set_active(find_config_tree_int(cmd, "global/use_lvmetad", 0));

	return 1;
}

static int _set_tag(struct cmd_context *cmd, const char *tag)
{
	log_very_verbose("Setting host tag: %s", dm_pool_strdup(cmd->libmem, tag));

	if (!str_list_add(cmd->libmem, &cmd->tags, tag)) {
		log_error("_set_tag: str_list_add %s failed", tag);
		return 0;
	}

	return 1;
}

static int _check_host_filters(struct cmd_context *cmd, const struct dm_config_node *hn,
			       int *passes)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;

	*passes = 1;

	for (cn = hn; cn; cn = cn->sib) {
		if (!cn->v)
			continue;
		if (!strcmp(cn->key, "host_list")) {
			*passes = 0;
			if (cn->v->type == DM_CFG_EMPTY_ARRAY)
				continue;
			for (cv = cn->v; cv; cv = cv->next) {
				if (cv->type != DM_CFG_STRING) {
					log_error("Invalid hostname string "
						  "for tag %s", cn->key);
					return 0;
				}
				if (!strcmp(cv->v.str, cmd->hostname)) {
					*passes = 1;
					return 1;
				}
			}
		}
		if (!strcmp(cn->key, "host_filter")) {
			log_error("host_filter not supported yet");
			return 0;
		}
	}

	return 1;
}

static int _init_tags(struct cmd_context *cmd, struct dm_config_tree *cft)
{
	const struct dm_config_node *tn, *cn;
	const char *tag;
	int passes;

	if (!(tn = dm_config_find_node(cft->root, "tags")) || !tn->child)
		return 1;

	/* NB hosttags 0 when already 1 intentionally does not delete the tag */
	if (!cmd->hosttags && dm_config_find_int(cft->root, "tags/hosttags",
					      DEFAULT_HOSTTAGS)) {
		/* FIXME Strip out invalid chars: only A-Za-z0-9_+.- */
		if (!_set_tag(cmd, cmd->hostname))
			return_0;
		cmd->hosttags = 1;
	}

	for (cn = tn->child; cn; cn = cn->sib) {
		if (cn->v)
			continue;
		tag = cn->key;
		if (*tag == '@')
			tag++;
		if (!validate_name(tag)) {
			log_error("Invalid tag in config file: %s", cn->key);
			return 0;
		}
		if (cn->child) {
			passes = 0;
			if (!_check_host_filters(cmd, cn->child, &passes))
				return_0;
			if (!passes)
				continue;
		}
		if (!_set_tag(cmd, tag))
			return_0;
	}

	return 1;
}

static int _load_config_file(struct cmd_context *cmd, const char *tag)
{
	static char config_file[PATH_MAX] = "";
	const char *filler = "";
	struct stat info;
	struct config_tree_list *cfl;

	if (*tag)
		filler = "_";

	if (dm_snprintf(config_file, sizeof(config_file), "%s/lvm%s%s.conf",
			 cmd->system_dir, filler, tag) < 0) {
		log_error("LVM_SYSTEM_DIR or tag was too long");
		return 0;
	}

	if (!(cfl = dm_pool_alloc(cmd->libmem, sizeof(*cfl)))) {
		log_error("config_tree_list allocation failed");
		return 0;
	}

	if (!(cfl->cft = config_file_open(config_file, 0))) {
		log_error("config_tree allocation failed");
		return 0;
	}

	/* Is there a config file? */
	if (stat(config_file, &info) == -1) {
		if (errno == ENOENT) {
			dm_list_add(&cmd->config_files, &cfl->list);
			goto out;
		}
		log_sys_error("stat", config_file);
		config_file_destroy(cfl->cft);
		return 0;
	}

	log_very_verbose("Loading config file: %s", config_file);
	if (!config_file_read(cfl->cft)) {
		log_error("Failed to load config file %s", config_file);
		config_file_destroy(cfl->cft);
		return 0;
	}

	dm_list_add(&cmd->config_files, &cfl->list);

      out:
	if (*tag) {
		if (!_init_tags(cmd, cfl->cft))
			return_0;
	} else
		/* Use temporary copy of lvm.conf while loading other files */
		cmd->cft = cfl->cft;

	return 1;
}

/* Find and read first config file */
static int _init_lvm_conf(struct cmd_context *cmd)
{
	/* No config file if LVM_SYSTEM_DIR is empty */
	if (!*cmd->system_dir) {
		if (!(cmd->cft = config_file_open(NULL, 0))) {
			log_error("Failed to create config tree");
			return 0;
		}
		return 1;
	}

	if (!_load_config_file(cmd, ""))
		return_0;

	return 1;
}

/* Read any additional config files */
static int _init_tag_configs(struct cmd_context *cmd)
{
	struct str_list *sl;

	/* Tag list may grow while inside this loop */
	dm_list_iterate_items(sl, &cmd->tags) {
		if (!_load_config_file(cmd, sl->str))
			return_0;
	}

	return 1;
}

static struct dm_config_tree *_merge_config_files(struct cmd_context *cmd, struct dm_config_tree *cft)
{
	struct config_tree_list *cfl;

	/* Replace temporary duplicate copy of lvm.conf */
	if (cft->root) {
		if (!(cft = config_file_open(NULL, 0))) {
			log_error("Failed to create config tree");
			return 0;
		}
	}

	dm_list_iterate_items(cfl, &cmd->config_files) {
		/* Merge all config trees into cmd->cft using merge/tag rules */
		if (!merge_config_tree(cmd, cft, cfl->cft))
			return_0;
	}

	return cft;
}

static void _destroy_tags(struct cmd_context *cmd)
{
	struct dm_list *slh, *slht;

	dm_list_iterate_safe(slh, slht, &cmd->tags) {
		dm_list_del(slh);
	}
}

int config_files_changed(struct cmd_context *cmd)
{
	struct config_tree_list *cfl;

	dm_list_iterate_items(cfl, &cmd->config_files) {
		if (config_file_changed(cfl->cft))
			return 1;
	}

	return 0;
}

/*
 * Returns cmdline config_tree that overrides all others, if present.
 */
static struct dm_config_tree *_destroy_tag_configs(struct cmd_context *cmd)
{
	struct config_tree_list *cfl;
	struct dm_config_tree *cft_cmdline = NULL, *cft;

	cft = dm_config_remove_cascaded_tree(cmd->cft);
	if (cft) {
		cft_cmdline = cmd->cft;
		cmd->cft = cft;
	}

	dm_list_iterate_items(cfl, &cmd->config_files) {
		if (cfl->cft == cmd->cft)
			cmd->cft = NULL;
		config_file_destroy(cfl->cft);
	}

	if (cmd->cft) {
		config_file_destroy(cmd->cft);
		cmd->cft = NULL;
	}

	dm_list_init(&cmd->config_files);

	return cft_cmdline;
}

static int _init_dev_cache(struct cmd_context *cmd)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	size_t len, udev_dir_len = strlen(DM_UDEV_DEV_DIR);
	int len_diff;
	int device_list_from_udev;

	init_dev_disable_after_error_count(
		find_config_tree_int(cmd, "devices/disable_after_error_count",
				     DEFAULT_DISABLE_AFTER_ERROR_COUNT));

	if (!dev_cache_init(cmd))
		return_0;

	device_list_from_udev = udev_is_running() ?
		find_config_tree_bool(cmd, "devices/obtain_device_list_from_udev",
				      DEFAULT_OBTAIN_DEVICE_LIST_FROM_UDEV) : 0;
	init_obtain_device_list_from_udev(device_list_from_udev);

	if (!(cn = find_config_tree_node(cmd, "devices/scan"))) {
		if (!dev_cache_add_dir("/dev")) {
			log_error("Failed to add /dev to internal "
				  "device cache");
			return 0;
		}
		log_verbose("device/scan not in config file: "
			    "Defaulting to /dev");
		return 1;
	}

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_error("Invalid string in config file: "
				  "devices/scan");
			return 0;
		}

		if (device_list_from_udev) {
			len = strlen(cv->v.str);

			/*
			 * DM_UDEV_DEV_DIR always has '/' at its end.
			 * If the item in the conf does not have it, be sure
			 * to make the right comparison without the '/' char!
			 */
			len_diff = len && cv->v.str[len - 1] != '/' ?
					udev_dir_len - 1 != len :
					udev_dir_len != len;

			if (len_diff || strncmp(DM_UDEV_DEV_DIR, cv->v.str, len)) {
				device_list_from_udev = 0;
				init_obtain_device_list_from_udev(0);
			}
		}

		if (!dev_cache_add_dir(cv->v.str)) {
			log_error("Failed to add %s to internal device cache",
				  cv->v.str);
			return 0;
		}
	}

	if (!(cn = find_config_tree_node(cmd, "devices/loopfiles")))
		return 1;

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_error("Invalid string in config file: "
				  "devices/loopfiles");
			return 0;
		}

		if (!dev_cache_add_loopfile(cv->v.str)) {
			log_error("Failed to add loopfile %s to internal "
				  "device cache", cv->v.str);
			return 0;
		}
	}


	return 1;
}

#define MAX_FILTERS 5

static struct dev_filter *_init_filter_components(struct cmd_context *cmd)
{
	int nr_filt = 0;
	const struct dm_config_node *cn;
	struct dev_filter *filters[MAX_FILTERS] = { 0 };
	struct dev_filter *composite;

	/*
	 * Filters listed in order: top one gets applied first.
	 * Failure to initialise some filters is not fatal.
	 * Update MAX_FILTERS definition above when adding new filters.
	 */

	/*
	 * sysfs filter. Only available on 2.6 kernels.  Non-critical.
	 * Listed first because it's very efficient at eliminating
	 * unavailable devices.
	 */
	if (find_config_tree_bool(cmd, "devices/sysfs_scan",
			     DEFAULT_SYSFS_SCAN)) {
		if ((filters[nr_filt] = sysfs_filter_create(cmd->sysfs_dir)))
			nr_filt++;
	}

	/* regex filter. Optional. */
	if (!(cn = find_config_tree_node(cmd, "devices/filter")))
		log_very_verbose("devices/filter not found in config file: "
				 "no regex filter installed");

	else if (!(filters[nr_filt] = regex_filter_create(cn->v))) {
		log_error("Failed to create regex device filter");
		goto bad;
	} else
		nr_filt++;

	/* device type filter. Required. */
	cn = find_config_tree_node(cmd, "devices/types");
	if (!(filters[nr_filt] = lvm_type_filter_create(cmd->proc_dir, cn))) {
		log_error("Failed to create lvm type filter");
		goto bad;
	}
	nr_filt++;

	/* md component filter. Optional, non-critical. */
	if (find_config_tree_bool(cmd, "devices/md_component_detection",
			     DEFAULT_MD_COMPONENT_DETECTION)) {
		init_md_filtering(1);
		if ((filters[nr_filt] = md_filter_create()))
			nr_filt++;
	}

	/* mpath component filter. Optional, non-critical. */
	if (find_config_tree_bool(cmd, "devices/multipath_component_detection",
			     DEFAULT_MULTIPATH_COMPONENT_DETECTION)) {
		if ((filters[nr_filt] = mpath_filter_create(cmd->sysfs_dir)))
			nr_filt++;
	}

	/* Only build a composite filter if we really need it. */
	if (nr_filt == 1)
		return filters[0];

	if (!(composite = composite_filter_create(nr_filt, filters)))
		goto_bad;

	return composite;
bad:
	while (--nr_filt >= 0)
		 filters[nr_filt]->destroy(filters[nr_filt]);

	return NULL;
}

static int _init_filters(struct cmd_context *cmd, unsigned load_persistent_cache)
{
	static char cache_file[PATH_MAX];
	const char *dev_cache = NULL, *cache_dir, *cache_file_prefix;
	struct dev_filter *f3, *f4;
	struct stat st;

	cmd->dump_filter = 0;

	if (!(f3 = _init_filter_components(cmd)))
		return_0;

	init_ignore_suspended_devices(find_config_tree_int(cmd,
	    "devices/ignore_suspended_devices", DEFAULT_IGNORE_SUSPENDED_DEVICES));

	/*
	 * If 'cache_dir' or 'cache_file_prefix' is set, ignore 'cache'.
	 */
	cache_dir = find_config_tree_str(cmd, "devices/cache_dir", NULL);
	cache_file_prefix = find_config_tree_str(cmd, "devices/cache_file_prefix", NULL);

	if (cache_dir || cache_file_prefix) {
		if (dm_snprintf(cache_file, sizeof(cache_file),
		    "%s%s%s/%s.cache",
		    cache_dir ? "" : cmd->system_dir,
		    cache_dir ? "" : "/",
		    cache_dir ? : DEFAULT_CACHE_SUBDIR,
		    cache_file_prefix ? : DEFAULT_CACHE_FILE_PREFIX) < 0) {
			log_error("Persistent cache filename too long.");
			f3->destroy(f3);
			return 0;
		}
	} else if (!(dev_cache = find_config_tree_str(cmd, "devices/cache", NULL)) &&
		   (dm_snprintf(cache_file, sizeof(cache_file),
				"%s/%s/%s.cache",
				cmd->system_dir, DEFAULT_CACHE_SUBDIR,
				DEFAULT_CACHE_FILE_PREFIX) < 0)) {
		log_error("Persistent cache filename too long.");
		f3->destroy(f3);
		return 0;
	}

	if (!dev_cache)
		dev_cache = cache_file;

	if (!(f4 = persistent_filter_create(f3, dev_cache))) {
		log_verbose("Failed to create persistent device filter.");
		f3->destroy(f3);
		return_0;
	}

	/* Should we ever dump persistent filter state? */
	if (find_config_tree_int(cmd, "devices/write_cache_state", 1))
		cmd->dump_filter = 1;

	if (!*cmd->system_dir)
		cmd->dump_filter = 0;

	/*
	 * Only load persistent filter device cache on startup if it is newer
	 * than the config file and this is not a long-lived process.
	 */
	if (load_persistent_cache && !cmd->is_long_lived &&
	    !stat(dev_cache, &st) &&
	    (st.st_ctime > config_file_timestamp(cmd->cft)) &&
	    !persistent_filter_load(f4, NULL))
		log_verbose("Failed to load existing device cache from %s",
			    dev_cache);

	cmd->filter = f4;

	return 1;
}

struct format_type *get_format_by_name(struct cmd_context *cmd, const char *format)
{
        struct format_type *fmt;

        dm_list_iterate_items(fmt, &cmd->formats)
                if (!strcasecmp(fmt->name, format) ||
                    !strcasecmp(fmt->name + 3, format) ||
                    (fmt->alias && !strcasecmp(fmt->alias, format)))
                        return fmt;

        return NULL;
}

static int _init_formats(struct cmd_context *cmd)
{
	const char *format;

	struct format_type *fmt;

#ifdef HAVE_LIBDL
	const struct dm_config_node *cn;
#endif

#ifdef LVM1_INTERNAL
	if (!(fmt = init_lvm1_format(cmd)))
		return 0;
	fmt->library = NULL;
	dm_list_add(&cmd->formats, &fmt->list);
#endif

#ifdef POOL_INTERNAL
	if (!(fmt = init_pool_format(cmd)))
		return 0;
	fmt->library = NULL;
	dm_list_add(&cmd->formats, &fmt->list);
#endif

#ifdef HAVE_LIBDL
	/* Load any formats in shared libs if not static */
	if (!is_static() &&
	    (cn = find_config_tree_node(cmd, "global/format_libraries"))) {

		const struct dm_config_value *cv;
		struct format_type *(*init_format_fn) (struct cmd_context *);
		void *lib;

		for (cv = cn->v; cv; cv = cv->next) {
			if (cv->type != DM_CFG_STRING) {
				log_error("Invalid string in config file: "
					  "global/format_libraries");
				return 0;
			}
			if (!(lib = load_shared_library(cmd, cv->v.str,
							"format", 0)))
				return_0;

			if (!(init_format_fn = dlsym(lib, "init_format"))) {
				log_error("Shared library %s does not contain "
					  "format functions", cv->v.str);
				dlclose(lib);
				return 0;
			}

			if (!(fmt = init_format_fn(cmd))) {
				dlclose(lib);
				return_0;
			}

			fmt->library = lib;
			dm_list_add(&cmd->formats, &fmt->list);
		}
	}
#endif

	if (!(fmt = create_text_format(cmd)))
		return 0;
	fmt->library = NULL;
	dm_list_add(&cmd->formats, &fmt->list);

	cmd->fmt_backup = fmt;

	format = find_config_tree_str(cmd, "global/format",
				 DEFAULT_FORMAT);

	dm_list_iterate_items(fmt, &cmd->formats) {
		if (!strcasecmp(fmt->name, format) ||
		    (fmt->alias && !strcasecmp(fmt->alias, format))) {
			cmd->default_settings.fmt_name = fmt->name;
			cmd->fmt = fmt;
			return 1;
		}
	}

	log_error("_init_formats: Default format (%s) not found", format);
	return 0;
}

int init_lvmcache_orphans(struct cmd_context *cmd)
{
	struct format_type *fmt;

	dm_list_iterate_items(fmt, &cmd->formats)
		if (!lvmcache_add_orphan_vginfo(fmt->orphan_vg_name, fmt))
			return_0;

	return 1;
}

struct segtype_library {
	struct cmd_context *cmd;
	void *lib;
	const char *libname;
};

int lvm_register_segtype(struct segtype_library *seglib,
			 struct segment_type *segtype)
{
	struct segment_type *segtype2;

	segtype->library = seglib->lib;
	segtype->cmd = seglib->cmd;

	dm_list_iterate_items(segtype2, &seglib->cmd->segtypes) {
		if (strcmp(segtype2->name, segtype->name))
			continue;
		log_error("Duplicate segment type %s: "
			  "unloading shared library %s",
			  segtype->name, seglib->libname);
		segtype->ops->destroy(segtype);
		return 0;
	}

	dm_list_add(&seglib->cmd->segtypes, &segtype->list);

	return 1;
}

static int _init_single_segtype(struct cmd_context *cmd,
				struct segtype_library *seglib)
{
	struct segment_type *(*init_segtype_fn) (struct cmd_context *);
	struct segment_type *segtype;

	if (!(init_segtype_fn = dlsym(seglib->lib, "init_segtype"))) {
		log_error("Shared library %s does not contain segment type "
			  "functions", seglib->libname);
		return 0;
	}

	if (!(segtype = init_segtype_fn(seglib->cmd)))
		return_0;

	return lvm_register_segtype(seglib, segtype);
}

static int _init_segtypes(struct cmd_context *cmd)
{
	int i;
	struct segment_type *segtype;
	struct segtype_library seglib = { .cmd = cmd, .lib = NULL };
	struct segment_type *(*init_segtype_array[])(struct cmd_context *cmd) = {
		init_striped_segtype,
		init_zero_segtype,
		init_error_segtype,
		init_free_segtype,
#ifdef SNAPSHOT_INTERNAL
		init_snapshot_segtype,
#endif
#ifdef MIRRORED_INTERNAL
		init_mirrored_segtype,
#endif
		NULL
	};

#ifdef HAVE_LIBDL
	const struct dm_config_node *cn;
#endif

	for (i = 0; init_segtype_array[i]; i++) {
		if (!(segtype = init_segtype_array[i](cmd)))
			return 0;
		segtype->library = NULL;
		dm_list_add(&cmd->segtypes, &segtype->list);
	}

#ifdef REPLICATOR_INTERNAL
	if (!init_replicator_segtype(cmd, &seglib))
		return 0;
#endif

#ifdef RAID_INTERNAL
	if (!init_raid_segtypes(cmd, &seglib))
		return 0;
#endif

#ifdef THIN_INTERNAL
	if (!init_thin_segtypes(cmd, &seglib))
		return 0;
#endif

#ifdef HAVE_LIBDL
	/* Load any formats in shared libs unless static */
	if (!is_static() &&
	    (cn = find_config_tree_node(cmd, "global/segment_libraries"))) {

		const struct dm_config_value *cv;
		int (*init_multiple_segtypes_fn) (struct cmd_context *,
						  struct segtype_library *);

		for (cv = cn->v; cv; cv = cv->next) {
			if (cv->type != DM_CFG_STRING) {
				log_error("Invalid string in config file: "
					  "global/segment_libraries");
				return 0;
			}
			seglib.libname = cv->v.str;
			if (!(seglib.lib = load_shared_library(cmd,
							seglib.libname,
							"segment type", 0)))
				return_0;

			if ((init_multiple_segtypes_fn =
			    dlsym(seglib.lib, "init_multiple_segtypes"))) {
				if (dlsym(seglib.lib, "init_segtype"))
					log_warn("WARNING: Shared lib %s has "
						 "conflicting init fns.  Using"
						 " init_multiple_segtypes().",
						 seglib.libname);
			} else
				init_multiple_segtypes_fn =
				    _init_single_segtype;
 
			if (!init_multiple_segtypes_fn(cmd, &seglib)) {
				struct dm_list *sgtl, *tmp;
				log_error("init_multiple_segtypes() failed: "
					  "Unloading shared library %s",
					  seglib.libname);
				dm_list_iterate_safe(sgtl, tmp, &cmd->segtypes) {
					segtype = dm_list_item(sgtl, struct segment_type);
					if (segtype->library == seglib.lib) {
						dm_list_del(&segtype->list);
						segtype->ops->destroy(segtype);
					}
				}
				dlclose(seglib.lib);
				return_0;
			}
		}
	}
#endif

	return 1;
}

static int _init_hostname(struct cmd_context *cmd)
{
	struct utsname uts;

	if (uname(&uts)) {
		log_sys_error("uname", "_init_hostname");
		return 0;
	}

	if (!(cmd->hostname = dm_pool_strdup(cmd->libmem, uts.nodename))) {
		log_error("_init_hostname: dm_pool_strdup failed");
		return 0;
	}

	if (!(cmd->kernel_vsn = dm_pool_strdup(cmd->libmem, uts.release))) {
		log_error("_init_hostname: dm_pool_strdup kernel_vsn failed");
		return 0;
	}

	return 1;
}

static int _init_backup(struct cmd_context *cmd)
{
	static char default_dir[PATH_MAX];
	uint32_t days, min;
	const char *dir;

	if (!cmd->system_dir[0]) {
		log_warn("WARNING: Metadata changes will NOT be backed up");
		backup_init(cmd, "", 0);
		archive_init(cmd, "", 0, 0, 0);
		return 1;
	}

	/* set up archiving */
	cmd->default_settings.archive =
	    find_config_tree_bool(cmd, "backup/archive",
			     DEFAULT_ARCHIVE_ENABLED);

	days = (uint32_t) find_config_tree_int(cmd, "backup/retain_days",
					  DEFAULT_ARCHIVE_DAYS);

	min = (uint32_t) find_config_tree_int(cmd, "backup/retain_min",
					 DEFAULT_ARCHIVE_NUMBER);

	if (dm_snprintf
	    (default_dir, sizeof(default_dir), "%s/%s", cmd->system_dir,
	     DEFAULT_ARCHIVE_SUBDIR) == -1) {
		log_error("Couldn't create default archive path '%s/%s'.",
			  cmd->system_dir, DEFAULT_ARCHIVE_SUBDIR);
		return 0;
	}

	dir = find_config_tree_str(cmd, "backup/archive_dir",
			      default_dir);

	if (!archive_init(cmd, dir, days, min,
			  cmd->default_settings.archive)) {
		log_debug("archive_init failed.");
		return 0;
	}

	/* set up the backup */
	cmd->default_settings.backup =
	    find_config_tree_bool(cmd, "backup/backup",
			     DEFAULT_BACKUP_ENABLED);

	if (dm_snprintf
	    (default_dir, sizeof(default_dir), "%s/%s", cmd->system_dir,
	     DEFAULT_BACKUP_SUBDIR) == -1) {
		log_error("Couldn't create default backup path '%s/%s'.",
			  cmd->system_dir, DEFAULT_BACKUP_SUBDIR);
		return 0;
	}

	dir = find_config_tree_str(cmd, "backup/backup_dir", default_dir);

	if (!backup_init(cmd, dir, cmd->default_settings.backup)) {
		log_debug("backup_init failed.");
		return 0;
	}

	return 1;
}

static void _init_rand(struct cmd_context *cmd)
{
	if (read_urandom(&cmd->rand_seed, sizeof(cmd->rand_seed))) {
		reset_lvm_errno(1);
		return;
	}

	cmd->rand_seed = (unsigned) time(NULL) + (unsigned) getpid();
	reset_lvm_errno(1);
}

static void _init_globals(struct cmd_context *cmd)
{
	init_full_scan_done(0);
	init_mirror_in_sync(0);
}

/*
 * Close and reopen stream on file descriptor fd.
 */
static int _reopen_stream(FILE *stream, int fd, const char *mode, const char *name, FILE **new_stream)
{
	int fd_copy, new_fd;

	if ((fd_copy = dup(fd)) < 0) {
		log_sys_error("dup", name);
		return 0;
	}

	if (fclose(stream))
		log_sys_error("fclose", name);

	if ((new_fd = dup2(fd_copy, fd)) < 0)
		log_sys_error("dup2", name);
	else if (new_fd != fd)
		log_error("dup2(%d, %d) returned %d", fd_copy, fd, new_fd);

	if (close(fd_copy) < 0)
		log_sys_error("close", name);

	if (!(*new_stream = fdopen(fd, mode))) {
		log_sys_error("fdopen", name);
		return 0;
	}

	return 1;
}

/* Entry point */
struct cmd_context *create_toolcontext(unsigned is_long_lived,
				       const char *system_dir,
				       unsigned set_buffering,
				       unsigned threaded)
{
	struct cmd_context *cmd;
	FILE *new_stream;

#ifdef M_MMAP_MAX
	mallopt(M_MMAP_MAX, 0);
#endif

	if (!setlocale(LC_ALL, ""))
		log_very_verbose("setlocale failed");

#ifdef INTL_PACKAGE
	bindtextdomain(INTL_PACKAGE, LOCALEDIR);
#endif

	init_syslog(DEFAULT_LOG_FACILITY);

	if (!(cmd = dm_zalloc(sizeof(*cmd)))) {
		log_error("Failed to allocate command context");
		return NULL;
	}
	cmd->is_long_lived = is_long_lived;
	cmd->threaded = threaded ? 1 : 0;
	cmd->handles_missing_pvs = 0;
	cmd->handles_unknown_segments = 0;
	cmd->independent_metadata_areas = 0;
	cmd->hosttags = 0;
	dm_list_init(&cmd->arg_value_groups);
	dm_list_init(&cmd->formats);
	dm_list_init(&cmd->segtypes);
	dm_list_init(&cmd->tags);
	dm_list_init(&cmd->config_files);
	label_init();

	/* FIXME Make this configurable? */
	reset_lvm_errno(1);

	/* Set in/out stream buffering before glibc */
	if (set_buffering) {
		/* Allocate 2 buffers */
		if (!(cmd->linebuffer = dm_malloc(2 * linebuffer_size))) {
			log_error("Failed to allocate line buffer.");
			goto out;
		}

		if (!_reopen_stream(stdin, STDIN_FILENO, "r", "stdin", &new_stream))
			goto_out;
		stdin = new_stream;
		if (setvbuf(stdin, cmd->linebuffer, _IOLBF, linebuffer_size)) {
			log_sys_error("setvbuf", "");
			goto out;
		}

		if (!_reopen_stream(stdout, STDOUT_FILENO, "w", "stdout", &new_stream))
			goto_out;
		stdout = new_stream;
		if (setvbuf(stdout, cmd->linebuffer + linebuffer_size,
			     _IOLBF, linebuffer_size)) {
			log_sys_error("setvbuf", "");
			goto out;
		}
		/* Buffers are used for lines without '\n' */
	} else
		/* Without buffering, must not use stdin/stdout */
		init_silent(1);

	/*
	 * Environment variable LVM_SYSTEM_DIR overrides this below.
	 */
        if (system_dir)
		strncpy(cmd->system_dir, system_dir, sizeof(cmd->system_dir) - 1);
	else
		strcpy(cmd->system_dir, DEFAULT_SYS_DIR);

	if (!_get_env_vars(cmd))
		goto_out;

	/* Create system directory if it doesn't already exist */
	if (*cmd->system_dir && !dm_create_dir(cmd->system_dir)) {
		log_error("Failed to create LVM2 system dir for metadata backups, config "
			  "files and internal cache.");
		log_error("Set environment variable LVM_SYSTEM_DIR to alternative location "
			  "or empty string.");
		goto out;
	}

	if (!(cmd->libmem = dm_pool_create("library", 4 * 1024))) {
		log_error("Library memory pool creation failed");
		goto out;
	}

	if (!_init_lvm_conf(cmd))
		goto_out;

	_init_logging(cmd);

	if (!_init_hostname(cmd))
		goto_out;

	if (!_init_tags(cmd, cmd->cft))
		goto_out;

	if (!_init_tag_configs(cmd))
		goto_out;

	if (!(cmd->cft = _merge_config_files(cmd, cmd->cft)))
		goto_out;

	if (!_process_config(cmd))
		goto_out;

	if (!_init_dev_cache(cmd))
		goto_out;

	if (!_init_filters(cmd, 1))
		goto_out;

	if (!(cmd->mem = dm_pool_create("command", 4 * 1024))) {
		log_error("Command memory pool creation failed");
		goto out;
	}

	memlock_init(cmd);

	if (!_init_formats(cmd))
		goto_out;

	if (!init_lvmcache_orphans(cmd))
		goto_out;

	if (!_init_segtypes(cmd))
		goto_out;

	if (!_init_backup(cmd))
		goto_out;

	_init_rand(cmd);

	_init_globals(cmd);

	cmd->default_settings.cache_vgmetadata = 1;
	cmd->current_settings = cmd->default_settings;

	cmd->config_valid = 1;
out:
	if (cmd->config_valid != 1) {
		destroy_toolcontext(cmd);
		cmd = NULL;
	}

	return cmd;
}

static void _destroy_formats(struct cmd_context *cmd, struct dm_list *formats)
{
	struct dm_list *fmtl, *tmp;
	struct format_type *fmt;
	void *lib;

	dm_list_iterate_safe(fmtl, tmp, formats) {
		fmt = dm_list_item(fmtl, struct format_type);
		dm_list_del(&fmt->list);
		lib = fmt->library;
		fmt->ops->destroy(fmt);
#ifdef HAVE_LIBDL
		if (lib)
			dlclose(lib);
#endif
	}

	cmd->independent_metadata_areas = 0;
}

static void _destroy_segtypes(struct dm_list *segtypes)
{
	struct dm_list *sgtl, *tmp;
	struct segment_type *segtype;
	void *lib;

	dm_list_iterate_safe(sgtl, tmp, segtypes) {
		segtype = dm_list_item(sgtl, struct segment_type);
		dm_list_del(&segtype->list);
		lib = segtype->library;
		segtype->ops->destroy(segtype);
#ifdef HAVE_LIBDL
		/*
		 * If no segtypes remain from this library, close it.
		 */
		if (lib) {
			struct segment_type *segtype2;
			dm_list_iterate_items(segtype2, segtypes)
				if (segtype2->library == lib)
					goto skip_dlclose;
			dlclose(lib);
skip_dlclose:
			;
		}
#endif
	}
}

int refresh_filters(struct cmd_context *cmd)
{
	int r, saved_ignore_suspended_devices = ignore_suspended_devices();

	if (cmd->filter) {
		cmd->filter->destroy(cmd->filter);
		cmd->filter = NULL;
	}

	if (!(r = _init_filters(cmd, 0)))
                stack;

	/*
	 * During repair code must not reset suspended flag.
	 */
	init_ignore_suspended_devices(saved_ignore_suspended_devices);

	return r;
}

int refresh_toolcontext(struct cmd_context *cmd)
{
	struct dm_config_tree *cft_cmdline, *cft_tmp;

	log_verbose("Reloading config files");

	/*
	 * Don't update the persistent filter cache as we will
	 * perform a full rescan.
	 */

	activation_release();
	lvmcache_destroy(cmd, 0);
	label_exit();
	_destroy_segtypes(&cmd->segtypes);
	_destroy_formats(cmd, &cmd->formats);
	if (cmd->filter) {
		cmd->filter->destroy(cmd->filter);
		cmd->filter = NULL;
	}
	dev_cache_exit();
	_destroy_tags(cmd);

	cft_cmdline = _destroy_tag_configs(cmd);

	cmd->config_valid = 0;

	cmd->hosttags = 0;

	if (!_init_lvm_conf(cmd))
		return 0;

	/* Temporary duplicate cft pointer holding lvm.conf - replaced later */
	cft_tmp = cmd->cft;
	if (cft_cmdline)
		cmd->cft = dm_config_insert_cascaded_tree(cft_cmdline, cft_tmp);

	/* Uses cmd->cft i.e. cft_cmdline + lvm.conf */
	_init_logging(cmd);

	/* Init tags from lvm.conf. */
	if (!_init_tags(cmd, cft_tmp))
		return 0;

	/* Doesn't change cmd->cft */
	if (!_init_tag_configs(cmd))
		return 0;

	/* Merge all the tag config files with lvm.conf, returning a
	 * fresh cft pointer in place of cft_tmp. */
	if (!(cmd->cft = _merge_config_files(cmd, cft_tmp)))
		return 0;

	/* Finally we can make the proper, fully-merged, cmd->cft */
	if (cft_cmdline)
		cmd->cft = dm_config_insert_cascaded_tree(cft_cmdline, cmd->cft);

	if (!_process_config(cmd))
		return 0;

	if (!_init_dev_cache(cmd))
		return 0;

	if (!_init_filters(cmd, 0))
		return 0;

	if (!_init_formats(cmd))
		return 0;

	if (!init_lvmcache_orphans(cmd))
		return 0;

	if (!_init_segtypes(cmd))
		return 0;

	if (!_init_backup(cmd))
		return 0;

	cmd->config_valid = 1;

	reset_lvm_errno(1);
	return 1;
}

void destroy_toolcontext(struct cmd_context *cmd)
{
	struct dm_config_tree *cft_cmdline;
	FILE *new_stream;

	if (cmd->dump_filter)
		persistent_filter_dump(cmd->filter, 1);

	archive_exit(cmd);
	backup_exit(cmd);
	lvmcache_destroy(cmd, 0);
	label_exit();
	_destroy_segtypes(&cmd->segtypes);
	_destroy_formats(cmd, &cmd->formats);
	if (cmd->filter)
		cmd->filter->destroy(cmd->filter);
	if (cmd->mem)
		dm_pool_destroy(cmd->mem);
	dev_cache_exit();
	_destroy_tags(cmd);

	if ((cft_cmdline = _destroy_tag_configs(cmd)))
		dm_config_destroy(cft_cmdline);
	if (cmd->libmem)
		dm_pool_destroy(cmd->libmem);

	if (cmd->linebuffer) {
		/* Reset stream buffering to defaults */
		if (_reopen_stream(stdin, STDIN_FILENO, "r", "stdin", &new_stream)) {
			stdin = new_stream;
			setlinebuf(stdin);
		} else
			cmd->linebuffer = NULL;	/* Leave buffer in place (deliberate leak) */

		if (_reopen_stream(stdout, STDOUT_FILENO, "w", "stdout", &new_stream)) {
			stdout = new_stream;
			setlinebuf(stdout);
		} else
			cmd->linebuffer = NULL;	/* Leave buffer in place (deliberate leak) */

		dm_free(cmd->linebuffer);
	}

	dm_free(cmd);

	release_log_memory();
	activation_exit();
	reset_log_duplicated();
	fin_log();
	fin_syslog();
	reset_lvm_errno(0);
}
