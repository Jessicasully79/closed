/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/backends/cpu/runtime/xnnpack/xnn_threadpool.h"

#include <cstddef>
#include <cstdint>
#include <memory>

#include "pthreadpool.h"
#include "xla/backends/cpu/runtime/xnnpack/parallel_loop_runner.h"
#include "tsl/platform/env.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/threadpool.h"

#define EIGEN_USE_THREADS
#include "unsupported/Eigen/CXX11/Tensor"

// `pthreadpool` API implementation on top of ParallelLoopRunner.
//
// When building with `pthreadpool_header_only` config, `pthreadpool` becomes a
// header-only library, and we implement the API on top of ParallelLoopRunner.
//
// At link time `pthreadpool` symbols resolved to our own implementation. This
// is a temporary hack around the fact that it's impossible to customize
// `pthreadpool` implementation at run time. The downsize is that it's
// impossible to have two `pthreadpool` implementations linked into the same
// binary.
//
// WARNING: This is under construction and implements only the subset of the API
// surface which is needed by XNNPack uses inside XLA.

namespace xla::cpu {

bool IsCustomPthreadpoolEnabled() {
#if defined(XLA_CPU_USE_CUSTOM_PTHREADPOOL)
  return true;
#else
  return false;
#endif  // XLA_CPU_USE_CUSTOM_PTHREADPOOL
}

namespace {

class Pthreadpool {
 public:
  virtual ~Pthreadpool() = default;
  virtual ParallelLoopRunner* runner() = 0;
};

// Wraps user-provided parallel loop runner into the custom pthreadpool.
class WrappedParallelLoopRunner : public Pthreadpool {
 public:
  explicit WrappedParallelLoopRunner(ParallelLoopRunner* runner)
      : runner_(runner) {}
  ParallelLoopRunner* runner() final { return runner_; }

 private:
  ParallelLoopRunner* runner_;
};

// Wraps newly created thread pool into the custom pthreadpool.
class OwnedParallelLoopRunner : public Pthreadpool {
 public:
  explicit OwnedParallelLoopRunner(size_t threads_count)
      : thread_pool_(tsl::Env::Default(), "xnn_threadpool", threads_count),
        device_(thread_pool_.AsEigenThreadPool(), threads_count),
        runner_(&device_) {}

  ParallelLoopRunner* runner() final { return &runner_; }

 private:
  tsl::thread::ThreadPool thread_pool_;
  Eigen::ThreadPoolDevice device_;
  ParallelLoopRunner runner_;
};

}  // namespace

pthreadpool_t CreatePthreadpool(ParallelLoopRunner* runner) {
  if (IsCustomPthreadpoolEnabled()) {
    return reinterpret_cast<pthreadpool_t>(
        std::make_unique<WrappedParallelLoopRunner>(runner).release());
  }
  LOG(FATAL) << "To use custom pthreadpool, build with "
                "`--define pthreadpool_header_only=true`";
}

static pthreadpool_t CreatePthreadpool(size_t threads_count) {  // NOLINT
  if (IsCustomPthreadpoolEnabled()) {
    return reinterpret_cast<pthreadpool_t>(
        std::make_unique<OwnedParallelLoopRunner>(threads_count).release());
  }
  LOG(FATAL) << "To use custom pthreadpool, build with "
                "`--define pthreadpool_header_only=true`";
}

static Pthreadpool* Cast(pthreadpool_t threadpool) {
  return reinterpret_cast<Pthreadpool*>(threadpool);
}

xla::cpu::ParallelLoopRunner* GetParallelLoopRunner(pthreadpool_t threadpool) {
  return IsCustomPthreadpoolEnabled() ? Cast(threadpool)->runner() : nullptr;
}

//===----------------------------------------------------------------------===//
// C++ implementation of the subset of `pthreadpool` C API.
//===----------------------------------------------------------------------===//

static void DestroyPthreadpool(pthreadpool_t threadpool) {  // NOLINT
  delete Cast(threadpool);
}

static size_t GetThreadsCount(pthreadpool_t threadpool) {  // NOLINT
  return Cast(threadpool)->runner()->num_threads();
}

static void Parallelize1dTile1d(  // NOLINT
    pthreadpool_t threadpool, pthreadpool_task_1d_tile_1d_t function,
    void* context, size_t range, size_t tile, uint32_t flags) {
  ParallelLoopRunner::Task1D task = [function, context](size_t offset,
                                                        size_t extent) {
    (*function)(context, offset, extent);
  };

  Cast(threadpool)->runner()->Parallelize(range, tile, task);
}

}  // namespace xla::cpu

#if defined(XLA_CPU_USE_CUSTOM_PTHREADPOOL)

extern "C" pthreadpool_t pthreadpool_create(size_t threads_count) {
  return xla::cpu::CreatePthreadpool(threads_count);
}

extern "C" void pthreadpool_destroy(pthreadpool_t threadpool) {
  xla::cpu::DestroyPthreadpool(threadpool);
}

extern "C" size_t pthreadpool_get_threads_count(pthreadpool_t threadpool) {
  return xla::cpu::GetThreadsCount(threadpool);
}

extern "C" void pthreadpool_parallelize_1d(pthreadpool_t threadpool,
                                           pthreadpool_task_1d_t function,
                                           void* context, size_t range,
                                           uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_1d_with_thread(
    pthreadpool_t threadpool, pthreadpool_task_1d_with_thread_t function,
    void* context, size_t range, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_1d_with_uarch(
    pthreadpool_t threadpool, pthreadpool_task_1d_with_id_t function,
    void* context, uint32_t default_uarch_index, uint32_t max_uarch_index,
    size_t range, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_1d_tile_1d(
    pthreadpool_t threadpool, pthreadpool_task_1d_tile_1d_t function,
    void* context, size_t range, size_t tile, uint32_t flags) {
  xla::cpu::Parallelize1dTile1d(threadpool, function, context, range, tile,
                                flags);
}

extern "C" void pthreadpool_parallelize_2d(pthreadpool_t threadpool,
                                           pthreadpool_task_2d_t function,
                                           void* context, size_t range_i,
                                           size_t range_j, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_2d_with_thread(
    pthreadpool_t threadpool, pthreadpool_task_2d_with_thread_t function,
    void* context, size_t range_i, size_t range_j, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_2d_tile_1d(
    pthreadpool_t threadpool, pthreadpool_task_2d_tile_1d_t function,
    void* context, size_t range_i, size_t range_j, size_t tile_j,
    uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_2d_tile_1d_with_uarch(
    pthreadpool_t threadpool, pthreadpool_task_2d_tile_1d_with_id_t function,
    void* context, uint32_t default_uarch_index, uint32_t max_uarch_index,
    size_t range_i, size_t range_j, size_t tile_j, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_2d_tile_1d_with_uarch_with_thread(
    pthreadpool_t threadpool,
    pthreadpool_task_2d_tile_1d_with_id_with_thread_t function, void* context,
    uint32_t default_uarch_index, uint32_t max_uarch_index, size_t range_i,
    size_t range_j, size_t tile_j, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_2d_tile_2d(
    pthreadpool_t threadpool, pthreadpool_task_2d_tile_2d_t function,
    void* context, size_t range_i, size_t range_j, size_t tile_i, size_t tile_j,
    uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_2d_tile_2d_with_uarch(
    pthreadpool_t threadpool, pthreadpool_task_2d_tile_2d_with_id_t function,
    void* context, uint32_t default_uarch_index, uint32_t max_uarch_index,
    size_t range_i, size_t range_j, size_t tile_i, size_t tile_j,
    uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_3d(pthreadpool_t threadpool,
                                           pthreadpool_task_3d_t function,
                                           void* context, size_t range_i,
                                           size_t range_j, size_t range_k,
                                           uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_3d_tile_1d(
    pthreadpool_t threadpool, pthreadpool_task_3d_tile_1d_t function,
    void* context, size_t range_i, size_t range_j, size_t range_k,
    size_t tile_k, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_3d_tile_1d_with_thread(
    pthreadpool_t threadpool,
    pthreadpool_task_3d_tile_1d_with_thread_t function, void* context,
    size_t range_i, size_t range_j, size_t range_k, size_t tile_k,
    uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_3d_tile_1d_with_uarch(
    pthreadpool_t threadpool, pthreadpool_task_3d_tile_1d_with_id_t function,
    void* context, uint32_t default_uarch_index, uint32_t max_uarch_index,
    size_t range_i, size_t range_j, size_t range_k, size_t tile_k,
    uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_3d_tile_1d_with_uarch_with_thread(
    pthreadpool_t threadpool,
    pthreadpool_task_3d_tile_1d_with_id_with_thread_t function, void* context,
    uint32_t default_uarch_index, uint32_t max_uarch_index, size_t range_i,
    size_t range_j, size_t range_k, size_t tile_k, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_3d_tile_2d(
    pthreadpool_t threadpool, pthreadpool_task_3d_tile_2d_t function,
    void* context, size_t range_i, size_t range_j, size_t range_k,
    size_t tile_j, size_t tile_k, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_3d_tile_2d_with_uarch(
    pthreadpool_t threadpool, pthreadpool_task_3d_tile_2d_with_id_t function,
    void* context, uint32_t default_uarch_index, uint32_t max_uarch_index,
    size_t range_i, size_t range_j, size_t range_k, size_t tile_j,
    size_t tile_k, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_4d(pthreadpool_t threadpool,
                                           pthreadpool_task_4d_t function,
                                           void* context, size_t range_i,
                                           size_t range_j, size_t range_k,
                                           size_t range_l, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_4d_tile_1d(
    pthreadpool_t threadpool, pthreadpool_task_4d_tile_1d_t function,
    void* context, size_t range_i, size_t range_j, size_t range_k,
    size_t range_l, size_t tile_l, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_4d_tile_2d(
    pthreadpool_t threadpool, pthreadpool_task_4d_tile_2d_t function,
    void* context, size_t range_i, size_t range_j, size_t range_k,
    size_t range_l, size_t tile_k, size_t tile_l, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_4d_tile_2d_with_uarch(
    pthreadpool_t threadpool, pthreadpool_task_4d_tile_2d_with_id_t function,
    void* context, uint32_t default_uarch_index, uint32_t max_uarch_index,
    size_t range_i, size_t range_j, size_t range_k, size_t range_l,
    size_t tile_k, size_t tile_l, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_5d(pthreadpool_t threadpool,
                                           pthreadpool_task_5d_t function,
                                           void* context, size_t range_i,
                                           size_t range_j, size_t range_k,
                                           size_t range_l, size_t range_m,
                                           uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_5d_tile_1d(
    pthreadpool_t threadpool, pthreadpool_task_5d_tile_1d_t function,
    void* context, size_t range_i, size_t range_j, size_t range_k,
    size_t range_l, size_t range_m, size_t tile_m, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_5d_tile_2d(
    pthreadpool_t threadpool, pthreadpool_task_5d_tile_2d_t function,
    void* context, size_t range_i, size_t range_j, size_t range_k,
    size_t range_l, size_t range_m, size_t tile_l, size_t tile_m,
    uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_6d(pthreadpool_t threadpool,
                                           pthreadpool_task_6d_t function,
                                           void* context, size_t range_i,
                                           size_t range_j, size_t range_k,
                                           size_t range_l, size_t range_m,
                                           size_t range_n, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_6d_tile_1d(
    pthreadpool_t threadpool, pthreadpool_task_6d_tile_1d_t function,
    void* context, size_t range_i, size_t range_j, size_t range_k,
    size_t range_l, size_t range_m, size_t range_n, size_t tile_n,
    uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

extern "C" void pthreadpool_parallelize_6d_tile_2d(
    pthreadpool_t threadpool, pthreadpool_task_6d_tile_2d_t function,
    void* context, size_t range_i, size_t range_j, size_t range_k,
    size_t range_l, size_t range_m, size_t range_n, size_t tile_m,
    size_t tile_n, uint32_t flags) {
  LOG(FATAL) << "Not implemented";
}

#endif  // XLA_CPU_USE_CUSTOM_PTHREADPOOL
