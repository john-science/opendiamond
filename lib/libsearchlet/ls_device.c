/*
 *  The OpenDiamond Platform for Interactive Search
 *  Version 4
 *
 *  Copyright (c) 2002-2005 Intel Corporation
 *  Copyright (c) 2006 Larry Huston <larry@thehustons.net>
 *  Copyright (c) 2008-2009 Carnegie Mellon University
 *  All rights reserved.
 *
 *  This software is distributed under the terms of the Eclipse Public
 *  License, Version 1.0 which can be found in the file named LICENSE.
 *  ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
 *  RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
 */

/*
 * These file handles a lot of the device specific code.  For the current
 * version we have state for each of the devices.
 */
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <netdb.h>

#include "lib_tools.h"
#include "lib_searchlet.h"
#include "lib_odisk.h"
#include "lib_log.h"
#include "lib_search_priv.h"
#include "log_socket.h"
#include "log_impl.h"
#include "assert.h"
#include "lib_hstub.h"
#include "dctl_common.h"
#include "dctl_impl.h"

/*
 *  This function intializes the background processing thread that
 *  is used for taking data ariving from the storage devices
 *  and completing the processing.  This thread initializes the ring
 *  that takes incoming data.
 */

void
dev_log_data_cb(void *cookie, char *data, int len, int devid)
{
	log_info_t     *linfo;
	device_handle_t *dev;

	dev = (device_handle_t *) cookie;

	linfo = (log_info_t *) malloc(sizeof(*linfo));
	if (linfo == NULL) {
		/*
		 * we failed to allocate the space, we just free the log
		 * data. 
		 */
		free(data);
		return;
	}


	linfo->data = data;
	linfo->len = len;
	linfo->dev = devid;

	if (ring_enq(dev->sc->log_ring, (void *) linfo)) {
		/*
		 * If we failed to log, then we just fee
		 * the information and loose this log.
		 */
		free(data);
		free(linfo);
		return;
	}

	return;
}


static void
dev_search_done_cb(void *hcookie)
{
	device_handle_t *dev;
	time_t          cur_time;
	time_t          delta;

	dev = (device_handle_t *) hcookie;

	log_message(LOGT_BG, LOGL_INFO, "device %s search is complete",
	    dev->dev_name);

	dev->flags |= DEV_FLAG_COMPLETE;
	time(&cur_time);
	delta = cur_time - dev->start_time;
	fprintf(stdout, "complete: %s elapsed time %ld data %s ",
		dev->dev_name, delta, ctime(&cur_time));
	return;
}

static void
conn_down_cb(void *hcookie)
{
	device_handle_t *dev;

	dev = (device_handle_t *) hcookie;

	log_message(LOGT_BG, LOGL_ERR, "device %s connection is down",
	    dev->dev_name);

	dev->flags |= DEV_FLAG_COMPLETE;
	dev->flags |= DEV_FLAG_DOWN;
	return;
}


/*
 * Search through the list of current devices to see
 * if we have one with the same ID.
 */

static device_handle_t *
lookup_dev_by_name(search_context_t * sc, const char *host)
{
	device_handle_t *cur_dev;

	cur_dev = sc->dev_list;
	while (cur_dev != NULL) {
		if ((cur_dev->flags & DEV_FLAG_DOWN) == 0 &&
		    strcmp(cur_dev->dev_name, host) == 0)
			break;
		cur_dev = cur_dev->next;
	}

	return (cur_dev);
}


static int
remote_write_leaf(char *path, size_t len, char *data, void *cookie)
{
	void *handle = ((device_handle_t *)cookie)->dev_handle;
	return device_write_leaf(handle, path, len, data);
}


static int
remote_read_leaf(char *path, dctl_data_type_t *dtype, size_t *len, char *data,
		 void *cookie)
{
	void *handle = ((device_handle_t *)cookie)->dev_handle;
	return device_read_leaf(handle, path, dtype, len, data);
}


static int
remote_list_nodes(char *path, int *num_ents, dctl_entry_t *space, void *cookie)
{
	void *handle = ((device_handle_t *)cookie)->dev_handle;
	return device_list_nodes(handle, path, num_ents, space);
}


static int
remote_list_leafs(char *path, int *num_ents, dctl_entry_t *space, void *cookie)
{
	void *handle = ((device_handle_t *)cookie)->dev_handle;
	return device_list_leafs(handle, path, num_ents, space);
}


static int
read_float_as_uint32(void *cookie, size_t *len, char *data)
{

	assert(cookie != NULL);
	assert(data != NULL);

	if (*len < sizeof(uint32_t)) {
		*len = sizeof(uint32_t);
		return (ENOMEM);
	}


	*len = sizeof(uint32_t);
	*(uint32_t *) data = (uint32_t) (*(float *) cookie);

	return (0);
}


static void
register_remote_dctl(const char *host, device_handle_t * dev_handle)
{
	dctl_fwd_cbs_t  cbs;
	int		err;
	char           *node_name, *delim;
	char            cr_name[128];

	node_name = strdup(host);

	/*
	 * replace all the '.' with '_'
	 */
	while ((delim = index(node_name, '.')) != NULL)
	    *delim = '_';

	cbs.dfwd_rleaf_cb = remote_read_leaf;
	cbs.dfwd_wleaf_cb = remote_write_leaf;
	cbs.dfwd_lnodes_cb = remote_list_nodes;
	cbs.dfwd_lleafs_cb = remote_list_leafs;
	cbs.dfwd_cookie = (void *) dev_handle;

	err = dctl_register_fwd_node(HOST_DEVICE_PATH, node_name, &cbs);
	if (err) {
		log_message(LOGT_BG, LOGL_ERR, 
		    "register_remove_dctl:  failed to register remote - err=%d",
		    err);
		free(node_name);
		return;
	}

	err = snprintf(cr_name, 128, "%s_%s", "be_serviced", node_name);
	dctl_register_u32(HOST_DEVICE_PATH, cr_name, O_RDONLY,
			  &dev_handle->serviced);

	free(node_name);
}



/*
 * This is called called to initialize a new device
 * so that we can connect to it.
 */

static device_handle_t *
create_new_device(search_context_t * sc, const char *host)
{
	device_handle_t *new_dev;
	hstub_cb_args_t cb_data;

	new_dev = (device_handle_t *) malloc(sizeof(*new_dev));
	if (new_dev == NULL) {
		log_message(LOGT_BG, LOGL_CRIT, 
		    "create_new_device: failed malloc");
		return (NULL);
	}

	new_dev->dev_name = strdup(host);
	new_dev->flags = 0;
	new_dev->sc = sc;

	new_dev->serviced = 0;

	cb_data.log_data_cb = dev_log_data_cb;
	cb_data.search_done_cb = dev_search_done_cb;
	cb_data.conn_down_cb = conn_down_cb;

	new_dev->dev_handle = device_init(host, (void *)new_dev, &cb_data);

	if (new_dev->dev_handle == NULL) {
		log_message(LOGT_BG, LOGL_CRIT, 
		    "create_new_device: failed device init");
		free(new_dev);
		return (NULL);
	}

	/*
	 * Put this device on the list of devices involved
	 * in the search.
	 */
	new_dev->next = sc->dev_list;
	sc->dev_list = new_dev;

	register_remote_dctl(host, new_dev);

	return (new_dev);
}




int
device_add_scope(search_context_t *sc, const char *cookie, const char *host)
{
	device_handle_t *cur_dev;

	cur_dev = lookup_dev_by_name(sc, host);
	if (cur_dev == NULL) {
		cur_dev = create_new_device(sc, host);
		if (cur_dev == NULL) {
			log_message(LOGT_BG, LOGL_CRIT, 
				    "device_add_scope: create_device failed");
			return ENOENT;
		}
	}

	return device_set_scope(cur_dev->dev_handle, cookie);
}