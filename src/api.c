/*
 * Copyright IBM Corporation. 2007
 *
 * Author:	Dhaval Giani <dhaval@linux.vnet.ibm.com>
 * Author:	Balbir Singh <balbir@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2.1 of the GNU Lesser General Public License
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * TODOs:
 *	1. Convert comments to Docbook style.
 *	2. Add more APIs for the control groups.
 *	3. Handle the configuration related APIs.
 *	4. Error handling.
 *
 * Code initiated and designed by Dhaval Giani. All faults are most likely
 * his mistake.
 *
 * Bharata B Rao <bharata@linux.vnet.ibm.com> is willing is take blame
 * for mistakes in APIs for reading statistics.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <errno.h>
#include <libcgroup.h>
#include <libcgroup-internal.h>
#include <mntent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fts.h>
#include <ctype.h>
#include <pwd.h>
#include <libgen.h>
#include <assert.h>

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION 0.01
#endif

#define VERSION(ver)	#ver

/*
 * The errno which happend the last time (have to be thread specific)
 */
__thread int last_errno;

#define MAXLEN 256

/* the value have to be thread specific */
__thread char errtext[MAXLEN];

/*
 * Remember to bump this up for major API changes.
 */
const static char cg_version[] = VERSION(PACKAGE_VERSION);

struct cg_mount_table_s cg_mount_table[CG_CONTROLLER_MAX];
static pthread_rwlock_t cg_mount_table_lock = PTHREAD_RWLOCK_INITIALIZER;

/* Check if cgroup_init has been called or not. */
static int cgroup_initialized;

/* Check if the rules cache has been loaded or not. */
static bool cgroup_rules_loaded;

/* List of configuration rules */
static struct cgroup_rule_list rl;

/* Temporary list of configuration rules (for non-cache apps) */
static struct cgroup_rule_list trl;

/* Lock for the list of rules (rl) */
static pthread_rwlock_t rl_lock = PTHREAD_RWLOCK_INITIALIZER;

char *cgroup_strerror_codes[] = {
	"Cgroup is not compiled in",
	"Cgroup is not mounted",
	"Cgroup does not exist",
	"Cgroup has not been created",
	"Cgroup one of the needed subsystems is not mounted",
	"Cgroup, request came in from non owner",
	"Cgroup controllers controllers are bound to different mount points",
	"Cgroup, operation not allowed",
	"Cgroup value set exceeds maximum",
	"Cgroup controller already exists",
	"Cgroup value already exists",
	"Cgroup invalid operation",
	"Cgroup, creation of controller failed",
	"Cgroup operation failed",
	"Cgroup not initialized",
	"Cgroup trying to set value for control that does not exist",
	"Cgroup generic error",
	"Cgroup values are not equal",
	"Cgroup controllers are different",
	"Cgroup parsing failed",
	"Cgroup, rules file does not exist",
	"Cgroup mounting failed",
	"The config file can not be opend",
	"End of File or iterator",
};

static int cg_chown_file(FTS *fts, FTSENT *ent, uid_t owner, gid_t group)
{
	int ret = 0;
	const char *filename = fts->fts_path;
	cgroup_dbg("seeing file %s\n", filename);
	switch (ent->fts_info) {
	case FTS_ERR:
		errno = ent->fts_errno;
		break;
	case FTS_D:
	case FTS_DC:
	case FTS_NSOK:
	case FTS_NS:
	case FTS_DNR:
	case FTS_DP:
		ret = chown(filename, owner, group);
		if (ret)
			goto fail_chown;
		ret = chmod(filename, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP |
					S_IWGRP | S_IXGRP | S_IROTH | S_IXOTH);
		break;
	case FTS_F:
	case FTS_DEFAULT:
		ret = chown(filename, owner, group);
		if (ret)
			goto fail_chown;
		ret = chmod(filename, S_IRUSR | S_IWUSR |  S_IRGRP |
						S_IWGRP | S_IROTH);
		break;
	}
fail_chown:
	if (ret < 0) {
		last_errno = errno;
		ret = ECGOTHER;
	}
	return ret;
}

/*
 * TODO: Need to decide a better place to put this function.
 */
static int cg_chown_recursive(char **path, uid_t owner, gid_t group)
{
	int ret = 0;
	cgroup_dbg("path is %s\n", *path);
	FTS *fts = fts_open(path, FTS_PHYSICAL | FTS_NOCHDIR |
				FTS_NOSTAT, NULL);
	while (1) {
		FTSENT *ent;
		ent = fts_read(fts);
		if (!ent) {
			cgroup_dbg("fts_read failed\n");
			break;
		}
		ret = cg_chown_file(fts, ent, owner, group);
	}
	fts_close(fts);
	return ret;
}

static int cgroup_test_subsys_mounted(const char *name)
{
	int i;

	pthread_rwlock_rdlock(&cg_mount_table_lock);

	for (i = 0; cg_mount_table[i].name[0] != '\0'; i++) {
		if (strncmp(cg_mount_table[i].name, name,
				sizeof(cg_mount_table[i].name)) == 0) {
			pthread_rwlock_unlock(&cg_mount_table_lock);
			return 1;
		}
	}
	pthread_rwlock_unlock(&cg_mount_table_lock);
	return 0;
}

/**
 * Free a single cgroup_rule struct.
 * 	@param r The rule to free from memory
 */
static void cgroup_free_rule(struct cgroup_rule *r)
{
	/* Loop variable */
	int i = 0;

	/* Make sure our rule is not NULL, first. */
	if (!r) {
		cgroup_dbg("Warning: Attempted to free NULL rule.\n");
		return;
	}

	/* We must free any used controller strings, too. */
	for(i = 0; i < MAX_MNT_ELEMENTS; i++) {
		if (r->controllers[i])
			free(r->controllers[i]);
	}

	free(r);
}

/**
 * Free a list of cgroup_rule structs.  If rl is the main list of rules,
 * the lock must be taken for writing before calling this function!
 * 	@param rl Pointer to the list of rules to free from memory
 */
static void cgroup_free_rule_list(struct cgroup_rule_list *rl)
{
	/* Temporary pointer */
	struct cgroup_rule *tmp = NULL;

	/* Make sure we're not freeing NULL memory! */
	if (!(rl->head)) {
		cgroup_dbg("Warning: Attempted to free NULL list.\n");
		return;
	}

	while (rl->head) {
		tmp = rl->head;
		rl->head = tmp->next;
		cgroup_free_rule(tmp);
	}

	/* Don't leave wild pointers around! */
	rl->head = NULL;
	rl->tail = NULL;
}

/**
 * Parse the configuration file that maps UID/GIDs to cgroups.  If ever the
 * configuration file is modified, applications should call this function to
 * load the new configuration rules.  The function caller is responsible for
 * calling free() on each rule in the list.
 *
 * The cache parameter alters the behavior of this function.  If true, this
 * function will read the entire configuration file and store the results in
 * rl (global rules list).  If false, this function will only parse until it
 * finds a rule matching the given UID or GID.  It will store this rule in rl,
 * as well as any children rules (rules that begin with a %) that it has.
 *
 * This function is NOT thread safe!
 * 	@param cache True to cache rules, else false
 * 	@param muid If cache is false, the UID to match against
 * 	@param mgid If cache is false, the GID to match against
 * 	@return 0 on success, -1 if no cache and match found, > 0 on error.
 * TODO: Make this function thread safe!
 */
static int cgroup_parse_rules(bool cache, uid_t muid, gid_t mgid)
{
	/* File descriptor for the configuration file */
	FILE *fp = NULL;

	/* Buffer to store the line we're working on */
	char *buff = NULL;

	/* Iterator for the line we're working on */
	char *itr = NULL;

	/* Pointer to the list that we're using */
	struct cgroup_rule_list *lst = NULL;

	/* Rule to add to the list */
	struct cgroup_rule *newrule = NULL;

	/* Structure to get GID from group name */
	struct group *grp = NULL;

	/* Structure to get UID from user name */
	struct passwd *pwd = NULL;

	/* Temporary storage for a configuration rule */
	char user[LOGIN_NAME_MAX] = { '\0' };
	char controllers[CG_CONTROLLER_MAX] = { '\0' };
	char destination[FILENAME_MAX] = { '\0' };
	uid_t uid = CGRULE_INVALID;
	gid_t gid = CGRULE_INVALID;

	/* The current line number */
	unsigned int linenum = 0;

	/* Did we skip the previous line? */
	bool skipped = false;

	/* Have we found a matching rule (non-cache mode)? */
	bool matched = false;

	/* Return codes */
	int ret = 0;

	/* Temporary buffer for strtok() */
	char *stok_buff = NULL;

	/* Loop variable. */
	int i = 0;

	/* Open the configuration file. */
	pthread_rwlock_wrlock(&rl_lock);
	fp = fopen(CGRULES_CONF_FILE, "r");
	if (!fp) {
		cgroup_dbg("Failed to open configuration file %s with"
				" error: %s\n", CGRULES_CONF_FILE,
				strerror(errno));
		last_errno = errno;
		ret = ECGOTHER;
		goto unlock;
	}

	buff = calloc(CGROUP_RULE_MAXLINE, sizeof(char));
	if (!buff) {
		cgroup_dbg("Out of memory?  Error: %s\n", strerror(errno));
		last_errno = errno;
		ret = ECGOTHER;
		goto close;
	}

	/* Determine which list we're using. */
	if (cache)
		lst = &rl;
	else
		lst = &trl;

	/* If our list already exists, clean it. */
	if (lst->head)
		cgroup_free_rule_list(lst);

	/* Now, parse the configuration file one line at a time. */
	cgroup_dbg("Parsing configuration file.\n");
	while ((itr = fgets(buff, CGROUP_RULE_MAXLINE, fp)) != NULL) {
		linenum++;

		/* We ignore anything after a # sign as comments. */
		if ((itr = strchr(buff, '#')))
			*itr = '\0';

		/* We also need to remove the newline character. */
		if ((itr = strchr(buff, '\n')))
			*itr = '\0';

		/* Now, skip any leading tabs and spaces. */
		itr = buff;
		while (itr && isblank(*itr))
			itr++;

		/* If there's nothing left, we can ignore this line. */
		if (!strlen(itr))
			continue;

		/*
		 * If we skipped the last rule and this rule is a continuation
		 * of it (begins with %), then we should skip this rule too.
		 */
		if (skipped && *itr == '%') {
			cgroup_dbg("Warning: Skipped child of invalid rule,"
					" line %d.\n", linenum);
			memset(buff, '\0', CGROUP_RULE_MAXLINE);
			continue;
		}

		/*
		 * If there is something left, it should be a rule.  Otherwise,
		 * there's an error in the configuration file.
		 */
		skipped = false;
		memset(user, '\0', LOGIN_NAME_MAX);
		memset(controllers, '\0', CG_CONTROLLER_MAX);
		memset(destination, '\0', FILENAME_MAX);
		i = sscanf(itr, "%s%s%s", user, controllers, destination);
		if (i != 3) {
			cgroup_dbg("Failed to parse configuration file on"
					" line %d.\n", linenum);
			goto parsefail;
		}

		/*
		 * Next, check the user/group.  If it's a % sign, then we
		 * are continuing another rule and UID/GID should not be
		 * reset.  If it's a @, we're dealing with a GID rule.  If
		 * it's a *, then we do not need to do a lookup because the
		 * rule always applies (it's a wildcard).  If we're using
		 * non-cache mode and we've found a matching rule, we only
		 * continue to parse if we're looking at a child rule.
		 */
		if ((!cache) && matched && (strncmp(user, "%", 1) != 0)) {
			/* If we make it here, we finished (non-cache). */
			cgroup_dbg("Parsing of configuration file"
				" complete.\n\n");
			ret = -1;
			goto cleanup;
		}
		if (strncmp(user, "@", 1) == 0) {
			/* New GID rule. */
			itr = &(user[1]);
			if ((grp = getgrnam(itr))) {
				uid = CGRULE_INVALID;
				gid = grp->gr_gid;
			} else {
				cgroup_dbg("Warning: Entry for %s not"
						"found.  Skipping rule on line"
						" %d.\n", itr, linenum);
				memset(buff, '\0', CGROUP_RULE_MAXLINE);
				skipped = true;
				continue;
			}
		} else if (strncmp(user, "*", 1) == 0) {
			/* Special wildcard rule. */
			uid = CGRULE_WILD;
			gid = CGRULE_WILD;
		} else if (*itr != '%') {
			/* New UID rule. */
			if ((pwd = getpwnam(user))) {
				uid = pwd->pw_uid;
				gid = CGRULE_INVALID;
			} else {
				cgroup_dbg("Warning: Entry for %s not"
						"found.  Skipping rule on line"
						" %d.\n", user, linenum);
				memset(buff, '\0', CGROUP_RULE_MAXLINE);
				skipped = true;
				continue;
			}
		} /* Else, we're continuing another rule (UID/GID are okay). */

		/*
		 * If we are not caching rules, then we need to check for a
		 * match before doing anything else.  We consider four cases:
		 * The UID matches, the GID matches, the UID is a member of the
		 * GID, or we're looking at the wildcard rule, which always
		 * matches.  If none of these are true, we simply continue to
		 * the next line in the file.
		 */
		if (grp && muid != CGRULE_INVALID) {
			pwd = getpwuid(muid);
			for (i = 0; grp->gr_mem[i]; i++) {
				if (!(strcmp(pwd->pw_name, grp->gr_mem[i])))
					matched = true;
			}
		}

		if (uid == muid || gid == mgid || uid == CGRULE_WILD) {
			matched = true;
		}

		if (!cache && !matched)
			continue;

		/*
		 * Now, we're either caching rules or we found a match.  Either
		 * way, copy everything into a new rule and push it into the
		 * list.
		 */
		newrule = calloc(1, sizeof(struct cgroup_rule));
		if (!newrule) {
			cgroup_dbg("Out of memory?  Error: %s\n",
				strerror(errno));
			last_errno = errno;
			ret = ECGOTHER;
			goto cleanup;
		}

		newrule->uid = uid;
		newrule->gid = gid;
		strncpy(newrule->name, user, sizeof(newrule->name) - 1);
		strncpy(newrule->destination, destination,
			sizeof(newrule->destination) - 1);
		newrule->next = NULL;

		/* Parse the controller list, and add that to newrule too. */
		stok_buff = strtok(controllers, ",");
		if (!stok_buff) {
			cgroup_dbg("Failed to parse controllers on line"
					" %d\n", linenum);
			goto destroyrule;
		}

		i = 0;
		do {
			if (i >= MAX_MNT_ELEMENTS) {
				cgroup_dbg("Too many controllers listed"
					" on line %d\n", linenum);
				goto destroyrule;
			}

			newrule->controllers[i] = strndup(stok_buff,
							strlen(stok_buff) + 1);
			if (!(newrule->controllers[i])) {
				cgroup_dbg("Out of memory?  Error was: %s\n",
					strerror(errno));
				goto destroyrule;
			}
			i++;
		} while ((stok_buff = strtok(NULL, ",")));

		/* Now, push the rule. */
		if (lst->head == NULL) {
			lst->head = newrule;
			lst->tail = newrule;
		} else {
			lst->tail->next = newrule;
			lst->tail = newrule;
		}

		cgroup_dbg("Added rule %s (UID: %d, GID: %d) -> %s for"
			" controllers:", lst->tail->name, lst->tail->uid,
			lst->tail->gid, lst->tail->destination);
                for (i = 0; lst->tail->controllers[i]; i++) {
			cgroup_dbg(" %s", lst->tail->controllers[i]);
		}
		cgroup_dbg("\n");

		/* Finally, clear the buffer. */
		memset(buff, '\0', CGROUP_RULE_MAXLINE);
		grp = NULL;
		pwd = NULL;
	}

	/* If we make it here, there were no errors. */
	cgroup_dbg("Parsing of configuration file complete.\n\n");
	ret = (matched && !cache) ? -1 : 0;
	goto cleanup;

destroyrule:
	cgroup_free_rule(newrule);

parsefail:
	ret = ECGROUPPARSEFAIL;

cleanup:
	free(buff);

close:
	fclose(fp);
unlock:
	pthread_rwlock_unlock(&rl_lock);
	return ret;
}

/**
 * cgroup_init(), initializes the MOUNT_POINT.
 *
 * This code is theoretically thread safe now. Its not really tested
 * so it can blow up. If does for you, please let us know with your
 * test case and we can really make it thread safe.
 *
 */
int cgroup_init()
{
	FILE *proc_mount = NULL;
	struct mntent *ent = NULL;
	struct mntent *temp_ent = NULL;
	int found_mnt = 0;
	int ret = 0;
	static char *controllers[CG_CONTROLLER_MAX];
	FILE *proc_cgroup = NULL;
	char subsys_name[FILENAME_MAX];
	int hierarchy, num_cgroups, enabled;
	int i=0;
	char *mntopt = NULL;
	int err;
	char *buf = NULL;
	char mntent_buffer[4 * FILENAME_MAX];
	char *strtok_buffer = NULL;

	pthread_rwlock_wrlock(&cg_mount_table_lock);

	proc_cgroup = fopen("/proc/cgroups", "r");

	if (!proc_cgroup) {
		last_errno = errno;
		ret = ECGOTHER;
		goto unlock_exit;
	}

	/*
	 * The first line of the file has stuff we are not interested in.
	 * So just read it and discard the information.
	 *
	 * XX: fix the size for fgets
	 */
	buf = malloc(FILENAME_MAX);
	if (!buf) {
		last_errno = errno;
		ret = ECGOTHER;
		goto unlock_exit;
	}
	if (!fgets(buf, FILENAME_MAX, proc_cgroup)) {
		free(buf);
		last_errno = errno;
		ret = ECGOTHER;
		goto unlock_exit;
	}
	free(buf);

	while (!feof(proc_cgroup)) {
		err = fscanf(proc_cgroup, "%s %d %d %d", subsys_name,
				&hierarchy, &num_cgroups, &enabled);
		if (err < 0)
			break;
		controllers[i] = strdup(subsys_name);
		i++;
	}
	controllers[i] = NULL;

	proc_mount = fopen("/proc/mounts", "r");
	if (proc_mount == NULL) {
		ret = ECGFAIL;
		goto unlock_exit;
	}

	temp_ent = (struct mntent *) malloc(sizeof(struct mntent));

	if (!temp_ent) {
		last_errno = errno; 
		ret = ECGOTHER;
		goto unlock_exit;
	}

	while ((ent = getmntent_r(proc_mount, temp_ent,
					mntent_buffer,
					sizeof(mntent_buffer))) != NULL) {
		if (strcmp(ent->mnt_type, "cgroup"))
			continue;

		for (i = 0; controllers[i] != NULL; i++) {
			mntopt = hasmntopt(ent, controllers[i]);

			if (!mntopt)
				continue;

			mntopt = strtok_r(mntopt, ",", &strtok_buffer);

			if (strcmp(mntopt, controllers[i]))
				continue;

			cgroup_dbg("matched %s:%s\n", mntopt, controllers[i]);
			strcpy(cg_mount_table[found_mnt].name, controllers[i]);
			strcpy(cg_mount_table[found_mnt].path, ent->mnt_dir);
			cgroup_dbg("Found cgroup option %s, count %d\n",
				ent->mnt_opts, found_mnt);
			found_mnt++;
		}
	}

	free(temp_ent);

	if (!found_mnt) {
		cg_mount_table[0].name[0] = '\0';
		ret = ECGROUPNOTMOUNTED;
		goto unlock_exit;
	}

	found_mnt++;
	cg_mount_table[found_mnt].name[0] = '\0';

	cgroup_initialized = 1;

unlock_exit:
	if (proc_cgroup)
		fclose(proc_cgroup);

	if (proc_mount)
		fclose(proc_mount);

	for (i = 0; controllers[i]; i++) {
		free(controllers[i]);
		controllers[i] = NULL;
	}

	pthread_rwlock_unlock(&cg_mount_table_lock);

	return ret;
}

static int cg_test_mounted_fs()
{
	FILE *proc_mount = NULL;
	struct mntent *ent = NULL;
	struct mntent *temp_ent = NULL;
	char mntent_buff[4 * FILENAME_MAX];
	int ret = 1;

	proc_mount = fopen("/proc/mounts", "r");
	if (proc_mount == NULL) {
		return 0;
	}

	temp_ent = (struct mntent *) malloc(sizeof(struct mntent));
	if (!temp_ent) {
		/* We just fail at the moment. */
		fclose(proc_mount);
		return 0;
	}

	ent = getmntent_r(proc_mount, temp_ent, mntent_buff,
						sizeof(mntent_buff));

	if (!ent) {
		ret = 0;
		goto done;
	}

	while (strcmp(ent->mnt_type, "cgroup") !=0) {
		ent = getmntent_r(proc_mount, temp_ent, mntent_buff,
						sizeof(mntent_buff));
		if (ent == NULL) {
			ret = 0;
			goto done;
		}
	}
done:
	fclose(proc_mount);
	free(temp_ent);
	return ret;
}

static inline pid_t cg_gettid()
{
	return syscall(__NR_gettid);
}


/* Call with cg_mount_table_lock taken */
static char *cg_build_path_locked(char *name, char *path, char *type)
{
	int i;
	for (i = 0; cg_mount_table[i].name[0] != '\0'; i++) {
		/*
		 * XX: Change to snprintf once you figure what n should be
		 */
		if (strcmp(cg_mount_table[i].name, type) == 0) {
			sprintf(path, "%s/", cg_mount_table[i].path);
			if (name) {
				char *tmp;
				tmp = strdup(path);
				sprintf(path, "%s%s/", tmp, name);
				free(tmp);
			}
			return path;
		}
	}
	return NULL;
}

char *cg_build_path(char *name, char *path, char *type)
{
	pthread_rwlock_rdlock(&cg_mount_table_lock);
	path = cg_build_path_locked(name, path, type);
	pthread_rwlock_unlock(&cg_mount_table_lock);

	return path;
}

static int __cgroup_attach_task_pid(char *path, pid_t tid)
{
	int ret = 0;
	FILE *tasks = NULL;

	tasks = fopen(path, "w");
	if (!tasks) {
		switch (errno) {
		case EPERM:
			return ECGROUPNOTOWNER;
		case ENOENT:
			return ECGROUPNOTEXIST;
		default:
			return ECGROUPNOTALLOWED;
		}
	}
	ret = fprintf(tasks, "%d", tid);
	if (ret < 0) {
		last_errno = errno;
		ret = ECGOTHER;
		goto err;
	}
	ret = fflush(tasks);
	if (ret) {
		last_errno = errno;
		ret = ECGOTHER;
		goto err;
	}
	fclose(tasks);
	return 0;
err:
	cgroup_dbg("Error writing tid %d to %s:%s\n",
			tid, path, strerror(errno));
	fclose(tasks);
	return ret;
}

/** cgroup_attach_task_pid is used to assign tasks to a cgroup.
 *  struct cgroup *cgroup: The cgroup to assign the thread to.
 *  pid_t tid: The thread to be assigned to the cgroup.
 *
 *  returns 0 on success.
 *  returns ECGROUPNOTOWNER if the caller does not have access to the cgroup.
 *  returns ECGROUPNOTALLOWED for other causes of failure.
 */
int cgroup_attach_task_pid(struct cgroup *cgroup, pid_t tid)
{
	char path[FILENAME_MAX];
	int i, ret = 0;

	if (!cgroup_initialized) {
		cgroup_dbg("libcgroup is not initialized\n");
		return ECGROUPNOTINITIALIZED;
	}
	if(!cgroup)
	{
		pthread_rwlock_rdlock(&cg_mount_table_lock);
		for(i = 0; i < CG_CONTROLLER_MAX &&
				cg_mount_table[i].name[0]!='\0'; i++) {
			if (!cg_build_path_locked(NULL, path,
						cg_mount_table[i].name))
				continue;
			strncat(path, "/tasks", sizeof(path) - strlen(path));
			ret = __cgroup_attach_task_pid(path, tid);
			if (ret) {
				pthread_rwlock_unlock(&cg_mount_table_lock);
				return ret;
			}
		}
		pthread_rwlock_unlock(&cg_mount_table_lock);
	} else {
		for (i = 0; i < cgroup->index; i++) {
			if (!cgroup_test_subsys_mounted(cgroup->controller[i]->name)) {
				cgroup_dbg("subsystem %s is not mounted\n",
					cgroup->controller[i]->name);
				return ECGROUPSUBSYSNOTMOUNTED;
			}
		}

		for (i = 0; i < cgroup->index; i++) {
			if (!cg_build_path(cgroup->name, path,
					cgroup->controller[i]->name))
				continue;
			strncat(path, "/tasks", sizeof(path) - strlen(path));
			ret = __cgroup_attach_task_pid(path, tid);
			if (ret)
				return ret;
		}
	}
	return 0;
}

/** cgroup_attach_task is used to attach the current thread to a cgroup.
 *  struct cgroup *cgroup: The cgroup to assign the current thread to.
 *
 *  See cg_attach_task_pid for return values.
 */
int cgroup_attach_task(struct cgroup *cgroup)
{
	pid_t tid = cg_gettid();
	int error;

	error = cgroup_attach_task_pid(cgroup, tid);

	return error;
}

/**
 * cg_mkdir_p, emulate the mkdir -p command (recursively creating paths)
 * @path: path to create
 */
static int cg_mkdir_p(const char *path)
{
	char *real_path = NULL;
	char *wd = NULL;
	int i = 0, j = 0;
	char pos;
	char *str = NULL;
	int ret = 0;
	char cwd[FILENAME_MAX];
	char *buf = NULL;

	buf = getcwd(cwd, FILENAME_MAX);

	if (!buf) {
		last_errno = errno;
		return ECGOTHER;
	}

	real_path = strdup(path);
	if (!real_path) {
		last_errno = errno;
		return ECGOTHER;
	}

	do {
		while (real_path[j] != '\0' && real_path[j] != '/')
			j++;
		while (real_path[j] != '\0' && real_path[j] == '/')
			j++;
		if (i == j)
			continue;
		pos = real_path[j];
		real_path[j] = '\0';		/* Temporarily overwrite "/" */
		str = &real_path[i];
		ret = mkdir(str, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
		wd = strdup(str);
		if (!wd) {
			last_errno = errno;
			ret = ECGOTHER;
			break;
		}
		real_path[j] = pos;
		if (ret) {
			switch (errno) {
			case EEXIST:
				ret = 0;	/* Not fatal really */
				break;
			case EPERM:
				ret = ECGROUPNOTOWNER;
				free(wd);
				goto done;
			default:
				ret = ECGROUPNOTALLOWED;
				free(wd);
				goto done;
			}
		}
		i = j;
		ret = chdir(wd);
		if (ret) {
			cgroup_dbg("could not chdir to child directory (%s)\n",
				wd);
			break;
		}
		free(wd);
	} while (real_path[i]);

	ret = chdir(buf);
	if (ret) {
		last_errno = errno;
		ret = ECGOTHER;
		cgroup_dbg("could not go back to old directory (%s)\n", cwd);
	}

done:
	free(real_path);
	return ret;
}

/*
 * create_control_group()
 * This is the basic function used to create the control group. This function
 * just makes the group. It does not set any permissions, or any control values.
 * The argument path is the fully qualified path name to make it generic.
 */
static int cg_create_control_group(char *path)
{
	int error;
	if (!cg_test_mounted_fs())
		return ECGROUPNOTMOUNTED;
	error = cg_mkdir_p(path);
	return error;
}

/*
 * set_control_value()
 * This is the low level function for putting in a value in a control file.
 * This function takes in the complete path and sets the value in val in that
 * file.
 */
static int cg_set_control_value(char *path, char *val)
{
	FILE *control_file = NULL;
	if (!cg_test_mounted_fs())
		return ECGROUPNOTMOUNTED;

	control_file = fopen(path, "r+");

	if (!control_file) {
		if (errno == EPERM) {
			/*
			 * We need to set the correct error value, does the
			 * group exist but we don't have the subsystem
			 * mounted at that point, or is it that the group
			 * does not exist. So we check if the tasks file
			 * exist. Before that, we need to extract the path.
			 */
			int len = strlen(path);

			while (*(path+len) != '/')
				len--;
			*(path+len+1) = '\0';
			strncat(path, "tasks", sizeof(path) - strlen(path));
			control_file = fopen(path, "r");
			if (!control_file) {
				if (errno == ENOENT)
					return ECGROUPSUBSYSNOTMOUNTED;
			}
			fclose(control_file);
			return ECGROUPNOTALLOWED;
		}
		return ECGROUPVALUENOTEXIST;
	}

	fprintf(control_file, "%s", val);
	fclose(control_file);
	return 0;
}

/** cgroup_modify_cgroup modifies the cgroup control files.
 * struct cgroup *cgroup: The name will be the cgroup to be modified.
 * The values will be the values to be modified, those not mentioned
 * in the structure will not be modified.
 *
 * The uids cannot be modified yet.
 *
 * returns 0 on success.
 *
 */

int cgroup_modify_cgroup(struct cgroup *cgroup)
{
	char *path, base[FILENAME_MAX];
	int i;
	int error;
	int ret;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!cgroup)
		return ECGROUPNOTALLOWED;

	for (i = 0; i < cgroup->index; i++) {
		if (!cgroup_test_subsys_mounted(cgroup->controller[i]->name)) {
			cgroup_dbg("subsystem %s is not mounted\n",
				cgroup->controller[i]->name);
			return ECGROUPSUBSYSNOTMOUNTED;
		}
	}

	for (i = 0; i < cgroup->index; i++) {
		int j;
		if (!cg_build_path(cgroup->name, base,
			cgroup->controller[i]->name))
			continue;
		for (j = 0; j < cgroup->controller[i]->index; j++) {
			ret = asprintf(&path, "%s%s", base,
				cgroup->controller[i]->values[j]->name);
			if (ret < 0) {
				last_errno = errno;
				error = ECGOTHER;
				goto err;
			}
			error = cg_set_control_value(path,
				cgroup->controller[i]->values[j]->value);
			free(path);
			path = NULL;
			if (error)
				goto err;
		}
	}
	if (path)
		free(path);
	return 0;
err:
	if (path)
		free(path);
	return error;

}

/**
 * @dst: Destination controller
 * @src: Source controller from which values will be copied to dst
 *
 * Create a duplicate copy of values under the specified controller
 */
int cgroup_copy_controller_values(struct cgroup_controller *dst,
					struct cgroup_controller *src)
{
	int i, ret = 0;

	if (!dst || !src)
		return ECGFAIL;

	strncpy(dst->name, src->name, FILENAME_MAX);
	for (i = 0; i < src->index; i++, dst->index++) {
		struct control_value *src_val = src->values[i];
		struct control_value *dst_val;

		dst->values[i] = calloc(1, sizeof(struct control_value));
		if (!dst->values[i]) {
			ret = ECGFAIL;
			goto err;
		}

		dst_val = dst->values[i];
		strncpy(dst_val->value, src_val->value, CG_VALUE_MAX);
		strncpy(dst_val->name, src_val->name, FILENAME_MAX);
	}
err:
	return ret;
}

/**
 * @dst: Destination control group
 * @src: Source from which values will be copied to dst
 *
 * Create a duplicate copy of src in dst. This will be useful for those who
 * that intend to create new instances based on an existing control group
 */
int cgroup_copy_cgroup(struct cgroup *dst, struct cgroup *src)
{
	int ret = 0, i;

	if (!dst || !src)
		return ECGROUPNOTEXIST;

	/*
	 * Should we just use the restrict keyword instead?
	 */
	if (dst == src)
		return ECGFAIL;

	cgroup_free_controllers(dst);

	for (i = 0; i < src->index; i++, dst->index++) {
		struct cgroup_controller *src_ctlr = src->controller[i];
		struct cgroup_controller *dst_ctlr;

		dst->controller[i] = calloc(1, sizeof(struct cgroup_controller));
		if (!dst->controller[i]) {
			ret = ECGFAIL;
			goto err;
		}

		dst_ctlr = dst->controller[i];
		ret = cgroup_copy_controller_values(dst_ctlr, src_ctlr);
		if (ret)
			goto err;
	}
err:
	return ret;
}

/** cgroup_create_cgroup creates a new control group.
 * struct cgroup *cgroup: The control group to be created
 *
 * returns 0 on success. We recommend calling cg_delete_cgroup
 * if this routine fails. That should do the cleanup operation.
 */
int cgroup_create_cgroup(struct cgroup *cgroup, int ignore_ownership)
{
	char *fts_path[2];
	char *base = NULL;
	char *path = NULL;
	int i, j, k;
	int error = 0;
	int retval = 0;
	int ret;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!cgroup)
		return ECGROUPNOTALLOWED;

	for (i = 0; i < cgroup->index;	i++) {
		if (!cgroup_test_subsys_mounted(cgroup->controller[i]->name))
			return ECGROUPSUBSYSNOTMOUNTED;
	}

	fts_path[0] = (char *)malloc(FILENAME_MAX);
	if (!fts_path[0]) {
		last_errno = errno;
		return ECGOTHER;
	}
	fts_path[1] = NULL;
	path = fts_path[0];

	/*
	 * XX: One important test to be done is to check, if you have multiple
	 * subsystems mounted at one point, all of them *have* be on the cgroup
	 * data structure. If not, we fail.
	 */
	for (k = 0; k < cgroup->index; k++) {
		if (!cg_build_path(cgroup->name, path,
				cgroup->controller[k]->name))
			continue;

		error = cg_create_control_group(path);
		if (error)
			goto err;

		base = strdup(path);

		if (!base) {
			last_errno = errno;
			error = ECGOTHER;
			goto err;
		}

		if (!ignore_ownership) {
			cgroup_dbg("Changing ownership of %s\n", fts_path[0]);
			error = cg_chown_recursive(fts_path,
				cgroup->control_uid, cgroup->control_gid);
		}

		if (error)
			goto err;

		for (j = 0; j < cgroup->controller[k]->index; j++) {
			ret = snprintf(path, FILENAME_MAX, "%s%s", base,
					cgroup->controller[k]->values[j]->name);
			cgroup_dbg("setting %s to %s, error %d\n", path,
				cgroup->controller[k]->values[j]->name, ret);
			if (ret < 0 || ret >= FILENAME_MAX) {
				last_errno = errno;
				error = ECGOTHER;
				goto err;
			}
			error = cg_set_control_value(path,
				cgroup->controller[k]->values[j]->value);
			/*
			 * Should we undo, what we've done in the loops above?
			 * An error should not be treated as fatal, since we
			 * have several read-only files and several files that
			 * are only conditionally created in the child.
			 *
			 * A middle ground would be to track that there
			 * was an error and return that value.
			 */
			if (error) {
				retval = error;
				continue;
			}
		}

		if (!ignore_ownership) {
			ret = snprintf(path, FILENAME_MAX, "%s/tasks", base);
			if (ret < 0 || ret >= FILENAME_MAX) {
				last_errno = errno;
				error = ECGOTHER;
				goto err;
			}
			error = chown(path, cgroup->tasks_uid,
							cgroup->tasks_gid);
			if (error) {
				last_errno = errno;
				error = ECGOTHER;
				goto err;
			}
		}
		free(base);
		base = NULL;
	}

err:
	if (path)
		free(path);
	if (base)
		free(base);
	if (retval && !error)
		error = retval;
	return error;
}

/**
 * Find the parent of the specified directory. It returns the parent (the
 * parent is usually name/.. unless name is a mount point.
 */
char *cgroup_find_parent(char *name)
{
	char child[FILENAME_MAX];
	char *parent = NULL;
	struct stat stat_child, stat_parent;
	char *type = NULL;
	char *dir = NULL;

	pthread_rwlock_rdlock(&cg_mount_table_lock);
	type = cg_mount_table[0].name;
	if (!cg_build_path_locked(name, child, type)) {
		pthread_rwlock_unlock(&cg_mount_table_lock);
		return NULL;
	}
	pthread_rwlock_unlock(&cg_mount_table_lock);

	cgroup_dbg("path is %s\n", child);
	dir = dirname(child);
	cgroup_dbg("directory name is %s\n", dir);

	if (asprintf(&parent, "%s/..", dir) < 0)
		return NULL;

	cgroup_dbg("parent's name is %s\n", parent);

	if (stat(dir, &stat_child) < 0)
		goto free_parent;

	if (stat(parent, &stat_parent) < 0)
		goto free_parent;

	/*
	 * Is the specified "name" a mount point?
	 */
	if (stat_parent.st_dev != stat_child.st_dev) {
		cgroup_dbg("parent is a mount point\n");
		strcpy(parent, ".");
	} else {
		dir = strdup(name);
		if (!dir)
			goto free_parent;
		dir = dirname(dir);
		if (strcmp(dir, ".") == 0)
			strcpy(parent, "..");
		else
			strcpy(parent, dir);
	}

	return parent;

free_parent:
	free(parent);
	return NULL;
}

/**
 * @cgroup: cgroup data structure to be filled with parent values and then
 *	  passed down for creation
 * @ignore_ownership: Ignore doing a chown on the newly created cgroup
 */
int cgroup_create_cgroup_from_parent(struct cgroup *cgroup,
					int ignore_ownership)
{
	char *parent = NULL;
	struct cgroup *parent_cgroup = NULL;
	int ret = ECGFAIL;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	parent = cgroup_find_parent(cgroup->name);
	if (!parent)
		return ret;

	cgroup_dbg("parent is %s\n", parent);
	parent_cgroup = cgroup_new_cgroup(parent);
	if (!parent_cgroup)
		goto err_nomem;

	if (cgroup_get_cgroup(parent_cgroup))
		goto err_parent;

	cgroup_dbg("got parent group for %s\n", parent_cgroup->name);
	ret = cgroup_copy_cgroup(cgroup, parent_cgroup);
	if (ret)
		goto err_parent;

	cgroup_dbg("copied parent group %s to %s\n", parent_cgroup->name,
							cgroup->name);
	ret = cgroup_create_cgroup(cgroup, ignore_ownership);

err_parent:
	cgroup_free(&parent_cgroup);
err_nomem:
	free(parent);
	return ret;
}

/** cgroup_delete cgroup deletes a control group.
 *  struct cgroup *cgroup takes the group which is to be deleted.
 *
 *  returns 0 on success.
 */
int cgroup_delete_cgroup(struct cgroup *cgroup, int ignore_migration)
{
	FILE *delete_tasks = NULL, *base_tasks = NULL;
	int tids;
	char path[FILENAME_MAX];
	int error = ECGROUPNOTALLOWED;
	int i, ret;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!cgroup)
		return ECGROUPNOTALLOWED;

	for (i = 0; i < cgroup->index; i++) {
		if (!cgroup_test_subsys_mounted(cgroup->controller[i]->name))
			return ECGROUPSUBSYSNOTMOUNTED;
	}

	for (i = 0; i < cgroup->index; i++) {
		if (!cg_build_path(cgroup->name, path,
					cgroup->controller[i]->name))
			continue;
		strncat(path, "../tasks", sizeof(path) - strlen(path));

		base_tasks = fopen(path, "w");
		if (!base_tasks)
			goto open_err;

		if (!cg_build_path(cgroup->name, path,
					cgroup->controller[i]->name)) {
			fclose(base_tasks);
			continue;
		}

		strncat(path, "tasks", sizeof(path) - strlen(path));

		delete_tasks = fopen(path, "r");
		if (!delete_tasks) {
			fclose(base_tasks);
			goto open_err;
		}

		while (!feof(delete_tasks)) {
			ret = fscanf(delete_tasks, "%d", &tids);
			if (ret == EOF || ret < 1)
				break;
			fprintf(base_tasks, "%d", tids);
		}

		fclose(delete_tasks);
		fclose(base_tasks);

		if (!cg_build_path(cgroup->name, path,
					cgroup->controller[i]->name))
			continue;
		error = rmdir(path);
		last_errno = errno;
	}
open_err:
	if (ignore_migration) {
		for (i = 0; i < cgroup->index; i++) {
			if (!cg_build_path(cgroup->name, path,
						cgroup->controller[i]->name))
				continue;
			error = rmdir(path);
			if (error < 0 && errno == ENOENT) {
				last_errno = errno;
				error = 0;
			}
		}
	}
	if (error)
		return ECGOTHER;

	return error;
}

/*
 * This function should really have more checks, but this version
 * will assume that the callers have taken care of everything.
 * Including the locking.
 */
static int cg_rd_ctrl_file(char *subsys, char *cgroup, char *file, char **value)
{
	char path[FILENAME_MAX];
	FILE *ctrl_file = NULL;
	int ret;

	if (!cg_build_path_locked(cgroup, path, subsys))
		return ECGFAIL;

	strncat(path, file, sizeof(path) - strlen(path));
	ctrl_file = fopen(path, "r");
	if (!ctrl_file)
		return ECGROUPVALUENOTEXIST;

	*value = malloc(CG_VALUE_MAX);
	if (!*value) {
		last_errno = errno;
		return ECGOTHER;
	}

	/*
	 * using %as crashes when we try to read from files like
	 * memory.stat
	 */
	ret = fscanf(ctrl_file, "%s", *value);
	if (ret == 0 || ret == EOF) {
		free(*value);
		*value = NULL;
	}

	fclose(ctrl_file);

	return 0;
}

/*
 * Call this function with required locks taken.
 */
static int cgroup_fill_cgc(struct dirent *ctrl_dir, struct cgroup *cgroup,
			struct cgroup_controller *cgc, int index)
{
	char *ctrl_name = NULL;
	char *ctrl_file = NULL;
	char *ctrl_value = NULL;
	char *d_name = NULL;
	char path[FILENAME_MAX+1];
	char *buffer = NULL;
	int error = 0;
	struct stat stat_buffer;

	d_name = strdup(ctrl_dir->d_name);

	if (!strcmp(d_name, ".") || !strcmp(d_name, "..")) {
		error = ECGINVAL;
		goto fill_error;
	}


	/*
	 * This part really needs to be optimized out. Probably use
	 * some sort of a flag, but this is fine for now.
	 */

	cg_build_path_locked(cgroup->name, path, cg_mount_table[index].name);
	strncat(path, d_name, sizeof(path) - strlen(path));

	error = stat(path, &stat_buffer);

	if (error) {
		error = ECGFAIL;
		goto fill_error;
	}

	cgroup->control_uid = stat_buffer.st_uid;
	cgroup->control_gid = stat_buffer.st_gid;

	ctrl_name = strtok_r(d_name, ".", &buffer);

	if (!ctrl_name) {
		error = ECGFAIL;
		goto fill_error;
	}

	ctrl_file = strtok_r(NULL, ".", &buffer);

	if (!ctrl_file) {
		error = ECGINVAL;
		goto fill_error;
	}

	if (strcmp(ctrl_name, cg_mount_table[index].name) == 0) {
		error = cg_rd_ctrl_file(cg_mount_table[index].name,
				cgroup->name, ctrl_dir->d_name, &ctrl_value);
		if (error || !ctrl_value)
			goto fill_error;

		if (cgroup_add_value_string(cgc, ctrl_dir->d_name,
				ctrl_value)) {
			error = ECGFAIL;
			goto fill_error;
		}
	}
fill_error:
	if (ctrl_value)
		free(ctrl_value);
	free(d_name);
	return error;
}

/*
 * cgroup_get_cgroup reads the cgroup data from the filesystem.
 * struct cgroup has the name of the group to be populated
 *
 * return 0 on success.
 */
int cgroup_get_cgroup(struct cgroup *cgroup)
{
	int i;
	char path[FILENAME_MAX];
	DIR *dir = NULL;
	struct dirent *ctrl_dir = NULL;
	char *control_path = NULL;
	int error;
	int ret;

	if (!cgroup_initialized) {
		/* ECGROUPNOTINITIALIZED */
		return ECGROUPNOTINITIALIZED;
	}

	if (!cgroup) {
		/* ECGROUPNOTALLOWED */
		return ECGROUPNOTALLOWED;
	}

	pthread_rwlock_rdlock(&cg_mount_table_lock);
	for (i = 0; i < CG_CONTROLLER_MAX &&
			cg_mount_table[i].name[0] != '\0'; i++) {
		/*
		 * cgc will not leak, since it has to be freed using
		 * cgroup_free_cgroup
		 */
		struct cgroup_controller *cgc;
		struct stat stat_buffer;
		int path_len;

		if (!cg_build_path_locked(NULL, path,
					cg_mount_table[i].name))
			continue;

		path_len = strlen(path);
		strncat(path, cgroup->name, FILENAME_MAX - path_len - 1);

		if (access(path, F_OK))
			continue;

		if (!cg_build_path_locked(cgroup->name, path,
					cg_mount_table[i].name)) {
			/*
			 * This fails when the cgroup does not exist
			 * for that controller.
			 */
			continue;
		}

		/*
		 * Get the uid and gid information
		 */

		ret = asprintf(&control_path, "%s/tasks", path);

		if (ret < 0) {
			last_errno = errno;
			error = ECGOTHER;
			goto unlock_error;
		}

		if (stat(control_path, &stat_buffer)) {
			last_errno = errno;
			free(control_path);
			error = ECGOTHER;
			goto unlock_error;
		}

		cgroup->tasks_uid = stat_buffer.st_uid;
		cgroup->tasks_gid = stat_buffer.st_gid;

		free(control_path);

		cgc = cgroup_add_controller(cgroup,
				cg_mount_table[i].name);
		if (!cgc) {
			error = ECGINVAL;
			goto unlock_error;
		}

		dir = opendir(path);
		if (!dir) {
			last_errno = errno;
			error = ECGOTHER;
			goto unlock_error;
		}

		while ((ctrl_dir = readdir(dir)) != NULL) {
			/*
			 * Skip over non regular files
			 */
			if (ctrl_dir->d_type != DT_REG)
				continue;

			error = cgroup_fill_cgc(ctrl_dir, cgroup, cgc, i);
			if (error == ECGFAIL) {
				closedir(dir);
				goto unlock_error;
			}
		}
		closedir(dir);
	}

	/* Check if the group really exists or not */
	if (!cgroup->index) {
		error = ECGROUPNOTEXIST;
		goto unlock_error;
	}

	pthread_rwlock_unlock(&cg_mount_table_lock);
	return 0;

unlock_error:
	pthread_rwlock_unlock(&cg_mount_table_lock);
	/*
	 * XX: Need to figure out how to cleanup? Cleanup just the stuff
	 * we added, or the whole structure.
	 */
	cgroup_free_controllers(cgroup);
	cgroup = NULL;
	return error;
}

/** cg_prepare_cgroup
 * Process the selected rule. Prepare the cgroup structure which can be
 * used to add the task to destination cgroup.
 *
 *
 *  returns 0 on success.
 */
static int cg_prepare_cgroup(struct cgroup *cgroup, pid_t pid,
					const char *dest,
					char *controllers[])
{
	int ret = 0, i;
	char *controller = NULL;
	struct cgroup_controller *cptr = NULL;

	/* Fill in cgroup details.  */
	cgroup_dbg("Will move pid %d to cgroup '%s'\n", pid, dest);

	strcpy(cgroup->name, dest);

	/* Scan all the controllers */
	for (i = 0; i < CG_CONTROLLER_MAX; i++) {
		if (!controllers[i])
			return 0;
		controller = controllers[i];

		/* If first string is "*" that means all the mounted
		 * controllers. */
		if (strcmp(controller, "*") == 0) {
			pthread_rwlock_rdlock(&cg_mount_table_lock);
			for (i = 0; i < CG_CONTROLLER_MAX &&
				cg_mount_table[i].name[0] != '\0'; i++) {
				cgroup_dbg("Adding controller %s\n",
					cg_mount_table[i].name);
				cptr = cgroup_add_controller(cgroup,
						cg_mount_table[i].name);
				if (!cptr) {
					cgroup_dbg("Adding controller '%s'"
						" failed\n",
						cg_mount_table[i].name);
					pthread_rwlock_unlock(&cg_mount_table_lock);
					cgroup_free_controllers(cgroup);
					return ECGROUPNOTALLOWED;
				}
			}
			pthread_rwlock_unlock(&cg_mount_table_lock);
			return ret;
		}

		/* it is individual controller names and not "*" */
		cgroup_dbg("Adding controller %s\n", controller);
		cptr = cgroup_add_controller(cgroup, controller);
		if (!cptr) {
			cgroup_dbg("Adding controller '%s' failed\n",
				controller);
			cgroup_free_controllers(cgroup);
			return ECGROUPNOTALLOWED;
		}
	}

	return ret;
}

/**
 * Finds the first rule in the cached list that matches the given UID or GID,
 * and returns a pointer to that rule.  This function uses rl_lock.
 *
 * This function may NOT be thread safe.
 * 	@param uid The UID to match
 * 	@param gid The GID to match
 * 	@return Pointer to the first matching rule, or NULL if no match
 * TODO: Determine thread-safeness and fix if not safe.
 */
static struct cgroup_rule *cgroup_find_matching_rule_uid_gid(const uid_t uid,
				const gid_t gid)
{
	/* Return value */
	struct cgroup_rule *ret = rl.head;

	/* Temporary user data */
	struct passwd *usr = NULL;

	/* Temporary group data */
	struct group *grp = NULL;

	/* Temporary string pointer */
	char *sp = NULL;

	/* Loop variable */
	int i = 0;

	pthread_rwlock_wrlock(&rl_lock);
	while (ret) {
		/* The wildcard rule always matches. */
		if ((ret->uid == CGRULE_WILD) && (ret->gid == CGRULE_WILD)) {
			goto finished;
		}

		/* This is the simple case of the UID matching. */
		if (ret->uid == uid) {
			goto finished;
		}

		/* This is the simple case of the GID matching. */
		if (ret->gid == gid) {
			goto finished;
		}

		/* If this is a group rule, the UID might be a member. */
		if (ret->name[0] == '@') {
			/* Get the group data. */
			sp = &(ret->name[1]);
			grp = getgrnam(sp);
			if (!grp) {
				continue;
			}

			/* Get the data for UID. */
			usr = getpwuid(uid);
			if (!usr) {
				continue;
			}

			/* If UID is a member of group, we matched. */
			for (i = 0; grp->gr_mem[i]; i++) {
				if (!(strcmp(usr->pw_name, grp->gr_mem[i])))
					goto finished;
			}
		}

		/* If we haven't matched, try the next rule. */
		ret = ret->next;
	}

	/* If we get here, no rules matched. */
	ret = NULL;

finished:
	pthread_rwlock_unlock(&rl_lock);
	return ret;
}

/**
 * Changes the cgroup of a program based on the rules in the config file.  If a
 * rule exists for the given UID or GID, then the given PID is placed into the
 * correct group.  By default, this function parses the configuration file each
 * time it is called.
 * 
 * The flags can alter the behavior of this function:
 * 	CGFLAG_USECACHE: Use cached rules instead of parsing the config file
 *
 * This function may NOT be thread safe. 
 * 	@param uid The UID to match
 * 	@param gid The GID to match
 * 	@param pid The PID of the process to move
 * 	@param flags Bit flags to change the behavior, as defined above
 * 	@return 0 on success, > 0 on error
 * TODO: Determine thread-safeness and fix of not safe.
 */
int cgroup_change_cgroup_uid_gid_flags(const uid_t uid, const gid_t gid,
				const pid_t pid, const int flags)
{
	/* Temporary pointer to a rule */
	struct cgroup_rule *tmp = NULL;

	/* Return codes */
	int ret = 0;

	/* We need to check this before doing anything else! */
	if (!cgroup_initialized) {
		cgroup_dbg("libcgroup is not initialized\n");
		ret = ECGROUPNOTINITIALIZED;
		goto finished;
	}

	/* 
	 * If the user did not ask for cached rules, we must parse the
	 * configuration to find a matching rule (if one exists).  Else, we'll
	 * find the first match in the cached list (rl).
	 */
	if (!(flags & CGFLAG_USECACHE)) {
		cgroup_dbg("Not using cached rules for PID %d.\n", pid);
		ret = cgroup_parse_rules(false, uid, gid);

		/* The configuration file has an error!  We must exit now. */
		if (ret != -1 && ret != 0) {
			cgroup_dbg("Failed to parse the configuration"
				" rules.\n");
			goto finished;
		}

		/* We did not find a matching rule, so we're done. */
		if (ret == 0) {
			cgroup_dbg("No rule found to match PID: %d, UID: %d, "
				"GID: %d\n", pid, uid, gid);
			goto finished;
		}

		/* Otherwise, we did match a rule and it's in trl. */
		tmp = trl.head;
	} else {
		/* Find the first matching rule in the cached list. */
		tmp = cgroup_find_matching_rule_uid_gid(uid, gid);
		if (!tmp) {
			cgroup_dbg("No rule found to match PID: %d, UID: %d, "
				"GID: %d\n", pid, uid, gid);
			ret = 0;
			goto finished;
		}
	}
	cgroup_dbg("Found matching rule %s for PID: %d, UID: %d, GID: %d\n",
			tmp->name, pid, uid, gid);

	/* If we are here, then we found a matching rule, so execute it. */
	do {
		cgroup_dbg("Executing rule %s for PID %d... ", tmp->name, pid);
		ret = cgroup_change_cgroup_path(tmp->destination,
				pid, tmp->controllers);
		if (ret) {
			cgroup_dbg("FAILED! (Error Code: %d)\n", ret);
			goto finished;
		}
		cgroup_dbg("OK!\n");

		/* Now, check for multi-line rules.  As long as the "next"
		 * rule starts with '%', it's actually part of the rule that
		 * we just executed.
		 */
		tmp = tmp->next;
	} while (tmp && (tmp->name[0] == '%'));

finished:
	return ret;
}

/**
 * Provides backwards-compatibility with older versions of the API.  This
 * function is deprecated, and cgroup_change_cgroup_uid_gid_flags() should be
 * used instead.  In fact, this function simply calls the newer one with flags
 * set to 0 (none).
 * 	@param uid The UID to match
 * 	@param gid The GID to match
 * 	@param pid The PID of the process to move
 * 	@return 0 on success, > 0 on error
 * 
 */
int cgroup_change_cgroup_uid_gid(uid_t uid, gid_t gid, pid_t pid)
{
	return cgroup_change_cgroup_uid_gid_flags(uid, gid, pid, 0);
}

/**
 * Changes the cgroup of a program based on the path provided.  In this case,
 * the user must already know into which cgroup the task should be placed and
 * no rules will be parsed.
 *
 *  returns 0 on success.
 */
int cgroup_change_cgroup_path(char *dest, pid_t pid, char *controllers[])
{
	int ret;
	struct cgroup cgroup;

	if (!cgroup_initialized) {
		cgroup_dbg("libcgroup is not initialized\n");
		return ECGROUPNOTINITIALIZED;
	}
	memset(&cgroup, 0, sizeof(struct cgroup));

	ret = cg_prepare_cgroup(&cgroup, pid, dest, controllers);
	if (ret)
		return ret;
	/* Add task to cgroup */
	ret = cgroup_attach_task_pid(&cgroup, pid);
	if (ret)
		cgroup_dbg("cgroup_attach_task_pid failed:%d\n", ret);
	cgroup_free_controllers(&cgroup);
	return ret;
}

/**
 * Print the cached rules table.  This function should be called only after
 * first calling cgroup_parse_config(), but it will work with an empty rule
 * list.
 * 	@param fp The file stream to print to
 */
void cgroup_print_rules_config(FILE *fp)
{
	/* Iterator */
	struct cgroup_rule *itr = NULL;

	/* Loop variable */
	int i = 0;

	pthread_rwlock_rdlock(&rl_lock);

	if (!(rl.head)) {
		fprintf(fp, "The rules table is empty.\n\n");
		pthread_rwlock_unlock(&rl_lock);
		return;
	}

	itr = rl.head;
	while (itr) {
		fprintf(fp, "Rule: %s\n", itr->name);

		if (itr->uid == CGRULE_WILD)
			fprintf(fp, "  UID: any\n");
		else if (itr->uid == CGRULE_INVALID)
			fprintf(fp, "  UID: N/A\n");
		else
			fprintf(fp, "  UID: %d\n", itr->uid);

		if (itr->gid == CGRULE_WILD)
			fprintf(fp, "  GID: any\n");
		else if (itr->gid == CGRULE_INVALID)
			fprintf(fp, "  GID: N/A\n");
		else
			fprintf(fp, "  GID: %d\n", itr->gid);

		fprintf(fp, "  DEST: %s\n", itr->destination);

		fprintf(fp, "  CONTROLLERS:\n");
		for (i = 0; i < MAX_MNT_ELEMENTS; i++) {
			if (itr->controllers[i]) {
				fprintf(fp, "    %s\n", itr->controllers[i]);
			}
		}
		fprintf(fp, "\n");
		itr = itr->next;
	}
	pthread_rwlock_unlock(&rl_lock);
}

/**
 * Reloads the rules list, using the given configuration file.  This function
 * is probably NOT thread safe (calls cgroup_parse_rules()).
 * 	@return 0 on success, > 0 on failure
 */
int cgroup_reload_cached_rules()
{
	/* Return codes */
	int ret = 0;

	cgroup_dbg("Reloading cached rules from %s.\n", CGRULES_CONF_FILE);
	if ((ret = cgroup_parse_rules(true, CGRULE_INVALID, CGRULE_INVALID))) {
		cgroup_dbg("Error parsing configuration file \"%s\": %d.\n",
			CGRULES_CONF_FILE, ret);
		ret = ECGROUPPARSEFAIL;
		goto finished;
	}
		
	#ifdef CGROUP_DEBUG
		cgroup_print_rules_config(stdout);
	#endif

finished:
	return ret;
}

/**
 * Initializes the rules cache.
 * 	@return 0 on success, > 0 on error
 */
int cgroup_init_rules_cache()
{
	/* Return codes */
	int ret = 0;

	/* Attempt to read the configuration file and cache the rules. */
	ret = cgroup_parse_rules(true, CGRULE_INVALID, CGRULE_INVALID);
	if (ret) {
		cgroup_dbg("Could not initialize rule cache, error was: %d\n",
			ret);
		cgroup_rules_loaded = false;
	} else {
		cgroup_rules_loaded = true;
	}

	return ret;
}

/**
 * cgroup_get_current_controller_path
 * @pid: pid of the current process for which the path is to be determined
 * @controller: name of the controller for which to determine current path
 * @current_path: a pointer that is filled with the value of the current
 * 		path as seen in /proc/<pid>/cgroup
 */
int cgroup_get_current_controller_path(pid_t pid, const char *controller,
					char **current_path)
{
	char *path = NULL;
	int ret;
	FILE *pid_cgroup_fd = NULL;

	if (!controller)
		return ECGOTHER;

	if (!cgroup_initialized) {
		cgroup_dbg("libcgroup is not initialized\n");
		return ECGROUPNOTINITIALIZED;
	}

	ret = asprintf(&path, "/proc/%d/cgroup", pid);
	if (ret <= 0) {
		cgroup_dbg("cannot allocate memory (/proc/pid/cgroup) ret %d\n",
			ret);
		return ret;
	}

	ret = ECGROUPNOTEXIST;
	pid_cgroup_fd = fopen(path, "r");
	if (!pid_cgroup_fd)
		goto cleanup_path;

	/*
	 * Why do we grab the cg_mount_table_lock?, the reason is that
	 * the cgroup of a pid can change via the cgroup_attach_task_pid()
	 * call. To make sure, we return consitent and safe results,
	 * we acquire the lock upfront. We can optimize by acquiring
	 * and releasing the lock in the while loop, but that
	 * will be more expensive.
	 */
	pthread_rwlock_rdlock(&cg_mount_table_lock);
	while (!feof(pid_cgroup_fd)) {
		char controllers[FILENAME_MAX];
		char cgroup_path[FILENAME_MAX];
		int num;
		char *savedptr;
		char *token;

		ret = fscanf(pid_cgroup_fd, "%d:%[^:]:%s\n", &num, controllers,
				cgroup_path);
		/*
		 * Magic numbers like "3" seem to be integrating into
		 * my daily life, I need some magic to help make them
		 * disappear :)
		 */
		if (ret != 3 || ret == EOF) {
			cgroup_dbg("read failed for pid_cgroup_fd ret %d\n",
				ret);
			last_errno = errno;
			ret = ECGOTHER;
			goto done;
		}

		token = strtok_r(controllers, ",", &savedptr);
		do {
			if (strncmp(controller, token, strlen(controller) + 1)
								== 0) {
				*current_path = strdup(cgroup_path);
				if (!*current_path) {
					last_errno = errno;
					ret = ECGOTHER;
					goto done;
				}
				ret = 0;
				goto done;
			}
			token = strtok_r(NULL, ",", &savedptr);
		} while (token);
	}

done:
	pthread_rwlock_unlock(&cg_mount_table_lock);
	fclose(pid_cgroup_fd);
cleanup_path:
	free(path);
	return ret;
}

char *cgroup_strerror(int code)
{
	assert((code >= ECGROUPNOTCOMPILED) && (code < ECGSENTINEL));
	if (code == ECGOTHER) {
		snprintf(errtext, MAXLEN, "%s, error message: %s",
			cgroup_strerror_codes[code % ECGROUPNOTCOMPILED],
			strerror(cgroup_get_last_errno()));
		return errtext;
	}
	return cgroup_strerror_codes[code % ECGROUPNOTCOMPILED];
}

/**
 * Return last errno, which caused ECGOTHER error.
 */
int cgroup_get_last_errno()
{
    return last_errno;
}


static int cg_walk_node(FTS *fts, FTSENT *ent, const int depth,
			struct cgroup_file_info *info)
{
	int ret = 0;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	cgroup_dbg("seeing file %s\n", ent->fts_path);

	info->path = ent->fts_name;
	info->parent = ent->fts_parent->fts_name;
	info->full_path = ent->fts_path;
	info->depth = ent->fts_level;
	info->type = CGROUP_FILE_TYPE_OTHER;

	if (depth && (info->depth > depth))
		return 0;

	switch (ent->fts_info) {
	case FTS_DNR:
	case FTS_ERR:
		errno = ent->fts_errno;
		break;
	case FTS_D:
		info->type = CGROUP_FILE_TYPE_DIR;
		break;
	case FTS_DC:
	case FTS_NSOK:
	case FTS_NS:
	case FTS_DP:
		break;
	case FTS_F:
		info->type = CGROUP_FILE_TYPE_FILE;
		break;
	case FTS_DEFAULT:
		break;
	}
	return ret;
}

int cgroup_walk_tree_next(const int depth, void **handle,
				struct cgroup_file_info *info, int base_level)
{
	int ret = 0;
	FTS *fts = *(FTS **)handle;
	FTSENT *ent;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!handle)
		return ECGINVAL;
	ent = fts_read(fts);
	if (!ent)
		return ECGEOF;
	if (!base_level && depth)
		base_level = ent->fts_level + depth;
	ret = cg_walk_node(fts, ent, base_level, info);
	*handle = fts;
	return ret;
}

int cgroup_walk_tree_end(void **handle)
{
	FTS *fts = *(FTS **)handle;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!handle)
		return ECGINVAL;
	fts_close(fts);
	return 0;
}

/*
 * TODO: Need to decide a better place to put this function.
 */
int cgroup_walk_tree_begin(char *controller, char *base_path, const int depth,
				void **handle, struct cgroup_file_info *info,
				int *base_level)
{
	int ret = 0;
	cgroup_dbg("path is %s\n", base_path);
	char *cg_path[2];
	char full_path[FILENAME_MAX];
	FTSENT *ent;
	FTS *fts;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!cg_build_path(base_path, full_path, controller))
		return ECGOTHER;

	*base_level = 0;
	cg_path[0] = full_path;
	cg_path[1] = NULL;

	fts = fts_open(cg_path, FTS_LOGICAL | FTS_NOCHDIR |
				FTS_NOSTAT, NULL);
	ent = fts_read(fts);
	if (!ent) {
		cgroup_dbg("fts_read failed\n");
		return ECGINVAL;
	}
	if (!*base_level && depth)
		*base_level = ent->fts_level + depth;
	ret = cg_walk_node(fts, ent, *base_level, info);
	*handle = fts;
	return ret;
}

/*
 * This parses a stat line which is in the form of (name value) pair
 * separated by a space.
 */
int cg_read_stat(FILE *fp, struct cgroup_stat *stat)
{
	int ret = 0;
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	char *token, *saveptr;

	read = getline(&line, &len, fp);
	if (read == -1)
		return ECGEOF;

	token = strtok_r(line, " ", &saveptr);
	if (!token) {
		ret = ECGINVAL;
		goto out_free;
	}
	strncpy(stat->name, token, FILENAME_MAX);

	token = strtok_r(NULL, " ", &saveptr);
	if (!token) {
		ret = ECGINVAL;
		goto out_free;
	}
	strncpy(stat->value, token, CG_VALUE_MAX);

out_free:
	free(line);
	return 0;
}

int cgroup_read_stats_end(void **handle)
{
	FILE *fp;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!handle)
		return ECGINVAL;

	fp = (FILE *)*handle;
	fclose(fp);
	return 0;
}

int cgroup_read_stats_next(void **handle, struct cgroup_stat *stat)
{
	int ret = 0;
	FILE *fp;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!handle || !stat)
		return ECGINVAL;

	fp = (FILE *)*handle;
	ret = cg_read_stat(fp, stat);
	*handle = fp;
	return ret;
}

/*
 * TODO: Need to decide a better place to put this function.
 */
int cgroup_read_stats_begin(char *controller, char *path, void **handle,
				struct cgroup_stat *stat)
{
	int ret = 0;
	char stat_file[FILENAME_MAX];
	FILE *fp;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!stat || !handle)
		return ECGINVAL;

	if (!cg_build_path(path, stat_file, controller))
		return ECGOTHER;

	sprintf(stat_file, "%s/%s.stat", stat_file, controller);

	fp = fopen(stat_file, "r");
	if (!fp) {
		cgroup_dbg("fopen failed\n");
		return ECGINVAL;
	}

	ret = cg_read_stat(fp, stat);
	*handle = fp;
	return ret;
}

int cgroup_get_task_end(void **handle)
{
	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!*handle)
		return ECGINVAL;

	fclose((FILE *) *handle);
	*handle = NULL;

	return 0;
}

int cgroup_get_task_next(void *handle, pid_t *pid)
{
	int ret;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!handle)
		return ECGINVAL;

	ret = fscanf((FILE *) handle, "%u", pid);

	if (ret != 1) {
		if (ret == EOF)
			return ECGEOF;
		last_errno = errno;
		return ECGOTHER;
	}

	return 0;
}

int cgroup_get_task_begin(char *cgroup, char *controller, void **handle,
								pid_t *pid)
{
	int ret = 0;
	char path[FILENAME_MAX];
	char *fullpath = NULL;

	if (!cgroup_initialized)
		return ECGROUPNOTINITIALIZED;

	if (!cg_build_path(cgroup, path, controller))
		return ECGOTHER;

	ret = asprintf(&fullpath, "%s/tasks", path);

	if (ret < 0) {
		last_errno = errno;
		return ECGOTHER;
	}

	*handle = (void *) fopen(fullpath, "r");
	free(fullpath);

	if (!*handle) {
		last_errno = errno;
		return ECGOTHER;
	}
	ret = cgroup_get_task_next(*handle, pid);

	return ret;
}