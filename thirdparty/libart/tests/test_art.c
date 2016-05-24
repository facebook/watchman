#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <check.h>

#include "art.h"

START_TEST(test_art_init_and_destroy)
{
    art_tree t;
    int res = art_tree_init(&t);
    fail_unless(res == 0);

    fail_unless(art_size(&t) == 0);

    res = art_tree_destroy(&t);
    fail_unless(res == 0);
}
END_TEST

START_TEST(test_art_insert)
{
    art_tree t;
    int res = art_tree_init(&t);
    fail_unless(res == 0);

    int len;
    char buf[512];
    FILE *f = fopen("tests/words.txt", "r");

    uintptr_t line = 1;
    while (fgets(buf, sizeof buf, f)) {
        len = strlen(buf);
        buf[len-1] = '\0';
        fail_unless(NULL == art_insert(&t, (unsigned char*)buf, len, (void*)line));
        fail_unless(art_size(&t) == line);
        line++;
    }

    res = art_tree_destroy(&t);
    fail_unless(res == 0);
}
END_TEST

START_TEST(test_art_insert_verylong)
{
    art_tree t;
    int res = art_tree_init(&t);
    fail_unless(res == 0);

    unsigned char key1[300] = {16,0,0,0,7,10,0,0,0,2,17,10,0,0,0,120,10,0,0,0,120,10,0,
      0,0,216,10,0,0,0,202,10,0,0,0,194,10,0,0,0,224,10,0,0,0,
      230,10,0,0,0,210,10,0,0,0,206,10,0,0,0,208,10,0,0,0,232,
      10,0,0,0,124,10,0,0,0,124,2,16,0,0,0,2,12,185,89,44,213,
      251,173,202,211,95,185,89,110,118,251,173,202,199,101,0,
      8,18,182,92,236,147,171,101,150,195,112,185,218,108,246,
      139,164,234,195,58,177,0,8,16,0,0,0,2,12,185,89,44,213,
      251,173,202,211,95,185,89,110,118,251,173,202,199,101,0,
      8,18,180,93,46,151,9,212,190,95,102,178,217,44,178,235,
      29,190,218,8,16,0,0,0,2,12,185,89,44,213,251,173,202,
      211,95,185,89,110,118,251,173,202,199,101,0,8,18,180,93,
      46,151,9,212,190,95,102,183,219,229,214,59,125,182,71,
      108,180,220,238,150,91,117,150,201,84,183,128,8,16,0,0,
      0,2,12,185,89,44,213,251,173,202,211,95,185,89,110,118,
      251,173,202,199,101,0,8,18,180,93,46,151,9,212,190,95,
      108,176,217,47,50,219,61,134,207,97,151,88,237,246,208,
      8,18,255,255,255,219,191,198,134,5,223,212,72,44,208,
      250,180,14,1,0,0,8, '\0'};
    unsigned char key2[303] = {16,0,0,0,7,10,0,0,0,2,17,10,0,0,0,120,10,0,0,0,120,10,0,
      0,0,216,10,0,0,0,202,10,0,0,0,194,10,0,0,0,224,10,0,0,0,
      230,10,0,0,0,210,10,0,0,0,206,10,0,0,0,208,10,0,0,0,232,
      10,0,0,0,124,10,0,0,0,124,2,16,0,0,0,2,12,185,89,44,213,
      251,173,202,211,95,185,89,110,118,251,173,202,199,101,0,
      8,18,182,92,236,147,171,101,150,195,112,185,218,108,246,
      139,164,234,195,58,177,0,8,16,0,0,0,2,12,185,89,44,213,
      251,173,202,211,95,185,89,110,118,251,173,202,199,101,0,
      8,18,180,93,46,151,9,212,190,95,102,178,217,44,178,235,
      29,190,218,8,16,0,0,0,2,12,185,89,44,213,251,173,202,
      211,95,185,89,110,118,251,173,202,199,101,0,8,18,180,93,
      46,151,9,212,190,95,102,183,219,229,214,59,125,182,71,
      108,180,220,238,150,91,117,150,201,84,183,128,8,16,0,0,
      0,3,12,185,89,44,213,251,133,178,195,105,183,87,237,150,
      155,165,150,229,97,182,0,8,18,161,91,239,50,10,61,150,
      223,114,179,217,64,8,12,186,219,172,150,91,53,166,221,
      101,178,0,8,18,255,255,255,219,191,198,134,5,208,212,72,
      44,208,250,180,14,1,0,0,8, '\0'};


    fail_unless(NULL == art_insert(&t, key1, 299, (void*)key1));
    fail_unless(NULL == art_insert(&t, key2, 302, (void*)key2));
    art_insert(&t, key2, 302, (void*)key2);
    fail_unless(art_size(&t) == 2);

    res = art_tree_destroy(&t);
    fail_unless(res == 0);
}
END_TEST

START_TEST(test_art_insert_search)
{
    art_tree t;
    int res = art_tree_init(&t);
    fail_unless(res == 0);

    int len;
    char buf[512];
    FILE *f = fopen("tests/words.txt", "r");

    uintptr_t line = 1;
    while (fgets(buf, sizeof buf, f)) {
        len = strlen(buf);
        buf[len-1] = '\0';
        fail_unless(NULL ==
            art_insert(&t, (unsigned char*)buf, len, (void*)line));
        line++;
    }

    // Seek back to the start
    fseek(f, 0, SEEK_SET);

    // Search for each line
    line = 1;
    while (fgets(buf, sizeof buf, f)) {
        len = strlen(buf);
        buf[len-1] = '\0';

        uintptr_t val = (uintptr_t)art_search(&t, (unsigned char*)buf, len);
	fail_unless(line == val, "Line: %d Val: %" PRIuPTR " Str: %s\n", line,
	    val, buf);
        line++;
    }

    // Check the minimum
    art_leaf *l = art_minimum(&t);
    fail_unless(l && strcmp((char*)l->key, "A") == 0);

    // Check the maximum
    l = art_maximum(&t);
    fail_unless(l && strcmp((char*)l->key, "zythum") == 0);

    res = art_tree_destroy(&t);
    fail_unless(res == 0);
}
END_TEST

START_TEST(test_art_insert_delete)
{
    art_tree t;
    int res = art_tree_init(&t);
    fail_unless(res == 0);

    int len;
    char buf[512];
    FILE *f = fopen("tests/words.txt", "r");

    uintptr_t line = 1, nlines;
    while (fgets(buf, sizeof buf, f)) {
        len = strlen(buf);
        buf[len-1] = '\0';
        fail_unless(NULL ==
            art_insert(&t, (unsigned char*)buf, len, (void*)line));
        line++;
    }

    nlines = line - 1;

    // Seek back to the start
    fseek(f, 0, SEEK_SET);

    // Search for each line
    line = 1;
    while (fgets(buf, sizeof buf, f)) {
        len = strlen(buf);
        buf[len-1] = '\0';

        // Search first, ensure all entries still
        // visible
        uintptr_t val = (uintptr_t)art_search(&t, (unsigned char*)buf, len);
	fail_unless(line == val, "Line: %d Val: %" PRIuPTR " Str: %s\n", line,
	    val, buf);

        // Delete, should get lineno back
        val = (uintptr_t)art_delete(&t, (unsigned char*)buf, len);
	fail_unless(line == val, "Line: %d Val: %" PRIuPTR " Str: %s\n", line,
	    val, buf);

        // Check the size
        fail_unless(art_size(&t) == nlines - line);
        line++;
    }

    // Check the minimum and maximum
    fail_unless(!art_minimum(&t));
    fail_unless(!art_maximum(&t));

    res = art_tree_destroy(&t);
    fail_unless(res == 0);
}
END_TEST

int iter_cb(void *data, const unsigned char* key, uint32_t key_len, void *val) {
    uint64_t *out = (uint64_t*)data;
    uintptr_t line = (uintptr_t)val;
    uint64_t mask = (line * (key[0] + key_len));
    out[0]++;
    out[1] ^= mask;
    return 0;
}

START_TEST(test_art_insert_iter)
{
    art_tree t;
    int res = art_tree_init(&t);
    fail_unless(res == 0);

    int len;
    char buf[512];
    FILE *f = fopen("tests/words.txt", "r");

    uint64_t xor_mask = 0;
    uintptr_t line = 1, nlines;
    while (fgets(buf, sizeof buf, f)) {
        len = strlen(buf);
        buf[len-1] = '\0';
        fail_unless(NULL ==
            art_insert(&t, (unsigned char*)buf, len, (void*)line));

        xor_mask ^= (line * (buf[0] + len));
        line++;
    }
    nlines = line - 1;

    uint64_t out[] = {0, 0};
    fail_unless(art_iter(&t, iter_cb, &out) == 0);

    fail_unless(out[0] == nlines);
    fail_unless(out[1] == xor_mask);

    res = art_tree_destroy(&t);
    fail_unless(res == 0);
}
END_TEST

typedef struct {
    int count;
    int max_count;
    const char **expected;
} prefix_data;

static int test_prefix_cb(void *data, const unsigned char *k, uint32_t k_len, void *val) {
    prefix_data *p = (prefix_data*)data;
    fail_unless(p->count < p->max_count);
    fail_unless(memcmp(k, p->expected[p->count], k_len) == 0,
            "Key: %s Expect: %s", k,
            p->expected[p->count]);
    p->count++;
    return 0;
}

START_TEST(test_art_iter_prefix)
{
    art_tree t;
    int res = art_tree_init(&t);
    fail_unless(res == 0);

    const char *s = "api.foo.bar";
    fail_unless(NULL == art_insert(&t, (unsigned char*)s, strlen(s)+1, NULL));

    s = "api.foo.baz";
    fail_unless(NULL == art_insert(&t, (unsigned char*)s, strlen(s)+1, NULL));

    s = "api.foe.fum";
    fail_unless(NULL == art_insert(&t, (unsigned char*)s, strlen(s)+1, NULL));

    s = "abc.123.456";
    fail_unless(NULL == art_insert(&t, (unsigned char*)s, strlen(s)+1, NULL));

    s = "api.foo";
    fail_unless(NULL == art_insert(&t, (unsigned char*)s, strlen(s)+1, NULL));

    s = "api";
    fail_unless(NULL == art_insert(&t, (unsigned char*)s, strlen(s)+1, NULL));

    // Iterate over api
    const char *expected[] = {"api", "api.foe.fum", "api.foo", "api.foo.bar", "api.foo.baz"};
    prefix_data p = { 0, 5, expected };
    fail_unless(!art_iter_prefix(&t, (unsigned char*)"api", 3, test_prefix_cb, &p));
    fail_unless(p.count == p.max_count, "Count: %d Max: %d", p.count, p.max_count);

    // Iterate over 'a'
    const char *expected2[] = {"abc.123.456", "api", "api.foe.fum", "api.foo", "api.foo.bar", "api.foo.baz"};
    prefix_data p2 = { 0, 6, expected2 };
    fail_unless(!art_iter_prefix(&t, (unsigned char*)"a", 1, test_prefix_cb, &p2));
    fail_unless(p2.count == p2.max_count);

    // Check a failed iteration
    prefix_data p3 = { 0, 0, NULL };
    fail_unless(!art_iter_prefix(&t, (unsigned char*)"b", 1, test_prefix_cb, &p3));
    fail_unless(p3.count == 0);

    // Iterate over api.
    const char *expected4[] = {"api.foe.fum", "api.foo", "api.foo.bar", "api.foo.baz"};
    prefix_data p4 = { 0, 4, expected4 };
    fail_unless(!art_iter_prefix(&t, (unsigned char*)"api.", 4, test_prefix_cb, &p4));
    fail_unless(p4.count == p4.max_count, "Count: %d Max: %d", p4.count, p4.max_count);

    // Iterate over api.foo.ba
    const char *expected5[] = {"api.foo.bar"};
    prefix_data p5 = { 0, 1, expected5 };
    fail_unless(!art_iter_prefix(&t, (unsigned char*)"api.foo.bar", 11, test_prefix_cb, &p5));
    fail_unless(p5.count == p5.max_count, "Count: %d Max: %d", p5.count, p5.max_count);

    // Check a failed iteration on api.end
    prefix_data p6 = { 0, 0, NULL };
    fail_unless(!art_iter_prefix(&t, (unsigned char*)"api.end", 7, test_prefix_cb, &p6));
    fail_unless(p6.count == 0);

    // Iterate over empty prefix
    prefix_data p7 = { 0, 6, expected2 };
    fail_unless(!art_iter_prefix(&t, (unsigned char*)"", 0, test_prefix_cb, &p7));
    fail_unless(p7.count == p7.max_count);

    res = art_tree_destroy(&t);
    fail_unless(res == 0);
}
END_TEST

START_TEST(test_art_long_prefix)
{
    art_tree t;
    int res = art_tree_init(&t);
    fail_unless(res == 0);

    uintptr_t v;
    const char *s;

    s = "this:key:has:a:long:prefix:3";
    v = 3;
    fail_unless(NULL == art_insert(&t, (unsigned char*)s, strlen(s)+1, (void*)v));

    s = "this:key:has:a:long:common:prefix:2";
    v = 2;
    fail_unless(NULL == art_insert(&t, (unsigned char*)s, strlen(s)+1, (void*)v));

    s = "this:key:has:a:long:common:prefix:1";
    v = 1;
    fail_unless(NULL == art_insert(&t, (unsigned char*)s, strlen(s)+1, (void*)v));

    // Search for the keys
    s = "this:key:has:a:long:common:prefix:1";
    fail_unless(1 == (uintptr_t)art_search(&t, (unsigned char*)s, strlen(s)+1));

    s = "this:key:has:a:long:common:prefix:2";
    fail_unless(2 == (uintptr_t)art_search(&t, (unsigned char*)s, strlen(s)+1));

    s = "this:key:has:a:long:prefix:3";
    fail_unless(3 == (uintptr_t)art_search(&t, (unsigned char*)s, strlen(s)+1));


    const char *expected[] = {
        "this:key:has:a:long:common:prefix:1",
        "this:key:has:a:long:common:prefix:2",
        "this:key:has:a:long:prefix:3",
    };
    prefix_data p = { 0, 3, expected };
    fail_unless(!art_iter_prefix(&t, (unsigned char*)"this:key:has", 12, test_prefix_cb, &p));
    fail_unless(p.count == p.max_count, "Count: %d Max: %d", p.count, p.max_count);

    res = art_tree_destroy(&t);
    fail_unless(res == 0);
}
END_TEST

START_TEST(test_art_insert_search_uuid)
{
    art_tree t;
    int res = art_tree_init(&t);
    fail_unless(res == 0);

    int len;
    char buf[512];
    FILE *f = fopen("tests/uuid.txt", "r");

    uintptr_t line = 1;
    while (fgets(buf, sizeof buf, f)) {
        len = strlen(buf);
        buf[len-1] = '\0';
        fail_unless(NULL ==
            art_insert(&t, (unsigned char*)buf, len, (void*)line));
        line++;
    }

    // Seek back to the start
    fseek(f, 0, SEEK_SET);

    // Search for each line
    line = 1;
    while (fgets(buf, sizeof buf, f)) {
        len = strlen(buf);
        buf[len-1] = '\0';

        uintptr_t val = (uintptr_t)art_search(&t, (unsigned char*)buf, len);
	fail_unless(line == val, "Line: %d Val: %" PRIuPTR " Str: %s\n", line,
	    val, buf);
        line++;
    }

    // Check the minimum
    art_leaf *l = art_minimum(&t);
    fail_unless(l && strcmp((char*)l->key, "00026bda-e0ea-4cda-8245-522764e9f325") == 0);

    // Check the maximum
    l = art_maximum(&t);
    fail_unless(l && strcmp((char*)l->key, "ffffcb46-a92e-4822-82af-a7190f9c1ec5") == 0);

    res = art_tree_destroy(&t);
    fail_unless(res == 0);
}
END_TEST

