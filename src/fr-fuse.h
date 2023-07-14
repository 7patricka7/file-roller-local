/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  File-Roller
 *
 *  Copyright (C) 2023 SUSE LLC
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Written by:
 *     Alynx Zhou <alynx.zhou@gmail.com>
 */

#ifndef FR_FUSE_H
#define FR_FUSE_H

#include <gio/gio.h>
/* We only have async extracting, so using lowlevel async API is required. */
#define FUSE_USE_VERSION 35
#include <fuse_lowlevel.h>
#include "fr-archive.h"

G_BEGIN_DECLS

#define FR_TYPE_FUSE (fr_fuse_get_type ())
G_DECLARE_FINAL_TYPE (FrFuse, fr_fuse, FR, FUSE, GObject)

#define FR_FUSE_ERROR (fr_fuse_error_quark ())
typedef enum {
	FR_FUSE_ERROR_FUSE_NEW,
	FR_FUSE_ERROR_FUSE_MOUNT
} FrFuseError;
GQuark fr_fuse_error_quark (void);

FrFuse *fr_fuse_new (FrArchive *archive, const char *password, GError **error);
void fr_fuse_mount (FrFuse *ff, GError **error);
void fr_fuse_unmount (FrFuse *ff);
GFile *fr_fuse_get_mount_dir (FrFuse *ff);
void fr_fuse_update_inodes (FrFuse *ff);
gboolean fr_fuse_query_file_in_path (FrFuse *ff, GFile *file, const char *path);

G_END_DECLS

#endif /* FR_FUSE_H */
