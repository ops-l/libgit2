/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"
#include "path.h"
#include "path_w32.h"
#include "utf-conv.h"
#include "posix.h"
#include "reparse.h"
#include "dir.h"

#define PATH__NT_NAMESPACE     L"\\\\?\\"
#define PATH__NT_NAMESPACE_LEN 4

#define PATH__ABSOLUTE_LEN     3

#define path__is_dirsep(p) ((p) == '/' || (p) == '\\')

#define path__is_absolute(p) \
	(git__isalpha((p)[0]) && (p)[1] == ':' && ((p)[2] == '\\' || (p)[2] == '/'))

#define path__is_nt_namespace(p) \
	(((p)[0] == '\\' && (p)[1] == '\\' && (p)[2] == '?' && (p)[3] == '\\') || \
	 ((p)[0] == '/' && (p)[1] == '/' && (p)[2] == '?' && (p)[3] == '/'))

#define path__is_unc(p) \
	(((p)[0] == '\\' && (p)[1] == '\\') || ((p)[0] == '/' && (p)[1] == '/'))

/* Using _FIND_FIRST_EX_LARGE_FETCH may increase performance in Windows 7
 * and better.  Prior versions will ignore this.
 */
#define _FIND_FIRST_EX_LARGE_FETCH 2

GIT_INLINE(int) path__cwd(wchar_t *path, int size)
{
	int len;

	if ((len = GetCurrentDirectoryW(size, path)) == 0) {
		errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : ENOENT;
		return -1;
	} else if (len > size) {
		errno = ENAMETOOLONG;
		return -1;
	}

	/* The Win32 APIs may return "\\?\" once you've used it first.
	 * But it may not.  What a gloriously predictible API!
	 */
	if (wcsncmp(path, PATH__NT_NAMESPACE, PATH__NT_NAMESPACE_LEN))
		return len;

	len -= PATH__NT_NAMESPACE_LEN;

	memmove(path, path + PATH__NT_NAMESPACE_LEN, sizeof(wchar_t) * len);
	return len;
}

static wchar_t *path__skip_server(wchar_t *path)
{
	wchar_t *c;

	for (c = path; *c; c++) {
		if (path__is_dirsep(*c))
			return c + 1;
	}

	return c;
}

static wchar_t *path__skip_prefix(wchar_t *path)
{
	if (path__is_nt_namespace(path)) {
		path += PATH__NT_NAMESPACE_LEN;

		if (wcsncmp(path, L"UNC\\", 4) == 0)
			path = path__skip_server(path + 4);
		else if (path__is_absolute(path))
			path += PATH__ABSOLUTE_LEN;
	} else if (path__is_absolute(path)) {
		path += PATH__ABSOLUTE_LEN;
	} else if (path__is_unc(path)) {
		path = path__skip_server(path + 2);
	}

	return path;
}

int git_win32_path_canonicalize(git_win32_path path)
{
	wchar_t *base, *from, *to, *next;
	size_t len;

	base = to = path__skip_prefix(path);

	/* Unposixify if the prefix */
	for (from = path; from < to; from++) {
		if (*from == L'/')
			*from = L'\\';
	}

	while (*from) {
		for (next = from; *next; ++next) {
			if (*next == L'/') {
				*next = L'\\';
				break;
			}

			if (*next == L'\\')
				break;
		}

		len = next - from;

		if (len == 1 && from[0] == L'.')
			/* do nothing with singleton dot */;

		else if (len == 2 && from[0] == L'.' && from[1] == L'.') {
			if (to == base) {
				/* no more path segments to strip, eat the "../" */
				if (*next == L'\\')
					len++;

				base = to;
			} else {
				/* back up a path segment */
				while (to > base && to[-1] == L'\\') to--;
				while (to > base && to[-1] != L'\\') to--;
			}
		} else {
			if (*next == L'\\' && *from != L'\\')
				len++;

			if (to != from)
				memmove(to, from, sizeof(wchar_t) * len);

			to += len;
		}

		from += len;

		while (*from == L'\\') from++;
	}

	/* Strip trailing backslashes */
	while (to > base && to[-1] == L'\\') to--;

	*to = L'\0';

	return (to - path);
}

int git_win32_path__cwd(wchar_t *out, size_t len)
{
	int cwd_len;

	if ((cwd_len = path__cwd(out, len)) < 0)
		return -1;

	/* UNC paths */
	if (wcsncmp(L"\\\\", out, 2) == 0) {
		/* Our buffer must be at least 5 characters larger than the
		 * current working directory:  we swallow one of the leading
		 * '\'s, but we we add a 'UNC' specifier to the path, plus
		 * a trailing directory separator, plus a NUL.
		 */
		if (cwd_len > MAX_PATH - 4) {
			errno = ENAMETOOLONG;
			return -1;
		}

		memmove(out+2, out, sizeof(wchar_t) * cwd_len);
		out[0] = L'U';
		out[1] = L'N';
		out[2] = L'C';

		cwd_len += 2;
	}

	/* Our buffer must be at least 2 characters larger than the current
	 * working directory.  (One character for the directory separator,
	 * one for the null.
	 */
	else if (cwd_len > MAX_PATH - 2) {
		errno = ENAMETOOLONG;
		return -1;
	}

	return cwd_len;
}

int git_win32_path_from_utf8(git_win32_path out, const char *src)
{
	wchar_t *dest = out;

	/* All win32 paths are in NT-prefixed format, beginning with "\\?\". */
	memcpy(dest, PATH__NT_NAMESPACE, sizeof(wchar_t) * PATH__NT_NAMESPACE_LEN);
	dest += PATH__NT_NAMESPACE_LEN;

	/* See if this is an absolute path (beginning with a drive letter) */
	if (path__is_absolute(src)) {
		if (git__utf8_to_16(dest, MAX_PATH, src) < 0)
			return -1;
	}
	/* File-prefixed NT-style paths beginning with \\?\ */
	else if (path__is_nt_namespace(src)) {
		/* Skip the NT prefix, the destination already contains it */
		if (git__utf8_to_16(dest, MAX_PATH, src + PATH__NT_NAMESPACE_LEN) < 0)
			return -1;
	}
	/* UNC paths */
	else if (path__is_unc(src)) {
		memcpy(dest, L"UNC\\", sizeof(wchar_t) * 4);
		dest += 4;

		/* Skip the leading "\\" */
		if (git__utf8_to_16(dest, MAX_PATH - 2, src + 2) < 0)
			return -1;
	}
	/* Absolute paths omitting the drive letter */
	else if (src[0] == '\\' || src[0] == '/') {
		if (path__cwd(dest, MAX_PATH) < 0)
			return -1;

		if (!path__is_absolute(dest)) {
			errno = ENOENT;
			return -1;
		}

		/* Skip the drive letter specification ("C:") */	
		if (git__utf8_to_16(dest + 2, MAX_PATH - 2, src) < 0)
			return -1;
	}
	/* Relative paths */
	else {
		int cwd_len;

		if ((cwd_len = git_win32_path__cwd(dest, MAX_PATH)) < 0)
			return -1;

		dest[cwd_len++] = L'\\';

		if (git__utf8_to_16(dest + cwd_len, MAX_PATH - cwd_len, src) < 0)
			return -1;
	}

	return git_win32_path_canonicalize(out);
}

int git_win32_path_to_utf8(git_win32_utf8_path dest, const wchar_t *src)
{
	char *out = dest;
	int len;

	/* Strip NT namespacing "\\?\" */
	if (path__is_nt_namespace(src)) {
		src += 4;

		/* "\\?\UNC\server\share" -> "\\server\share" */
		if (wcsncmp(src, L"UNC\\", 4) == 0) {
			src += 4;

			memcpy(dest, "\\\\", 2);
			out = dest + 2;
		}
	}

	if ((len = git__utf16_to_8(out, GIT_WIN_PATH_UTF8, src)) < 0)
		return len;

	git_path_mkposix(dest);

	return len;
}

char *git_win32_path_8dot3_name(const char *path)
{
	git_win32_path longpath, shortpath;
	wchar_t *start;
	char *shortname;
	int len, namelen = 1;

	if (git_win32_path_from_utf8(longpath, path) < 0)
		return NULL;

	len = GetShortPathNameW(longpath, shortpath, GIT_WIN_PATH_UTF16);

	while (len && shortpath[len-1] == L'\\')
		shortpath[--len] = L'\0';

	if (len == 0 || len >= GIT_WIN_PATH_UTF16)
		return NULL;

	for (start = shortpath + (len - 1);
		start > shortpath && *(start-1) != '/' && *(start-1) != '\\';
		start--)
		namelen++;

	/* We may not have actually been given a short name.  But if we have,
	 * it will be in the ASCII byte range, so we don't need to worry about
	 * multi-byte sequences and can allocate naively.
	 */
	if (namelen > 12 || (shortname = git__malloc(namelen + 1)) == NULL)
		return NULL;

	if ((len = git__utf16_to_8(shortname, namelen + 1, start)) < 0)
		return NULL;

	return shortname;
}

GIT_INLINE(int) path_with_stat_alloc(
	git_path_with_stat **out,
	const char *parent_path,
	size_t parent_path_len,
	const wchar_t *child_path_utf16,
	bool trailing_slash)
{
	git_path_with_stat *ps;
	int inner_slash =
		(parent_path_len > 0 && parent_path[parent_path_len-1] != '/');
	size_t path_len, child_path_len, ps_size;

	if ((child_path_len = git__utf16_to_8(NULL, 0, child_path_utf16)) < 0) {
		giterr_set(GITERR_OS, "Could not convert path to UTF-8 (path too long?)");
		return -1;
	}

	GITERR_CHECK_ALLOC_ADD(&path_len, parent_path_len, inner_slash);
	GITERR_CHECK_ALLOC_ADD(&path_len, path_len, child_path_len);
	GITERR_CHECK_ALLOC_ADD(&path_len, path_len, trailing_slash ? 1 : 0);

	GITERR_CHECK_ALLOC_ADD(&ps_size, sizeof(git_path_with_stat), path_len);
	GITERR_CHECK_ALLOC_ADD(&ps_size, ps_size, 1);

	ps = git__calloc(1, ps_size);
	GITERR_CHECK_ALLOC(ps);

	if (parent_path_len)
		memcpy(ps->path, parent_path, parent_path_len);

	if (inner_slash)
		ps->path[parent_path_len] = '/';

	if (git__utf16_to_8(
			&ps->path[parent_path_len + inner_slash],
			child_path_len + 1, child_path_utf16) != child_path_len) {
		git__free(ps);
		giterr_set(GITERR_OS, "Could not convert path to UTF-8 (size changed)");
		return -1;
	}

	if (trailing_slash)
		ps->path[path_len-1] = '/';

	ps->path_len = path_len;

	*out = ps;
	return 0;
}

GIT_INLINE(int) path_add_base(
	git_win32_path out,
	size_t dir_len,
	wchar_t *base,
	size_t base_len)
{
	size_t out_len;

	if (GIT_ADD_SIZET_OVERFLOW(&out_len, dir_len, base_len) < 0 ||
		GIT_ADD_SIZET_OVERFLOW(&out_len, out_len, 2) < 0)
		return -1;

	if (out_len > GIT_WIN_PATH_UTF16) {
		giterr_set(GITERR_FILESYSTEM, "invalid path '%.*ls\\%.*ls' (path too long)", dir_len, out, base_len, base);
		return -1;
	}

	out[dir_len] = '\\';
	memcpy(&out[dir_len+1], base, base_len * sizeof(wchar_t));
	out[out_len-1] = '\0';

	return 0;
}

#if !defined(__MINGW32__)
int git_win32_path_dirload_with_stat(
	const char *path,
	size_t prefix_len,
	unsigned int flags,
	const char *start_stat,
	const char *end_stat,
	git_vector *contents)
{
	int error = 0;
	git_path_with_stat *ps;
	git_win32_path path_filter, file_path;
	DIR dir = {0};
	int(*strncomp)(const char *a, const char *b, size_t sz);
	size_t start_len = start_stat ? strlen(start_stat) : 0;
	size_t end_len = end_stat ? strlen(end_stat) : 0;
	size_t path_len, suffix_len, cmp_len;
	const char *suffix;
	int root_len;

	if ((root_len = git_win32_path_from_utf8(file_path, path)) < 0 ||
			!git_win32__findfirstfile_filter(path_filter, path)) {
		giterr_set(GITERR_OS, "Could not parse the path '%s'", path);
		return -1;
	}

	strncomp = (flags & GIT_PATH_DIR_IGNORE_CASE) != 0 ?
		git__strncasecmp : git__strncmp;

	path_len = strlen(path);

	suffix = path + prefix_len;
	suffix_len = path_len - prefix_len;

	/* We use FIND_FIRST_EX_LARGE_FETCH here for a minor perf bump; this
	 * flag should be ignored on previous version of Windows.
	 */
	dir.h = FindFirstFileExW(
		path_filter,
		FindExInfoBasic,
		&dir.f,
		FindExSearchNameMatch,
		NULL,
		_FIND_FIRST_EX_LARGE_FETCH);

	if (dir.h == INVALID_HANDLE_VALUE) {
		error = -1;
		giterr_set(GITERR_OS, "Could not open directory '%s'", path);
		goto clean_up_and_exit;
	}
	
	do {
		if (git_path_is_dot_or_dotdotW(dir.f.cFileName))
			continue;

		if ((error = path_add_base(file_path, root_len, dir.f.cFileName, wcslen(dir.f.cFileName))) < 0 ||
			(error = path_with_stat_alloc(&ps,
				suffix, suffix_len, dir.f.cFileName,
				(dir.f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) < 0)
			goto clean_up_and_exit;

		git_vector_insert(contents, ps);

		/* skip stat if before start_stat or after end_stat */
		cmp_len = min(start_len, ps->path_len);
		if (cmp_len && strncomp(ps->path, start_stat, cmp_len) < 0)
			continue;

		cmp_len = min(end_len, ps->path_len);
		if (cmp_len && strncomp(ps->path, end_stat, cmp_len) > 0)
			continue;

		if ((error = git_win32__file_attribute_to_stat(&ps->st,
				(WIN32_FILE_ATTRIBUTE_DATA *)&dir.f, file_path)) < 0) {
			goto clean_up_and_exit;
		}
	} while (FindNextFileW(dir.h, &dir.f));

	if (GetLastError() != ERROR_NO_MORE_FILES) {
		error = -1;
		giterr_set(GITERR_OS, "Could not get attributes for file in '%s'", path);
		goto clean_up_and_exit;
	}

	/* sort now that directory suffix is added */
	git_vector_sort(contents);

clean_up_and_exit:
	if (dir.h != INVALID_HANDLE_VALUE)
		FindClose(dir.h);

	return error;
}
#endif

static bool path_is_volume(wchar_t *target, size_t target_len)
{
	return (target_len && wcsncmp(target, L"\\??\\Volume{", 11) == 0);
}

/* On success, returns the length, in characters, of the path stored in dest.
* On failure, returns a negative value. */
int git_win32_path_readlink_w(git_win32_path dest, const git_win32_path path)
{
	BYTE buf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
	GIT_REPARSE_DATA_BUFFER *reparse_buf = (GIT_REPARSE_DATA_BUFFER *)buf;
	HANDLE handle = NULL;
	DWORD ioctl_ret;
	wchar_t *target;
	size_t target_len;

	int error = -1;

	handle = CreateFileW(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		errno = ENOENT;
		return -1;
	}

	if (!DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0,
		reparse_buf, sizeof(buf), &ioctl_ret, NULL)) {
		errno = EINVAL;
		goto on_error;
	}

	switch (reparse_buf->ReparseTag) {
	case IO_REPARSE_TAG_SYMLINK:
		target = reparse_buf->SymbolicLinkReparseBuffer.PathBuffer +
			(reparse_buf->SymbolicLinkReparseBuffer.SubstituteNameOffset / sizeof(WCHAR));
		target_len = reparse_buf->SymbolicLinkReparseBuffer.SubstituteNameLength / sizeof(WCHAR);
	break;
	case IO_REPARSE_TAG_MOUNT_POINT:
		target = reparse_buf->MountPointReparseBuffer.PathBuffer +
			(reparse_buf->MountPointReparseBuffer.SubstituteNameOffset / sizeof(WCHAR));
		target_len = reparse_buf->MountPointReparseBuffer.SubstituteNameLength / sizeof(WCHAR);
	break;
	default:
		errno = EINVAL;
		goto on_error;
	}

	if (path_is_volume(target, target_len)) {
		/* This path is a reparse point that represents another volume mounted
		* at this location, it is not a symbolic link our input was canonical.
		*/
		errno = EINVAL;
		error = -1;
	} else if (target_len) {
		/* The path may need to have a prefix removed. */
		target_len = git_win32__canonicalize_path(target, target_len);

		/* Need one additional character in the target buffer
		* for the terminating NULL. */
		if (GIT_WIN_PATH_UTF16 > target_len) {
			wcscpy(dest, target);
			error = (int)target_len;
		}
	}

on_error:
	CloseHandle(handle);
	return error;
}
