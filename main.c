#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "git.h"

int return_code = 0;

void print_usage(char *bin) {
	fprintf(stderr, "Usage: %s [options]\n", bin);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "\t-h\tHelp\n");
	fprintf(stderr, "\t-d\tPath of .git repo\n");
	fprintf(stderr, "\t-p\tPattern to match against\n");
	fprintf(stderr, "\t-r\tReplacement of the pattern\n");
	fprintf(stderr, "\t-f\tRename files too\n");
	fprintf(stderr, "\t-c\tReplace in file contents too\n");
}

int main(int argc, char *argv[]) {
	int flags, opt;
	char *directory = NULL;
	asprintf(&directory, ".git");
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
				asprintf(&directory, optarg);
				break;
			case 'p':
				asprintf(&pattern, optarg);
				break;
			case 'r':
				asprintf(&replacement, optarg);
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

	if ((return_code = git_init(directory) != 0)) goto error;
	if ((return_code = git_draw_graph() != 0)) goto error;
    git_print_graph();

error:
	free(directory);
	free(pattern);
	free(replacement);
	git_fini();
	return return_code;
}
