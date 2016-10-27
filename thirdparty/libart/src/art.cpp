#include "art.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __SSE__
#include <emmintrin.h>
#endif
#include <assert.h>
#include <stdbool.h>
#include <algorithm>
#include <new>
#include "watchman_log.h"

#if defined(__clang__)
# if __has_feature(address_sanitizer)
#  define ART_SANITIZE_ADDRESS 1
# endif
#elif defined (__GNUC__) && \
      (((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ >= 5)) && \
      __SANITIZE_ADDRESS__
# define ART_SANITIZE_ADDRESS 1
#endif

/**
 * Macros to manipulate pointer tags
 */
#define IS_LEAF(x) (((uintptr_t)x & 1))
#define SET_LEAF(x) ((void*)((uintptr_t)x | 1))
#define LEAF_RAW(x) ((art_leaf*)((void*)((uintptr_t)x & ~1)))

// Valid values for art_node::type
enum {
    NODE4 = 1,
    NODE16,
    NODE48,
    NODE256
};

// Helper for dispatching to the correct node type
union node_ptr {
    art_node *n;
    art_node4 *n4;
    art_node16 *n16;
    art_node48 *n48;
    art_node256 *n256;
};

// const flavor of the above
union cnode_ptr {
    const art_node *n;
    const art_node4 *n4;
    const art_node16 *n16;
    const art_node48 *n48;
    const art_node256 *n256;
};

#ifdef _MSC_VER
#include <intrin.h>
static uint32_t __inline __builtin_ctz(uint32_t x) {
   DWORD r = 0;
   _BitScanForward(&r, x);
   return (uint32_t)r;
}
#endif

// The ART implementation requires that no key be a full prefix of an existing
// key during insertion.  In practice this means that each key must have a
// terminator character.  One approach is to ensure that the key and key_len
// includes a physical trailing NUL terminator when inserting C-strings.
// This doesn't help a great deal when working with binary strings that may be
// a slice in the middle of a buffer that has no termination.
//
// To facilitate this the key_at() function is used to look up the byte
// value at a given index.  If that index is 1 byte after the end of the
// key, we synthesize a fake NUL terminator byte.
//
// Note that if the keys contain NUL bytes earlier in the string this will
// break down and won't have the correct results.
//
// If the index is out of bounds we will assert to trap the fatal coding
// error inside this implementation.
//
// @param key pointer to the key bytes
// @param key_len the size of the byte, in bytes
// @param idx the index into the key
// @return the value of the key at the supplied index.
static inline unsigned char key_at(const unsigned char *key, int key_len, int idx) {
    if (idx == key_len) {
        // Implicit terminator
        return 0;
    }
#if !ART_SANITIZE_ADDRESS
    // If we were built with -fsanitize=address, let ASAN catch this,
    // otherwise, make sure we blow up if the input depth is out of bounds.
    w_assert(idx >= 0 && idx <= key_len,
             "key_at: key is %d %.*s and idx is %d, which is out of bounds",
             key_len, key_len, key, idx);
#endif
    return key[idx];
}

// A helper for looking at the key value at given index, in a leaf
static inline unsigned char leaf_key_at(const art_leaf *l, int idx) {
    return key_at(l->key, l->key_len, idx);
}

/**
 * Allocates a node of the given type,
 * initializes to zero and sets the type.
 */

art_node::art_node(uint8_t type) : type(type) {}

art_node4::art_node4() : n(NODE4) {
  memset(keys, 0, sizeof(keys));
  memset(children, 0, sizeof(children));
}

art_node16::art_node16() : n(NODE16) {
  memset(keys, 0, sizeof(keys));
  memset(children, 0, sizeof(children));
}

art_node48::art_node48() : n(NODE48) {
  memset(keys, 0, sizeof(keys));
  memset(children, 0, sizeof(children));
}

art_node256::art_node256() : n(NODE256) {
  memset(children, 0, sizeof(children));
}

/**
 * Initializes an ART tree
 * @return 0 on success.
 */
art_tree::art_tree() : root_(nullptr), size_(0) {}

static void destroy_leaf(art_leaf* leaf) {
  leaf->~art_leaf();
  delete[](char*) leaf;
}

// Recursively destroys the tree
static void destroy_node(art_node *n) {
    int i;
    union node_ptr p = {n};

    // Break if null
    if (!n) return;

    // Special case leafs
    if (IS_LEAF(n)) {
        destroy_leaf(LEAF_RAW(n));
        return;
    }

    // Handle each node type
    switch (n->type) {
        case NODE4:
            for (i=0;i<n->num_children;i++) {
                destroy_node(p.n4->children[i]);
            }
            delete p.n4;
            break;

        case NODE16:
            for (i=0;i<n->num_children;i++) {
                destroy_node(p.n16->children[i]);
            }
            delete p.n16;
            break;

        case NODE48:
            for (i=0;i<n->num_children;i++) {
                destroy_node(p.n48->children[i]);
            }
            delete p.n48;
            break;

        case NODE256:
            for (i=0;i<256;i++) {
                if (p.n256->children[i]) {
                    destroy_node(p.n256->children[i]);
                }
            }
            delete p.n256;
            break;

        default:
            abort();
    }
}

art_tree::~art_tree() {
  clear();
}

void art_tree::clear() {
  destroy_node(root_);
  root_ = nullptr;
  size_ = 0;
}

/**
 * Returns the size of the ART tree.
 */

art_node** art_node::findChild(unsigned char c) {
  int i;
  union node_ptr p = {this};
  switch (type) {
    case NODE4:
      for (i = 0; i < num_children; i++) {
        if (p.n4->keys[i] == c) {
          return &p.n4->children[i];
        }
      }
      return nullptr;

    case NODE16: {
#ifdef __SSE__
      __m128i cmp;
      int mask, bitfield;

      // Compare the key to all 16 stored keys
      cmp = _mm_cmpeq_epi8(
          _mm_set1_epi8(c), _mm_loadu_si128((__m128i*)p.n16->keys));

      // Use a mask to ignore children that don't exist
      mask = (1 << num_children) - 1;
      bitfield = _mm_movemask_epi8(cmp) & mask;

      /*
       * If we have a match (any bit set) then we can
       * return the pointer match using ctz to get
       * the index.
       */
      if (bitfield)
        return &p.n16->children[__builtin_ctz(bitfield)];
#else
      for (i = 0; i < num_children; i++) {
        if (p.n16->keys[i] == c) {
          return &p.n16->children[i];
        }
      }
#endif
      return nullptr;
    }

    case NODE48:
      i = p.n48->keys[c];
      if (i) {
        return &p.n48->children[i - 1];
      }
      return nullptr;

    case NODE256:
      if (p.n256->children[c]) {
        return &p.n256->children[c];
      }
      return nullptr;

    default:
      abort();
  }
}

/**
 * Returns the number of prefix characters shared between
 * the key and node.
 */
uint32_t art_node::checkPrefix(
    const unsigned char* key,
    uint32_t key_len,
    int depth) const {
  auto max_cmp =
      std::min(std::min(partial_len, ART_MAX_PREFIX_LEN), key_len - depth);
  int idx;
  for (idx = 0; idx < max_cmp; idx++) {
    if (partial[idx] != key[depth + idx]) {
      return idx;
    }
  }
  return idx;
}

/**
 * Checks if a leaf matches
 * @return true if the key is an exact match.
 */
static bool
leaf_matches(const art_leaf* n, const unsigned char* key, uint32_t key_len) {
  // Fail if the key lengths are different
  if (n->key_len != key_len) {
    return false;
  }

  return memcmp(n->key, key, key_len) == 0;
}

/**
 * Searches for a value in the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
void* art_tree::search(const unsigned char* key, uint32_t key_len) const {
  art_node** child;
  art_node* n = root_;
  int depth = 0;
  while (n) {
    // Might be a leaf
    if (IS_LEAF(n)) {
      art_leaf* leaf = LEAF_RAW(n);
      // Check if the expanded path matches
      if (leaf_matches(leaf, key, key_len)) {
        return leaf->value;
      }
      return NULL;
    }

    // Bail if the prefix does not match
    if (n->partial_len) {
      auto prefix_len = n->checkPrefix(key, key_len, depth);
      if (prefix_len != std::min(ART_MAX_PREFIX_LEN, n->partial_len))
        return NULL;
      depth = depth + n->partial_len;
    }

    if (depth > key_len) {
      // Stored key is longer than input key, can't be an exact match
      return NULL;
    }

    // Recursively search
    child = n->findChild(key_at(key, key_len, depth));
    n = (child) ? *child : NULL;
    depth++;
  }
  return NULL;
}

art_leaf* art_tree::longestMatch(const unsigned char* key, uint32_t key_len)
    const {
  art_node** child;
  art_node* n = root_;
  int depth = 0;
  while (n) {
    // Might be a leaf
    if (IS_LEAF(n)) {
      art_leaf* leaf = LEAF_RAW(n);
      // Check if the prefix matches
      auto prefix_len = std::min(leaf->key_len, key_len);
      if (prefix_len > 0 && memcmp(leaf->key, key, prefix_len) == 0) {
        // Shares the same prefix
        return leaf;
      }
      return nullptr;
    }

    // Bail if the prefix does not match
    if (n->partial_len) {
      auto prefix_len = n->checkPrefix(key, key_len, depth);
      if (prefix_len != std::min(ART_MAX_PREFIX_LEN, n->partial_len)) {
        return nullptr;
      }
      depth = depth + n->partial_len;
    }

    if (depth > key_len) {
      // Stored key is longer than input key, can't be an exact match
      return nullptr;
    }

    // Recursively search
    child = n->findChild(key_at(key, key_len, depth));
    n = (child) ? *child : nullptr;
    depth++;
  }
  return nullptr;
}

// Find the minimum leaf under a node
art_leaf* art_node::minimum() const {
  int idx;
  union cnode_ptr p = {this};

  while (p.n) {
    if (IS_LEAF(p.n)) {
      return LEAF_RAW(p.n);
    }

    switch (p.n->type) {
      case NODE4:
        p.n = p.n4->children[0];
        break;
      case NODE16:
        p.n = p.n16->children[0];
        break;
      case NODE48:
        idx = 0;
        while (!p.n48->keys[idx]) {
          idx++;
        }
        idx = p.n48->keys[idx] - 1;
        p.n = p.n48->children[idx];
        break;
      case NODE256:
        idx = 0;
        while (!p.n256->children[idx]) {
          idx++;
        }
        p.n = p.n256->children[idx];
        break;
      default:
        abort();
        return nullptr;
    }
  }
  return nullptr;
}

// Find the maximum leaf under a node
art_leaf* art_node::maximum() const {
  int idx;
  union cnode_ptr p = {this};

  while (p.n) {
    if (IS_LEAF(p.n)) {
      return LEAF_RAW(p.n);
    }

    switch (p.n->type) {
      case NODE4:
        p.n = p.n4->children[p.n->num_children - 1];
        break;
      case NODE16:
        p.n = p.n16->children[p.n->num_children - 1];
        break;
      case NODE48:
        idx = 255;
        while (!p.n48->keys[idx]) {
          idx--;
        }
        idx = p.n48->keys[idx] - 1;
        p.n = p.n48->children[idx];
        break;
      case NODE256:
        idx = 255;
        while (!p.n256->children[idx]) {
          idx--;
        }
        p.n = p.n256->children[idx];
        break;
      default:
        abort();
        return nullptr;
    }
  }
  return nullptr;
}

/**
 * Returns the minimum valued leaf
 */
art_leaf* art_tree::minimum() const {
  return root_->minimum();
}

/**
 * Returns the maximum valued leaf
 */
art_leaf* art_tree::maximum() const {
  return root_->maximum();
}

// Constructs a new leaf using the provided key.
static art_leaf* make_leaf(const unsigned char *key, int key_len, void *value) {
    // art_leaf::key is declared as key[1] so sizeof(art_leaf) is 1 too many;
    // deduct 1 so that we allocate the perfect size
    auto l = (art_leaf*)(new char[sizeof(art_leaf) + key_len - 1]);
    new (l) art_leaf();
    l->value = value;
    l->key_len = key_len;
    memcpy(l->key, key, key_len);
    return l;
}

static uint32_t longest_common_prefix(art_leaf* l1, art_leaf* l2, int depth) {
  auto max_cmp = std::min(l1->key_len, l2->key_len) - depth;
  int idx;
  for (idx = 0; idx < max_cmp; idx++) {
    if (l1->key[depth + idx] != l2->key[depth + idx]) {
      return idx;
    }
  }
  return idx;
}

static void copy_header(art_node *dest, art_node *src) {
    dest->num_children = src->num_children;
    dest->partial_len = src->partial_len;
    memcpy(
        dest->partial,
        src->partial,
        std::min(ART_MAX_PREFIX_LEN, src->partial_len));
}

static void add_child256(art_node256 *n, art_node **ref, unsigned char c, void *child) {
    (void)ref;
    n->n.num_children++;
    n->children[c] = (art_node*)child;
}

static void add_child48(art_node48 *n, art_node **ref, unsigned char c, void *child) {
    if (n->n.num_children < 48) {
        int pos = 0;
        while (n->children[pos]) pos++;
        n->children[pos] = (art_node*)child;
        n->keys[c] = pos + 1;
        n->n.num_children++;
    } else {
        auto new_node = new art_node256;
        int i;
        for (i=0;i<256;i++) {
            if (n->keys[i]) {
                new_node->children[i] = n->children[n->keys[i] - 1];
            }
        }
        copy_header((art_node*)new_node, (art_node*)n);
        *ref = (art_node*)new_node;
        delete n;
        add_child256(new_node, ref, c, child);
    }
}

static void add_child16(art_node16 *n, art_node **ref, unsigned char c, void *child) {
    if (n->n.num_children < 16) {
        unsigned idx;
#ifdef __SSE__
        __m128i cmp;
        unsigned mask, bitfield;

        // Compare the key to all 16 stored keys
        cmp = _mm_cmplt_epi8(_mm_set1_epi8(c),
                _mm_loadu_si128((__m128i*)n->keys));

        // Use a mask to ignore children that don't exist
        mask = (1 << n->n.num_children) - 1;
        bitfield = _mm_movemask_epi8(cmp) & mask;

        // Check if less than any
        if (bitfield) {
            idx = __builtin_ctz(bitfield);
            memmove(n->keys+idx+1,n->keys+idx,n->n.num_children-idx);
            memmove(n->children+idx+1,n->children+idx,
                    (n->n.num_children-idx)*sizeof(void*));
        } else
            idx = n->n.num_children;
#else
        for (idx = 0; idx < n->n.num_children; idx++) {
            if (c < n->keys[idx]) {
                memmove(n->keys + idx + 1, n->keys + idx,
                        n->n.num_children - idx);
                memmove(n->children + idx + 1, n->children + idx,
                        (n->n.num_children - idx) * sizeof(void *));
                break;
            }
        }
#endif

        // Set the child
        n->keys[idx] = c;
        n->children[idx] = (art_node*)child;
        n->n.num_children++;

    } else {
        auto new_node = new art_node48;
        int i;

        // Copy the child pointers and populate the key map
        memcpy(new_node->children, n->children,
                sizeof(void*)*n->n.num_children);
        for (i=0;i<n->n.num_children;i++) {
            new_node->keys[n->keys[i]] = i + 1;
        }
        copy_header((art_node*)new_node, (art_node*)n);
        *ref = (art_node*)new_node;
        delete n;
        add_child48(new_node, ref, c, child);
    }
}

static void add_child4(art_node4 *n, art_node **ref, unsigned char c, void *child) {
    if (n->n.num_children < 4) {
        int idx;
        for (idx=0; idx < n->n.num_children; idx++) {
            if (c < n->keys[idx]) break;
        }

        // Shift to make room
        memmove(n->keys+idx+1, n->keys+idx, n->n.num_children - idx);
        memmove(n->children+idx+1, n->children+idx,
                (n->n.num_children - idx)*sizeof(void*));

        // Insert element
        n->keys[idx] = c;
        n->children[idx] = (art_node*)child;
        n->n.num_children++;

    } else {
        auto new_node = new art_node16;

        // Copy the child pointers and the key map
        memcpy(new_node->children, n->children,
                sizeof(void*)*n->n.num_children);
        memcpy(new_node->keys, n->keys,
                sizeof(unsigned char)*n->n.num_children);
        copy_header((art_node*)new_node, (art_node*)n);
        *ref = (art_node*)new_node;
        delete n;
        add_child16(new_node, ref, c, child);
    }
}

static void add_child(art_node *n, art_node **ref, unsigned char c, void *child) {
    union node_ptr p = {n};

    switch (n->type) {
        case NODE4:
            add_child4(p.n4, ref, c, child);
            return;
        case NODE16:
            add_child16(p.n16, ref, c, child);
            return;
        case NODE48:
            add_child48(p.n48, ref, c, child);
            return;
        case NODE256:
            add_child256(p.n256, ref, c, child);
            return;
        default:
            abort();
    }
}

/**
 * Calculates the index at which the prefixes mismatch
 */
static uint32_t prefix_mismatch(
    const art_node* n,
    const unsigned char* key,
    uint32_t key_len,
    int depth) {
  auto max_cmp =
      std::min(std::min(ART_MAX_PREFIX_LEN, n->partial_len), key_len - depth);
  int idx;
  for (idx = 0; idx < max_cmp; idx++) {
    if (n->partial[idx] != key[depth + idx]) {
      return idx;
    }
  }

  // If the prefix is short we can avoid finding a leaf
  if (n->partial_len > ART_MAX_PREFIX_LEN) {
    // Prefix is longer than what we've checked, find a leaf
    art_leaf* l = n->minimum();
    max_cmp = std::min(l->key_len, key_len) - depth;
    for (; idx < max_cmp; idx++) {
      if (l->key[idx + depth] != key[depth + idx]) {
        return idx;
      }
    }
  }
  return idx;
}

static void *recursive_insert(art_node *n, art_node **ref,
                              const unsigned char *key, int key_len,
                              void *value, int depth, int *old) {
    art_leaf *l;
    // If we are at a NULL node, inject a leaf
    if (!n) {
        *ref = (art_node*)SET_LEAF(make_leaf(key, key_len, value));
        return NULL;
    }

    // If we are at a leaf, we need to replace it with a node
    if (IS_LEAF(n)) {
        art_node4 *new_node;
        art_leaf *l2;

        l = LEAF_RAW(n);

        // Check if we are updating an existing value
        if (leaf_matches(l, key, key_len)) {
            void *old_val = l->value;
            *old = 1;
            l->value = value;
            return old_val;
        }

        // New value, we must split the leaf into a node4
        new_node = new art_node4;

        // Create a new leaf
        l2 = make_leaf(key, key_len, value);

        // Determine longest prefix
        auto longest_prefix = longest_common_prefix(l, l2, depth);
        new_node->n.partial_len = longest_prefix;
        memcpy(
            new_node->n.partial,
            l2->key + depth,
            std::min(ART_MAX_PREFIX_LEN, longest_prefix));
        // Add the leafs to the new node4
        *ref = (art_node*)new_node;
        add_child4(new_node, ref, leaf_key_at(l, depth + longest_prefix),
                   SET_LEAF(l));
        add_child4(new_node, ref, leaf_key_at(l2, depth + longest_prefix),
                   SET_LEAF(l2));
        return NULL;
    }

    // Check if given node has a prefix
    if (n->partial_len) {
        // Determine if the prefixes differ, since we need to split
        auto prefix_diff = prefix_mismatch(n, key, key_len, depth);
        art_node4 *new_node;
        if (prefix_diff >= n->partial_len) {
          depth += n->partial_len;
          goto RECURSE_SEARCH;
        }

        // Create a new node
        new_node = new art_node4;
        *ref = (art_node*)new_node;
        new_node->n.partial_len = prefix_diff;
        memcpy(
            new_node->n.partial,
            n->partial,
            std::min(ART_MAX_PREFIX_LEN, prefix_diff));

        // Adjust the prefix of the old node
        if (n->partial_len <= ART_MAX_PREFIX_LEN) {
            add_child4(new_node, ref, n->partial[prefix_diff], n);
            n->partial_len -= (prefix_diff+1);
            memmove(
                n->partial,
                n->partial + prefix_diff + 1,
                std::min(ART_MAX_PREFIX_LEN, n->partial_len));
        } else {
            n->partial_len -= (prefix_diff+1);
            l = n->minimum();
            add_child4(new_node, ref, leaf_key_at(l, depth + prefix_diff), n);
            memcpy(
                n->partial,
                l->key + depth + prefix_diff + 1,
                std::min(ART_MAX_PREFIX_LEN, n->partial_len));
        }

        // Insert the new leaf
        l = make_leaf(key, key_len, value);
        add_child4(new_node, ref, leaf_key_at(l, depth + prefix_diff),
                   SET_LEAF(l));
        return NULL;
    }

RECURSE_SEARCH:;
    {
        // Find a child to recurse to
        art_node** child = n->findChild(key_at(key, key_len, depth));
        if (child) {
            return recursive_insert(*child, child, key, key_len, value,
                                    depth + 1, old);
        }
    }

    // No child, node goes within us
    l = make_leaf(key, key_len, value);
    add_child(n, ref, leaf_key_at(l, depth), SET_LEAF(l));
    return NULL;
}

/**
 * Inserts a new value into the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @arg value Opaque value.
 * @return NULL if the item was newly inserted, otherwise
 * the old value pointer is returned.
 */
void* art_tree::insert(
    const unsigned char* key,
    uint32_t key_len,
    void* value) {
  int old_val = 0;
  void* old = recursive_insert(root_, &root_, key, key_len, value, 0, &old_val);
  if (!old_val) {
    size_++;
  }
  return old;
}

static void remove_child256(art_node256 *n, art_node **ref, unsigned char c) {
    n->children[c] = NULL;
    n->n.num_children--;

    // Resize to a node48 on underflow, not immediately to prevent
    // trashing if we sit on the 48/49 boundary
    if (n->n.num_children == 37) {
        int i, pos = 0;
        auto new_node = new art_node48;
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);

        for (i=0;i<256;i++) {
            if (n->children[i]) {
                new_node->children[pos] = n->children[i];
                new_node->keys[i] = pos + 1;
                pos++;
            }
        }
        delete n;
    }
}

static void remove_child48(art_node48 *n, art_node **ref, unsigned char c) {
    int pos = n->keys[c];
    n->keys[c] = 0;
    n->children[pos-1] = NULL;
    n->n.num_children--;

    if (n->n.num_children == 12) {
        int i, child = 0;
        auto new_node = new art_node16;
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);

        for (i=0;i<256;i++) {
            pos = n->keys[i];
            if (pos) {
                new_node->keys[child] = i;
                new_node->children[child] = n->children[pos - 1];
                child++;
            }
        }
        delete n;
    }
}

static void remove_child16(art_node16 *n, art_node **ref, art_node **l) {
    int pos = l - n->children;
    memmove(n->keys+pos, n->keys+pos+1, n->n.num_children - 1 - pos);
    memmove(n->children+pos, n->children+pos+1, (n->n.num_children - 1 - pos)*sizeof(void*));
    n->n.num_children--;

    if (n->n.num_children == 3) {
        auto new_node = new art_node4;
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);
        memcpy(new_node->keys, n->keys, 4);
        memcpy(new_node->children, n->children, 4*sizeof(void*));
        delete n;
    }
}

static void remove_child4(art_node4 *n, art_node **ref, art_node **l) {
    int pos = l - n->children;
    memmove(n->keys+pos, n->keys+pos+1, n->n.num_children - 1 - pos);
    memmove(n->children+pos, n->children+pos+1, (n->n.num_children - 1 - pos)*sizeof(void*));
    n->n.num_children--;

    // Remove nodes with only a single child
    if (n->n.num_children == 1) {
        art_node *child = n->children[0];
        if (!IS_LEAF(child)) {
            // Concatenate the prefixes
            auto prefix = n->n.partial_len;
            if (prefix < ART_MAX_PREFIX_LEN) {
                n->n.partial[prefix] = n->keys[0];
                prefix++;
            }
            if (prefix < ART_MAX_PREFIX_LEN) {
              auto sub_prefix =
                  std::min(child->partial_len, ART_MAX_PREFIX_LEN - prefix);
              memcpy(n->n.partial + prefix, child->partial, sub_prefix);
              prefix += sub_prefix;
            }

            // Store the prefix in the child
            memcpy(
                child->partial,
                n->n.partial,
                std::min(prefix, ART_MAX_PREFIX_LEN));
            child->partial_len += n->n.partial_len + 1;
        }
        *ref = child;
        delete n;
    }
}

static void remove_child(art_node *n, art_node **ref, unsigned char c, art_node **l) {
    union node_ptr p = {n};

    switch (n->type) {
        case NODE4:
            remove_child4(p.n4, ref, l);
            return;
        case NODE16:
            remove_child16(p.n16, ref, l);
            return;
        case NODE48:
            remove_child48(p.n48, ref, c);
            return;
        case NODE256:
            remove_child256(p.n256, ref, c);
            return;
        default:
            abort();
    }
}

static art_leaf* recursive_delete(art_node *n, art_node **ref, const unsigned char *key, int key_len, int depth) {
    art_node **child;

    // Search terminated
    if (!n) return NULL;

    // Handle hitting a leaf node
    if (IS_LEAF(n)) {
        art_leaf *l = LEAF_RAW(n);
        if (leaf_matches(l, key, key_len)) {
            *ref = NULL;
            return l;
        }
        return NULL;
    }

    // Bail if the prefix does not match
    if (n->partial_len) {
        auto prefix_len = n->checkPrefix(key, key_len, depth);
        if (prefix_len != std::min(ART_MAX_PREFIX_LEN, n->partial_len)) {
          return NULL;
        }
        depth = depth + n->partial_len;
    }

    // Find child node
    child = n->findChild(key_at(key, key_len, depth));
    if (!child) return NULL;

    // If the child is leaf, delete from this node
    if (IS_LEAF(*child)) {
        art_leaf *l = LEAF_RAW(*child);
        if (leaf_matches(l, key, key_len)) {
            remove_child(n, ref, key_at(key, key_len, depth), child);
            return l;
        }
        return NULL;

    // Recurse
    } else {
        return recursive_delete(*child, child, key, key_len, depth+1);
    }
}

/**
 * Deletes a value from the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
void* art_tree::erase(const unsigned char* key, uint32_t key_len) {
  art_leaf* l = recursive_delete(root_, &root_, key, key_len, 0);
  if (l) {
    void* old = l->value;
    size_--;
    destroy_leaf(l);
    return old;
  }
  return NULL;
}

// Recursively iterates over the tree
static int recursive_iter(art_node *n, art_callback cb, void *data) {
    int i, idx, res;
    union node_ptr p = {n};

    // Handle base cases
    if (!n) return 0;
    if (IS_LEAF(n)) {
        art_leaf *l = LEAF_RAW(n);
        return cb(data, l->key, l->key_len, l->value);
    }

    switch (n->type) {
        case NODE4:
            for (i=0; i < n->num_children; i++) {
                res = recursive_iter(p.n4->children[i], cb, data);
                if (res) return res;
            }
            break;

        case NODE16:
            for (i=0; i < n->num_children; i++) {
                res = recursive_iter(p.n16->children[i], cb, data);
                if (res) return res;
            }
            break;

        case NODE48:
            for (i=0; i < 256; i++) {
                idx = p.n48->keys[i];
                if (!idx) continue;

                res = recursive_iter(p.n48->children[idx-1], cb, data);
                if (res) return res;
            }
            break;

        case NODE256:
            for (i=0; i < 256; i++) {
                if (!p.n256->children[i]) continue;
                res = recursive_iter(p.n256->children[i], cb, data);
                if (res) return res;
            }
            break;

        default:
            abort();
    }
    return 0;
}

/**
 * Iterates through the entries pairs in the map,
 * invoking a callback for each. The call back gets a
 * key, value for each and returns an integer stop value.
 * If the callback returns non-zero, then the iteration stops.
 * @arg t The tree to iterate over
 * @arg cb The callback function to invoke
 * @arg data Opaque handle passed to the callback
 * @return 0 on success, or the return of the callback.
 */
int art_tree::iter(art_callback cb, void* data) {
  return recursive_iter(root_, cb, data);
}

/**
 * Checks if a leaf prefix matches
 */
static bool leaf_prefix_matches(const art_leaf *n, const unsigned char *prefix,
                                int prefix_len) {
    // Fail if the key length is too short
    if (n->key_len < (uint32_t)prefix_len) {
        return false;
    }

    // Compare the keys
    return memcmp(n->key, prefix, prefix_len) == 0;
}

/**
 * Helper function for prefix iteration.
 * In some cases, such as when the relative key is longer than
 * ART_MAX_PREFIX_LEN, and especially after a series of inserts and deletes has
 * churned things up, the iterator locates a potential for matching within a
 * sub-tree that has shorter prefixes than desired (it calls minimum() to find
 * the candidate).  We need to filter these before calling the user supplied
 * iterator callback or else risk incorrect results.
 */

struct prefix_iterator_state {
    const unsigned char *key;
    uint32_t key_len;
    art_callback cb;
    void *data;
};

static int prefix_iterator_callback(void *data, const unsigned char *key,
                                    uint32_t key_len, void *value) {
    auto state = (prefix_iterator_state *)data;

    if (key_len < state->key_len) {
        // Can't match, keep iterating
        return 0;
    }

    if (memcmp(key, state->key, state->key_len) != 0) {
        // Prefix doesn't match, keep iterating
        return 0;
    }

    // Prefix matches, it is valid to call the user iterator callback
    return state->cb(state->data, key, key_len, value);
}

/**
 * Iterates through the entries pairs in the map,
 * invoking a callback for each that matches a given prefix.
 * The call back gets a key, value for each and returns an integer stop value.
 * If the callback returns non-zero, then the iteration stops.
 * @arg t The tree to iterate over
 * @arg prefix The prefix of keys to read
 * @arg prefix_len The length of the prefix
 * @arg cb The callback function to invoke
 * @arg data Opaque handle passed to the callback
 * @return 0 on success, or the return of the callback.
 */
int art_tree::iterPrefix(
    const unsigned char* key,
    uint32_t key_len,
    art_callback cb,
    void* data) {
  art_node** child;
  art_node* n = root_;
  uint32_t prefix_len, depth = 0;
  struct prefix_iterator_state state = {key, key_len, cb, data};
  while (n) {
    // Might be a leaf
    if (IS_LEAF(n)) {
      n = (art_node*)LEAF_RAW(n);
      // Check if the expanded path matches
      if (leaf_prefix_matches((art_leaf*)n, key, key_len)) {
        art_leaf* l = (art_leaf*)n;
        return cb(data, l->key, l->key_len, l->value);
      }
      return 0;
    }

    // If the depth matches the prefix, we need to handle this node
    if (depth == key_len) {
      art_leaf* l = n->minimum();
      if (leaf_prefix_matches(l, key, key_len)) {
        return recursive_iter(n, prefix_iterator_callback, &state);
      }
      return 0;
    }

    // Bail if the prefix does not match
    if (n->partial_len) {
      prefix_len = prefix_mismatch(n, key, key_len, depth);

      // If there is no match, search is terminated
      if (!prefix_len)
        return 0;

      // If we've matched the prefix, iterate on this node
      else if (depth + prefix_len == key_len) {
        return recursive_iter(n, prefix_iterator_callback, &state);
      }

      // if there is a full match, go deeper
      depth = depth + n->partial_len;
    }

    if (depth > key_len) {
      return 0;
    }

    // Recursively search
    child = n->findChild(key_at(key, key_len, depth));
    n = (child) ? *child : NULL;
    depth++;
  }
  return 0;
}

// vim:ts=4:sw=4:
