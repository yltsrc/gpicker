/*
 * Copyright (C) 2008,2009 Aliaksey Kandratsenka
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * `http://www.gnu.org/licenses/'.
 */
#define _GNU_SOURCE

#include <string.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <memory.h>
#include <assert.h>
#include <arpa/inet.h> // for ntohl

#include <glib.h>

#include "timing.h"
#include "filtration.h"
#include "xmalloc.h"
#include "loading.h"
#include "inline_qsort.h"

struct vector files_vector = {.eltsize = sizeof(struct filename)};

char *project_type;
char *project_dir;
gboolean disable_bzr;
gboolean disable_hg;
char *name_separator;
char *dir_separator;
gboolean read_stdin;
char *eat_prefix = "./";
gboolean multiselect;

int gpicker_bytes_readen;

gboolean gpicker_loading_completed;
static
GMainLoop *async_loading_loop;
static int current_fd = -1;

static volatile
gboolean reading_aborted;

static
void add_filename(char *p, int dirlength)
{
	struct filename *last = vector_append(&files_vector);

	last->p = p;
	last->dirlength = dirlength;
}

#define INIT_BUFSIZE (128*1024)
#define MIN_BUFSIZE_FREE 32768

static
char *input_names(int fd, char **endp)
{
	int bufsize = INIT_BUFSIZE;
	char *buf = xmalloc(bufsize);
	int filled = 0;

	do {
		int readen = read(fd, buf+filled, bufsize-filled);
		if (reading_aborted) {
			*endp = buf;
			buf[0] = 0;
			return buf;
		}
		if (readen == 0)
			break;
		else if (readen < 0) {
			if (errno == EINTR)
				continue;
			perror("read_names");
			break;
		}
		filled += readen;
		gpicker_bytes_readen = filled;
		if (bufsize - filled < MIN_BUFSIZE_FREE) {
			bufsize = filled + MIN_BUFSIZE_FREE * 2;
			buf = xrealloc(buf, bufsize);
		}
	} while (1);
	if (filled) {
		if (buf[filled-1] != name_separator[0])
			filled++;
		buf = xrealloc(buf, filled);
		buf[filled-1] = name_separator[0];
	}
	*endp = buf+filled;
	return buf;
}

static
int filename_compare(struct filename *a, struct filename *b)
{
	return strcasecmp(a->p, b->p);
}

void read_filenames(int fd)
{
	char *endp;
	char *buf = input_names(fd, &endp);
	char *p = buf;

	int eat_prefix_len = strlen(eat_prefix);

	while (p < endp) {
		int dirlength = 0;
		char *start;
		char ch;
		if (strncmp(eat_prefix, p, eat_prefix_len) == 0)
			p += eat_prefix_len;
		start = p;
		while ((ch = *p++) != name_separator[0])
			if (ch == filter_dir_separator)
				dirlength = p - start;
		p[-1] = 0;
		add_filename(start, dirlength);
	}

	_quicksort_top(files, nfiles, sizeof(struct filename),
		       (int (*)(const void *, const void *))filename_compare, files + FILTER_LIMIT);
}

static
gboolean idle_func(gpointer _dummy)
{
	if (async_loading_loop) {
		gpicker_loading_completed = TRUE;
		g_main_loop_quit(async_loading_loop);
	}
}

static
gpointer do_read_filenames_async(gpointer _dummy)
{
	read_filenames(current_fd);
	g_idle_add(idle_func, 0);
	return 0;
}

void read_filenames_with_main_loop(int fd)
{
	GMainLoop *loop = g_main_loop_new(g_main_context_get_thread_default(), FALSE);
	async_loading_loop = loop;

	current_fd = fd;

	void *rv = g_thread_create_full(do_read_filenames_async,
					0,
					0,
					FALSE,
					TRUE,
					G_THREAD_PRIORITY_NORMAL,
					0);

	g_main_loop_run(loop);
	g_main_loop_unref(loop);

	current_fd = -1;
	async_loading_loop = 0;
}

void read_filenames_abort(void)
{
	reading_aborted = TRUE;
	if (async_loading_loop) {
		g_main_loop_quit(async_loading_loop);
	}
}

void read_filenames_from_mlocate_db(int fd)
{
	static char read_buffer[65536];

	timing_t start;
	struct stat st;
	int rv = fstat(fd, &st);
	char *data;

	start = start_timing();

	if (rv < 0) {
		perror("read_filenames_from_mlocate_db:fstat");
		exit(1);
	}
	data = mmap(0, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		perror("read_filenames_from_mlocate_db:mmap");
		exit(1);
	}

	char *prefix = data + 0x10;
	int prefix_len = strlen(prefix) + 1;
	char *end = data + st.st_size;
	char *strings = prefix + prefix_len + ntohl(*((uint32_t *)(data + 0x8))) + 0x10;

	if (prefix[prefix_len-2] == '/')
		prefix_len--;

	while (strings < end) {
		// dir name is in strings
		int len = strlen(strings);
		int read_buffer_len = strlen(strings + prefix_len);
		memcpy(read_buffer, strings + prefix_len, read_buffer_len);
		if (read_buffer_len)
			read_buffer[read_buffer_len++] = '/';

		char *p = strings + len + 1;

		while (p[0] == 1 || p[0] == 0) {
			p++;
			int p_len = strlen(p);

			if (p[-1] == 0) {
				memcpy(read_buffer+read_buffer_len, p, p_len);
				int total_len = read_buffer_len + p_len;
				char *last_slash = memrchr(read_buffer, '/', total_len);
				int dirlength = last_slash ? last_slash - read_buffer : 0;
				char *dup = malloc(total_len+1);
				memcpy(dup, read_buffer, total_len);
				dup[total_len] = 0;
				add_filename(dup, dirlength);
			}

			p += p_len + 1;
		}

		assert(p[0] == 2);

		strings = p + 0x11;
	}

	{
		start = start_timing();
		_quicksort_top(files, nfiles, sizeof(struct filename), (int (*)(const void *, const void *))filename_compare, files + FILTER_LIMIT);
		finish_timing(start, "initial qsort");
	}
}
