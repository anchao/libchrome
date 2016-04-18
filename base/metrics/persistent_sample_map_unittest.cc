// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/metrics/persistent_sample_map.h"

#include <memory>

#include "base/metrics/persistent_histogram_allocator.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace base {
namespace {

TEST(PersistentSampleMapTest, AccumulateTest) {
  LocalPersistentMemoryAllocator allocator(64 << 10, 0, "");  // 64 KiB

  HistogramSamples::Metadata meta;
  PersistentSparseHistogramDataManager manager(&allocator);
  PersistentSampleMap samples(1, &manager, &meta);

  samples.Accumulate(1, 100);
  samples.Accumulate(2, 200);
  samples.Accumulate(1, -200);
  EXPECT_EQ(-100, samples.GetCount(1));
  EXPECT_EQ(200, samples.GetCount(2));

  EXPECT_EQ(300, samples.sum());
  EXPECT_EQ(100, samples.TotalCount());
  EXPECT_EQ(samples.redundant_count(), samples.TotalCount());
}

TEST(PersistentSampleMapTest, Accumulate_LargeValuesDontOverflow) {
  LocalPersistentMemoryAllocator allocator(64 << 10, 0, "");  // 64 KiB

  HistogramSamples::Metadata meta;
  PersistentSparseHistogramDataManager manager(&allocator);
  PersistentSampleMap samples(1, &manager, &meta);

  samples.Accumulate(250000000, 100);
  samples.Accumulate(500000000, 200);
  samples.Accumulate(250000000, -200);
  EXPECT_EQ(-100, samples.GetCount(250000000));
  EXPECT_EQ(200, samples.GetCount(500000000));

  EXPECT_EQ(75000000000LL, samples.sum());
  EXPECT_EQ(100, samples.TotalCount());
  EXPECT_EQ(samples.redundant_count(), samples.TotalCount());
}

TEST(PersistentSampleMapTest, AddSubtractTest) {
  LocalPersistentMemoryAllocator allocator(64 << 10, 0, "");  // 64 KiB

  HistogramSamples::Metadata meta1;
  PersistentSparseHistogramDataManager manager1(&allocator);
  PersistentSampleMap samples1(1, &manager1, &meta1);
  samples1.Accumulate(1, 100);
  samples1.Accumulate(2, 100);
  samples1.Accumulate(3, 100);

  HistogramSamples::Metadata meta2;
  PersistentSparseHistogramDataManager manager2(&allocator);
  PersistentSampleMap samples2(2, &manager2, &meta2);
  samples2.Accumulate(1, 200);
  samples2.Accumulate(2, 200);
  samples2.Accumulate(4, 200);

  samples1.Add(samples2);
  EXPECT_EQ(300, samples1.GetCount(1));
  EXPECT_EQ(300, samples1.GetCount(2));
  EXPECT_EQ(100, samples1.GetCount(3));
  EXPECT_EQ(200, samples1.GetCount(4));
  EXPECT_EQ(2000, samples1.sum());
  EXPECT_EQ(900, samples1.TotalCount());
  EXPECT_EQ(samples1.redundant_count(), samples1.TotalCount());

  samples1.Subtract(samples2);
  EXPECT_EQ(100, samples1.GetCount(1));
  EXPECT_EQ(100, samples1.GetCount(2));
  EXPECT_EQ(100, samples1.GetCount(3));
  EXPECT_EQ(0, samples1.GetCount(4));
  EXPECT_EQ(600, samples1.sum());
  EXPECT_EQ(300, samples1.TotalCount());
  EXPECT_EQ(samples1.redundant_count(), samples1.TotalCount());
}

TEST(PersistentSampleMapTest, PersistenceTest) {
  LocalPersistentMemoryAllocator allocator(64 << 10, 0, "");  // 64 KiB

  HistogramSamples::Metadata meta12;
  PersistentSparseHistogramDataManager manager1(&allocator);
  PersistentSampleMap samples1(12, &manager1, &meta12);
  samples1.Accumulate(1, 100);
  samples1.Accumulate(2, 200);
  samples1.Accumulate(1, -200);
  EXPECT_EQ(-100, samples1.GetCount(1));
  EXPECT_EQ(200, samples1.GetCount(2));
  EXPECT_EQ(300, samples1.sum());
  EXPECT_EQ(100, samples1.TotalCount());
  EXPECT_EQ(samples1.redundant_count(), samples1.TotalCount());

  PersistentSparseHistogramDataManager manager2(&allocator);
  PersistentSampleMap samples2(12, &manager2, &meta12);
  EXPECT_EQ(samples1.id(), samples2.id());
  EXPECT_EQ(samples1.sum(), samples2.sum());
  EXPECT_EQ(samples1.redundant_count(), samples2.redundant_count());
  EXPECT_EQ(samples1.TotalCount(), samples2.TotalCount());
  EXPECT_EQ(-100, samples2.GetCount(1));
  EXPECT_EQ(200, samples2.GetCount(2));
  EXPECT_EQ(300, samples2.sum());
  EXPECT_EQ(100, samples2.TotalCount());
  EXPECT_EQ(samples2.redundant_count(), samples2.TotalCount());

  EXPECT_EQ(0, samples2.GetCount(3));
  EXPECT_EQ(0, samples1.GetCount(3));
  samples2.Accumulate(3, 300);
  EXPECT_EQ(300, samples2.GetCount(3));
  EXPECT_EQ(300, samples1.GetCount(3));
  EXPECT_EQ(samples1.sum(), samples2.sum());
  EXPECT_EQ(samples1.redundant_count(), samples2.redundant_count());
  EXPECT_EQ(samples1.TotalCount(), samples2.TotalCount());

  EXPECT_EQ(0, samples2.GetCount(4));
  EXPECT_EQ(0, samples1.GetCount(4));
  samples1.Accumulate(4, 400);
  EXPECT_EQ(400, samples2.GetCount(4));
  EXPECT_EQ(400, samples1.GetCount(4));
  samples2.Accumulate(4, 4000);
  EXPECT_EQ(4400, samples2.GetCount(4));
  EXPECT_EQ(4400, samples1.GetCount(4));
  EXPECT_EQ(samples1.sum(), samples2.sum());
  EXPECT_EQ(samples1.redundant_count(), samples2.redundant_count());
  EXPECT_EQ(samples1.TotalCount(), samples2.TotalCount());
}

TEST(PersistentSampleMapIteratorTest, IterateTest) {
  LocalPersistentMemoryAllocator allocator(64 << 10, 0, "");  // 64 KiB

  HistogramSamples::Metadata meta;
  PersistentSparseHistogramDataManager manager(&allocator);
  PersistentSampleMap samples(1, &manager, &meta);
  samples.Accumulate(1, 100);
  samples.Accumulate(2, 200);
  samples.Accumulate(4, -300);
  samples.Accumulate(5, 0);

  std::unique_ptr<SampleCountIterator> it = samples.Iterator();

  HistogramBase::Sample min;
  HistogramBase::Sample max;
  HistogramBase::Count count;

  it->Get(&min, &max, &count);
  EXPECT_EQ(1, min);
  EXPECT_EQ(2, max);
  EXPECT_EQ(100, count);
  EXPECT_FALSE(it->GetBucketIndex(NULL));

  it->Next();
  it->Get(&min, &max, &count);
  EXPECT_EQ(2, min);
  EXPECT_EQ(3, max);
  EXPECT_EQ(200, count);

  it->Next();
  it->Get(&min, &max, &count);
  EXPECT_EQ(4, min);
  EXPECT_EQ(5, max);
  EXPECT_EQ(-300, count);

  it->Next();
  EXPECT_TRUE(it->Done());
}

TEST(PersistentSampleMapIteratorTest, SkipEmptyRanges) {
  LocalPersistentMemoryAllocator allocator(64 << 10, 0, "");  // 64 KiB

  HistogramSamples::Metadata meta1;
  PersistentSparseHistogramDataManager manager1(&allocator);
  PersistentSampleMap samples1(1, &manager1, &meta1);
  samples1.Accumulate(5, 1);
  samples1.Accumulate(10, 2);
  samples1.Accumulate(15, 3);
  samples1.Accumulate(20, 4);
  samples1.Accumulate(25, 5);

  HistogramSamples::Metadata meta2;
  PersistentSparseHistogramDataManager manager2(&allocator);
  PersistentSampleMap samples2(2, &manager2, &meta2);
  samples2.Accumulate(5, 1);
  samples2.Accumulate(20, 4);
  samples2.Accumulate(25, 5);

  samples1.Subtract(samples2);

  std::unique_ptr<SampleCountIterator> it = samples1.Iterator();
  EXPECT_FALSE(it->Done());

  HistogramBase::Sample min;
  HistogramBase::Sample max;
  HistogramBase::Count count;

  it->Get(&min, &max, &count);
  EXPECT_EQ(10, min);
  EXPECT_EQ(11, max);
  EXPECT_EQ(2, count);

  it->Next();
  EXPECT_FALSE(it->Done());

  it->Get(&min, &max, &count);
  EXPECT_EQ(15, min);
  EXPECT_EQ(16, max);
  EXPECT_EQ(3, count);

  it->Next();
  EXPECT_TRUE(it->Done());
}

// Only run this test on builds that support catching a DCHECK crash.
#if (!defined(NDEBUG) || defined(DCHECK_ALWAYS_ON)) && GTEST_HAS_DEATH_TEST
TEST(PersistentSampleMapIteratorDeathTest, IterateDoneTest) {
  LocalPersistentMemoryAllocator allocator(64 << 10, 0, "");  // 64 KiB

  HistogramSamples::Metadata meta;
  PersistentSparseHistogramDataManager manager(&allocator);
  PersistentSampleMap samples(1, &manager, &meta);

  std::unique_ptr<SampleCountIterator> it = samples.Iterator();

  EXPECT_TRUE(it->Done());

  HistogramBase::Sample min;
  HistogramBase::Sample max;
  HistogramBase::Count count;
  EXPECT_DEATH(it->Get(&min, &max, &count), "");

  EXPECT_DEATH(it->Next(), "");

  samples.Accumulate(1, 100);
  it = samples.Iterator();
  EXPECT_FALSE(it->Done());
}
#endif
// (!defined(NDEBUG) || defined(DCHECK_ALWAYS_ON)) && GTEST_HAS_DEATH_TEST

}  // namespace
}  // namespace base
