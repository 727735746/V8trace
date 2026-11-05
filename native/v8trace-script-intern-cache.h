#ifndef V8TRACE_SCRIPT_INTERN_CACHE_H_
#define V8TRACE_SCRIPT_INTERN_CACHE_H_

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <map>

namespace v8trace_script_intern {

struct CacheTraits {
  static constexpr std::size_t kCapacity = 256;

  template <typename Key>
  class Index {
   public:
    bool LookUpOrInsert(std::size_t* iid, const Key& key) {
      if (clock_ == std::numeric_limits<std::size_t>::max())
        std::abort();
      const auto used = ++clock_;
      auto found = values_.find(key);
      if (found != values_.end()) {
        found->second.last_used = used;
        *iid = found->second.iid;
        return true;
      }
      if (next_ == std::numeric_limits<std::size_t>::max())
        std::abort();
      if (values_.size() == kCapacity) {
        auto oldest = std::min_element(values_.begin(), values_.end(),
            [](const auto& left, const auto& right) {
              return left.second.last_used < right.second.last_used;
            });
        values_.erase(oldest);
      }
      *iid = ++next_;
      values_.emplace(key, Entry{*iid, used});
      return false;
    }

   private:
    struct Entry { std::size_t iid, last_used; };
    std::map<Key, Entry> values_;
    std::size_t next_ = 0;
    std::size_t clock_ = 0;
  };
};

}

#endif
