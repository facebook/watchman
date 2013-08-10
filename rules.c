/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* These are the legacy rules functions.
 * They're going to be replaced by equivalent functionality
 * in the query engine */

void w_free_rules(struct watchman_rule *head)
{
  struct watchman_rule *r;

  while (head) {
    r = head;
    head = r->next;

#ifdef HAVE_PCRE_H
    if (r->re) {
      pcre_free(r->re);
    }
    if (r->re_extra) {
      pcre_free(r->re_extra);
    }
#endif
    free((char*)r->pattern);
    free(r);
  }
}

static struct watchman_rule_match *add_match(
    struct watchman_rule_match **results_ptr,
    uint32_t *num_allocd_ptr,
    uint32_t *num_matches_ptr)
{
  uint32_t num_allocd = *num_allocd_ptr;
  uint32_t num_matches = *num_matches_ptr;
  struct watchman_rule_match *results = *results_ptr;
  struct watchman_rule_match *m;

  if (num_matches + 1 > num_allocd) {
    num_allocd = num_allocd ? num_allocd * 2 : 64;
    results = realloc(results,
        num_allocd * sizeof(struct watchman_rule_match));
    if (!results) {
      w_log(W_LOG_DBG, "out of memory while running rules!\n");
      return false;
    }
    *results_ptr = results;
    *num_allocd_ptr = num_allocd;
  }

  m = &results[num_matches++];
  *num_matches_ptr = num_matches;

  return m;
}

// must be called with root locked
uint32_t w_rules_match(w_root_t *root,
    struct watchman_file *oldest_file,
    struct watchman_rule_match **results,
    struct watchman_rule *head,
    struct w_clockspec *spec)
{
  struct watchman_file *file;
  struct watchman_rule *rule;
  w_string_t *full_name;
  w_string_t *relname;
  uint32_t num_matches = 0;
  uint32_t name_start;
  struct watchman_rule_match *res = NULL;
  uint32_t num_allocd = 0;
  struct w_query_since since;

  w_clockspec_eval(root, spec, &since);

  name_start = root->root_path->len + 1;

  for (file = oldest_file; file; file = file->prev) {
    // no rules means return everything
    bool matched = (head == NULL) ? true : false;

    full_name = w_string_path_cat(file->parent->path, file->name);
    // Record the name relative to the root
    relname = w_string_slice(full_name, name_start,
                full_name->len - name_start);
    w_string_delref(full_name);

    // Work through the rules; we stop as soon as we get a match.
    for (rule = head; rule && !matched; rule = rule->next) {

      // In theory, relname->buf may not be NUL terminated in
      // the right spot if it was created as a slice.
      // In practice, we don't see those, but if we do, we should
      // probably make a copy of the string into a stack buffer :-/

      switch (rule->rule_type) {
        case RT_FNMATCH:
          matched = fnmatch(rule->pattern, relname->buf, rule->flags) == 0;
          break;
        case RT_PCRE:
#ifdef HAVE_PCRE_H
          {
            int rc = pcre_exec(rule->re, rule->re_extra, relname->buf,
                relname->len, 0, 0, NULL, 0);

            if (rc == PCRE_ERROR_NOMATCH) {
              matched = false;
            } else if (rc >= 0) {
              matched = true;
            } else {
              w_log(W_LOG_ERR, "pcre match %s against %s failed: %d\n",
                  rule->pattern, relname->buf, rc);
              matched = false;
            }
          }
#else
          matched = false;
#endif
          break;
      }

      // If the rule is negated, we negate the sense of the
      // match result
      if (rule->negated) {
        matched = !matched;
      }

      // If the pattern matched then we're going to include the file
      // in our result set, but only if it is set to include.
      // If we're not including, we explicitly don't want to know
      // about the file, so pretend it didn't match and stop processing
      // rules for the file.
      if (matched && !rule->include) {
        matched = false;
        break;
      }
    }

    if (matched) {
      struct watchman_rule_match *m;

      m = add_match(&res, &num_allocd, &num_matches);

      if (!m) {
        w_log(W_LOG_ERR, "out of memory while running rules!\n");
        w_string_delref(relname);
        free(res);
        return 0;
      }

      m->root_number = root->number;
      m->relname = relname;
      m->file = file;
      if (since.is_timestamp) {
        m->is_new = w_timeval_compare(since.timestamp, file->ctime.tv) > 0;
      } else if (since.clock.is_fresh_instance) {
        m->is_new = true;
      } else {
        m->is_new = file->ctime.ticks > since.clock.ticks;
      }
    } else {
      w_string_delref(relname);
    }
  }

  *results = res;

  return num_matches;
}

void w_match_results_free(uint32_t num_matches,
    struct watchman_rule_match *matches)
{
  uint32_t i;

  for (i = 0; i < num_matches; i++) {
    w_string_delref(matches[i].relname);
  }
  free(matches);
}

/* Parses filename match rules.
 * By default, we want to include items that positively match
 * the set of fnmatch(3) patterns specified.
 * If -X is specified, we switch to exclude mode; any patterns
 * that are encountered after -X are excluded from the result set.
 * If -I is specified, we switch to include mode, so you can use
 * -I to turn on include mode again after using -X.
 * If "!" is specified, the following pattern is negated.
 * We switch back out of negation mode after that pattern.
 * If -p is specified, the following pattern is interpreted as a PCRE.
 * If -P is specified, the following pattern is interpreted as a PCRE
 * with the PCRE_CASELESS flag set.
 *
 * We stop processing args when we find "--" and update
 * *next_arg to the argv index after that argument.
 */
bool parse_watch_params(int start, json_t *args,
    struct watchman_rule **head_ptr,
    uint32_t *next_arg,
    char *errbuf, int errbuflen)
{
  bool include = true;
  bool negated = false;
  int is_pcre = 0;
  struct watchman_rule *rule, *prior = NULL;
  uint32_t i;

  if (!json_is_array(args)) {
    return false;
  }
  *head_ptr = NULL;

  for (i = start; i < json_array_size(args); i++) {
    const char *arg = json_string_value(json_array_get(args, i));
    if (!arg) {
      /* not a string value! */
      w_free_rules(*head_ptr);
      *head_ptr = NULL;
      snprintf(errbuf, errbuflen,
          "rule @ position %d is not a string value", i);
      return false;
    }

    if (!strcmp(arg, "--")) {
      i++;
      break;
    }
    if (!strcmp(arg, "-X")) {
      include = false;
      continue;
    }
    if (!strcmp(arg, "-I")) {
      include = true;
      continue;
    }
    if (!strcmp(arg, "!")) {
      negated = true;
      continue;
    }
    if (!strcmp(arg, "-P") || !strcmp(arg, "-p")) {
#ifdef HAVE_PCRE_H
      is_pcre = arg[1];
      continue;
#else
      snprintf(errbuf, errbuflen,
          "this watchman was not built with pcre support");
      return false;
#endif
    }

    rule = calloc(1, sizeof(*rule));
    if (!rule) {
      w_free_rules(*head_ptr);
      *head_ptr = NULL;
      snprintf(errbuf, errbuflen, "out of memory");
      return false;
    }

    rule->include = include;
    rule->negated = negated;
    // We default the fnmatch so that we can match against paths that include
    // slashes.
    // To recursively match the contents of a dir, use "dir/*".  To match all
    // "C" source files, use "*.c".  To match all makefiles, use
    // "*/Makefile" + "Makefile" (include the latter if the Makefile might
    // be at the top level).
    rule->rule_type = RT_FNMATCH;
    rule->pattern = strdup(arg);
    rule->flags = FNM_PERIOD;

#ifdef HAVE_PCRE_H
    if (is_pcre) {
      const char *errptr = NULL;
      int erroff = 0;
      int errcode = 0;

      rule->re = pcre_compile2(rule->pattern,
          is_pcre == 'P' ? PCRE_CASELESS : 0,
          &errcode, &errptr, &erroff, NULL);

      if (!rule->re) {
        snprintf(errbuf, errbuflen,
          "invalid pcre: `%s' at offset %d: code %d %s",
          rule->pattern, erroff, errcode, errptr);
        w_free_rules(rule);
        w_free_rules(*head_ptr);
        *head_ptr = NULL;
        return false;
      }

      if (rule->re) {
        rule->re_extra = pcre_study(rule->re, 0, &errptr);
      }
      rule->rule_type = RT_PCRE;
    }
#endif

    if (!prior) {
      *head_ptr = rule;
    } else {
      prior->next = rule;
    }
    prior = rule;

    // Reset negated flag
    negated = false;
    is_pcre = 0;
  }

  if (next_arg) {
    *next_arg = i;
  }

  return true;
}



/* vim:ts=2:sw=2:et:
 */

