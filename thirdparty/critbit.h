/*
 critbit - A crit-bit treemap implementation for strings

 Portions copyright 2013 Facebook.

 Derived from critbit89, released to the public domain by
 Jonas Gehring <jonas@jgehring.net>.
 */

/*
 A critical bit tree is an efficient data structure to store a prefix-free set
 of strings. Critbit trees branch out at differing ("critical") bits in
 strings. They consist of two sorts of nodes. _Internal_ nodes store the
 position of the next differing bit and two pointers to other nodes in the tree
 corresponding to the bit being 0 and 1, respectively, _External_ nodes are
 typically the strings themselves.

 For the set of bit strings 11010, 10100, and 11001, the tree will look like:

  <root> (bit 2) --0-- 10100
                 \
                  -1-- (bit 4) --0-- 11001
                               \
                                -1-- 11010

 Critical bit trees were invented by Daniel J. Bernstein in 2004: see
 <http://cr.yp.to/critbit.html> for his description.

 Critical bit trees are similar in principle to prefix trees, but prefix trees
 typically store each bit/character of each string in a separate node, causing
 much more pointer-chasing and memory use. Critical bit trees only branch on
 differing bits.

 Critical bit trees have several highly desirable properties:
 - the usual tree operations, including membership testing, insertion,
   deletion, and sorted traversal are all efficient.
 - prefix tree operations, such as finding all strings with a given prefix and
   checking whether a prefix exists, are efficient.
 - for null-terminated strings, they are faster than a standard binary tree
   because they avoid an expensive string comparison at each step.
 - they have predictable memory use: each string added ads
 - they are at worst around 3-4 times as slow as hash tables.
 - if operations are performed in sorted or mostly-sorted order, they range
   from only marginally slower to significantly faster than hash tables,
   while supporting many more operations.

 Implementation notes:
 - This implementation is actually a tree-based map, not a plain tree. External
   nodes are pairs of keys and values, and a key_getter function can be
   provided to map a key (which can be an arbitrary pointer) to a
   null-terminated string. By default the key itself is assumed to be the
   string.
 - The set of strings needs to be prefix-free. That is naturally true for the
   null-terminated strings we use here.
 - Hooks called on_clear and on_copy are provided. They are called for a
   key-value pair while clearing the map and copying it, respectively. By
   default they do nothing.
 */

#ifndef CRITBIT_H_
#define CRITBIT_H_

#include <stddef.h>
#include <sys/types.h>

#ifdef _MSC_VER /* MSVC */
typedef unsigned __int8 uint8_t;
typedef unsigned __int32 uint32_t;
#ifdef _WIN64
typedef signed __int64 intptr_t;
#else
typedef _W64 signed int intptr_t;
#endif
#else /* Not MSVC */
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The key getter translates from a void* to the start of the
// key string memory
typedef const char *(*cb_key_getter_func)(const void *key);

// The key size func translates from a void* to the size of the
// corresponding key string memory returned by the key_getter
typedef ssize_t (*cb_key_size_func)(const void *key);

// The hook func receives the key, value pointers of an item.
// the on_clear and on_copy hooks have this signature
typedef void (*cb_kv_hook_func)(void *key, void *value);

struct cb_node_t;
struct cb_node_kv_pair_t;
typedef uint32_t cb_node_idx_t;

/*! Main data structure */
typedef struct {
  // Index into _nodes of the root of the tree
  cb_node_idx_t root;
  // Number of stored items
  size_t count;
  cb_key_getter_func key_getter;
  cb_key_size_func key_size;
  cb_kv_hook_func on_clear;
  cb_kv_hook_func on_copy;
  // Internal tracking for bulk loading of sorted items
  const uint8_t *_last_sorted;
  // Storage for the nodes
  struct cb_node_kv_pair_t *_nodes;
  // How many elements are allocated to _nodes
  cb_node_idx_t _node_size;
  // The next available node index in _nodes
  cb_node_idx_t _next_node;
} cb_tree_t;

typedef struct cb_iter_stack {
  cb_node_idx_t node;
  struct cb_iter_stack *next;
} cb_iter_stack;

typedef struct {
  cb_tree_t *tree;
  cb_iter_stack *head;
} cb_iter_t;

/*! Creates an new, empty critbit tree */
extern cb_tree_t cb_tree_make(void);

/*! Returns the value stored for k, or NULL if not found */
extern void *cb_tree_getitem(cb_tree_t *tree, const void *k);

/*! Returns non-zero if tree contains str */
extern int cb_tree_contains(cb_tree_t *tree, const void *k);

/*! Inserts (k, v) into tree, returns 0 on insert, 1 on replace,
 * -1 on error.  If non-null, oldv will b filled in with the existing
 *  value replaced, if any. */
extern int cb_tree_setitem(cb_tree_t *tree, void *k, void *v,
                           const void **oldv);

/*! Inserts (k, v) into tree iff k is not already set */
extern int cb_tree_setdefault(cb_tree_t *tree, void *k, void *v,
                              const void **oldv);

/*! Deletes k from the tree, returns 0 on suceess */
extern int cb_tree_delete(cb_tree_t *tree, const void *k, const void **oldk,
                          const void **oldv);

/*! Returns 0 on success, setting oldk and oldv if not NULL */
extern int cb_tree_popitem(cb_tree_t *tree, const void **oldk,
                           const void **oldv);

/*! Clears the given tree */
extern void cb_tree_clear(cb_tree_t *tree);

/*! Makes a shallow copy of the given tree. If it fails, count = -1 */
extern cb_tree_t cb_tree_copy(cb_tree_t *tree);

extern int cb_tree_has_prefix_str(cb_tree_t *tree, const char *prefix_str,
                                  ssize_t prefix_size);
extern int cb_tree_has_prefix_key(cb_tree_t *tree, const void *prefix_key);

extern cb_iter_t cb_tree_iter(cb_tree_t *tree);
extern cb_iter_t cb_tree_iter_prefix_str(cb_tree_t *tree,
                                         const char *prefix_str,
                                         ssize_t prefix_size);
extern cb_iter_t cb_tree_iter_prefix_key(cb_tree_t *tree,
                                         const void *prefix_key);

extern int cb_tree_iter_next(cb_iter_t *iter, void **k, void **v);

#ifdef __cplusplus
}
#endif

#endif /* CRITBIT_H_ */
