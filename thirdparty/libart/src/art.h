#ifndef ART_H
#define ART_H
#include "config.h"
#include <stdint.h>

#define ART_MAX_PREFIX_LEN 10

#if defined(__GNUC__) && !defined(__clang__)
# if __STDC_VERSION__ >= 199901L && 402 == (__GNUC__ * 100 + __GNUC_MINOR__)
/*
 * GCC 4.2.2's C99 inline keyword support is pretty broken; avoid. Introduced in
 * GCC 4.2.something, fixed in 4.3.0. So checking for specific major.minor of
 * 4.2 is fine.
 */
#  define BROKEN_GCC_C99_INLINE
# endif
#endif

typedef int(*art_callback)(void *data, const unsigned char *key, uint32_t key_len, void *value);

/**
 * This struct is included as part
 * of all the various node sizes
 */
struct art_node {
  uint8_t type;
  uint8_t num_children{0};
  uint32_t partial_len{0};
  unsigned char partial[ART_MAX_PREFIX_LEN];

  explicit art_node(uint8_t type);
};

/**
 * Small node with only 4 children
 */
struct art_node4 {
  art_node n;
  unsigned char keys[4];
  art_node* children[4];

  art_node4();
};

/**
 * Node with 16 children
 */
struct art_node16 {
  art_node n;
  unsigned char keys[16];
  art_node* children[16];

  art_node16();
};

/**
 * Node with 48 children, but
 * a full 256 byte field.
 */
struct art_node48 {
  art_node n;
  unsigned char keys[256];
  art_node* children[48];

  art_node48();
};

/**
 * Full node with 256 children
 */
struct art_node256 {
  art_node n;
  art_node* children[256];

  art_node256();
};

/**
 * Represents a leaf. These are
 * of arbitrary size, as they include the key.
 */
struct art_leaf {
  void* value;
  uint32_t key_len;
  unsigned char key[1];
};

/**
 * Main struct, points to root.
 */
typedef struct {
    art_node *root;
    uint64_t size;
} art_tree;

/**
 * Initializes an ART tree
 * @return 0 on success.
 */
int art_tree_init(art_tree *t);

/**
 * Destroys an ART tree
 * @return 0 on success.
 */
int art_tree_destroy(art_tree *t);

/**
 * Returns the size of the ART tree.
 */
#ifdef BROKEN_GCC_C99_INLINE
# define art_size(t) ((t)->size)
#else
static inline uint64_t art_size(art_tree *t) {
    return t->size;
}
#endif

/**
 * Inserts a new value into the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @arg value Opaque value.
 * @return NULL if the item was newly inserted, otherwise
 * the old value pointer is returned.
 */
void* art_insert(art_tree *t, const unsigned char *key, int key_len, void *value);

/**
 * Deletes a value from the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
void* art_delete(art_tree *t, const unsigned char *key, int key_len);

/**
 * Searches for a value in the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
void* art_search(const art_tree *t, const unsigned char *key, int key_len);

/**
 * Searches for the longest prefix match for the input key.
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if no match was not found, otherwise
 * the leaf node with the longest matching prefix is returned.
 */
art_leaf *art_longest_match(const art_tree *t, const unsigned char *key,
                        int key_len);

/**
 * Returns the minimum valued leaf
 * @return The minimum leaf or NULL
 */
art_leaf* art_minimum(art_tree *t);

/**
 * Returns the maximum valued leaf
 * @return The maximum leaf or NULL
 */
art_leaf* art_maximum(art_tree *t);

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
int art_iter(art_tree *t, art_callback cb, void *data);

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
int art_iter_prefix(
    art_tree* t,
    const unsigned char* prefix,
    uint32_t prefix_len,
    art_callback cb,
    void* data);

#endif
