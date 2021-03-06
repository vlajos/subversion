/* fs-pack-test.c --- tests for the filesystem
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <stdlib.h>
#include <string.h>
#include <apr_pools.h>

#include "../svn_test.h"
#include "../../libsvn_fs_fs/fs.h"

#include "svn_pools.h"
#include "svn_props.h"
#include "svn_fs.h"
#include "private/svn_string_private.h"

#include "../svn_test_fs.h"



/*** Helper Functions ***/

/* Write the format number and maximum number of files per directory
   to a new format file in PATH, overwriting a previously existing
   file.  Use POOL for temporary allocation.

   (This implementation is largely stolen from libsvn_fs_fs/fs_fs.c.) */
static svn_error_t *
write_format(const char *path,
             int format,
             int max_files_per_dir,
             apr_pool_t *pool)
{
  const char *contents;

  path = svn_dirent_join(path, "format", pool);

  if (format >= SVN_FS_FS__MIN_LAYOUT_FORMAT_OPTION_FORMAT)
    {
      if (format >= SVN_FS_FS__MIN_LOG_ADDRESSING_FORMAT)
        {
          if (max_files_per_dir)
            contents = apr_psprintf(pool,
                                    "%d\n"
                                    "layout sharded %d\n"
                                    "addressing logical 0\n",
                                    format, max_files_per_dir);
          else
            /* linear layouts never use logical addressing */
            contents = apr_psprintf(pool,
                                    "%d\n"
                                    "layout linear\n"
                                    "addressing physical\n",
                                    format);
        }
      else
        {
          if (max_files_per_dir)
            contents = apr_psprintf(pool,
                                    "%d\n"
                                    "layout sharded %d\n",
                                    format, max_files_per_dir);
          else
            contents = apr_psprintf(pool,
                                    "%d\n"
                                    "layout linear\n",
                                    format);
        }
    }
  else
    {
      contents = apr_psprintf(pool, "%d\n", format);
    }

  SVN_ERR(svn_io_write_atomic(path, contents, strlen(contents),
                              NULL /* copy perms */, pool));

  /* And set the perms to make it read only */
  return svn_io_set_file_read_only(path, FALSE, pool);
}

/* Return the expected contents of "iota" in revision REV. */
static const char *
get_rev_contents(svn_revnum_t rev, apr_pool_t *pool)
{
  /* Toss in a bunch of magic numbers for spice. */
  apr_int64_t num = ((rev * 1234353 + 4358) * 4583 + ((rev % 4) << 1)) / 42;
  return apr_psprintf(pool, "%" APR_INT64_T_FMT "\n", num);
}

struct pack_notify_baton
{
  apr_int64_t expected_shard;
  svn_fs_pack_notify_action_t expected_action;
};

static svn_error_t *
pack_notify(void *baton,
            apr_int64_t shard,
            svn_fs_pack_notify_action_t action,
            apr_pool_t *pool)
{
  struct pack_notify_baton *pnb = baton;

  SVN_TEST_ASSERT(shard == pnb->expected_shard);
  SVN_TEST_ASSERT(action == pnb->expected_action);

  /* Update expectations. */
  switch (action)
    {
      case svn_fs_pack_notify_start:
        pnb->expected_action = svn_fs_pack_notify_end;
        break;

      case svn_fs_pack_notify_end:
        pnb->expected_action = svn_fs_pack_notify_start;
        pnb->expected_shard++;
        break;

      default:
        return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                                "Unknown notification action when packing");
    }

  return SVN_NO_ERROR;
}

#define R1_LOG_MSG "Let's serf"

/* Create a packed filesystem in DIR.  Set the shard size to
   SHARD_SIZE and create NUM_REVS number of revisions (in addition to
   r0).  Use POOL for allocations.  After this function successfully
   completes, the filesystem's youngest revision number will be the
   same as NUM_REVS.  */
static svn_error_t *
create_packed_filesystem(const char *dir,
                         const svn_test_opts_t *opts,
                         svn_revnum_t num_revs,
                         int shard_size,
                         apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t after_rev;
  apr_pool_t *subpool = svn_pool_create(pool);
  struct pack_notify_baton pnb;
  apr_pool_t *iterpool;
  int version;

  /* Bail (with success) on known-untestable scenarios */
  if (strcmp(opts->fs_type, "fsfs") != 0)
    return svn_error_create(SVN_ERR_TEST_SKIPPED, NULL,
                            "this will test FSFS repositories only");

  if (opts->server_minor_version && (opts->server_minor_version < 6))
    return svn_error_create(SVN_ERR_TEST_SKIPPED, NULL,
                            "pre-1.6 SVN doesn't support FSFS packing");

  /* Create a filesystem, then close it */
  SVN_ERR(svn_test__create_fs(&fs, dir, opts, subpool));
  svn_pool_destroy(subpool);

  subpool = svn_pool_create(pool);

  /* Rewrite the format file.  (The rest of this function is backend-agnostic,
     so we just avoid adding the FSFS-specific format information if we run on
     some other backend.) */
  if ((strcmp(opts->fs_type, "fsfs") == 0))
    {
      SVN_ERR(svn_io_read_version_file(&version,
                                       svn_dirent_join(dir, "format", subpool),
                                       subpool));
      SVN_ERR(write_format(dir, version, shard_size, subpool));
    }

  /* Reopen the filesystem */
  SVN_ERR(svn_fs_open(&fs, dir, NULL, subpool));

  /* Revision 1: the Greek tree */
  SVN_ERR(svn_fs_begin_txn(&txn, fs, 0, subpool));
  SVN_ERR(svn_fs_txn_root(&txn_root, txn, subpool));
  SVN_ERR(svn_test__create_greek_tree(txn_root, subpool));
  SVN_ERR(svn_fs_change_txn_prop(txn, SVN_PROP_REVISION_LOG,
                                 svn_string_create(R1_LOG_MSG, pool), 
                                 pool));
  SVN_ERR(svn_fs_commit_txn(&conflict, &after_rev, txn, subpool));
  SVN_TEST_ASSERT(SVN_IS_VALID_REVNUM(after_rev));

  /* Revisions 2 thru NUM_REVS-1: content tweaks to "iota". */
  iterpool = svn_pool_create(subpool);
  while (after_rev < num_revs)
    {
      svn_pool_clear(iterpool);
      SVN_ERR(svn_fs_begin_txn(&txn, fs, after_rev, iterpool));
      SVN_ERR(svn_fs_txn_root(&txn_root, txn, iterpool));
      SVN_ERR(svn_test__set_file_contents(txn_root, "iota",
                                          get_rev_contents(after_rev + 1,
                                                           iterpool),
                                          iterpool));
      SVN_ERR(svn_fs_commit_txn(&conflict, &after_rev, txn, iterpool));
      SVN_TEST_ASSERT(SVN_IS_VALID_REVNUM(after_rev));
    }
  svn_pool_destroy(iterpool);
  svn_pool_destroy(subpool);

  /* Now pack the FS */
  pnb.expected_shard = 0;
  pnb.expected_action = svn_fs_pack_notify_start;
  return svn_fs_pack(dir, pack_notify, &pnb, NULL, NULL, pool);
}

/* Create a packed FSFS filesystem for revprop tests at REPO_NAME with
 * MAX_REV revisions and the given SHARD_SIZE and OPTS.  Return it in *FS.
 * Use POOL for allocations.
 */
static svn_error_t *
prepare_revprop_repo(svn_fs_t **fs,
                     const char *repo_name,
                     svn_revnum_t max_rev,
                     int shard_size,
                     const svn_test_opts_t *opts,
                     apr_pool_t *pool)
{
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t after_rev;
  apr_pool_t *subpool;

  /* Create the packed FS and open it. */
  SVN_ERR(create_packed_filesystem(repo_name, opts, max_rev, shard_size, pool));
  SVN_ERR(svn_fs_open(fs, repo_name, NULL, pool));

  subpool = svn_pool_create(pool);
  /* Do a commit to trigger packing. */
  SVN_ERR(svn_fs_begin_txn(&txn, *fs, max_rev, subpool));
  SVN_ERR(svn_fs_txn_root(&txn_root, txn, subpool));
  SVN_ERR(svn_test__set_file_contents(txn_root, "iota", "new-iota",  subpool));
  SVN_ERR(svn_fs_commit_txn(&conflict, &after_rev, txn, subpool));
  SVN_TEST_ASSERT(SVN_IS_VALID_REVNUM(after_rev));
  svn_pool_destroy(subpool);

  /* Pack the repository. */
  SVN_ERR(svn_fs_pack(repo_name, NULL, NULL, NULL, NULL, pool));

  return SVN_NO_ERROR;
}

/* For revision REV, return a short log message allocated in POOL.
 */
static svn_string_t *
default_log(svn_revnum_t rev, apr_pool_t *pool)
{
  return svn_string_createf(pool, "Default message for rev %ld", rev);
}

/* For revision REV, return a long log message allocated in POOL.
 */
static svn_string_t *
large_log(svn_revnum_t rev, apr_size_t length, apr_pool_t *pool)
{
  svn_stringbuf_t *temp = svn_stringbuf_create_ensure(100000, pool);
  int i, count = (int)(length - 50) / 6;

  svn_stringbuf_appendcstr(temp, "A ");
  for (i = 0; i < count; ++i)
    svn_stringbuf_appendcstr(temp, "very, ");

  svn_stringbuf_appendcstr(temp,
    apr_psprintf(pool, "very long message for rev %ld, indeed", rev));

  return svn_stringbuf__morph_into_string(temp);
}

/* For revision REV, return a long log message allocated in POOL.
 */
static svn_string_t *
huge_log(svn_revnum_t rev, apr_pool_t *pool)
{
  return large_log(rev, 90000, pool);
}


/*** Tests ***/

/* ------------------------------------------------------------------------ */
#define REPO_NAME "test-repo-fsfs-pack"
#define SHARD_SIZE 7
#define MAX_REV 53
static svn_error_t *
pack_filesystem(const svn_test_opts_t *opts,
                apr_pool_t *pool)
{
  int i;
  svn_node_kind_t kind;
  const char *path;
  char buf[80];
  apr_file_t *file;
  apr_size_t len;

  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, MAX_REV, SHARD_SIZE,
                                   pool));

  /* Check to see that the pack files exist, and that the rev directories
     don't. */
  for (i = 0; i < (MAX_REV + 1) / SHARD_SIZE; i++)
    {
      path = svn_dirent_join_many(pool, REPO_NAME, "revs",
                                  apr_psprintf(pool, "%d.pack", i / SHARD_SIZE),
                                  "pack", SVN_VA_NULL);

      /* These files should exist. */
      SVN_ERR(svn_io_check_path(path, &kind, pool));
      if (kind != svn_node_file)
        return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                                 "Expected pack file '%s' not found", path);

      if (opts->server_minor_version && (opts->server_minor_version < 9))
        {
          path = svn_dirent_join_many(pool, REPO_NAME, "revs",
                                      apr_psprintf(pool, "%d.pack", i / SHARD_SIZE),
                                      "manifest", SVN_VA_NULL);
          SVN_ERR(svn_io_check_path(path, &kind, pool));
          if (kind != svn_node_file)
            return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                                     "Expected manifest file '%s' not found",
                                     path);
        }
      else
        {
          path = svn_dirent_join_many(pool, REPO_NAME, "revs",
                                      apr_psprintf(pool, "%d.pack", i / SHARD_SIZE),
                                      "pack.l2p", SVN_VA_NULL);
          SVN_ERR(svn_io_check_path(path, &kind, pool));
          if (kind != svn_node_file)
            return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                                     "Expected log-to-phys index file '%s' not found",
                                     path);

          path = svn_dirent_join_many(pool, REPO_NAME, "revs",
                                      apr_psprintf(pool, "%d.pack", i / SHARD_SIZE),
                                      "pack.p2l", NULL);
          SVN_ERR(svn_io_check_path(path, &kind, pool));
          if (kind != svn_node_file)
            return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                                     "Expected phys-to-log index file '%s' not found",
                                     path);
        }

      /* This directory should not exist. */
      path = svn_dirent_join_many(pool, REPO_NAME, "revs",
                                  apr_psprintf(pool, "%d", i / SHARD_SIZE),
                                  SVN_VA_NULL);
      SVN_ERR(svn_io_check_path(path, &kind, pool));
      if (kind != svn_node_none)
        return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                                 "Unexpected directory '%s' found", path);
    }

  /* Ensure the min-unpacked-rev jives with the above operations. */
  SVN_ERR(svn_io_file_open(&file,
                           svn_dirent_join(REPO_NAME, PATH_MIN_UNPACKED_REV,
                                           pool),
                           APR_READ | APR_BUFFERED, APR_OS_DEFAULT, pool));
  len = sizeof(buf);
  SVN_ERR(svn_io_read_length_line(file, buf, &len, pool));
  SVN_ERR(svn_io_file_close(file, pool));
  if (SVN_STR_TO_REV(buf) != (MAX_REV / SHARD_SIZE) * SHARD_SIZE)
    return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                             "Bad '%s' contents", PATH_MIN_UNPACKED_REV);

  /* Finally, make sure the final revision directory does exist. */
  path = svn_dirent_join_many(pool, REPO_NAME, "revs",
                              apr_psprintf(pool, "%d", (i / SHARD_SIZE) + 1),
                              SVN_VA_NULL);
  SVN_ERR(svn_io_check_path(path, &kind, pool));
  if (kind != svn_node_none)
    return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                             "Expected directory '%s' not found", path);


  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef SHARD_SIZE
#undef MAX_REV

/* ------------------------------------------------------------------------ */
#define REPO_NAME "test-repo-fsfs-pack-even"
#define SHARD_SIZE 4
#define MAX_REV 11
static svn_error_t *
pack_even_filesystem(const svn_test_opts_t *opts,
                     apr_pool_t *pool)
{
  svn_node_kind_t kind;
  const char *path;

  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, MAX_REV, SHARD_SIZE,
                                   pool));

  path = svn_dirent_join_many(pool, REPO_NAME, "revs", "2.pack", SVN_VA_NULL);
  SVN_ERR(svn_io_check_path(path, &kind, pool));
  if (kind != svn_node_dir)
    return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                             "Packing did not complete as expected");

  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef SHARD_SIZE
#undef MAX_REV

/* ------------------------------------------------------------------------ */
#define REPO_NAME "test-repo-read-packed-fs"
#define SHARD_SIZE 5
#define MAX_REV 11
static svn_error_t *
read_packed_fs(const svn_test_opts_t *opts,
               apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_stream_t *rstream;
  svn_stringbuf_t *rstring;
  svn_revnum_t i;

  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, MAX_REV, SHARD_SIZE, pool));
  SVN_ERR(svn_fs_open(&fs, REPO_NAME, NULL, pool));

  for (i = 1; i < (MAX_REV + 1); i++)
    {
      svn_fs_root_t *rev_root;
      svn_stringbuf_t *sb;

      SVN_ERR(svn_fs_revision_root(&rev_root, fs, i, pool));
      SVN_ERR(svn_fs_file_contents(&rstream, rev_root, "iota", pool));
      SVN_ERR(svn_test__stream_to_string(&rstring, rstream, pool));

      if (i == 1)
        sb = svn_stringbuf_create("This is the file 'iota'.\n", pool);
      else
        sb = svn_stringbuf_create(get_rev_contents(i, pool), pool);

      if (! svn_stringbuf_compare(rstring, sb))
        return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                                 "Bad data in revision %ld.", i);
    }

  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef SHARD_SIZE
#undef MAX_REV

/* ------------------------------------------------------------------------ */
#define REPO_NAME "test-repo-commit-packed-fs"
#define SHARD_SIZE 5
#define MAX_REV 10
static svn_error_t *
commit_packed_fs(const svn_test_opts_t *opts,
                 apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t after_rev;

  /* Create the packed FS and open it. */
  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, MAX_REV, 5, pool));
  SVN_ERR(svn_fs_open(&fs, REPO_NAME, NULL, pool));

  /* Now do a commit. */
  SVN_ERR(svn_fs_begin_txn(&txn, fs, MAX_REV, pool));
  SVN_ERR(svn_fs_txn_root(&txn_root, txn, pool));
  SVN_ERR(svn_test__set_file_contents(txn_root, "iota",
          "How much better is it to get wisdom than gold! and to get "
          "understanding rather to be chosen than silver!", pool));
  SVN_ERR(svn_fs_commit_txn(&conflict, &after_rev, txn, pool));
  SVN_TEST_ASSERT(SVN_IS_VALID_REVNUM(after_rev));

  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef MAX_REV
#undef SHARD_SIZE

/* ------------------------------------------------------------------------ */
#define REPO_NAME "test-repo-get-set-revprop-packed-fs"
#define SHARD_SIZE 4
#define MAX_REV 10
static svn_error_t *
get_set_revprop_packed_fs(const svn_test_opts_t *opts,
                          apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_string_t *prop_value;

  /* Create the packed FS and open it. */
  SVN_ERR(prepare_revprop_repo(&fs, REPO_NAME, MAX_REV, SHARD_SIZE, opts,
                               pool));

  /* Try to get revprop for revision 0
   * (non-packed due to special handling). */
  SVN_ERR(svn_fs_revision_prop(&prop_value, fs, 0, SVN_PROP_REVISION_AUTHOR,
                               pool));

  /* Try to change revprop for revision 0
   * (non-packed due to special handling). */
  SVN_ERR(svn_fs_change_rev_prop(fs, 0, SVN_PROP_REVISION_AUTHOR,
                                 svn_string_create("tweaked-author", pool),
                                 pool));

  /* verify */
  SVN_ERR(svn_fs_revision_prop(&prop_value, fs, 0, SVN_PROP_REVISION_AUTHOR,
                               pool));
  SVN_TEST_STRING_ASSERT(prop_value->data, "tweaked-author");

  /* Try to get packed revprop for revision 5. */
  SVN_ERR(svn_fs_revision_prop(&prop_value, fs, 5, SVN_PROP_REVISION_AUTHOR,
                               pool));

  /* Try to change packed revprop for revision 5. */
  SVN_ERR(svn_fs_change_rev_prop(fs, 5, SVN_PROP_REVISION_AUTHOR,
                                 svn_string_create("tweaked-author2", pool),
                                 pool));

  /* verify */
  SVN_ERR(svn_fs_revision_prop(&prop_value, fs, 5, SVN_PROP_REVISION_AUTHOR,
                               pool));
  SVN_TEST_STRING_ASSERT(prop_value->data, "tweaked-author2");

  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef MAX_REV
#undef SHARD_SIZE

/* ------------------------------------------------------------------------ */
#define REPO_NAME "test-repo-get-set-large-revprop-packed-fs"
#define SHARD_SIZE 4
#define MAX_REV 11
static svn_error_t *
get_set_large_revprop_packed_fs(const svn_test_opts_t *opts,
                                apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_string_t *prop_value;
  svn_revnum_t rev;

  /* Create the packed FS and open it. */
  SVN_ERR(prepare_revprop_repo(&fs, REPO_NAME, MAX_REV, SHARD_SIZE, opts,
                               pool));

  /* Set commit messages to different, large values that fill the pack
   * files but do not exceed the pack size limit. */
  for (rev = 0; rev <= MAX_REV; ++rev)
    SVN_ERR(svn_fs_change_rev_prop(fs, rev, SVN_PROP_REVISION_LOG,
                                   large_log(rev, 15000, pool),
                                   pool));

  /* verify */
  for (rev = 0; rev <= MAX_REV; ++rev)
    {
      SVN_ERR(svn_fs_revision_prop(&prop_value, fs, rev,
                                   SVN_PROP_REVISION_LOG, pool));
      SVN_TEST_STRING_ASSERT(prop_value->data,
                             large_log(rev, 15000, pool)->data);
    }

  /* Put a larger revprop into the last, some middle and the first revision
   * of a pack.  This should cause the packs to split in the middle. */
  SVN_ERR(svn_fs_change_rev_prop(fs, 3, SVN_PROP_REVISION_LOG,
                                 /* rev 0 is not packed */
                                 large_log(3, 37000, pool),
                                 pool));
  SVN_ERR(svn_fs_change_rev_prop(fs, 5, SVN_PROP_REVISION_LOG,
                                 large_log(5, 25000, pool),
                                 pool));
  SVN_ERR(svn_fs_change_rev_prop(fs, 8, SVN_PROP_REVISION_LOG,
                                 large_log(8, 25000, pool),
                                 pool));

  /* verify */
  for (rev = 0; rev <= MAX_REV; ++rev)
    {
      SVN_ERR(svn_fs_revision_prop(&prop_value, fs, rev,
                                   SVN_PROP_REVISION_LOG, pool));

      if (rev == 3)
        SVN_TEST_STRING_ASSERT(prop_value->data,
                               large_log(rev, 37000, pool)->data);
      else if (rev == 5 || rev == 8)
        SVN_TEST_STRING_ASSERT(prop_value->data,
                               large_log(rev, 25000, pool)->data);
      else
        SVN_TEST_STRING_ASSERT(prop_value->data,
                               large_log(rev, 15000, pool)->data);
    }

  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef MAX_REV
#undef SHARD_SIZE

/* ------------------------------------------------------------------------ */
#define REPO_NAME "test-repo-get-set-huge-revprop-packed-fs"
#define SHARD_SIZE 4
#define MAX_REV 10
static svn_error_t *
get_set_huge_revprop_packed_fs(const svn_test_opts_t *opts,
                               apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_string_t *prop_value;
  svn_revnum_t rev;

  /* Create the packed FS and open it. */
  SVN_ERR(prepare_revprop_repo(&fs, REPO_NAME, MAX_REV, SHARD_SIZE, opts,
                               pool));

  /* Set commit messages to different values */
  for (rev = 0; rev <= MAX_REV; ++rev)
    SVN_ERR(svn_fs_change_rev_prop(fs, rev, SVN_PROP_REVISION_LOG,
                                   default_log(rev, pool),
                                   pool));

  /* verify */
  for (rev = 0; rev <= MAX_REV; ++rev)
    {
      SVN_ERR(svn_fs_revision_prop(&prop_value, fs, rev,
                                   SVN_PROP_REVISION_LOG, pool));
      SVN_TEST_STRING_ASSERT(prop_value->data, default_log(rev, pool)->data);
    }

  /* Put a huge revprop into the last, some middle and the first revision
   * of a pack.  They will cause the pack files to split accordingly. */
  SVN_ERR(svn_fs_change_rev_prop(fs, 3, SVN_PROP_REVISION_LOG,
                                 huge_log(3, pool),
                                 pool));
  SVN_ERR(svn_fs_change_rev_prop(fs, 5, SVN_PROP_REVISION_LOG,
                                 huge_log(5, pool),
                                 pool));
  SVN_ERR(svn_fs_change_rev_prop(fs, 8, SVN_PROP_REVISION_LOG,
                                 huge_log(8, pool),
                                 pool));

  /* verify */
  for (rev = 0; rev <= MAX_REV; ++rev)
    {
      SVN_ERR(svn_fs_revision_prop(&prop_value, fs, rev,
                                   SVN_PROP_REVISION_LOG, pool));

      if (rev == 3 || rev == 5 || rev == 8)
        SVN_TEST_STRING_ASSERT(prop_value->data,
                               huge_log(rev, pool)->data);
      else
        SVN_TEST_STRING_ASSERT(prop_value->data,
                               default_log(rev, pool)->data);
    }

  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef MAX_REV
#undef SHARD_SIZE

/* ------------------------------------------------------------------------ */
/* Regression test for issue #3571 (fsfs 'svnadmin recover' expects
   youngest revprop to be outside revprops.db). */
#define REPO_NAME "test-repo-recover-fully-packed"
#define SHARD_SIZE 4
#define MAX_REV 7
static svn_error_t *
recover_fully_packed(const svn_test_opts_t *opts,
                     apr_pool_t *pool)
{
  apr_pool_t *subpool;
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t after_rev;
  svn_error_t *err;

  /* Create a packed FS for which every revision will live in a pack
     digest file, and then recover it. */
  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, MAX_REV, SHARD_SIZE, pool));
  SVN_ERR(svn_fs_recover(REPO_NAME, NULL, NULL, pool));

  /* Add another revision, re-pack, re-recover. */
  subpool = svn_pool_create(pool);
  SVN_ERR(svn_fs_open(&fs, REPO_NAME, NULL, subpool));
  SVN_ERR(svn_fs_begin_txn(&txn, fs, MAX_REV, subpool));
  SVN_ERR(svn_fs_txn_root(&txn_root, txn, subpool));
  SVN_ERR(svn_test__set_file_contents(txn_root, "A/mu", "new-mu", subpool));
  SVN_ERR(svn_fs_commit_txn(&conflict, &after_rev, txn, subpool));
  SVN_TEST_ASSERT(SVN_IS_VALID_REVNUM(after_rev));
  svn_pool_destroy(subpool);
  SVN_ERR(svn_fs_pack(REPO_NAME, NULL, NULL, NULL, NULL, pool));
  SVN_ERR(svn_fs_recover(REPO_NAME, NULL, NULL, pool));

  /* Now, delete the youngest revprop file, and recover again.  This
     time we want to see an error! */
  SVN_ERR(svn_io_remove_file2(
              svn_dirent_join_many(pool, REPO_NAME, PATH_REVPROPS_DIR,
                                   apr_psprintf(pool, "%ld/%ld",
                                                after_rev / SHARD_SIZE,
                                                after_rev),
                                   SVN_VA_NULL),
              FALSE, pool));
  err = svn_fs_recover(REPO_NAME, NULL, NULL, pool);
  if (! err)
    return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                            "Expected SVN_ERR_FS_CORRUPT error; got none");
  if (err->apr_err != SVN_ERR_FS_CORRUPT)
    return svn_error_create(SVN_ERR_TEST_FAILED, err,
                            "Expected SVN_ERR_FS_CORRUPT error; got:");
  svn_error_clear(err);
  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef MAX_REV
#undef SHARD_SIZE

/* ------------------------------------------------------------------------ */
/* Regression test for issue #4320 (fsfs file-hinting fails when reading a rep
   from the transaction that is commiting rev = SHARD_SIZE). */
#define REPO_NAME "test-repo-file-hint-at-shard-boundary"
#define SHARD_SIZE 4
#define MAX_REV (SHARD_SIZE - 1)
static svn_error_t *
file_hint_at_shard_boundary(const svn_test_opts_t *opts,
                            apr_pool_t *pool)
{
  apr_pool_t *subpool;
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *file_contents;
  svn_stringbuf_t *retrieved_contents;
  svn_error_t *err = SVN_NO_ERROR;

  /* Create a packed FS and MAX_REV revisions */
  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, MAX_REV, SHARD_SIZE, pool));

  /* Reopen the filesystem */
  subpool = svn_pool_create(pool);
  SVN_ERR(svn_fs_open(&fs, REPO_NAME, NULL, subpool));

  /* Revision = SHARD_SIZE */
  file_contents = get_rev_contents(SHARD_SIZE, subpool);
  SVN_ERR(svn_fs_begin_txn(&txn, fs, MAX_REV, subpool));
  SVN_ERR(svn_fs_txn_root(&txn_root, txn, subpool));
  SVN_ERR(svn_test__set_file_contents(txn_root, "iota", file_contents,
                                      subpool));

  /* Retrieve the file. */
  SVN_ERR(svn_test__get_file_contents(txn_root, "iota", &retrieved_contents,
                                      subpool));
  if (strcmp(retrieved_contents->data, file_contents))
    {
      err = svn_error_create(SVN_ERR_TEST_FAILED, err,
                              "Retrieved incorrect contents from iota.");
    }

  /* Close the repo. */
  svn_pool_destroy(subpool);

  return err;
}
#undef REPO_NAME
#undef MAX_REV
#undef SHARD_SIZE

/* ------------------------------------------------------------------------ */
#define REPO_NAME "test-repo-fsfs-info"
#define SHARD_SIZE 3
#define MAX_REV 5
static svn_error_t *
test_info(const svn_test_opts_t *opts,
          apr_pool_t *pool)
{
  svn_fs_t *fs;
  const svn_fs_fsfs_info_t *fsfs_info;
  const svn_fs_info_placeholder_t *info;

  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, MAX_REV, SHARD_SIZE,
                                   pool));

  SVN_ERR(svn_fs_open(&fs, REPO_NAME, NULL, pool));
  SVN_ERR(svn_fs_info(&info, fs, pool, pool));
  info = svn_fs_info_dup(info, pool, pool);

  SVN_TEST_STRING_ASSERT(opts->fs_type, info->fs_type);

  /* Bail (with success) on known-untestable scenarios */
  if (strcmp(opts->fs_type, "fsfs") != 0)
    return SVN_NO_ERROR;

  fsfs_info = (const void *)info;
  if (opts->server_minor_version && (opts->server_minor_version < 6))
    {
      SVN_TEST_ASSERT(fsfs_info->shard_size == 0);
      SVN_TEST_ASSERT(fsfs_info->min_unpacked_rev == 0);
    }
  else
    {
      SVN_TEST_ASSERT(fsfs_info->shard_size == SHARD_SIZE);
      SVN_TEST_ASSERT(fsfs_info->min_unpacked_rev
                      == (MAX_REV + 1) / SHARD_SIZE * SHARD_SIZE);
    }

  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef SHARD_SIZE
#undef MAX_REV

/* ------------------------------------------------------------------------ */
#define REPO_NAME "test-repo-fsfs-pack-shard-size-one"
#define SHARD_SIZE 1
#define MAX_REV 4
static svn_error_t *
pack_shard_size_one(const svn_test_opts_t *opts,
                     apr_pool_t *pool)
{
  svn_string_t *propval;
  svn_fs_t *fs;

  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, MAX_REV, SHARD_SIZE,
                                   pool));
  SVN_ERR(svn_fs_open(&fs, REPO_NAME, NULL, pool));
  /* whitebox: revprop packing special-cases r0, which causes
     (start_rev==1, end_rev==0) in pack_revprops_shard().  So test that. */
  SVN_ERR(svn_fs_revision_prop(&propval, fs, 1, SVN_PROP_REVISION_LOG, pool));
  SVN_TEST_STRING_ASSERT(propval->data, R1_LOG_MSG);

  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef SHARD_SIZE
#undef MAX_REV
/* ------------------------------------------------------------------------ */
#define REPO_NAME "get_set_multiple_huge_revprops_packed_fs"
#define SHARD_SIZE 4
#define MAX_REV 9
static svn_error_t *
get_set_multiple_huge_revprops_packed_fs(const svn_test_opts_t *opts,
                                         apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_string_t *prop_value;
  svn_revnum_t rev;

  /* Create the packed FS and open it. */
  SVN_ERR(prepare_revprop_repo(&fs, REPO_NAME, MAX_REV, SHARD_SIZE, opts,
                               pool));

  /* Set commit messages to different values */
  for (rev = 0; rev <= MAX_REV; ++rev)
    SVN_ERR(svn_fs_change_rev_prop(fs, rev, SVN_PROP_REVISION_LOG,
                                   default_log(rev, pool),
                                   pool));

  /* verify */
  for (rev = 0; rev <= MAX_REV; ++rev)
    {
      SVN_ERR(svn_fs_revision_prop(&prop_value, fs, rev,
                                   SVN_PROP_REVISION_LOG, pool));
      SVN_TEST_STRING_ASSERT(prop_value->data, default_log(rev, pool)->data);
    }

  /* Put a huge revprop into revision 1 and 2. */
  SVN_ERR(svn_fs_change_rev_prop(fs, 1, SVN_PROP_REVISION_LOG,
                                 huge_log(1, pool),
                                 pool));
  SVN_ERR(svn_fs_change_rev_prop(fs, 2, SVN_PROP_REVISION_LOG,
                                 huge_log(2, pool),
                                 pool));
  SVN_ERR(svn_fs_change_rev_prop(fs, 5, SVN_PROP_REVISION_LOG,
                                 huge_log(5, pool),
                                 pool));
  SVN_ERR(svn_fs_change_rev_prop(fs, 6, SVN_PROP_REVISION_LOG,
                                 huge_log(6, pool),
                                 pool));

  /* verify */
  for (rev = 0; rev <= MAX_REV; ++rev)
    {
      SVN_ERR(svn_fs_revision_prop(&prop_value, fs, rev,
                                   SVN_PROP_REVISION_LOG, pool));

      if (rev == 1 || rev == 2 || rev == 5 || rev == 6)
        SVN_TEST_STRING_ASSERT(prop_value->data,
                               huge_log(rev, pool)->data);
      else
        SVN_TEST_STRING_ASSERT(prop_value->data,
                               default_log(rev, pool)->data);
    }

  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef MAX_REV
#undef SHARD_SIZE

/* ------------------------------------------------------------------------ */
#define SHARD_SIZE 4
static svn_error_t *
upgrade_txns_to_log_addressing(const svn_test_opts_t *opts,
                               const char *repo_name,
                               svn_revnum_t max_rev,
                               svn_boolean_t upgrade_before_txns,
                               apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_revnum_t rev;
  apr_array_header_t *txns;
  apr_array_header_t *txn_names;
  int i, k;
  svn_test_opts_t temp_opts;
  svn_fs_root_t *root;
  apr_pool_t *iterpool = svn_pool_create(pool);

  static const char * const paths[SHARD_SIZE][2]
    = {
        { "A/mu",        "A/B/lambda" },
        { "A/B/E/alpha", "A/D/H/psi"  },
        { "A/D/gamma",   "A/B/E/beta" },
        { "A/D/G/pi",    "A/D/G/rho"  }
      };

  /* Bail (with success) on known-untestable scenarios */
  if ((strcmp(opts->fs_type, "fsfs") != 0)
      || (opts->server_minor_version && (opts->server_minor_version < 9)))
    return svn_error_create(SVN_ERR_TEST_SKIPPED, NULL,
                            "pre-1.9 SVN doesn't support log addressing");

  /* Create the packed FS in phys addressing format and open it. */
  temp_opts = *opts;
  temp_opts.server_minor_version = 8;
  SVN_ERR(prepare_revprop_repo(&fs, repo_name, max_rev, SHARD_SIZE,
                               &temp_opts, pool));

  if (upgrade_before_txns)
    {
      /* upgrade to final repo format (using log addressing) and re-open */
      SVN_ERR(svn_fs_upgrade2(repo_name, NULL, NULL, NULL, NULL, pool));
      SVN_ERR(svn_fs_open(&fs, repo_name, svn_fs_config(fs, pool), pool));
    }

  /* Create 4 concurrent transactions */
  txns = apr_array_make(pool, SHARD_SIZE, sizeof(svn_fs_txn_t *));
  txn_names = apr_array_make(pool, SHARD_SIZE, sizeof(const char *));
  for (i = 0; i < SHARD_SIZE; ++i)
    {
      svn_fs_txn_t *txn;
      const char *txn_name;

      SVN_ERR(svn_fs_begin_txn(&txn, fs, max_rev, pool));
      APR_ARRAY_PUSH(txns, svn_fs_txn_t *) = txn;

      SVN_ERR(svn_fs_txn_name(&txn_name, txn, pool));
      APR_ARRAY_PUSH(txn_names, const char *) = txn_name;
    }

  /* Let all txns touch at least 2 files.
   * Thus, the addressing data of at least one representation in the txn
   * will differ between addressing modes. */
  for (i = 0; i < SHARD_SIZE; ++i)
    {
      svn_fs_txn_t *txn = APR_ARRAY_IDX(txns, i, svn_fs_txn_t *);
      SVN_ERR(svn_fs_txn_root(&root, txn, pool));

      for (k = 0; k < 2; ++k)
        {
          svn_stream_t *stream;
          const char *file_path = paths[i][k];
          svn_pool_clear(iterpool);

          SVN_ERR(svn_fs_apply_text(&stream, root, file_path, NULL, iterpool));
          SVN_ERR(svn_stream_printf(stream, iterpool,
                                    "This is file %s in txn %d",
                                    file_path, i));
          SVN_ERR(svn_stream_close(stream));
        }
    }

  if (!upgrade_before_txns)
    {
      /* upgrade to final repo format (using log addressing) and re-open */
      SVN_ERR(svn_fs_upgrade2(repo_name, NULL, NULL, NULL, NULL, pool));
      SVN_ERR(svn_fs_open(&fs, repo_name, svn_fs_config(fs, pool), pool));
    }

  /* Commit all transactions
   * (in reverse order to make things more interesting) */
  for (i = SHARD_SIZE - 1; i >= 0; --i)
    {
      svn_fs_txn_t *txn;
      const char *txn_name = APR_ARRAY_IDX(txn_names, i, const char *);
      svn_pool_clear(iterpool);

      SVN_ERR(svn_fs_open_txn(&txn, fs, txn_name, iterpool));
      SVN_ERR(svn_fs_commit_txn(NULL, &rev, txn, iterpool));
    }

  /* Further changes to fill the shard */

  SVN_ERR(svn_fs_youngest_rev(&rev, fs, pool));
  SVN_TEST_ASSERT(rev == SHARD_SIZE + max_rev + 1);

  while ((rev + 1) % SHARD_SIZE)
    {
      svn_fs_txn_t *txn;
      if (rev % SHARD_SIZE == 0)
        break;

      svn_pool_clear(iterpool);

      SVN_ERR(svn_fs_begin_txn(&txn, fs, rev, iterpool));
      SVN_ERR(svn_fs_txn_root(&root, txn, iterpool));
      SVN_ERR(svn_test__set_file_contents(root, "iota",
                                          get_rev_contents(rev + 1, iterpool),
                                          iterpool));
      SVN_ERR(svn_fs_commit_txn(NULL, &rev, txn, iterpool));
    }

  /* Make sure to close all file handles etc. from the last iteration */

  svn_pool_clear(iterpool);

  /* Pack repo to verify that old and new shard get packed according to
     their respective addressing mode */

  SVN_ERR(svn_fs_pack(repo_name, NULL, NULL, NULL, NULL, pool));

  /* verify that our changes got in */

  SVN_ERR(svn_fs_revision_root(&root, fs, rev, pool));
  for (i = 0; i < SHARD_SIZE; ++i)
    {
      for (k = 0; k < 2; ++k)
        {
          svn_stream_t *stream;
          const char *file_path = paths[i][k];
          svn_string_t *string;
          const char *expected;

          svn_pool_clear(iterpool);

          SVN_ERR(svn_fs_file_contents(&stream, root, file_path, iterpool));
          SVN_ERR(svn_string_from_stream(&string, stream, iterpool, iterpool));

          expected = apr_psprintf(pool,"This is file %s in txn %d",
                                  file_path, i);
          SVN_TEST_STRING_ASSERT(string->data, expected);
        }
    }

  /* verify that the indexes are consistent, we calculated the correct
     low-level checksums etc. */
  SVN_ERR(svn_fs_verify(repo_name, NULL,
                        SVN_INVALID_REVNUM, SVN_INVALID_REVNUM,
                        NULL, NULL, NULL, NULL, pool));
  for (; rev >= 0; --rev)
    {
      svn_pool_clear(iterpool);
      SVN_ERR(svn_fs_revision_root(&root, fs, rev, iterpool));
      SVN_ERR(svn_fs_verify_root(root, iterpool));
    }

  return SVN_NO_ERROR;
}
#undef SHARD_SIZE

#define REPO_NAME "upgrade_new_txns_to_log_addressing"
#define MAX_REV 8
static svn_error_t *
upgrade_new_txns_to_log_addressing(const svn_test_opts_t *opts,
                                   apr_pool_t *pool)
{
  SVN_ERR(upgrade_txns_to_log_addressing(opts, REPO_NAME, MAX_REV, TRUE,
                                         pool));

  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef MAX_REV

/* ------------------------------------------------------------------------ */
#define REPO_NAME "upgrade_old_txns_to_log_addressing"
#define MAX_REV 8
static svn_error_t *
upgrade_old_txns_to_log_addressing(const svn_test_opts_t *opts,
                                   apr_pool_t *pool)
{
  SVN_ERR(upgrade_txns_to_log_addressing(opts, REPO_NAME, MAX_REV, FALSE,
                                         pool));

  return SVN_NO_ERROR;
}

#undef REPO_NAME
#undef MAX_REV

/* ------------------------------------------------------------------------ */

/* The test table.  */

int svn_test_max_threads = 4;

struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_OPTS_PASS(pack_filesystem,
                       "pack a FSFS filesystem"),
    SVN_TEST_OPTS_PASS(pack_even_filesystem,
                       "pack FSFS where revs % shard = 0"),
    SVN_TEST_OPTS_PASS(read_packed_fs,
                       "read from a packed FSFS filesystem"),
    SVN_TEST_OPTS_PASS(commit_packed_fs,
                       "commit to a packed FSFS filesystem"),
    SVN_TEST_OPTS_PASS(get_set_revprop_packed_fs,
                       "get/set revprop while packing FSFS filesystem"),
    SVN_TEST_OPTS_PASS(get_set_large_revprop_packed_fs,
                       "get/set large packed revprops in FSFS"),
    SVN_TEST_OPTS_PASS(get_set_huge_revprop_packed_fs,
                       "get/set huge packed revprops in FSFS"),
    SVN_TEST_OPTS_PASS(recover_fully_packed,
                       "recover a fully packed filesystem"),
    SVN_TEST_OPTS_PASS(file_hint_at_shard_boundary,
                       "test file hint at shard boundary"),
    SVN_TEST_OPTS_PASS(test_info,
                       "test svn_fs_info"),
    SVN_TEST_OPTS_PASS(pack_shard_size_one,
                       "test packing with shard size = 1"),
    SVN_TEST_OPTS_PASS(get_set_multiple_huge_revprops_packed_fs,
                       "set multiple huge revprops in packed FSFS"),
    SVN_TEST_OPTS_PASS(upgrade_new_txns_to_log_addressing,
                       "upgrade txns to log addressing in shared FSFS"),
    SVN_TEST_OPTS_PASS(upgrade_old_txns_to_log_addressing,
                       "upgrade txns started before svnadmin upgrade"),
    SVN_TEST_NULL
  };
