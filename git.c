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
#include <git2/sys/commit.h>
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

/*!
 * @brief 			Recursively copy a tree
 *
 * @param tree 		the tree to copy
 *
 * @return 			oid of the the new tree object; or NULL on failure
 */
git_oid *copy_tree_r(git_tree *tree, int *flag_replaced)
{
	int toplevel = 0;
	if (flag_replaced == NULL) {
		toplevel = 1;
		flag_replaced = (int *) malloc(sizeof(int));
		*flag_replaced = 0;
	}
	char *name;
	const git_tree_entry *entry = NULL;
	static git_oid buf_oid;
	git_oid *new_oid = NULL;
	git_otype type;
	size_t entrycount = git_tree_entrycount(tree);
	git_treebuilder *builder = NULL;
	git_treebuilder_new(&builder, repo, NULL);

	for (size_t i = 0; i < entrycount; i++) {
		entry = git_tree_entry_byindex(tree, i);
		type = git_tree_entry_type(entry);

		if (type == GIT_OBJ_TREE) {
			git_tree *local_tree = NULL;
			git_tree_lookup(&local_tree, repo, git_tree_entry_id(entry));
			new_oid = copy_tree_r(local_tree, flag_replaced);
			git_tree_free(local_tree);
			if (new_oid == NULL) goto error;
		} else if (type == GIT_OBJ_BLOB) {
			new_oid = (git_oid *) git_tree_entry_id(entry);
			//TODO: implement replacing file contents
		} else if (type == GIT_OBJ_COMMIT) {
			new_oid = (git_oid *) git_tree_entry_id(entry);
		} else {
			fprintf(stderr, "Can't handle tree entry of type %d\n", type);
			new_oid = NULL;
		}


		replace(git_tree_entry_name(entry), &name) > 0 && (*flag_replaced = 1);
		git_treebuilder_insert(NULL, builder, name, new_oid, git_tree_entry_filemode(entry));
		free((void *) name);
	}

	if (*flag_replaced){
		new_oid = &buf_oid;
		git_treebuilder_write(new_oid, builder);
	} else {
		new_oid = (git_oid *) git_tree_id(tree);
	}

error:
	if (toplevel) free(flag_replaced);
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
	char *message_encoding = NULL, *message = NULL;

	// Get array of new parents
	git_oid **new_parent_oids = NULL;
	DBT key, value;

	size_t parentCount = git_commit_parentcount(commit);
	new_parent_oids = (git_oid **) malloc(parentCount * sizeof(git_oid *));
	git_oid *new_parent_oid = NULL;

	for (size_t i = 0; i < parentCount; i++) {
		key.data = (void *) git_commit_parent_id(commit, i);
		key.size = sizeof(git_oid);

		if (oldToNew->get(oldToNew, &key, &value, 0) != 0) {
			fprintf(stderr, "Couldn't find equivalent in new repo of commit %s\n", git_oid_tostr_s(git_commit_id(commit)));
			ret = 1;
			goto error;
		}

		new_parent_oids[i] = value.data;
	}
	// Got new parents

	if ((message_encoding = (char *) git_commit_message_encoding(commit)) != NULL) // If the message encoding is not UTF-8, let's not touch it
		message = (char *) git_commit_message(commit);
	else
		replace(git_commit_message(commit), &message);
	//TODO: Handle case when no new commit is necessary

	if ((ret = git_commit_tree(&tree, commit)) != 0) goto error;
	if ((new_tree_oid = (rename_files) ? copy_tree_r(tree, NULL) : (git_oid *) git_tree_id(tree)) == NULL) {ret = -15; goto error;}

	if ((ret = git_commit_create_from_ids(&new_commit_oid, repo, NULL,
					git_commit_author(commit), git_commit_committer(commit),
					message_encoding, message, new_tree_oid,
					parentCount,
					(const git_oid **)new_parent_oids))
			!= 0) {
		fprintf(stderr, "Couldn't copy commit %s into new repo\n", git_oid_tostr_s(git_commit_id(commit)));
		goto error;
	} else {
		key.data = (void *) git_commit_id(commit);
		key.size = value.size = sizeof(git_oid);
		value.data = &new_commit_oid;
		ret = oldToNew->put(oldToNew, &key, &value, 0);
	}

error:
	if (message_encoding == NULL) free((void *) message);
	free(new_parent_oids);
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

int git_perform_replace()
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
		git_reference_create(&new_ref, repo, refKey.data, newRefValue.data, 1, log_message);
		git_reference_free(new_ref);
	}

error:
	if (pending) pending->close(pending);
	return ret;
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
