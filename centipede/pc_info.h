// Copyright 2023 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef THIRD_PARTY_CENTIPEDE_PC_INFO_H_
#define THIRD_PARTY_CENTIPEDE_PC_INFO_H_

#include <cstddef>
#include <cstdint>

namespace centipede {

// PCInfo is a pair {PC, bit mask with PC flags}.
// See https://clang.llvm.org/docs/SanitizerCoverage.html#pc-table
struct PCInfo {
  enum PCFlags : uintptr_t {
    kFuncEntry = 1 << 0,  // The PC is the function entry block.
  };

  uintptr_t pc;
  uintptr_t flags;

  bool has_flag(PCFlags f) const { return flags & f; }
};

}  // namespace centipede

#endif  // THIRD_PARTY_CENTIPEDE_PC_INFO_H_
