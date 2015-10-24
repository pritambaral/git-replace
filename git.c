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

#include <git2.h>
#include <db_185.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define SHA1LEN 40

typedef struct {
	char _[SHA1LEN+1];
} hash;
size_t hashlen = sizeof(hash);

git_repository *repo = NULL;
DB *childrenOf = NULL, *parentsOf = NULL, *refs = NULL;
int maxparents = 3;

int git_init(const char *path)
{
	git_libgit2_init();
	return git_repository_open(&repo, path);
}

int put_children_of(void *parent_id, void *child_id)
{
	DBT key, value;
	key.data = parent_id;
	key.size = hashlen;
	void *child_ids = NULL;
	if (childrenOf->get(childrenOf, &key, &value, 0) == 1) { //parent_id doesn't exist
		value.data = child_id;
		value.size = hashlen;
	} else { //parent_id already has some children
		child_ids = malloc( value.size + hashlen );
		memcpy(child_ids, value.data, value.size);
		memcpy(child_ids+value.size, child_id, hashlen);
		value.data = child_ids;
		value.size += hashlen;
	}
	childrenOf->put(childrenOf, &key, &value, 0);
	free(child_ids);
}

int git_draw_graph()
{
	free(childrenOf);
	free(parentsOf);
	free(refs);
	DB *pending = NULL, *done = NULL;
	int ret;
	/*int pending_len = 0, pending_cap = 512;*/
	/*git_oid *pending_commits = (git_oid *) malloc(pending_cap * sizeof(git_oid));*/
	DBT key, id, value;
	value.size = 0;

	childrenOf = dbopen(NULL, O_CREAT | O_RDWR, 0777, DB_HASH, NULL);
	parentsOf = dbopen(NULL, O_CREAT | O_RDWR, 0777, DB_HASH, NULL);
	refs = dbopen(NULL, O_CREAT | O_RDWR, 0777, DB_HASH, NULL);
	pending = dbopen(NULL, O_CREAT | O_RDWR, 0777, DB_HASH, NULL);
	done = dbopen(NULL, O_CREAT | O_RDWR, 0777, DB_HASH, NULL);
	if (childrenOf == NULL || parentsOf == NULL || refs == NULL || pending == NULL || done == NULL) return 2;

	// Save refs for later
	git_reference *branch;
	git_branch_t branch_type; //not really used
	git_branch_iterator *iter;
	git_oid oid;
	/* the position and size of id doesn't change */
	id.data = &oid;
	id.size = sizeof(oid);
	if (git_branch_iterator_new(&iter, repo, GIT_BRANCH_LOCAL) != 0) return 3;
	while((ret = git_branch_next(&branch, &branch_type, iter)) == 0) {
		key.data = (void *)git_reference_name(branch);
		key.size = strlen(key.data) + 1;

		git_reference_name_to_id(&oid, repo, key.data);
		/* id set before loop */

		if ((ret = refs->put(refs, &key, &id, 0)) < 0) goto error;
		//'id' is key, 'value' is value, because we need to store id
		if ((ret = pending->put(pending, &id, &value, 0)) < 0) goto error;
		git_reference_free(branch);
	}
	if (ret == GIT_ITEROVER) ret = 0;

	git_commit *commit = NULL;
	int parentCount = 0;
	void *ptr = NULL;
	git_oid *parent_oid;
	hash *parentIds = (hash *) malloc(maxparents * hashlen);
	char *commit_id = (char *) malloc(hashlen);
	key.data = commit_id;
	key.size = hashlen;
	while (pending->seq(pending, &id, &value, R_FIRST) == 0) {
		pending->del(pending, &id, R_CURSOR);
		done->put(done, &id, &value, 0);
		git_commit_free(commit);
		git_commit_lookup(&commit, repo, id.data);
		parentCount = git_commit_parentcount(commit);

		ptr = parentIds;
		if (parentCount > maxparents) {
			maxparents = parentCount;
			ptr = realloc((void *)parentIds, maxparents * hashlen);
			if (ptr == NULL) {
				ret = 2;
				goto error;
			}
			parentIds = ptr;
		}

		strncpy(commit_id, git_oid_tostr_s(id.data), hashlen);
		value.size = 0;
		for(int i = 0; i < parentCount; i++) {
			parent_oid = (git_oid *) git_commit_parent_id(commit, i);
			id.data = parent_oid; //No need to update id.size since the last usage of id was to store a (git_oid *) anyway
			if (done->get(done, &id, &value, 0) == 1) {
				pending->put(pending, &id, &value, 0);
			}
			strncpy(ptr, git_oid_tostr_s(parent_oid), hashlen);
			put_children_of(ptr, key.data);
			ptr += hashlen;
		}

		value.data = parentIds;
		value.size = hashlen * parentCount;
		parentsOf->put(parentsOf, &key, &value, 0);
	}

error:
	git_branch_iterator_free(iter);
	git_commit_free(commit);
	pending->close(pending);
	done->close(done);
	free(parentIds);
	return ret;
}

void git_print_graph()
{
	DBT id, values;
	char *child = NULL;
	while(parentsOf->seq(parentsOf, &id, &values, R_NEXT) == 0) {
		int children_count = values.size / hashlen;
		child = values.data;
		printf("Parents of: %s:\n", id.data);
		for(int i = 0; i < children_count; i++) {
			printf("\t%s;\n", child);
			child += hashlen;
		}
	}

	while(childrenOf->seq(childrenOf, &id, &values, R_NEXT) == 0) {
		int children_count = values.size / hashlen;
		child = values.data;
		printf("%d Children of: %s:\n", children_count, id.data);
		for(int i = 0; i < children_count; i++) {
			printf("\t%s;\n", child);
			child += hashlen;
		}
	}
}

void git_fini()
{
	git_repository_free(repo);
	if (childrenOf) childrenOf->close(childrenOf);
	if (parentsOf) parentsOf->close(parentsOf);
	if (refs) refs->close(refs);
	git_libgit2_shutdown();
}
