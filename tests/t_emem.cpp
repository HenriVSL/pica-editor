// t_emem.cpp - suite "emem": allocation wrappers, fault injection, Vec<T>, EBytes, MemSource/ViewSource.
// Owner after Wave 0: overseer.
#include "tf.h"
#include "esource.h"
#include <string.h>

struct Pod { u32 a; u16 b; u8 c; u8 d; };

static void test_alloc(TF* tf)
{
  u32 base = e_alloc_count();
  void* p = e_alloc(0);
  void* q = e_alloc(0);
  void* r;
  TF_CHECK(p != 0);
  TF_CHECK(q != 0 && q != p);
  TF_EQ_U(e_alloc_count(), base + 2);
  r = e_realloc(p, 100);
  TF_CHECK(r != 0);
  memset(r, 0xAB, 100);
  TF_EQ_U(e_alloc_count(), base + 2);
  e_free(r);
  e_free(q);
  e_free(0);
  TF_EQ_U(e_alloc_count(), base);
  p = e_realloc(0, 10);                 // realloc(NULL) allocates
  TF_CHECK(p != 0);
  TF_EQ_U(e_alloc_count(), base + 1);
  e_free(p);
  p = e_valloc(1 << 20);
  TF_CHECK(p != 0);
  if (p) { memset(p, 1, 1 << 20); }
  TF_EQ_U(e_alloc_count(), base + 1);
  e_vfree(p, 1 << 20);
  e_vfree(0, 0);
  TF_EQ_U(e_alloc_count(), base);
}

static void test_fail_after(TF* tf)
{
  void* a;
  void* b;
  void* c;
  u32 base = e_alloc_count();
  e_test_fail_after(2);                 // the 2nd following allocation fails, once
  a = e_alloc(8);
  b = e_alloc(8);
  c = e_alloc(8);
  TF_CHECK(a != 0);
  TF_CHECK(b == 0);
  TF_CHECK(c != 0);
  e_free(a); e_free(c);
  e_test_fail_after(2);
  a = e_alloc(8);
  TF_CHECK(a != 0);
  b = e_realloc(a, 64);                 // realloc fails, a unchanged
  TF_CHECK(b == 0);
  if (b) a = b;
  e_free(a);
  e_test_fail_after(1);
  a = e_valloc(4096);
  TF_CHECK(a == 0);
  e_test_fail_after(-1);
  a = e_alloc(1);
  TF_CHECK(a != 0);
  e_free(a);
  TF_EQ_U(e_alloc_count(), base);
}

static void test_vec(TF* tf)
{
  Vec<u32> v;
  Vec<u32> w;
  Vec<Pod> pv;
  Pod pod;
  u32 i, ok;
  u32 ins[3] = { 100, 101, 102 };
  TF_CHECK(v.empty());
  TF_EQ_U(v.size(), 0);
  for (i = 0; i < 1000; i++) TF_EQ_I(v.push(i), E_OK);
  TF_EQ_U(v.size(), 1000);
  ok = 1;
  for (i = 0; i < 1000; i++) if (v[i] != i) ok = 0;
  TF_CHECK(ok);
  TF_EQ_U(v.back(), 999);
  v.pop();
  TF_EQ_U(v.size(), 999);
  TF_EQ_I(v.insert(1, ins, 3), E_OK);
  TF_EQ_U(v.size(), 1002);
  TF_EQ_U(v[0], 0); TF_EQ_U(v[1], 100); TF_EQ_U(v[3], 102); TF_EQ_U(v[4], 1);
  v.erase(1, 3);
  TF_EQ_U(v.size(), 999);
  TF_EQ_U(v[1], 1);
  v.erase(990, 100);                    // clamps to the end
  TF_EQ_U(v.size(), 990);
  v.erase(5000, 1);                     // out of range: no-op
  TF_EQ_U(v.size(), 990);
  TF_EQ_I(v.insert(v.size(), ins, 3), E_OK);   // append via insert
  TF_EQ_U(v.back(), 102);
  TF_EQ_I(v.resize(2000), E_OK);
  TF_EQ_U(v[1999], 0);                  // zero-filled
  TF_EQ_U(v[1500], 0);
  TF_EQ_I(v.resize(10), E_OK);
  TF_EQ_U(v.size(), 10);
  TF_EQ_U(v[9], 9);
  v.swap(w);
  TF_EQ_U(v.size(), 0);
  TF_EQ_U(w.size(), 10);
  w.clear();
  TF_EQ_U(w.size(), 0);
  TF_CHECK(w.data() != 0);              // clear keeps capacity
  w.release();
  TF_CHECK(w.data() == 0);
  TF_EQ_I(v.reserve(100), E_OK);
  TF_EQ_U(v.size(), 0);
  pod.a = 1; pod.b = 2; pod.c = 3; pod.d = 4;
  TF_EQ_I(pv.push(pod), E_OK);
  TF_EQ_U(pv[0].b, 2);
  // OOM: push fails and leaves the vector unchanged
  v.release();
  for (i = 0; i < 8; i++) v.push(i);
  e_test_fail_after(1);
  TF_EQ_I(v.push(8), E_NOMEM);
  e_test_fail_after(-1);
  TF_EQ_U(v.size(), 8);
  TF_EQ_U(v[7], 7);
  e_test_fail_after(1);
  TF_EQ_I(v.resize(100000), E_NOMEM);
  e_test_fail_after(-1);
  TF_EQ_U(v.size(), 8);
}

static void test_ebytes(TF* tf)
{
  EBytes b;
  TF_EQ_I(b.append("abc", 3), E_OK);
  TF_EQ_I(b.append("", 0), E_OK);
  TF_EQ_I(b.append("de", 2), E_OK);
  TF_EQ_U(b.size(), 5);
  TF_EQ_MEM(b.data(), "abcde", 5);
}

static void test_sources(TF* tf)
{
  MemSource m;
  u8 buf[16];
  epos avail = 99;
  const u8* p;
  u8* blk;
  ViewSource v((const u8*)"hello world", 11);
  TF_EQ_U(m.size(), 0);
  TF_CHECK(m.get(0, &avail) == 0);
  TF_EQ_U(avail, 0);
  TF_EQ_I(m.copy_from((const u8*)"0123456789", 10), E_OK);
  TF_EQ_U(m.size(), 10);
  p = m.get(3, &avail);
  TF_CHECK(p != 0 && *p == '3');
  TF_EQ_U(avail, 7);
  TF_CHECK(m.get(10, &avail) == 0);
  TF_EQ_I(m.read(2, buf, 5), E_OK);
  TF_EQ_MEM(buf, "23456", 5);
  TF_EQ_I(m.read(8, buf, 3), E_RANGE);
  TF_EQ_I(m.read(10, buf, 0), E_OK);
  blk = (u8*)e_valloc(4);
  if (blk) {
    memcpy(blk, "WXYZ", 4);
    TF_EQ_I(m.adopt(blk, 4, 1), E_OK);
    TF_EQ_U(m.size(), 4);
    TF_EQ_MEM(m.data(), "WXYZ", 4);
  }
  TF_EQ_U(v.size(), 11);
  TF_EQ_I(v.read(6, buf, 5), E_OK);
  TF_EQ_MEM(buf, "world", 5);
  p = v.get(11, &avail);
  TF_CHECK(p == 0);
  TF_EQ_I(v.err(), E_OK);
}

void suite_emem(TF* tf)
{
  test_alloc(tf);
  test_fail_after(tf);
  test_vec(tf);
  test_ebytes(tf);
  test_sources(tf);
}
