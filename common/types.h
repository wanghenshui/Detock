#pragma once

#include <chrono>
#include <string>

#include "proto/transaction.pb.h"

using namespace std::chrono;

namespace slog {

using Key = std::string;
using KeyReplica = std::string;
using Value = std::string;
using TxnId = uint64_t;
using BatchId = uint32_t;
using SlotId = uint32_t;
using Channel = uint32_t;
using MachineId = int;

const uint32_t DEFAULT_MASTER_REGION_OF_NEW_KEY = 0;

struct Metadata {
  Metadata() = default;
  Metadata(uint32_t m, uint32_t c = 0) : master(m), counter(c) {}
  void operator=(const MasterMetadata& metadata) {
    master = metadata.master();
    counter = metadata.counter();
  }

  uint32_t master = DEFAULT_MASTER_REGION_OF_NEW_KEY;
  uint32_t counter = 0;
};

struct Record {
  Record(const Value& v, uint32_t m, uint32_t c = 0) : value(v), metadata(m, c) {}
  Record() = default;

  Value value;
  Metadata metadata;
};

enum class LockMode { UNLOCKED, READ, WRITE };
enum class AcquireLocksResult { ACQUIRED, WAITING, ABORT };

inline KeyReplica MakeKeyReplica(Key key, uint32_t master) {
  std::string new_key;
  auto master_str = std::to_string(master);
  new_key.reserve(key.length() + master_str.length() + 1);
  new_key += key;
  new_key += ":";
  new_key += master_str;
  return new_key;
}

}  // namespace slog