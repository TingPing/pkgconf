/*
 * pkg.c
 * higher-level dependency graph compilation, management and manipulation
 *
 * Copyright (c) 2011, 2012, 2013 pkgconf authors (see AUTHORS).
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * This software is provided 'as is' and without any warranty, express or
 * implied.  In no event shall the authors be liable for any damages arising
 * from the use of this software.
 */

#include <libpkgconf/stdinc.h>
#include <libpkgconf/config.h>
#include <libpkgconf/libpkgconf.h>

/*
 * !doc
 *
 * libpkgconf `pkg` module
 * =======================
 *
 * The `pkg` module provides dependency resolution services and the overall `.pc` file parsing
 * routines.
 */

#ifdef _WIN32
#	define PKG_CONFIG_REG_KEY "Software\\pkgconfig\\PKG_CONFIG_PATH"
#	undef PKG_DEFAULT_PATH
#	define PKG_DEFAULT_PATH "../lib/pkgconfig;../share/pkgconfig"
#	define strncasecmp _strnicmp
#	define strcasecmp _stricmp
#endif

#define PKG_CONFIG_EXT ".pc"

static inline bool
str_has_suffix(const char *str, const char *suffix)
{
	size_t str_len = strlen(str);
	size_t suf_len = strlen(suffix);

	if (str_len < suf_len)
		return false;

	return !strncasecmp(str + str_len - suf_len, suffix, suf_len);
}

static inline const char *
get_default_pkgconfig_path(char *outbuf, size_t outlen)
{
#ifdef _WIN32
	char namebuf[MAX_PATH];
	char *p;

	int sizepath = GetModuleFileName(NULL, namebuf, sizeof namebuf);
	char * winslash;
	namebuf[sizepath] = '\0';
	while ((winslash = strchr (namebuf, '\\')) != NULL)
	{
		*winslash = '/';
	}
	p = strrchr(namebuf, '/');
	if (p == NULL)
		return PKG_DEFAULT_PATH;

	*p = '\0';
	pkgconf_strlcpy(outbuf, namebuf, outlen);
	pkgconf_strlcat(outbuf, "/", outlen);
	pkgconf_strlcat(outbuf, "../lib/pkgconfig", outlen);
	pkgconf_strlcat(outbuf, ";", outlen);
	pkgconf_strlcat(outbuf, namebuf, outlen);
	pkgconf_strlcat(outbuf, "/", outlen);
	pkgconf_strlcat(outbuf, "../share/pkgconfig", outlen);

	return outbuf;
#else
	(void) outbuf;
	(void) outlen;
#endif

	return PKG_DEFAULT_PATH;
}

static const char *
pkg_get_parent_dir(pkgconf_pkg_t *pkg, char *buf, size_t buflen)
{
	char *pathbuf;

	pkgconf_strlcpy(buf, pkg->filename, buflen);
	pathbuf = strrchr(buf, PKG_DIR_SEP_S);
	if (pathbuf == NULL)
		pathbuf = strrchr(buf, '/');
	if (pathbuf != NULL)
		pathbuf[0] = '\0';

	return buf;
}

/*
 * !doc
 *
 * .. c:function:: void pkgconf_pkg_dir_list_build(pkgconf_client_t *client)
 *
 *    Bootstraps the package search paths.  If the ``PKGCONF_PKG_PKGF_ENV_ONLY`` `flag` is set on the client,
 *    then only the ``PKG_CONFIG_PATH`` environment variable will be used, otherwise both the
 *    ``PKG_CONFIG_PATH`` and ``PKG_CONFIG_LIBDIR`` environment variables will be used.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to bootstrap.
 *    :return: nothing
 */
void
pkgconf_pkg_dir_list_build(pkgconf_client_t *client)
{
	pkgconf_path_build_from_environ("PKG_CONFIG_PATH", NULL, &client->dir_list, true);

	if (!(client->flags & PKGCONF_PKG_PKGF_ENV_ONLY))
	{
		char pathbuf[PKGCONF_BUFSIZE];

		pkgconf_path_build_from_environ("PKG_CONFIG_LIBDIR", get_default_pkgconfig_path(pathbuf, sizeof pathbuf), &client->dir_list, true);
	}
}

typedef void (*pkgconf_pkg_parser_keyword_func_t)(const pkgconf_client_t *client, pkgconf_pkg_t *pkg, const char *keyword, const size_t lineno, const ptrdiff_t offset, char *value);
typedef struct {
	const char *keyword;
	const pkgconf_pkg_parser_keyword_func_t func;
	const ptrdiff_t offset;
} pkgconf_pkg_parser_keyword_pair_t;

static int pkgconf_pkg_parser_keyword_pair_cmp(const void *key, const void *ptr)
{
	const pkgconf_pkg_parser_keyword_pair_t *pair = ptr;
	return strcasecmp(key, pair->keyword);
}

static void
pkgconf_pkg_parser_tuple_func(const pkgconf_client_t *client, pkgconf_pkg_t *pkg, const char *keyword, const size_t lineno, const ptrdiff_t offset, char *value)
{
	(void) keyword;
	(void) lineno;

	char **dest = (char **)((char *) pkg + offset);
	*dest = pkgconf_tuple_parse(client, &pkg->vars, value);
}

static void
pkgconf_pkg_parser_fragment_func(const pkgconf_client_t *client, pkgconf_pkg_t *pkg, const char *keyword, const size_t lineno, const ptrdiff_t offset, char *value)
{
	pkgconf_list_t *dest = (pkgconf_list_t *)((char *) pkg + offset);
	bool ret = pkgconf_fragment_parse(client, dest, &pkg->vars, value);

	if (!ret)
	{
		pkgconf_warn(client, "%s:" SIZE_FMT_SPECIFIER ": warning: unable to parse field '%s' into an argument vector, value [%s]\n", pkg->filename,
			     lineno, keyword, value);
	}
}

static void
pkgconf_pkg_parser_dependency_func(const pkgconf_client_t *client, pkgconf_pkg_t *pkg, const char *keyword, const size_t lineno, const ptrdiff_t offset, char *value)
{
	(void) keyword;
	(void) lineno;

	pkgconf_list_t *dest = (pkgconf_list_t *)((char *) pkg + offset);
	pkgconf_dependency_parse(client, pkg, dest, value);
}

/* keep this in alphabetical order */
static const pkgconf_pkg_parser_keyword_pair_t pkgconf_pkg_parser_keyword_funcs[] = {
	{"CFLAGS", pkgconf_pkg_parser_fragment_func, offsetof(pkgconf_pkg_t, cflags)},
	{"CFLAGS.private", pkgconf_pkg_parser_fragment_func, offsetof(pkgconf_pkg_t, cflags_private)},
	{"Conflicts", pkgconf_pkg_parser_dependency_func, offsetof(pkgconf_pkg_t, conflicts)},
	{"Description", pkgconf_pkg_parser_tuple_func, offsetof(pkgconf_pkg_t, description)},
	{"LIBS", pkgconf_pkg_parser_fragment_func, offsetof(pkgconf_pkg_t, libs)},
	{"LIBS.private", pkgconf_pkg_parser_fragment_func, offsetof(pkgconf_pkg_t, libs_private)},
	{"Name", pkgconf_pkg_parser_tuple_func, offsetof(pkgconf_pkg_t, realname)},
	{"Provides", pkgconf_pkg_parser_dependency_func, offsetof(pkgconf_pkg_t, provides)},
	{"Requires", pkgconf_pkg_parser_dependency_func, offsetof(pkgconf_pkg_t, required)},
	{"Requires.private", pkgconf_pkg_parser_dependency_func, offsetof(pkgconf_pkg_t, requires_private)},
	{"Version", pkgconf_pkg_parser_tuple_func, offsetof(pkgconf_pkg_t, version)},
};

static bool
pkgconf_pkg_parser_keyword_set(const pkgconf_client_t *client, pkgconf_pkg_t *pkg, const size_t lineno, const char *keyword, char *value)
{
	const pkgconf_pkg_parser_keyword_pair_t *pair = bsearch(keyword,
		pkgconf_pkg_parser_keyword_funcs, PKGCONF_ARRAY_SIZE(pkgconf_pkg_parser_keyword_funcs),
		sizeof(pkgconf_pkg_parser_keyword_pair_t), pkgconf_pkg_parser_keyword_pair_cmp);

	if (pair == NULL || pair->func == NULL)
		return false;

	pair->func(client, pkg, keyword, lineno, pair->offset, value);
	return true;
}

static const char *
determine_prefix(const pkgconf_pkg_t *pkg, char *buf, size_t buflen)
{
	char *pathiter;

	pkgconf_strlcpy(buf, pkg->filename, buflen);
	pkgconf_path_relocate(buf, buflen);

	pathiter = strrchr(buf, PKG_DIR_SEP_S);
	if (pathiter == NULL)
		pathiter = strrchr(buf, '/');
	if (pathiter != NULL)
		pathiter[0] = '\0';

	pathiter = strrchr(buf, PKG_DIR_SEP_S);
	if (pathiter == NULL)
		pathiter = strrchr(buf, '/');
	if (pathiter == NULL)
		return NULL;

	/* parent dir is not pkgconfig, can't relocate then */
	if (strcmp(pathiter + 1, "pkgconfig"))
		return NULL;

	/* okay, work backwards and do it again. */
	pathiter[0] = '\0';
	pathiter = strrchr(buf, PKG_DIR_SEP_S);
	if (pathiter == NULL)
		pathiter = strrchr(buf, '/');
	if (pathiter == NULL)
		return NULL;

	pathiter[0] = '\0';

	return buf;
}

typedef struct {
	const char *field;
	const ptrdiff_t offset;
} pkgconf_pkg_validity_check_t;

static const pkgconf_pkg_validity_check_t pkgconf_pkg_validations[] = {
	{"Name", offsetof(pkgconf_pkg_t, realname)},
	{"Description", offsetof(pkgconf_pkg_t, description)},
	{"Version", offsetof(pkgconf_pkg_t, version)},
};

static bool
pkgconf_pkg_validate(const pkgconf_client_t *client, const pkgconf_pkg_t *pkg)
{
	size_t i;
	bool valid = true;

	for (i = 0; i < PKGCONF_ARRAY_SIZE(pkgconf_pkg_validations); i++)
	{
		char **p = (char **)((char *) pkg + pkgconf_pkg_validations[i].offset);

		if (*p != NULL)
			continue;

		pkgconf_warn(client, "%s: warning: file does not declare a `%s' field\n", pkg->filename, pkgconf_pkg_validations[i].field);
		valid = false;
	}

	return valid;
}

/*
 * !doc
 *
 * .. c:function:: pkgconf_pkg_t *pkgconf_pkg_new_from_file(const pkgconf_client_t *client, const char *filename, FILE *f)
 *
 *    Parse a .pc file into a pkgconf_pkg_t object structure.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to use for dependency resolution.
 *    :param char* filename: The filename of the package file (including full path).
 *    :param FILE* f: The file object to read from.
 *    :returns: A ``pkgconf_pkg_t`` object which contains the package data.
 *    :rtype: pkgconf_pkg_t *
 */
pkgconf_pkg_t *
pkgconf_pkg_new_from_file(pkgconf_client_t *client, const char *filename, FILE *f)
{
	pkgconf_pkg_t *pkg;
	char readbuf[PKGCONF_BUFSIZE];
	char pathbuf[PKGCONF_ITEM_SIZE];
	char *idptr;
	size_t lineno = 0;

	pkg = calloc(sizeof(pkgconf_pkg_t), 1);
	pkg->filename = strdup(filename);
	pkgconf_tuple_add(client, &pkg->vars, "pcfiledir", pkg_get_parent_dir(pkg, pathbuf, sizeof pathbuf), true);

	/* make module id */
	if ((idptr = strrchr(pkg->filename, PKG_DIR_SEP_S)) != NULL)
		idptr++;
#ifdef _WIN32
	else if ((idptr = strrchr(pkg->filename, '/')) != NULL)
		idptr++;
#endif
	else
		idptr = pkg->filename;

	pkg->id = strdup(idptr);
	idptr = strrchr(pkg->id, '.');
	if (idptr)
		*idptr = '\0';

	while (pkgconf_fgetline(readbuf, PKGCONF_BUFSIZE, f) != NULL)
	{
		char op, *p, *key, *value;
		bool warned_key_whitespace = false, warned_value_whitespace = false;

		lineno++;

		PKGCONF_TRACE(client, "%s:" SIZE_FMT_SPECIFIER " > [%s]", filename, lineno, readbuf);

		p = readbuf;
		while (*p && (isalpha((unsigned int)*p) || isdigit((unsigned int)*p) || *p == '_' || *p == '.'))
			p++;

		key = readbuf;
		if (!isalpha((unsigned int)*key) && !isdigit((unsigned int)*p))
			continue;

		while (*p && isspace((unsigned int)*p))
		{
			if (!warned_key_whitespace)
			{
				pkgconf_warn(client, "%s:" SIZE_FMT_SPECIFIER ": warning: whitespace encountered while parsing key section\n",
					pkg->filename, lineno);
				warned_key_whitespace = true;
			}

			/* set to null to avoid trailing spaces in key */
			*p = '\0';
			p++;
		}

		op = *p;
		*p = '\0';
		p++;

		while (*p && isspace((unsigned int)*p))
			p++;

		value = p;
		p = value + (strlen(value) - 1);
		while (*p && isspace((unsigned int) *p) && p > value)
		{
			if (!warned_value_whitespace && op == '=')
			{
				pkgconf_warn(client, "%s:" SIZE_FMT_SPECIFIER ": warning: trailing whitespace encountered while parsing value section\n",
					pkg->filename, lineno);
				warned_value_whitespace = true;
			}

			*p = '\0';
			p--;
		}

		switch (op)
		{
		case ':':
			pkgconf_pkg_parser_keyword_set(client, pkg, lineno, key, value);
			break;
		case '=':
			if (strcmp(key, client->prefix_varname) || !(client->flags & PKGCONF_PKG_PKGF_REDEFINE_PREFIX))
				pkgconf_tuple_add(client, &pkg->vars, key, value, true);
			else
			{
				const char *relvalue = determine_prefix(pkg, pathbuf, sizeof pathbuf);
				if (relvalue != NULL)
				{
					pkgconf_tuple_add(client, &pkg->vars, "orig_prefix", value, true);
					pkgconf_tuple_add(client, &pkg->vars, key, relvalue, false);
				}
				else
					pkgconf_tuple_add(client, &pkg->vars, key, value, true);
			}
			break;
		default:
			break;
		}
	}

	fclose(f);

	if (!pkgconf_pkg_validate(client, pkg))
	{
		pkgconf_warn(client, "%s: warning: skipping invalid file\n", pkg->filename);
		pkgconf_pkg_free(client, pkg);
		return NULL;
	}

	pkgconf_dependency_add(client, &pkg->provides, pkg->id, pkg->version, PKGCONF_CMP_EQUAL);

	return pkgconf_pkg_ref(client, pkg);
}

/*
 * !doc
 *
 * .. c:function:: void pkgconf_pkg_free(pkgconf_client_t *client, pkgconf_pkg_t *pkg)
 *
 *    Releases all releases for a given ``pkgconf_pkg_t`` object.
 *
 *    :param pkgconf_client_t* client: The client which owns the ``pkgconf_pkg_t`` object, `pkg`.
 *    :param pkgconf_pkg_t* pkg: The package to free.
 *    :return: nothing
 */
void
pkgconf_pkg_free(pkgconf_client_t *client, pkgconf_pkg_t *pkg)
{
	if (pkg == NULL)
		return;

	if (pkg->flags & PKGCONF_PKG_PROPF_STATIC && !(pkg->flags & PKGCONF_PKG_PROPF_VIRTUAL))
		return;

	pkgconf_cache_remove(client, pkg);

	pkgconf_dependency_free(&pkg->required);
	pkgconf_dependency_free(&pkg->requires_private);
	pkgconf_dependency_free(&pkg->conflicts);
	pkgconf_dependency_free(&pkg->provides);

	pkgconf_fragment_free(&pkg->cflags);
	pkgconf_fragment_free(&pkg->cflags_private);
	pkgconf_fragment_free(&pkg->libs);
	pkgconf_fragment_free(&pkg->libs_private);

	pkgconf_tuple_free(&pkg->vars);

	if (pkg->flags & PKGCONF_PKG_PROPF_VIRTUAL)
		return;

	if (pkg->id != NULL)
		free(pkg->id);

	if (pkg->filename != NULL)
		free(pkg->filename);

	if (pkg->realname != NULL)
		free(pkg->realname);

	if (pkg->version != NULL)
		free(pkg->version);

	if (pkg->description != NULL)
		free(pkg->description);

	if (pkg->url != NULL)
		free(pkg->url);

	if (pkg->pc_filedir != NULL)
		free(pkg->pc_filedir);

	free(pkg);
}

/*
 * !doc
 *
 * .. c:function:: pkgconf_pkg_t *pkgconf_pkg_ref(const pkgconf_client_t *client, pkgconf_pkg_t *pkg)
 *
 *    Adds an additional reference to the package object.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object which owns the package being referenced.
 *    :param pkgconf_pkg_t* pkg: The package object being referenced.
 *    :return: The package itself with an incremented reference count.
 *    :rtype: pkgconf_pkg_t *
 */
pkgconf_pkg_t *
pkgconf_pkg_ref(pkgconf_client_t *client, pkgconf_pkg_t *pkg)
{
	if (pkg->owner != NULL && pkg->owner != client)
		PKGCONF_TRACE(client, "WTF: client %p refers to package %p owned by other client %p", client, pkg, pkg->owner);

	pkg->owner = client;
	pkg->refcount++;
	PKGCONF_TRACE(client, "refcount@%p: %d", pkg, pkg->refcount);

	return pkg;
}

/*
 * !doc
 *
 * .. c:function:: void pkgconf_pkg_unref(pkgconf_client_t *client, pkgconf_pkg_t *pkg)
 *
 *    Releases a reference on the package object.  If the reference count is 0, then also free the package.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object which owns the package being dereferenced.
 *    :param pkgconf_pkg_t* pkg: The package object being dereferenced.
 *    :return: nothing
 */
void
pkgconf_pkg_unref(pkgconf_client_t *client, pkgconf_pkg_t *pkg)
{
	if (pkg->owner != NULL && pkg->owner != client)
		PKGCONF_TRACE(client, "WTF: client %p unrefs package %p owned by other client %p", client, pkg, pkg->owner);

	pkg->refcount--;
	PKGCONF_TRACE(pkg->owner, "refcount@%p: %d", pkg, pkg->refcount);

	if (pkg->refcount <= 0)
		pkgconf_pkg_free(pkg->owner, pkg);
}

static inline pkgconf_pkg_t *
pkgconf_pkg_try_specific_path(pkgconf_client_t *client, const char *path, const char *name)
{
	pkgconf_pkg_t *pkg = NULL;
	FILE *f;
	char locbuf[PKGCONF_ITEM_SIZE];
	char uninst_locbuf[PKGCONF_ITEM_SIZE];

	PKGCONF_TRACE(client, "trying path: %s for %s", path, name);

	snprintf(locbuf, sizeof locbuf, "%s/%s" PKG_CONFIG_EXT, path, name);
	snprintf(uninst_locbuf, sizeof uninst_locbuf, "%s/%s-uninstalled" PKG_CONFIG_EXT, path, name);

	if (!(client->flags & PKGCONF_PKG_PKGF_NO_UNINSTALLED) && (f = fopen(uninst_locbuf, "r")) != NULL)
	{
		PKGCONF_TRACE(client, "found (uninstalled): %s", uninst_locbuf);
		pkg = pkgconf_pkg_new_from_file(client, uninst_locbuf, f);
		pkg->flags |= PKGCONF_PKG_PROPF_UNINSTALLED;
	}
	else if ((f = fopen(locbuf, "r")) != NULL)
	{
		PKGCONF_TRACE(client, "found: %s", locbuf);
		pkg = pkgconf_pkg_new_from_file(client, locbuf, f);
	}

	return pkg;
}

static pkgconf_pkg_t *
pkgconf_pkg_scan_dir(pkgconf_client_t *client, const char *path, void *data, pkgconf_pkg_iteration_func_t func)
{
	DIR *dir;
	struct dirent *dirent;
	pkgconf_pkg_t *outpkg = NULL;

	dir = opendir(path);
	if (dir == NULL)
		return NULL;

	PKGCONF_TRACE(client, "scanning dir [%s]", path);

	for (dirent = readdir(dir); dirent != NULL; dirent = readdir(dir))
	{
		char filebuf[PKGCONF_ITEM_SIZE];
		pkgconf_pkg_t *pkg;
		FILE *f;

		pkgconf_strlcpy(filebuf, path, sizeof filebuf);
		pkgconf_strlcat(filebuf, "/", sizeof filebuf);
		pkgconf_strlcat(filebuf, dirent->d_name, sizeof filebuf);

		if (!str_has_suffix(filebuf, PKG_CONFIG_EXT))
			continue;

		PKGCONF_TRACE(client, "trying file [%s]", filebuf);

		f = fopen(filebuf, "r");
		if (f == NULL)
			continue;

		pkg = pkgconf_pkg_new_from_file(client, filebuf, f);
		if (pkg != NULL)
		{
			if (func(pkg, data))
			{
				outpkg = pkg;
				goto out;
			}

			pkgconf_pkg_unref(client, pkg);
		}
	}

out:
	closedir(dir);
	return outpkg;
}

/*
 * !doc
 *
 * .. c:function:: pkgconf_pkg_t *pkgconf_scan_all(pkgconf_client_t *client, void *data, pkgconf_pkg_iteration_func_t func)
 *
 *    Iterates over all packages found in the `package directory list`, running ``func`` on them.  If ``func`` returns true,
 *    then stop iteration and return the last iterated package.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to use for dependency resolution.
 *    :param void* data: An opaque pointer to data to provide the iteration function with.
 *    :param pkgconf_pkg_iteration_func_t func: A function which is called for each package to determine if the package matches,
 *        always return ``false`` to iterate over all packages.
 *    :return: A package object reference if one is found by the scan function, else ``NULL``.
 *    :rtype: pkgconf_pkg_t *
 */
pkgconf_pkg_t *
pkgconf_scan_all(pkgconf_client_t *client, void *data, pkgconf_pkg_iteration_func_t func)
{
	pkgconf_node_t *n;
	pkgconf_pkg_t *pkg;

	PKGCONF_FOREACH_LIST_ENTRY(client->dir_list.head, n)
	{
		pkgconf_path_t *pnode = n->data;

		PKGCONF_TRACE(client, "scanning directory: %s", pnode->path);

		if ((pkg = pkgconf_pkg_scan_dir(client, pnode->path, data, func)) != NULL)
			return pkg;
	}

	return NULL;
}

#ifdef _WIN32
static pkgconf_pkg_t *
pkgconf_pkg_find_in_registry_key(pkgconf_client_t *client, HKEY hkey, const char *name)
{
	pkgconf_pkg_t *pkg = NULL;

	HKEY key;
	int i = 0;

	char buf[16384]; /* per registry limits */
	DWORD bufsize = sizeof buf;
	if (RegOpenKeyEx(hkey, PKG_CONFIG_REG_KEY,
				0, KEY_READ, &key) != ERROR_SUCCESS)
		return NULL;

	while (RegEnumValue(key, i++, buf, &bufsize, NULL, NULL, NULL, NULL)
			== ERROR_SUCCESS)
	{
		char pathbuf[PKGCONF_ITEM_SIZE];
		DWORD type;
		DWORD pathbuflen = sizeof pathbuf;

		if (RegQueryValueEx(key, buf, NULL, &type, (LPBYTE) pathbuf, &pathbuflen)
				== ERROR_SUCCESS && type == REG_SZ)
		{
			pkg = pkgconf_pkg_try_specific_path(client, pathbuf, name);
			if (pkg != NULL)
				break;
		}

		bufsize = sizeof buf;
	}

	RegCloseKey(key);
	return pkg;
}
#endif

/*
 * !doc
 *
 * .. c:function:: pkgconf_pkg_t *pkgconf_pkg_find(pkgconf_client_t *client, const char *name)
 *
 *    Search for a package.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to use for dependency resolution.
 *    :param char* name: The name of the package `atom` to use for searching.
 *    :return: A package object reference if the package was found, else ``NULL``.
 *    :rtype: pkgconf_pkg_t *
 */
pkgconf_pkg_t *
pkgconf_pkg_find(pkgconf_client_t *client, const char *name)
{
	char pathbuf[PKGCONF_ITEM_SIZE];
	pkgconf_pkg_t *pkg = NULL;
	pkgconf_node_t *n;
	FILE *f;

	PKGCONF_TRACE(client, "looking for: %s", name);

	/* name might actually be a filename. */
	if (str_has_suffix(name, PKG_CONFIG_EXT))
	{
		if ((f = fopen(name, "r")) != NULL)
		{
			pkgconf_pkg_t *pkg;

			PKGCONF_TRACE(client, "%s is a file", name);

			pkg = pkgconf_pkg_new_from_file(client, name, f);
			if (pkg != NULL)
			{
				pkgconf_path_add(pkg_get_parent_dir(pkg, pathbuf, sizeof pathbuf), &client->dir_list, true);
				return pkg;
			}
		}
	}

	/* check builtins */
	if ((pkg = pkgconf_builtin_pkg_get(name)) != NULL)
	{
		PKGCONF_TRACE(client, "%s is a builtin", name);
		return pkg;
	}

	/* check cache */
	if (!(client->flags & PKGCONF_PKG_PKGF_NO_CACHE))
	{
		if ((pkg = pkgconf_cache_lookup(client, name)) != NULL)
		{
			PKGCONF_TRACE(client, "%s is cached", name);
			return pkg;
		}
	}

	PKGCONF_FOREACH_LIST_ENTRY(client->dir_list.head, n)
	{
		pkgconf_path_t *pnode = n->data;

		pkg = pkgconf_pkg_try_specific_path(client, pnode->path, name);
		if (pkg != NULL)
			goto out;
	}

#ifdef _WIN32
	/* support getting PKG_CONFIG_PATH from registry */
	pkg = pkgconf_pkg_find_in_registry_key(client, HKEY_CURRENT_USER, name);
	if (!pkg)
		pkg = pkgconf_pkg_find_in_registry_key(client, HKEY_LOCAL_MACHINE, name);
#endif

out:
	pkgconf_cache_add(client, pkg);

	return pkg;
}

/*
 * !doc
 *
 * .. c:function:: int pkgconf_compare_version(const char *a, const char *b)
 *
 *    Compare versions using RPM version comparison rules as described in the LSB.
 *
 *    :param char* a: The first version to compare in the pair.
 *    :param char* b: The second version to compare in the pair.
 *    :return: -1 if the first version is greater, 0 if both versions are equal, 1 if the second version is greater.
 *    :rtype: int
 */
int
pkgconf_compare_version(const char *a, const char *b)
{
	char oldch1, oldch2;
	char buf1[PKGCONF_ITEM_SIZE], buf2[PKGCONF_ITEM_SIZE];
	char *str1, *str2;
	char *one, *two;
	int ret;
	bool isnum;

	/* optimization: if version matches then it's the same version. */
	if (a == NULL)
		return 1;

	if (b == NULL)
		return -1;

	if (!strcasecmp(a, b))
		return 0;

	pkgconf_strlcpy(buf1, a, sizeof buf1);
	pkgconf_strlcpy(buf2, b, sizeof buf2);

	one = str1 = buf1;
	two = str2 = buf2;

	while (*one || *two)
	{
		while (*one && !isalnum((unsigned int)*one) && *one != '~')
			one++;
		while (*two && !isalnum((unsigned int)*two) && *two != '~')
			two++;

		if (*one == '~' || *two == '~')
		{
			if (*one != '~')
				return -1;
			if (*two != '~')
				return 1;

			one++;
			two++;
			continue;
		}

		if (!(*one && *two))
			break;

		str1 = one;
		str2 = two;

		if (isdigit((unsigned int)*str1))
		{
			while (*str1 && isdigit((unsigned int)*str1))
				str1++;

			while (*str2 && isdigit((unsigned int)*str2))
				str2++;

			isnum = true;
		}
		else
		{
			while (*str1 && isalpha((unsigned int)*str1))
				str1++;

			while (*str2 && isalpha((unsigned int)*str2))
				str2++;

			isnum = false;
		}

		oldch1 = *str1;
		oldch2 = *str2;

		*str1 = '\0';
		*str2 = '\0';

		if (one == str1)
			return -1;

		if (two == str2)
			return (isnum ? 1 : -1);

		if (isnum)
		{
			int onelen, twolen;

			while (*one == '0')
				one++;

			while (*two == '0')
				two++;

			onelen = strlen(one);
			twolen = strlen(two);

			if (onelen > twolen)
				return 1;
			else if (twolen > onelen)
				return -1;
		}

		ret = strcmp(one, two);
		if (ret)
			return ret;

		*str1 = oldch1;
		*str2 = oldch2;

		one = str1;
		two = str2;
	}

	if ((!*one) && (!*two))
		return 0;

	if (!*one)
		return -1;

	return 1;
}

static pkgconf_pkg_t pkg_config_virtual = {
	.id = "pkg-config",
	.realname = "pkg-config",
	.description = "virtual package defining pkg-config API version supported",
	.url = PACKAGE_BUGREPORT,
	.version = PACKAGE_VERSION,
	.flags = PKGCONF_PKG_PROPF_STATIC,
	.vars = {
		.head = &(pkgconf_node_t){
			.next = &(pkgconf_node_t){
				.next = &(pkgconf_node_t){
					.data = &(pkgconf_tuple_t){
						.key = "pc_system_libdirs",
						.value = SYSTEM_LIBDIR,
					}
				},
				.data = &(pkgconf_tuple_t){
					.key = "pc_system_includedirs",
					.value = SYSTEM_INCLUDEDIR,
				}
			},
			.data = &(pkgconf_tuple_t){
				.key = "pc_path",
				.value = PKG_DEFAULT_PATH,
			},
		},
		.tail = NULL,
	}
};

static pkgconf_pkg_t pkgconf_virtual = {
	.id = "pkgconf",
	.realname = "pkgconf",
	.description = "virtual package defining pkgconf API version supported",
	.url = PACKAGE_BUGREPORT,
	.version = PACKAGE_VERSION,
	.flags = PKGCONF_PKG_PROPF_STATIC,
	.vars = {
		.head = &(pkgconf_node_t){
			.next = &(pkgconf_node_t){
				.next = &(pkgconf_node_t){
					.data = &(pkgconf_tuple_t){
						.key = "pc_system_libdirs",
						.value = SYSTEM_LIBDIR,
					}
				},
				.data = &(pkgconf_tuple_t){
					.key = "pc_system_includedirs",
					.value = SYSTEM_INCLUDEDIR,
				}
			},
			.data = &(pkgconf_tuple_t){
				.key = "pc_path",
				.value = PKG_DEFAULT_PATH,
			},
		},
		.tail = NULL,
	},
};

typedef struct {
	const char *name;
	pkgconf_pkg_t *pkg;
} pkgconf_builtin_pkg_pair_t;

/* keep these in alphabetical order */
static const pkgconf_builtin_pkg_pair_t pkgconf_builtin_pkg_pair_set[] = {
	{"pkg-config", &pkg_config_virtual},
	{"pkgconf", &pkgconf_virtual},
};

static int pkgconf_builtin_pkg_pair_cmp(const void *key, const void *ptr)
{
	const pkgconf_builtin_pkg_pair_t *pair = ptr;
	return strcasecmp(key, pair->name);
}

/*
 * !doc
 *
 * .. c:function:: pkgconf_pkg_t *pkgconf_builtin_pkg_get(const char *name)
 *
 *    Looks up a built-in package.  The package should not be freed or dereferenced.
 *
 *    :param char* name: An atom corresponding to a built-in package to search for.
 *    :return: the built-in package if present, else ``NULL``.
 *    :rtype: pkgconf_pkg_t *
 */
pkgconf_pkg_t *
pkgconf_builtin_pkg_get(const char *name)
{
	const pkgconf_builtin_pkg_pair_t *pair = bsearch(name, pkgconf_builtin_pkg_pair_set,
		PKGCONF_ARRAY_SIZE(pkgconf_builtin_pkg_pair_set), sizeof(pkgconf_builtin_pkg_pair_t),
		pkgconf_builtin_pkg_pair_cmp);

	return (pair != NULL) ? pair->pkg : NULL;
}

typedef bool (*pkgconf_vercmp_res_func_t)(const char *a, const char *b);

typedef struct {
	const char *name;
	pkgconf_pkg_comparator_t compare;
} pkgconf_pkg_comparator_pair_t;

static const pkgconf_pkg_comparator_pair_t pkgconf_pkg_comparator_names[] = {
	{"!=",		PKGCONF_CMP_NOT_EQUAL},
	{"(any)",	PKGCONF_CMP_ANY},
	{"<",		PKGCONF_CMP_LESS_THAN},
	{"<=",		PKGCONF_CMP_LESS_THAN_EQUAL},
	{"=",		PKGCONF_CMP_EQUAL},
	{">",		PKGCONF_CMP_GREATER_THAN},
	{">=",		PKGCONF_CMP_GREATER_THAN_EQUAL},
};

static int pkgconf_pkg_comparator_pair_namecmp(const void *key, const void *ptr)
{
	const pkgconf_pkg_comparator_pair_t *pair = ptr;
	return strcmp(key, pair->name);
}

static bool pkgconf_pkg_comparator_lt(const char *a, const char *b)
{
	return (pkgconf_compare_version(a, b) < 0);
}

static bool pkgconf_pkg_comparator_gt(const char *a, const char *b)
{
	return (pkgconf_compare_version(a, b) > 0);
}

static bool pkgconf_pkg_comparator_lte(const char *a, const char *b)
{
	return (pkgconf_compare_version(a, b) <= 0);
}

static bool pkgconf_pkg_comparator_gte(const char *a, const char *b)
{
	return (pkgconf_compare_version(a, b) >= 0);
}

static bool pkgconf_pkg_comparator_eq(const char *a, const char *b)
{
	return (pkgconf_compare_version(a, b) == 0);
}

static bool pkgconf_pkg_comparator_ne(const char *a, const char *b)
{
	return (pkgconf_compare_version(a, b) != 0);
}

static bool pkgconf_pkg_comparator_any(const char *a, const char *b)
{
	(void) a;
	(void) b;

	return true;
}

static bool pkgconf_pkg_comparator_none(const char *a, const char *b)
{
	(void) a;
	(void) b;

	return false;
}

static const pkgconf_vercmp_res_func_t pkgconf_pkg_comparator_impls[] = {
	[PKGCONF_CMP_ANY]			= pkgconf_pkg_comparator_any,
	[PKGCONF_CMP_LESS_THAN]			= pkgconf_pkg_comparator_lt,
	[PKGCONF_CMP_GREATER_THAN]		= pkgconf_pkg_comparator_gt,
	[PKGCONF_CMP_LESS_THAN_EQUAL]		= pkgconf_pkg_comparator_lte,
	[PKGCONF_CMP_GREATER_THAN_EQUAL]	= pkgconf_pkg_comparator_gte,
	[PKGCONF_CMP_EQUAL]			= pkgconf_pkg_comparator_eq,
	[PKGCONF_CMP_NOT_EQUAL]			= pkgconf_pkg_comparator_ne,
};

/*
 * !doc
 *
 * .. c:function:: const char *pkgconf_pkg_get_comparator(const pkgconf_dependency_t *pkgdep)
 *
 *    Returns the comparator used in a depgraph dependency node as a string.
 *
 *    :param pkgconf_dependency_t* pkgdep: The depgraph dependency node to return the comparator for.
 *    :return: A string matching the comparator or ``"???"``.
 *    :rtype: char *
 */
const char *
pkgconf_pkg_get_comparator(const pkgconf_dependency_t *pkgdep)
{
	if (pkgdep->compare >= PKGCONF_ARRAY_SIZE(pkgconf_pkg_comparator_names))
		return "???";

	return pkgconf_pkg_comparator_names[pkgdep->compare].name;
}

/*
 * !doc
 *
 * .. c:function:: pkgconf_pkg_comparator_t pkgconf_pkg_comparator_lookup_by_name(const char *name)
 *
 *    Look up the appropriate comparator bytecode in the comparator set (defined
 *    in ``pkg.c``, see ``pkgconf_pkg_comparator_names`` and ``pkgconf_pkg_comparator_impls``).
 *
 *    :param char* name: The comparator to look up by `name`.
 *    :return: The comparator bytecode if found, else ``PKGCONF_CMP_ANY``.
 *    :rtype: pkgconf_pkg_comparator_t
 */
pkgconf_pkg_comparator_t
pkgconf_pkg_comparator_lookup_by_name(const char *name)
{
	const pkgconf_pkg_comparator_pair_t *p = bsearch(name, pkgconf_pkg_comparator_names,
		PKGCONF_ARRAY_SIZE(pkgconf_pkg_comparator_names), sizeof(pkgconf_pkg_comparator_pair_t),
		pkgconf_pkg_comparator_pair_namecmp);

	return (p != NULL) ? p->compare : PKGCONF_CMP_ANY;
}

typedef struct {
	pkgconf_dependency_t *pkgdep;
} pkgconf_pkg_scan_providers_ctx_t;

typedef struct {
	const pkgconf_vercmp_res_func_t rulecmp[PKGCONF_CMP_COUNT];
	const pkgconf_vercmp_res_func_t depcmp[PKGCONF_CMP_COUNT];
} pkgconf_pkg_provides_vermatch_rule_t;

static const pkgconf_pkg_provides_vermatch_rule_t pkgconf_pkg_provides_vermatch_rules[] = {
	[PKGCONF_CMP_ANY] = {
		.rulecmp = {
			[PKGCONF_CMP_ANY]			= pkgconf_pkg_comparator_none,
                },
		.depcmp = {
			[PKGCONF_CMP_ANY]			= pkgconf_pkg_comparator_none,
                },
	},
	[PKGCONF_CMP_LESS_THAN] = {
		.rulecmp = {
			[PKGCONF_CMP_ANY]			= pkgconf_pkg_comparator_none,
			[PKGCONF_CMP_LESS_THAN]			= pkgconf_pkg_comparator_lt,
			[PKGCONF_CMP_GREATER_THAN]		= pkgconf_pkg_comparator_gt,
			[PKGCONF_CMP_LESS_THAN_EQUAL]		= pkgconf_pkg_comparator_lte,
			[PKGCONF_CMP_GREATER_THAN_EQUAL]	= pkgconf_pkg_comparator_gte,
		},
		.depcmp = {
			[PKGCONF_CMP_GREATER_THAN]		= pkgconf_pkg_comparator_lt,
			[PKGCONF_CMP_GREATER_THAN_EQUAL]	= pkgconf_pkg_comparator_lt,
			[PKGCONF_CMP_EQUAL]			= pkgconf_pkg_comparator_lt,
			[PKGCONF_CMP_NOT_EQUAL]			= pkgconf_pkg_comparator_gte,
		},
	},
	[PKGCONF_CMP_GREATER_THAN] = {
		.rulecmp = {
			[PKGCONF_CMP_ANY]			= pkgconf_pkg_comparator_none,
			[PKGCONF_CMP_LESS_THAN]			= pkgconf_pkg_comparator_lt,
			[PKGCONF_CMP_GREATER_THAN]		= pkgconf_pkg_comparator_gt,
			[PKGCONF_CMP_LESS_THAN_EQUAL]		= pkgconf_pkg_comparator_lte,
			[PKGCONF_CMP_GREATER_THAN_EQUAL]	= pkgconf_pkg_comparator_gte,
		},
		.depcmp = {
			[PKGCONF_CMP_LESS_THAN]			= pkgconf_pkg_comparator_gt,
			[PKGCONF_CMP_LESS_THAN_EQUAL]		= pkgconf_pkg_comparator_gt,
			[PKGCONF_CMP_EQUAL]			= pkgconf_pkg_comparator_gt,
			[PKGCONF_CMP_NOT_EQUAL]			= pkgconf_pkg_comparator_lte,
		},
	},
	[PKGCONF_CMP_LESS_THAN_EQUAL] = {
		.rulecmp = {
			[PKGCONF_CMP_ANY]			= pkgconf_pkg_comparator_none,
			[PKGCONF_CMP_LESS_THAN]			= pkgconf_pkg_comparator_lt,
			[PKGCONF_CMP_GREATER_THAN]		= pkgconf_pkg_comparator_gt,
			[PKGCONF_CMP_LESS_THAN_EQUAL]		= pkgconf_pkg_comparator_lte,
			[PKGCONF_CMP_GREATER_THAN_EQUAL]	= pkgconf_pkg_comparator_gte,
		},
		.depcmp = {
			[PKGCONF_CMP_GREATER_THAN]		= pkgconf_pkg_comparator_lte,
			[PKGCONF_CMP_GREATER_THAN_EQUAL]	= pkgconf_pkg_comparator_lte,
			[PKGCONF_CMP_EQUAL]			= pkgconf_pkg_comparator_lte,
			[PKGCONF_CMP_NOT_EQUAL]			= pkgconf_pkg_comparator_gt,
		},
	},
	[PKGCONF_CMP_GREATER_THAN_EQUAL] = {
		.rulecmp = {
			[PKGCONF_CMP_ANY]			= pkgconf_pkg_comparator_none,
			[PKGCONF_CMP_LESS_THAN]			= pkgconf_pkg_comparator_lt,
			[PKGCONF_CMP_GREATER_THAN]		= pkgconf_pkg_comparator_gt,
			[PKGCONF_CMP_LESS_THAN_EQUAL]		= pkgconf_pkg_comparator_lte,
			[PKGCONF_CMP_GREATER_THAN_EQUAL]	= pkgconf_pkg_comparator_gte,
		},
		.depcmp = {
			[PKGCONF_CMP_LESS_THAN]			= pkgconf_pkg_comparator_gte,
			[PKGCONF_CMP_LESS_THAN_EQUAL]		= pkgconf_pkg_comparator_gte,
			[PKGCONF_CMP_EQUAL]			= pkgconf_pkg_comparator_gte,
			[PKGCONF_CMP_NOT_EQUAL]			= pkgconf_pkg_comparator_lt,
		},
	},
	[PKGCONF_CMP_EQUAL] = {
		.rulecmp = {
			[PKGCONF_CMP_ANY]			= pkgconf_pkg_comparator_none,
			[PKGCONF_CMP_LESS_THAN]			= pkgconf_pkg_comparator_lt,
			[PKGCONF_CMP_GREATER_THAN]		= pkgconf_pkg_comparator_gt,
			[PKGCONF_CMP_LESS_THAN_EQUAL]		= pkgconf_pkg_comparator_lte,
			[PKGCONF_CMP_GREATER_THAN_EQUAL]	= pkgconf_pkg_comparator_gte,
			[PKGCONF_CMP_EQUAL]			= pkgconf_pkg_comparator_eq,
			[PKGCONF_CMP_NOT_EQUAL]			= pkgconf_pkg_comparator_ne
		},
		.depcmp = {
			[PKGCONF_CMP_ANY]			= pkgconf_pkg_comparator_none,
                },
	},
	[PKGCONF_CMP_NOT_EQUAL] = {
		.rulecmp = {
			[PKGCONF_CMP_ANY]			= pkgconf_pkg_comparator_none,
			[PKGCONF_CMP_LESS_THAN]			= pkgconf_pkg_comparator_gte,
			[PKGCONF_CMP_GREATER_THAN]		= pkgconf_pkg_comparator_lte,
			[PKGCONF_CMP_LESS_THAN_EQUAL]		= pkgconf_pkg_comparator_gt,
			[PKGCONF_CMP_GREATER_THAN_EQUAL]	= pkgconf_pkg_comparator_lt,
			[PKGCONF_CMP_EQUAL]			= pkgconf_pkg_comparator_ne,
			[PKGCONF_CMP_NOT_EQUAL]			= pkgconf_pkg_comparator_eq
		},
		.depcmp = {
			[PKGCONF_CMP_ANY]			= pkgconf_pkg_comparator_none,
                },
	},
};

/*
 * pkgconf_pkg_scan_provides_vercmp(pkgdep, provider)
 *
 * compare a provides node against the requested dependency node.
 *
 * XXX: maybe handle PKGCONF_CMP_ANY in a versioned comparison
 */
static bool
pkgconf_pkg_scan_provides_vercmp(const pkgconf_dependency_t *pkgdep, const pkgconf_dependency_t *provider)
{
	const pkgconf_pkg_provides_vermatch_rule_t *rule = &pkgconf_pkg_provides_vermatch_rules[pkgdep->compare];

	if (rule->depcmp[provider->compare] != NULL &&
	    !rule->depcmp[provider->compare](provider->version, pkgdep->version))
		return false;

	if (rule->rulecmp[provider->compare] != NULL &&
	    !rule->rulecmp[provider->compare](pkgdep->version, provider->version))
		return false;

	return true;
}

/*
 * pkgconf_pkg_scan_provides_entry(pkg, ctx)
 *
 * attempt to match a single package's Provides rules against the requested dependency node.
 */
static bool
pkgconf_pkg_scan_provides_entry(const pkgconf_pkg_t *pkg, const pkgconf_pkg_scan_providers_ctx_t *ctx)
{
	const pkgconf_dependency_t *pkgdep = ctx->pkgdep;
	pkgconf_node_t *node;

	PKGCONF_FOREACH_LIST_ENTRY(pkg->provides.head, node)
	{
		const pkgconf_dependency_t *provider = node->data;
		if (!strcmp(provider->package, pkgdep->package))
			return pkgconf_pkg_scan_provides_vercmp(pkgdep, provider);
	}

	return false;
}

/*
 * pkgconf_pkg_scan_providers(client, pkgdep, eflags)
 *
 * scan all available packages to see if a Provides rule matches the pkgdep.
 */
static pkgconf_pkg_t *
pkgconf_pkg_scan_providers(pkgconf_client_t *client, pkgconf_dependency_t *pkgdep, unsigned int *eflags)
{
	pkgconf_pkg_t *pkg;
	pkgconf_pkg_scan_providers_ctx_t ctx = {
		.pkgdep = pkgdep,
	};

	pkg = pkgconf_scan_all(client, &ctx, (pkgconf_pkg_iteration_func_t) pkgconf_pkg_scan_provides_entry);
	if (pkg != NULL)
	{
		pkgdep->match = pkgconf_pkg_ref(client, pkg);
		return pkg;
	}

	if (eflags != NULL)
		*eflags |= PKGCONF_PKG_ERRF_PACKAGE_NOT_FOUND;

	return NULL;
}

/*
 * !doc
 *
 * .. c:function:: pkgconf_pkg_t *pkgconf_pkg_verify_dependency(pkgconf_client_t *client, pkgconf_dependency_t *pkgdep, unsigned int *eflags)
 *
 *    Verify a pkgconf_dependency_t node in the depgraph.  If the dependency is solvable,
 *    return the appropriate ``pkgconf_pkg_t`` object, else ``NULL``.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to use for dependency resolution.
 *    :param pkgconf_dependency_t* pkgdep: The dependency graph node to solve.
 *    :param uint* eflags: An optional pointer that, if set, will be populated with an error code from the resolver.
 *    :return: On success, the appropriate ``pkgconf_pkg_t`` object to solve the dependency, else ``NULL``.
 *    :rtype: pkgconf_pkg_t *
 */
pkgconf_pkg_t *
pkgconf_pkg_verify_dependency(pkgconf_client_t *client, pkgconf_dependency_t *pkgdep, unsigned int *eflags)
{
	pkgconf_pkg_t *pkg = NULL;

	if (eflags != NULL)
		*eflags = PKGCONF_PKG_ERRF_OK;

	PKGCONF_TRACE(client, "trying to verify dependency: %s", pkgdep->package);

	if (pkgdep->match != NULL)
	{
		PKGCONF_TRACE(client, "cached dependency: %s -> %s@%p", pkgdep->package, pkgdep->match->id, pkgdep->match);
		return pkgconf_pkg_ref(client, pkgdep->match);
	}

	pkg = pkgconf_pkg_find(client, pkgdep->package);
	if (pkg == NULL)
	{
		if (client->flags & PKGCONF_PKG_PKGF_SKIP_PROVIDES)
		{
			if (eflags != NULL)
				*eflags |= PKGCONF_PKG_ERRF_PACKAGE_NOT_FOUND;

			return NULL;
		}

		return pkgconf_pkg_scan_providers(client, pkgdep, eflags);
	}

	if (pkg->id == NULL)
		pkg->id = strdup(pkgdep->package);

	if (pkgconf_pkg_comparator_impls[pkgdep->compare](pkg->version, pkgdep->version) != true)
	{
		if (eflags != NULL)
			*eflags |= PKGCONF_PKG_ERRF_PACKAGE_VER_MISMATCH;
	}
	else
		pkgdep->match = pkgconf_pkg_ref(client, pkg);

	return pkg;
}

/*
 * !doc
 *
 * .. c:function:: unsigned int pkgconf_pkg_verify_graph(pkgconf_client_t *client, pkgconf_pkg_t *root, int depth)
 *
 *    Verify the graph dependency nodes are satisfiable by walking the tree using
 *    ``pkgconf_pkg_traverse()``.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to use for dependency resolution.
 *    :param pkgconf_pkg_t* root: The root entry in the package dependency graph which should contain the top-level dependencies to resolve.
 *    :param int depth: The maximum allowed depth for dependency resolution.
 *    :return: On success, ``PKGCONF_PKG_ERRF_OK`` (0), else an error code.
 *    :rtype: unsigned int
 */
unsigned int
pkgconf_pkg_verify_graph(pkgconf_client_t *client, pkgconf_pkg_t *root, int depth)
{
	return pkgconf_pkg_traverse(client, root, NULL, NULL, depth);
}

static unsigned int
pkgconf_pkg_report_graph_error(pkgconf_client_t *client, pkgconf_pkg_t *parent, pkgconf_pkg_t *pkg, pkgconf_dependency_t *node, unsigned int eflags)
{
	if (eflags & PKGCONF_PKG_ERRF_PACKAGE_NOT_FOUND)
	{
		if (!(client->flags & PKGCONF_PKG_PKGF_SIMPLIFY_ERRORS) & !client->already_sent_notice)
		{
			pkgconf_error(client, "Package %s was not found in the pkg-config search path.\n", node->package);
			pkgconf_error(client, "Perhaps you should add the directory containing `%s.pc'\n", node->package);
			pkgconf_error(client, "to the PKG_CONFIG_PATH environment variable\n");
			client->already_sent_notice = true;
		}

		pkgconf_error(client, "Package '%s', required by '%s', not found\n", node->package, parent->id);
		pkgconf_audit_log(client, "%s NOT-FOUND\n", node->package);
	}
	else if (eflags & PKGCONF_PKG_ERRF_PACKAGE_VER_MISMATCH)
	{
		pkgconf_error(client, "Package dependency requirement '%s %s %s' could not be satisfied.\n",
			node->package, pkgconf_pkg_get_comparator(node), node->version);

		if (pkg != NULL)
			pkgconf_error(client, "Package '%s' has version '%s', required version is '%s %s'\n",
				node->package, pkg->version, pkgconf_pkg_get_comparator(node), node->version);
	}

	if (pkg != NULL)
		pkgconf_pkg_unref(client, pkg);

	return eflags;
}

static inline unsigned int
pkgconf_pkg_walk_list(pkgconf_client_t *client,
	pkgconf_pkg_t *parent,
	pkgconf_list_t *deplist,
	pkgconf_pkg_traverse_func_t func,
	void *data,
	int depth)
{
	unsigned int eflags = PKGCONF_PKG_ERRF_OK;
	pkgconf_node_t *node;

	PKGCONF_FOREACH_LIST_ENTRY(deplist->head, node)
	{
		unsigned int eflags_local = PKGCONF_PKG_ERRF_OK;
		pkgconf_dependency_t *depnode = node->data;
		pkgconf_pkg_t *pkgdep;

		if (*depnode->package == '\0')
			continue;

		pkgdep = pkgconf_pkg_verify_dependency(client, depnode, &eflags_local);

		eflags |= eflags_local;
		if (eflags_local != PKGCONF_PKG_ERRF_OK && !(client->flags & PKGCONF_PKG_PKGF_SKIP_ERRORS))
		{
			pkgconf_pkg_report_graph_error(client, parent, pkgdep, depnode, eflags_local);
			continue;
		}
		if (pkgdep == NULL)
			continue;

		if (pkgdep->flags & PKGCONF_PKG_PROPF_SEEN)
		{
			pkgconf_pkg_unref(client, pkgdep);
			continue;
		}

		pkgconf_audit_log_dependency(client, pkgdep, depnode);

		pkgdep->flags |= PKGCONF_PKG_PROPF_SEEN;
		eflags |= pkgconf_pkg_traverse(client, pkgdep, func, data, depth - 1);
		pkgdep->flags &= ~PKGCONF_PKG_PROPF_SEEN;
		pkgconf_pkg_unref(client, pkgdep);
	}

	return eflags;
}

static inline unsigned int
pkgconf_pkg_walk_conflicts_list(pkgconf_client_t *client,
	pkgconf_pkg_t *root, pkgconf_list_t *deplist)
{
	unsigned int eflags;
	pkgconf_node_t *node, *childnode;

	PKGCONF_FOREACH_LIST_ENTRY(deplist->head, node)
	{
		pkgconf_dependency_t *parentnode = node->data;

		if (*parentnode->package == '\0')
			continue;

		PKGCONF_FOREACH_LIST_ENTRY(root->required.head, childnode)
		{
			pkgconf_pkg_t *pkgdep;
			pkgconf_dependency_t *depnode = childnode->data;

			if (*depnode->package == '\0' || strcmp(depnode->package, parentnode->package))
				continue;

			pkgdep = pkgconf_pkg_verify_dependency(client, parentnode, &eflags);
			if (eflags == PKGCONF_PKG_ERRF_OK)
			{
				pkgconf_error(client, "Version '%s' of '%s' conflicts with '%s' due to satisfying conflict rule '%s %s%s%s'.\n",
					pkgdep->version, pkgdep->realname, root->realname, parentnode->package, pkgconf_pkg_get_comparator(parentnode),
					parentnode->version != NULL ? " " : "", parentnode->version != NULL ? parentnode->version : "");

				if (!(client->flags & PKGCONF_PKG_PKGF_SIMPLIFY_ERRORS))
				{
					pkgconf_error(client, "It may be possible to ignore this conflict and continue, try the\n");
					pkgconf_error(client, "PKG_CONFIG_IGNORE_CONFLICTS environment variable.\n");
				}

				pkgconf_pkg_unref(client, pkgdep);

				return PKGCONF_PKG_ERRF_PACKAGE_CONFLICT;
			}

			pkgconf_pkg_unref(client, pkgdep);
		}
	}

	return PKGCONF_PKG_ERRF_OK;
}

/*
 * !doc
 *
 * .. c:function:: unsigned int pkgconf_pkg_traverse(pkgconf_client_t *client, pkgconf_pkg_t *root, pkgconf_pkg_traverse_func_t func, void *data, int maxdepth)
 *
 *    Walk and resolve the dependency graph up to `maxdepth` levels.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to use for dependency resolution.
 *    :param pkgconf_pkg_t* root: The root of the dependency graph.
 *    :param pkgconf_pkg_traverse_func_t func: A traversal function to call for each resolved node in the dependency graph.
 *    :param void* data: An opaque pointer to data to be passed to the traversal function.
 *    :param int maxdepth: The maximum depth to walk the dependency graph for.  -1 means infinite recursion.
 *    :return: ``PKGCONF_PKG_ERRF_OK`` on success, else an error code.
 *    :rtype: unsigned int
 */
unsigned int
pkgconf_pkg_traverse(pkgconf_client_t *client,
	pkgconf_pkg_t *root,
	pkgconf_pkg_traverse_func_t func,
	void *data,
	int maxdepth)
{
	unsigned int eflags = PKGCONF_PKG_ERRF_OK;

	if (maxdepth == 0)
		return eflags;

	PKGCONF_TRACE(client, "%s: level %d", root->id, maxdepth);

	if ((root->flags & PKGCONF_PKG_PROPF_VIRTUAL) != PKGCONF_PKG_PROPF_VIRTUAL || (client->flags & PKGCONF_PKG_PKGF_SKIP_ROOT_VIRTUAL) != PKGCONF_PKG_PKGF_SKIP_ROOT_VIRTUAL)
	{
		if (func != NULL)
			func(client, root, data);
	}

	if (!(client->flags & PKGCONF_PKG_PKGF_SKIP_CONFLICTS))
	{
		eflags = pkgconf_pkg_walk_conflicts_list(client, root, &root->conflicts);
		if (eflags != PKGCONF_PKG_ERRF_OK)
			return eflags;
	}

	PKGCONF_TRACE(client, "%s: walking requires list", root->id);
	eflags = pkgconf_pkg_walk_list(client, root, &root->required, func, data, maxdepth);
	if (eflags != PKGCONF_PKG_ERRF_OK)
		return eflags;

	if (client->flags & PKGCONF_PKG_PKGF_SEARCH_PRIVATE)
	{
		PKGCONF_TRACE(client, "%s: walking requires.private list", root->id);

		/* XXX: ugly */
		client->flags |= PKGCONF_PKG_PKGF_ITER_PKG_IS_PRIVATE;
		eflags = pkgconf_pkg_walk_list(client, root, &root->requires_private, func, data, maxdepth);
		client->flags &= ~PKGCONF_PKG_PKGF_ITER_PKG_IS_PRIVATE;

		if (eflags != PKGCONF_PKG_ERRF_OK)
			return eflags;
	}

	return eflags;
}

static void
pkgconf_pkg_cflags_collect(pkgconf_client_t *client, pkgconf_pkg_t *pkg, void *data)
{
	pkgconf_list_t *list = data;
	pkgconf_node_t *node;

	PKGCONF_FOREACH_LIST_ENTRY(pkg->cflags.head, node)
	{
		pkgconf_fragment_t *frag = node->data;
		pkgconf_fragment_copy(client, list, frag, false);
	}
}

static void
pkgconf_pkg_cflags_private_collect(pkgconf_client_t *client, pkgconf_pkg_t *pkg, void *data)
{
	pkgconf_list_t *list = data;
	pkgconf_node_t *node;

	PKGCONF_FOREACH_LIST_ENTRY(pkg->cflags_private.head, node)
	{
		pkgconf_fragment_t *frag = node->data;
		pkgconf_fragment_copy(client, list, frag, true);
	}
}

/*
 * !doc
 *
 * .. c:function:: int pkgconf_pkg_cflags(pkgconf_client_t *client, pkgconf_pkg_t *root, pkgconf_list_t *list, int maxdepth)
 *
 *    Walks a dependency graph and extracts relevant ``CFLAGS`` fragments.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to use for dependency resolution.
 *    :param pkgconf_pkg_t* root: The root of the dependency graph.
 *    :param pkgconf_list_t* list: The fragment list to add the extracted ``CFLAGS`` fragments to.
 *    :param int maxdepth: The maximum allowed depth for dependency resolution.  -1 means infinite recursion.
 *    :return: ``PKGCONF_PKG_ERRF_OK`` if successful, otherwise an error code.
 *    :rtype: unsigned int
 */
unsigned int
pkgconf_pkg_cflags(pkgconf_client_t *client, pkgconf_pkg_t *root, pkgconf_list_t *list, int maxdepth)
{
	unsigned int eflag;

	eflag = pkgconf_pkg_traverse(client, root, pkgconf_pkg_cflags_collect, list, maxdepth);
	if (eflag != PKGCONF_PKG_ERRF_OK)
		pkgconf_fragment_free(list);

	if (client->flags & PKGCONF_PKG_PKGF_MERGE_PRIVATE_FRAGMENTS)
	{
		eflag = pkgconf_pkg_traverse(client, root, pkgconf_pkg_cflags_private_collect, list, maxdepth);
		if (eflag != PKGCONF_PKG_ERRF_OK)
			pkgconf_fragment_free(list);
	}

	return eflag;
}

static void
pkgconf_pkg_libs_collect(pkgconf_client_t *client, pkgconf_pkg_t *pkg, void *data)
{
	pkgconf_list_t *list = data;
	pkgconf_node_t *node;

	PKGCONF_FOREACH_LIST_ENTRY(pkg->libs.head, node)
	{
		pkgconf_fragment_t *frag = node->data;
		pkgconf_fragment_copy(client, list, frag, (client->flags & PKGCONF_PKG_PKGF_ITER_PKG_IS_PRIVATE) != 0);
	}

	if (client->flags & PKGCONF_PKG_PKGF_MERGE_PRIVATE_FRAGMENTS)
	{
		PKGCONF_FOREACH_LIST_ENTRY(pkg->libs_private.head, node)
		{
			pkgconf_fragment_t *frag = node->data;
			pkgconf_fragment_copy(client, list, frag, true);
		}
	}
}

/*
 * !doc
 *
 * .. c:function:: int pkgconf_pkg_libs(pkgconf_client_t *client, pkgconf_pkg_t *root, pkgconf_list_t *list, int maxdepth)
 *
 *    Walks a dependency graph and extracts relevant ``LIBS`` fragments.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to use for dependency resolution.
 *    :param pkgconf_pkg_t* root: The root of the dependency graph.
 *    :param pkgconf_list_t* list: The fragment list to add the extracted ``LIBS`` fragments to.
 *    :param int maxdepth: The maximum allowed depth for dependency resolution.  -1 means infinite recursion.
 *    :return: ``PKGCONF_PKG_ERRF_OK`` if successful, otherwise an error code.
 *    :rtype: unsigned int
 */
unsigned int
pkgconf_pkg_libs(pkgconf_client_t *client, pkgconf_pkg_t *root, pkgconf_list_t *list, int maxdepth)
{
	unsigned int eflag;

	eflag = pkgconf_pkg_traverse(client, root, pkgconf_pkg_libs_collect, list, maxdepth);

	if (eflag != PKGCONF_PKG_ERRF_OK)
	{
		pkgconf_fragment_free(list);
		return eflag;
	}

	return eflag;
}
