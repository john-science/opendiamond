/*
 *
 *
 *                          Diamond 1.0
 * 
 *            Copyright (c) 2002-2004, Intel Corporation
 *                         All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *    * Neither the name of Intel nor the names of its contributors may
 *      be used to endorse or promote products derived from this software 
 *      without specific prior written permission.
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

/*
 * These file handles a lot of the device specific code.  For the current
 * version we have state for each of the devices.
 */
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <netdb.h>
#include <string.h>
#include <assert.h>
#include "ring.h"
#include "obj_attr.h"
#include "lib_od.h"
#include "lib_odisk.h"
#include "lib_searchlet.h"
#include "socket_trans.h"
#include "lib_dctl.h"
#include "lib_sstub.h"
#include "sstub_impl.h"


static int
queued_objects(cstate_t *cstate)
{
	int	count;

	count = ring_2count(cstate->partial_obj_ring);
	count += ring_2count(cstate->complete_obj_ring);
	return(count);
}

static int
drop_attributes(cstate_t *cstate)
{

	unsigned int	rv;
	int	tx_count;
	if ((cstate->attr_policy == NW_ATTR_POLICY_PROPORTIONAL) ||
	    (cstate->attr_policy == NW_ATTR_POLICY_FIXED)) {
		rv = random();
		if (rv > cstate->attr_threshold) {
			return(1);
		} else {
			return(0);
		}
	} else if (cstate->attr_policy == NW_ATTR_POLICY_QUEUE) {
		tx_count = queued_objects(cstate);
		if ((tx_count > DESIRED_MAX_TX_THRESH) &&
		    (cstate->cc_credits >= DESIRED_MAX_CREDITS)) {
			return(1);
		} else {
			return(0);
		}
	}
	return(0);

}

static float
prop_get_tx_ratio(cstate_t *cstate)
{
	float	ratio;
	int		count;

	count = queued_objects(cstate);
	ratio = (float)count/(float)DESIRED_MAX_TX_QUEUE;
	if (ratio > 1.0) {
		ratio = 1.0;
	} else if (ratio < 0) {
		ratio = 0.0;
	}

	return(ratio);
}

static float
prop_get_rx_ratio(cstate_t *cstate)
{
	float	ratio;

	ratio = (float)cstate->cc_credits/(float)DESIRED_MAX_CREDITS;
	if (ratio > 1.0) {
		ratio = 1.0;
	} else if (ratio < 0) {
		ratio = 0.0;
	}
	return(ratio);
}


static void
update_attr_policy(cstate_t *cstate)
{
	float tx_ratio;
	float rx_ratio;

	/*
	 * we only do updates for the proportional scheduling today.
	 */
	if (cstate->attr_policy == NW_ATTR_POLICY_PROPORTIONAL) {
		tx_ratio = prop_get_tx_ratio(cstate);
		rx_ratio = prop_get_rx_ratio(cstate);
		if (rx_ratio > tx_ratio) {
			cstate->attr_threshold = rx_ratio * RAND_MAX;
			cstate->attr_ratio = (int) (rx_ratio * 100.0);
		} else {
			cstate->attr_threshold = tx_ratio * RAND_MAX;
			cstate->attr_ratio = (int) (tx_ratio * 100.0);
		}

	} else if (cstate->attr_policy == NW_ATTR_POLICY_FIXED) {
		if (cstate->attr_ratio == 100) {
			cstate->attr_threshold = RAND_MAX;
		} else {
			cstate->attr_threshold = (RAND_MAX/100) * cstate->attr_ratio;
		}
	}
	return;
}


static int
sstub_attr_len(obj_data_t *obj, int drop_attrs)
{
	int	err;
	size_t	len, total;
	char *	buf;
	void *	cookie;

	total = 0;

	err = obj_get_attr_first(&obj->attr_info, &buf, &len, &cookie, drop_attrs);
	while (err == 0) {
		total += len;
		err = obj_get_attr_next(&obj->attr_info, &buf, &len, &cookie,
		                        drop_attrs);
	}

	return(total);
}


void
sstub_write_data(listener_state_t *lstate, cstate_t *cstate)
{
	obj_data_t	*obj;
	int		sent;
	void *		vnum;
	void *		junk;
	int		err;
	int		header_remain=0, header_offset=0;
	size_t		attr_remain=0, attr_offset=0;
	int		data_remain=0, data_offset=0;
	char *		data;


	if (cstate->data_tx_state == DATA_TX_NO_PENDING) {
		pthread_mutex_lock(&cstate->cmutex);
		err = ring_2deq(cstate->complete_obj_ring, &junk, &vnum);
		/*
		 * If we don't get a complete object, look for a partial.
		 */
		if (err) {
			err = ring_2deq(cstate->partial_obj_ring, &junk, &vnum);
		}

		/*
		 * if there is no other data, then clear the obj data flag
		 */
		if (err) {
			cstate->flags &= ~CSTATE_OBJ_DATA;
			pthread_mutex_unlock(&cstate->cmutex);
			return;
		}
		obj = (obj_data_t *)junk;
		pthread_mutex_unlock(&cstate->cmutex);

		/*
			 * periodically we want to update our send policy if
			 * we are dynamic.
			 */
		if ((cstate->stats_objs_tx & 0xF) == 0) {
			update_attr_policy(cstate);
		}

		cstate->data_tx_obj = obj;

		/*
		 * Decide if we are going to send the attributes on this
		 * object.
		 */
		cstate->drop_attrs = drop_attributes(cstate);

		/*
		 * Construct the header for the object we are going
		 * to send out.
		 */
		cstate->data_tx_oheader.obj_magic = htonl(OBJ_MAGIC_HEADER);
		cstate->data_tx_oheader.attr_len  =
		    htonl(sstub_attr_len(obj, cstate->drop_attrs));
		cstate->data_tx_oheader.data_len  =
		    htonl((int)obj->data_len);
		cstate->data_tx_oheader.version_num  = htonl((int)vnum);



		/* setup the remain and offset counters */
		header_offset = 0;
		header_remain = sizeof(cstate->data_tx_oheader);

		/* setup attr setup */
		err = obj_get_attr_first(&cstate->data_tx_obj->attr_info,
		                         &cstate->attr_buf, &cstate->attr_remain,
		                         &cstate->attr_cookie,  cstate->drop_attrs);
		attr_offset = 0;
		if (err == ENOENT) {
			attr_remain = 0;
		} else {
			attr_remain = cstate->attr_remain;
		}
		data_offset = 0;
		data_remain = obj->data_len;

	} else if (cstate->data_tx_state == DATA_TX_HEADER) {
		obj = cstate->data_tx_obj;

		header_offset = cstate->data_tx_offset;
		header_remain = sizeof(cstate->data_tx_oheader) -
		                header_offset;

		/* setup the attribute information */
		err = obj_get_attr_first(&cstate->data_tx_obj->attr_info,
		                         &cstate->attr_buf, &cstate->attr_remain,
		                         &cstate->attr_cookie,  cstate->drop_attrs);
		attr_offset = 0;
		if (err == ENOENT) {
			attr_remain = 0;
		} else {
			attr_remain = cstate->attr_remain;
		}

		data_offset = 0;
		data_remain = obj->data_len;

	} else if (cstate->data_tx_state == DATA_TX_ATTR) {
		obj = cstate->data_tx_obj;
		header_offset = 0;
		header_remain = 0;
		attr_offset = cstate->data_tx_offset;
		attr_remain = cstate->attr_remain;
		data_offset = 0;
		data_remain = obj->data_len;
	} else {
		assert(cstate->data_tx_state == DATA_TX_DATA);

		obj = cstate->data_tx_obj;

		header_offset = 0;
		header_remain = 0;
		attr_offset = 0;
		attr_remain = 0;
		data_offset = cstate->data_tx_offset;
		data_remain = obj->data_len - data_offset;
	}

	/*
	 * If we haven't sent all the header yet, then go ahead
	 * and send it.
	 */

	if (header_remain > 0) {
		data = (char *)&cstate->data_tx_oheader;
		sent = send(cstate->data_fd, &data[header_offset],
		            header_remain, 0);

		if (sent < 0) {
			if (errno == EAGAIN) {
				cstate->data_tx_state = DATA_TX_HEADER;
				cstate->data_tx_offset = header_offset;
				return;
			} else {
				/* XXX what errors should we handles ?? */
				perror("send oheader ");
				printf("XXX error while sending oheader\n");
				exit(1);
			}

		}
		if (sent != header_remain) {
			cstate->data_tx_state = DATA_TX_HEADER;
			cstate->data_tx_offset = header_offset + sent;
			return;
		}
	}

	/*
	 * If there is still some attributes to send, then go ahead and
	 * send it.
	 */

more_attrs:

	if (attr_remain) {
		sent = send(cstate->data_fd, &cstate->attr_buf[attr_offset],
		            attr_remain, 0);

		if (sent < 0) {
			if (errno == EAGAIN) {
				cstate->data_tx_state = DATA_TX_ATTR;
				cstate->data_tx_offset = attr_offset;
				cstate->attr_remain = attr_remain;
				return;
			} else {
				/* XXX what errors should we handles ?? */
				perror("send attr ");
				exit(1);
			}

		}
		if (sent != attr_remain) {
			cstate->data_tx_state = DATA_TX_ATTR;
			cstate->data_tx_offset = attr_offset + sent;
			cstate->attr_remain = attr_remain - sent;
			return;
		} else {
			err = obj_get_attr_next(&cstate->data_tx_obj->attr_info,
			                        &cstate->attr_buf, &attr_remain,
			                        &cstate->attr_cookie,  cstate->drop_attrs);
			if (err == ENOENT) {
				attr_remain = 0;
			} else {
				attr_offset = 0;
				goto more_attrs;
			}
		}
		/* XXX fix up attr bytes send !!! */
	}


	/*
	 * If there is still data to be sent, then go ahead and
	 * send it.
	 */

	if (data_remain) {
		data = (char *)cstate->data_tx_obj->data;

		sent = send(cstate->data_fd, &data[data_offset],
		            data_remain, 0);

		if (sent < 0) {
			if (errno == EAGAIN) {
				cstate->data_tx_state = DATA_TX_DATA;
				cstate->data_tx_offset = data_offset;
				return;
			} else {
				/* XXX what errors should we handles ?? */
				perror("send data ");
				exit(1);
			}

		}
		if (sent != data_remain) {
			cstate->data_tx_state = DATA_TX_DATA;
			cstate->data_tx_offset = data_offset + sent;
			return;
		}
	}

	/* some stats */
	cstate->stats_objs_tx++;
	cstate->stats_objs_attr_bytes_tx += obj->attr_info.attr_len;
	cstate->stats_objs_data_bytes_tx += obj->data_len;
	cstate->stats_objs_hdr_bytes_tx += sizeof(cstate->data_tx_oheader);
	cstate->stats_objs_total_bytes_tx += sizeof(cstate->data_tx_oheader) +
	                                     obj->attr_info.attr_len + obj->data_len;

	/*
	 * If we make it here, then we have sucessfully sent
	 * the object so we need to make sure our state is set
	 * to no data pending, and we will call the callback the frees
	 * the object.
	 */

	cstate->data_tx_state = DATA_TX_NO_PENDING;
	(*lstate->release_obj_cb)(cstate->app_cookie, cstate->data_tx_obj);

	/* decrement credit count */
	/* XXX do I need to lock */
	if (cstate->cc_credits > 0) {
		cstate->cc_credits--;
	}

	return;
}

void
sstub_except_data(listener_state_t *lstate, cstate_t *cstate)
{
	printf("XXX except data \n");
	/* Handle the case where we are shutting down */
	if (cstate->flags & CSTATE_SHUTTING_DOWN) {
		return;
	}

	return;
}



void
sstub_read_data(listener_state_t *lstate, cstate_t *cstate)
{

	char *	data;
	size_t	data_size;
	size_t	rsize;

	/* Handle the case where we are shutting down */
	if (cstate->flags & CSTATE_SHUTTING_DOWN) {
		return;
	}

	/* XXX handle case where we did read all the data last time.
	 * XXXX this should probably never occur ...
	    */
	data = (char *)&cstate->cc_msg;
	data_size = sizeof(credit_count_msg_t);
	rsize = recv(cstate->data_fd, data, data_size, 0);

	/* make sure we read the whole message and that it has the right header */
	if (rsize == -1) {
		perror("sstub_read_data:");
		return;
	} else if (rsize == 0) {
		//printf("no data \n");
		return;
	} else if (rsize != data_size) {
		printf("bad readsize %d  %d\n", rsize, data_size);
		exit(1);
	}
	assert(ntohl(cstate->cc_msg.cc_magic) == CC_MAGIC_HEADER);

	/* update the count */
	cstate->cc_credits = ntohl(cstate->cc_msg.cc_count);
	return;
}

