#include "cache.h"
#include "config.h"
#include "builtin.h"
#include "exec_cmd.h"
#include "run-command.h"
#include "levenshtein.h"
#include "help.h"
#include "common-cmds.h"
#include "string-list.h"
#include "column.h"
#include "version.h"
#include "refs.h"
#include "parse-options.h"

void add_cmdname(struct cmdnames *cmds, const char *name, int len)
{
	struct cmdname *ent;
	FLEX_ALLOC_MEM(ent, name, name, len);
	ent->len = len;

	ALLOC_GROW(cmds->names, cmds->cnt + 1, cmds->alloc);
	cmds->names[cmds->cnt++] = ent;
}

static void clean_cmdnames(struct cmdnames *cmds)
{
	int i;
	for (i = 0; i < cmds->cnt; ++i)
		free(cmds->names[i]);
	free(cmds->names);
	cmds->cnt = 0;
	cmds->alloc = 0;
}

static int cmdname_compare(const void *a_, const void *b_)
{
	struct cmdname *a = *(struct cmdname **)a_;
	struct cmdname *b = *(struct cmdname **)b_;
	return strcmp(a->name, b->name);
}

static void uniq(struct cmdnames *cmds)
{
	int i, j;

	if (!cmds->cnt)
		return;

	for (i = j = 1; i < cmds->cnt; i++) {
		if (!strcmp(cmds->names[i]->name, cmds->names[j-1]->name))
			free(cmds->names[i]);
		else
			cmds->names[j++] = cmds->names[i];
	}

	cmds->cnt = j;
}

void exclude_cmds(struct cmdnames *cmds, struct cmdnames *excludes)
{
	int ci, cj, ei;
	int cmp;

	ci = cj = ei = 0;
	while (ci < cmds->cnt && ei < excludes->cnt) {
		cmp = strcmp(cmds->names[ci]->name, excludes->names[ei]->name);
		if (cmp < 0)
			cmds->names[cj++] = cmds->names[ci++];
		else if (cmp == 0) {
			ei++;
			free(cmds->names[ci++]);
		} else if (cmp > 0)
			ei++;
	}

	while (ci < cmds->cnt)
		cmds->names[cj++] = cmds->names[ci++];

	cmds->cnt = cj;
}

static void pretty_print_cmdnames(struct cmdnames *cmds, unsigned int colopts)
{
	struct string_list list = STRING_LIST_INIT_NODUP;
	struct column_options copts;
	int i;

	for (i = 0; i < cmds->cnt; i++)
		string_list_append(&list, cmds->names[i]->name);
	/*
	 * always enable column display, we only consult column.*
	 * about layout strategy and stuff
	 */
	colopts = (colopts & ~COL_ENABLE_MASK) | COL_ENABLED;
	memset(&copts, 0, sizeof(copts));
	copts.indent = "  ";
	copts.padding = 2;
	print_columns(&list, colopts, &copts);
	string_list_clear(&list, 0);
}

static void list_commands_in_dir(struct cmdnames *cmds,
					 const char *path,
					 const char *prefix)
{
	DIR *dir = opendir(path);
	struct dirent *de;
	struct strbuf buf = STRBUF_INIT;
	int len;

	if (!dir)
		return;
	if (!prefix)
		prefix = "git-";

	strbuf_addf(&buf, "%s/", path);
	len = buf.len;

	while ((de = readdir(dir)) != NULL) {
		const char *ent;
		size_t entlen;

		if (!skip_prefix(de->d_name, prefix, &ent))
			continue;

		strbuf_setlen(&buf, len);
		strbuf_addstr(&buf, de->d_name);
		if (!is_executable(buf.buf))
			continue;

		entlen = strlen(ent);
		strip_suffix(ent, ".exe", &entlen);

		add_cmdname(cmds, ent, entlen);
	}
	closedir(dir);
	strbuf_release(&buf);
}

void load_command_list(const char *prefix,
		struct cmdnames *main_cmds,
		struct cmdnames *other_cmds)
{
	const char *env_path = getenv("PATH");
	const char *exec_path = git_exec_path();

	if (exec_path) {
		list_commands_in_dir(main_cmds, exec_path, prefix);
		QSORT(main_cmds->names, main_cmds->cnt, cmdname_compare);
		uniq(main_cmds);
	}

	if (env_path) {
		char *paths, *path, *colon;
		path = paths = xstrdup(env_path);
		while (1) {
			if ((colon = strchr(path, PATH_SEP)))
				*colon = 0;
			if (!exec_path || strcmp(path, exec_path))
				list_commands_in_dir(other_cmds, path, prefix);

			if (!colon)
				break;
			path = colon + 1;
		}
		free(paths);

		QSORT(other_cmds->names, other_cmds->cnt, cmdname_compare);
		uniq(other_cmds);
	}
	exclude_cmds(other_cmds, main_cmds);
}

void list_commands(unsigned int colopts,
		   struct cmdnames *main_cmds, struct cmdnames *other_cmds)
{
	if (main_cmds->cnt) {
		const char *exec_path = git_exec_path();
		printf_ln(_("available git commands in '%s'"), exec_path);
		putchar('\n');
		pretty_print_cmdnames(main_cmds, colopts);
		putchar('\n');
	}

	if (other_cmds->cnt) {
		printf_ln(_("git commands available from elsewhere on your $PATH"));
		putchar('\n');
		pretty_print_cmdnames(other_cmds, colopts);
		putchar('\n');
	}
}

static void extract_common_cmds(struct cmdname_help **p_common_cmds,
				int *p_nr)
{
	int i, nr = 0;
	struct cmdname_help *common_cmds;

	ALLOC_ARRAY(common_cmds, ARRAY_SIZE(command_list));

	for (i = 0; i < ARRAY_SIZE(command_list); i++) {
		const struct cmdname_help *cmd = command_list + i;

		if (cmd->category != CAT_mainporcelain ||
		    cmd->group == GROUP_NONE)
			continue;

		common_cmds[nr++] = *cmd;
	}

	*p_common_cmds = common_cmds;
	*p_nr = nr;
}

static int cmd_group_cmp(const void *elem1, const void *elem2)
{
	const struct cmdname_help *e1 = elem1;
	const struct cmdname_help *e2 = elem2;

	if (e1->group < e2->group)
		return -1;
	if (e1->group > e2->group)
		return 1;
	return strcmp(e1->name, e2->name);
}

void list_common_cmds_help(void)
{
	int i, longest = 0;
	int current_grp = -1;
	int nr = 0;
	struct cmdname_help *common_cmds;

	extract_common_cmds(&common_cmds, &nr);

	for (i = 0; i < nr; i++) {
		if (longest < strlen(common_cmds[i].name))
			longest = strlen(common_cmds[i].name);
	}

	QSORT(common_cmds, nr, cmd_group_cmp);

	puts(_("These are common Git commands used in various situations:"));

	for (i = 0; i < nr; i++) {
		if (common_cmds[i].group != current_grp) {
			printf("\n%s\n", _(common_cmd_groups[common_cmds[i].group]));
			current_grp = common_cmds[i].group;
		}

		printf("   %s   ", common_cmds[i].name);
		mput_char(' ', longest - strlen(common_cmds[i].name));
		puts(_(common_cmds[i].help));
	}
	free(common_cmds);
}

void list_all_cmds(void)
{
	struct cmdnames main_cmds, other_cmds;
	int i;

	memset(&main_cmds, 0, sizeof(main_cmds));
	memset(&other_cmds, 0, sizeof(other_cmds));
	load_command_list("git-", &main_cmds, &other_cmds);

	for (i = 0; i < main_cmds.cnt; i++)
		puts(main_cmds.names[i]->name);
	for (i = 0; i < other_cmds.cnt; i++)
		puts(other_cmds.names[i]->name);

	clean_cmdnames(&main_cmds);
	clean_cmdnames(&other_cmds);
}

void list_porcelain_cmds(void)
{
	int i, nr = ARRAY_SIZE(command_list);
	struct cmdname_help *cmds = command_list;

	for (i = 0; i < nr; i++) {
		if (cmds[i].category != CAT_mainporcelain)
			continue;
		puts(cmds[i].name);
	}
}

static int cmd_category_cmp(const void *elem1, const void *elem2)
{
	const struct cmdname_help *e1 = elem1;
	const struct cmdname_help *e2 = elem2;

	if (e1->category < e2->category)
		return -1;
	if (e1->category > e2->category)
		return 1;
	return strcmp(e1->name, e2->name);
}

static void list_commands_by_category(int cat, struct cmdname_help *cmds,
				      int nr, int longest)
{
	int i;

	for (i = 0; i < nr; i++) {
		struct cmdname_help *cmd = cmds + i;

		if (cmd->category != cat)
			continue;

		printf("   %s   ", cmd->name);
		mput_char(' ', longest - strlen(cmd->name));
		puts(_(cmd->help));
	}
}

void list_all_cmds_help(void)
{
	int i, longest = 0;
	int nr = ARRAY_SIZE(command_list);
	struct cmdname_help *cmds = command_list;

	for (i = 0; i < nr; i++) {
		struct cmdname_help *cmd = cmds + i;

		if (longest < strlen(cmd->name))
			longest = strlen(cmd->name);
	}

	QSORT(cmds, nr, cmd_category_cmp);

	printf("%s\n\n", _("Main Porcelain Commands"));
	list_commands_by_category(CAT_mainporcelain, cmds, nr, longest);

	printf("\n%s\n\n", _("Ancillary Commands / Manipulators"));
	list_commands_by_category(CAT_ancillarymanipulators, cmds, nr, longest);

	printf("\n%s\n\n", _("Ancillary Commands / Interrogators"));
	list_commands_by_category(CAT_ancillaryinterrogators, cmds, nr, longest);

	printf("\n%s\n\n", _("Interacting with Others"));
	list_commands_by_category(CAT_foreignscminterface, cmds, nr, longest);

	printf("\n%s\n\n", _("Low-level Commands / Manipulators"));
	list_commands_by_category(CAT_plumbingmanipulators, cmds, nr, longest);

	printf("\n%s\n\n", _("Low-level Commands / Interrogators"));
	list_commands_by_category(CAT_plumbinginterrogators, cmds, nr, longest);

	printf("\n%s\n\n", _("Low-level Commands / Synching Repositories"));
	list_commands_by_category(CAT_synchingrepositories, cmds, nr, longest);

	printf("\n%s\n\n", _("Low-level Commands / Internal Helpers"));
	list_commands_by_category(CAT_purehelpers, cmds, nr, longest);
}

int is_in_cmdlist(struct cmdnames *c, const char *s)
{
	int i;
	for (i = 0; i < c->cnt; i++)
		if (!strcmp(s, c->names[i]->name))
			return 1;
	return 0;
}

static int autocorrect;
static struct cmdnames aliases;

static int git_unknown_cmd_config(const char *var, const char *value, void *cb)
{
	const char *p;

	if (!strcmp(var, "help.autocorrect"))
		autocorrect = git_config_int(var,value);
	/* Also use aliases for command lookup */
	if (skip_prefix(var, "alias.", &p))
		add_cmdname(&aliases, p, strlen(p));

	return git_default_config(var, value, cb);
}

static int levenshtein_compare(const void *p1, const void *p2)
{
	const struct cmdname *const *c1 = p1, *const *c2 = p2;
	const char *s1 = (*c1)->name, *s2 = (*c2)->name;
	int l1 = (*c1)->len;
	int l2 = (*c2)->len;
	return l1 != l2 ? l1 - l2 : strcmp(s1, s2);
}

static void add_cmd_list(struct cmdnames *cmds, struct cmdnames *old)
{
	int i;
	ALLOC_GROW(cmds->names, cmds->cnt + old->cnt, cmds->alloc);

	for (i = 0; i < old->cnt; i++)
		cmds->names[cmds->cnt++] = old->names[i];
	FREE_AND_NULL(old->names);
	old->cnt = 0;
}

/* An empirically derived magic number */
#define SIMILARITY_FLOOR 7
#define SIMILAR_ENOUGH(x) ((x) < SIMILARITY_FLOOR)

static const char bad_interpreter_advice[] =
	N_("'%s' appears to be a git command, but we were not\n"
	"able to execute it. Maybe git-%s is broken?");

const char *help_unknown_cmd(const char *cmd)
{
	int i, n, best_similarity = 0, nr_common;
	struct cmdnames main_cmds, other_cmds;
	struct cmdname_help *common_cmds;

	memset(&main_cmds, 0, sizeof(main_cmds));
	memset(&other_cmds, 0, sizeof(other_cmds));
	memset(&aliases, 0, sizeof(aliases));

	read_early_config(git_unknown_cmd_config, NULL);

	load_command_list("git-", &main_cmds, &other_cmds);

	add_cmd_list(&main_cmds, &aliases);
	add_cmd_list(&main_cmds, &other_cmds);
	QSORT(main_cmds.names, main_cmds.cnt, cmdname_compare);
	uniq(&main_cmds);

	extract_common_cmds(&common_cmds, &nr_common);

	/* This abuses cmdname->len for levenshtein distance */
	for (i = 0, n = 0; i < main_cmds.cnt; i++) {
		int cmp = 0; /* avoid compiler stupidity */
		const char *candidate = main_cmds.names[i]->name;

		/*
		 * An exact match means we have the command, but
		 * for some reason exec'ing it gave us ENOENT; probably
		 * it's a bad interpreter in the #! line.
		 */
		if (!strcmp(candidate, cmd))
			die(_(bad_interpreter_advice), cmd, cmd);

		/* Does the candidate appear in common_cmds list? */
		while (n < nr_common &&
		       (cmp = strcmp(common_cmds[n].name, candidate)) < 0)
			n++;
		if ((n < nr_common) && !cmp) {
			/* Yes, this is one of the common commands */
			n++; /* use the entry from common_cmds[] */
			if (starts_with(candidate, cmd)) {
				/* Give prefix match a very good score */
				main_cmds.names[i]->len = 0;
				continue;
			}
		}

		main_cmds.names[i]->len =
			levenshtein(cmd, candidate, 0, 2, 1, 3) + 1;
	}
	FREE_AND_NULL(common_cmds);

	QSORT(main_cmds.names, main_cmds.cnt, levenshtein_compare);

	if (!main_cmds.cnt)
		die(_("Uh oh. Your system reports no Git commands at all."));

	/* skip and count prefix matches */
	for (n = 0; n < main_cmds.cnt && !main_cmds.names[n]->len; n++)
		; /* still counting */

	if (main_cmds.cnt <= n) {
		/* prefix matches with everything? that is too ambiguous */
		best_similarity = SIMILARITY_FLOOR + 1;
	} else {
		/* count all the most similar ones */
		for (best_similarity = main_cmds.names[n++]->len;
		     (n < main_cmds.cnt &&
		      best_similarity == main_cmds.names[n]->len);
		     n++)
			; /* still counting */
	}
	if (autocorrect && n == 1 && SIMILAR_ENOUGH(best_similarity)) {
		const char *assumed = main_cmds.names[0]->name;
		main_cmds.names[0] = NULL;
		clean_cmdnames(&main_cmds);
		fprintf_ln(stderr,
			   _("WARNING: You called a Git command named '%s', "
			     "which does not exist."),
			   cmd);
		if (autocorrect < 0)
			fprintf_ln(stderr,
				   _("Continuing under the assumption that "
				     "you meant '%s'."),
				   assumed);
		else {
			fprintf_ln(stderr,
				   _("Continuing in %0.1f seconds, "
				     "assuming that you meant '%s'."),
				   (float)autocorrect/10.0, assumed);
			sleep_millisec(autocorrect * 100);
		}
		return assumed;
	}

	fprintf_ln(stderr, _("git: '%s' is not a git command. See 'git --help'."), cmd);

	if (SIMILAR_ENOUGH(best_similarity)) {
		fprintf_ln(stderr,
			   Q_("\nThe most similar command is",
			      "\nThe most similar commands are",
			   n));

		for (i = 0; i < n; i++)
			fprintf(stderr, "\t%s\n", main_cmds.names[i]->name);
	}

	exit(1);
}

int cmd_version(int argc, const char **argv, const char *prefix)
{
	int build_options = 0;
	const char * const usage[] = {
		N_("git version [<options>]"),
		NULL
	};
	struct option options[] = {
		OPT_BOOL(0, "build-options", &build_options,
			 "also print build options"),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options, usage, 0);

	/*
	 * The format of this string should be kept stable for compatibility
	 * with external projects that rely on the output of "git version".
	 *
	 * Always show the version, even if other options are given.
	 */
	printf("git version %s\n", git_version_string);

	if (build_options) {
		printf("cpu: %s\n", GIT_HOST_CPU);
		if (git_built_from_commit_string[0])
			printf("built from commit: %s\n",
			       git_built_from_commit_string);
		else
			printf("no commit associated with this build\n");
		printf("sizeof-long: %d\n", (int)sizeof(long));
		/* NEEDSWORK: also save and output GIT-BUILD_OPTIONS? */
	}
	return 0;
}

struct similar_ref_cb {
	const char *base_ref;
	struct string_list *similar_refs;
};

static int append_similar_ref(const char *refname, const struct object_id *oid,
			      int flags, void *cb_data)
{
	struct similar_ref_cb *cb = (struct similar_ref_cb *)(cb_data);
	char *branch = strrchr(refname, '/') + 1;
	const char *remote;

	/* A remote branch of the same name is deemed similar */
	if (skip_prefix(refname, "refs/remotes/", &remote) &&
	    !strcmp(branch, cb->base_ref))
		string_list_append(cb->similar_refs, remote);
	return 0;
}

static struct string_list guess_refs(const char *ref)
{
	struct similar_ref_cb ref_cb;
	struct string_list similar_refs = STRING_LIST_INIT_NODUP;

	ref_cb.base_ref = ref;
	ref_cb.similar_refs = &similar_refs;
	for_each_ref(append_similar_ref, &ref_cb);
	return similar_refs;
}

void help_unknown_ref(const char *ref, const char *cmd, const char *error)
{
	int i;
	struct string_list suggested_refs = guess_refs(ref);

	fprintf_ln(stderr, _("%s: %s - %s"), cmd, ref, error);

	if (suggested_refs.nr > 0) {
		fprintf_ln(stderr,
			   Q_("\nDid you mean this?",
			      "\nDid you mean one of these?",
			      suggested_refs.nr));
		for (i = 0; i < suggested_refs.nr; i++)
			fprintf(stderr, "\t%s\n", suggested_refs.items[i].string);
	}

	string_list_clear(&suggested_refs, 0);
	exit(1);
}
