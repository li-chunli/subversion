/*
 * diff_file.c :  routines for doing diffs on files
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */


#include <apr.h>
#include <apr_pools.h>
#include <apr_general.h>
#include <apr_md5.h>
#include <apr_file_io.h>
#include <apr_file_info.h>
#include <apr_time.h>
#include <apr_mmap.h>

#include "svn_error.h"
#include "svn_diff.h"
#include "svn_types.h"
#include "svn_string.h"
#include "svn_io.h"
#include "svn_pools.h"
#include "diff.h"


typedef struct svn_diff__file_token_t
{
  struct svn_diff__file_token_t *next;
  svn_diff_datasource_e datasource;
  apr_off_t offset;
  apr_off_t length;
} svn_diff__file_token_t;


typedef struct svn_diff__file_baton_t
{
  const char *path[4];

  apr_file_t *file[4];
  apr_off_t size[4];

  int chunk[4];
  char *buffer[4];
  char *curp[4];
  char *endp[4];

  svn_diff__file_token_t *tokens;

  apr_pool_t *pool;
} svn_diff__file_baton_t;


static
int
svn_diff__file_datasource_to_index(svn_diff_datasource_e datasource)
{
  switch (datasource)
    {
    case svn_diff_datasource_original:
      return 0;

    case svn_diff_datasource_modified:
      return 1;

    case svn_diff_datasource_latest:
      return 2;

    case svn_diff_datasource_ancestor:
      return 3;
    }

  return -1;
}

/* Files are read in chunks of 128k.  There is no support for this number
 * whatsoever.  If there is a number someone comes up with that has some
 * argumentation, let's use that.
 */
#define CHUNK_SHIFT 17
#define CHUNK_SIZE (1 << CHUNK_SHIFT)

#define chunk_to_offset(chunk) ((chunk) << CHUNK_SHIFT)
#define offset_to_chunk(offset) ((offset) >> CHUNK_SHIFT)
#define offset_in_chunk(offset) ((offset) & (CHUNK_SIZE - 1))


/* Read a chunk from a FILE into BUFFER, starting from OFFSET, going for
 * *LENGTH.  The actual bytes read are stored in *LENGTH on return.
 */
static APR_INLINE
svn_error_t *
read_chunk(apr_file_t *file, const char *path,
           char *buffer, apr_size_t length,
           apr_off_t offset, apr_pool_t *pool)
{
  /* XXX: The final offset may not be the one we asked for.
   * XXX: Check.
   */
  SVN_ERR (svn_io_file_seek (file, APR_SET, &offset, pool));
  SVN_ERR (svn_io_file_read_full (file, buffer, length, NULL, pool));

  return SVN_NO_ERROR;
}


/* Map or read a file at PATH. *BUFFER will point to the file
 * contents; if the file was mapped, *FILE and *MM will contain the
 * mmap context; otherwise they will be NULL.  SIZE will contain the
 * file size.  Allocate from POOL.
 */
#if APR_HAS_MMAP
#define MMAP_T_PARAM(NAME) apr_mmap_t **NAME,
#define MMAP_T_ARG(NAME)   &(NAME),
#else
#define MMAP_T_PARAM(NAME)
#define MMAP_T_ARG(NAME)
#endif

static
svn_error_t *
map_or_read_file(apr_file_t **file,
                 MMAP_T_PARAM(mm)
                 char **buffer, apr_off_t *size,
                 const char *path, apr_pool_t *pool)
{
  apr_finfo_t finfo;
  apr_status_t rv;

  *buffer = NULL;

  SVN_ERR (svn_io_file_open (file, path, APR_READ, APR_OS_DEFAULT, pool));
  SVN_ERR (svn_io_file_info_get (&finfo, APR_FINFO_SIZE, *file, pool));

#if APR_HAS_MMAP
  if (finfo.size > APR_MMAP_THRESHOLD)
    {
      rv = apr_mmap_create(mm, *file, 0, finfo.size, APR_MMAP_READ, pool);
      if (rv == APR_SUCCESS)
        {
          *buffer = (*mm)->mm;
        }

      /* On failure we just fall through and try reading the file into
       * memory instead.
       */
    }
#endif /* APR_HAS_MMAP */

   if (*buffer == NULL && finfo.size > 0)
    {
      *buffer = apr_palloc(pool, finfo.size);

      SVN_ERR (svn_io_file_read_full (*file, *buffer, finfo.size, NULL, pool));

      /* Since we have the entire contents of the file we can
       * close it now.
       */
      SVN_ERR (svn_io_file_close(*file, pool));

      *file = NULL;
    }

  *size = finfo.size;

  return SVN_NO_ERROR;
}


static
svn_error_t *
svn_diff__file_datasource_open(void *baton,
                               svn_diff_datasource_e datasource)
{
  svn_diff__file_baton_t *file_baton = baton;
  int idx;
  apr_status_t apr_err;
  apr_finfo_t finfo;
  apr_size_t length;
  char *curp;
  char *endp;

  idx = svn_diff__file_datasource_to_index(datasource);

  SVN_ERR( svn_io_file_open (&file_baton->file[idx], file_baton->path[idx],
                             APR_READ, APR_OS_DEFAULT, file_baton->pool));

  SVN_ERR (svn_io_file_info_get (&finfo, APR_FINFO_SIZE, 
                                 file_baton->file[idx], file_baton->pool));

  file_baton->size[idx] = finfo.size;
  length = finfo.size > CHUNK_SIZE ? CHUNK_SIZE : finfo.size;

  if (length == 0)
    return SVN_NO_ERROR;

  endp = curp = apr_palloc(file_baton->pool, length);
  endp += length;

  file_baton->buffer[idx] = file_baton->curp[idx] = curp;
  file_baton->endp[idx] = endp;

  SVN_ERR (read_chunk (file_baton->file[idx], file_baton->path[idx],
                       curp, length, 0, file_baton->pool));

  return SVN_NO_ERROR;
}


static
svn_error_t *
svn_diff__file_datasource_close(void *baton,
                                svn_diff_datasource_e datasource)
{
  /* Do nothing.  The compare_token function needs previous datasources
   * to stay available until all datasources are processed.
   */

  return SVN_NO_ERROR;
}


static
svn_error_t *
svn_diff__file_datasource_get_next_token(apr_uint32_t *hash, void **token,
                                         void *baton,
                                         svn_diff_datasource_e datasource)
{
  svn_diff__file_baton_t *file_baton = baton;
  svn_diff__file_token_t *file_token;
  int idx;
  char *endp;
  char *curp;
  char *eol;
  int last_chunk;
  apr_size_t length;
  apr_uint32_t h = 0;

  *token = NULL;

  idx = svn_diff__file_datasource_to_index(datasource);

  curp = file_baton->curp[idx];
  endp = file_baton->endp[idx];

  last_chunk = offset_to_chunk(file_baton->size[idx]);

  if (curp == endp
      && last_chunk == file_baton->chunk[idx])
    {
      return SVN_NO_ERROR;
    }

  /* Get a new token */
  file_token = file_baton->tokens;
  if (file_token)
    {
      file_baton->tokens = file_token->next;
    }
  else
    {
      file_token = apr_palloc(file_baton->pool, sizeof(*file_token));
    }

  file_token->datasource = datasource;
  file_token->offset = chunk_to_offset(file_baton->chunk[idx])
                       + (curp - file_baton->buffer[idx]);
  file_token->length = 0;

  while (1)
    {
      /* XXX: '\n' doesn't really cut it.  We need to be able to detect
       * XXX: '\n', '\r' and '\r\n'.
       */
      eol = memchr(curp, '\n', endp - curp);
      if (eol)
        {
          eol++;
          break;
        }

      if (file_baton->chunk[idx] == last_chunk)
        {
          eol = endp;
          break;
        }

      length = endp - curp;
      file_token->length += length;
      h = svn_diff__adler32(h, curp, length);

      curp = endp = file_baton->buffer[idx];
      file_baton->chunk[idx]++;
      length = file_baton->chunk[idx] == last_chunk ? offset_in_chunk(file_baton->size[idx]) : CHUNK_SIZE;
      endp += length;
      file_baton->endp[idx] = endp;

      SVN_ERR(read_chunk(file_baton->file[idx], file_baton->path[idx],
                         curp, length, 
                         chunk_to_offset(file_baton->chunk[idx]),
                         file_baton->pool));
    }

  length = eol - curp;
  file_token->length += length;
  *hash = svn_diff__adler32(h, curp, length);

  file_baton->curp[idx] = eol;
  *token = file_token;

  return SVN_NO_ERROR;
}

#define COMPARE_CHUNK_SIZE 4096

static
svn_error_t *
svn_diff__file_token_compare(void *baton,
                             void *token1,
                             void *token2,
                             int *compare)
{
  svn_diff__file_baton_t *file_baton = baton;
  svn_diff__file_token_t *file_token1 = token1;
  svn_diff__file_token_t *file_token2 = token2;
  char buffer[2][COMPARE_CHUNK_SIZE];
  char *bufp[2];
  apr_off_t offset[2];
  int idx[2];
  apr_off_t length[2];
  apr_off_t total_length;
  apr_off_t len;
  int i;
  int chunk[2];

  if (file_token1->length < file_token2->length)
    {
      *compare = -1;
      return SVN_NO_ERROR;
    }

  if (file_token1->length > file_token2->length)
    {
      *compare = 1;
      return SVN_NO_ERROR;
    }

  total_length = file_token1->length;
  if (total_length == 0)
    {
      *compare = 0;
      return SVN_NO_ERROR;
    }

  idx[0] = svn_diff__file_datasource_to_index(file_token1->datasource);
  idx[1] = svn_diff__file_datasource_to_index(file_token2->datasource);
  offset[0] = file_token1->offset;
  offset[1] = file_token2->offset;
  chunk[0] = file_baton->chunk[idx[0]];
  chunk[1] = file_baton->chunk[idx[1]];

  do
    {
      for (i = 0; i < 2; i++)
        {
          if (offset_to_chunk(offset[i]) == chunk[i])
            {
              /* If the start of the token is in memory, the entire token is
               * in memory.
               */
              bufp[i] = file_baton->buffer[idx[i]];
              bufp[i] += offset_in_chunk(offset[i]);

              length[i] = total_length;
            }
          else
            {
              /* Read a chunk from disk into a buffer */
              bufp[i] = buffer[i];
              length[i] = total_length > COMPARE_CHUNK_SIZE ? COMPARE_CHUNK_SIZE : total_length;

              SVN_ERR(read_chunk(file_baton->file[idx[i]],
                                 file_baton->path[idx[i]],
                                 bufp[i], length[i], offset[i],
                                 file_baton->pool));
            }
        }

      len = length[0] > length[1] ? length[1] : length[0];
      offset[0] += len;
      offset[1] += len;

      /* Compare two chunks (that could be entire tokens if they both reside
       * in memory).
       */
      *compare = memcmp(bufp[0], bufp[1], len);
      if (*compare != 0)
        return SVN_NO_ERROR;

      total_length -= len;
    }
  while(total_length > 0);

  *compare = 0;
  return SVN_NO_ERROR;
}


static
void
svn_diff__file_token_discard(void *baton,
                             void *token)
{
  svn_diff__file_baton_t *file_baton = baton;
  svn_diff__file_token_t *file_token = token;

  file_token->next = file_baton->tokens;
  file_baton->tokens = file_token;
}


static
void
svn_diff__file_token_discard_all(void *baton)
{
  svn_diff__file_baton_t *file_baton = baton;

  /* Discard all memory in use by the tokens, and close all open files. */
  svn_pool_clear(file_baton->pool);
}


static const svn_diff_fns_t svn_diff__file_vtable =
{
  svn_diff__file_datasource_open,
  svn_diff__file_datasource_close,
  svn_diff__file_datasource_get_next_token,
  svn_diff__file_token_compare,
  svn_diff__file_token_discard,
  svn_diff__file_token_discard_all
};

svn_error_t *
svn_diff_file_diff(svn_diff_t **diff,
                   const char *original,
                   const char *modified,
                   apr_pool_t *pool)
{
  svn_diff__file_baton_t baton;

  memset(&baton, 0, sizeof(baton));
  baton.path[0] = original;
  baton.path[1] = modified;
  baton.pool = svn_pool_create(pool);

  SVN_ERR(svn_diff_diff(diff, &baton, &svn_diff__file_vtable, pool));

  svn_pool_destroy(baton.pool);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_diff_file_diff3(svn_diff_t **diff,
                    const char *original,
                    const char *modified,
                    const char *latest,
                    apr_pool_t *pool)
{
  svn_diff__file_baton_t baton;

  memset(&baton, 0, sizeof(baton));
  baton.path[0] = original;
  baton.path[1] = modified;
  baton.path[2] = latest;
  baton.pool = svn_pool_create(pool);

  SVN_ERR(svn_diff_diff3(diff, &baton, &svn_diff__file_vtable, pool));

  svn_pool_destroy(baton.pool);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_diff_file_diff4(svn_diff_t **diff,
                    const char *original,
                    const char *modified,
                    const char *latest,
                    const char *ancestor,
                    apr_pool_t *pool)
{
  svn_diff__file_baton_t baton;

  memset(&baton, 0, sizeof(baton));
  baton.path[0] = original;
  baton.path[1] = modified;
  baton.path[2] = latest;
  baton.path[3] = ancestor;
  baton.pool = svn_pool_create(pool);

  SVN_ERR(svn_diff_diff4(diff, &baton, &svn_diff__file_vtable, pool));

  svn_pool_destroy(baton.pool);
  return SVN_NO_ERROR;
}


/** Display unified context diffs **/

#define SVN_DIFF__UNIFIED_CONTEXT_SIZE 3

typedef struct svn_diff__file_output_baton_t
{
  svn_stream_t *output_stream;

  const char *path[2];
  apr_file_t *file[2];

  apr_off_t   current_line[2];

  char        buffer[2][4096];
  apr_size_t  length[2];
  char       *curp[2];

  apr_off_t   hunk_start[2];
  apr_off_t   hunk_length[2];
  svn_stringbuf_t *hunk;

  apr_pool_t *pool;
} svn_diff__file_output_baton_t;

typedef enum svn_diff__file_output_unified_type_e
{
  svn_diff__file_output_unified_skip,
  svn_diff__file_output_unified_context,
  svn_diff__file_output_unified_delete,
  svn_diff__file_output_unified_insert
} svn_diff__file_output_unified_type_e;


static
svn_error_t *
svn_diff__file_output_unified_line(svn_diff__file_output_baton_t *baton,
                                   svn_diff__file_output_unified_type_e type,
                                   int idx)
{
  char *curp;
  char *eol;
  apr_size_t length;
  svn_error_t *err;
  svn_boolean_t bytes_processed = FALSE;

  length = baton->length[idx];
  curp = baton->curp[idx];

  /* Lazily update the current line even if we're at EOF.
   * This way we fake output of context at EOF
   */
  baton->current_line[idx]++;

  if (length == 0 && apr_file_eof(baton->file[idx]))
    {
      return SVN_NO_ERROR;
    }

  do
    {
      if (length > 0)
        {
          if (!bytes_processed)
            {
              switch (type)
                {
                case svn_diff__file_output_unified_context:
                  svn_stringbuf_appendbytes(baton->hunk, " ", 1);
                  baton->hunk_length[0]++;
                  baton->hunk_length[1]++;
                  break;
                case svn_diff__file_output_unified_delete:
                  svn_stringbuf_appendbytes(baton->hunk, "-", 1);
                  baton->hunk_length[0]++;
                  break;
                case svn_diff__file_output_unified_insert:
                  svn_stringbuf_appendbytes(baton->hunk, "+", 1);
                  baton->hunk_length[1]++;
                  break;
                default:
                  break;
                }
            }

          /* XXX: '\n' doesn't really cut it.  We need to be able to detect
           * XXX: '\n', '\r' and '\r\n'.
           */
          eol = memchr(curp, '\n', length);

          if (eol != NULL)
            {
              apr_size_t len;

              eol++;
              len = (apr_size_t)(eol - curp);
              length -= len;

              if (type != svn_diff__file_output_unified_skip)
                {
                  svn_stringbuf_appendbytes(baton->hunk, curp, len);
                }

              baton->curp[idx] = eol;
              baton->length[idx] = length;

              err = SVN_NO_ERROR;

              break;
            }

          if (type != svn_diff__file_output_unified_skip)
            {
              svn_stringbuf_appendbytes(baton->hunk, curp, length);
            }

          bytes_processed = TRUE;
        }

      curp = baton->buffer[idx];
      length = sizeof(baton->buffer[idx]);

      err = svn_io_file_read(baton->file[idx], curp, &length, baton->pool);
    }
  while (! err);

  if (err && ! APR_STATUS_IS_EOF(err->apr_err))
    return err;

  if (err && APR_STATUS_IS_EOF(err->apr_err))
    {
      svn_error_clear (err);
      /* Special case if we reach the end of file AND the last line is in the
         changed range AND the file doesn't end with a newline */
      if (bytes_processed && (type != svn_diff__file_output_unified_skip))
        {
          svn_stringbuf_appendcstr(baton->hunk,
            APR_EOL_STR "\\ No newline at end of file" APR_EOL_STR);
        }

      baton->length[idx] = 0;
    }

  return SVN_NO_ERROR;
}

static
svn_error_t *
svn_diff__file_output_unified_flush_hunk(svn_diff__file_output_baton_t *baton)
{
  apr_off_t target_line;
  apr_size_t hunk_len;
  int i;

  if (svn_stringbuf_isempty(baton->hunk))
    {
      /* Nothing to flush */
      return SVN_NO_ERROR;
    }

  target_line = baton->hunk_start[0] + baton->hunk_length[0]
                + SVN_DIFF__UNIFIED_CONTEXT_SIZE;

  /* Add trailing context to the hunk */
  while (baton->current_line[0] < target_line)
    {
      SVN_ERR(svn_diff__file_output_unified_line(baton,
                svn_diff__file_output_unified_context, 0));
    }

  /* If the file is non-empty, convert the line indexes from
     zero based to one based */
  for (i = 0; i < 2; i++)
    {
      if (baton->hunk_length[i] > 0)
        baton->hunk_start[i]++;
    }

  /* Output the hunk header.  If the hunk length is 1, the file is a one line
     file.  In this case, surpress the number of lines in the hunk (it is
     1 implicitly) 
   */
  SVN_ERR(svn_stream_printf(baton->output_stream, baton->pool,
                            "@@ -%" APR_OFF_T_FMT,
                            baton->hunk_start[0]));
  if (baton->hunk_length[0] != 1)
    {
      SVN_ERR(svn_stream_printf(baton->output_stream, baton->pool,
                                ",%" APR_OFF_T_FMT,
                                baton->hunk_length[0]));
    }

  SVN_ERR(svn_stream_printf(baton->output_stream, baton->pool,
                            " +%" APR_OFF_T_FMT,
                            baton->hunk_start[1]));
  if (baton->hunk_length[1] != 1)
    {
      SVN_ERR(svn_stream_printf(baton->output_stream, baton->pool,
                                ",%" APR_OFF_T_FMT,
                                baton->hunk_length[1]));
    }

  SVN_ERR(svn_stream_printf(baton->output_stream, baton->pool,
                            " @@" APR_EOL_STR));

  /* Output the hunk content */
  hunk_len = baton->hunk->len;
  SVN_ERR (svn_stream_write(baton->output_stream, baton->hunk->data,
                            &hunk_len));

  /* Prepare for the next hunk */
  baton->hunk_length[0] = 0;
  baton->hunk_length[1] = 0;
  svn_stringbuf_setempty(baton->hunk);

  return SVN_NO_ERROR;
}

static
svn_error_t *
svn_diff__file_output_unified_diff_modified(void *baton,
  apr_off_t original_start, apr_off_t original_length,
  apr_off_t modified_start, apr_off_t modified_length,
  apr_off_t latest_start, apr_off_t latest_length)
{
  svn_diff__file_output_baton_t *output_baton = baton;
  apr_off_t target_line[2];
  int i;

  target_line[0] = original_start >= SVN_DIFF__UNIFIED_CONTEXT_SIZE
                   ? original_start - SVN_DIFF__UNIFIED_CONTEXT_SIZE : 0;
  target_line[1] = modified_start;

  /* If the changed ranges are far enough apart (no overlapping or connecting
     context), flush the current hunk, initialize the next hunk and skip the
     lines not in context.  Also do this when this is the first hunk.
   */
  if (output_baton->current_line[0] < target_line[0]
      && (output_baton->hunk_start[0] + output_baton->hunk_length[0]
          + SVN_DIFF__UNIFIED_CONTEXT_SIZE < target_line[0]
          || output_baton->hunk_length[0] == 0))
    {
      SVN_ERR(svn_diff__file_output_unified_flush_hunk(output_baton));

      output_baton->hunk_start[0] = target_line[0];
      output_baton->hunk_start[1] = target_line[1] + target_line[0]
                                    - original_start;

      /* Skip lines until we are at the beginning of the context we want to
         display */
      while (output_baton->current_line[0] < target_line[0])
        {
          SVN_ERR(svn_diff__file_output_unified_line(output_baton,
                    svn_diff__file_output_unified_skip, 0));
        }
    }

  /* Skip lines until we are at the start of the changed range */
  while (output_baton->current_line[1] < target_line[1])
    {
      SVN_ERR(svn_diff__file_output_unified_line(output_baton,
                svn_diff__file_output_unified_skip, 1));
    }

  /* Output the context preceding the changed range */
  while (output_baton->current_line[0] < original_start)
    {
      SVN_ERR(svn_diff__file_output_unified_line(output_baton,
                svn_diff__file_output_unified_context, 0));
    }

  target_line[0] = original_start + original_length;
  target_line[1] = modified_start + modified_length;

  /* Output the changed range */
  for (i = 0; i < 2; i++)
    {
      while (output_baton->current_line[i] < target_line[i])
        {
          SVN_ERR(svn_diff__file_output_unified_line(output_baton, i == 0
                    ? svn_diff__file_output_unified_delete
                    : svn_diff__file_output_unified_insert, i));
        }
    }

  return SVN_NO_ERROR;
}

static
const char *
svn_diff__file_output_unified_default_hdr(apr_pool_t *pool,
                                          const char *path)
{
  apr_finfo_t file_info;
  apr_time_exp_t exploded_time;
  char time_buffer[64];
  apr_size_t time_len;

  svn_io_stat(&file_info, path, APR_FINFO_MTIME, pool);
  apr_time_exp_lt(&exploded_time, file_info.mtime);

  apr_strftime(time_buffer, &time_len, sizeof(time_buffer) - 1,
               "%a %b %e %H:%M:%S %Y", &exploded_time);

  return apr_psprintf(pool, "%s\t%s", path, time_buffer);
}

static const svn_diff_output_fns_t svn_diff__file_output_unified_vtable =
{
  NULL, /* output_common */
  svn_diff__file_output_unified_diff_modified,
  NULL, /* output_diff_latest */
  NULL, /* output_diff_common */
  NULL  /* output_conflict */
};

svn_error_t *
svn_diff_file_output_unified(svn_stream_t *output_stream,
                             svn_diff_t *diff,
                             const char *original_path,
                             const char *modified_path,
                             const char *original_header,
                             const char *modified_header,
                             apr_pool_t *pool)
{
  svn_diff__file_output_baton_t baton;
  int i;

  if (svn_diff_contains_diffs(diff))
    {
      memset(&baton, 0, sizeof(baton));
      baton.output_stream = output_stream;
      baton.pool = pool;
      baton.path[0] = original_path;
      baton.path[1] = modified_path;
      baton.hunk = svn_stringbuf_create("", pool);

      for (i = 0; i < 2; i++)
        {
          SVN_ERR (svn_io_file_open (&baton.file[i], baton.path[i],
                                     APR_READ, APR_OS_DEFAULT, pool) );
        }

      if (original_header == NULL)
        {
          original_header =
            svn_diff__file_output_unified_default_hdr(pool, original_path);
        }

      if (modified_header == NULL)
        {
          modified_header =
            svn_diff__file_output_unified_default_hdr(pool, modified_path);
        }

      SVN_ERR(svn_stream_printf(output_stream, pool,
                                "--- %s" APR_EOL_STR
                                "+++ %s" APR_EOL_STR,
                                original_header, modified_header));

      SVN_ERR(svn_diff_output(diff, &baton,
                              &svn_diff__file_output_unified_vtable));
      SVN_ERR(svn_diff__file_output_unified_flush_hunk(&baton));

      for (i = 0; i < 2; i++)
        {
	  SVN_ERR (svn_io_file_close (baton.file[i], pool));
        }
    }

  return SVN_NO_ERROR;
}


/** Display diff3 **/

typedef struct svn_diff3__file_output_baton_t
{
  svn_stream_t *output_stream;

  const char *path[3];

  apr_off_t   current_line[3];

  char       *buffer[3];
  char       *endp[3];
  char       *curp[3];

  const char *conflict_modified;
  const char *conflict_original;
  const char *conflict_separator;
  const char *conflict_latest;

  svn_boolean_t display_original_in_conflict;
  svn_boolean_t display_resolved_conflicts;

  apr_pool_t *pool;
} svn_diff3__file_output_baton_t;

typedef enum svn_diff3__file_output_type_e
{
  svn_diff3__file_output_skip,
  svn_diff3__file_output_normal
} svn_diff3__file_output_type_e;


static
svn_error_t *
svn_diff3__file_output_line(svn_diff3__file_output_baton_t *baton,
                            svn_diff3__file_output_type_e type,
                            int idx)
{
  char *curp;
  char *endp;
  char *eol;
  apr_size_t len;

  curp = baton->curp[idx];
  endp = baton->endp[idx];

  /* Lazily update the current line even if we're at EOF.
   */
  baton->current_line[idx]++;

  if (curp == endp)
    return SVN_NO_ERROR;

  /* XXX: '\n' doesn't really cut it.  We need to be able to detect
   * XXX: '\n', '\r' and '\r\n'.
   */
  eol = memchr(curp, '\n', endp - curp);
  if (!eol)
    eol = endp;
  else
    eol++;

  if (type != svn_diff3__file_output_skip)
    {
      len = eol - curp;
      SVN_ERR(svn_stream_write(baton->output_stream, curp, &len));
    }

  baton->curp[idx] = eol;

  return SVN_NO_ERROR;
}

static
svn_error_t *
svn_diff3__file_output_hunk(void *baton,
  int idx,
  apr_off_t target_line, apr_off_t target_length)
{
  svn_diff3__file_output_baton_t *output_baton = baton;

  /* Skip lines until we are at the start of the changed range */
  while (output_baton->current_line[idx] < target_line)
    {
      SVN_ERR(svn_diff3__file_output_line(output_baton,
                svn_diff3__file_output_skip, idx));
    }

  target_line += target_length;

  while (output_baton->current_line[idx] < target_line)
    {
      SVN_ERR(svn_diff3__file_output_line(output_baton,
                svn_diff3__file_output_normal, idx));
    }

  return SVN_NO_ERROR;
}

static
svn_error_t *
svn_diff3__file_output_common(void *baton,
  apr_off_t original_start, apr_off_t original_length,
  apr_off_t modified_start, apr_off_t modified_length,
  apr_off_t latest_start, apr_off_t latest_length)
{
  return svn_diff3__file_output_hunk(baton, 0,
           original_start, original_length);
}

static
svn_error_t *
svn_diff3__file_output_diff_modified(void *baton,
  apr_off_t original_start, apr_off_t original_length,
  apr_off_t modified_start, apr_off_t modified_length,
  apr_off_t latest_start, apr_off_t latest_length)
{
  return svn_diff3__file_output_hunk(baton, 1,
           modified_start, modified_length);
}

static
svn_error_t *
svn_diff3__file_output_diff_latest(void *baton,
  apr_off_t original_start, apr_off_t original_length,
  apr_off_t modified_start, apr_off_t modified_length,
  apr_off_t latest_start, apr_off_t latest_length)
{
  return svn_diff3__file_output_hunk(baton, 2,
           latest_start, latest_length);
}

static
svn_error_t *
svn_diff3__file_output_conflict(void *baton,
  apr_off_t original_start, apr_off_t original_length,
  apr_off_t modified_start, apr_off_t modified_length,
  apr_off_t latest_start, apr_off_t latest_length,
  svn_diff_t *diff);

static const svn_diff_output_fns_t svn_diff3__file_output_vtable =
{
  svn_diff3__file_output_common,
  svn_diff3__file_output_diff_modified,
  svn_diff3__file_output_diff_latest,
  svn_diff3__file_output_diff_modified, /* output_diff_common */
  svn_diff3__file_output_conflict
};

static
svn_error_t *
svn_diff3__file_output_conflict(void *baton,
  apr_off_t original_start, apr_off_t original_length,
  apr_off_t modified_start, apr_off_t modified_length,
  apr_off_t latest_start, apr_off_t latest_length,
  svn_diff_t *diff)
{
  svn_diff3__file_output_baton_t *file_baton = baton;
  apr_size_t len;

  if (diff && file_baton->display_resolved_conflicts)
    {
      return svn_diff_output(diff, baton,
                             &svn_diff3__file_output_vtable);
    }

  len = strlen(file_baton->conflict_modified);
  SVN_ERR(svn_stream_write(file_baton->output_stream,
                           file_baton->conflict_modified,
                           &len));
  len = sizeof(APR_EOL_STR) - 1;
  SVN_ERR(svn_stream_write(file_baton->output_stream,
                           APR_EOL_STR, &len));

  SVN_ERR(svn_diff3__file_output_hunk(baton, 1,
            modified_start, modified_length));

  if (file_baton->display_original_in_conflict)
    {
      len = strlen(file_baton->conflict_original);
      SVN_ERR(svn_stream_write(file_baton->output_stream,
                               file_baton->conflict_original, &len));
      len = sizeof(APR_EOL_STR) - 1;
      SVN_ERR(svn_stream_write(file_baton->output_stream,
                               APR_EOL_STR, &len));

      SVN_ERR(svn_diff3__file_output_hunk(baton, 0,
              original_start, original_length));
    }

  len = strlen(file_baton->conflict_separator);
  SVN_ERR(svn_stream_write(file_baton->output_stream,
                           file_baton->conflict_separator, &len));
  len = sizeof(APR_EOL_STR) - 1;
  SVN_ERR(svn_stream_write(file_baton->output_stream,
                           APR_EOL_STR, &len));

  SVN_ERR(svn_diff3__file_output_hunk(baton, 2,
            latest_start, latest_length));

  len = strlen(file_baton->conflict_latest);
  SVN_ERR(svn_stream_write(file_baton->output_stream,
                           file_baton->conflict_latest, &len));
  len = sizeof(APR_EOL_STR) - 1;
  SVN_ERR(svn_stream_write(file_baton->output_stream,
                           APR_EOL_STR, &len));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_diff_file_output_merge(svn_stream_t *output_stream,
                           svn_diff_t *diff,
                           const char *original_path,
                           const char *modified_path,
                           const char *latest_path,
                           const char *conflict_original,
                           const char *conflict_modified,
                           const char *conflict_latest,
                           const char *conflict_separator,
                           svn_boolean_t display_original_in_conflict,
                           svn_boolean_t display_resolved_conflicts,
                           apr_pool_t *pool)
{
  svn_diff3__file_output_baton_t baton;
  apr_file_t *file[3];
  apr_off_t size;
  int idx;
#if APR_HAS_MMAP
  apr_mmap_t *mm[3] = { 0 };
#endif /* APR_HAS_MMAP */

  memset(&baton, 0, sizeof(baton));
  baton.output_stream = output_stream;
  baton.pool = pool;
  baton.path[0] = original_path;
  baton.path[1] = modified_path;
  baton.path[2] = latest_path;
  baton.conflict_modified = conflict_modified ? conflict_modified
                            : apr_psprintf(pool, "<<<<<<< %s", modified_path);
  baton.conflict_original = conflict_original ? conflict_original
                            : apr_psprintf(pool, "||||||| %s", original_path);
  baton.conflict_separator = conflict_separator ? conflict_separator
                             : "=======";
  baton.conflict_latest = conflict_latest ? conflict_latest
                          : apr_psprintf(pool, ">>>>>>> %s", latest_path);

  baton.display_original_in_conflict = display_original_in_conflict;
  baton.display_resolved_conflicts = display_resolved_conflicts &&
                                     !display_original_in_conflict;

  for (idx = 0; idx < 3; idx++)
    {
      SVN_ERR(map_or_read_file(&file[idx],
                               MMAP_T_ARG(mm[idx])
                               &baton.buffer[idx], &size,
                               baton.path[idx], pool));

      baton.curp[idx] = baton.buffer[idx];
      baton.endp[idx] = baton.buffer[idx];

      if (baton.endp[idx])
        baton.endp[idx] += size;
    }

  SVN_ERR(svn_diff_output(diff, &baton,
                          &svn_diff3__file_output_vtable));

  for (idx = 0; idx < 3; idx++)
    {
#if APR_HAS_MMAP
      if (mm[idx])
        {
          apr_status_t rv = apr_mmap_delete(mm[idx]);
          if (rv != APR_SUCCESS)
            {
              return svn_error_wrap_apr(rv, "Failed to delete mmap '%s'",
                                        baton.path[idx]);
            }
        }
#endif /* APR_HAS_MMAP */

      if (file[idx])
        {
          SVN_ERR (svn_io_file_close (file[idx], pool));
        }
    }

  return SVN_NO_ERROR;
}
