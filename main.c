/*
 * git-replace / replace string in git history
 * Copyright (C) 2015, Chhatoi Pritam Baral <pritam@pritambaral.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "strreplace/strreplace.hh"
#include "git.h"

int return_code = 0;

void print_usage(char *bin) {
	fprintf(stderr, "Usage: %s [options]\n", bin);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "\t-h\tHelp\n");
	fprintf(stderr, "\t-d\tPath of git repo\n");
	fprintf(stderr, "\t-p\tPattern to match against\n");
	fprintf(stderr, "\t-r\tReplacement of the pattern\n");
	fprintf(stderr, "\t-f\tRename files too\n");
	fprintf(stderr, "\t-c\tReplace in file contents too\n");
}

int main(int argc, char *argv[]) {
	int flags, opt;
	char *directory = NULL, *new_directory = NULL;
	asprintf(&directory, ".");
	char *pattern = NULL, *replacement = NULL;
	char file_rename = 0, content_replace = 0;
	if (argc == 1) {
		print_usage(argv[0]);
		return_code = 1;
		goto error;
	}
	while((opt = getopt(argc, argv, "hd:p:r:fc")) != -1) {
		switch(opt) {
			case 'd':
				free(directory);
				asprintf(&directory, "%s", optarg);
				break;
			case 'p':
				asprintf(&pattern, "%s", optarg);
				break;
			case 'r':
				asprintf(&replacement, "%s", optarg);
				break;
			case 'f':
				file_rename = 1;
				break;
			case 'c':
				content_replace = 1;
				break;
			case 'h':
				print_usage(argv[0]);
				goto error;
			default:
				print_usage(argv[0]);
				return_code = 1;
				goto error;
		}
	}

	if (pattern == NULL || replacement == NULL) {
		fprintf(stderr, "Both pattern and replacement must be supplied\n");
		return_code = 1;
		goto error;
	}
	set_regex(pattern, replacement);

	asprintf(&new_directory, "%s/new_repo", directory);
	struct stat repo_stat;
	stat(directory, &repo_stat);
	if ((return_code = mkdir(new_directory, repo_stat.st_mode)) != 0) {
		fprintf(stderr, "cannot create directory '%s': %s", new_directory, strerror(errno));
		goto error;
	}

	if ((return_code = git_init(directory, file_rename) != 0)) goto error;
	if ((return_code = git_draw_graph() != 0)) goto error;
	git_print_graph();
	if ((return_code = git_create_new_repo(new_directory) != 0)) goto error;
	if ((return_code = git_populate_new_repo() != 0)) goto error;


error:
	free(directory);
	free(new_directory);
	free(pattern);
	free(replacement);
	git_fini();
	return return_code;
}
