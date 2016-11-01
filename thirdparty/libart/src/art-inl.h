#include "make_unique.h"

#ifdef __SSE__
#include <emmintrin.h>
#endif
#include <algorithm>
#include <new>

#if defined(__clang__)
# if __has_feature(address_sanitizer)
#  define ART_SANITIZE_ADDRESS 1
# endif
#elif defined (__GNUC__) && \
      (((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ >= 5)) && \
      __SANITIZE_ADDRESS__
# define ART_SANITIZE_ADDRESS 1
#endif

#ifdef _MSC_VER
#include <intrin.h>
uint32_t inline __builtin_ctz(uint32_t x) {
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
// To facilitate this the keyAt() function is used to look up the byte
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
template <typename ValueType>
inline unsigned char art_tree<ValueType>::keyAt(
    const unsigned char* key,
    uint32_t key_len,
    uint32_t idx) {
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
template <typename ValueType>
inline unsigned char art_tree<ValueType>::Leaf::keyAt(uint32_t idx) const {
  return art_tree<ValueType>::keyAt(key, key_len, idx);
}

/**
 * Allocates a node of the given type,
 * initializes to zero and sets the type.
 */

template <typename ValueType>
art_tree<ValueType>::Node::Node(Node_type type) : type(type) {}

template <typename ValueType>
art_tree<ValueType>::Node::Node(Node_type type, const Node& other)
    : type(type),
      num_children(other.num_children),
      partial_len(other.partial_len) {
  memcpy(partial, other.partial, std::min(ART_MAX_PREFIX_LEN, partial_len));
}

// --------------------- Node4

template <typename ValueType>
art_tree<ValueType>::Node4::Node4() : Node(NODE4) {
  memset(keys, 0, sizeof(keys));
}

template <typename ValueType>
art_tree<ValueType>::Node4::Node4(Node16&& n16) : Node(NODE4, n16) {
  memset(keys, 0, sizeof(keys));
  memcpy(keys, n16.keys, n16.num_children * sizeof(keys[0]));
  std::move(
      n16.children.begin(),
      n16.children.begin() + n16.num_children,
      children.begin());

  n16.num_children = 0;
}

template <typename ValueType>
void art_tree<ValueType>::Node4::addChild(
    NodePtr& ref,
    unsigned char c,
    NodePtr&& child) {
  if (this->num_children < 4) {
    int idx;
    for (idx = 0; idx < this->num_children; idx++) {
      if (c < keys[idx]) {
        break;
      }
    }

    // Shift to make room
    memmove(keys + idx + 1, keys + idx, this->num_children - idx);

    std::move_backward(
        children.begin() + idx,
        children.begin() + this->num_children,
        children.begin() + this->num_children + 1);

    // Insert element
    keys[idx] = c;
    children[idx] = std::move(child);
    this->num_children++;

  } else {
    ref = watchman::make_unique<Node16, Deleter>(std::move(*this));
    ref->addChild(ref, c, std::move(child));
  }
}

template <typename ValueType>
typename art_tree<ValueType>::NodePtr* art_tree<ValueType>::Node4::findChild(
    unsigned char c) {
  int i;
  for (i = 0; i < this->num_children; i++) {
    if (keys[i] == c) {
      return &children[i];
    }
  }
  return nullptr;
}

template <typename ValueType>
typename art_tree<ValueType>::NodePtr art_tree<ValueType>::Node4::removeChild(
    NodePtr& ref,
    unsigned char,
    NodePtr* l) {
  auto pos = l - children.data();
  memmove(keys + pos, keys + pos + 1, this->num_children - 1 - pos);

  NodePtr result = std::move(children[pos]);

  std::move(
      children.begin() + pos + 1,
      children.begin() + this->num_children,
      children.begin() + pos);

  this->num_children--;

  // Remove nodes with only a single child
  if (this->num_children == 1) {
    auto child = children[0].get();

    if (!IS_LEAF(child)) {
      // Concatenate the prefixes
      auto prefix = this->partial_len;
      if (prefix < ART_MAX_PREFIX_LEN) {
        this->partial[prefix] = keys[0];
        prefix++;
      }
      if (prefix < ART_MAX_PREFIX_LEN) {
        auto sub_prefix =
            std::min(child->partial_len, ART_MAX_PREFIX_LEN - prefix);
        memcpy(this->partial + prefix, child->partial, sub_prefix);
        prefix += sub_prefix;
      }

      // Store the prefix in the child
      memcpy(
          child->partial, this->partial, std::min(prefix, ART_MAX_PREFIX_LEN));
      child->partial_len += this->partial_len + 1;
    }

    ref = std::move(children[0]);
  }

  return result;
}

// --------------------- Node16

template <typename ValueType>
art_tree<ValueType>::Node16::Node16() : Node(NODE16) {
  memset(keys, 0, sizeof(keys));
}

template <typename ValueType>
art_tree<ValueType>::Node16::Node16(Node4&& n4) : Node(NODE16, n4) {
  memset(keys, 0, sizeof(keys));

  std::move(
      n4.children.begin(),
      n4.children.begin() + this->num_children,
      children.begin());
  memcpy(keys, n4.keys, this->num_children * sizeof(keys[0]));

  n4.num_children = 0;
}

template <typename ValueType>
art_tree<ValueType>::Node16::Node16(Node48&& n48) : Node(NODE16, n48) {
  int i, child = 0;
  memset(keys, 0, sizeof(keys));

  for (i = 0; i < 256; i++) {
    auto pos = n48.keys[i];
    if (pos) {
      keys[child] = i;
      children[child] = std::move(n48.children[pos - 1]);
      child++;
    }
  }

  n48.num_children = 0;
}

template <typename ValueType>
void art_tree<ValueType>::Node16::addChild(
    NodePtr& ref,
    unsigned char c,
    NodePtr&& child) {
  if (this->num_children < 16) {
    unsigned idx;
#ifdef __SSE__
    __m128i cmp;
    unsigned mask, bitfield;

    // Compare the key to all 16 stored keys
    cmp = _mm_cmplt_epi8(_mm_set1_epi8(c), _mm_loadu_si128((__m128i*)keys));

    // Use a mask to ignore children that don't exist
    mask = (1 << this->num_children) - 1;
    bitfield = _mm_movemask_epi8(cmp) & mask;

    // Check if less than any
    if (bitfield) {
      idx = __builtin_ctz(bitfield);
      memmove(keys + idx + 1, keys + idx, this->num_children - idx);
      std::move_backward(
          children.begin() + idx,
          children.begin() + this->num_children,
          children.begin() + this->num_children + 1);
    } else {
      idx = this->num_children;
    }
#else
    for (idx = 0; idx < this->num_children; idx++) {
      if (c < keys[idx]) {
        memmove(keys + idx + 1, keys + idx, this->num_children - idx);
        std::move_backward(
            children.begin() + idx,
            children.begin() + this->num_children,
            children.begin() + this->num_children + 1);
        break;
      }
    }
#endif

    // Set the child
    keys[idx] = c;
    children[idx] = std::move(child);
    this->num_children++;

  } else {
    ref = watchman::make_unique<Node48, Deleter>(std::move(*this));
    ref->addChild(ref, c, std::move(child));
  }
}

template <typename ValueType>
typename art_tree<ValueType>::NodePtr* art_tree<ValueType>::Node16::findChild(
    unsigned char c) {
#ifdef __SSE__
  __m128i cmp;
  int mask, bitfield;

  // Compare the key to all 16 stored keys
  cmp = _mm_cmpeq_epi8(_mm_set1_epi8(c), _mm_loadu_si128((__m128i*)keys));

  // Use a mask to ignore children that don't exist
  mask = (1 << this->num_children) - 1;
  bitfield = _mm_movemask_epi8(cmp) & mask;

  /*
   * If we have a match (any bit set) then we can
   * return the pointer match using ctz to get
   * the index.
   */
  if (bitfield) {
    return &children[__builtin_ctz(bitfield)];
  }
#else
  int i;
  for (i = 0; i < this->num_children; i++) {
    if (keys[i] == c) {
      return &children[i];
    }
  }
#endif
  return nullptr;
}

template <typename ValueType>
typename art_tree<ValueType>::NodePtr art_tree<ValueType>::Node16::removeChild(
    NodePtr& ref,
    unsigned char,
    NodePtr* l) {
  auto pos = l - children.data();
  memmove(keys + pos, keys + pos + 1, this->num_children - 1 - pos);

  NodePtr result = std::move(children[pos]);

  std::move(
      children.begin() + pos + 1,
      children.begin() + this->num_children,
      children.begin() + pos);
  this->num_children--;

  if (this->num_children == 3) {
    ref = watchman::make_unique<Node4, Deleter>(std::move(*this));
  }

  return result;
}

// --------------------- Node48

template <typename ValueType>
art_tree<ValueType>::Node48::Node48() : Node(NODE48) {
  memset(keys, 0, sizeof(keys));
}

template <typename ValueType>
art_tree<ValueType>::Node48::Node48(Node16&& n16) : Node(NODE48, n16) {
  int i;
  memset(keys, 0, sizeof(keys));

  std::move(
      n16.children.begin(),
      n16.children.begin() + n16.num_children,
      children.begin());

  for (i = 0; i < n16.num_children; i++) {
    keys[n16.keys[i]] = i + 1;
  }

  n16.num_children = 0;
}

template <typename ValueType>
art_tree<ValueType>::Node48::Node48(Node256&& n256)
    : art_tree::Node(NODE48, n256) {
  int i, pos = 0;
  memset(keys, 0, sizeof(keys));

  for (i = 0; i < 256; i++) {
    if (n256.children[i]) {
      children[pos] = std::move(n256.children[i]);
      keys[i] = pos + 1;
      pos++;
    }
  }

  n256.num_children = 0;
}

template <typename ValueType>
void art_tree<ValueType>::Node48::addChild(
    NodePtr& ref,
    unsigned char c,
    NodePtr&& child) {
  if (this->num_children < 48) {
    int pos = 0;
    while (children[pos]) {
      pos++;
    }
    children[pos] = std::move(child);
    keys[c] = pos + 1;
    this->num_children++;
  } else {
    ref = watchman::make_unique<Node256, Deleter>(std::move(*this));
    ref->addChild(ref, c, std::move(child));
  }
}

template <typename ValueType>
typename art_tree<ValueType>::NodePtr* art_tree<ValueType>::Node48::findChild(
    unsigned char c) {
  auto i = keys[c];
  if (i) {
    return &children[i - 1];
  }
  return nullptr;
}

template <typename ValueType>
typename art_tree<ValueType>::NodePtr art_tree<ValueType>::Node48::removeChild(
    NodePtr& ref,
    unsigned char c,
    NodePtr*) {
  int pos = keys[c];
  keys[c] = 0;

  NodePtr result = std::move(children[pos - 1]);
  this->num_children--;

  if (this->num_children == 12) {
    ref = watchman::make_unique<Node16, Deleter>(std::move(*this));
  }

  return result;
}

// --------------------- Node256

template <typename ValueType>
art_tree<ValueType>::Node256::Node256(Node48&& n48) : Node(NODE256, n48) {
  int i;
  for (i = 0; i < 256; i++) {
    if (n48.keys[i]) {
      children[i] = std::move(n48.children[n48.keys[i] - 1]);
    }
  }

  n48.num_children = 0;
}

template <typename ValueType>
void art_tree<ValueType>::Node256::addChild(
    NodePtr&,
    unsigned char c,
    NodePtr&& child) {
  this->num_children++;
  children[c] = std::move(child);
}

template <typename ValueType>
typename art_tree<ValueType>::NodePtr* art_tree<ValueType>::Node256::findChild(
    unsigned char c) {
  if (children[c]) {
    return &children[c];
  }
  return nullptr;
}

template <typename ValueType>
typename art_tree<ValueType>::NodePtr art_tree<ValueType>::Node256::removeChild(
    NodePtr& ref,
    unsigned char c,
    NodePtr*) {
  NodePtr result = std::move(children[c]);
  this->num_children--;

  // Resize to a node48 on underflow, not immediately to prevent
  // trashing if we sit on the 48/49 boundary
  if (this->num_children == 37) {
    ref = watchman::make_unique<Node48, Deleter>(std::move(*this));
  }

  return result;
}

/**
 * Initializes an ART tree
 * @return 0 on success.
 */
template <typename ValueType>
art_tree<ValueType>::art_tree() : root_(nullptr), size_(0) {}

template <typename ValueType>
void art_tree<ValueType>::Deleter::operator()(Leaf* leaf) const {
  leaf->~Leaf();
  delete[](char*) leaf;
}

template <typename ValueType>
void art_tree<ValueType>::Deleter::operator()(Node* node) const {
  // Break if null
  if (!node) {
    return;
  }

  // Special case leafs
  if (IS_LEAF(node)) {
    operator()(LEAF_RAW(node));
    return;
  }

  delete node;
}

template <typename ValueType>
art_tree<ValueType>::~art_tree() {
  clear();
}

template <typename ValueType>
void art_tree<ValueType>::clear() {
  root_.reset();
  size_ = 0;
}

/**
 * Returns the number of prefix characters shared between
 * the key and node.
 */
template <typename ValueType>
uint32_t art_tree<ValueType>::Node::checkPrefix(
    const unsigned char* key,
    uint32_t key_len,
    uint32_t depth) const {
  auto max_cmp =
      std::min(std::min(partial_len, ART_MAX_PREFIX_LEN), key_len - depth);
  uint32_t idx;
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
template <typename ValueType>
bool art_tree<ValueType>::Leaf::matches(
    const unsigned char* key,
    uint32_t key_len) const {
  // Fail if the key lengths are different
  if (this->key_len != key_len) {
    return false;
  }

  return memcmp(this->key, key, key_len) == 0;
}

/**
 * Searches for a value in the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
template <typename ValueType>
ValueType* art_tree<ValueType>::search(
    const unsigned char* key,
    uint32_t key_len) const {
  auto n = root_.get();
  uint32_t depth = 0;
  while (n) {
    // Might be a leaf
    if (IS_LEAF(n)) {
      auto leaf = LEAF_RAW(n);
      // Check if the expanded path matches
      if (leaf->matches(key, key_len)) {
        return &leaf->value;
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
    auto child = n->findChild(keyAt(key, key_len, depth));
    n = child ? child->get() : nullptr;
    depth++;
  }
  return nullptr;
}

template <typename ValueType>
typename art_tree<ValueType>::Leaf* art_tree<ValueType>::longestMatch(
    const unsigned char* key,
    uint32_t key_len) const {
  auto n = root_.get();
  uint32_t depth = 0;
  while (n) {
    // Might be a leaf
    if (IS_LEAF(n)) {
      auto leaf = LEAF_RAW(n);
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
    auto child = n->findChild(keyAt(key, key_len, depth));
    n = child ? child->get() : nullptr;
    depth++;
  }
  return nullptr;
}

// Find the minimum leaf under a node
template <typename ValueType>
typename art_tree<ValueType>::Leaf* art_tree<ValueType>::Node::minimum() const {
  uint32_t idx;
  union cnode_ptr p = {this};

  while (p.n) {
    if (IS_LEAF(p.n)) {
      return LEAF_RAW(p.n);
    }

    switch (p.n->type) {
      case NODE4:
        p.n = p.n4->children[0].get();
        break;
      case NODE16:
        p.n = p.n16->children[0].get();
        break;
      case NODE48:
        idx = 0;
        while (!p.n48->keys[idx]) {
          idx++;
        }
        idx = p.n48->keys[idx] - 1;
        p.n = p.n48->children[idx].get();
        break;
      case NODE256:
        idx = 0;
        while (!p.n256->children[idx]) {
          idx++;
        }
        p.n = p.n256->children[idx].get();
        break;
      default:
        abort();
        return nullptr;
    }
  }
  return nullptr;
}

// Find the maximum leaf under a node
template <typename ValueType>
typename art_tree<ValueType>::Leaf* art_tree<ValueType>::Node::maximum() const {
  uint32_t idx;
  union cnode_ptr p = {this};

  while (p.n) {
    if (IS_LEAF(p.n)) {
      return LEAF_RAW(p.n);
    }

    switch (p.n->type) {
      case NODE4:
        p.n = p.n4->children[p.n->num_children - 1].get();
        break;
      case NODE16:
        p.n = p.n16->children[p.n->num_children - 1].get();
        break;
      case NODE48:
        idx = 255;
        while (!p.n48->keys[idx]) {
          idx--;
        }
        idx = p.n48->keys[idx] - 1;
        p.n = p.n48->children[idx].get();
        break;
      case NODE256:
        idx = 255;
        while (!p.n256->children[idx]) {
          idx--;
        }
        p.n = p.n256->children[idx].get();
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
template <typename ValueType>
typename art_tree<ValueType>::Leaf* art_tree<ValueType>::minimum() const {
  return root_->minimum();
}

/**
 * Returns the maximum valued leaf
 */
template <typename ValueType>
typename art_tree<ValueType>::Leaf* art_tree<ValueType>::maximum() const {
  return root_->maximum();
}

// Constructs a new leaf using the provided key.
template <typename ValueType>
template <typename... Args>
typename art_tree<ValueType>::LeafPtr art_tree<ValueType>::Leaf::make(
    const unsigned char* key,
    uint32_t key_len,
    Args&&... args) {
  // Leaf::key is declared as key[1] so sizeof(Leaf) is 1 too many;
  // deduct 1 so that we allocate the perfect size
  auto l = (Leaf*)(new char[sizeof(art_tree::Leaf) + key_len - 1]);
  new (l) Leaf(key, key_len, std::forward<Args>(args)...);
  return LeafPtr(l);
}

template <typename ValueType>
template <typename... Args>
art_tree<ValueType>::Leaf::Leaf(
    const unsigned char* key,
    uint32_t key_len,
    Args&&... args)
    : value(std::forward<Args>(args)...), key_len(key_len) {
  memcpy(this->key, key, key_len);
}

template <typename ValueType>
uint32_t art_tree<ValueType>::Leaf::longestCommonPrefix(
    const Leaf* l2,
    uint32_t depth) const {
  auto max_cmp = std::min(key_len, l2->key_len) - depth;
  uint32_t idx;
  for (idx = 0; idx < max_cmp; idx++) {
    if (key[depth + idx] != l2->key[depth + idx]) {
      return idx;
    }
  }
  return idx;
}

/**
 * Calculates the index at which the prefixes mismatch
 */
template <typename ValueType>
uint32_t art_tree<ValueType>::Node::prefixMismatch(
    const unsigned char* key,
    uint32_t key_len,
    uint32_t depth) const {
  auto max_cmp =
      std::min(std::min(ART_MAX_PREFIX_LEN, partial_len), key_len - depth);
  uint32_t idx;
  for (idx = 0; idx < max_cmp; idx++) {
    if (partial[idx] != key[depth + idx]) {
      return idx;
    }
  }

  // If the prefix is short we can avoid finding a leaf
  if (partial_len > ART_MAX_PREFIX_LEN) {
    // Prefix is longer than what we've checked, find a leaf
    auto l = minimum();
    max_cmp = std::min(l->key_len, key_len) - depth;
    for (; idx < max_cmp; idx++) {
      if (l->key[idx + depth] != key[depth + idx]) {
        return idx;
      }
    }
  }
  return idx;
}

template <typename ValueType>
template <typename... Args>
void art_tree<ValueType>::recursiveInsert(
    NodePtr& ref,
    const unsigned char* key,
    uint32_t key_len,
    uint32_t depth,
    int* old,
    Args&&... args) {
  // If we are at a NULL node, inject a leaf
  if (!ref) {
    ref = LeafToNode(Leaf::make(key, key_len, std::forward<Args>(args)...));
    return;
  }

  // If we are at a leaf, we need to replace it with a node
  if (IS_LEAF(ref.get())) {
    auto l = LEAF_RAW(ref.get());

    // Check if we are updating an existing value
    if (l->matches(key, key_len)) {
      *old = 1;
      l->value = ValueType(std::forward<Args>(args)...);
      return;
    }

    // New value, we must split the leaf into a node4
    NodePtr new_node = watchman::make_unique<Node4, Deleter>();

    // Create a new leaf
    auto l2 = Leaf::make(key, key_len, std::forward<Args>(args)...);

    // Determine longest prefix
    auto longest_prefix = l->longestCommonPrefix(l2.get(), depth);
    new_node->partial_len = longest_prefix;
    memcpy(
        new_node->partial,
        l2->key + depth,
        std::min(ART_MAX_PREFIX_LEN, longest_prefix));

    // Add the leafs to the new node4
    new_node->addChild(
        new_node, l->keyAt(depth + longest_prefix), std::move(ref));

    auto leaf2Weak = l2.get();
    new_node->addChild(
        new_node,
        leaf2Weak->keyAt(depth + longest_prefix),
        LeafToNode(std::move(l2)));

    ref = std::move(new_node);
    return;
  }

  // Check if given node has a prefix
  if (ref->partial_len) {
    // Determine if the prefixes differ, since we need to split
    auto prefix_diff = ref->prefixMismatch(key, key_len, depth);
    if (prefix_diff >= ref->partial_len) {
      depth += ref->partial_len;
      goto RECURSE_SEARCH;
    }

    // Weak ref to current node
    auto origNode = ref.get();

    // Create a new node
    NodePtr new_node = watchman::make_unique<Node4, Deleter>();
    new_node->partial_len = prefix_diff;
    memcpy(
        new_node->partial,
        origNode->partial,
        std::min(ART_MAX_PREFIX_LEN, prefix_diff));

    // Adjust the prefix of the old node
    if (origNode->partial_len <= ART_MAX_PREFIX_LEN) {
      new_node->addChild(
          new_node, origNode->partial[prefix_diff], std::move(ref));
      origNode->partial_len -= (prefix_diff + 1);
      memmove(
          origNode->partial,
          origNode->partial + prefix_diff + 1,
          std::min(ART_MAX_PREFIX_LEN, origNode->partial_len));
    } else {
      origNode->partial_len -= (prefix_diff + 1);
      auto minLeaf = origNode->minimum();
      new_node->addChild(
          new_node, minLeaf->keyAt(depth + prefix_diff), std::move(ref));
      memcpy(
          origNode->partial,
          minLeaf->key + depth + prefix_diff + 1,
          std::min(ART_MAX_PREFIX_LEN, origNode->partial_len));
    }

    // Insert the new leaf
    auto l = Leaf::make(key, key_len, std::forward<Args>(args)...);
    auto leafWeak = l.get();
    new_node->addChild(
        new_node,
        leafWeak->keyAt(depth + prefix_diff),
        LeafToNode(std::move(l)));

    ref = std::move(new_node);
    return;
  }

RECURSE_SEARCH:;
  {
    // Find a child to recurse to
    auto child = ref->findChild(keyAt(key, key_len, depth));
    if (child) {
      recursiveInsert(
          *child, key, key_len, depth + 1, old, std::forward<Args>(args)...);
      return;
    }

    // No child, node goes within us
    auto l = Leaf::make(key, key_len, std::forward<Args>(args)...);
    auto leafWeak = l.get();
    ref->addChild(ref, leafWeak->keyAt(depth), LeafToNode(std::move(l)));
  }
}

template <typename ValueType>
template <typename... Args>
void art_tree<ValueType>::insert(
    const unsigned char* key,
    uint32_t key_len,
    Args&&... args) {
  int old_val = 0;
  recursiveInsert(
      root_, key, key_len, 0, &old_val, std::forward<Args>(args)...);
  if (!old_val) {
    size_++;
  }
}

template <typename ValueType>
typename art_tree<ValueType>::NodePtr art_tree<ValueType>::recursiveDelete(
    NodePtr& ref,
    const unsigned char* key,
    uint32_t key_len,
    uint32_t depth) {
  // Search terminated
  if (!ref) {
    return nullptr;
  }

  // Handle hitting a leaf node
  if (IS_LEAF(ref.get())) {
    auto l = LEAF_RAW(ref.get());
    if (l->matches(key, key_len)) {
      NodePtr result;
      std::swap(result, ref);
      return result;
    }
    return nullptr;
  }

  // Bail if the prefix does not match
  if (ref->partial_len) {
    auto prefix_len = ref->checkPrefix(key, key_len, depth);
    if (prefix_len != std::min(ART_MAX_PREFIX_LEN, ref->partial_len)) {
      return nullptr;
    }
    depth = depth + ref->partial_len;
  }

  // Find child node
  auto child = ref->findChild(keyAt(key, key_len, depth));
  if (!child) {
    return nullptr;
  }

  // If the child is leaf, delete from this node
  if (IS_LEAF(child->get())) {
    auto l = LEAF_RAW(child->get());
    if (l->matches(key, key_len)) {
      return ref->removeChild(ref, keyAt(key, key_len, depth), child);
    }
    return nullptr;
  }
  // Recurse
  return recursiveDelete(*child, key, key_len, depth + 1);
}

/**
 * Deletes a value from the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
template <typename ValueType>
typename art_tree<ValueType>::LeafPtr art_tree<ValueType>::erase(
    const unsigned char* key,
    uint32_t key_len) {
  auto l = recursiveDelete(root_, key, key_len, 0);
  if (l) {
    size_--;
    return NodeToLeaf(std::move(l));
  }
  return nullptr;
}

// Recursively iterates over the tree
template <typename ValueType>
int art_tree<ValueType>::recursiveIter(Node* n, art_callback cb, void* data) {
  int i, idx, res;
  union node_ptr p = {n};

  // Handle base cases
  if (!n) {
    return 0;
  }
  if (IS_LEAF(n)) {
    auto l = LEAF_RAW(n);
    return cb(data, l->key, l->key_len, l->value);
  }

  switch (n->type) {
    case Node_type::NODE4:
      for (i = 0; i < n->num_children; i++) {
        res = recursiveIter(p.n4->children[i].get(), cb, data);
        if (res) {
          return res;
        }
      }
      break;

    case Node_type::NODE16:
      for (i = 0; i < n->num_children; i++) {
        res = recursiveIter(p.n16->children[i].get(), cb, data);
        if (res) {
          return res;
        }
      }
      break;

    case Node_type::NODE48:
      for (i = 0; i < 256; i++) {
        idx = p.n48->keys[i];
        if (!idx) {
          continue;
        }

        res = recursiveIter(p.n48->children[idx - 1].get(), cb, data);
        if (res) {
          return res;
        }
      }
      break;

    case Node_type::NODE256:
      for (i = 0; i < 256; i++) {
        if (!p.n256->children[i]) {
          continue;
        }
        res = recursiveIter(p.n256->children[i].get(), cb, data);
        if (res) {
          return res;
        }
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
template <typename ValueType>
int art_tree<ValueType>::iter(art_callback cb, void* data) {
  return recursiveIter(root_.get(), cb, data);
}

/**
 * Checks if a leaf prefix matches
 */
template <typename ValueType>
bool art_tree<ValueType>::Leaf::prefixMatches(
    const unsigned char* prefix,
    uint32_t prefix_len) const {
  // Fail if the key length is too short
  if (key_len < prefix_len) {
    return false;
  }

  // Compare the keys
  return memcmp(key, prefix, prefix_len) == 0;
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

template <typename ValueType>
struct prefix_iterator_state {
  const unsigned char* key;
  uint32_t key_len;
  typename art_tree<ValueType>::art_callback cb;
  void* data;
};

template <typename ValueType>
inline int prefix_iterator_callback(
    void* data,
    const unsigned char* key,
    uint32_t key_len,
    ValueType& value) {
  auto state = (prefix_iterator_state<ValueType>*)data;

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
template <typename ValueType>
int art_tree<ValueType>::iterPrefix(
    const unsigned char* key,
    uint32_t key_len,
    art_callback cb,
    void* data) {
  auto n = root_.get();
  uint32_t prefix_len, depth = 0;
  struct prefix_iterator_state<ValueType> state = {
    key, key_len, cb, data
  };
  while (n) {
    // Might be a leaf
    if (IS_LEAF(n)) {
      auto l = LEAF_RAW(n);
      // Check if the expanded path matches
      if (l->prefixMatches(key, key_len)) {
        return cb(data, l->key, l->key_len, l->value);
      }
      return 0;
    }

    // If the depth matches the prefix, we need to handle this node
    if (depth == key_len) {
      auto l = n->minimum();
      if (l->prefixMatches(key, key_len)) {
        return recursiveIter(n, prefix_iterator_callback<ValueType>, &state);
      }
      return 0;
    }

    // Bail if the prefix does not match
    if (n->partial_len) {
      prefix_len = n->prefixMismatch(key, key_len, depth);

      // If there is no match, search is terminated
      if (!prefix_len) {
        return 0;
      }
      // If we've matched the prefix, iterate on this node
      else if (depth + prefix_len == key_len) {
        return recursiveIter(n, prefix_iterator_callback<ValueType>, &state);
      }

      // if there is a full match, go deeper
      depth = depth + n->partial_len;
    }

    if (depth > key_len) {
      return 0;
    }

    // Recursively search
    auto child = n->findChild(keyAt(key, key_len, depth));
    n = child ? child->get() : nullptr;
    depth++;
  }
  return 0;
}
