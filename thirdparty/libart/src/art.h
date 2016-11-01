#ifndef ART_H
#define ART_H
#include "config.h"
#include <cstdint>
#include <functional>

#define ART_MAX_PREFIX_LEN 10u

/**
 * Main struct, points to root.
 */
struct art_tree {
  struct Leaf;

  /**
   * This struct is included as part
   * of all the various node sizes
   */
  struct Node {
    enum Node_type : uint8_t { NODE4 = 1, NODE16, NODE48, NODE256 };
    Node_type type;
    uint8_t num_children{0};
    uint32_t partial_len{0};
    unsigned char partial[ART_MAX_PREFIX_LEN];

    virtual ~Node() = default;
    explicit Node(Node_type type);
    Node(Node_type type, const Node& other);
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    Leaf* maximum() const;
    Leaf* minimum() const;
    virtual Node** findChild(unsigned char c) = 0;

    // Returns the number of prefix characters shared between the key and node.
    uint32_t checkPrefix(const unsigned char* key, uint32_t key_len, int depth)
        const;
    // Calculates the index at which the prefixes mismatch
    uint32_t
    prefixMismatch(const unsigned char* key, uint32_t key_len, int depth) const;

    virtual void addChild(Node** ref, unsigned char c, Node* child) = 0;
    virtual void removeChild(Node** ref, unsigned char c, Node** l) = 0;
  };

  struct Node4;
  struct Node16;
  struct Node48;
  struct Node256;

  /**
   * Small node with only 4 children
   */
  struct Node4 : public Node {
    unsigned char keys[4];
    Node* children[4];

    ~Node4();
    Node4();
    explicit Node4(Node16&& n16);
    void addChild(Node** ref, unsigned char c, Node* child) override;
    void removeChild(Node** ref, unsigned char c, Node** l) override;
    Node** findChild(unsigned char c) override;
  };

  /**
   * Node with 16 children
   */
  struct Node16 : public Node {
    unsigned char keys[16];
    Node* children[16];

    ~Node16();
    Node16();
    explicit Node16(Node4&& n4);
    explicit Node16(Node48&& n48);
    void addChild(Node** ref, unsigned char c, Node* child) override;
    void removeChild(Node** ref, unsigned char c, Node** l) override;
    Node** findChild(unsigned char c) override;
  };

  /**
   * Node with 48 children, but
   * a full 256 byte field.
   */
  struct Node48 : public Node {
    unsigned char keys[256];
    Node* children[48];

    ~Node48();
    Node48();
    explicit Node48(Node16&& n16);
    explicit Node48(Node256&& n256);
    void addChild(Node** ref, unsigned char c, Node* child) override;
    void removeChild(Node** ref, unsigned char c, Node** l) override;
    Node** findChild(unsigned char c) override;
  };

  /**
   * Full node with 256 children
   */
  struct Node256 : public Node {
    Node* children[256];

    ~Node256();
    Node256();
    explicit Node256(Node48&& n48);
    void addChild(Node** ref, unsigned char c, Node* child) override;
    void removeChild(Node** ref, unsigned char c, Node** l) override;
    Node** findChild(unsigned char c) override;
  };

  /**
   * Represents a leaf. These are
   * of arbitrary size, as they include the key.
   */
  struct Leaf {
    void* value;
    uint32_t key_len;
    unsigned char key[1];

    bool matches(const unsigned char* key, uint32_t key_len) const;

    static Leaf* make(const unsigned char* key, uint32_t key_len, void* value);

    uint32_t longestCommonPrefix(const Leaf* other, int depth) const;
    bool prefixMatches(const unsigned char* prefix, uint32_t prefix_len) const;
  };

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
   */
  void insert(const unsigned char* key, uint32_t key_len, void* value);

  /**
   * Deletes a value from the ART tree
   * @arg key The key
   * @arg key_len The length of the key
   * @return true if the item was erased, false otherwise.
   */
  bool erase(const unsigned char* key, uint32_t key_len);

  /**
   * Searches for a value in the ART tree
   * @arg key The key
   * @arg key_len The length of the key
   * @return NULL if the item was not found, otherwise
   * the value pointer is returned.
   */
  void* search(const unsigned char* key, uint32_t key_len) const;

  /**
   * Searches for the longest prefix match for the input key.
   * @arg key The key
   * @arg key_len The length of the key
   * @return NULL if no match was not found, otherwise
   * the leaf node with the longest matching prefix is returned.
   */
  Leaf* longestMatch(const unsigned char* key, uint32_t key_len) const;

  /**
   * Returns the minimum valued leaf
   * @return The minimum leaf or NULL
   */
  Leaf* minimum() const;

  /**
   * Returns the maximum valued leaf
   * @return The maximum leaf or NULL
   */
  Leaf* maximum() const;

  using art_callback = std::function<
      int(void* data, const unsigned char* key, uint32_t key_len, void* value)>;

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

 private:
  Node* root_;
  uint64_t size_;

  void recursiveInsert(
      Node* n,
      Node** ref,
      const unsigned char* key,
      int key_len,
      void* value,
      int depth,
      int* old);
  Leaf* recursiveDelete(
      Node* n,
      Node** ref,
      const unsigned char* key,
      int key_len,
      int depth);
  int recursiveIter(Node* n, art_callback cb, void* data);
};

#endif
