/*
 *
 * Copyright 2017 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

extern "C" {
#include "src/core/lib/debug/stats.h"
}

#include <mutex>
#include <thread>

#include <grpc/grpc.h>
#include <grpc/support/cpu.h>
#include <grpc/support/log.h>
#include <gtest/gtest.h>

namespace grpc {
namespace testing {

class Snapshot {
 public:
  Snapshot() { grpc_stats_collect(&begin_); }

  grpc_stats_data delta() {
    grpc_stats_data now;
    grpc_stats_collect(&now);
    grpc_stats_data delta;
    grpc_stats_diff(&now, &begin_, &delta);
    return delta;
  }

 private:
  grpc_stats_data begin_;
};

TEST(StatsTest, IncCounters) {
  for (int i = 0; i < GRPC_STATS_COUNTER_COUNT; i++) {
    Snapshot snapshot;

    grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
    GRPC_STATS_INC_COUNTER(&exec_ctx, (grpc_stats_counters)i);
    grpc_exec_ctx_finish(&exec_ctx);

    EXPECT_EQ(snapshot.delta().counters[i], 1);
  }
}

TEST(StatsTest, IncSpecificCounter) {
  Snapshot snapshot;

  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  GRPC_STATS_INC_SYSCALL_POLL(&exec_ctx);
  grpc_exec_ctx_finish(&exec_ctx);

  EXPECT_EQ(snapshot.delta().counters[GRPC_STATS_COUNTER_SYSCALL_POLL], 1);
}

static int FindExpectedBucket(int i, int j) {
  if (j < 0) {
    return 0;
  }
  if (j >= grpc_stats_histo_bucket_boundaries[i][grpc_stats_histo_buckets[i]]) {
    return grpc_stats_histo_buckets[i] - 1;
  }
  return std::upper_bound(grpc_stats_histo_bucket_boundaries[i],
                          grpc_stats_histo_bucket_boundaries[i] +
                              grpc_stats_histo_buckets[i],
                          j) -
         grpc_stats_histo_bucket_boundaries[i] - 1;
}

class HistogramTest : public ::testing::TestWithParam<int> {};

TEST_P(HistogramTest, IncHistogram) {
  const int kHistogram = GetParam();
  std::vector<std::thread> threads;
  int cur_bucket = 0;
  auto run = [kHistogram](const std::vector<int>& test_values,
                          int expected_bucket) {
    gpr_log(GPR_DEBUG, "expected_bucket:%d nvalues=%" PRIdPTR, expected_bucket,
            test_values.size());
    for (auto j : test_values) {
      Snapshot snapshot;

      grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
      grpc_stats_inc_histogram[kHistogram](&exec_ctx, j);
      grpc_exec_ctx_finish(&exec_ctx);

      auto delta = snapshot.delta();

      EXPECT_EQ(
          delta
              .histograms[grpc_stats_histo_start[kHistogram] + expected_bucket],
          1)
          << "\nhistogram:" << kHistogram
          << "\nexpected_bucket:" << expected_bucket << "\nj:" << j;
    }
  };
  std::vector<int> test_values;
  for (int j = -1000;
       j < grpc_stats_histo_bucket_boundaries
                   [kHistogram][grpc_stats_histo_buckets[kHistogram] - 1] +
               1000;
       j++) {
    int expected_bucket = FindExpectedBucket(kHistogram, j);
    if (cur_bucket != expected_bucket) {
      threads.emplace_back(
          [test_values, run, cur_bucket]() { run(test_values, cur_bucket); });
      cur_bucket = expected_bucket;
      test_values.clear();
    }
    test_values.push_back(j);
  }
  run(test_values, cur_bucket);
  for (auto& t : threads) {
    t.join();
  }
}

INSTANTIATE_TEST_CASE_P(HistogramTestCases, HistogramTest,
                        ::testing::Range<int>(0, GRPC_STATS_HISTOGRAM_COUNT));

}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc_init();
  int ret = RUN_ALL_TESTS();
  grpc_shutdown();
  return ret;
}
