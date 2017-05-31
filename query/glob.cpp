/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "InMemoryView.h"
#include "make_unique.h"
#include "thirdparty/wildmatch/wildmatch.h"
#include "watchman.h"
#include "watchman_scopeguard.h"

using watchman::CaseSensitivity;

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

watchman_glob_tree::watchman_glob_tree(
    const char* pattern,
    uint32_t pattern_len)
    : pattern(pattern, pattern_len),
      is_leaf(0),
      had_specials(0),
      is_doublestar(0) {}

// Simple brute force lookup of pattern within a node.
// This is run at compile time and most glob sets are low enough cardinality
// that this doesn't turn out to be a hot spot in practice.
static watchman_glob_tree* lookup_node_child(
    std::vector<std::unique_ptr<watchman_glob_tree>>* vec,
    const char* pattern,
    uint32_t pattern_len) {
  for (auto& kid : *vec) {
    if (kid->pattern.size() == pattern_len &&
        memcmp(kid->pattern.data(), pattern, pattern_len) == 0) {
      return kid.get();
    }
  }
  return nullptr;
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
    auto* container = &parent->children;

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
      container->emplace_back(watchman::make_unique<watchman_glob_tree>(
          pattern, (uint32_t)(end - pattern)));
      node = container->back().get();
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

void parse_globs(w_query* res, const json_ref& query) {
  size_t i;
  int noescape = 0;
  int includedotfiles = 0;

  auto globs = query.get_default("glob");
  if (!globs) {
    return;
  }

  if (!json_is_array(globs)) {
    throw QueryParseError("'glob' must be an array");
  }

  // Globs implicitly enable dedup_results mode
  res->dedup_results = true;

  if (json_unpack(query, "{s?b}", "glob_noescape", &noescape) != 0) {
    throw QueryParseError("glob_noescape must be a boolean");
  }

  if (json_unpack(query, "{s?b}", "glob_includedotfiles", &includedotfiles) !=
      0) {
    throw QueryParseError("glob_includedotfiles must be a boolean");
  }

  res->glob_flags =
      (includedotfiles ? 0 : WM_PERIOD) | (noescape ? WM_NOESCAPE : 0);

  res->glob_tree = watchman::make_unique<watchman_glob_tree>("", 0);
  for (i = 0; i < json_array_size(globs); i++) {
    const auto& ele = globs.at(i);
    const auto& pattern = json_to_w_string(ele);

    if (!add_glob(res->glob_tree.get(), pattern)) {
      throw QueryParseError("failed to compile multi-glob");
    }
  }
}

/** Concatenate dir_name and name around a unix style directory
 * separator.
 * dir_name may be NULL in which case this returns a copy of name.
 */
static inline std::string make_path_name(
    const char* dir_name,
    uint32_t dlen,
    const char* name,
    uint32_t nlen) {
  std::string result;
  result.reserve(dlen + nlen + 1);

  if (dlen) {
    result.append(dir_name, dlen);
    // wildmatch wants unix separators
    result.push_back('/');
  }
  result.append(name, nlen);
  return result;
}

namespace watchman {
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
void InMemoryView::globGeneratorDoublestar(
    struct w_query_ctx* ctx,
    const struct watchman_dir* dir,
    const struct watchman_glob_tree* node,
    const char* dir_name,
    uint32_t dir_name_len) const {
  bool matched;

  // First step is to walk the set of files contained in this node
  for (auto& it : dir->files) {
    auto file = it.second.get();
    auto file_name = file->getName();

    ctx->bumpNumWalked();

    if (!file->exists) {
      // Globs can only match files that exist
      continue;
    }

    auto subject = make_path_name(
        dir_name, dir_name_len, file_name.data(), file_name.size());

    // Now that we have computed the name of this candidate file node,
    // attempt to match against each of the possible doublestar patterns
    // in turn.  As soon as any one of them matches we can stop this loop
    // as it doesn't make a lot of sense to yield multiple results for
    // the same file.
    for (const auto& child_node : node->doublestar_children) {
      matched = wildmatch(child_node->pattern.c_str(), subject.c_str(),
                          ctx->query->glob_flags | WM_PATHNAME |
                              (ctx->query->case_sensitive ==
                                       CaseSensitivity::CaseSensitive
                                   ? 0
                                   : WM_CASEFOLD),
                          0) == WM_MATCH;

      if (matched) {
        w_query_process_file(
            ctx->query,
            ctx,
            make_unique<InMemoryFileResult>(file, contentHashCache_));
        // No sense running multiple matches for this same file node
        // if this one succeeded.
        break;
      }
    }
  }

  // And now walk down to any dirs; all dirs are eligible
  for (auto& it : dir->dirs) {
    const auto child = it.second.get();

    if (!child->last_check_existed) {
      // Globs can only match files in dirs that exist
      continue;
    }

    auto subject = make_path_name(
        dir_name, dir_name_len, child->name.data(), child->name.size());
    globGeneratorDoublestar(ctx, child, node, subject.data(), subject.size());
  }
}

/* Match each child of node against the children of dir */
void InMemoryView::globGeneratorTree(
    struct w_query_ctx* ctx,
    const struct watchman_glob_tree* node,
    const struct watchman_dir* dir) const {
  w_string_t component;

  if (!node->doublestar_children.empty()) {
    globGeneratorDoublestar(ctx, dir, node, nullptr, 0);
  }

  for (const auto& child_node : node->children) {
    w_assert(!child_node->is_doublestar, "should not get here with ** glob");

    // If there are child dirs, consider them for recursion.
    // Note that we don't restrict this to !leaf because the user may have
    // set their globs list to something like ["some_dir", "some_dir/file"]
    // and we don't want to preclude matching the latter.
    if (!dir->dirs.empty()) {
      // Attempt direct lookup if possible
      if (!child_node->had_specials &&
          ctx->query->case_sensitive == CaseSensitivity::CaseSensitive) {
        w_string_new_len_typed_stack(
            &component,
            child_node->pattern.data(),
            child_node->pattern.size(),
            W_STRING_BYTE);
        const auto child_dir = dir->getChildDir(&component);

        if (child_dir) {
          globGeneratorTree(ctx, child_node.get(), child_dir);
        }
      } else {
        // Otherwise we have to walk and match
        for (auto& it : dir->dirs) {
          const auto child_dir = it.second.get();

          if (!child_dir->last_check_existed) {
            // Globs can only match files in dirs that exist
            continue;
          }

          if (wildmatch(child_node->pattern.c_str(), child_dir->name.c_str(),
                        ctx->query->glob_flags |
                            (ctx->query->case_sensitive ==
                                     CaseSensitivity::CaseSensitive
                                 ? 0
                                 : WM_CASEFOLD),
                        0) == WM_MATCH) {
            globGeneratorTree(ctx, child_node.get(), child_dir);
          }
        }
      }
    }

    // If the node is a leaf we are in a position to match files.
    if (child_node->is_leaf && !dir->files.empty()) {
      // Attempt direct lookup if possible
      if (!child_node->had_specials &&
          ctx->query->case_sensitive == CaseSensitivity::CaseSensitive) {
        w_string_new_len_typed_stack(
            &component,
            child_node->pattern.data(),
            child_node->pattern.size(),
            W_STRING_BYTE);
        auto file = dir->getChildFile(&component);

        if (file) {
          ctx->bumpNumWalked();
          if (file->exists) {
            // Globs can only match files that exist
            w_query_process_file(
                ctx->query,
                ctx,
                make_unique<InMemoryFileResult>(file, contentHashCache_));
          }
        }
      } else {
        for (auto& it : dir->files) {
          // Otherwise we have to walk and match
          auto file = it.second.get();
          auto file_name = file->getName();
          ctx->bumpNumWalked();

          if (!file->exists) {
            // Globs can only match files that exist
            continue;
          }

          if (wildmatch(child_node->pattern.c_str(), file_name.data(),
                        ctx->query->glob_flags |
                            (ctx->query->case_sensitive ==
                                     CaseSensitivity::CaseSensitive
                                 ? WM_CASEFOLD
                                 : 0),
                        0) == WM_MATCH) {
            w_query_process_file(
                ctx->query,
                ctx,
                make_unique<InMemoryFileResult>(file, contentHashCache_));
          }
        }
      }
    }
  }
}

void InMemoryView::globGenerator(w_query* query, struct w_query_ctx* ctx)
    const {
  w_string_t *relative_root;

  if (query->relative_root) {
    relative_root = query->relative_root;
  } else {
    relative_root = root_path;
  }

  auto view = view_.rlock();

  const auto dir = resolveDir(view, relative_root);
  if (!dir) {
    throw QueryExecError(watchman::to<std::string>(
        "glob_generator could not resolve ",
        w_string_piece(relative_root),
        ", check your "
        "relative_root parameter!"));
  }

  globGeneratorTree(ctx, query->glob_tree.get(), dir);
}
}

/* vim:ts=2:sw=2:et:
 */
