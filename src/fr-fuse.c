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

#include "fr-fuse.h"
#include "glib-utils.h"
#include "file-utils.h"
#include "fr-file-data.h"
#include "fuse_lowlevel.h"

struct _FrFuse {
	GObject parent_instance;
	FrArchive *archive;
	char *password;
	struct fuse_session *fuse;

	/* Index will be inode number. */
	GPtrArray *inodes;
	/* This is used to quickly find an inode for a path. */
	GHashTable *paths;

	/*
	 * We cannot directly extract file into buffer, so use 2 dirs here.
	 * `mount_dir` is where we expose the archive via FUSE, and will provide
	 * it as drag source.
	 * `work_dir` is where we actually extract files, we will then read
	 * files from it into buffers and provide buffers to FUSE.
	 */
	GFile *mount_dir;
	GFile *work_dir;

	GThread *loop_thread;
	gboolean started;

	GCancellable *cancellable;
	GError *error;
};
G_DEFINE_FINAL_TYPE (FrFuse, fr_fuse, G_TYPE_OBJECT)

G_DEFINE_QUARK (fr-fuse-error-quark, fr_fuse_error)

#define FR_FUSE_INODE_START (FUSE_ROOT_ID + 1)

static void
_fr_fuse_create_inode (FrFuse *ff, FrFileData *src)
{
	FrFileData *fdata = src == NULL ? NULL : fr_file_data_copy (src);
	// g_print ("Create ino %u for %p\n", ff->inodes->len, fdata);
	g_ptr_array_add (ff->inodes, fdata);
	/* We may add empty inodes. */
	if (fdata != NULL)
		g_hash_table_replace (ff->paths, fdata->full_path, fdata);
}

static void
_fr_fuse_delete_inode (FrFuse *ff, unsigned int ino)
{
	FrFileData *fdata = NULL;

	if (ino <= FUSE_ROOT_ID || ino >= ff->inodes->len)
		return;

	fdata = ff->inodes->pdata[ino];
	/* Check whether already deleted. */
	if (fdata == NULL)
		return;

	// g_print ("Delete ino %u\n", ino);
	g_hash_table_remove (ff->paths, fdata->full_path);
	/*
	 * We never use `g_ptr_array_remove()` to delete an inode, just clear
	 * it, so every file has its unique inode.
	 */
	g_clear_pointer (&ff->inodes->pdata[ino], fr_file_data_free);
}

static FrFileData *
_fr_fuse_get_file_by_ino (FrFuse *ff, fuse_ino_t ino)
{
	if (ino <= FUSE_ROOT_ID || ino >= ff->inodes->len)
		return NULL;

	return ff->inodes->pdata[ino];
}

static goffset
_fr_fuse_get_size_by_ino (FrFuse *ff, fuse_ino_t ino)
{
	goffset total_size = 0;

	/*
	 * Archive has size for files and dirs, but because it doesn't have an
	 * entry for root dir, we will calculate it manually.
	 */
	if (ino != FUSE_ROOT_ID) {
		FrFileData *fdata = _fr_fuse_get_file_by_ino (ff, ino);
		if (fdata == NULL)
			return 0;
		return fr_file_data_is_dir (fdata) ?
			fdata->dir_size : fdata->size;
	}

	for (unsigned int i = FR_FUSE_INODE_START; i < ff->inodes->len; ++i) {
		FrFileData *fdata = ff->inodes->pdata[i];
		g_autofree char *canonicalized = NULL;
		if (fdata == NULL)
			continue;

		canonicalized = g_canonicalize_filename (fdata->path,
							 "/");
		if (g_strcmp0 (canonicalized, "/") == 0)
			total_size += fr_file_data_is_dir (fdata) ?
				fdata->dir_size : fdata->size;
	}

	return total_size;
}

struct dirbuf {
	char *buffer;
	size_t size;
};

static void
_fr_fuse_dirbuf_add (fuse_req_t req,
		     struct dirbuf *b,
		     const char *name,
		     fuse_ino_t ino)
{
	struct stat stbuf;
	memset (&stbuf, 0, sizeof (stbuf));
	stbuf.st_ino = ino;
	size_t old_size = b->size;
	b->size += fuse_add_direntry (req, NULL, 0, name, &stbuf, 0);
	b->buffer = g_realloc (b->buffer, b->size);
	fuse_add_direntry (req, b->buffer + old_size, b->size - old_size, name,
			   &stbuf, b->size);
}

static int
_fr_fuse_reply_buf_limited (fuse_req_t req,
			    const char *buf,
			    size_t bufsize,
			    off_t off,
			    size_t maxsize)
{
	if ((size_t)off < bufsize)
		return fuse_reply_buf (req, buf + off,
				       MIN (bufsize - off, maxsize));
	else
		return fuse_reply_buf (req, NULL, 0);
}

static void
_fr_fuse_lookup (fuse_req_t req, fuse_ino_t parent, const char *name)
{
	FrFuse *ff = fuse_req_userdata (req);
	g_autofree char *path = NULL;
	gboolean found = FALSE;

	// g_print ("lookup: %lu, %s\n", parent, name);

	if (parent == FUSE_ROOT_ID) {
		path = g_strdup ("/");
	} else {
		FrFileData *dir = NULL;
		dir = _fr_fuse_get_file_by_ino (ff, parent);
		if (dir == NULL) {
			fuse_reply_err (req, ENOTDIR);
			return;
		}
		/*
		 * NOTE: Some archives, for example tar, allow user to archive
		 * directory called `.`. This will conflict with file system
		 * because we always assume `.` is the same with current dir.
		 * When extracting such tarball, `.` will be merged with current
		 * dir, so we also handle it like this with
		 * `g_canonicalize_file()`, it will resolve `.` and `..` in
		 * path.
		 */
		path = g_canonicalize_filename (dir->full_path, "/");
	}

	if (g_strcmp0 (name, ".") == 0) {
		/*
		 * Are you kidding? Is there anyone looking up the dir itself?
		 * Just in case we handle it here.
		 */
		struct fuse_entry_param e;
		memset (&e, 0, sizeof (e));
		e.ino = FUSE_ROOT_ID;
		e.attr_timeout = 0.0;
		e.entry_timeout = 0.0;
		e.attr.st_ino = e.ino;
		e.attr.st_mode = S_IFDIR | 0755;
		e.attr.st_nlink = 1;
		e.attr.st_size = _fr_fuse_get_size_by_ino (ff, FUSE_ROOT_ID);
		fuse_reply_entry (req, &e);
		return;
	}

	for (unsigned int i = FR_FUSE_INODE_START; i < ff->inodes->len; ++i) {
		FrFileData *fdata = ff->inodes->pdata[i];
		g_autofree char *canonicalized = NULL;
		if (fdata == NULL)
			continue;

		canonicalized = g_canonicalize_filename (fdata->path, "/");
		if (g_strcmp0 (canonicalized, path) == 0 &&
		    g_strcmp0 (fdata->name, name) == 0) {
			struct fuse_entry_param e;
			memset (&e, 0, sizeof (e));
			e.ino = i;
			e.attr_timeout = 0.0;
			e.entry_timeout = 0.0;
			e.attr.st_ino = i;
			e.attr.st_mode = fr_file_data_is_dir (fdata) ?
				S_IFDIR | 0755 : S_IFREG | 0644;
			e.attr.st_nlink = 1;
			e.attr.st_size = _fr_fuse_get_size_by_ino (ff, i);
			fuse_reply_entry (req, &e);
			found = TRUE;
			break;
		}
	}

	if (!found)
		fuse_reply_err (req, ENOENT);
}

static void
_fr_fuse_getattr (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	FrFuse *ff = fuse_req_userdata (req);
	FrFileData *fdata = NULL;
	gboolean is_dir = FALSE;
	struct stat stbuf;

	// g_print ("getattr: %lu\n", ino);

	if (ino == FUSE_ROOT_ID) {
		is_dir = TRUE;
	} else {
		fdata = _fr_fuse_get_file_by_ino (ff, ino);
		if (fdata == NULL) {
			fuse_reply_err (req, ENOENT);
			return;
		}
		is_dir = fr_file_data_is_dir (fdata);
	}

	memset (&stbuf, 0, sizeof (stbuf));
	stbuf.st_ino = ino;
	stbuf.st_mode = is_dir ? S_IFDIR | 0755 : S_IFREG | 0644;
	stbuf.st_nlink = 1;
	stbuf.st_size = _fr_fuse_get_size_by_ino (ff, ino);
	fuse_reply_attr (req, &stbuf, 0.0);
}

/* The inode <-> file name map is assigned via readdir. */
static void
_fr_fuse_readdir (fuse_req_t req,
		  fuse_ino_t ino,
		  size_t size,
		  off_t off,
		  struct fuse_file_info *fi)
{
	FrFuse *ff = fuse_req_userdata (req);
	g_autofree char *dir_path = NULL;
	struct dirbuf b;

	// g_print ("readdir: %lu\n", ino);

	if (ino == FUSE_ROOT_ID) {
		dir_path = g_strdup ("/");
	} else {
		FrFileData *dir = NULL;
		dir = _fr_fuse_get_file_by_ino (ff, ino);
		if (dir == NULL) {
			fuse_reply_err (req, ENOTDIR);
			return;
		}
		/*
		 * Some archives, like tar, full_path is ended with `/`.
		 * `g_canonicalize_filename()` will remove the trailing `/`.
		 * It is safe to pass `/` as `relative_to`, because archives
		 * always starts path from root.
		 */
		dir_path = g_canonicalize_filename (dir->full_path, "/");
	}

	// g_print ("readdir: dir_path: %s\n", dir_path);

	memset (&b, 0, sizeof (b));
	for (unsigned int i = FR_FUSE_INODE_START; i < ff->inodes->len; ++i) {
		FrFileData *fdata = ff->inodes->pdata[i];
		g_autofree char *canonicalized = NULL;
		if (fdata == NULL)
			continue;

		canonicalized = g_canonicalize_filename (fdata->path, "/");
		// g_print ("canonicalized: %s\n", canonicalized);
		// g_print ("dir_path: %s\n", dir_path);
		// g_print ("\n");
		if (g_strcmp0 (canonicalized, dir_path) == 0) {
			// g_print ("readdir: replied %s with ino %d\n", fdata->name, i);
			_fr_fuse_dirbuf_add (req, &b, fdata->name, i);
		}
	}
	_fr_fuse_reply_buf_limited (req, b.buffer, b.size, off, size);

	g_free (b.buffer);
}

static void
_fr_fuse_open (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	FrFuse *ff = fuse_req_userdata (req);
	FrFileData *fdata = NULL;

	// g_print ("open: %lu\n", ino);

	if (ino == FUSE_ROOT_ID) {
		fuse_reply_err (req, EISDIR);
		return;
	}

	if ((fi->flags & O_ACCMODE) != O_RDONLY) {
		fuse_reply_err (req, EACCES);
		return;
	}

	fdata = _fr_fuse_get_file_by_ino (ff, ino);
	if (fdata == NULL) {
		fuse_reply_err (req, ENOENT);
		return;
	} else if (fr_file_data_is_dir (fdata)) {
		fuse_reply_err (req, EISDIR);
		return;
	}

	// g_print ("open: file path: %s\n", fdata->original_path);

	/* Allow bigger reading segment. */
	fi->direct_io = 1;
	fuse_reply_open (req, fi);
}

struct _load {
	FrFuse *ff;
	fuse_req_t req;
	off_t off;
	size_t size;
};

static void
_fr_fuse_on_loaded (GObject      *source_object,
		    GAsyncResult *result,
		    gpointer      user_data)
{
	g_autofree struct _load *ldata = user_data;
	g_autoptr (FrFuse) ff = ldata->ff;
	GFile *file = G_FILE (source_object);
	g_autofree char *buffer = NULL;
	size_t size = 0;

	g_file_load_contents_finish (file, result, &buffer, &size, NULL,
				     &ff->error);

	_fr_fuse_reply_buf_limited (ldata->req, buffer, size, ldata->off,
				    ldata->size);
}

struct _extract {
	FrFuse *ff;
	char *original_path;
	fuse_req_t req;
	off_t off;
	size_t size;
};

static void
_fr_fuse_on_extracted (GObject      *source_object,
		       GAsyncResult *result,
		       gpointer      user_data)
{
	g_autofree struct _extract *edata = user_data;
	g_autoptr (FrFuse) ff = edata->ff;
	g_autofree char *original_path = edata->original_path;
	g_autoptr (GFile) extracted_file = _g_file_append_path (ff->work_dir,
								original_path,
								NULL);
	struct _load *ldata = NULL;

	fr_archive_operation_finish (FR_ARCHIVE (source_object), result,
				     &ff->error);

	ldata = g_malloc (sizeof (*ldata));
	ldata->ff = g_object_ref (ff);
	ldata->req = edata->req;
	ldata->off = edata->off;
	ldata->size = edata->size;

	g_file_load_contents_async (extracted_file, ff->cancellable,
				    _fr_fuse_on_loaded, ldata);
}

static void
_fr_fuse_read (fuse_req_t req,
	       fuse_ino_t ino,
	       size_t size,
	       off_t off,
	       struct fuse_file_info *fi)
{
	FrFuse *ff = fuse_req_userdata (req);
	FrFileData *fdata = NULL;
	GList *file_list = NULL;
	struct _extract *edata = NULL;

	// g_print ("read: %lu\n", ino);

	if (ino == FUSE_ROOT_ID) {
		fuse_reply_err (req, EISDIR);
		return;
	}

	fdata = _fr_fuse_get_file_by_ino (ff, ino);
	if (fdata == NULL) {
		fuse_reply_err (req, ENOENT);
		return;
	} else if (fr_file_data_is_dir (fdata)) {
		fuse_reply_err (req, EISDIR);
		return;
	}

	/*
	 * We don't need to call `g_canonicalize_filename()`, both extracting of
	 * tar and `GFile` will resolve `.` and `..` by themselves.
	 */
	file_list = g_list_append (file_list, g_strdup (fdata->original_path));
	edata = g_malloc (sizeof (*edata));
	edata->ff = g_object_ref (ff);
	edata->original_path = g_strdup (fdata->original_path);
	edata->req = req;
	edata->off = off;
	edata->size = size;

	// g_print ("read: file path: %s\n", fdata->original_path);

	/*
	 * NOTE: Do extracting, but don't overwrite! That is very IMPORTANT,
	 * because kernel and FUSE do not read the whole file, but they split it
	 * into many read calls with ~128 KB segments. We cannot afford extract
	 * each time and will get error if we do so, so just extract at the
	 * first read call, and then reuse the same file.
	 */
	fr_archive_extract (ff->archive,
			    file_list,
			    ff->work_dir,
			    NULL,
			    FALSE,
			    /* TODO: Update overwrite. */
			    FALSE,
			    FALSE,
			    ff->password,
			    ff->cancellable,
			    _fr_fuse_on_extracted,
			    edata);

	g_list_free_full (file_list, g_free);
}

static void
_fr_fuse_release (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	FrFuse *ff = fuse_req_userdata (req);
	FrFileData *fdata = NULL;
	g_autoptr (GFile) extracted_file = NULL;

	// g_print ("release: %lu\n", ino);

	fdata = _fr_fuse_get_file_by_ino (ff, ino);
	if (fdata == NULL) {
		fuse_reply_err (req, ENOENT);
		return;
	} else if (fr_file_data_is_dir (fdata)) {
		fuse_reply_err (req, EISDIR);
		return;
	}

	// g_print ("release: file path: %s\n", fdata->original_path);

	extracted_file = _g_file_append_path (ff->work_dir,
					      fdata->original_path, NULL);
	/*
	 * If a process first `open()` then `close()` a file, but do not
	 * `read()` it, we still get `release()`, but we don't need to delete
	 * extracted file because we only extract file in `read()`, so check
	 * before actually delete it.
	 */
	if (g_file_query_exists (extracted_file, ff->cancellable))
		g_file_delete (extracted_file, ff->cancellable, &ff->error);
}

static const struct fuse_lowlevel_ops lowlevel_ops = {
	.lookup = _fr_fuse_lookup,
	.getattr = _fr_fuse_getattr,
	.readdir = _fr_fuse_readdir,
	.open = _fr_fuse_open,
	.read = _fr_fuse_read,
	/*
	 * Typically functions above is enough for a read-only FUSE file system,
	 * but we need to refresh files if archive get updated, I just simply
	 * remove extracted file in `release()`, so if no one reading a file,
	 * it will be removed, and the next one reading will get a newly
	 * extracted file. This prevents from getting corrupted files.
	 */
	.release = _fr_fuse_release
};

static void
fr_fuse_init (FrFuse *ff)
{
	ff->error = NULL;
	ff->loop_thread = NULL;
	ff->archive = NULL;
	ff->inodes = NULL;
	ff->paths = NULL;
	ff->started = FALSE;

	/*
	 * We only need FUSE to implement drag source, not drop target, so make
	 * the FUSE mount read-only so we don't need to handle write into
	 * archive files and implement a lot of write related APIs.
	 * Use `default_permissions` so kernel will do most permissions checks
	 * for us.
	 */
	/* argv[0] is assumed to be the program name, don't remove. */
	char *fuse_argv[] = {
		"file-roller", "-o", "ro,default_permissions", NULL
	};
	struct fuse_args fuse_args = {
		.allocated = FALSE,
		.argc = G_N_ELEMENTS (fuse_argv) - 1,
		.argv = fuse_argv
	};
	ff->fuse = fuse_session_new (&fuse_args, &lowlevel_ops,
				     sizeof (lowlevel_ops), ff);
	if (ff->fuse == NULL) {
		g_set_error (&ff->error, FR_FUSE_ERROR, FR_FUSE_ERROR_FUSE_NEW,
			     "Failed to create FUSE session.");
		return;
	}

	ff->work_dir = _g_file_get_temp_work_dir (NULL);
	ff->mount_dir = _g_file_get_temp_work_dir (NULL);

	fr_fuse_mount (ff, &ff->error);
}

static void
fr_fuse_dispose (GObject *obj)
{
	FrFuse *ff = FR_FUSE (obj);

	if (ff->started)
		fr_fuse_unmount (ff);

	_g_file_remove_directory (ff->mount_dir, NULL, NULL);
	g_clear_object (&ff->mount_dir);

	_g_file_remove_directory (ff->work_dir, NULL, NULL);
	g_clear_object (&ff->work_dir);

	g_clear_object (&ff->archive);

	G_OBJECT_CLASS (fr_fuse_parent_class)->dispose (obj);
}

static void
fr_fuse_finalize (GObject *obj)
{
	FrFuse *ff = FR_FUSE (obj);

	g_clear_pointer (&ff->fuse, fuse_session_destroy);
	g_clear_pointer (&ff->password, g_free);

	G_OBJECT_CLASS (fr_fuse_parent_class)->finalize (obj);
}

static void
fr_fuse_class_init (FrFuseClass *ff_class)
{
	GObjectClass *obj_class = G_OBJECT_CLASS (ff_class);

	obj_class->dispose = fr_fuse_dispose;
	obj_class->finalize = fr_fuse_finalize;
}

FrFuse *
fr_fuse_new (FrArchive *archive, const char *password, GError **error)
{
	FrFuse *ff = NULL;

	ff = g_object_new (FR_TYPE_FUSE, NULL);
	if (ff->error != NULL) {
		g_object_unref (ff);
		g_propagate_error(error, ff->error);
		return NULL;
	}
	ff->archive = g_object_ref (archive);
	ff->password = g_strdup (password);

	return ff;
}

static gpointer
_fr_fuse_run_loop (gpointer data)
{
	struct fuse_session *fuse = data;

	fuse_session_loop (fuse);

	return NULL;
}

void
fr_fuse_mount (FrFuse *ff, GError **error)
{
	g_autofree char *mount_point = NULL;
	g_autofree char *work_path = NULL;

	g_return_if_fail (ff != NULL);

	if (ff->started)
		return;

	mount_point = g_file_get_path (ff->mount_dir);
	if (fuse_session_mount (ff->fuse, mount_point) < 0) {
		g_set_error (&ff->error, FR_FUSE_ERROR,
			     FR_FUSE_ERROR_FUSE_MOUNT,
			     "Failed to mount FUSE at %s.", mount_point);
		g_propagate_error(error, ff->error);
		return;
	}

	ff->loop_thread = g_thread_try_new ("fuse-session-loop",
					    _fr_fuse_run_loop, ff->fuse,
					    &ff->error);
	if (ff->error != NULL) {
		g_propagate_error(error, ff->error);
		return;
	}

	ff->inodes = g_ptr_array_new_with_free_func ((GDestroyNotify)fr_file_data_free);
	/* This hash table never own data. */
	ff->paths = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
	/*
	 * FUSE_ROOT_ID is 1, so we preserve inode 0 and 1, then create inodes
	 * for actual files in archive.
	 */
	_fr_fuse_create_inode (ff, NULL);
	_fr_fuse_create_inode (ff, NULL);

	// g_print ("total inodes: %u\n", ff->inodes->len);

	ff->started = TRUE;
	g_print ("Archive mounted as FUSE in %s\n", mount_point);
	work_path = g_file_get_path (ff->work_dir);
	g_print ("Archive will extract temporary files into %s\n", work_path);
}

void
fr_fuse_unmount (FrFuse *ff)
{
	g_return_if_fail (ff != NULL);

	if (ff->started) {
		ff->started = FALSE;

		fuse_session_unmount (ff->fuse);
		fuse_session_exit (ff->fuse);
		g_clear_pointer (&ff->inodes, g_ptr_array_unref);
		g_clear_pointer (&ff->paths, g_hash_table_unref);
	}
}

GFile *
fr_fuse_get_mount_dir (FrFuse *ff)
{
	g_return_val_if_fail (ff != NULL, NULL);

	if (!ff->started)
		return NULL;

	return ff->mount_dir;
}

/*
 * NOTE: This should be called after listing archive files, otherwise we have no
 * dir tree.
 */
void
fr_fuse_update_inodes (FrFuse *ff)
{
	g_return_if_fail (ff != NULL);
	g_return_if_fail (ff->started);

	for (unsigned int i = FR_FUSE_INODE_START; i < ff->inodes->len; ++i) {
		FrFileData *fdata = ff->inodes->pdata[i];
		if (fdata == NULL)
			continue;
		/* File has been removed from the archive. */
		if (!g_hash_table_contains (ff->archive->files_hash, fdata->original_path))
			_fr_fuse_delete_inode (ff, i);
	}

	for (unsigned int i = 0; i < ff->archive->files->len; ++i) {
		FrFileData *fdata = ff->archive->files->pdata[i];
		/* File has been added into the archive. */
		if (!g_hash_table_contains (ff->paths, fdata->full_path))
		    _fr_fuse_create_inode (ff, fdata);
	}
}

gboolean
fr_fuse_query_file_in_path (FrFuse *ff, GFile *file, const char *path)
{
	g_autoptr (GFile) test = NULL;
	g_autofree char *test_path = g_strdup (path);

	g_return_val_if_fail (ff != NULL, FALSE);
	g_return_val_if_fail (ff->started, FALSE);

	if (file == NULL || test_path == NULL)
		return FALSE;

	test = _g_file_append_path (ff->mount_dir, test_path, NULL);

	if (!g_file_query_exists (test, ff->cancellable))
		return FALSE;

	return g_file_has_parent (file, test);
}
