/* uuid.c : operations on repository uuids
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

#include <assert.h>

#include "fs.h"
#include "fs_fs.h"
#include "err.h"


svn_error_t *
svn_fs_fs__get_uuid (svn_fs_t *fs,
                 const char **uuid,
                 apr_pool_t *pool)
{
  SVN_ERR (svn_fs_fs__check_fs (fs));

  SVN_ERR (svn_fs__fs_get_uuid (uuid, fs, pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__set_uuid (svn_fs_t *fs,
                 const char *uuid,
                 apr_pool_t *pool)
{
  SVN_ERR (svn_fs__fs_set_uuid (fs, uuid, pool));
  
  return SVN_NO_ERROR;
}

