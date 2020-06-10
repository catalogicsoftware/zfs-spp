/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011 Gunnar Beutner
 * Copyright (c) 2012 Cyril Plisko. All rights reserved.
 * Copyright (c) 2019 by Delphix. All rights reserved.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libgen.h>
#include <libzfs.h>
#include <libshare.h>
#include "libshare_impl.h"

#define	ZFS_EXPORTS_DIR			"/etc/exports.d"
#define	ZFS_EXPORTS_FILE		ZFS_EXPORTS_DIR"/zfs.exports"
#define	ZFS_EXPORTS_LOCK		ZFS_EXPORTS_FILE".lock"

static sa_fstype_t *nfs_fstype;

typedef int (*nfs_shareopt_callback_t)(const char *opt, const char *value,
    void *cookie);

typedef int (*nfs_host_callback_t)(const char *sharepath, const char *filename,
    const char *host, const char *security, const char *access, void *cookie);

static int nfs_lock_fd = -1;

/*
 * The nfs_exports_[lock|unlock] is used to guard against conconcurrent
 * updates to the exports file. Each protocol is responsible for
 * providing the necessary locking to ensure consistency.
 */
int
nfs_exports_lock(void)
{
	nfs_lock_fd = open(ZFS_EXPORTS_LOCK,
	    O_RDWR | O_CREAT, 0600);
	if (nfs_lock_fd == -1) {
		fprintf(stderr, "failed to lock %s: %s\n",
		    ZFS_EXPORTS_LOCK, strerror(errno));
		return (errno);
	}
	return (flock(nfs_lock_fd, LOCK_EX));
}

int
nfs_exports_unlock(void)
{
	if (nfs_lock_fd < 0)
		return (-1);

	int error = flock(nfs_lock_fd, LOCK_UN);
	close(nfs_lock_fd);
	nfs_lock_fd = -1;
	return (error);
}

/*
 * Invokes the specified callback function for each Solaris share option
 * listed in the specified string.
 */
static int
foreach_nfs_shareopt(const char *shareopts,
    nfs_shareopt_callback_t callback, void *cookie)
{
	char *shareopts_dup, *opt, *cur, *value;
	int was_nul, rc;

	if (shareopts == NULL)
		return (SA_OK);

	if (strcmp(shareopts, "on") == 0)
		shareopts = "rw,crossmnt";

	shareopts_dup = strdup(shareopts);

	if (shareopts_dup == NULL)
		return (SA_NO_MEMORY);

	opt = shareopts_dup;
	was_nul = 0;

	while (1) {
		cur = opt;

		while (*cur != ',' && *cur != '\0')
			cur++;

		if (*cur == '\0')
			was_nul = 1;

		*cur = '\0';

		if (cur > opt) {
			value = strchr(opt, '=');

			if (value != NULL) {
				*value = '\0';
				value++;
			}

			rc = callback(opt, value, cookie);

			if (rc != SA_OK) {
				free(shareopts_dup);
				return (rc);
			}
		}

		opt = cur + 1;

		if (was_nul)
			break;
	}

	free(shareopts_dup);

	return (SA_OK);
}

typedef struct nfs_host_cookie_s {
	nfs_host_callback_t callback;
	const char *sharepath;
	void *cookie;
	const char *filename;
	const char *security;
} nfs_host_cookie_t;

/*
 * Helper function for foreach_nfs_host. This function checks whether the
 * current share option is a host specification and invokes a callback
 * function with information about the host.
 */
static int
foreach_nfs_host_cb(const char *opt, const char *value, void *pcookie)
{
	int rc;
	const char *access;
	char *host_dup, *host, *next;
	nfs_host_cookie_t *udata = (nfs_host_cookie_t *)pcookie;

#ifdef DEBUG
	fprintf(stderr, "foreach_nfs_host_cb: key=%s, value=%s\n", opt, value);
#endif

	if (strcmp(opt, "sec") == 0)
		udata->security = value;

	if (strcmp(opt, "rw") == 0 || strcmp(opt, "ro") == 0) {
		if (value == NULL)
			value = "*";

		access = opt;

		host_dup = strdup(value);

		if (host_dup == NULL)
			return (SA_NO_MEMORY);

		host = host_dup;

		do {
			next = strchr(host, ':');
			if (next != NULL) {
				*next = '\0';
				next++;
			}

			rc = udata->callback(udata->filename,
			    udata->sharepath, host, udata->security,
			    access, udata->cookie);

			if (rc != SA_OK) {
				free(host_dup);

				return (rc);
			}

			host = next;
		} while (host != NULL);

		free(host_dup);
	}

	return (SA_OK);
}

/*
 * Invokes a callback function for all NFS hosts that are set for a share.
 */
static int
foreach_nfs_host(sa_share_impl_t impl_share, char *filename,
    nfs_host_callback_t callback, void *cookie)
{
	nfs_host_cookie_t udata;
	char *shareopts;

	udata.callback = callback;
	udata.sharepath = impl_share->sa_mountpoint;
	udata.cookie = cookie;
	udata.filename = filename;
	udata.security = "sys";

	shareopts = FSINFO(impl_share, nfs_fstype)->shareopts;

	return (foreach_nfs_shareopt(shareopts, foreach_nfs_host_cb,
	    &udata));
}

/*
 * Converts a Solaris NFS host specification to its Linux equivalent.
 */
static int
get_linux_hostspec(const char *solaris_hostspec, char **plinux_hostspec)
{
	/*
	 * For now we just support CIDR masks (e.g. @192.168.0.0/16) and host
	 * wildcards (e.g. *.example.org).
	 */
	if (solaris_hostspec[0] == '@') {
		/*
		 * Solaris host specifier, e.g. @192.168.0.0/16; we just need
		 * to skip the @ in this case
		 */
		*plinux_hostspec = strdup(solaris_hostspec + 1);
	} else {
		*plinux_hostspec = strdup(solaris_hostspec);
	}

	if (*plinux_hostspec == NULL) {
		return (SA_NO_MEMORY);
	}

	return (SA_OK);
}

static int exports_lock_fd = -1;

int
nfs_exports_lock(void)
{
	struct flock lock;
	char *name = strdup(ZFS_EXPORTS);
	if (name == NULL)
		return (SA_NO_MEMORY);

	verify(exports_lock_fd == -1);

	char *dname = dirname(name);
	if (mkdir(dname, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "failed to create %s: %s\n",
		    dname, strerror(errno));
		free(name);
		return (errno);
	}
	free(name);

	exports_lock_fd = open(ZFS_EXPORTS_LOCK, (O_RDWR | O_CREAT), 0600);
	if (exports_lock_fd < 0) {
		perror("failed to open exports lock");
		return (errno);
	}

	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 0;
	int rc = -1;
	if ((rc = fcntl(exports_lock_fd, F_SETLKW, &lock)) < 0) {
		perror("failed to lock exports");
		close(exports_lock_fd);
		return (errno);
	}

	if (access(ZFS_EXPORTS, F_OK) == 0) {
		if (remove(ZFS_EXPORTS) == -1) {
			perror("failed to remove ZFS_EXPORTS file");
			close(exports_lock_fd);
			return (errno);
		}
	}

	int fd = creat(ZFS_EXPORTS, 0644);
	if (fd == -1) {
		perror("failed to create ZFS_EXPORTS file");
		close(exports_lock_fd);
		return (errno);
	}
	close(fd);
	return (0);
}

int
nfs_exports_unlock(void)
{
	struct flock lock;
	int retval = -1;

	if (exports_lock_fd < 0)
		return (-1);
	lock.l_type = F_UNLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 0;
	if (fcntl(exports_lock_fd, F_SETLK, &lock) == 0)
		retval = 0;
	close(exports_lock_fd);
	exports_lock_fd = -1;
	return (retval);
}

/*
 * This function populates an entry into /etc/exports.d/zfs.exports.
 * This file is consumed by the linux nfs server so that zfs shares are
 * automatically exported upon boot or whenever the nfs server restarts.
 */
static int
nfs_exports_entry(const char *sharepath, const char *host,
    const char *security, const char *access_opts, void *pcookie)
{
	int rc;
	char *linuxhost;
	const char *linux_opts = (const char *)pcookie;

	verify(exports_lock_fd != -1);

	int fd = open(ZFS_EXPORTS, O_RDWR | O_APPEND, 0644);
	if (fd == -1) {
		perror("failed to open exports file");
		exit(1);
	}

	rc = get_linux_hostspec(host, &linuxhost);
	if (rc < 0) {
		close(fd);
		exit(1);
	}

	if (linux_opts == NULL)
		linux_opts = "";

	const char *fmt = "%s %s(sec=%s,%s,%s)\n";
	int bytes = snprintf(NULL, 0, fmt, sharepath,
	    linuxhost, security, access_opts, linux_opts);
	char *buffer = malloc(bytes + 1);
	if (buffer == NULL) {
		close(fd);
		free(linuxhost);
		exit(1);
	}

	sprintf(buffer, fmt, sharepath, linuxhost,
	    security, access_opts, linux_opts);
	free(linuxhost);

	if (write(fd, buffer, strlen(buffer)) == -1) {
		perror("failed to write exports file");
		close(fd);
		free(buffer);
		exit(1);
	}
	fsync(fd);
	close(fd);
	free(buffer);
	return (SA_OK);
}

/*
 * This function populates an entry into /etc/exports.d/zfs.exports.
 * This file is consumed by the linux nfs server so that zfs shares are
 * automatically exported upon boot or whenever the nfs server restarts.
 */
static int
nfs_add_entry(const char *filename, const char *sharepath,
    const char *host, const char *security, const char *access_opts,
    void *pcookie)
{
	int rc;
	char *linuxhost;
	const char *linux_opts = (const char *)pcookie;

	rc = get_linux_hostspec(host, &linuxhost);
	if (rc != SA_OK)
		return (rc);

	if (linux_opts == NULL)
		linux_opts = "";

	FILE *fp = fopen(filename, "a+");
	if (fp == NULL) {
		fprintf(stderr, "failed to open %s file: %s", filename,
		    strerror(errno));
		free(linuxhost);
		return (SA_SYSTEM_ERR);
	}

	if (fprintf(fp, "%s %s(sec=%s,%s,%s)\n", sharepath, linuxhost,
	    security, access_opts, linux_opts) < 0) {
		fprintf(stderr, "failed to write to %s\n", filename);
		free(linuxhost);
		fclose(fp);
		return (SA_SYSTEM_ERR);
	}

	free(linuxhost);
	if (fclose(fp) != 0) {
		fprintf(stderr, "Unable to close file %s: %s\n",
		    filename, strerror(errno));
		return (SA_SYSTEM_ERR);
	}
	return (SA_OK);
}

/*
 * Adds a Linux share option to an array of NFS options.
 */
static int
add_linux_shareopt(char **plinux_opts, const char *key, const char *value)
{
	size_t len = 0;
	char *new_linux_opts;

	if (*plinux_opts != NULL)
		len = strlen(*plinux_opts);

	new_linux_opts = realloc(*plinux_opts, len + 1 + strlen(key) +
	    (value ? 1 + strlen(value) : 0) + 1);

	if (new_linux_opts == NULL)
		return (SA_NO_MEMORY);

	new_linux_opts[len] = '\0';

	if (len > 0)
		strcat(new_linux_opts, ",");

	strcat(new_linux_opts, key);

	if (value != NULL) {
		strcat(new_linux_opts, "=");
		strcat(new_linux_opts, value);
	}

	*plinux_opts = new_linux_opts;

	return (SA_OK);
}

/*
 * Validates and converts a single Solaris share option to its Linux
 * equivalent.
 */
static int
get_linux_shareopts_cb(const char *key, const char *value, void *cookie)
{
	char **plinux_opts = (char **)cookie;

	/* host-specific options, these are taken care of elsewhere */
	if (strcmp(key, "ro") == 0 || strcmp(key, "rw") == 0 ||
	    strcmp(key, "sec") == 0)
		return (SA_OK);

	if (strcmp(key, "anon") == 0)
		key = "anonuid";

	if (strcmp(key, "root_mapping") == 0) {
		(void) add_linux_shareopt(plinux_opts, "root_squash", NULL);
		key = "anonuid";
	}

	if (strcmp(key, "nosub") == 0)
		key = "subtree_check";

	if (strcmp(key, "insecure") != 0 && strcmp(key, "secure") != 0 &&
	    strcmp(key, "async") != 0 && strcmp(key, "sync") != 0 &&
	    strcmp(key, "no_wdelay") != 0 && strcmp(key, "wdelay") != 0 &&
	    strcmp(key, "nohide") != 0 && strcmp(key, "hide") != 0 &&
	    strcmp(key, "crossmnt") != 0 &&
	    strcmp(key, "no_subtree_check") != 0 &&
	    strcmp(key, "subtree_check") != 0 &&
	    strcmp(key, "insecure_locks") != 0 &&
	    strcmp(key, "secure_locks") != 0 &&
	    strcmp(key, "no_auth_nlm") != 0 && strcmp(key, "auth_nlm") != 0 &&
	    strcmp(key, "no_acl") != 0 && strcmp(key, "mountpoint") != 0 &&
	    strcmp(key, "mp") != 0 && strcmp(key, "fsuid") != 0 &&
	    strcmp(key, "refer") != 0 && strcmp(key, "replicas") != 0 &&
	    strcmp(key, "root_squash") != 0 &&
	    strcmp(key, "no_root_squash") != 0 &&
	    strcmp(key, "all_squash") != 0 &&
	    strcmp(key, "no_all_squash") != 0 && strcmp(key, "fsid") != 0 &&
	    strcmp(key, "anonuid") != 0 && strcmp(key, "anongid") != 0) {
		return (SA_SYNTAX_ERR);
	}

	(void) add_linux_shareopt(plinux_opts, key, value);

	return (SA_OK);
}

/*
 * Takes a string containing Solaris share options (e.g. "sync,no_acl") and
 * converts them to a NULL-terminated array of Linux NFS options.
 */
static int
get_linux_shareopts(const char *shareopts, char **plinux_opts)
{
	int error;

	assert(plinux_opts != NULL);

	*plinux_opts = NULL;

	/* no_subtree_check - Default as of nfs-utils v1.1.0 */
	(void) add_linux_shareopt(plinux_opts, "no_subtree_check", NULL);

	/* mountpoint - Restrict exports to ZFS mountpoints */
	(void) add_linux_shareopt(plinux_opts, "mountpoint", NULL);

	error = foreach_nfs_shareopt(shareopts, get_linux_shareopts_cb,
	    plinux_opts);

	if (error != SA_OK) {
		free(*plinux_opts);
		*plinux_opts = NULL;
	}

	return (error);
}

static int
nfs_generate_share(sa_share_impl_t impl_share)
{
	char *shareopts, *linux_opts;
	int rc;

	shareopts = FSINFO(impl_share, nfs_fstype)->shareopts;
	if (shareopts == NULL)
		return (SA_OK);

	rc = get_linux_shareopts(shareopts, &linux_opts);
	if (rc != SA_OK)
		return (rc);

	rc = foreach_nfs_host(impl_share, nfs_exports_entry, linux_opts);

	free(linux_opts);
	return (rc);
}

/*
 * Enables NFS sharing for the specified share.
 */
static char *
nfs_init_tmpfile(void)
{
	char *tmpfile = NULL;

	if (asprintf(&tmpfile, "%s%s", ZFS_EXPORTS_FILE, ".XXXXXX") == -1) {
		fprintf(stderr, "Unable to allocate temporary file\n");
		return (NULL);
	}

	int fd = mkstemp(tmpfile);
	if (fd == -1) {
		fprintf(stderr, "Unable to create temporary file: %s",
		    strerror(errno));
		return (NULL);
	}
	close(fd);
	return (tmpfile);
}

static int
nfs_fini_tmpfile(char *tmpfile)
{
	if (rename(tmpfile, ZFS_EXPORTS_FILE) == -1) {
		fprintf(stderr, "Unable to rename %s: %s\n", tmpfile,
		    strerror(errno));
		unlink(tmpfile);	
		free(tmpfile);
		return (SA_SYSTEM_ERR);
	}
	free(tmpfile);
	return (SA_OK);
}

static int
nfs_copy_entries(char *filename, const char *mountpoint)
{
	char *buf = NULL;
	size_t buflen = MAXPATHLEN;
	int error = SA_OK;

	/*
         * If the file doesn't exist then there is nothing more
         * we need to do.
         */
	FILE *oldfp = fopen(ZFS_EXPORTS_FILE, "r");
	if (oldfp == NULL)
		return (SA_OK);

	FILE *newfp = fopen(filename, "w+");
	while ((getline(&buf, &buflen, oldfp)) != -1) {
		char *space = NULL;

		if ((space = strchr(buf, ' ')) != NULL) {
			int mountpoint_len = strlen(mountpoint);

			if (space - buf == mountpoint_len &&
			    strncmp(mountpoint, buf, mountpoint_len) == 0) {
				continue;
			}
		}
		fputs(buf, newfp);
	}

	if (oldfp != NULL && ferror(oldfp) != 0) {
		error = ferror(oldfp);
	}
	if (error == 0 && ferror(newfp) != 0) {
		error = ferror(newfp);
	}

	free(buf);
	if (fclose(newfp) != 0) {
		fprintf(stderr, "Unable to close file %s: %s\n",
		    filename, strerror(errno));
		error = error != 0 ? error : SA_SYSTEM_ERR;
	}
	fclose(oldfp);

	return (error);
}

static int
nfs_commit_shares(void)
{
	char *argv[] = {
	    "/usr/sbin/exportfs",
	    "-ra",
	    NULL
	};

	return (libzfs_run_process(argv[0], argv, 0));
}

/*
 * Enables NFS sharing for the specified share.
 */
static int
nfs_enable_share(sa_share_impl_t impl_share)
{
	char *shareopts, *linux_opts;
	int error;
	char *filename = NULL;

	if ((filename = nfs_init_tmpfile()) == NULL)
		return (SA_SYSTEM_ERR);

	VERIFY0(nfs_exports_lock());
	verify(nfs_copy_entries(filename, impl_share->sa_mountpoint) == SA_OK);

	shareopts = FSINFO(impl_share, nfs_fstype)->shareopts;
	error = get_linux_shareopts(shareopts, &linux_opts);
	if (error != SA_OK) {
		unlink(filename);
		free(filename);
		VERIFY0(nfs_exports_unlock());
		return (error);
	}

	error = foreach_nfs_host(impl_share, filename, nfs_add_entry,
	    linux_opts);
	free(linux_opts);
	if (error == 0) {
		error = nfs_fini_tmpfile(filename);
	} else {
		unlink(filename);
		free(filename);
	}
	VERIFY0(nfs_exports_unlock());
	return (error);
}

/*
 * Disables NFS sharing for the specified share.
 */
static int
nfs_disable_share(sa_share_impl_t impl_share)
{
	char *filename = NULL;

	if ((filename = nfs_init_tmpfile()) == NULL)
		return (SA_SYSTEM_ERR);

	VERIFY0(nfs_exports_lock());
	verify(nfs_copy_entries(filename, impl_share->sa_mountpoint) == SA_OK);
	int error = nfs_fini_tmpfile(filename);
	VERIFY0(nfs_exports_unlock());
	return (error);
}

/*
 * Checks whether the specified NFS share options are syntactically correct.
 */
static int
nfs_validate_shareopts(const char *shareopts)
{
	char *linux_opts;
	int error;

	error = get_linux_shareopts(shareopts, &linux_opts);

	if (error != SA_OK)
		return (error);

	free(linux_opts);
	return (SA_OK);
}

static boolean_t
nfs_is_shared(sa_share_impl_t impl_share)
{
	size_t buflen = 0;
	char *buf = NULL;

	FILE *fp = fopen(ZFS_EXPORTS_FILE, "r");
	if (fp == NULL) {
		return (B_FALSE);
	}
	while ((getline(&buf, &buflen, fp)) != -1) {
		char *space = NULL;

		if ((space = strchr(buf, ' ')) != NULL) {
			int mountpoint_len = strlen(impl_share->sa_mountpoint);

			if (space - buf == mountpoint_len &&
			    strncmp(impl_share->sa_mountpoint, buf,
			    mountpoint_len) == 0) {
				fclose(fp);
				free(buf);
				return (B_TRUE);
			}
		}
	}
	free(buf);
	fclose(fp);
	return (B_FALSE);
}

static int
nfs_update_shareopts(sa_share_impl_t impl_share, const char *shareopts)
{
	FSINFO(impl_share, nfs_fstype)->shareopts = (char *)shareopts;

	return (SA_OK);
}

/*
 * Clears a share's NFS options. Used by libshare to
 * clean up shares that are about to be free()'d.
 */
static void
nfs_clear_shareopts(sa_share_impl_t impl_share)
{
	FSINFO(impl_share, nfs_fstype)->shareopts = NULL;
}

static const sa_share_ops_t nfs_shareops = {
	.enable_share = nfs_enable_share,
	.disable_share = nfs_disable_share,
	.is_shared = nfs_is_shared,

	.validate_shareopts = nfs_validate_shareopts,
	.update_shareopts = nfs_update_shareopts,
	.generate_share = nfs_generate_share,
	.clear_shareopts = nfs_clear_shareopts,
	.commit_shares = nfs_commit_shares,
};

/*
 * Initializes the NFS functionality of libshare.
 */
void
libshare_nfs_init(void)
{
	struct stat sb;

	nfs_fstype = register_fstype("nfs", &nfs_shareops);

	if (stat(ZFS_EXPORTS_DIR, &sb) < 0 &&
	    mkdir(ZFS_EXPORTS_DIR, 0755) < 0) {
		fprintf(stderr, "failed to create %s: %s\n",
		    ZFS_EXPORTS_DIR, strerror(errno));
	}
}
