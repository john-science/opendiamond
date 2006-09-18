/*
 *      Diamond (Release 1.0)
 *      A system for interactive brute-force search
 *
 *      Copyright (c) 2002-2005, Intel Corporation
 *      All Rights Reserved
 *
 *  This software is distributed under the terms of the Eclipse Public
 *  License, Version 1.0 which can be found in the file named LICENSE.
 *  ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
 *  RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
 */


/*
 *  Copyright (c) 2006 Larry Huston <larry@thehustons.net>
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
#include <stdint.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <string.h>
#include <dirent.h>
#include <assert.h>
#include "sig_calc.h"
#include "ring.h"
#include "rstat.h"
#include "diamond_consts.h"
#include "diamond_types.h"
#include "obj_attr.h"
#include "lib_odisk.h"
#include "lib_dctl.h"
#include "lib_sstub.h"
#include "lib_log.h"
#include "rcomb.h"
#include "lib_filterexec.h"
#include "search_state.h"
#include "dctl_common.h"
#include "lib_ocache.h"
#include "sys_attr.h"
#include "obj_attr.h"

static char const cvsid[] =
    "$Header$";

#define	SAMPLE_TIME_FLOAT	0.2
#define	SAMPLE_TIME_NANO	200000000

static void    *update_bypass(void *arg);

/*
 * XXX other place 
 */
extern int      do_cleanup;

/*
 * XXX move to seperate header file !!! 
 */
#define	CONTROL_RING_SIZE	512

static int      search_free_obj(search_state_t * sstate, obj_data_t * obj);

typedef enum {
	DEV_STOP,
	DEV_TERM,
	DEV_START,
	DEV_SPEC,
	DEV_OBJ,
	DEV_BLOB
} dev_op_type_t;



typedef struct {
	char           *fname;
	void           *blob;
	int             blen;
} dev_blob_data_t;

typedef struct {
	dev_op_type_t   cmd;
	int             id;
	sig_val_t	sig;
	union {
		dev_blob_data_t bdata;
	} extra_data;
} dev_cmd_data_t;

int
search_stop(void *app_cookie, int gen_num)
{
	dev_cmd_data_t *cmd;
	search_state_t *sstate;
	int             err;

	sstate = (search_state_t *) app_cookie;

	cmd = (dev_cmd_data_t *) malloc(sizeof(*cmd));
	if (cmd == NULL) {
		return (1);
	}

	cmd->cmd = DEV_STOP;
	cmd->id = gen_num;

	err = ring_enq(sstate->control_ops, (void *) cmd);
	if (err) {
		free(cmd);
		return (1);
	}
	return (0);
}


int
search_term(void *app_cookie, int id)
{
	dev_cmd_data_t *cmd;
	search_state_t *sstate;
	int             err;

	sstate = (search_state_t *) app_cookie;

	/*
	 * Allocate a new command and put it onto the ring
	 * of commands being processed.
	 */
	cmd = (dev_cmd_data_t *) malloc(sizeof(*cmd));
	if (cmd == NULL) {
		return (1);
	}
	cmd->cmd = DEV_TERM;
	cmd->id = id;

	/*
	 * Put it on the ring.
	 */
	err = ring_enq(sstate->control_ops, (void *) cmd);
	if (err) {
		free(cmd);
		return (1);
	}
	return (0);
}

int
search_setlog(void *app_cookie, uint32_t level, uint32_t src)
{
	uint32_t        hlevel,
	                hsrc;

	hlevel = ntohl(level);
	hsrc = ntohl(src);

	log_setlevel(hlevel);
	log_settype(hsrc);
	return (0);
}



int
search_start(void *app_cookie, int id)
{
	dev_cmd_data_t *cmd;
	int             err;
	search_state_t *sstate;

	/*
	 * XXX start 
	 */

	sstate = (search_state_t *) app_cookie;
	cmd = (dev_cmd_data_t *) malloc(sizeof(*cmd));
	if (cmd == NULL) {
		return (1);
	}
	cmd->cmd = DEV_START;
	cmd->id = id;

	err = ring_enq(sstate->control_ops, (void *) cmd);
	if (err) {
		free(cmd);
		return (1);
	}


	return (0);
}


/*
 * This is called to set the searchlet for the current search.
 */

int
search_set_spec(void *app_cookie, int id, sig_val_t *spec_sig)
{
	dev_cmd_data_t *cmd;
	int             err;
	search_state_t *sstate;

	sstate = (search_state_t *) app_cookie;

	cmd = (dev_cmd_data_t *) malloc(sizeof(*cmd));
	if (cmd == NULL) {
		return (1);
	}

	cmd->cmd = DEV_SPEC;
	cmd->id = id;

	memcpy(&cmd->sig, spec_sig, sizeof(*spec_sig));
	err = ring_enq(sstate->control_ops, (void *) cmd);
	if (err) {
		free(cmd);
		return (1);
	}
	return (0);
}


int
search_set_obj(void *app_cookie, int id, sig_val_t * objsig)
{
	dev_cmd_data_t *cmd;
	int             err;
	search_state_t *sstate;

	sstate = (search_state_t *) app_cookie;

	cmd = (dev_cmd_data_t *) malloc(sizeof(*cmd));
	if (cmd == NULL) {
		return (1);
	}

	cmd->cmd = DEV_OBJ;
	cmd->id = id;

	memcpy(&cmd->sig, objsig, sizeof(*objsig));
	err = ring_enq(sstate->control_ops, (void *) cmd);
	if (err) {
		free(cmd);
		return (1);
	}
	return (0);
}

/*
 * Reset the statistics for the current search.
 */
void
clear_ss_stats(search_state_t * sstate)
{
	sstate->obj_total = 0;
	sstate->obj_processed = 0;
	sstate->obj_dropped = 0;
	sstate->obj_passed = 0;
	sstate->obj_skipped = 0;
	sstate->network_stalls = 0;
	sstate->tx_full_stalls = 0;
	sstate->tx_idles = 0;

	sstate->avg_ratio = 0.0;
	sstate->avg_int_ratio = 0;
	sstate->old_proc = 0;
}


static void
sstats_drop(void *cookie)
{
	search_state_t *sstate = (search_state_t *) cookie;

	sstate->obj_dropped++;
}


static void
sstats_process(void *cookie)
{
	search_state_t *sstate = (search_state_t *) cookie;
	sstate->obj_processed++;
}

/*
 * Take the current command and process it.  Note, the command
 * will be free'd by the caller.
 */
static void
dev_process_cmd(search_state_t * sstate, dev_cmd_data_t * cmd)
{
	int             err;

	switch (cmd->cmd) {
	case DEV_STOP:
		/*
		 * Stop the current search 
		 */
		sstate->flags &= ~DEV_FLAG_RUNNING;
		err = odisk_flush(sstate->ostate);
		assert(err == 0);

		ceval_stop(sstate->fdata);
		/*
		 * clean up the filter exec state 
		 */
		fexec_term_search(sstate->fdata);

		/*
		 * flush objects in the transmit queue 
		 */
		err = sstub_flush_objs(sstate->comm_cookie, sstate->ver_no);
		assert(err == 0);

		// usleep(1000);
		break;

	case DEV_TERM:
		break;

	case DEV_START:
		/*
		 * Start the emulated device for now.
		 * XXX do this for real later.
		 */

		/*
		 * Clear the stats.
		 */
		clear_ss_stats(sstate);

		/*
		 * JIAYING: for now, we calculate the signature for the
		 * whole library and spec file 
		 */
		ceval_init_search(sstate->fdata, sstate->cstate);

		err = odisk_reset(sstate->ostate);
		if (err) {
			/*
			 * XXX log 
			 */
			/*
			 * XXX crap !! 
			 */
			return;
		}
		err = ocache_start();
		if (err) {
			return;
		}

		/*
		 * init the filter exec code 
		 */
		fexec_init_search(sstate->fdata);
		err = ceval_start(sstate->fdata);
		if (err) {
			return;
		}

		sstate->obj_total = odisk_get_obj_cnt(sstate->ostate);
		sstate->ver_no = cmd->id;
		sstate->flags |= DEV_FLAG_RUNNING;
		break;

	case DEV_SPEC:
		sstate->ver_no = cmd->id;
		err = fexec_load_spec(&sstate->fdata, &cmd->sig);
		if (err) {
			/*
			 * XXX log 
			 */
			assert(0);
			return;
		}
		break;

	case DEV_OBJ:
		err = fexec_load_obj(sstate->fdata, &cmd->sig);
		if (err) {
			/*
			 * XXX log 
			 */
			assert(0);
			return;
		}
		break;

	case DEV_BLOB:{
			char           *name;
			int             blen;
			void           *blob;

			name = cmd->extra_data.bdata.fname;
			blen = cmd->extra_data.bdata.blen;
			blob = cmd->extra_data.bdata.blob;

			err = fexec_set_blob(sstate->fdata, name, blen, blob);
			assert(err == 0);

			free(name);
			free(blob);
			break;
	}

	default:
		printf("unknown command %d \n", cmd->cmd);
		break;

	}
}


#ifdef	XXX
static void
dynamic_update_bypass(search_state_t * sstate)
{
	int             err;
	float           avg_cost;

	err = fexec_estimate_cost(sstate->fdata, sstate->fdata->fd_perm,
				  1, 0, &avg_cost);
	if (err) {
		avg_cost = 30000000.0;
	}

	if (sstate->obj_processed != 0) {
		float           ratio;
		float           new_val;

		ratio =
		    (float) sstate->old_proc / (float) sstate->obj_processed;
		new_val = sstate->avg_ratio * ratio;
		new_val += sstate->split_ratio * (1 - ratio);
		sstate->avg_ratio = new_val;
		sstate->avg_int_ratio = (int) new_val;
		sstate->smoothed_ratio = 0.5 * sstate->smoothed_ratio +
		    0.5 * new_val;
		sstate->smoothed_int_ratio = (int) sstate->smoothed_ratio;
		sstate->old_proc = sstate->obj_processed;
	}

	sstate->split_ratio = (int) ((sstate->pend_compute *
				      (float) sstate->split_mult));

	if (sstate->split_ratio < 5) {
		sstate->split_ratio = 5;
	}
	if (sstate->split_ratio > 100) {
		sstate->split_ratio = 100;
	}
}
#else

static float
deq_beta(float proc_rate, float erate)
{
	float           beta;
	if ((proc_rate == 0.0) || (erate < 0.00001)) {
		return (0.5);
	}
	beta = proc_rate / erate;
	return (beta);
}

static float
enq_beta(float proc_rate, float drate, int pend_objs)
{
	float           beta;
	float           target;

	if ((proc_rate == 0.0) || (drate < 0.00001)) {
		return (0.5);
	}

	target = ((drate * SAMPLE_TIME_FLOAT) +
		  ((float) (20 - pend_objs))) / SAMPLE_TIME_FLOAT;
	if (target < 0.0) {
		target = 0;
	}
	beta = 1.0 / ((target / proc_rate) + 1);

	return (beta);
}


static void
dynamic_update_bypass(search_state_t * sstate)
{
	int             err;
	float           avg_cost;
	float           betain;
	float           betaout;
	float           proc_rate;
	float           erate;
	float           drate;

	err = fexec_estimate_cur_cost(sstate->fdata, &avg_cost);
	if (err) {
		avg_cost = 30000000.0;
	}

	if (sstate->obj_processed != 0) {
		float           ratio;
		float           new_val;

		ratio =
		    (float) sstate->old_proc / (float) sstate->obj_processed;
		new_val = sstate->avg_ratio * ratio;
		new_val += sstate->split_ratio * (1 - ratio);
		sstate->avg_ratio = new_val;
		sstate->avg_int_ratio = (int) new_val;
		sstate->smoothed_ratio = 0.5 * sstate->smoothed_ratio +
		    0.5 * new_val;
		sstate->smoothed_int_ratio = (int) sstate->smoothed_ratio;
		sstate->old_proc = sstate->obj_processed;
	}

	drate = sstub_get_drate(sstate->comm_cookie);
	erate = odisk_get_erate(sstate->ostate);
	proc_rate = fexec_get_prate(sstate->fdata);


	betain = deq_beta(proc_rate, erate);
	betaout = enq_beta(proc_rate, drate, sstate->pend_objs);

	// printf("betain %f betaout %f drate %f erate %f prate %f\n",
	// betain, betaout,
	// drate, erate, proc_rate);
	if (betain > betaout) {
		sstate->split_ratio = (int) (betain * 100.0);
	} else {
		sstate->split_ratio = (int) (betaout * 100.0);
	}

	if (sstate->split_ratio < 5) {
		sstate->split_ratio = 5;
	}
	if (sstate->split_ratio > 100) {
		sstate->split_ratio = 100;
	}
}
#endif

static void    *
update_bypass(void *arg)
{
	search_state_t *sstate = (search_state_t *) arg;
	uint            old_target;
	float           ratio;
	struct timespec ts;

	while (1) {
		if (sstate->flags & DEV_FLAG_RUNNING) {
			switch (sstate->split_type) {
			case SPLIT_TYPE_FIXED:
				ratio = ((float) sstate->split_ratio) / 100.0;
				fexec_update_bypass(sstate->fdata, ratio);
				fexec_update_grouping(sstate->fdata, ratio);
				break;

			case SPLIT_TYPE_DYNAMIC:
				old_target = sstate->split_ratio;
				dynamic_update_bypass(sstate);
				ratio = ((float) sstate->split_ratio) / 100.0;
				fexec_update_bypass(sstate->fdata, ratio);
				fexec_update_grouping(sstate->fdata, ratio);
				break;
			}
		}
		ts.tv_sec = 0;
		ts.tv_nsec = SAMPLE_TIME_NANO;
		nanosleep(&ts, NULL);
	}
}

/*
 * This function is called to see if we should continue
 * processing an object, or put it into the queue.
 */
static int
continue_fn(void *cookie)
{
	search_state_t *sstate = cookie;
#ifdef	XXX

	float           avg_cost;
	int             err;
	err = fexec_estimate_cost(sstate->fdata, sstate->fdata->fd_perm,
				  1, 0, &avg_cost);
	if (err) {
		avg_cost = 30000000.0;
	}

	/*
	 * XXX include input queue size 
	 */
	if ((int) (sstate->pend_compute / avg_cost) < sstate->split_bp_thresh) {
		return (0);
	} else {
		return (1);
	}
#else
	if ((sstate->pend_objs < 4)
	    && (odisk_num_waiting(sstate->ostate) > 4)) {
		return (0);
	} else {
		return (2);
	}
#endif
}


typedef struct {
	int	ver_no;
	int	max_names;
	int	num_names;
	char ** nlist;
} good_objs_t;


static void
init_good_objs(good_objs_t *gobj, int ver_no)
{
	gobj->num_names = 0;
	gobj->max_names = 256;
	gobj->ver_no = ver_no;

	gobj->nlist = malloc(sizeof(char *) * gobj->max_names);
	assert(gobj->nlist != NULL); 
}

static void
clear_good_objs(good_objs_t *gobj, int ver_no)
{
	int	i;

	for (i=0; i < gobj->num_names; i++)
		free(gobj->nlist[i]);

	gobj->num_names = 0;
	gobj->ver_no = ver_no;
}

/*
 * Helper function to save hits.
 */ 

static void
save_good_name(good_objs_t *gobj, obj_data_t *obj, int ver_no)
{
	size_t          size;
	int             err;
	unsigned char  *name;
	

	if (gobj->num_names == gobj->max_names) {
		gobj->max_names += 256;
		fprintf(stderr, "realloc: max %d \n", gobj->max_names);
		gobj->nlist = realloc(gobj->nlist, 
		    sizeof(char *) * gobj->max_names);
		assert(gobj->nlist != NULL);
	}


	err = obj_ref_attr(&obj->attr_info, DISPLAY_NAME, &size, &name);
	if (err) {
		fprintf(stdout, "name Unknown \n");
	} else {
		gobj->nlist[gobj->num_names] = strdup((char *)name);
		gobj->ver_no = ver_no;
		assert(gobj->nlist[gobj->num_names] != NULL);
		gobj->num_names++;
	}
}



/*
 * This is the main thread that executes a "search" on a device.
 * This interates it handles incoming messages as well as processing
 * object.
 */

static void    *
device_main(void *arg)
{
	search_state_t *sstate;
	dev_cmd_data_t *cmd;
	obj_data_t     *new_obj;
	int             err;
	int             any;
	int             lookahead = 0;
	int             complete;
	struct timespec timeout;
	int             force_eval;
	good_objs_t	gobj;

	sstate = (search_state_t *) arg;

	init_good_objs(&gobj, sstate->ver_no);

	log_thread_register(sstate->log_cookie);
	dctl_thread_register(sstate->dctl_cookie);

	/*
	 * XXX need to open comm channel with device
	 */


	while (1) {
		any = 0;
		/*
		 * log_message(LOGT_DISK, LOGL_TRACE, "loop top"); 
		 */
		cmd = (dev_cmd_data_t *) ring_deq(sstate->control_ops);
		if (cmd != NULL) {
			any = 1;
			dev_process_cmd(sstate, cmd);
			free(cmd);
		}

		if (sstate->pend_compute >= sstate->pend_max) {
		}

		/*
		 * XXX look for data from device to process.
		 */
		if ((sstate->flags & DEV_FLAG_RUNNING) &&
		    (sstate->pend_objs < sstate->pend_max)) {

			/*
			 * If we ever did lookahead to heat the cache
			 * we now inject those names back into the processing
			 * stages. If the lookehead was from a previous
			 * search we will just clean up otherwise tell
			 * the cache eval code about the objects.
			 */	
			if (lookahead) {
				if (gobj.ver_no != sstate->ver_no) {
					clear_good_objs(&gobj, sstate->ver_no);
				} else {
					ceval_inject_names(gobj.nlist, 
						gobj.num_names);
					init_good_objs(&gobj, sstate->ver_no);
				}
				lookahead = 0;
			}
			force_eval = 0;
			err = odisk_next_obj(&new_obj, sstate->ostate);

			/*
			 * If no fresh objects, try to get some from
			 * the partial queue.
			 */
			if (err == ENOENT) {
				err = sstub_get_partial(sstate->comm_cookie,
						      &new_obj);
				if (err == 0) {
					force_eval = 1;
					sstate->pend_objs--;
					sstate->pend_compute -=
					    new_obj->remain_compute;
				} else {
					err = ENOENT;
				}
			}
			if (err == ENOENT) {
				/*
				 * We have processed all the objects,
				 * clear the running and set the complete
				 * flags.
				 */
				sstate->flags &= ~DEV_FLAG_RUNNING;
				sstate->flags |= DEV_FLAG_COMPLETE;

				/*
				 * XXX big hack for now.  To indiate
				 * we are done we send object with
				 * no data or attributes.
				 */
				new_obj = odisk_null_obj();
				new_obj->remain_compute = 0.0;
				err = sstub_send_obj(sstate->comm_cookie,
						     new_obj, sstate->ver_no,
						     1);
				if (err) {
					/*
					 * XXX overflow gracefully  and log
					 */
				} else {
					/*
					 * XXX log 
					 */
					sstate->pend_objs++;
				}
			} else if (err) {
				printf("read error \n");
				/*
				 * printf("dmain: failed to get obj !! \n"); 
				 */
				/*
				 * sleep(1); 
				 */
				continue;
			} else {
				any = 1;
				sstate->obj_processed++;

				/*
				 * We want to process some number of objects
				 * locally to get the necessary statistics.  
				 * Setting force_eval will make sure process 
				 * the whole object.
				 */

				if ((sstate->obj_processed & 0xf) == 0xf) {
					force_eval = 1;
				}

				err = ceval_filters2(new_obj, sstate->fdata,
						   force_eval, sstate,
						   continue_fn);

				if (err == 0) {
					sstate->obj_dropped++;
					search_free_obj(sstate, new_obj);
				} else {
					sstate->obj_passed++;
					if (err == 1) {
						complete = 0;
					} else {
						complete = 1;
					}

					sstate->pend_objs++;
					sstate->pend_compute +=
					    new_obj->remain_compute;

					err = sstub_send_obj(sstate->
							   comm_cookie,
							   new_obj,
							   sstate->ver_no,
							   complete);
					if (err) {
						/*
						 * XXX overflow gracefully 
						 */
						sstate->pend_objs--;
						sstate->pend_compute -=
						    new_obj->remain_compute;
					}
				}
			}
		} else if ((sstate->flags & DEV_FLAG_RUNNING) &&
		    sstate->work_ahead) {
			/* 
			 * If work ahead is enabled we continue working
			 * on the objects.  We keep track of the ones that
			 * pass so we can re-fetch them later.
			 */
			err = odisk_next_obj(&new_obj, sstate->ostate);
			if (err == 0) {
				any = 1;	
				lookahead = 1;
				err = ceval_filters2(new_obj, sstate->fdata,
					   1, sstate, NULL);
				if (err == 0) {
					fprintf(stderr, "lookahead drop\n");
					sstate->obj_dropped++;
					sstate->obj_processed++;
				} else {
					save_good_name(&gobj, new_obj,
					   sstate->ver_no);
				}
				search_free_obj(sstate, new_obj);
			} else {
				sstate->tx_full_stalls++;
			}
		} else if ((sstate->flags & DEV_FLAG_RUNNING)) {
			sstate->tx_full_stalls++;
		}

		/*
		 * If we didn't have any work to process this time around,
		 * then we sleep on a cond variable for a small amount
		 * of time.
		 */
		if (!any) {
			timeout.tv_sec = 0;
			timeout.tv_nsec = 10000000;	/* XXX 10ms */
			nanosleep(&timeout, NULL);
		}
	}
}



/*
 * This is called when we have finished sending a log entry.  For the 
 * time being, we ignore the arguments because we only have one
 * request outstanding.  This sets a condition value to wake up
 * the main thread.
 */
int
search_log_done(void *app_cookie, char *buf, int len)
{
	search_state_t *sstate;

	sstate = (search_state_t *) app_cookie;

	pthread_mutex_lock(&sstate->log_mutex);
	pthread_cond_signal(&sstate->log_cond);
	pthread_mutex_unlock(&sstate->log_mutex);

	return (0);
}


static void    *
log_main(void *arg)
{
	search_state_t *sstate;
	char           *log_buf;
	int             err;
	struct timeval  now;
	struct timespec timeout;
	struct timezone tz;
	int             len;

	tz.tz_minuteswest = 0;
	tz.tz_dsttime = 0;

	sstate = (search_state_t *) arg;
	log_thread_register(sstate->log_cookie);
	dctl_thread_register(sstate->dctl_cookie);

	while (1) {

		len = log_getbuf(&log_buf);
		if (len > 0) {
			/*
			 * send the buffer 
			 */
			err =
			    sstub_send_log(sstate->comm_cookie, log_buf, len);
			if (err) {
				/*
				 * probably shouldn't happen
				 * but we ignore and return the data
				 */
				log_advbuf(len);
				continue;
			}

			/*
			 * wait on cv for the send to complete 
			 */
			pthread_mutex_lock(&sstate->log_mutex);
			pthread_cond_wait(&sstate->log_cond,
					  &sstate->log_mutex);
			pthread_mutex_unlock(&sstate->log_mutex);

			/*
			 * let the log library know this space can
			 * be re-used.
			 */
			log_advbuf(len);
		} else {
			gettimeofday(&now, &tz);
			pthread_mutex_lock(&sstate->log_mutex);
			timeout.tv_sec = now.tv_sec + 1;
			timeout.tv_nsec = now.tv_usec * 1000;

			pthread_cond_timedwait(&sstate->log_cond,
					       &sstate->log_mutex, &timeout);
			pthread_mutex_unlock(&sstate->log_mutex);
		}
	}
}





/*
 * This is the callback that is called when a new connection
 * has been established at the network layer.  This creates
 * new search contect and creates a thread to process
 * the data. 
 */

int
search_new_conn(void *comm_cookie, void **app_cookie)
{
	search_state_t *sstate;
	int             err;

	sstate = (search_state_t *) malloc(sizeof(*sstate));
	if (sstate == NULL) {
		*app_cookie = NULL;
		return (ENOMEM);
	}

	memset((void *) sstate, 0, sizeof(*sstate));
	/*
	 * Set the return values to this "handle".
	 */
	*app_cookie = sstate;

	/*
	 * This is called in the new process, now we initializes it
	 * log data.
	 */

	log_init(&sstate->log_cookie);
	dctl_init(&sstate->dctl_cookie);

	dctl_register_node(ROOT_PATH, SEARCH_NAME);

	dctl_register_leaf(DEV_SEARCH_PATH, "version_num",
			   DCTL_DT_UINT32, dctl_read_uint32, NULL,
			   &sstate->ver_no);
	dctl_register_leaf(DEV_SEARCH_PATH, "work_ahead", DCTL_DT_UINT32,
			   dctl_read_uint32, dctl_write_uint32, 
			   &sstate->work_ahead);
	dctl_register_leaf(DEV_SEARCH_PATH, "obj_total", DCTL_DT_UINT32,
			   dctl_read_uint32, NULL, &sstate->obj_total);
	dctl_register_leaf(DEV_SEARCH_PATH, "obj_processed", DCTL_DT_UINT32,
			   dctl_read_uint32, NULL, &sstate->obj_processed);
	dctl_register_leaf(DEV_SEARCH_PATH, "obj_dropped", DCTL_DT_UINT32,
			   dctl_read_uint32, NULL, &sstate->obj_dropped);
	dctl_register_leaf(DEV_SEARCH_PATH, "obj_pass", DCTL_DT_UINT32,
			   dctl_read_uint32, NULL, &sstate->obj_passed);
	dctl_register_leaf(DEV_SEARCH_PATH, "obj_skipped", DCTL_DT_UINT32,
			   dctl_read_uint32, NULL, &sstate->obj_skipped);

	dctl_register_leaf(DEV_SEARCH_PATH, "nw_stalls", DCTL_DT_UINT32,
			   dctl_read_uint32, NULL, &sstate->network_stalls);

	dctl_register_leaf(DEV_SEARCH_PATH, "tx_full_stalls", DCTL_DT_UINT32,
			   dctl_read_uint32, NULL, &sstate->tx_full_stalls);

	dctl_register_leaf(DEV_SEARCH_PATH, "tx_idles", DCTL_DT_UINT32,
			   dctl_read_uint32, NULL, &sstate->tx_idles);

	dctl_register_leaf(DEV_SEARCH_PATH, "pend_objs", DCTL_DT_UINT32,
			   dctl_read_uint32, NULL, &sstate->pend_objs);
	dctl_register_leaf(DEV_SEARCH_PATH, "pend_maximum", DCTL_DT_UINT32,
			   dctl_read_uint32, dctl_write_uint32,
			   &sstate->pend_max);
	dctl_register_leaf(DEV_SEARCH_PATH, "split_type", DCTL_DT_UINT32,
			   dctl_read_uint32, dctl_write_uint32,
			   &sstate->split_type);
	dctl_register_leaf(DEV_SEARCH_PATH, "split_ratio", DCTL_DT_UINT32,
			   dctl_read_uint32, dctl_write_uint32,
			   &sstate->split_ratio);
	dctl_register_leaf(DEV_SEARCH_PATH, "split_auto_step", DCTL_DT_UINT32,
			   dctl_read_uint32, dctl_write_uint32,
			   &sstate->split_auto_step);
	dctl_register_leaf(DEV_SEARCH_PATH, "split_bp_thresh", DCTL_DT_UINT32,
			   dctl_read_uint32, dctl_write_uint32,
			   &sstate->split_bp_thresh);
	dctl_register_leaf(DEV_SEARCH_PATH, "split_multiplier",
			   DCTL_DT_UINT32, dctl_read_uint32,
			   dctl_write_uint32, &sstate->split_mult);
	dctl_register_leaf(DEV_SEARCH_PATH, "average_ratio", DCTL_DT_UINT32,
			   dctl_read_uint32, NULL, &sstate->avg_int_ratio);
	dctl_register_leaf(DEV_SEARCH_PATH, "smoothed_beta", DCTL_DT_UINT32,
			   dctl_read_uint32, NULL,
			   &sstate->smoothed_int_ratio);



	dctl_register_node(ROOT_PATH, DEV_NETWORK_NODE);
	dctl_register_node(ROOT_PATH, DEV_FEXEC_NODE);

	dctl_register_node(ROOT_PATH, DEV_OBJ_NODE);

	dctl_register_node(ROOT_PATH, DEV_CACHE_NODE);


	/*
	 * initialize libfilterexec
	 */
	fexec_system_init();

	/*
	 * init the ring to hold the queue of pending operations.
	 */
	err = ring_init(&sstate->control_ops, CONTROL_RING_SIZE);
	if (err) {
		free(sstate);
		*app_cookie = NULL;
		return (ENOENT);
	}

	sstate->flags = 0;
	sstate->comm_cookie = comm_cookie;

	sstate->work_ahead = SSTATE_DEFAULT_WORKAHEAD;

	sstate->pend_max = SSTATE_DEFAULT_PEND_MAX;
	sstate->pend_objs = 0;

	sstate->pend_compute = 0.0;

	sstate->smoothed_ratio = 0.0;
	sstate->smoothed_int_ratio = 0.0;

	/*
	 * default setting way computation is split between the host
	 * and the storage device.
	 */
	sstate->split_type = SPLIT_DEFAULT_TYPE;
	sstate->split_ratio = SPLIT_DEFAULT_RATIO;
	;
	sstate->split_auto_step = SPLIT_DEFAULT_AUTO_STEP;
	sstate->split_bp_thresh = SPLIT_DEFAULT_BP_THRESH;
	sstate->split_mult = SPLIT_DEFAULT_MULT;


	/*
	 * Create a new thread that handles the searches for this current
	 * search.  (We probably want to make this a seperate process ??).
	 */

	err = pthread_create(&sstate->thread_id, PATTR_DEFAULT, device_main,
			     (void *) sstate);
	if (err) {
		/*
		 * XXX log 
		 */
		free(sstate);
		*app_cookie = NULL;
		return (ENOENT);
	}

	/*
	 * Now we also setup a thread that handles getting the log
	 * data and pusshing it to the host.
	 */
	pthread_cond_init(&sstate->log_cond, NULL);
	pthread_mutex_init(&sstate->log_mutex, NULL);
	err = pthread_create(&sstate->log_thread, PATTR_DEFAULT, log_main,
			     (void *) sstate);
	if (err) {
		/*
		 * XXX log 
		 */
		free(sstate);
		/*
		 * XXX what else 
		 */
		return (ENOENT);
	}

	/*
	 * thread to update the ration 
	 */
	err = pthread_create(&sstate->bypass_id, PATTR_DEFAULT, update_bypass,
			     (void *) sstate);
	if (err) {
		/*
		 * XXX log 
		 */
		free(sstate);
		*app_cookie = NULL;

	}
	/*
	 * Initialize our communications with the object
	 * disk sub-system.
	 */
	err = odisk_init(&sstate->ostate, NULL, sstate->dctl_cookie,
			 sstate->log_cookie);
	if (err) {
		fprintf(stderr, "Failed to init the object disk \n");
		assert(0);
		return (err);
	}

	/*
	 * JIAYING: add ocache_init 
	 */
	err = ocache_init(NULL, sstate->dctl_cookie, sstate->log_cookie);
	if (err) {
		fprintf(stderr, "Failed to init the object cache \n");
		assert(0);
		return (err);
	}

	err = ceval_init(&sstate->cstate, sstate->ostate, (void *) sstate,
			 sstats_drop, sstats_process);
	return (0);
}

/*
 * a request to get the characteristics of the device.
 */
int
search_get_char(void *app_cookie, int gen_num)
{
	device_char_t   dev_char;
	search_state_t *sstate;
	u_int64_t       val;
	int             err;


	sstate = (search_state_t *) app_cookie;

	dev_char.dc_isa = DEV_ISA_IA32;
	dev_char.dc_speed = (r_cpu_freq(&val) ? 0 : val);
	dev_char.dc_mem = (r_freemem(&val) ? 0 : val);

	/*
	 * XXX 
	 */
	err = sstub_send_dev_char(sstate->comm_cookie, &dev_char);

	return 0;
}



int
search_close_conn(void *app_cookie)
{
	/*
	 * JIAYING: may use dctl option later 
	 */
	ocache_stop(NULL);
	// exit(0);
	return (0);
}

int
search_free_obj(search_state_t * sstate, obj_data_t * obj)
{
	odisk_release_obj(obj);
	return (0);
}

/*
 * This releases an object that is no longer needed.
 */

int
search_release_obj(void *app_cookie, obj_data_t * obj)
{
	search_state_t *sstate;
	sstate = (search_state_t *) app_cookie;

	if (obj == NULL) {
		return (0);
	}

	sstate->pend_objs--;
	if (sstate->pend_objs == 0) {
		sstate->tx_idles++;
	}
	sstate->pend_compute -= obj->remain_compute;

	odisk_release_obj(obj);
	return (0);
}


int
search_set_list(void *app_cookie, int gen_num)
{
	/*
	 * printf("XXX set list \n"); 
	 */
	return (0);
}

/*
 * Get the current statistics on the system.
 */

void
search_get_stats(void *app_cookie, int gen_num)
{
	search_state_t *sstate;
	dev_stats_t    *stats;
	int             err;
	int             num_filt;
	int             len;
	float           prate;

	sstate = (search_state_t *) app_cookie;

	/*
	 * Figure out how many filters we have an allocate
	 * the needed space.
	 */
	num_filt = fexec_num_filters(sstate->fdata);
	len = DEV_STATS_SIZE(num_filt);

	stats = (dev_stats_t *) malloc(len);
	if (stats == NULL) {
		/*
		 * This is a periodic poll, so we can ingore this
		 * one if we don't have enough state.
		 */
		log_message(LOGT_DISK, LOGL_ERR, "search_get_stats: no mem");
		return;
	}

	/*
	 * Fill in the state we can handle here.
	 */
	stats->ds_objs_total = sstate->obj_total;
	stats->ds_objs_processed = sstate->obj_processed;
	stats->ds_objs_dropped = sstate->obj_dropped;
	stats->ds_objs_nproc = sstate->obj_skipped;
	stats->ds_system_load = (int) (fexec_get_load(sstate->fdata) * 100.0);
	prate = fexec_get_prate(sstate->fdata);
	stats->ds_avg_obj_time = (long long) (prate * 1000.0);
	stats->ds_num_filters = num_filt;


	/*
	 * Get the stats for each filter.
	 */
	err =
	    fexec_get_stats(sstate->fdata, num_filt, stats->ds_filter_stats);
	if (err) {
		free(stats);
		log_message(LOGT_DISK, LOGL_ERR,
			    "search_get_stats: failed to get filter stats");
		return;
	}



	/*
	 * Send the stats.
	 */
	err = sstub_send_stats(sstate->comm_cookie, stats, len);
	free(stats);
	if (err) {
		log_message(LOGT_DISK, LOGL_ERR,
			    "search_get_stats: failed to send stats");
	}
	return;
}

#define MAX_DBUF    1024
#define MAX_ENTS    512

int
search_read_leaf(void *app_cookie, char *path, int32_t opid)
{

	/*
	 * XXX hack for now 
	 */
	int             len;
	char            data_buf[MAX_DBUF];
	dctl_data_type_t dtype;
	int             err,
	                eno;
	search_state_t *sstate;

	sstate = (search_state_t *) app_cookie;

	len = MAX_DBUF;
	err = dctl_read_leaf(path, &dtype, &len, data_buf);
	/*
	 * XXX deal with ENOSPC 
	 */

	if (err) {
		len = 0;
	}

	eno = sstub_rleaf_response(sstate->comm_cookie, err, dtype, len,
				   data_buf, opid);
	assert(eno == 0);

	return (0);
}


int
search_write_leaf(void *app_cookie, char *path, int len, char *data,
		  int32_t opid)
{
	/*
	 * XXX hack for now 
	 */
	int             err,
	                eno;
	search_state_t *sstate;
	sstate = (search_state_t *) app_cookie;

	err = dctl_write_leaf(path, len, data);
	/*
	 * XXX deal with ENOSPC 
	 */

	if (err) {
		len = 0;
	}

	eno = sstub_wleaf_response(sstate->comm_cookie, err, opid);
	assert(eno == 0);

	return (0);
}

int
search_list_leafs(void *app_cookie, char *path, int32_t opid)
{

	/*
	 * XXX hack for now 
	 */
	int             err,
	                eno;
	dctl_entry_t    ent_data[MAX_ENTS];
	int             num_ents;
	search_state_t *sstate;

	sstate = (search_state_t *) app_cookie;

	num_ents = MAX_ENTS;
	err = dctl_list_leafs(path, &num_ents, ent_data);
	/*
	 * XXX deal with ENOSPC 
	 */

	if (err) {
		num_ents = 0;
	}

	eno = sstub_lleaf_response(sstate->comm_cookie, err, num_ents,
				   ent_data, opid);
	assert(eno == 0);

	return (0);
}


int
search_list_nodes(void *app_cookie, char *path, int32_t opid)
{

	/*
	 * XXX hack for now 
	 */
	int             err,
	                eno;
	dctl_entry_t    ent_data[MAX_ENTS];
	int             num_ents;
	search_state_t *sstate;

	sstate = (search_state_t *) app_cookie;

	num_ents = MAX_ENTS;
	err = dctl_list_nodes(path, &num_ents, ent_data);
	/*
	 * XXX deal with ENOSPC 
	 */

	if (err) {
		num_ents = 0;
	}

	eno = sstub_lnode_response(sstate->comm_cookie, err, num_ents,
				   ent_data, opid);
	assert(eno == 0);

	return (0);
}

int
search_set_gid(void *app_cookie, int gen_num, groupid_t gid)
{
	int             err;
	search_state_t *sstate;

	sstate = (search_state_t *) app_cookie;
	err = odisk_set_gid(sstate->ostate, gid);
	assert(err == 0);
	return (0);
}


int
search_clear_gids(void *app_cookie, int gen_num)
{
	int             err;
	search_state_t *sstate;

	/*
	 * XXX check gen num 
	 */
	sstate = (search_state_t *) app_cookie;
	err = odisk_clear_gids(sstate->ostate);
	assert(err == 0);
	return (0);
}

int
search_set_blob(void *app_cookie, int gen_num, char *name,
		int blob_len, void *blob)
{
	dev_cmd_data_t *cmd;
	int             err;
	search_state_t *sstate;
	void           *new_blob;

	sstate = (search_state_t *) app_cookie;

	cmd = (dev_cmd_data_t *) malloc(sizeof(*cmd));
	if (cmd == NULL) {
		return (1);
	}

	new_blob = malloc(blob_len);
	assert(new_blob != NULL);
	memcpy(new_blob, blob, blob_len);

	cmd->cmd = DEV_BLOB;
	cmd->id = gen_num;

	cmd->extra_data.bdata.fname = strdup(name);
	assert(cmd->extra_data.bdata.fname != NULL);
	cmd->extra_data.bdata.blen = blob_len;
	cmd->extra_data.bdata.blob = new_blob;


	err = ring_enq(sstate->control_ops, (void *) cmd);
	if (err) {
		free(cmd);
		assert(0);
		return (1);
	}
	return (0);
}

extern int      fexec_cpu_slowdown;

/*
 * XXXX remove this 
 */
int
search_set_offload(void *app_cookie, int gen_num, uint64_t load)
{
	return (0);
}
