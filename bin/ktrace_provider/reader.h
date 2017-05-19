// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_TRACING_SRC_KTRACE_PROVIDER_READER_H_
#define APPS_TRACING_SRC_KTRACE_PROVIDER_READER_H_

#include <magenta/ktrace.h>

#include "lib/ftl/files/unique_fd.h"
#include "lib/ftl/macros.h"

namespace ktrace_provider {

class Reader {
 public:
  Reader();

  ktrace_header_t* ReadNextRecord();

 private:
  static constexpr size_t kChunkSize{16 * 4 * 1024};

  inline size_t AvailableBytes() const {
    return std::distance(current_, marker_);
  }

  void ReadMoreData();

  ftl::UniqueFD fd_;
  char buffer_[kChunkSize];
  char* current_ = buffer_;
  char* marker_ = buffer_;
  char* end_ = buffer_ + kChunkSize;

  FTL_DISALLOW_COPY_AND_ASSIGN(Reader);
};

}  // namespace ktrace_provider

#endif  // APPS_TRACING_SRC_KTRACE_PROVIDER_READER_H_
