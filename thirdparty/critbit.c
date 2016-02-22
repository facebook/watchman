/*
 * critbit89 - A crit-bit tree implementation for strings in C89
 * Written by Jonas Gehring <jonas@jgehring.net>
 * Portions copyright 2013 Facebook.
 */

/*
 * The code makes the assumption that malloc returns pointers aligned at at
 * least a two-byte boundary. Since the C standard requires that malloc return
 * pointers that can store any type, there are no commonly-used toolchains for
 * which this assumption is false.
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "critbit.h"

typedef struct cb_node_t {
  // Index into _nodes of the 0/1 children
  cb_node_idx_t child[2];
  // The byte offset into the string
  uint32_t byte;
  uint8_t otherbits;
} cb_node_t;

/* Key-value pair */
typedef struct {
  void *k;
  void *v;
} cb_kv_pair_t;

// This is the storage for each element of the _nodes array
typedef struct cb_node_kv_pair_t {
  // Internal node
  cb_node_t i;
  // External node
  cb_kv_pair_t e;
} cb_node_kv_pair_t;

#define INVALID_NODE (cb_node_idx_t) - 1

static void *realloc_and_zero(void *ptr, size_t oldsize, size_t newsize) {
  ptr = realloc(ptr, newsize);
  memset(ptr + oldsize, 0, newsize - oldsize);
  return ptr;
}

static cb_node_idx_t new_node(cb_tree_t *tree) {
  if (tree->_next_node >= tree->_node_size) {
    cb_node_idx_t newsize = tree->_node_size * 2;
    tree->_nodes = realloc_and_zero(tree->_nodes, tree->_node_size *
                                                      sizeof(cb_node_kv_pair_t),
                                    newsize * sizeof(cb_node_kv_pair_t));
    tree->_node_size = newsize;
  }
  return tree->_next_node++;
}

static void free_inode(cb_tree_t *tree, cb_node_idx_t node) {
  /* This does nothing for now. */
  (void)tree; /* prevent compiler warnings */
  (void)node;
}

static void free_enode(cb_tree_t *tree, cb_node_idx_t node) {
  tree->_nodes[node].e.k = NULL;
  tree->_nodes[node].e.v = NULL;
}

// The default key type is assumed to be a C-string,
// so the getter and size functions return the pointer and compute
// the length, respectively
static const char *key_getter_std(const void *k) { return (const char *)k; }
static ssize_t key_size_std(const void *k) { return strlen((const char *)k); }

static void kv_hook_func_std(void *k, void *v) {
  /* Since we only store references, this should do nothing. */
  (void)k; /* prevent compiler warnings */
  (void)v;
  return;
}

static void _init(cb_tree_t *tree) {
  tree->root = INVALID_NODE;
  tree->count = 0;
  tree->_last_sorted = NULL;
  tree->_nodes = calloc(16, sizeof(cb_node_kv_pair_t));
  tree->_node_size = 16;
  tree->_next_node = 0;
}

cb_tree_t cb_tree_make(void) {
  cb_tree_t tree;
  _init(&tree);
  tree.key_getter = &key_getter_std;
  tree.key_size = &key_size_std;
  tree.on_clear = &kv_hook_func_std;
  tree.on_copy = &kv_hook_func_std;
  return tree;
}

void *cb_tree_getitem(cb_tree_t *tree, const void *k) {
  const cb_node_kv_pair_t *nodes = tree->_nodes;
  const char *str = tree->key_getter(k);
  const uint8_t *ubytes = (void *)str;
  const ssize_t ulen = tree->key_size(k);
  cb_node_idx_t p = tree->root;
  cb_node_idx_t pairindex;

  if (p == INVALID_NODE) {
    return NULL;
  }

  while (1 & p) {
    cb_node_idx_t q = p >> 1;
    uint8_t c = 0;
    int direction;

    if (nodes[q].i.byte < ulen) {
      c = ubytes[nodes[q].i.byte];
    }
    direction = (1 + (nodes[q].i.otherbits | c)) >> 8;

    p = nodes[q].i.child[direction];
  }

  pairindex = p >> 1;
  if (memcmp(str, tree->key_getter(nodes[pairindex].e.k),
             tree->key_size(nodes[pairindex].e.k)) == 0) {
    return nodes[pairindex].e.v;
  }
  return NULL;
}

int cb_tree_contains(cb_tree_t *tree, const void *k) {
  return (cb_tree_getitem(tree, k) != NULL);
}

static int _setitem(cb_tree_t *tree, void *k, void *v, int replace,
                    const void **oldv) {
  cb_node_kv_pair_t *nodes = tree->_nodes;
  const char *str = tree->key_getter(k);
  const uint8_t *const ubytes = (void *)str;
  const ssize_t ulen = tree->key_size(k);
  cb_node_idx_t p = tree->root;
  cb_node_idx_t pairindex;
  const uint8_t *pairkeystr;
  uint8_t c;
  uint32_t newbyte;
  uint32_t newotherbits;
  int direction, newdirection;
  cb_node_idx_t newnodeindex;
  cb_node_idx_t *wherep;

  if (p == INVALID_NODE) {
    pairindex = new_node(tree);
    nodes[pairindex].e.k = k;
    nodes[pairindex].e.v = v;
    tree->root = pairindex << 1;
    tree->count = 1;
    tree->_last_sorted = ubytes;
    return 0;
  }

longest_common_prefix_search:
  if (tree->_last_sorted) {
    /*
    This is a really neat optimization for when elements are
    inserted in sorted order. When they are, then one of the
    elements that shares the longest common prefix with k is the
    last element inserted. Compare against that, and if our
    assumption fails fall back to a tree search.
    */
    pairkeystr = tree->_last_sorted;
    pairindex = tree->count - 1;
  } else {
    while (1 & p) {
      cb_node_idx_t q = p >> 1;
      c = 0;
      if (nodes[q].i.byte < ulen) {
        c = ubytes[nodes[q].i.byte];
      }
      direction = (1 + (nodes[q].i.otherbits | c)) >> 8;

      p = nodes[q].i.child[direction];
    }

    pairindex = p >> 1;
    /* This needs to be unsigned because it's compared against ubytes */
    pairkeystr = (uint8_t *)tree->key_getter(nodes[pairindex].e.k);
  }

  for (newbyte = 0; newbyte < ulen; ++newbyte) {
    if (pairkeystr[newbyte] != ubytes[newbyte]) {
      newotherbits = pairkeystr[newbyte] ^ ubytes[newbyte];
      goto different_byte_found;
    }
  }
  if (pairkeystr[newbyte] != 0) {
    newotherbits = pairkeystr[newbyte];
    goto different_byte_found;
  }

  /* Key already in the tree -- replace */
  if (oldv) {
    *oldv = nodes[pairindex].e.v;
  }
  if (replace) {
    nodes[pairindex].e.v = v;
    return 1;
  }
  return -1;

different_byte_found:
  newotherbits |= newotherbits >> 1;
  newotherbits |= newotherbits >> 2;
  newotherbits |= newotherbits >> 4;
  newotherbits = (newotherbits & ~(newotherbits >> 1)) ^ 255;
  c = pairkeystr[newbyte];
  newdirection = (1 + (newotherbits | c)) >> 8;

  /* newdirection is 1 for a lexicographically smaller element */
  if (newdirection && tree->_last_sorted) {
    /* whoops, the optimization failed */
    tree->_last_sorted = NULL;
    goto longest_common_prefix_search;
  }

  newnodeindex = new_node(tree);
  if (newnodeindex == INVALID_NODE) {
    return -1;
  }
  /* possible realloc */
  nodes = tree->_nodes;

  nodes[newnodeindex].i.byte = newbyte;
  nodes[newnodeindex].i.otherbits = newotherbits;
  nodes[newnodeindex].i.child[1 - newdirection] = newnodeindex << 1;
  nodes[newnodeindex].e.k = k;
  nodes[newnodeindex].e.v = v;
  if (oldv) {
    *oldv = NULL;
  }

  /* Insert into tree */
  wherep = &tree->root;
  for (;;) {
    cb_node_idx_t q;
    p = *wherep;
    if (!(1 & p)) {
      break;
    }

    q = p >> 1;
    if (nodes[q].i.byte > newbyte) {
      break;
    }
    if (nodes[q].i.byte == newbyte && nodes[q].i.otherbits > newotherbits) {
      break;
    }

    c = 0;
    if (nodes[q].i.byte < ulen) {
      c = ubytes[nodes[q].i.byte];
    }
    direction = (1 + (nodes[q].i.otherbits | c)) >> 8;
    wherep = &nodes[q].i.child[direction];
  }

  nodes[newnodeindex].i.child[newdirection] = *wherep;
  *wherep = (newnodeindex << 1) + 1;
  tree->count++;
  if (tree->_last_sorted) {
    tree->_last_sorted = ubytes;
  }
  return 0;
}

int cb_tree_setitem(cb_tree_t *tree, void *k, void *v, const void **oldv) {
  return _setitem(tree, k, v, 1, oldv);
}

int cb_tree_setdefault(cb_tree_t *tree, void *k, void *v, const void **oldv) {
  return _setitem(tree, k, v, 0, oldv);
}

/*! Deletes str from the tree, returns 0 on success */
int cb_tree_delete(cb_tree_t *tree, const void *k, const void **oldk,
                   const void **oldv) {
  cb_node_kv_pair_t *nodes = tree->_nodes;
  const char *str = tree->key_getter(k);
  const uint8_t *ubytes = (void *)str;
  const ssize_t ulen = tree->key_size(k);
  cb_node_idx_t p = tree->root;
  cb_node_idx_t *wherep = 0, *whereq = 0;
  cb_node_idx_t pairindex;
  cb_node_idx_t q /*= -1*/;
  int direction = 0;

  if (p == INVALID_NODE) {
    return 1;
  }
  wherep = &tree->root;

  while (1 & p) {
    uint8_t c = 0;
    whereq = wherep;
    q = p >> 1;

    if (nodes[q].i.byte < ulen) {
      c = ubytes[nodes[q].i.byte];
    }
    direction = (1 + (nodes[q].i.otherbits | c)) >> 8;
    wherep = &nodes[q].i.child[direction];
    p = *wherep;
  }

  pairindex = p >> 1;
  if (memcmp(str, tree->key_getter(nodes[pairindex].e.k),
             tree->key_size(nodes[pairindex].e.k)) != 0) {
    return 1;
  }

  if (oldk) {
    *oldk = nodes[pairindex].e.k;
  }
  if (oldv) {
    *oldv = nodes[pairindex].e.v;
  }

  free_enode(tree, pairindex);
  tree->count--;
  /* don't bother with the last_sorted optimization */
  tree->_last_sorted = NULL;

  if (!whereq) {
    tree->root = -1;
    return 0;
  }

  *whereq = nodes[q].i.child[1 - direction];
  free_inode(tree, q);
  return 0;
}

int cb_tree_popitem(cb_tree_t *tree, const void **k, const void **v) {
  cb_node_kv_pair_t *nodes = tree->_nodes;
  cb_node_idx_t p = tree->root;
  cb_node_idx_t *wherep = 0, *whereq = 0;
  cb_node_idx_t pairindex;

  cb_node_idx_t q /* = -1*/;
  if (p == INVALID_NODE) {
    return 1;
  }

  wherep = &tree->root;

  while (1 & p) {
    whereq = wherep;
    q = p >> 1;
    wherep = &nodes[q].i.child[0];
    p = *wherep;
  }

  pairindex = p >> 1;
  if (k) {
    *k = nodes[pairindex].e.k;
  }
  if (v) {
    *v = nodes[pairindex].e.v;
  }
  free_enode(tree, pairindex);
  tree->count--;
  /* the last_sorted optimization is still fine because the
  lexicographically first element is removed... */

  if (!whereq) {
    tree->root = INVALID_NODE;
    /* ...unless there's just one element, of course */
    tree->_last_sorted = NULL;
    return 0;
  }

  *whereq = nodes[q].i.child[1];
  free_inode(tree, q);
  return 0;
}

void cb_tree_clear(cb_tree_t *tree) {
  cb_node_kv_pair_t *nodes = tree->_nodes;
  cb_node_idx_t i;
  cb_node_idx_t size = tree->_node_size;
  cb_kv_hook_func onclear = tree->on_clear;

  for (i = 0; i < size; i++) {
    if (nodes[i].e.k) {
      onclear(nodes[i].e.k, nodes[i].e.v);
    }
  }
  free(nodes);
  _init(tree);
}

cb_tree_t cb_tree_copy(cb_tree_t *tree) {
  cb_tree_t newtree;
  cb_node_idx_t i;
  size_t size = tree->_node_size * sizeof(cb_node_kv_pair_t);
  newtree.root = tree->root;
  newtree.count = tree->count;
  newtree.key_getter = tree->key_getter;
  newtree.key_size = tree->key_size;
  newtree.on_clear = tree->on_clear;
  newtree.on_copy = tree->on_copy;

  newtree._nodes = malloc(size);
  memcpy(newtree._nodes, tree->_nodes, size);
  newtree._node_size = tree->_node_size;
  newtree._next_node = tree->_next_node;
  for (i = 0; i < newtree._node_size; i++) {
    if (newtree._nodes[i].e.k) {
      newtree.on_copy(newtree._nodes[i].e.k, newtree._nodes[i].e.v);
    }
  }
  return newtree;
}

/* Returns the part of the tree containing this prefix, or NULL if not found */
static cb_node_idx_t _find_prefix(cb_tree_t *tree, const char *prefix_str,
                             ssize_t ulen) {
  const cb_node_kv_pair_t *nodes = tree->_nodes;
  uint8_t *ubytes;
  cb_node_idx_t p = tree->root;
  cb_node_idx_t top = p;
  const char *str;

  if (prefix_str == NULL) {
    return p;
  }
  if (top == INVALID_NODE) {
    return INVALID_NODE;
  }

  ubytes = (void *)prefix_str;

  while (1 & p) {
    cb_node_idx_t q = p >> 1;
    uint8_t c = 0;
    int direction;

    if (nodes[q].i.byte < ulen) {
      c = ubytes[nodes[q].i.byte];
    }
    direction = (1 + (nodes[q].i.otherbits | c)) >> 8;

    p = nodes[q].i.child[direction];
    if (nodes[q].i.byte < ulen)
      top = p;
  }

  /* Check that what we found is actually the prefix */
  str = tree->key_getter(nodes[p >> 1].e.k);
  if (tree->key_size(nodes[p >> 1].e.k) >= ulen &&
      memcmp(str, prefix_str, ulen) == 0) {
    return top;
  }
  return INVALID_NODE;
}

int cb_tree_has_prefix_key(cb_tree_t *tree, const void *prefix_key) {
  const char *str = NULL;
  ssize_t size = -1;
  if (prefix_key != NULL) {
    str = tree->key_getter(prefix_key);
    size = tree->key_size(prefix_key);
  }
  return cb_tree_has_prefix_str(tree, str, size);
}

int cb_tree_has_prefix_str(cb_tree_t *tree, const char *prefix_str,
                           ssize_t prefix_size) {
  return (_find_prefix(tree, prefix_str, prefix_size) != INVALID_NODE);
}

cb_iter_t cb_tree_iter(cb_tree_t *tree) {
  return cb_tree_iter_prefix_str(tree, NULL, -1);
}

cb_iter_t cb_tree_iter_prefix_key(cb_tree_t *tree, const void *prefix_key) {
  const char *str = NULL;
  ssize_t size = -1;
  if (prefix_key != NULL) {
    str = tree->key_getter(prefix_key);
    size = tree->key_size(prefix_key);
  }
  return cb_tree_iter_prefix_str(tree, str, size);
}

cb_iter_t cb_tree_iter_prefix_str(cb_tree_t *tree, const char *prefix_str,
                                  ssize_t prefix_size) {
  cb_iter_t rv = {tree, NULL};

  cb_node_idx_t top = _find_prefix(tree, prefix_str, prefix_size);
  if (top != INVALID_NODE) {
    rv.tree = tree;
    rv.head = malloc(sizeof(cb_iter_stack));
    rv.head->node = top;
    rv.head->next = NULL;
  }
  return rv;
}

/*! Returns 1 if k and v are returned, 0 if exhausted, -1 on error */
int cb_tree_iter_next(cb_iter_t *iter, void **k, void **v) {
  const cb_node_kv_pair_t *nodes = iter->tree->_nodes;
  /* At any time we will remove exactly one node from the stack */
  uint32_t currnode;
  cb_iter_stack *newhead;
  uint32_t pairindex;

  if (iter->head == NULL) {
    if (k != NULL) {
      *k = NULL;
    }
    if (v != NULL) {
      *v = NULL;
    }
    return 0;
  }

  currnode = iter->head->node;
  newhead = iter->head->next;
  free(iter->head);
  while (1 & currnode) {
    cb_node_idx_t q = currnode >> 1;
    cb_iter_stack *newnode = malloc(sizeof(cb_iter_stack));
    if (newnode == NULL) {
      iter->head = NULL;
      return -1;
    }
    newnode->node = nodes[q].i.child[1];
    newnode->next = newhead;
    newhead = newnode;
    currnode = nodes[q].i.child[0];
  }
  iter->head = newhead;
  pairindex = currnode >> 1;
  if (k != NULL) {
    *k = nodes[pairindex].e.k;
  }
  if (v != NULL) {
    *v = nodes[pairindex].e.v;
  }
  return 1;
}
