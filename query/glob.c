/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "thirdparty/wildmatch/wildmatch.h"

/* The glob generator.
 * The user can specify a list of globs as the set of candidate nodes
 * for their query expression.
 * The list may feature redundant components that we desire to avoid
 * matching more times than we need.
 * For example ["some/deep/path/foo.h", "some/deep/path/bar.h"] have
 * a common path prefix that we only want to match once.
 *
 * To deal with this we compile the set of glob patterns into a tree
 * structure, splitting the pattern by the unix directory separator.
 *
 * At execution time we walk down the watchman_dir tree and the pattern
 * tree concurrently.  If the watchman_dir tree has no matching component
 * then we can terminate evaluation of that portion of the pattern tree
 * early.
 */

W_CAP_REG("glob_generator")

// Holds a list of glob matching rules
struct glob_child_vec {
  struct watchman_glob_tree **children;
  uint32_t num_children;
  uint32_t num_children_allocd;
};

// A node in the tree of node matching rules
struct watchman_glob_tree {
  char *pattern;
  uint32_t pattern_len;

  // The list of child rules, excluding any ** rules
  struct glob_child_vec children;
  // The list of ** rules that exist under this node
  struct glob_child_vec doublestar_children;

  unsigned is_leaf:1;       // if true, generate files for matches
  unsigned had_specials:1;  // if false, can do simple string compare
  unsigned is_doublestar:1; // pattern begins with **
};

// Look ahead in pattern; we want to find the directory separator.
// While we are looking, check for wildmatch special characters.
// If we do not find a directory separator, return NULL.
static inline const char *find_sep_and_specials(const char *pattern,
                                                const char *end,
                                                bool *had_specials) {
  *had_specials = false;
  while (pattern < end) {
    switch (*pattern) {
      case '*':
      case '?':
      case '[':
      case '\\':
        *had_specials = true;
        break;
      case '/':
        return pattern;
    }
    ++pattern;
  }
  // No separator found
  return NULL;
}

static void destroy_glob_child_vec(struct glob_child_vec *vec) {
  uint32_t i;
  for (i = 0; i < vec->num_children; ++i) {
    free_glob_tree(vec->children[i]);
  }
  free(vec->children);
}

void free_glob_tree(struct watchman_glob_tree *node) {

  if (!node) {
    return;
  }
  destroy_glob_child_vec(&node->children);
  destroy_glob_child_vec(&node->doublestar_children);

  free(node);
}

static struct watchman_glob_tree *make_node(const char *pattern,
                                            uint32_t pattern_len) {
  struct watchman_glob_tree *node;

  node = calloc(1, sizeof(*node) + pattern_len + 1);
  if (!node) {
    return NULL;
  }

  node->pattern = (char*)(node + 1);
  memcpy(node->pattern, pattern, pattern_len);
  node->pattern[pattern_len] = '\0';

  node->pattern_len = pattern_len;

  return node;
}

// Simple brute force lookup of pattern within a node.
// This is run at compile time and most glob sets are low enough cardinality
// that this doesn't turn out to be a hot spot in practice.
static struct watchman_glob_tree *lookup_node_child(struct glob_child_vec *vec,
                                                    const char *pattern,
                                                    uint32_t pattern_len) {
  uint32_t i;

  for (i = 0; i < vec->num_children; ++i) {
    if (vec->children[i]->pattern_len == pattern_len &&
        memcmp(vec->children[i]->pattern, pattern, pattern_len) == 0) {
      return vec->children[i];
    }
  }
  return NULL;
}

// Add a child node to parent, allocating more space if needed.
static bool add_node(struct glob_child_vec *vec,
                     struct watchman_glob_tree *child) {
  if (!vec->children) {
    vec->num_children_allocd = 8;
    vec->children = calloc(vec->num_children_allocd, sizeof(*vec->children));
    if (!vec->children) {
      return false;
    }
  } else if (vec->num_children + 1 > vec->num_children_allocd) {
    struct watchman_glob_tree **bigger = realloc(
        vec->children, vec->num_children_allocd * 2 * sizeof(*vec->children));
    if (!bigger) {
      return false;
    }
    vec->num_children_allocd *= 2;
    vec->children = bigger;
  }

  vec->children[vec->num_children++] = child;
  return true;
}

// Compile and add a new glob pattern to the tree.
// Compilation splits a pattern into nodes, with one node for each directory
// separator separated path component.
static bool add_glob(struct watchman_glob_tree *tree, w_string_t *glob_str) {
  struct watchman_glob_tree *parent = tree;
  const char *pattern = glob_str->buf;
  const char *pattern_end = pattern + glob_str->len;
  bool had_specials;

  while (pattern < pattern_end) {
    const char *sep =
        find_sep_and_specials(pattern, pattern_end, &had_specials);
    const char *end;
    struct watchman_glob_tree *node;
    bool is_doublestar = false;
    struct glob_child_vec *container = &parent->children;

    end = sep ? sep : pattern_end;

    // If a node uses double-star (recursive glob) then we take the remainder
    // of the pattern string, regardless of whether we found a separator or
    // not, because the ** forces us to walk the entire sub-tree and try the
    // match for every possible node.
    if (had_specials && end - pattern >= 2 && pattern[0] == '*' &&
        pattern[1] == '*') {
      end = pattern_end;
      is_doublestar = true;

      // Queue this up for the doublestar code path
      container = &parent->doublestar_children;
    }

    // If we can re-use an existing node, we just saved ourselves from a
    // redundant match at execution time!
    node = lookup_node_child(container, pattern, (uint32_t)(end - pattern));
    if (!node) {
      // This is a new matching possibility.
      node = make_node(pattern, (uint32_t)(end - pattern));
      if (!node) {
        return false;
      }
      if (!add_node(container, node)) {
        return false;
      }
      node->had_specials = had_specials;
      node->is_doublestar = is_doublestar;
    }

    // If we didn't find a separator in the remainder of this pattern, it
    // means that we expect it to be able to match files (it is therefore the
    // "leaf" of the pattern path).  Remember that fact as it can help us avoid
    // matching files when the pattern can only match dirs.
    if (!sep) {
      node->is_leaf = true;
    }

    pattern = end + 1; // skip separator
    parent = node;     // the next iteration uses this node as its parent
  }

  return true;
}

bool parse_globs(w_query *res, json_t *query)
{
  json_t *globs;
  size_t i;
  int noescape = 0;
  int includedotfiles = 0;

  globs = json_object_get(query, "glob");
  if (!globs) {
    return true;
  }

  if (!json_is_array(globs)) {
    res->errmsg = strdup("'glob' must be an array");
    return false;
  }

  // Globs implicitly enable dedup_results mode
  res->dedup_results = true;

  if (json_unpack(query, "{s?b}", "glob_noescape", &noescape) != 0) {
    res->errmsg = strdup("glob_noescape must be a boolean");
    return false;
  }

  if (json_unpack(query, "{s?b}", "glob_includedotfiles", &includedotfiles) !=
      0) {
    res->errmsg = strdup("glob_includedotfiles must be a boolean");
    return false;
  }

  res->glob_flags =
      (includedotfiles ? 0 : WM_PERIOD) | (noescape ? WM_NOESCAPE : 0);

  res->glob_tree = make_node("", 0);
  for (i = 0; i < json_array_size(globs); i++) {
    json_t *ele = json_array_get(globs, i);
    w_string_t *pattern = json_to_w_string(ele);

    if (!add_glob(res->glob_tree, pattern)) {
      res->errmsg = strdup("failed to compile multi-glob");
      return false;
    }
  }

  return true;
}

/** Concatenate dir_name and name around a unix style directory
 * separator.
 * dir_name may be NULL in which case this returns a copy of name.
 */
static inline char *make_path_name(const char *dir_name, uint32_t dlen,
                                   const char *name, uint32_t nlen) {
  char *result;

  if (dlen) {
    result = malloc(dlen + nlen + 2);
    if (!result) {
      return NULL;
    }
    memcpy(result, dir_name, dlen);
    result[dlen] = '/'; // wildmatch wants unix separators
    memcpy(result + dlen + 1, name, nlen);
    result[dlen + nlen + 1] = '\0';

    return result;
  }

  result = malloc(nlen + 1);
  if (!result) {
    return NULL;
  }
  memcpy(result, name, nlen);
  result[nlen] = '\0';
  return result;
}

/** This is our specialized handler for the ** recursive glob pattern.
 * This is the unhappy path because we have no choice but to recursively
 * walk the tree; we have no way to prune portions that won't match.
 * We do coalesce recursive matches together that might generate multiple
 * results.
 * For example: */
// globs: ["foo/**/*.h", "foo/**/**/*.h"]
/* effectively runs the same query multiple times.  By combining the
 * doublestar walk for both into a single walk, we can then match each
 * file against the list of patterns, terminating that match as soon
 * as any one of them matches the file node.
 */
static bool glob_generator_doublestar(struct w_query_ctx *ctx,
                                      int64_t *num_walked,
                                      struct read_locked_watchman_root *lock,
                                      const struct watchman_dir *dir,
                                      const struct watchman_glob_tree *node,
                                      const char *dir_name,
                                      uint32_t dir_name_len) {
  w_ht_iter_t i;
  int64_t n = 0;
  bool result = true;
  bool matched;
  char *subject;
  uint32_t j;

  // First step is to walk the set of files contained in this node
  if (w_ht_first(dir->files, &i)) do {
    struct watchman_file *file = w_ht_val_ptr(i.value);
    w_string_t *file_name = w_file_get_name(file);

    ++n;

    if (!file->exists) {
      // Globs can only match files that exist
      continue;
    }

    subject =
        make_path_name(dir_name, dir_name_len, file_name->buf, file_name->len);
    if (!subject) {
      result = false;
      goto done;
    }

    // Now that we have computed the name of this candidate file node,
    // attempt to match against each of the possible doublestar patterns
    // in turn.  As soon as any one of them matches we can stop this loop
    // as it doesn't make a lot of sense to yield multiple results for
    // the same file.
    for (j = 0; j < node->doublestar_children.num_children; ++j) {
      struct watchman_glob_tree *child_node =
          node->doublestar_children.children[j];

      matched = wildmatch(child_node->pattern, subject,
                          ctx->query->glob_flags | WM_PATHNAME |
                              (ctx->query->case_sensitive ? 0 : WM_CASEFOLD),
                          0) == WM_MATCH;

      if (matched) {
        if (!w_query_process_file(ctx->query, ctx, file)) {
          result = false;
          free(subject);
          goto done;
        }
        // No sense running multiple matches for this same file node
        // if this one succeeded.
        break;
      }
    }

    free(subject);
  } while (w_ht_next(dir->files, &i));

  // And now walk down to any dirs; all dirs are eligible
  if (w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = w_ht_val_ptr(i.value);
    int64_t child_walked = 0;

    if (!child->last_check_existed) {
      // Globs can only match files in dirs that exist
      continue;
    }

    subject = make_path_name(dir_name, dir_name_len, child->name->buf,
                             child->name->len);
    if (!subject) {
      result = false;
      goto done;
    }
    result = glob_generator_doublestar(ctx, &child_walked, lock, child, node,
                                       subject, strlen_uint32(subject));
    free(subject);
    n += child_walked;
    if (!result) {
      goto done;
    }
  } while (w_ht_next(dir->dirs, &i));

done:
  *num_walked = n;
  return result;
}

/* Match each child of node against the children of dir */
static bool glob_generator_tree(struct w_query_ctx *ctx, int64_t *num_walked,
                                struct read_locked_watchman_root *lock,
                                const struct watchman_glob_tree *node,
                                const struct watchman_dir *dir) {
  uint32_t i;
  w_string_t component;
  w_ht_iter_t iter;
  const struct watchman_dir *child_dir;
  bool result = true;
  int64_t n = 0;

  if (node->doublestar_children.num_children > 0) {
    int64_t child_walked = 0;
    result =
        glob_generator_doublestar(ctx, &child_walked, lock, dir, node, NULL, 0);
    n += child_walked;
    if (!result) {
      goto done;
    }
  }

  for (i = 0; i < node->children.num_children; ++i) {
    const struct watchman_glob_tree *child_node = node->children.children[i];

    w_assert(!child_node->is_doublestar, "should not get here with ** glob");

    // If there are child dirs, consider them for recursion.
    // Note that we don't restrict this to !leaf because the user may have
    // set their globs list to something like ["some_dir", "some_dir/file"]
    // and we don't want to preclude matching the latter.
    if (dir->dirs) {

      // Attempt direct lookup if possible
      if (!child_node->had_specials && ctx->query->case_sensitive) {
        w_string_new_len_typed_stack(&component, child_node->pattern,
            child_node->pattern_len, W_STRING_BYTE);
        child_dir = w_ht_val_ptr(w_ht_get(dir->dirs, w_ht_ptr_val(&component)));

        if (child_dir) {
          int64_t child_walked = 0;
          result = glob_generator_tree(ctx, &child_walked, lock, child_node,
              child_dir);
          n += child_walked;
          if (!result) {
            goto done;
          }
        }
      } else {
        // Otherwise we have to walk and match
        if (w_ht_first(dir->dirs, &iter)) do {
          child_dir = w_ht_val_ptr(iter.value);

          if (!child_dir->last_check_existed) {
            // Globs can only match files in dirs that exist
            continue;
          }

          if (wildmatch(child_node->pattern, child_dir->name->buf,
                        ctx->query->glob_flags |
                            (ctx->query->case_sensitive ? 0 : WM_CASEFOLD),
                        0) == WM_MATCH) {
            int64_t child_walked = 0;
            result = glob_generator_tree(ctx, &child_walked, lock, child_node,
                                         child_dir);
            n += child_walked;
            if (!result) {
              goto done;
            }
          }
        } while (w_ht_next(dir->dirs, &iter));
      }
    }

    // If the node is a leaf we are in a position to match files.
    if (child_node->is_leaf && dir->files) {
      // Attempt direct lookup if possible
      if (!child_node->had_specials && ctx->query->case_sensitive) {
        struct watchman_file *file;

        w_string_new_len_typed_stack(&component, child_node->pattern,
                                     child_node->pattern_len, W_STRING_BYTE);
        file = w_ht_val_ptr(w_ht_get(dir->files, w_ht_ptr_val(&component)));

        if (file) {
          ++n;
          if (file->exists) {
            // Globs can only match files that exist
            result = w_query_process_file(ctx->query, ctx, file);
            if (!result) {
              goto done;
            }
          }
        }
      } else if (w_ht_first(dir->files, &iter)) do {
        // Otherwise we have to walk and match
        struct watchman_file *file = w_ht_val_ptr(iter.value);
        w_string_t *file_name = w_file_get_name(file);
        ++n;

        if (!file->exists) {
          // Globs can only match files that exist
          continue;
        }

        if (wildmatch(child_node->pattern, file_name->buf,
                      ctx->query->glob_flags |
                          (ctx->query->case_sensitive ? WM_CASEFOLD : 0),
                      0) == WM_MATCH) {
          if (!w_query_process_file(ctx->query, ctx, file)) {
            result = false;
            goto done;
          }
        }
      } while (w_ht_next(dir->files, &iter));
    }
  }

done:
  *num_walked = n;
  return result;
}

bool glob_generator(w_query *query, struct read_locked_watchman_root *lock,
                    struct w_query_ctx *ctx, int64_t *num_walked) {
  w_string_t *relative_root;
  const struct watchman_dir *dir;

  if (query->relative_root != NULL) {
    relative_root = query->relative_root;
  } else {
    relative_root = lock->root->root_path;
  }
  dir = w_root_resolve_dir_read(lock, relative_root);

  return glob_generator_tree(ctx, num_walked, lock, query->glob_tree, dir);
}

/* vim:ts=2:sw=2:et:
 */
