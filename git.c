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
#include "strreplace/strreplace.hh"

#define SHA1LEN 40

typedef struct {
	char _[SHA1LEN+1];
} hash;
size_t hashlen = sizeof(hash);

git_repository *repo = NULL, *new_repo = NULL;
DB *childrenOf = NULL, *parentsOf = NULL, *refs = NULL, *oldToNew = NULL;
git_commit *root = NULL;
int maxparents = 3;
int rename_files = 0;

int git_init(const char *path, int rename_flag)
{
	git_libgit2_init();
	rename_files = rename_flag;
	return git_repository_open(&repo, path);
}

void put_children_of(void *parent_id, void *child_id)
{
	DBT key, value;
	key.data = parent_id;
	key.size = sizeof(git_oid);
	void *child_ids = NULL;
	if (childrenOf->get(childrenOf, &key, &value, 0) == 1) { //parent_id doesn't exist
		value.data = child_id;
		value.size = sizeof(git_oid);
	} else { //parent_id already has some children
		child_ids = malloc( value.size + sizeof(git_oid) );
		memcpy(child_ids, value.data, value.size);
		memcpy(child_ids+value.size, child_id, sizeof(git_oid));
		value.data = child_ids;
		value.size += sizeof(git_oid);
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
	git_oid *parentIds = (git_oid *) malloc(maxparents * sizeof(git_oid));
	key.size = sizeof(git_oid);
	while (pending->seq(pending, &id, &value, R_FIRST) == 0) {
		pending->del(pending, &id, R_CURSOR);
		done->put(done, &id, &value, 0);
		if (root != commit) git_commit_free(commit);
		git_commit_lookup(&commit, repo, id.data);
		parentCount = git_commit_parentcount(commit);
		if (parentCount == 0) root = commit;

		ptr = parentIds;
		if (parentCount > maxparents) {
			maxparents = parentCount;
			ptr = realloc((void *)parentIds, maxparents * sizeof(git_oid));
			if (ptr == NULL) {
				ret = 2;
				goto error;
			}
			parentIds = ptr;
		}

		key.data = id.data;
		value.size = 0;
		for(int i = 0; i < parentCount; i++) {
			parent_oid = (git_oid *) git_commit_parent_id(commit, i);
			id.data = parent_oid; //No need to update id.size since the last usage of id was to store a (git_oid *) anyway
			if (done->get(done, &id, &value, 0) == 1) {
				pending->put(pending, &id, &value, 0);
			}
			memcpy(ptr, parent_oid, sizeof(git_oid));
			put_children_of(ptr, key.data);
			ptr += sizeof(git_oid);
		}

		value.data = parentIds;
		value.size = sizeof(git_oid) * parentCount;
		parentsOf->put(parentsOf, &key, &value, 0);
	}

error:
	git_branch_iterator_free(iter);
	if (root != commit) git_commit_free(commit);
	pending->close(pending);
	done->close(done);
	free(parentIds);
	return ret;
}

int git_create_new_repo(const char *path)
{
	return git_repository_init(&new_repo, path, 0);
}

/*!
 * @brief 			Recursively copy a tree
 *
 * @param tree 		the tree to copy
 *
 * @return 			oid of the the new tree object; or NULL on failure
 */
git_oid *copy_tree_r(git_tree *tree)
{
	const char *name;
	const git_tree_entry *entry = NULL;
	static git_oid buf_oid;
	git_oid *new_oid = NULL;
	git_otype type;
	size_t entrycount = git_tree_entrycount(tree);
	git_treebuilder *builder = NULL;
	git_treebuilder_new(&builder, new_repo, NULL);

	for (size_t i = 0; i < entrycount; i++) {
		entry = git_tree_entry_byindex(tree, i);
		type = git_tree_entry_type(entry);

		if (type == GIT_OBJ_TREE) {
			git_tree *local_tree = NULL;
			git_tree_lookup(&local_tree, repo, git_tree_entry_id(entry));
			new_oid = copy_tree_r(local_tree);
			git_tree_free(local_tree);
			if (new_oid == NULL) goto error;
		} else if (type == GIT_OBJ_BLOB) {
			new_oid = &buf_oid;
			git_blob *local_blob;
			git_blob_lookup(&local_blob, repo, git_tree_entry_id(entry));
			git_blob_create_frombuffer(new_oid, new_repo, git_blob_rawcontent(local_blob), git_blob_rawsize(local_blob));
			git_blob_free(local_blob);
			//TODO: implement replacing file contents
		} else if (type == GIT_OBJ_COMMIT) {
			new_oid = (git_oid *) git_tree_entry_id(entry);
		} else {
			fprintf(stderr, "Can't handle tree entry of type %d\n", type);
			new_oid = NULL;
		}

		if (rename_files)
			name = replace(git_tree_entry_name(entry));
		else
			name = git_tree_entry_name(entry);

		git_treebuilder_insert(NULL, builder, name, new_oid, git_tree_entry_filemode(entry));
	}

	new_oid = &buf_oid;
	git_treebuilder_write(new_oid, builder);

error:
	git_treebuilder_free(builder);
	return new_oid;
}

/*!
 * @brief 			make a copy in the new repo of a commit from the old repo
 *
 * @param commit	the commit to copy
 *
 * @return			0 for success
 */
int copy_commit(git_commit *commit)
{
	int ret = 0;
	git_tree *tree = NULL, *new_tree = NULL;
	git_oid *new_tree_oid = NULL, new_commit_oid;
	const char *message_encoding = NULL, *message = NULL;

	// Get array of new parents
	git_oid **parent_oids = NULL;
	git_commit **new_parents = NULL;
	DBT key, value;
	key.data = (void *) git_commit_id(commit);
	key.size = sizeof(git_oid);

	parentsOf->get(parentsOf, &key, &value, 0);
	parent_oids = value.data;
	size_t parentCount = value.size / sizeof(git_oid);
	new_parents = (git_commit **) malloc(parentCount * sizeof(git_commit *));
	git_oid *new_parent_oid = NULL;

	for (size_t i = 0; i < parentCount; i++) {
		key.data = parent_oids[i];
		key.size = sizeof(git_oid);

		if (oldToNew->get(oldToNew, &key, &value, 0) != 0) {
			fprintf(stderr, "Couldn't find equivalent in new repo of commit %s\n", git_oid_tostr_s(git_commit_id(commit)));
			ret = 1;
			goto error;
		}

		new_parents[i] = value.data;
	}
	// Got new parents

	if ((message_encoding = git_commit_message_encoding(commit)) != NULL) // If the message encoding is not UTF-8, let's not touch it
		message = git_commit_message(commit);
	else
		message = replace(git_commit_message(commit));

	if ((ret = git_commit_tree(&tree, commit)) != 0) goto error;
	if ((new_tree_oid = copy_tree_r(tree)) == NULL) {ret = -15; goto error;}
	git_tree_lookup(&new_tree, new_repo, new_tree_oid);

	if ((ret = git_commit_create(&new_commit_oid, new_repo, NULL,
					git_commit_author(commit), git_commit_committer(commit),
					message_encoding, message, new_tree,
					parentCount,
					(const git_commit **)new_parents))
			!= 0) {
		fprintf(stderr, "Couldn't copy commit %s into new repo\n", git_oid_tostr_s(git_commit_id(commit)));
		goto error;
	} else {
		key.data = (void *) git_commit_id(commit);
		key.size = value.size = sizeof(git_oid);
		value.data = &new_commit_oid;
		oldToNew->put(oldToNew, &key, &value, 0);
	}

error:
	free(new_parents);
	git_tree_free(tree);
	return ret;
}

int add_pending(git_commit *commit, DB *db)
{
	const git_oid *parent_oid = NULL;
	int parentCount = 0, pendingIndex = 0, ret = 0, i;
	DBT key, value;
	if ((parentCount = git_commit_parentcount(commit)) == 0) pendingIndex = 0;
	else if ((ret = db->seq(db, &key, &value, R_LAST)) < 0) goto error;
	else if (ret == 1) ret = pendingIndex = 0; // There are no elements in the pending list
	else pendingIndex = *(int *)key.data;

	for(i = 0; i < parentCount; i++) {
		parent_oid = git_commit_parent_id(commit, i);
		key.data = (void *) parent_oid;
		key.size = sizeof(git_oid);
		if (oldToNew->get(oldToNew, &key, &value, 0) != 0) {
			fprintf(stderr, "Couldn't get corresponding commit of %s in new repo - while adding pending\n", git_oid_tostr_s(key.data));
			break;
		}
	}
	if (i != parentCount) return 0;

	pendingIndex++;
	key.data = &pendingIndex;
	key.size = sizeof(pendingIndex);
	value.data = (void *) git_commit_id(commit);
	value.size = sizeof(git_oid);
	ret = db->put(db, &key, &value, 0);

error:
	return ret;
}

/*!
 * @brief			retrieves the first commit from the pending list
 *
 * @param	db		The DB object the pending list is stored in
 *
 * @return	commit	A ptr to a pending git_commit; must be freed by the caller
 */
git_commit * get_pending(DB *db)
{
	git_oid *oid = NULL;
	git_commit *commit = NULL;
	DBT key, value;
	if (db->seq(db, &key, &value, R_FIRST) != 0) return NULL;

	if (git_commit_lookup(&commit, repo, value.data) != 0) return NULL;
	db->del(db, &key, 0);
	return commit;
}

int git_populate_new_repo()
{
	int ret = 0;
	git_commit *commit = NULL, *child = NULL;
	git_oid *child_oid = NULL;
	DBT key, value;
	DB *pending = dbopen(NULL, O_CREAT | O_RDWR, 0777, DB_BTREE, NULL);
	if (pending == NULL) {ret = 1; goto error;}
	if ((ret = add_pending(root, pending)) != 0) goto error;
	oldToNew = dbopen(NULL, O_CREAT | O_RDWR, 0777, DB_BTREE, NULL);
	const char *log_message = "Setting ref to corresponding commit in new repo";

	key.size = sizeof(git_oid);
	while((commit = get_pending(pending)) != NULL) {
		if ((ret = copy_commit(commit)) != 0) goto error;

		// Try and add children to the pending list
		key.data = (void *) git_commit_id(commit);
		if (childrenOf->get(childrenOf, &key, &value, 0) == 0) {
			child_oid = value.data;
			do {
				if ((ret = git_commit_lookup(&child, repo, child_oid)) != 0) {
					fprintf(stderr, "Couldn't lookup commit of child oid %s\n", git_oid_tostr_s(child_oid));
					goto error;
				}
				ret = add_pending(child, pending);
				if (ret != 0) break;
				child_oid += sizeof(git_oid *);
				git_commit_free(child);
			} while((void *)child_oid < (value.data + value.size));
		}

		git_commit_free(commit);
		if (ret != 0) goto error;
	}

	DBT refKey, refValue, newRefValue;
	git_reference *new_ref;
	while(refs->seq(refs, &refKey, &refValue, R_NEXT) == 0) {
		if ((ret = oldToNew->get(oldToNew, &refValue, &newRefValue, 0)) == 1) {
			fprintf(stderr, "Couldn't get commit of ref %s to copy to new repo\n", refKey.data);
			goto error;
		}
		git_reference_create(&new_ref, new_repo, refKey.data, newRefValue.data, 0, log_message);
		git_reference_free(new_ref);
	}

error:
	if (pending) pending->close(pending);
	return ret;
}

void git_print_graph()
{
	DBT id, values;
	git_oid *oid = NULL;
	while(parentsOf->seq(parentsOf, &id, &values, R_NEXT) == 0) {
		int children_count = values.size / sizeof(git_oid);
		oid = values.data;
		printf("Parents of: %s:\n", git_oid_tostr_s(id.data));
		for(int i = 0; i < children_count; i++) {
			printf("\t%s;\n", git_oid_tostr_s(oid));
			oid += sizeof(git_oid);
		}
	}

	while(childrenOf->seq(childrenOf, &id, &values, R_NEXT) == 0) {
		int children_count = values.size / sizeof(git_oid);
		oid = values.data;
		printf("%d Children of: %s:\n", children_count, git_oid_tostr_s(id.data));
		for(int i = 0; i < children_count; i++) {
			printf("\t%s;\n", git_oid_tostr_s(oid));
			oid += sizeof(git_oid);
		}
	}
}

void git_fini()
{
	git_repository_free(repo);
	if (childrenOf) childrenOf->close(childrenOf);
	if (parentsOf) parentsOf->close(parentsOf);
	if (refs) refs->close(refs);
	if (oldToNew) oldToNew->close(oldToNew);
	if (root) git_commit_free(root);
	git_libgit2_shutdown();
}
