/* This is a multi-thread download tool, for pan.baidu.com
 *   		Copyright (C) 2015  Yang Zhang <yzfedora@gmail.com>
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *************************************************************************/
#define _POSIX_C_SOURCE	200809L
#define _DEFAULT_SOURCE
#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>
#include "dlpart.h"
#include "dlcommon.h"
#include "err_handler.h"

#define	DLPART_NEW_TIMES	60
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


/*
 * Suppoort continuous download:
 * | file | nthreads | 1th-thread range | 2th-thread range |...
 * |length|  4bytes  |    8bytes x 2    |   8bytes x 2     |...
 */
static void dlpart_update(struct dlpart *dp)
{
	struct dlinfo *dl = dp->dp_info;
	int ret;
	int local = dl->di_local;
	typeof(dp->dp_start) start = dp->dp_start;
	typeof(dp->dp_end) end = dp->dp_end;
	ssize_t offset = dl->di_length + sizeof(dl->di_nthreads) +
			sizeof(start) * dp->dp_no * 2;

try_pwrite_range_start:
	ret = pwrite(local, &start, sizeof(start), offset);
	if (ret != sizeof(start))
		goto try_pwrite_range_start;

try_pwrite_range_end:
	ret = pwrite(local, &end, sizeof(end), offset + sizeof(start));
	if (ret != sizeof(end))
		goto try_pwrite_range_end;
}


/* HTTP Header format:
 * 	GET url HTTP/1.1\r\n
 * 	Range: bytes=x-y\r\n
 */
void dlpart_send_header(struct dlpart *dp)
{
	char sbuf[DLPART_BUFSZ];
	struct dlinfo *dl = dp->dp_info;

	sprintf(sbuf, "GET %s HTTP/1.1\r\n"
		      "Host: %s\r\n"
		      "Range: bytes=%ld-%ld\r\n\r\n",
		      geturi(dl->di_url, dl->di_host), dp->dp_info->di_host,
		      (long)dp->dp_start, (long)dp->dp_end);

#ifdef __DEBUG__
	printf("---------------Sending Header(%ld-%ld)----------------\n"
		"%s---------------------------------------------------\n",
		dp->dp_start, dp->dp_end, sbuf);
#endif
	nwrite(dp->dp_remote, sbuf, strlen(sbuf));
}

int dlpart_recv_header(struct dlpart *dp)
{
	struct dlinfo *dl = dp->dp_info;
	int is_header = 1, code;
	ssize_t start, end;
	char *sp, *p, *ep;

	while (is_header) {
		dp->read(dp);
		if (dp->dp_nrd <= 0)
			return -1;

		if ((sp = strstr(dp->dp_buf, "\r\n\r\n"))) {
			*sp = 0;
			sp += 4;
			dp->dp_nrd -= (strlen(dp->dp_buf) + 4);
			is_header = 0;
		}
#ifdef __DEBUG__
		printf("------------Receiving Heading(%ld-%ld)----------\n"
				"%s\n-----------------------------------\n",
				dp->dp_start, dp->dp_end, dp->dp_buf);
#endif
		/* multi-thread download, the response code should be 206. */
		code = getrcode(dp->dp_buf);
		if (code != 206 && code != 200) {
			return -1;
		} else if (code == 200 && dl->di_nthreads != 1) {
			err_msg(0, "Host: %s does not support multi-thread "
				   "download.\n try option '-n 1' again.\n",
				    dl->di_host);
		}


#define RANGE	"Content-Range: bytes "
		if (NULL != (p = strstr(dp->dp_buf, RANGE))) {
			start = strtol(p + sizeof(RANGE) - 1, &ep, 10);
			if  (ep && *ep == '-')
				end = strtol(ep + 1, NULL, 10);

			if (start != dp->dp_start || end != dp->dp_end) {
				err_msg(0, "response range error: %ld-%ld "
					 "(%ld-%ld)\n",
					 start, end, dp->dp_start, dp->dp_end);
				return -1;
			}
		}
	}

	/* FUCKING THE IMPLEMENTATION OF STRNCPY! try using memcpy() instead.
	 * strncpy(dp->dp_buf, sp, dp->dp_nrd);
	 */
	memmove(dp->dp_buf, sp, dp->dp_nrd);
	return 0;
}

void dlpart_read(struct dlpart *dp)
{
	/* 
	 * If any errors occured, -1 will returned, and errno will be
	 * set correctly
	 */
	dp->dp_nrd = read(dp->dp_remote, dp->dp_buf, DLPART_BUFSZ - 1);
	if (dp->dp_nrd <= 0)
		return;
	
	dp->dp_buf[dp->dp_nrd] = 0;	
}

/*
 * Write 'dp->dp_nrd' data which pointed by 'dp->dp_buf' to local file.
 * And the offset 'dp->dp_start' will be updated also.
 */
void dlpart_write(struct dlpart *dp, ssize_t *total_read,
		ssize_t *bytes_per_sec)
{
	int s, n, len = dp->dp_nrd;
	char *buf = dp->dp_buf;

	while (len > 0) {
		/*debug("pwrite %d bytes >> fd(%d), offset %ld\n",
			len, fd, offset);*/
		n = pwrite(dp->dp_info->di_local, buf, len, dp->dp_start);
		if (n > 0) {
			len -= n;
			buf += n;
			dp->dp_start += n;
		} else if (n == 0) {
			err_exit(0, "pwrite end-of-file");
		} else {
			if (errno == EINTR)
				continue;
			err_msg(errno, "pwrite");
		}
	}

	/*
	 * only lock the dp->read call is necessary, since it may
	 * update the global varibale total_read and bytes_per_sec.
	 */
	if ((s = pthread_mutex_lock(&mutex)) != 0)
		err_msg(s, "pthread_mutex_lock");

	/* 
	 * Since we updated the 'total_read' and 'bytes_per_sec', calling
	 * this dlpart_read need to be protected by pthead_mutex
	 */
	if (total_read)
		*total_read += dp->dp_nrd;
	if (bytes_per_sec)
		*bytes_per_sec += dp->dp_nrd;
	
	if ((s = pthread_mutex_unlock(&mutex)) != 0)
		err_msg(s, "pthread_mutex_unlock");

	dlpart_update(dp);
}

void dlpart_delete(struct dlpart *dp)
{
	if (close(dp->dp_remote) == -1)
		err_msg(errno, "close %d", dp->dp_remote);
	free(dp);
}

struct dlpart *dlpart_new(struct dlinfo *dl, ssize_t start, ssize_t end, int no)
{
	unsigned int try_times = 0;
	struct dlpart *dp;

	if (!(dp = (struct dlpart *)malloc(sizeof(*dp))))
		return NULL;

	memset(dp, 0, sizeof(*dp));
	dp->dp_no = no;
	dp->dp_remote = dl->connect(dl);
	dp->dp_info = dl;

	dp->sendhdr = dlpart_send_header;
	dp->recvhdr = dlpart_recv_header;
	dp->read = dlpart_read;
	dp->write = dlpart_write;
	dp->delete = dlpart_delete;

	dp->dp_start = start;
	dp->dp_end = end;
	dlpart_update(dp);	/* Force to write the record into end of file*/
try_sendhdr_again:
	dp->sendhdr(dp);
	if (dp->recvhdr(dp) == -1) {
		if (try_times++ > DLPART_NEW_TIMES)
			return NULL;

		if (close(dp->dp_remote) == -1)
			err_msg(errno, "close");

		usleep(100000);
		dp->dp_remote = dl->connect(dl);
		goto try_sendhdr_again;
	}

	/*
	 * for to get maximun of concurrency, set dp->dp_remote to nonblock.
	 */
	int flags;
	if ((flags = fcntl(dp->dp_remote, F_GETFL, 0)) == -1)
		err_exit(errno, "fcntl-getfl");
	flags |= O_NONBLOCK;
	if (fcntl(dp->dp_remote, F_SETFL, flags) == -1)
		err_exit(errno, "fcntl-setfl");

	return dp;
}
