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

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/file.h>
#include "lib_od.h"
#include "obj_attr.h"
#include "lib_odisk.h"
#include "odisk_priv.h"

#define TEMP_ATTR_BUF_SIZE      1024
#define CACHE_DIR               "cache"

#define	OBJ_ALIGN	512
int
obj_read_attr_file(char *attr_fname, obj_attr_t *attr)
{
	int		attr_fd;
	struct stat	stats;
	int		err;
	off_t		size;
	off_t		rsize;

	/* clear the umask so we get the permissions we want */
	/* XXX do we really want to do this ??? */
	umask(0000);

	/*
	 * Open the file or create it.
	 */
	attr_fd = open(attr_fname, O_CREAT|O_RDONLY|O_DIRECT, 00777);
	if (attr_fd == -1) {
		perror("failed to open stat file");
		exit(1);
	}

	err = fstat(attr_fd, &stats);
	if (err != 0) {
		perror("failed to stat attributes\n");
		exit(1);
	}

	size = stats.st_size;

	if (size == 0) {
		attr->attr_len = 0;
		attr->attr_data = NULL;
	} else  {
		attr->attr_len = size;
		attr->attr_base = (char *)malloc(size + (2 * OBJ_ALIGN));
		if (attr->attr_base == NULL) {
			perror("no memory available");
			exit(1);
		}
		attr->attr_data = (char *)(((uint32_t)attr->attr_base + 
			OBJ_ALIGN - 1) & (~(OBJ_ALIGN - 1)));
		rsize = read(attr_fd, attr->attr_data, 
			((size + OBJ_ALIGN) &(~(OBJ_ALIGN-1))));
		if (rsize != size) {
			perror("failed to read all data \n");
			exit(1);
		}
	}

	close(attr_fd);
	return(0);
}

/*
int
obj_read_oattr(char *disk_path, uint64_t oid, char *fsig, char *iattrsig, obj_attr_t *attr)
{
	int fd;
	unsigned int *name_len;
	char *name;
	off_t *data_len;
	char *data;
	off_t           size, rsize;
	struct stat     stats;
	char attrbuf[PATH_MAX];
	uint64_t tmp1, tmp2, tmp3, tmp4;
	int err, len;
	char *base;
	char *ptr;

	if( (fsig == NULL) || (iattrsig == NULL) ) {
		printf("fsig or iattrsig is NULL\n");
		return (EINVAL);
	}
	memcpy( &tmp1, fsig, sizeof(tmp1) );
	memcpy( &tmp2, fsig+8, sizeof(tmp2) );
	memcpy( &tmp3, iattrsig, sizeof(tmp3) );
	memcpy( &tmp4, iattrsig+8, sizeof(tmp4) );

	len = snprintf(attrbuf, MAX_ATTR_NAME,
	               "%s/%s/%016llX%016llX/%016llX.%016llX%016llX",
	               disk_path, CACHE_DIR, tmp1, tmp2, oid, tmp3, tmp4);
	assert(len < (MAX_ATTR_NAME - 1));

	err = access(attrbuf, F_OK);
	if( err != 0 ) {
		//printf("file %s does not exist\n", attrbuf);
		return(EINVAL);
	}

	//printf("use oattr file %s\n", attrbuf);
	fd = open(attrbuf, O_RDONLY );
	if (fd == -1) {
		printf("failed to open file %s\n", attrbuf);
		return (EINVAL);
	}

	err = flock(fd, LOCK_EX);
	if (err != 0) {
		perror("failed to lock attributes file\n");
		close(fd);
		return (EINVAL);
	}
	err = fstat(fd, &stats);
	if (err != 0) {
		perror("failed to stat attributes\n");
		close(fd);
		return (EINVAL);
	}

	size = stats.st_size;
	if (size == 0) {
		close(fd);
		return (0);
	}
	base = (char *) malloc(size + 1);
	if(base == NULL) {
		perror("no memory available");
		close(fd);
		exit(1);
	}
	rsize = read(fd, base, size);
	if( rsize != size) {
		perror("failed to read oattr data \n");
		exit(1);
	}
	close(fd);

	ptr = base;
	while( size > 0 ){
		name_len = (unsigned int *)ptr;
		if( *name_len >= MAX_ATTR_NAME ) {
			printf("too long att name %d for oid %016llX\n", *name_len, oid);
			return(EINVAL);
		}
		name = ptr + sizeof(unsigned int);
		ptr = name + *name_len + 1;
		data_len = (off_t *)ptr;
		data = ptr + sizeof(off_t);
		err = obj_write_attr(attr, name, *data_len, data);
		if( err != 0 ) {
			printf("CHECK OBJECT %016llX ATTR FILE\n", oid);
		}
		rsize = sizeof(unsigned int) + *name_len + 1 + sizeof(off_t) + *data_len;
		//printf("size %ld, rsize %ld\n", size, rsize);
		ptr = ptr + sizeof(off_t) + *data_len;
		size -= rsize;
	}

	free(base);
	return (0);
}
*/

int
obj_write_attr_file(char *attr_fname, obj_attr_t *attr)
{
	int		attr_fd;
	off_t		wsize;
	size_t		len;
	char 	*	buf;
	int			err;
	void *		cookie;

	/* clear the umask so we get the permissions we want */
	/* XXX do we really want to do this ??? */
	umask(0000);

	/*
	 * Open the file or create it.
	 */
	attr_fd = open(attr_fname, O_CREAT|O_WRONLY|O_TRUNC, 00777);
	if (attr_fd == -1) {
		perror("failed to open stat file");
		exit(1);
	}


	err = obj_get_attr_first(attr, &buf, &len, &cookie, 0);
	while (err != ENOENT) {
		wsize = write(attr_fd, buf, len);
		if (wsize != len) {
			perror("failed to write attributes \n");
			exit(1);
		}
		err = obj_get_attr_next(attr, &buf, &len, &cookie, 0);
	}

	close(attr_fd);
	return(0);
}



static int
extend_attr_store(obj_attr_t *attr, int new_size)
{

	char *			new_attr;
	int			new_len;
	int			offset;
	attr_record_t * 	new_record;
	int			real_increment;


	/* we extend the store by multiples of attr_increment */
	real_increment = (new_size + (ATTR_INCREMENT -1)) &
	                 ~(ATTR_INCREMENT - 1);

	new_len = attr->attr_len + real_increment;

	new_attr = (char *)malloc(new_len);
	if (new_attr == NULL) {
		/* XXX log */
		return(ENOMEM);
	}
	memset( new_attr, 0, new_len);

	memcpy(new_attr, attr->attr_data, attr->attr_len);
	free(attr->attr_base);

	attr->attr_data = new_attr;
	attr->attr_base = new_attr;
	attr->attr_len = new_len;

	/*
	 * We need to make the new data a record. Ideally
	 * we will coalesce with the data before this but 
	 * this will be later.  
	 * XXX coalsce.
	 */
	offset = new_len - real_increment;
	new_record = (attr_record_t *) &new_attr[offset];
	new_record->rec_len = real_increment;
	new_record->flags = ATTR_FLAG_FREE;

	return (0);
}

/*
 * This finds a free record of the specified size.
 * This may be done by splitting another record, etc.
 *
 * If we can't find a record the necessary size, then we try
 * to extend the space used by the attributes.
 */
static attr_record_t *
find_free_record(obj_attr_t *attr, int size)
{
	attr_record_t *		cur_rec;
	attr_record_t *		new_frag;
	int			cur_offset;
	int			err;

	cur_offset = 0;
	while (1) {
		//assert(cur_offset <= attr->attr_len);
		if( cur_offset > attr->attr_len) {
			return (NULL);
		}
		if (cur_offset == attr->attr_len) {
			err = extend_attr_store(attr, size);
			if (err == ENOMEM) {
				return (NULL);
			}
		}

		if( (cur_offset + sizeof(cur_rec)) > attr->attr_len ) {
			return(NULL);
		}

		cur_rec = (attr_record_t *)&attr->attr_data[cur_offset];
		if (((cur_rec->flags & ATTR_FLAG_FREE) == ATTR_FLAG_FREE) &&
		    (cur_rec->rec_len >= size)) {
			break;
		}
		if( cur_rec->rec_len < 0 ) {
			printf("invalid rec, rec_len is %d\n", cur_rec->rec_len );
			return(NULL);
		}
		/* this one doesn't work advance */
		cur_offset += cur_rec->rec_len;
	}


	if( size < 0 ) {
		return (NULL);
	}
	if( (cur_offset+size+sizeof(cur_rec)) > attr->attr_len ) {
		return (NULL);
	}
	/* we have chunk, now decide if we want to split it */
	if ((cur_rec->rec_len - size) >= ATTR_MIN_FRAG) {
		new_frag = (attr_record_t *)&attr->attr_data[cur_offset+size];
		new_frag->rec_len = cur_rec->rec_len - size;
		new_frag->flags = ATTR_FLAG_FREE;

		/* set the new size and clear free flag */
		cur_rec->rec_len = size;
		cur_rec->flags &= ~ATTR_FLAG_FREE;
	} else {
		/* the only thing we do is clear free flag */
		cur_rec->flags &= ~ATTR_FLAG_FREE;
	}

	return(cur_rec);
}

/*
 * Mark a given attribute record as free.  This is done
 * by setting the free flag.  
 *
 * XXX in the future we should look for items before, after
 * this that also have free space to see if we can collapse them.
 *
 */

static void
free_record(obj_attr_t *attr, attr_record_t *rec)
{

	/*
	 * XXX try to colesce in later versions ???
	 */
	rec->flags |= ATTR_FLAG_FREE;
}


/*
 * Look for a record that matches a specific name string.  If
 * we find a match, we return a pointer to this record, otherwise
 * we return NULL.
 */


static attr_record_t *
find_record(obj_attr_t *attr, const char *name)
{
	int			namelen;
	int			cur_offset;
	attr_record_t *		cur_rec = NULL;

	if( name == NULL )
		return (NULL);
	namelen = strlen(name) + 1;	/* include termination */
	cur_offset = 0;
	while (cur_offset < attr->attr_len) {
		//assert( (cur_offset + sizeof(cur_rec)) <= attr->attr_len );
		if( (cur_offset + sizeof(*cur_rec)) > attr->attr_len ) {
			return(NULL);
		}
		cur_rec = (attr_record_t *)&attr->attr_data[cur_offset];
		if (((cur_rec->flags & ATTR_FLAG_FREE) == 0) &&
		    (cur_rec->name_len >= namelen) &&
		    (strcmp(name, cur_rec->data) == 0)) {
			break;
		}

		/* this one doesn't work advance */
		//assert(cur_rec->rec_len >= 0);
		if( cur_rec->rec_len < 0 ) {
			printf("invalid rec, rec_len is %d\n", cur_rec->rec_len );
			return(NULL);
		}
		cur_offset += cur_rec->rec_len;

	}
	if (cur_offset >= attr->attr_len) {
		cur_rec = NULL;
	}
	return(cur_rec);
}



/*
 * This writes an attribute related with with an object.
 */

int
obj_write_attr(obj_attr_t *attr, const char * name, off_t len, const char *data)
{
	attr_record_t *	data_rec;
	int		total_size;
	int		namelen;

	/* XXX validate object ??? */
	/* XXX make sure we don't have the same name on the list ?? */

	if( name == NULL )
		return (EINVAL);
	namelen = strlen(name) + 1;

	/* XXX this overcounts data space !! \n */
	total_size  = sizeof(*data_rec) + namelen + len;

	data_rec = find_record(attr, name);
	if (data_rec != NULL) {
		/*
		 * If we have an existing record make sure it is large
		 * enough to hold the new data.
		 */
		if (data_rec->rec_len < total_size) {
			free_record(attr, data_rec);
			data_rec = NULL;
		}
	}

	if (data_rec == NULL) {
		data_rec = find_free_record(attr, total_size);
		if (data_rec == NULL) {
			/* XXX log */
			return (ENOMEM);
		}
	}

	/*
	 * Now we have the record, so write in the data.
	 */
	data_rec->name_len = namelen;
	data_rec->data_len = len;
	memcpy(data_rec->data, name, namelen);
	memcpy(&data_rec->data[namelen], data, len);

	return(0);
}

/*
 *  This reads a piece of named ancillary data.  
 */

int
obj_read_attr(obj_attr_t *attr, const char * name, off_t *len, char *data)
{
	attr_record_t *		record;
	char *			dptr;


	record = find_record(attr, name);
	if (record == NULL) {
		return (ENOENT);
	}


	/*
	 * We set the return length to the amount of data and
	 * make sure data is large enought to hold it.  We return
	 * an error if it isnt' big enough.  It is important to set the
	 * value first because we need to set in both the error and
	 * non-error cases.
	 */
	if (record->data_len > *len) {
		*len = record->data_len;
		return (ENOMEM);
	}
	*len = record->data_len;

	/*
	 * set dptr to the data portion of the record,
	 * and copy the data in.
	 */
	dptr = &record->data[record->name_len];
	/* looks good, return the data */
	memcpy(data, dptr, record->data_len);
	return(0);
}


/*
 * Delete an attribute that was previously associated
 * with the object.
 */

int
obj_del_attr(obj_attr_t *attr, const char * name)
{
	attr_record_t *		record;

	record = find_record(attr, name);
	if (record == NULL) {
		return (ENOENT);
	}

	free_record(attr, record);
	return(0);
}

int
obj_get_attr_first(obj_attr_t *attr, char **buf, size_t *len, void **cookie,
                   int  skip_big)
{
	attr_record_t *		record;
	size_t			offset;

	offset = 0;

again:
	/* see if we have gone through all the attributes */
	if (offset >= attr->attr_len) {
		printf("first: no data \n");
		return(ENOENT);
	}

	record = (attr_record_t *)&attr->attr_data[offset];
	offset += record->rec_len;

	/* Skip any records without data */
	if (record->flags & ATTR_FLAG_FREE) {
		goto again;
	}

	/* XXX see if we should toss this */
	if ((record->data_len > ATTR_BIG_THRESH) && (skip_big)) {
		goto again;
	}

	*len = record->rec_len;
	*buf = (void *)record;

	*cookie = (void *)offset;

	return(0);
}

int
obj_get_attr_next(obj_attr_t *attr, char **buf, size_t *len, void **cookie,
                  int skip_big)
{
	attr_record_t *		record;
	size_t			offset;

	offset = (size_t)*cookie;

again:
	/* see if we have gone through all the attributes */
	if (offset >= attr->attr_len) {
		return(ENOENT);
	}

	record = (attr_record_t *)&attr->attr_data[offset];
	offset += record->rec_len;

	/* Skip any records without data */
	if (record->flags & ATTR_FLAG_FREE) {
		goto again;
	}

	if ((record->data_len > ATTR_BIG_THRESH) && (skip_big)) {
		goto again;
	}

	/* XXX see if we should toss this */

	*len = record->rec_len;
	*buf = (void *)record;

	*cookie = (void *)offset;


	return(0);
}

int
obj_read_oattr(char *disk_path, uint64_t oid, char *fsig, char *iattrsig, obj_attr_t *attr)
{
	int fd;
	unsigned int name_len;
	char name[MAX_ATTR_NAME];
	off_t data_len;
	char data[TEMP_ATTR_BUF_SIZE];
	char *ldata;
	off_t           size, rsize;
	struct stat     stats;
	char attrbuf[PATH_MAX];
	uint64_t tmp1, tmp2, tmp3, tmp4;
	int err, len;
	attr_record_t * data_rec;
	int             total_size;

	if( (fsig == NULL) || (iattrsig == NULL) ) {
		printf("fsig or iattrsig is NULL\n");
		return (EINVAL);
	}
	memcpy( &tmp1, fsig, sizeof(tmp1) );
	memcpy( &tmp2, fsig+8, sizeof(tmp2) );
	memcpy( &tmp3, iattrsig, sizeof(tmp3) );
	memcpy( &tmp4, iattrsig+8, sizeof(tmp4) );

	len = snprintf(attrbuf, MAX_ATTR_NAME,
	               "%s/%s/%016llX%016llX/%016llX.%016llX%016llX",
	               disk_path, CACHE_DIR, tmp1, tmp2, oid, tmp3, tmp4);
	assert(len < (MAX_ATTR_NAME - 1));

	err = access(attrbuf, F_OK);
	if( err != 0 ) {
		//printf("file %s does not exist\n", attrbuf);
		return(EINVAL);
	}

	//printf("use oattr file %s\n", attrbuf);
	fd = open(attrbuf, O_RDONLY );
	if (fd == -1) {
		printf("failed to open file %s\n", attrbuf);
		return (EINVAL);
	}

	err = flock(fd, LOCK_EX);
	if (err != 0) {
		perror("failed to lock attributes file\n");
		close(fd);
		return (EINVAL);
	}
	err = fstat(fd, &stats);
	if (err != 0) {
		perror("failed to stat attributes\n");
		close(fd);
		return (EINVAL);
	}
	size = stats.st_size;

	if (size == 0) {
		close(fd);
		return (0);
	}
	while(size > 0) {
		read(fd, &name_len, sizeof(unsigned int));
		if( name_len >= MAX_ATTR_NAME ) {
			printf("too long att name %d for oid %016llX\n", name_len, oid);
			close(fd);
			return(EINVAL);
		}
		assert( name_len < MAX_ATTR_NAME );
		name_len = name_len +1;
		read(fd, name, name_len);
		read(fd, &data_len, sizeof(off_t));

		total_size  = sizeof(*data_rec) + name_len + data_len;
		
		data_rec = find_record(attr, name);
        	if (data_rec != NULL) {
                	if (data_rec->rec_len < total_size) {
                        	free_record(attr, data_rec);
                        	data_rec = NULL;
                	}
        	}
                                                                                
        	if (data_rec == NULL) {
                	data_rec = find_free_record(attr, total_size);
                	if (data_rec == NULL) {
                        	return (ENOMEM);
                	}
        	}
                                                                                
        	data_rec->name_len = name_len;
        	data_rec->data_len = data_len;
        	memcpy(data_rec->data, name, name_len);
		read(fd, &data_rec->data[name_len], data_len);

		rsize = sizeof(unsigned int) + name_len + sizeof(off_t) + data_len;
		size -= rsize;
	}
	close(fd);
	return (0);
}
