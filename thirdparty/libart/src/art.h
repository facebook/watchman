#ifndef ART_H
#define ART_H
#include "config.h"
#include <stdint.h>

#define ART_MAX_PREFIX_LEN 10

typedef int(*art_callback)(void *data, const unsigned char *key, uint32_t key_len, void *value);
struct art_leaf;

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

  art_leaf* maximum() const;
  art_leaf* minimum() const;
  art_node** findChild(unsigned char c);
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
struct art_tree {
  art_node* root_;
  uint64_t size_;

  art_tree();
  ~art_tree();

  inline uint64_t size() const {
    return size_;
  }

  void clear();

  /**
   * Inserts a new value into the ART tree
   * @arg key The key
   * @arg key_len The length of the key
   * @arg value Opaque value.
   * @return NULL if the item was newly inserted, otherwise
   * the old value pointer is returned.
   */
  void* insert(const unsigned char* key, int key_len, void* value);

  /**
   * Deletes a value from the ART tree
   * @arg key The key
   * @arg key_len The length of the key
   * @return NULL if the item was not found, otherwise
   * the value pointer is returned.
   */
  void* erase(const unsigned char* key, int key_len);

  /**
   * Searches for a value in the ART tree
   * @arg key The key
   * @arg key_len The length of the key
   * @return NULL if the item was not found, otherwise
   * the value pointer is returned.
   */
  void* search(const unsigned char* key, int key_len) const;

  /**
   * Searches for the longest prefix match for the input key.
   * @arg key The key
   * @arg key_len The length of the key
   * @return NULL if no match was not found, otherwise
   * the leaf node with the longest matching prefix is returned.
   */
  art_leaf* longestMatch(const unsigned char* key, int key_len) const;

  /**
   * Returns the minimum valued leaf
   * @return The minimum leaf or NULL
   */
  art_leaf* minimum() const;

  /**
   * Returns the maximum valued leaf
   * @return The maximum leaf or NULL
   */
  art_leaf* maximum() const;

  /**
   * Iterates through the entries pairs in the map,
   * invoking a callback for each. The call back gets a
   * key, value for each and returns an integer stop value.
   * If the callback returns non-zero, then the iteration stops.
   * @arg cb The callback function to invoke
   * @arg data Opaque handle passed to the callback
   * @return 0 on success, or the return of the callback.
   */
  int iter(art_callback cb, void* data);

  /**
   * Iterates through the entries pairs in the map,
   * invoking a callback for each that matches a given prefix.
   * The call back gets a key, value for each and returns an integer stop value.
   * If the callback returns non-zero, then the iteration stops.
   * @arg prefix The prefix of keys to read
   * @arg prefix_len The length of the prefix
   * @arg cb The callback function to invoke
   * @arg data Opaque handle passed to the callback
   * @return 0 on success, or the return of the callback.
   */
  int iterPrefix(
      const unsigned char* prefix,
      uint32_t prefix_len,
      art_callback cb,
      void* data);
};

#endif
