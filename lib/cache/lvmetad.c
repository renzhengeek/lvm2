/*
 * Copyright (C) 2012 Red Hat, Inc.
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
#include "device.h"
#include "lvmetad.h"
#include "lvmcache.h"
#include "lvmetad-client.h"
#include "format-text.h" // TODO for disk_locn, used as a DA representation
#include "crc.h"
#include "lvm-signal.h"
#include "lvmlockd.h"

#include <time.h>

#define SCAN_TIMEOUT_SECONDS	80
#define MAX_RESCANS		10	/* Maximum number of times to scan all PVs and retry if the daemon returns a token mismatch error */

static daemon_handle _lvmetad = { .error = 0 };
static int _lvmetad_use = 0;
static int _lvmetad_connected = 0;

static char *_lvmetad_token = NULL;
static const char *_lvmetad_socket = NULL;
static struct cmd_context *_lvmetad_cmd = NULL;

static struct volume_group *lvmetad_pvscan_vg(struct cmd_context *cmd, struct volume_group *vg);

static uint64_t _monotonic_seconds(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return 0;
	return ts.tv_sec;
}

static int _log_debug_inequality(const char *name, struct dm_config_node *a, struct dm_config_node *b)
{
	int result = 0;
	int final_result = 0;

	if (a->v && b->v) {
		result = compare_value(a->v, b->v);
		if (result) {
			struct dm_config_value *av = a->v;
			struct dm_config_value *bv = b->v;

			if (!strcmp(a->key, b->key)) {
				if (a->v->type == DM_CFG_STRING && b->v->type == DM_CFG_STRING)
					log_debug_lvmetad("VG %s metadata inequality at %s / %s: %s / %s",
							  name, a->key, b->key, av->v.str, bv->v.str);
				else if (a->v->type == DM_CFG_INT && b->v->type == DM_CFG_INT)
					log_debug_lvmetad("VG %s metadata inequality at %s / %s: %li / %li",
							  name, a->key, b->key, (int64_t)av->v.i, (int64_t)bv->v.i);
				else
					log_debug_lvmetad("VG %s metadata inequality at %s / %s: type %d / type %d",
							  name, a->key, b->key, av->type, bv->type);
			} else {
				log_debug_lvmetad("VG %s metadata inequality at %s / %s", name, a->key, b->key);
			}
			final_result = result;
		}
	}

	if (a->v && !b->v) {
		log_debug_lvmetad("VG %s metadata inequality at %s / %s", name, a->key, b->key);
		final_result = 1;
	}

	if (!a->v && b->v) {
		log_debug_lvmetad("VG %s metadata inequality at %s / %s", name, a->key, b->key);
		final_result = -1;
	}

	if (a->child && b->child) {
		result = _log_debug_inequality(name, a->child, b->child);
		if (result)
			final_result = result;
	}

	if (a->sib && b->sib) {
		result = _log_debug_inequality(name, a->sib, b->sib);
		if (result)
			final_result = result;
	}
	

	if (a->sib && !b->sib) {
		log_debug_lvmetad("VG %s metadata inequality at %s / %s", name, a->key, b->key);
		final_result = 1;
	}

	if (!a->sib && b->sib) {
		log_debug_lvmetad("VG %s metadata inequality at %s / %s", name, a->key, b->key);
		final_result = -1;
	}

	return final_result;
}

void lvmetad_disconnect(void)
{
	if (_lvmetad_connected)
		daemon_close(_lvmetad);
	_lvmetad_connected = 0;
}

void lvmetad_init(struct cmd_context *cmd)
{
	if (!_lvmetad_use && !access(getenv("LVM_LVMETAD_PIDFILE") ? : LVMETAD_PIDFILE, F_OK))
		log_warn("WARNING: lvmetad is running but disabled."
			 " Restart lvmetad before enabling it!");
	_lvmetad_cmd = cmd;
}

static void _lvmetad_connect(void)
{
	if (!_lvmetad_use || !_lvmetad_socket || _lvmetad_connected)
		return;

	_lvmetad = lvmetad_open(_lvmetad_socket);
	if (_lvmetad.socket_fd >= 0 && !_lvmetad.error) {
		log_debug_lvmetad("Successfully connected to lvmetad on fd %d.",
				  _lvmetad.socket_fd);
		_lvmetad_connected = 1;
	}

	if (!_lvmetad_connected)
		_lvmetad_use = 0;
}

void lvmetad_connect_or_warn(void)
{
	if (!_lvmetad_use)
		return;

	if (!_lvmetad_connected && !_lvmetad.error) {
		_lvmetad_connect();

		if ((_lvmetad.socket_fd < 0 || _lvmetad.error))
			log_warn("WARNING: Failed to connect to lvmetad. Falling back to internal scanning.");
	}

	if (!_lvmetad_connected)
		_lvmetad_use = 0;
}

int lvmetad_used(void)
{
	return _lvmetad_use;
}

int lvmetad_socket_present(void)
{
	const char *socket = _lvmetad_socket ?: LVMETAD_SOCKET;
	int r;

	if ((r = access(socket, F_OK)) && errno != ENOENT)
		log_sys_error("lvmetad_socket_present", "");

	return !r;
}

int lvmetad_active(void)
{
	lvmetad_connect_or_warn();
	return _lvmetad_connected;
}

void lvmetad_set_active(struct cmd_context *cmd, int active)
{
	_lvmetad_use = active;
	if (!active && lvmetad_active())
		lvmetad_disconnect();
	if (cmd && !refresh_filters(cmd))
		stack;
}

/*
 * Use a crc of the strings in the filter as the lvmetad token.
 */
void lvmetad_set_token(const struct dm_config_value *filter)
{
	int ft = 0;

	dm_free(_lvmetad_token);

	while (filter && filter->type == DM_CFG_STRING) {
		ft = calc_crc(ft, (const uint8_t *) filter->v.str, strlen(filter->v.str));
		filter = filter->next;
	}

	if (dm_asprintf(&_lvmetad_token, "filter:%u", ft) < 0)
		log_warn("WARNING: Failed to set lvmetad token. Out of memory?");
}

void lvmetad_release_token(void)
{
	dm_free(_lvmetad_token);
	_lvmetad_token = NULL;
}

void lvmetad_set_socket(const char *sock)
{
	_lvmetad_socket = sock;
}

/*
 * Check if lvmetad's token matches our token.  The token is a hash of the
 * global filter used to populate lvmetad.  The lvmetad token was set by the
 * last command to populate lvmetad, and it was set to the hash of the global
 * filter that command used when scanning to populate lvmetad.
 *
 * Our token is a hash of the global filter this command is using.
 *
 * If the lvmetad token is not set (or "none"), then lvmetad has not been
 * populated.  If the lvmetad token is "update in progress", then lvmetad is
 * currently being populated -- this should be temporary, so wait for a while
 * for the current update to finish and then compare our token with the new one
 * (hopefully it will match).  If the lvmetad token otherwise differs from
 * ours, then lvmetad was populated using a different global filter that we are
 * using.
 *
 * Return 1 if the lvmetad token matches ours.  We can use it as is.
 *
 * Return 0 if the lvmetad token does not match ours (lvmetad is empty or
 * populated using a different global filter).  The caller will repopulate
 * lvmetad (via lvmetad_pvscan_all_devs) before using lvmetad.
 *
 * If we time out waiting for an lvmetad update to finish, then disable this
 * command's use of lvmetad and return 0.
 */

int lvmetad_token_matches(struct cmd_context *cmd)
{
	daemon_reply reply;
	const char *daemon_token;
	unsigned int delay_usec = 0;
	unsigned int wait_sec = 0;
	uint64_t now = 0, wait_start = 0;
	int ret = 1;

	wait_sec = (unsigned int)find_config_tree_int(cmd, global_lvmetad_update_wait_time_CFG, NULL);

retry:
	log_debug_lvmetad("lvmetad send get_global_info");

	reply = daemon_send_simple(_lvmetad, "get_global_info",
				   "token = %s", "skip",
				   NULL);
	if (reply.error) {
		log_warn("WARNING: Not using lvmetad after send error (%d).", reply.error);
		goto fail;
	}

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK")) {
		log_warn("WARNING: Not using lvmetad after response error.");
		goto fail;
	}

	if (!(daemon_token = daemon_reply_str(reply, "token", NULL))) {
		log_warn("WARNING: Not using lvmetad with older version."); 
		goto fail;
	}

	/*
	 * If lvmetad is being updated by another command, then sleep and retry
	 * until the token shows the update is done, and go on to the token
	 * comparison.
	 *
	 * Between retries, sleep for a random period between 1 and 2 seconds.
	 * Retry in this way for up to a configurable period of time.
	 *
	 * If lvmetad is still being updated after the timeout period,
	 * then disable this command's use of lvmetad.
	 *
	 * (lvmetad could return the number of objects in its cache along with
	 * the update message so that callers could detect when a rescan has
	 * stalled while updating lvmetad.)
	 */
	if (!strcmp(daemon_token, "update in progress")) {
		if (!(now = _monotonic_seconds()))
			goto fail;

		if (!wait_start)
			wait_start = now;

		if (now - wait_start >= wait_sec) {
			log_warn("WARNING: Not using lvmetad after %u sec lvmetad_update_wait_time.", wait_sec);
			goto fail;
		}

		log_warn("WARNING: lvmetad is being updated, retrying (setup) for %u more seconds.",
			 wait_sec - (unsigned int)(now - wait_start));

		/* Delay a random period between 1 and 2 seconds. */
		delay_usec = 1000000 + lvm_even_rand(&_lvmetad_cmd->rand_seed, 1000000);
		usleep(delay_usec);
		daemon_reply_destroy(reply);
		goto retry;
	}

	/*
	 * lvmetad is empty, not yet populated.
	 * The caller should do a disk scan to populate lvmetad.
	 */
	if (!strcmp(daemon_token, "none")) {
		ret = 0;
		goto out;
	}

	/*
	 * lvmetad has an unmatching token; it was last populated using
	 * a different global filter.
	 * The caller should do a disk scan to populate lvmetad with
	 * our global filter.
	 */
	if (strcmp(daemon_token, _lvmetad_token)) {
		ret = 0;
		goto out;
	}

out:
	daemon_reply_destroy(reply);
	return ret;

fail:
	daemon_reply_destroy(reply);
	/* The command will not use lvmetad and will revert to scanning. */
	lvmetad_set_active(cmd, 0);
	return 0;
}

/*
 * Wait up to lvmetad_update_wait_time for the lvmetad updating state to be
 * finished.
 *
 * Return 0 if lvmetad is not updating or there's an error and we can't tell.
 * Return 1 if lvmetad is updating.
 */
static int _lvmetad_is_updating(struct cmd_context *cmd, int do_wait)
{
	daemon_reply reply;
	const char *daemon_token;
	unsigned int wait_sec = 0;
	uint64_t now = 0, wait_start = 0;
	int ret = 0;

	wait_sec = (unsigned int)find_config_tree_int(cmd, global_lvmetad_update_wait_time_CFG, NULL);
retry:
	log_debug_lvmetad("lvmetad send get_global_info");

	reply = daemon_send_simple(_lvmetad, "get_global_info",
				   "token = %s", "skip",
				   NULL);
	if (reply.error)
		goto out;

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK"))
		goto out;

	if (!(daemon_token = daemon_reply_str(reply, "token", NULL)))
		goto out;

	if (!strcmp(daemon_token, "update in progress")) {
		ret = 1;

		if (!do_wait)
			goto out;

		if (!(now = _monotonic_seconds()))
			goto out;

		if (!wait_start)
			wait_start = now;

		if (now - wait_start >= wait_sec)
			goto out;

		log_warn("WARNING: lvmetad is being updated, waiting for %u more seconds.",
			 wait_sec - (unsigned int)(now - wait_start));

		usleep(1000000);
		daemon_reply_destroy(reply);
		goto retry;
	} else {
		ret = 0;
	}

out:
	daemon_reply_destroy(reply);
	return ret;
}

static int _lvmetad_pvscan_all_devs(struct cmd_context *cmd, activation_handler handler,
				    int ignore_obsolete, int do_wait);

static daemon_reply _lvmetad_send(struct cmd_context *cmd, const char *id, ...)
{
	va_list ap;
	daemon_reply reply = { 0 };
	daemon_request req;
	unsigned int delay_usec;
	unsigned int wait_sec = 0;
	uint64_t now = 0, wait_start = 0;

	if (cmd)
		wait_sec = (unsigned int)find_config_tree_int(cmd, global_lvmetad_update_wait_time_CFG, NULL);
retry:
	log_debug_lvmetad("lvmetad_send %s", id);

	req = daemon_request_make(id);

	if (_lvmetad_token && !daemon_request_extend(req, "token = %s", _lvmetad_token, NULL)) {
		reply.error = ENOMEM;
		return reply;
	}

	va_start(ap, id);
	daemon_request_extend_v(req, ap);
	va_end(ap);

	reply = daemon_send(_lvmetad, req);

	daemon_request_destroy(req);

	if (reply.error)
		goto out;

	if (!strcmp(daemon_reply_str(reply, "response", ""), "token_mismatch")) {
		if (!strcmp(daemon_reply_str(reply, "expected", ""), "update in progress")) {
			/*
			 * Another command is updating the lvmetad cache, and
			 * we cannot use lvmetad until the update is finished.
			 * Retry our request for a while; the update should
			 * finish shortly.  This should not usually happen
			 * because this command already checked that the token
			 * is usable in lvmetad_token_matches(), but it's
			 * possible for another command's rescan to slip in
			 * between the time we call lvmetad_token_matches()
			 * and the time we get here to lvmetad_send().
			 */
			if (!(now = _monotonic_seconds()))
				goto out;

			if (!wait_start)
				wait_start = now;

			if (!wait_sec || (now - wait_start >= wait_sec)) {
				log_warn("WARNING: Cannot use lvmetad after %u sec lvmetad_update_wait_time.", wait_sec);
				goto out;
			}

			log_warn("WARNING: lvmetad is being updated, retrying (%s) for %u more seconds.",
				 id, wait_sec - (unsigned int)(now - wait_start));

			/* Delay a random period between 1 and 2 seconds. */
			delay_usec = 1000000 + lvm_even_rand(&_lvmetad_cmd->rand_seed, 1000000);
			usleep(delay_usec);
			daemon_reply_destroy(reply);
			goto retry;
		} else {
			/*
			 * Another command has updated the lvmetad cache, and
			 * has done so using a different device filter from our
			 * own, which has made the lvmetad token and our token
			 * not match.  This should not usually happen because
			 * this command has already checked for a matching token
			 * in lvmetad_token_matches(), but it's possible for
			 * another command's rescan to slip in between the time
			 * we call lvmetad_token_matches() and the time we get
			 * here to lvmetad_send().  With a mismatched token
			 * (different set of devices), we cannot use the lvmetad
			 * cache.
			 *
			 * FIXME: it would be nice to have this command ignore
			 * lvmetad at this point and revert to disk scanning,
			 * but the layers above lvmetad_send are not yet able
			 * to switch modes in the middle of processing.
			 *
			 * (The advantage of lvmetad_check_token is that it
			 * can rescan to get the token in sync, or if that
			 * fails it can make the command revert to scanning
			 * from the start.)
			 */
			log_warn("WARNING: Cannot use lvmetad while it caches different devices.");
		}
	}
out:
	return reply;
}

static int _token_update(int *replaced_update)
{
	daemon_reply reply;
	const char *prev_token;

	log_debug_lvmetad("Sending updated token to lvmetad: %s", _lvmetad_token ? : "<NONE>");
	reply = _lvmetad_send(NULL, "token_update", NULL);

	if (replaced_update)
		*replaced_update = 0;

	if (reply.error || strcmp(daemon_reply_str(reply, "response", ""), "OK")) {
		daemon_reply_destroy(reply);
		return 0;
	}

	if ((prev_token = daemon_reply_str(reply, "prev_token", NULL))) {
		if (!strcmp(prev_token, "update in progress"))
			if (replaced_update)
				*replaced_update = 1;
	}

	daemon_reply_destroy(reply);

	return 1;
}

/*
 * Helper; evaluate the reply from lvmetad, check for errors, print diagnostics
 * and return a summary success/failure exit code.
 *
 * If found is set, *found indicates whether or not device exists,
 * and missing device is not treated as an error.
 */
static int _lvmetad_handle_reply(daemon_reply reply, const char *id, const char *object, int *found)
{
	int action_modifies = 0;
	const char *action;

	if (!id)
		action = "<none>";
	else if (!strcmp(id, "pv_list"))
		action = "list PVs";
	else if (!strcmp(id, "vg_list"))
		action = "list VGs";
	else if (!strcmp(id, "vg_lookup"))
		action = "lookup VG";
	else if (!strcmp(id, "pv_lookup"))
		action = "lookup PV";
	else if (!strcmp(id, "pv_clear_all"))
		action = "clear info about all PVs";
	else if (!strcmp(id, "vg_clear_outdated_pvs"))
		action = "clear the list of outdated PVs";
	else if (!strcmp(id, "vg_update")) {
		action = "update VG";
		action_modifies = 1;
	} else if (!strcmp(id, "vg_remove")) {
		action = "remove VG";
		action_modifies = 1;
	} else if (!strcmp(id, "pv_found")) {
		action = "update PV";
		action_modifies = 1;
	} else if (!strcmp(id, "pv_gone")) {
		action = "drop PV";
		action_modifies = 1;
	} else {
		log_error(INTERNAL_ERROR "Unchecked lvmetad message %s.", id);
		action = "action unknown";
	}

	if (reply.error) {
		log_error("Request to %s %s%sin lvmetad gave response %s.",
			  action, object, *object ? " " : "", strerror(reply.error));
		goto fail;
	}

	/*
	 * See the description of the token mismatch errors in lvmetad_send.
	 */
	if (!strcmp(daemon_reply_str(reply, "response", ""), "token_mismatch")) {
		if (!strcmp(daemon_reply_str(reply, "expected", ""), "update in progress")) {
			/*
			 * lvmetad_send retried up to the limit and eventually
			 * printed a warning and gave up.
			 */
			log_error("Request to %s %s%sin lvmetad failed after lvmetad_update_wait_time expired.",
				  action, object, *object ? " " : "");
		} else {
			/*
			 * lvmetad is caching different devices based on a different
			 * device filter which causes a token mismatch.
			 */
			log_error("Request to %s %s%sin lvmetad failed after device filter mismatch.",
				  action, object, *object ? " " : "");
		}
		goto fail;
	}

	/* All OK? */
	if (!strcmp(daemon_reply_str(reply, "response", ""), "OK")) {
		if (found)
			*found = 1;
		return 1;
	}

	/* Unknown device permitted? */
	if (found && !strcmp(daemon_reply_str(reply, "response", ""), "unknown")) {
		log_very_verbose("Request to %s %s%sin lvmetad did not find any matching object.",
				 action, object, *object ? " " : "");
		*found = 0;
		return 1;
	}

	/* Multiple VGs with the same name were found. */
	if (found && !strcmp(daemon_reply_str(reply, "response", ""), "multiple")) {
		log_very_verbose("Request to %s %s%sin lvmetad found multiple matching objects.",
				 action, object, *object ? " " : "");
		if (found)
			*found = 2;
		return 1;
	}

	/*
	 * Generic error message for error cases not specifically checked above.
	 */
	log_error("Request to %s %s%sin lvmetad gave response %s. Reason: %s",
		  action, object, *object ? " " : "", 
		  daemon_reply_str(reply, "response", "<missing>"),
		  daemon_reply_str(reply, "reason", "<missing>"));
fail:
	/*
	 * If the failed lvmetad message was updating lvmetad, it is important
	 * to restart lvmetad (or at least rescan.)
	 *
	 * FIXME: attempt to set the disabled state in lvmetad here so that
	 * commands will not use it until it's been properly repopulated.
	 */
	if (action_modifies)
		log_error("lvmetad update failed.  Restart lvmetad immediately.");

	return 0;
}

static int _read_mda(struct lvmcache_info *info,
		     struct format_type *fmt,
		     const struct dm_config_node *cn)
{
	struct metadata_area_ops *ops;

	dm_list_iterate_items(ops, &fmt->mda_ops)
		if (ops->mda_import_text && ops->mda_import_text(info, cn))
			return 1;

	return 0;
}

static int _pv_populate_lvmcache(struct cmd_context *cmd,
				 struct dm_config_node *cn,
				 struct format_type *fmt, dev_t fallback)
{
	struct device *dev, *dev_alternate, *dev_alternate_cache = NULL;
	struct label *label;
	struct id pvid, vgid;
	char mda_id[32];
	char da_id[32];
	int i = 0;
	struct dm_config_node *mda, *da;
	struct dm_config_node *alt_devices = dm_config_find_node(cn->child, "devices_alternate");
	struct dm_config_value *alt_device = NULL;
	uint64_t offset, size;
	struct lvmcache_info *info, *info_alternate;
	const char *pvid_txt = dm_config_find_str(cn->child, "id", NULL),
		   *vgid_txt = dm_config_find_str(cn->child, "vgid", NULL),
		   *vgname = dm_config_find_str(cn->child, "vgname", NULL),
		   *fmt_name = dm_config_find_str(cn->child, "format", NULL);
	dev_t devt = dm_config_find_int(cn->child, "device", 0);
	uint64_t devsize = dm_config_find_int64(cn->child, "dev_size", 0),
		 label_sector = dm_config_find_int64(cn->child, "label_sector", 0);

	if (!fmt && fmt_name)
		fmt = get_format_by_name(cmd, fmt_name);

	if (!fmt) {
		log_error("PV %s not recognised. Is the device missing?", pvid_txt);
		return 0;
	}

	dev = dev_cache_get_by_devt(devt, cmd->filter);
	if (!dev && fallback)
		dev = dev_cache_get_by_devt(fallback, cmd->filter);

	if (!dev) {
		log_warn("WARNING: Device for PV %s not found or rejected by a filter.", pvid_txt);
		return 0;
	}

	if (!pvid_txt || !id_read_format(&pvid, pvid_txt)) {
		log_error("Missing or ill-formatted PVID for PV: %s.", pvid_txt);
		return 0;
	}

	if (vgid_txt) {
		if (!id_read_format(&vgid, vgid_txt))
			return_0;
	} else
		strcpy((char*)&vgid, fmt->orphan_vg_name);

	if (!vgname)
		vgname = fmt->orphan_vg_name;

	if (!(info = lvmcache_add(fmt->labeller, (const char *)&pvid, dev,
				  vgname, (const char *)&vgid, 0)))
		return_0;

	lvmcache_get_label(info)->sector = label_sector;
	lvmcache_get_label(info)->dev = dev;
	lvmcache_set_device_size(info, devsize);
	lvmcache_del_das(info);
	lvmcache_del_mdas(info);
	lvmcache_del_bas(info);

	do {
		sprintf(mda_id, "mda%d", i);
		mda = dm_config_find_node(cn->child, mda_id);
		if (mda)
			_read_mda(info, fmt, mda);
		++i;
	} while (mda);

	i = 0;
	do {
		sprintf(da_id, "da%d", i);
		da = dm_config_find_node(cn->child, da_id);
		if (da) {
			if (!dm_config_get_uint64(da->child, "offset", &offset)) return_0;
			if (!dm_config_get_uint64(da->child, "size", &size)) return_0;
			lvmcache_add_da(info, offset, size);
		}
		++i;
	} while (da);

	i = 0;
	do {
		sprintf(da_id, "ba%d", i);
		da = dm_config_find_node(cn->child, da_id);
		if (da) {
			if (!dm_config_get_uint64(da->child, "offset", &offset)) return_0;
			if (!dm_config_get_uint64(da->child, "size", &size)) return_0;
			lvmcache_add_ba(info, offset, size);
		}
		++i;
	} while (da);

	if (alt_devices)
		alt_device = alt_devices->v;

	while (alt_device) {
		dev_alternate = dev_cache_get_by_devt(alt_device->v.i, cmd->filter);

		log_verbose("PV on device %s (%d:%d %d) is also on device %s (%d:%d %d) %s",
			    dev_name(dev),
			    (int)MAJOR(devt), (int)MINOR(devt), (int)devt,
			    dev_alternate ? dev_name(dev_alternate) : "unknown",
			    (int)MAJOR(alt_device->v.i), (int)MINOR(alt_device->v.i), (int)alt_device->v.i,
			    pvid_txt);

		if (dev_alternate) {
			if ((info_alternate = lvmcache_add(fmt->labeller, (const char *)&pvid, dev_alternate,
							   vgname, (const char *)&vgid, 0))) {
				dev_alternate_cache = dev_alternate;
				info = info_alternate;
				lvmcache_get_label(info)->dev = dev_alternate;
			}
		}
		alt_device = alt_device->next;
	}

	/*
	 * Update lvmcache with the info about the alternate device by
	 * reading its label, which should update lvmcache.
	 */
	if (dev_alternate_cache) {
		if (!label_read(dev_alternate_cache, &label, 0)) {
			log_warn("No PV label found on duplicate device %s.", dev_name(dev_alternate_cache));
		}
	}

	lvmcache_set_preferred_duplicates((const char *)&vgid);
	return 1;
}

struct volume_group *lvmetad_vg_lookup(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	struct volume_group *vg = NULL;
	struct volume_group *vg2 = NULL;
	daemon_reply reply;
	int found;
	char uuid[64];
	struct format_instance *fid = NULL;
	struct format_instance_ctx fic;
	struct dm_config_node *top;
	const char *name, *diag_name;
	const char *fmt_name;
	struct format_type *fmt;
	struct dm_config_node *pvcn;
	struct pv_list *pvl;
	struct lvmcache_info *info;

	if (!lvmetad_active())
		return NULL;

	if (vgid) {
		if (!id_write_format((const struct id*)vgid, uuid, sizeof(uuid)))
			return_NULL;
	}

	if (vgid && vgname) {
		log_debug_lvmetad("Asking lvmetad for VG %s %s", uuid, vgname);
		reply = _lvmetad_send(cmd, "vg_lookup",
				      "uuid = %s", uuid,
				      "name = %s", vgname,
				      NULL);
		diag_name = uuid;

	} else if (vgid) {
		log_debug_lvmetad("Asking lvmetad for VG vgid %s", uuid);
		reply = _lvmetad_send(cmd, "vg_lookup", "uuid = %s", uuid, NULL);
		diag_name = uuid;

	} else if (vgname) {
		log_debug_lvmetad("Asking lvmetad for VG %s", vgname);
		reply = _lvmetad_send(cmd, "vg_lookup", "name = %s", vgname, NULL);
		diag_name = vgname;

	} else {
		log_error(INTERNAL_ERROR "VG name required (VGID not available)");
		goto out;
	}

	if (_lvmetad_handle_reply(reply, "vg_lookup", diag_name, &found) && found) {

		if ((found == 2) && vgname) {
			log_error("Multiple VGs found with the same name: %s.", vgname);
			log_error("See the --select option with VG UUID (vg_uuid).");
			goto out;
		}

		if (!(top = dm_config_find_node(reply.cft->root, "metadata"))) {
			log_error(INTERNAL_ERROR "metadata config node not found.");
			goto out;
		}

		name = daemon_reply_str(reply, "name", NULL);

		/* fall back to lvm2 if we don't know better */
		fmt_name = dm_config_find_str(top, "metadata/format", "lvm2");
		if (!(fmt = get_format_by_name(cmd, fmt_name))) {
			log_error(INTERNAL_ERROR
				  "We do not know the format (%s) reported by lvmetad.",
				  fmt_name);
			goto out;
		}

		fic.type = FMT_INSTANCE_MDAS | FMT_INSTANCE_AUX_MDAS;
		fic.context.vg_ref.vg_name = name;
		fic.context.vg_ref.vg_id = vgid;

		if (!(fid = fmt->ops->create_instance(fmt, &fic)))
			goto_out;

		if ((pvcn = dm_config_find_node(top, "metadata/physical_volumes")))
			for (pvcn = pvcn->child; pvcn; pvcn = pvcn->sib)
				_pv_populate_lvmcache(cmd, pvcn, fmt, 0);

		top->key = name;
		if (!(vg = import_vg_from_config_tree(reply.cft, fid)))
			goto_out;

		/*
		 * locking may have detected a newer vg version and
		 * invalidated the cached vg.
		 */
		if (dm_config_find_node(reply.cft->root, "vg_invalid")) {
			log_debug_lvmetad("Update invalid lvmetad cache for VG %s", vgname);
			vg2 = lvmetad_pvscan_vg(cmd, vg);
			release_vg(vg);
			vg = vg2;
			fid = vg->fid;
		}

		dm_list_iterate_items(pvl, &vg->pvs) {
			if ((info = lvmcache_info_from_pvid((const char *)&pvl->pv->id, 0))) {
				pvl->pv->label_sector = lvmcache_get_label(info)->sector;
				pvl->pv->dev = lvmcache_device(info);
				if (!pvl->pv->dev)
					pvl->pv->status |= MISSING_PV;
				if (!lvmcache_fid_add_mdas_pv(info, fid)) {
					vg = NULL;
					goto_out;	/* FIXME error path */
				}
			} else
				pvl->pv->status |= MISSING_PV; /* probably missing */
		}

		lvmcache_update_vg(vg, 0);
		vg_mark_partial_lvs(vg, 1);
	}

out:
	if (!vg && fid)
		fid->fmt->ops->destroy_instance(fid);
	daemon_reply_destroy(reply);

	return vg;
}

struct _fixup_baton {
	int i;
	int find;
	int ignore;
};

static int _fixup_ignored(struct metadata_area *mda, void *baton) {
	struct _fixup_baton *b = baton;
	if (b->i == b->find)
		mda_set_ignored(mda, b->ignore);
	b->i ++;
	return 1;
}

int lvmetad_vg_update(struct volume_group *vg)
{
	daemon_reply reply;
	struct dm_hash_node *n;
	struct metadata_area *mda;
	char mda_id[128], *num;
	struct pv_list *pvl;
	struct lvmcache_info *info;
	struct _fixup_baton baton;

	if (!vg)
		return 0;

	if (!lvmetad_active() || test_mode())
		return 1; /* fake it */

	if (!vg->cft_precommitted) {
		log_error(INTERNAL_ERROR "VG update without precommited");
		return 0;
	}

	log_debug_lvmetad("Sending lvmetad updated metadata for VG %s (seqno %" PRIu32 ")", vg->name, vg->seqno);
	reply = _lvmetad_send(vg->cmd, "vg_update", "vgname = %s", vg->name,
			      "metadata = %t", vg->cft_precommitted, NULL);

	if (!_lvmetad_handle_reply(reply, "vg_update", vg->name, NULL)) {
		daemon_reply_destroy(reply);
		return 0;
	}

	daemon_reply_destroy(reply);

	n = (vg->fid && vg->fid->metadata_areas_index) ?
		dm_hash_get_first(vg->fid->metadata_areas_index) : NULL;
	while (n) {
		mda = dm_hash_get_data(vg->fid->metadata_areas_index, n);
		strcpy(mda_id, dm_hash_get_key(vg->fid->metadata_areas_index, n));
		if ((num = strchr(mda_id, '_'))) {
			*num = 0;
			++num;
			if ((info = lvmcache_info_from_pvid(mda_id, 0))) {
				memset(&baton, 0, sizeof(baton));
				baton.find = atoi(num);
				baton.ignore = mda_is_ignored(mda);
				lvmcache_foreach_mda(info, _fixup_ignored, &baton);
			}
		}
		n = dm_hash_get_next(vg->fid->metadata_areas_index, n);
	}

	dm_list_iterate_items(pvl, &vg->pvs) {
		/* NB. the PV fmt pointer is sometimes wrong during vgconvert */
		if (pvl->pv->dev && !lvmetad_pv_found(&pvl->pv->id, pvl->pv->dev,
						      vg->fid ? vg->fid->fmt : pvl->pv->fmt,
						      pvl->pv->label_sector, NULL, NULL))
			return 0;
	}

	return 1;
}

int lvmetad_vg_remove(struct volume_group *vg)
{
	char uuid[64];
	daemon_reply reply;
	int result;

	if (!lvmetad_active() || test_mode())
		return 1; /* just fake it */

	if (!id_write_format(&vg->id, uuid, sizeof(uuid)))
		return_0;

	log_debug_lvmetad("Telling lvmetad to remove VGID %s (%s)", uuid, vg->name);
	reply = _lvmetad_send(vg->cmd, "vg_remove", "uuid = %s", uuid, NULL);
	result = _lvmetad_handle_reply(reply, "vg_remove", vg->name, NULL);

	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_lookup(struct cmd_context *cmd, struct id pvid, int *found)
{
	char uuid[64];
	daemon_reply reply;
	int result = 0;
	struct dm_config_node *cn;

	if (!lvmetad_active())
		return_0;

	if (!id_write_format(&pvid, uuid, sizeof(uuid)))
		return_0;

	log_debug_lvmetad("Asking lvmetad for PV %s", uuid);
	reply = _lvmetad_send(cmd, "pv_lookup", "uuid = %s", uuid, NULL);
	if (!_lvmetad_handle_reply(reply, "pv_lookup", "", found))
		goto_out;

	if (found && !*found)
		goto out_success;

	if (!(cn = dm_config_find_node(reply.cft->root, "physical_volume")))
		goto_out;
        else if (!_pv_populate_lvmcache(cmd, cn, NULL, 0))
		goto_out;

out_success:
	result = 1;

out:
	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_lookup_by_dev(struct cmd_context *cmd, struct device *dev, int *found)
{
	int result = 0;
	daemon_reply reply;
	struct dm_config_node *cn;

	if (!lvmetad_active())
		return_0;

	log_debug_lvmetad("Asking lvmetad for PV on %s", dev_name(dev));
	reply = _lvmetad_send(cmd, "pv_lookup", "device = %" PRId64, (int64_t) dev->dev, NULL);
	if (!_lvmetad_handle_reply(reply, "pv_lookup", dev_name(dev), found))
		goto_out;

	if (found && !*found)
		goto out_success;

	cn = dm_config_find_node(reply.cft->root, "physical_volume");
	if (!cn || !_pv_populate_lvmcache(cmd, cn, NULL, dev->dev))
		goto_out;

out_success:
	result = 1;

out:
	daemon_reply_destroy(reply);
	return result;
}

int lvmetad_pv_list_to_lvmcache(struct cmd_context *cmd)
{
	daemon_reply reply;
	struct dm_config_node *cn;

	if (!lvmetad_active())
		return 1;

	log_debug_lvmetad("Asking lvmetad for complete list of known PVs");
	reply = _lvmetad_send(cmd, "pv_list", NULL);
	if (!_lvmetad_handle_reply(reply, "pv_list", "", NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	if ((cn = dm_config_find_node(reply.cft->root, "physical_volumes")))
		for (cn = cn->child; cn; cn = cn->sib)
			_pv_populate_lvmcache(cmd, cn, NULL, 0);

	daemon_reply_destroy(reply);

	return 1;
}

int lvmetad_get_vgnameids(struct cmd_context *cmd, struct dm_list *vgnameids)
{
	struct vgnameid_list *vgnl;
	struct id vgid;
	const char *vgid_txt;
	const char *vg_name;
	daemon_reply reply;
	struct dm_config_node *cn;

	log_debug_lvmetad("Asking lvmetad for complete list of known VG ids/names");
	reply = _lvmetad_send(cmd, "vg_list", NULL);
	if (!_lvmetad_handle_reply(reply, "vg_list", "", NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	if ((cn = dm_config_find_node(reply.cft->root, "volume_groups"))) {
		for (cn = cn->child; cn; cn = cn->sib) {
			vgid_txt = cn->key;
			if (!id_read_format(&vgid, vgid_txt)) {
				stack;
				continue;
			}

			if (!(vgnl = dm_pool_alloc(cmd->mem, sizeof(*vgnl)))) {
				log_error("vgnameid_list allocation failed.");
				return 0;
			}

			if (!(vg_name = dm_config_find_str(cn->child, "name", NULL))) {
				log_error("vg_list no name found.");
				return 0;
			}

			vgnl->vgid = dm_pool_strdup(cmd->mem, (char *)&vgid);
			vgnl->vg_name = dm_pool_strdup(cmd->mem, vg_name);

			if (!vgnl->vgid || !vgnl->vg_name) {
				log_error("vgnameid_list member allocation failed.");
				return 0;
			}

			dm_list_add(vgnameids, &vgnl->list);
		}
	}

	daemon_reply_destroy(reply);
	return 1;
}

int lvmetad_vg_list_to_lvmcache(struct cmd_context *cmd)
{
	struct volume_group *tmp;
	struct id vgid;
	const char *vgid_txt;
	daemon_reply reply;
	struct dm_config_node *cn;

	if (!lvmetad_active())
		return 1;

	log_debug_lvmetad("Asking lvmetad for complete list of known VGs");
	reply = _lvmetad_send(cmd, "vg_list", NULL);
	if (!_lvmetad_handle_reply(reply, "vg_list", "", NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	if ((cn = dm_config_find_node(reply.cft->root, "volume_groups")))
		for (cn = cn->child; cn; cn = cn->sib) {
			vgid_txt = cn->key;
			if (!id_read_format(&vgid, vgid_txt)) {
				stack;
				continue;
			}

			/* the call to lvmetad_vg_lookup will poke the VG into lvmcache */
			tmp = lvmetad_vg_lookup(cmd, NULL, (const char*)&vgid);
			release_vg(tmp);
		}

	daemon_reply_destroy(reply);
	return 1;
}

struct _extract_dl_baton {
	int i;
	struct dm_config_tree *cft;
	struct dm_config_node *pre_sib;
};

static int _extract_mda(struct metadata_area *mda, void *baton)
{
	struct _extract_dl_baton *b = baton;
	struct dm_config_node *cn;
	char id[32];

	if (!mda->ops->mda_export_text) /* do nothing */
		return 1;

	(void) dm_snprintf(id, 32, "mda%d", b->i);
	if (!(cn = make_config_node(b->cft, id, b->cft->root, b->pre_sib)))
		return 0;
	if (!mda->ops->mda_export_text(mda, b->cft, cn))
		return 0;

	b->i ++;
	b->pre_sib = cn; /* for efficiency */

	return 1;
}

static int _extract_disk_location(const char *name, struct disk_locn *dl, void *baton)
{
	struct _extract_dl_baton *b = baton;
	struct dm_config_node *cn;
	char id[32];

	if (!dl)
		return 1;

	(void) dm_snprintf(id, 32, "%s%d", name, b->i);
	if (!(cn = make_config_node(b->cft, id, b->cft->root, b->pre_sib)))
		return 0;
	if (!config_make_nodes(b->cft, cn, NULL,
			       "offset = %"PRId64, (int64_t) dl->offset,
			       "size = %"PRId64, (int64_t) dl->size,
			       NULL))
		return 0;

	b->i ++;
	b->pre_sib = cn; /* for efficiency */

	return 1;
}

static int _extract_da(struct disk_locn *da, void *baton)
{
	return _extract_disk_location("da", da, baton);
}

static int _extract_ba(struct disk_locn *ba, void *baton)
{
	return _extract_disk_location("ba", ba, baton);
}

static int _extract_mdas(struct lvmcache_info *info, struct dm_config_tree *cft,
			 struct dm_config_node *pre_sib)
{
	struct _extract_dl_baton baton = { .i = 0, .cft = cft, .pre_sib = NULL };

	if (!lvmcache_foreach_mda(info, &_extract_mda, &baton))
		return 0;
	baton.i = 0;
	if (!lvmcache_foreach_da(info, &_extract_da, &baton))
		return 0;
	baton.i = 0;
	if (!lvmcache_foreach_ba(info, &_extract_ba, &baton))
		return 0;

	return 1;
}

int lvmetad_pv_found(const struct id *pvid, struct device *dev, const struct format_type *fmt,
		     uint64_t label_sector, struct volume_group *vg, activation_handler handler)
{
	char uuid[64];
	daemon_reply reply;
	struct lvmcache_info *info;
	struct dm_config_tree *pvmeta, *vgmeta;
	const char *status, *vgname, *vgid;
	int64_t changed;
	int result;

	if (!lvmetad_active() || test_mode())
		return 1;

	if (!id_write_format(pvid, uuid, sizeof(uuid)))
                return_0;

	pvmeta = dm_config_create();
	if (!pvmeta)
		return_0;

	info = lvmcache_info_from_pvid((const char *)pvid, 0);

	if (!(pvmeta->root = make_config_node(pvmeta, "pv", NULL, NULL))) {
		dm_config_destroy(pvmeta);
		return_0;
	}

	if (!config_make_nodes(pvmeta, pvmeta->root, NULL,
			       "device = %"PRId64, (int64_t) dev->dev,
			       "dev_size = %"PRId64, (int64_t) (info ? lvmcache_device_size(info) : 0),
			       "format = %s", fmt->name,
			       "label_sector = %"PRId64, (int64_t) label_sector,
			       "id = %s", uuid,
			       NULL))
	{
		dm_config_destroy(pvmeta);
		return_0;
	}

	if (info)
		/* FIXME A more direct route would be much preferable. */
		_extract_mdas(info, pvmeta, pvmeta->root);

	if (vg) {
		if (!(vgmeta = export_vg_to_config_tree(vg))) {
			dm_config_destroy(pvmeta);
			return_0;
		}

		log_debug_lvmetad("Telling lvmetad to store PV %s (%s) in VG %s", dev_name(dev), uuid, vg->name);
		reply = _lvmetad_send(vg->cmd, "pv_found",
				      "pvmeta = %t", pvmeta,
				      "vgname = %s", vg->name,
				      "metadata = %t", vgmeta,
				      NULL);
		dm_config_destroy(vgmeta);
	} else {
		/*
		 * There is no VG metadata stored on this PV.
		 * It might or might not be an orphan.
		 */
		log_debug_lvmetad("Telling lvmetad to store PV %s (%s)", dev_name(dev), uuid);
		reply = _lvmetad_send(NULL, "pv_found", "pvmeta = %t", pvmeta, NULL);
	}

	dm_config_destroy(pvmeta);

	result = _lvmetad_handle_reply(reply, "pv_found", uuid, NULL);

	if (vg && result &&
	    (daemon_reply_int(reply, "seqno_after", -1) != vg->seqno ||
	     daemon_reply_int(reply, "seqno_after", -1) != daemon_reply_int(reply, "seqno_before", -1)))
		log_warn("WARNING: Inconsistent metadata found for VG %s", vg->name);

	if (result && handler) {
		status = daemon_reply_str(reply, "status", "<missing>");
		vgname = daemon_reply_str(reply, "vgname", "<missing>");
		vgid = daemon_reply_str(reply, "vgid", "<missing>");
		changed = daemon_reply_int(reply, "changed", 0);
		if (!strcmp(status, "partial"))
			handler(_lvmetad_cmd, vgname, vgid, 1, changed, CHANGE_AAY);
		else if (!strcmp(status, "complete"))
			handler(_lvmetad_cmd, vgname, vgid, 0, changed, CHANGE_AAY);
		else if (!strcmp(status, "orphan"))
			;
		else
			log_error("Request to %s %s in lvmetad gave status %s.",
			  "update PV", uuid, status);
	}

	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_gone(dev_t devno, const char *pv_name, activation_handler handler)
{
	daemon_reply reply;
	int result;
	int found;

	if (!lvmetad_active() || test_mode())
		return 1;

	/*
         *  TODO: automatic volume deactivation takes place here *before*
         *        all cached info is gone - call handler. Also, consider
         *        integrating existing deactivation script  that deactivates
         *        the whole stack from top to bottom (not yet upstream).
         */

	log_debug_lvmetad("Telling lvmetad to forget any PV on %s", pv_name);
	reply = _lvmetad_send(NULL, "pv_gone", "device = %" PRId64, (int64_t) devno, NULL);

	result = _lvmetad_handle_reply(reply, "pv_gone", pv_name, &found);
	/* We don't care whether or not the daemon had the PV cached. */

	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_gone_by_dev(struct device *dev, activation_handler handler)
{
	return lvmetad_pv_gone(dev->dev, dev_name(dev), handler);
}

/*
 * The following code implements pvscan --cache.
 */

struct _lvmetad_pvscan_baton {
	struct volume_group *vg;
	struct format_instance *fid;
};

static int _lvmetad_pvscan_single(struct metadata_area *mda, void *baton)
{
	struct _lvmetad_pvscan_baton *b = baton;
	struct volume_group *this;

	if (!(this = mda_is_ignored(mda) ? NULL : mda->ops->vg_read(b->fid, "", mda, NULL, NULL, 1)))
		return 1;

	/* FIXME Also ensure contents match etc. */
	if (!b->vg || this->seqno > b->vg->seqno)
		b->vg = this;
	else if (b->vg)
		release_vg(this);

	return 1;
}

/*
 * The lock manager may detect that the vg cached in lvmetad is out of date,
 * due to something like an lvcreate from another host.
 * This is limited to changes that only affect the vg (not global state like
 * orphan PVs), so we only need to reread mdas on the vg's existing pvs.
 */

static struct volume_group *lvmetad_pvscan_vg(struct cmd_context *cmd, struct volume_group *vg)
{
	struct volume_group *vg_ret = NULL;
	struct dm_config_tree *vgmeta_ret = NULL;
	struct dm_config_tree *vgmeta;
	struct pv_list *pvl;
	struct lvmcache_info *info;
	struct format_instance *fid;
	struct format_instance_ctx fic = { .type = 0 };
	struct _lvmetad_pvscan_baton baton;
	struct device *save_dev = NULL;

	dm_list_iterate_items(pvl, &vg->pvs) {
		/* missing pv */
		if (!pvl->pv->dev)
			continue;

		if (!(info = lvmcache_info_from_pvid((const char *)&pvl->pv->id, 0))) {
			log_error("Failed to find cached info for PV %s.", pv_dev_name(pvl->pv));
			return NULL;
		}

		baton.vg = NULL;
		baton.fid = lvmcache_fmt(info)->ops->create_instance(lvmcache_fmt(info), &fic);

		if (!baton.fid)
			return NULL;

		if (baton.fid->fmt->features & FMT_OBSOLETE) {
			log_error("WARNING: Ignoring obsolete format of metadata (%s) on device %s when using lvmetad",
			  	baton.fid->fmt->name, dev_name(pvl->pv->dev));
			lvmcache_fmt(info)->ops->destroy_instance(baton.fid);
			return NULL;
		}

		lvmcache_foreach_mda(info, _lvmetad_pvscan_single, &baton);

		if (!baton.vg) {
			lvmcache_fmt(info)->ops->destroy_instance(baton.fid);
			return NULL;
		}

		if (!(vgmeta = export_vg_to_config_tree(baton.vg))) {
			log_error("VG export to config tree failed");
			release_vg(baton.vg);
			return NULL;
		}

		if (!vgmeta_ret) {
			vgmeta_ret = vgmeta;
			save_dev = pvl->pv->dev;
		} else {
			if (compare_config(vgmeta_ret->root, vgmeta->root)) {
				log_error("VG %s metadata comparison failed for device %s vs %s",
					  vg->name, dev_name(pvl->pv->dev), save_dev ? dev_name(save_dev) : "none");
				_log_debug_inequality(vg->name, vgmeta_ret->root, vgmeta->root);
				dm_config_destroy(vgmeta);
				dm_config_destroy(vgmeta_ret);
				release_vg(baton.vg);
				return NULL;
			}
			dm_config_destroy(vgmeta);
		}

		release_vg(baton.vg);
	}

	if (vgmeta_ret) {
		fid = lvmcache_fmt(info)->ops->create_instance(lvmcache_fmt(info), &fic);
		if (!(vg_ret = import_vg_from_config_tree(vgmeta_ret, fid))) {
			log_error("VG import from config tree failed");
			lvmcache_fmt(info)->ops->destroy_instance(fid);
			goto out;
		}

		/*
		 * Update lvmetad with the newly read version of the VG.
		 * The "precommitted" name is a misnomer in this case,
		 * but that is the field which lvmetad_vg_update() uses
		 * to send the metadata cft to lvmetad.
		 */
		vg_ret->cft_precommitted = vgmeta_ret;
		if (!lvmetad_vg_update(vg_ret))
			log_error("Failed to update lvmetad with new VG meta");
		vg_ret->cft_precommitted = NULL;
		dm_config_destroy(vgmeta_ret);
	}
out:
	return vg_ret;
}

int lvmetad_pvscan_single(struct cmd_context *cmd, struct device *dev,
			  activation_handler handler, int ignore_obsolete)
{
	struct label *label;
	struct lvmcache_info *info;
	struct _lvmetad_pvscan_baton baton;
	/* Create a dummy instance. */
	struct format_instance_ctx fic = { .type = 0 };

	if (!lvmetad_active()) {
		log_error("Cannot proceed since lvmetad is not active.");
		return 0;
	}

	if (!label_read(dev, &label, 0)) {
		log_print_unless_silent("No PV label found on %s.", dev_name(dev));
		if (!lvmetad_pv_gone_by_dev(dev, handler))
			goto_bad;
		return 1;
	}

	info = (struct lvmcache_info *) label->info;

	baton.vg = NULL;
	baton.fid = lvmcache_fmt(info)->ops->create_instance(lvmcache_fmt(info), &fic);

	if (!baton.fid)
		goto_bad;

	if (baton.fid->fmt->features & FMT_OBSOLETE) {
		if (ignore_obsolete)
			log_warn("WARNING: Ignoring obsolete format of metadata (%s) on device %s when using lvmetad",
				  baton.fid->fmt->name, dev_name(dev));
		else
			log_error("WARNING: Ignoring obsolete format of metadata (%s) on device %s when using lvmetad",
				  baton.fid->fmt->name, dev_name(dev));
		lvmcache_fmt(info)->ops->destroy_instance(baton.fid);

		if (ignore_obsolete)
			return 1;
		return 0;
	}

	lvmcache_foreach_mda(info, _lvmetad_pvscan_single, &baton);

	/*
	 * LVM1 VGs have no MDAs and lvmcache_foreach_mda isn't worth fixing
	 * to use pseudo-mdas for PVs.
	 * Note that the single_device parameter also gets ignored and this code
	 * can scan further devices.
	 */
	if (!baton.vg && !(baton.fid->fmt->features & FMT_MDAS))
		baton.vg = ((struct metadata_area *) dm_list_first(&baton.fid->metadata_areas_in_use))->ops->vg_read(baton.fid, lvmcache_vgname_from_info(info), NULL, NULL, NULL, 1);

	if (!baton.vg)
		lvmcache_fmt(info)->ops->destroy_instance(baton.fid);

	/*
	 * NB. If this command failed and we are relying on lvmetad to have an
	 * *exact* image of the system, the lvmetad instance that went out of
	 * sync needs to be killed.
	 */
	if (!lvmetad_pv_found((const struct id *) &dev->pvid, dev, lvmcache_fmt(info),
			      label->sector, baton.vg, handler)) {
		release_vg(baton.vg);
		goto_bad;
	}

	release_vg(baton.vg);
	return 1;

bad:
	return 0;
}

/*
 * Update the lvmetad cache: clear the current lvmetad cache, and scan all
 * devs, sending all info from the devs to lvmetad.
 *
 * We want only one command to be doing this at a time.  When do_wait is set,
 * this will first check if lvmetad is currently being updated by another
 * command, and if so it will delay until that update is finished, or until a
 * timeout, at which point it will go ahead and do the lvmetad update.
 *
 * Callers that have already checked and waited for the updating state, e.g. by
 * using lvmetad_token_matches(), will generaly set do_wait to 0.  Callers that
 * have not checked for the updating state yet will generally set do_wait to 1.
 *
 * If another command doing an update failed, it left lvmetad in the "update in
 * progess" state, so we can't just wait until that state has cleared, but have
 * to go ahead after a timeout.
 *
 * The _lvmetad_is_updating check avoids most races to update lvmetad from
 * multiple commands (which shouldn't generally happen anway) but does not
 * eliminate them.  If an update race happens, the second will see that the
 * previous token was "update in progress" when it calls _token_update().  It
 * will then fail, and the command calling lvmetad_pvscan_all_devs() will
 * generally revert disk scanning and not use lvmetad.
 */

static int _lvmetad_pvscan_all_devs(struct cmd_context *cmd, activation_handler handler,
				    int ignore_obsolete, int do_wait)
{
	struct dev_iter *iter;
	struct device *dev;
	daemon_reply reply;
	int r = 1;
	char *future_token;
	int was_silent;
	int replacing_other_update = 0;
	int replaced_update = 0;
	int retries = 0;

	if (!lvmetad_active()) {
		log_error("Cannot proceed since lvmetad is not active.");
		return 0;
	}

 retry:
	/*
	 * If another update is in progress, delay to allow it to finish,
	 * rather than interrupting it with our own update.
	 */
	if (do_wait && _lvmetad_is_updating(cmd, 1)) {
		log_warn("WARNING: lvmetad update is interrupting another update in progress.");
		replacing_other_update = 1;
	}

	log_verbose("Scanning all devices to update lvmetad.");

	if (!(iter = dev_iter_create(cmd->lvmetad_filter, 1))) {
		log_error("dev_iter creation failed");
		return 0;
	}

	future_token = _lvmetad_token;
	_lvmetad_token = (char *) "update in progress";

	if (!_token_update(&replaced_update)) {
		log_error("Failed to update lvmetad which had an update in progress.");
		dev_iter_destroy(iter);
		_lvmetad_token = future_token;
		return 0;
	}

	/*
	 * if _token_update() sets replaced_update to 1, it means that we set
	 * "update in progress" when the lvmetad was already set to "udpate in
	 * progress".  This detects a race between two commands doing updates
	 * at once.  The attempt above to avoid this race using
	 * _lvmetad_is_updating isn't perfect.
	 */
	if (!replacing_other_update && replaced_update) {
		if (do_wait && !retries) {
			retries = 1;
			log_warn("WARNING: lvmetad update in progress, retry update.");
			dev_iter_destroy(iter);
			_lvmetad_token = future_token;
			goto retry;
		}
		log_error("Concurrent lvmetad updates failed.");
		dev_iter_destroy(iter);
		_lvmetad_token = future_token;
		return 0;
	}

	log_debug_lvmetad("Telling lvmetad to clear its cache");
	reply = _lvmetad_send(cmd, "pv_clear_all", NULL);
	if (!_lvmetad_handle_reply(reply, "pv_clear_all", "", NULL))
		r = 0;
	daemon_reply_destroy(reply);

	was_silent = silent_mode();
	init_silent(1);

	while ((dev = dev_iter_get(iter))) {
		if (sigint_caught()) {
			r = 0;
			stack;
			break;
		}
		if (!lvmetad_pvscan_single(cmd, dev, handler, ignore_obsolete))
			r = 0;
	}

	init_silent(was_silent);

	dev_iter_destroy(iter);

	_lvmetad_token = future_token;
	if (!_token_update(NULL))
		return 0;

	return r;
}

int lvmetad_pvscan_all_devs(struct cmd_context *cmd, activation_handler handler, int do_wait)
{
	return _lvmetad_pvscan_all_devs(cmd, handler, 0, do_wait);
}

/* 
 * FIXME Implement this function, skipping PVs known to belong to local or clustered,
 * non-exported VGs.
 */
int lvmetad_pvscan_foreign_vgs(struct cmd_context *cmd, activation_handler handler)
{
	return _lvmetad_pvscan_all_devs(cmd, handler, 1, 1);
}

int lvmetad_vg_clear_outdated_pvs(struct volume_group *vg)
{
	char uuid[64];
	daemon_reply reply;
	int result;

	if (!id_write_format(&vg->id, uuid, sizeof(uuid)))
		return_0;

	reply = _lvmetad_send(vg->cmd, "vg_clear_outdated_pvs", "vgid = %s", uuid, NULL);
	result = _lvmetad_handle_reply(reply, "vg_clear_outdated_pvs", vg->name, NULL);
	daemon_reply_destroy(reply);

	return result;
}

/*
 * Records the state of cached PVs in lvmetad so we can look for changes
 * after rescanning.
 */
struct pv_cache_list {
	struct dm_list list;
	dev_t devt;
	struct id pvid;
	const char *vgid;
	unsigned found : 1;
	unsigned update_udev : 1;
};

/*
 * Get the list of PVs known to lvmetad.
 */
static int _lvmetad_get_pv_cache_list(struct cmd_context *cmd, struct dm_list *pvc_list)
{
	daemon_reply reply;
	struct dm_config_node *cn;
	struct pv_cache_list *pvcl;
	const char *pvid_txt;
	const char *vgid;

	if (!lvmetad_active())
		return 1;

	log_debug_lvmetad("Asking lvmetad for complete list of known PVs");

	reply = _lvmetad_send(cmd, "pv_list", NULL);
	if (!_lvmetad_handle_reply(reply, "pv_list", "", NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	if ((cn = dm_config_find_node(reply.cft->root, "physical_volumes"))) {
		for (cn = cn->child; cn; cn = cn->sib) {
			if (!(pvcl = dm_pool_zalloc(cmd->mem, sizeof(*pvcl)))) {
				log_error("pv_cache_list allocation failed.");
				return 0;
			}

			pvid_txt = cn->key;
			if (!id_read_format(&pvcl->pvid, pvid_txt)) {
				stack;
				continue;
			}

			pvcl->devt = dm_config_find_int(cn->child, "device", 0);

			if ((vgid = dm_config_find_str(cn->child, "vgid", NULL)))
				pvcl->vgid = dm_pool_strdup(cmd->mem, vgid);

			dm_list_add(pvc_list, &pvcl->list);
		}
	}

	daemon_reply_destroy(reply);

	return 1;
}

/*
 * Opening the device RDWR should trigger a udev db update.
 * FIXME: is there a better way to update the udev db than
 * doing an open/close of the device? - For example writing
 * "change" to /sys/block/<device>/uevent?
 */
static void _update_pv_in_udev(struct cmd_context *cmd, dev_t devt)
{
	struct device *dev;

	log_debug_devs("device %d:%d open to update udev",
		       (int)MAJOR(devt), (int)MINOR(devt));

	if (!(dev = dev_cache_get_by_devt(devt, cmd->lvmetad_filter))) {
		log_error("_update_pv_in_udev no dev found");
		return;
	}

	if (!dev_open(dev)) {
		stack;
		return;
	}

	if (!dev_close(dev))
		stack;
}

/*
 * Compare before and after PV lists from before/after rescanning,
 * and update udev db for changes.
 *
 * For PVs that have changed pvid or vgid in lvmetad from rescanning,
 * there may be information in the udev database to update, so open
 * these devices to trigger a udev update.
 *
 * "before" refers to the list of pvs from lvmetad before rescanning
 * "after" refers to the list of pvs from lvmetad after rescanning
 *
 * Comparing both lists, we can see which PVs changed (pvid or vgid),
 * and trigger a udev db update for those.
 */
static void _update_changed_pvs_in_udev(struct cmd_context *cmd,
					struct dm_list *pvc_before,
					struct dm_list *pvc_after)
{
	struct pv_cache_list *before;
	struct pv_cache_list *after;
	char id_before[ID_LEN + 1]  __attribute__((aligned(8)));
	char id_after[ID_LEN + 1]  __attribute__((aligned(8)));
	int found;

	dm_list_iterate_items(before, pvc_before) {
		found = 0;

		dm_list_iterate_items(after, pvc_after) {
			if (after->found)
				continue;

			if (before->devt != after->devt)
				continue;

			if (!id_equal(&before->pvid, &after->pvid)) {
				memset(id_before, 0, sizeof(id_before));
				memset(id_after, 0, sizeof(id_after));
				strncpy(&id_before[0], (char *) &before->pvid, sizeof(id_before) - 1);
				strncpy(&id_after[0], (char *) &after->pvid, sizeof(id_after) - 1);

				log_debug_devs("device %d:%d changed pvid from %s to %s",
					       (int)MAJOR(before->devt), (int)MINOR(before->devt),
					       id_before, id_after);

				before->update_udev = 1;

			} else if ((before->vgid && !after->vgid) ||
				   (after->vgid && !before->vgid) ||
				   (before->vgid && after->vgid && strcmp(before->vgid, after->vgid))) {

				log_debug_devs("device %d:%d changed vg from %s to %s",
					       (int)MAJOR(before->devt), (int)MINOR(before->devt),
					       before->vgid ?: "none", after->vgid ?: "none");

				before->update_udev = 1;
			}

			after->found = 1;
			before->found = 1;
			found = 1;
			break;
		}

		if (!found) {
			memset(id_before, 0, sizeof(id_before));
			strncpy(&id_before[0], (char *) &before->pvid, sizeof(id_before) - 1);

			log_debug_devs("device %d:%d pvid %s vg %s is gone",
				       (int)MAJOR(before->devt), (int)MINOR(before->devt),
				       id_before, before->vgid ? before->vgid : "none");

			before->update_udev = 1;
		}
	}

	dm_list_iterate_items(before, pvc_before) {
		if (before->update_udev)
			_update_pv_in_udev(cmd, before->devt);
	}

	dm_list_iterate_items(after, pvc_after) {
		if (after->update_udev)
			_update_pv_in_udev(cmd, after->devt);
	}
}

/*
 * Before this command was run, some external entity may have
 * invalidated lvmetad's cache of global information, e.g. lvmlockd.
 *
 * The global information includes things like a new VG, a
 * VG that was removed, the assignment of a PV to a VG;
 * any change that is not isolated within a single VG.
 *
 * The external entity, like a lock manager, would invalidate
 * the lvmetad global cache if it detected that the global
 * information had been changed on disk by something other
 * than a local lvm command, e.g. an lvm command on another
 * host with access to the same devices.  (How it detects
 * the change is specific to lock manager or other entity.)
 *
 * The effect is that metadata on disk is newer than the metadata
 * in the local lvmetad daemon, and the local lvmetad's cache
 * should be updated from disk before this command uses it.
 *
 * So, using this function, a command checks if lvmetad's global
 * cache is valid.  If so, it does nothing.  If not, it rescans
 * devices to update the lvmetad cache, then it notifies lvmetad
 * that it's cache is valid again (consistent with what's on disk.)
 * This command can then go ahead and use the newly refreshed metadata.
 *
 * 1. Check if the lvmetad global cache is invalid.
 * 2. If so, reread metadata from all devices and update the lvmetad cache.
 * 3. Tell lvmetad that the global cache is now valid.
 */

void lvmetad_validate_global_cache(struct cmd_context *cmd, int force)
{
	struct dm_list pvc_before; /* pv_cache_list */
	struct dm_list pvc_after; /* pv_cache_list */
	const char *reason = NULL;
	daemon_reply reply;
	int global_invalid;

	dm_list_init(&pvc_before);
	dm_list_init(&pvc_after);

	if (!lvmlockd_use()) {
		log_error(INTERNAL_ERROR "validate global cache without lvmlockd");
		return;
	}

	if (!lvmetad_used())
		return;

	log_debug_lvmetad("Validating global lvmetad cache");

	if (force)
		goto do_scan;

	log_debug_lvmetad("lvmetad validate send get_global_info");

	reply = daemon_send_simple(_lvmetad, "get_global_info",
				   "token = %s", "skip",
				   NULL);

	if (reply.error) {
		log_error("lvmetad_validate_global_cache get_global_info error %d", reply.error);
		goto do_scan;
	}

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK")) {
		log_error("lvmetad_validate_global_cache get_global_info not ok");
		goto do_scan;
	}

	global_invalid = daemon_reply_int(reply, "global_invalid", -1);

	daemon_reply_destroy(reply);

	if (!global_invalid) {
		/* cache is valid */
		return;
	}

 do_scan:
	/*
	 * Save the current state of pvs from lvmetad so after devices are
	 * scanned, we can compare to the new state to see if pvs changed.
	 */
	_lvmetad_get_pv_cache_list(cmd, &pvc_before);

	/*
	 * Update the local lvmetad cache so it correctly reflects any
	 * changes made on remote hosts.  (It's possible that this command
	 * already refreshed the local lvmetad because of a token change,
	 * but we need to do it again here since we now hold the global
	 * lock.  Another host may have changed things between the time
	 * we rescanned for the token, and the time we acquired the global
	 * lock.)
	 */
	if (!lvmetad_pvscan_all_devs(cmd, NULL, 1)) {
		log_warn("WARNING: Not using lvmetad because cache update failed.");
		lvmetad_set_active(cmd, 0);
		return;
	}

	if (lvmetad_is_disabled(cmd, &reason)) {
		log_warn("WARNING: Not using lvmetad because %s.", reason);
		lvmetad_set_active(cmd, 0);
		return;
	}

	/*
	 * Clear the global_invalid flag in lvmetad.
	 * Subsequent local commands that read global state
	 * from lvmetad will not see global_invalid until
	 * another host makes another global change.
	 */
	log_debug_lvmetad("lvmetad validate send set_global_info");

	reply = daemon_send_simple(_lvmetad, "set_global_info",
				   "token = %s", "skip",
				   "global_invalid = %d", 0,
				   NULL);
	if (reply.error)
		log_error("lvmetad_validate_global_cache set_global_info error %d", reply.error);

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK"))
		log_error("lvmetad_validate_global_cache set_global_info not ok");

	daemon_reply_destroy(reply);

	/*
	 * Populate this command's lvmcache structures from lvmetad.
	 */
	lvmcache_seed_infos_from_lvmetad(cmd);

	/*
	 * Update the local udev database to reflect PV changes from
	 * other hosts.
	 *
	 * Compare the before and after PV lists, and if a PV's
	 * pvid or vgid has changed, then open that device to trigger
	 * a uevent to update the udev db.
	 *
	 * This has no direct benefit to lvm, but is just a best effort
	 * attempt to keep the udev db updated and reflecting current
	 * lvm information.
	 *
	 * FIXME: lvmcache_seed_infos_from_lvmetad() and _lvmetad_get_pv_cache_list()
	 * each get pv_list from lvmetad, and they could share a single pv_list reply.
	 */
	if (!dm_list_empty(&pvc_before)) {
		_lvmetad_get_pv_cache_list(cmd, &pvc_after);
		_update_changed_pvs_in_udev(cmd, &pvc_before, &pvc_after);
	}
}

int lvmetad_vg_is_foreign(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	daemon_reply reply;
	struct dm_config_node *top;
	const char *system_id = NULL;
	char uuid[64];
	int ret;

	if (!id_write_format((const struct id*)vgid, uuid, sizeof(uuid)))
		return_0;

	reply = _lvmetad_send(cmd, "vg_lookup",
			      "uuid = %s", uuid,
			      "name = %s", vgname,
			       NULL);

	if ((top = dm_config_find_node(reply.cft->root, "metadata")))
		system_id = dm_config_find_str(top, "metadata/system_id", NULL);

	ret = !is_system_id_allowed(cmd, system_id);

	daemon_reply_destroy(reply);

	return ret;
}

/*
 * lvmetad has a disabled state in which it continues running,
 * and returns the "disabled" flag in a get_global_info query.
 *
 * Case 1
 * ------
 * When "normal" commands start, (those not specifically
 * intended to rescan devs) they begin by checking lvmetad's
 * token and global info:
 *
 * - If the token doesn't match (should be uncommon), the
 * command first rescans devices to repopulate lvmetad with
 * the global_filter it is using.  After rescanning, the
 * lvmetad disabled state is set or cleared depending on
 * what the scan saw.
 *
 *     An unmatching token occurs when:
 *     . lvmetad was just started and has not been populated yet.
 *     . The global_filter has been changed in lvm.conf since the
 *       last command was run.
 *     . The global_filter is overriden on the command line.
 *       (There's little point in using lvmetad if global_filter
 *       is often changed/overridden.)
 *
 * - If the token does match (common case), the command and
 * lvmetad are using the same global_filter and the command
 * does not rescan devs to repopulate lvmetad, or change the
 * lvmetad disabled state.
 *
 * - After the token check/sync, the command checks if the
 * disabled flag is set in lvmetad.  If it is, the command will
 * not use the lvmetad cache and will revert to scanning, i.e.
 * it runs the same as if use_lvmetad=0.
 *
 * So, "normal" commands try to use the lvmetad cache to avoid
 * scanning devices.  In the uncommon case when the token doesn't
 * match, these commands will first rescan devs to repopulate the
 * lvmetad cache, and then attempt to use the lvmetad cache.
 * In the uncommon case where lvmetad is disabled (by a previous
 * command), the common commands do not rescan devs to repopulate
 * lvmetad, but revert the equivalent of use_lvmetad=0, reading
 * from disk instead of the cache.
 * The combination of those two uncommon cases means that a command
 * could begin by rescanning devs because of a token mismatch, then
 * disable lvmetad as a result of that scan, and continue without
 * using lvmetad.
 *
 * Case 2
 * ------
 * Commands that are meant to scan devices to repopulate the
 * lvmetad cache, e.g. pvscan --cache, will always rescan
 * devices and then set/clear the disabled state according to
 * what they found when scanning.  The global_filter is always
 * used when choosing which devices to scan to populate lvmetad.
 * The command-specific filter is never used when choosing
 * which devices to scan for repopulating the lvmetad cache.
 *
 * During a scan repopulating the lvmetad cache, a command looks
 * for PVs with lvm1 metadata, or duplicate PVs (two devices with
 * the same PVID).  If either of those are found during the scan,
 * the command sets the disabled state in lvmetad.  If none are
 * found, the command clears the disabled state in lvmetad.
 * (Other problems scanning may also cause the command to set the
 * disabled state.)
 *
 * Case 3
 * ------
 * The special command 'pvscan --cache <dev>' is meant to only
 * scan the specified device and send info from the dev to
 * lvmetad.  This single-dev pvscan will not detect duplicate PVs
 * since it only sees the one device.  If lvmetad already knows
 * about the same PV on another device, then lvmetad will be the
 * first to discover that a duplicate PV exists.  In this case,
 * lvmetad sets the disabled state for itself.
 *
 * Duplicates
 * ----------
 * The most common reasons for duplicate PVs to exist are:
 *
 * 1. Multipath.  When multipath is running, it creates a new
 * mpath device for the underlying "duplicate" devs.  lvm has
 * built in, automatic filtering that will hide the duplicate
 * devs of the underlying mpath dev, so the duplicates will
 * be skipping during scanning (multipath_component_detection).
 *
 * If multipath_component_detection=0, or if multipathd is not
 * running, or multipath is not set up to handle a particular
 * set of devs, then lvm will see the multipath paths as
 * duplicates.  lvm will choose one of them to use, consider
 * the other a duplicate, and disable lvmetad.  multipathd
 * should be configured and running to resolve these duplicates,
 * and multipath_component_detection enabled.
 *
 * 2. Cloning by copying.  One device is copied over another, e.g.
 * with dd.  This is a more concerning case because using the
 * wrong device could lead to corruption.  LVM will attempt to
 * choose the best device as the PV, but it may not always
 * be the right one.  In this case, lvmetad is disabled.
 * vgimportclone should be used on the new copy to resolve the
 * duplicates.
 *
 * 3. Cloning by hardware.  A LUN is cloned/snapshotted on
 * a hardware device.  The description here is the same as
 * cloning by copying.
 *
 * 4. Creating LVM snapshots of LVs being used as PVs.
 * If pvcreate is run on an LV, and lvcreate is used to
 * create a snapshot of that LV, then the two LVs will
 * appear to be duplicate PVs.
 *
 * Filtering duplicates
 * --------------------
 * 
 * If all but one copy of a PV is added to the global_filter,
 * then duplicates will not be seen when scanning to populate
 * the lvmetad cache.  Neither common commands nor scanning
 * commands will see the duplicates, and lvmetad will not be
 * disabled.
 *
 * If the global_filter is *not* used to hide duplicates,
 * then lvmetad will be disabled when they are scanned, but
 * common commands can use the command filter to hide the
 * duplicates and work with a selected instance of the PV.
 * The command will not use lvmetad in this case, but will
 * not see duplicate PVs itself because its command filter
 * is more restrictive than the global_filter and has hidden
 * the duplicates.
 */

/*
 * FIXME: if we fail to disable lvmetad, then other commands could
 * potentially use incorrect cache data from lvmetad.  Should we
 * do something more severe if the disable messages fails, like
 * sending SIGKILL to the lvmetad pid?
 *
 * FIXME: log something in syslog any time we disable lvmetad?
 * At a minimum if we fail to disable lvmetad.
 */
void lvmetad_set_disabled(struct cmd_context *cmd, const char *reason)
{
	daemon_reply reply;

	if (!_lvmetad_use)
		return;

	log_debug_lvmetad("lvmetad send disabled %s", reason);

	reply = daemon_send_simple(_lvmetad, "set_global_info",
				   "token = %s", "skip",
				   "global_disable = " FMTd64, (int64_t)1,
				   "disable_reason = %s", reason,
				   NULL);
	if (reply.error)
		log_error("Failed to send message to lvmetad %d", reply.error);

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK"))
		log_error("Failed response from lvmetad.");

	daemon_reply_destroy(reply);
}

void lvmetad_clear_disabled(struct cmd_context *cmd)
{
	daemon_reply reply;

	if (!_lvmetad_use)
		return;

	log_debug_lvmetad("lvmetad send disabled 0");

	reply = daemon_send_simple(_lvmetad, "set_global_info",
				   "token = %s", "skip",
				   "global_disable = " FMTd64, (int64_t)0,
				   NULL);
	if (reply.error)
		log_error("Failed to send message to lvmetad %d", reply.error);

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK"))
		log_error("Failed response from lvmetad.");

	daemon_reply_destroy(reply);
}

int lvmetad_is_disabled(struct cmd_context *cmd, const char **reason)
{
	daemon_reply reply;
	const char *reply_reason;
	int ret = 0;

	reply = daemon_send_simple(_lvmetad, "get_global_info",
				   "token = %s", "skip",
				   NULL);

	if (reply.error) {
		*reason = "send error";
		ret = 1;
		goto out;
	}

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK")) {
		*reason = "response error";
		ret = 1;
		goto out;
	}

	if (daemon_reply_int(reply, "global_disable", 0)) {
		ret = 1;

		reply_reason = daemon_reply_str(reply, "disable_reason", NULL);

		if (!reply_reason) {
			*reason = "<not set>";

		} else if (strstr(reply_reason, LVMETAD_DISABLE_REASON_DIRECT)) {
			*reason = "the disable flag was set directly";

		} else if (strstr(reply_reason, LVMETAD_DISABLE_REASON_LVM1)) {
			*reason = "LVM1 metadata was found";

		} else if (strstr(reply_reason, LVMETAD_DISABLE_REASON_DUPLICATES)) {
			*reason = "duplicate PVs were found";

		} else {
			*reason = "<unknown>";
		}
	}
out:
	daemon_reply_destroy(reply);
	return ret;
}

