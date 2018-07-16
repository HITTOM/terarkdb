//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include "util/arena.h"
#include "util/coding.h"
#include "utilities/write_batch_with_index/write_batch_with_index_internal.h"

#include <terark/fsa/dynamic_patricia_trie.inl>

namespace rocksdb {

template<bool OverwriteKey>
class WriteBatchEntryPTrieIndex : public WriteBatchEntryIndex {
 protected:
  terark::SubPatricia* index_;
  WriteBatchKeyExtractor extractor_;

#pragma pack(push)
#pragma pack(4)
  struct value_vector_t {
    uint32_t size;
    uint32_t loc;
    struct data_t {
      WriteBatchIndexEntry* value;

      operator size_t() const {
        return value->offset;
      }
    };

    bool full() {
      return terark::fast_popcount(size) == 1;
    }
  };
#pragma pack(pop)
  typedef typename value_vector_t::data_t value_wrap_t;

  static constexpr size_t num_words_update = 1024;
  
  struct IteratorImplWithoutOffset {
    terark::Patricia::ReaderToken token;
    WriteBatchKeyExtractor extractor;
    terark::ADFA_LexIterator* iter;
    size_t num_words;

    IteratorImplWithoutOffset(terark::Patricia* index, WriteBatchKeyExtractor e)
      : token(index),
        extractor(e),
        iter(index->adfa_make_iter()),
        num_words(index->num_words()) {
    }
    ~IteratorImplWithoutOffset() {
      delete iter;
    }

    bool Update() {
      if (token.main()->num_words() - num_words > num_words_update) {
        token.update();
        num_words = token.main()->num_words();
        return true;
      }
      return false;
    }
    WriteBatchIndexEntry* GetValue() {
      auto trie = static_cast<terark::MainPatricia*>(token.main());
      return *(WriteBatchIndexEntry**)trie->get_valptr(iter->word_state());
    }
    WriteBatchIndexEntry* Seek(WriteBatchIndexEntry* entry) {
      Update();
      auto key = extractor(entry);
      if (!iter->seek_lower_bound(terark::fstring(key.data(), key.size()))) {
        return nullptr;
      }
      return GetValue();
    }
    WriteBatchIndexEntry* SeekForPrev(WriteBatchIndexEntry* entry) {
      Update();
      auto key = extractor(entry);
      if (!iter->seek_rev_lower_bound(terark::fstring(key.data(), key.size()))) {
        return nullptr;
      }
      return GetValue();
    }
    WriteBatchIndexEntry* SeekToFirst() {
      Update();
      if (!iter->seek_begin()) {
        return nullptr;
      }
      return GetValue();
    }
    WriteBatchIndexEntry* SeekToLast() {
      Update();
      if (!iter->seek_end()) {
        return nullptr;
      }
      return GetValue();
    }
    WriteBatchIndexEntry* Next(WriteBatchIndexEntry* curr) {
      if (Update()) {
        auto key = extractor(curr);
        iter->seek_lower_bound(terark::fstring(key.data(), key.size()));
      }
      if (!iter->incr()) {
        return nullptr;
      }
      return GetValue();
    }
    WriteBatchIndexEntry* Prev(WriteBatchIndexEntry* curr) {
      if (Update()) {
        auto key = extractor(curr);
        iter->seek_lower_bound(terark::fstring(key.data(), key.size()));
      }
      if (!iter->decr()) {
        return nullptr;
      }
      return GetValue();
    }
  };
  struct IteratorImplWithOffset {
    terark::Patricia::ReaderToken token;
    WriteBatchKeyExtractor extractor;
    terark::ADFA_LexIterator* iter;
    uint32_t index;
    size_t num_words;

    IteratorImplWithOffset(terark::Patricia* index,  WriteBatchKeyExtractor e)
      : token(index),
        extractor(e),
        iter(index->adfa_make_iter()),
        index(uint32_t(-1)),
        num_words(index->num_words()) {
    }
    ~IteratorImplWithOffset() {
      delete iter;
    }

    bool Update() {
      if (token.main()->num_words() - num_words > num_words_update) {
        token.update();
        num_words = token.main()->num_words();
        return true;
      }
      return false;
    }
    struct VectorData {
      size_t size;
      const value_wrap_t* data;
    };
    VectorData GetVector() {
      auto trie = static_cast<terark::MainPatricia*>(token.main());
      auto vector_loc = *(uint32_t*)trie->get_valptr(iter->word_state());
      auto vector = (value_vector_t*)trie->mem_get(vector_loc);
      size_t size = vector->size;
      auto data = (value_wrap_t*)trie->mem_get(vector->loc);
      return { size, data };
    }
    WriteBatchIndexEntry* Seek(WriteBatchIndexEntry* entry) {
      Update();
      auto slice_key = extractor(entry);
      auto find_key = terark::fstring(slice_key.data(), slice_key.size());
      if (!iter->seek_lower_bound(find_key)) {
        return nullptr;
      }
      auto vec = GetVector();
      if (iter->word() == find_key) {
        index = terark::upper_bound_0(vec.data, vec.size, entry->offset) - 1;
        if (index != uint32_t(-1)) {
          return vec.data[index].value;
        }
        if (!iter->incr()) {
          return nullptr;
        }
        vec = GetVector();
      }
      assert(iter->word() > find_key);
      index = vec.size - 1;
      return vec.data[index].value;
    }
    WriteBatchIndexEntry* SeekForPrev(WriteBatchIndexEntry* entry) {
      Update();
      auto slice_key = extractor(entry);
      auto find_key = terark::fstring(slice_key.data(), slice_key.size());
      if (!iter->seek_rev_lower_bound(find_key)) {
        return nullptr;
      }
      auto vec = GetVector();
      if (iter->word() == find_key) {
        index = terark::lower_bound_0(vec.data, vec.size, entry->offset);
        if (index != vec.size) {
          return vec.data[index].value;
        }
        if (!iter->decr()) {
          return nullptr;
        }
        vec = GetVector();
      }
      assert(iter->word() < find_key);
      index = 0;
      return vec.data[index].value;
    }
    WriteBatchIndexEntry* SeekToFirst() {
      Update();
      if (!iter->seek_begin()) {
        return nullptr;
      }
      auto vec = GetVector();
      index = vec.size - 1;
      return vec.data[index].value;
    }
    WriteBatchIndexEntry* SeekToLast() {
      Update();
      if (!iter->seek_end()) {
        return nullptr;
      }
      auto vec = GetVector();
      index = 0;
      return vec.data[index].value;
    }
    WriteBatchIndexEntry* Next(WriteBatchIndexEntry* curr) {
      if (Update()) {
        auto key = extractor(curr);
        iter->seek_lower_bound(terark::fstring(key.data(), key.size()));
      }
      if (index-- == 0) {
        if (!iter->incr()) {
          return nullptr;
        }
        auto vec = GetVector();
        index = vec.size - 1;
        return vec.data[index].value;
      } else {
        auto vec = GetVector();
        return vec.data[index].value;
      }
    }
    WriteBatchIndexEntry* Prev(WriteBatchIndexEntry* curr) {
      if (Update()) {
        auto key = extractor(curr);
        iter->seek_lower_bound(terark::fstring(key.data(), key.size()));
      }
      auto vec = GetVector();
      if (++index == vec.size) {
        if (!iter->decr()) {
          return nullptr;
        }
        vec = GetVector();
        index = 0;
      }
      return vec.data[index].value;
    }
  };
  typedef typename std::conditional<OverwriteKey,
                                    IteratorImplWithoutOffset,
                                    IteratorImplWithOffset
                                    >::type IteratorImpl;

  class PTrieIterator : public WriteBatchEntryIndex::Iterator {
    typedef terark::Patricia::ReaderToken token_t;
   public:
    PTrieIterator(terark::Patricia* index, WriteBatchKeyExtractor e)
      : impl_(new IteratorImpl(index, e)),
        key_(nullptr) {
    }
    IteratorImpl* impl_;
    WriteBatchIndexEntry* key_;
    ~PTrieIterator() {
      delete impl_;
    }

   public:
    virtual bool Valid() const override {
      return key_ != nullptr;
    }
    virtual void SeekToFirst() override {
      key_ = impl_->SeekToFirst();
    }
    virtual void SeekToLast() override {
      key_ = impl_->SeekToLast();
    }
    virtual void Seek(WriteBatchIndexEntry* target) override {
      key_ = impl_->Seek(target);
    }
    virtual void SeekForPrev(WriteBatchIndexEntry* target) override {
      key_ = impl_->SeekForPrev(target);
    }
    virtual void Next() override {
      key_ = impl_->Next(key_);
    }
    virtual void Prev() override {
      key_ = impl_->Prev(key_);
    }
    virtual WriteBatchIndexEntry* key() const override {
      return key_;
    }
  };

 public:
   WriteBatchEntryPTrieIndex(terark::SubPatricia* index, WriteBatchKeyExtractor e,
                             const Comparator* c, Arena* a)
      : index_(index),
        extractor_(e) {
  }
  ~WriteBatchEntryPTrieIndex() {
    index_->~SubPatricia();
  }

  static constexpr size_t trie_value_size =
      OverwriteKey ? sizeof(void*) : sizeof(uint32_t);

  virtual Iterator* NewIterator() override {
    return new PTrieIterator(index_, extractor_);
  }
  virtual void NewIterator(IteratorStorage& storage) override {
    static_assert(sizeof(PTrieIterator) <= sizeof storage.buffer,
                  "Need larger buffer for PTrieIterator");
    storage.iter = new (storage.buffer) PTrieIterator(index_, extractor_);
  }
  virtual bool Upsert(WriteBatchIndexEntry* key) override {
    auto slice_key = extractor_(key);

    if (OverwriteKey) {
      terark::Patricia::WriterToken token(index_);
      if (index_->insert(terark::fstring(slice_key.data(), slice_key.size()),
                         &key, &token)) {
        return true;
      }
      // insert fail , replace
      auto entry = *(WriteBatchIndexEntry**)token.value();
      std::swap(entry->offset, key->offset);
      return false;
    } else {
      class Token : public terark::Patricia::WriterToken {
      public:
        Token(terark::Patricia* trie, WriteBatchIndexEntry* value)
          : terark::Patricia::WriterToken(trie),
            value_(value) {}
        terark::MainPatricia* trie() {
          return static_cast<terark::MainPatricia*>(main());
        }

      protected:
        bool init_value(void* valptr, size_t valsize) override {
          assert(valsize == sizeof(uint32_t));

          size_t data_loc = trie()->mem_alloc(sizeof(value_wrap_t));
          assert(data_loc != terark::MainPatricia::mem_alloc_fail);
          auto* data = (value_wrap_t*)trie()->mem_get(data_loc);
          data->value = value_;

          size_t vector_loc = trie()->mem_alloc(sizeof(value_vector_t));
          assert(vector_loc != terark::MainPatricia::mem_alloc_fail);
          auto* vector = (value_vector_t*)trie()->mem_get(vector_loc);
          vector->loc = (uint32_t)data_loc;
          vector->size = 1;

          uint32_t u32_vector_loc = vector_loc;
          memcpy(valptr, &u32_vector_loc, valsize);
          return true;
        }
      private:
        WriteBatchIndexEntry * value_;
      };
      Token token(index_, key);
      WriteBatchIndexEntry* value_store;
      if (!index_->insert(terark::fstring(slice_key.data(), slice_key.size()),
                          &value_store, &token)) {
        // insert fail , append to vector
        size_t vector_loc = *(uint32_t*)token.value();
        auto* vector = (value_vector_t*)index_->main()->mem_get(vector_loc);
        size_t data_loc = vector->loc;
        auto* data = (value_wrap_t*)index_->main()->mem_get(data_loc);
        size_t size = vector->size;
        assert(size > 0);
        assert(key->offset > data[size - 1].value->offset);
        if (!vector->full()) {
          data[size].value = key;
          vector->size = size + 1;
        } else {
          size_t cow_data_loc =
              index_->main()->mem_alloc(sizeof(value_wrap_t) * size * 2);
          assert(cow_data_loc != terark::MainPatricia::mem_alloc_fail);
          auto* cow_data = (value_wrap_t*)index_->main()->mem_get(cow_data_loc);
          memcpy(cow_data, data, sizeof(value_wrap_t) * size);
          cow_data[size].value = key;
          vector->loc = (uint32_t)cow_data_loc;
          vector->size = size + 1;
          index_->mem_lazy_free(data_loc, sizeof(value_wrap_t) * size);
        }
      }
      return true;
    }
  }
};

const WriteBatchEntryIndexFactory* WriteBatchEntryPTrieIndexFactory(const WriteBatchEntryIndexFactory* fallback) {
  class WriteBatchEntryPTrieIndexContext : public WriteBatchEntryIndexContext {
   public:
    WriteBatchEntryIndexContext* fallback_context;
    terark::MainPatricia patricia;

    WriteBatchEntryPTrieIndexContext()
      : fallback_context(nullptr),
        patricia(terark::valvec_reserve(), 0, terark::Patricia::OneWriteMultiRead) {
    }

    ~WriteBatchEntryPTrieIndexContext() {
      if (fallback_context != nullptr) {
        fallback_context->~WriteBatchEntryIndexContext();
      }
    }
  };
  class PTrieIndexFactory : public WriteBatchEntryIndexFactory {
   public:
    virtual WriteBatchEntryIndexContext* NewContext(Arena* a) const {
      typedef WriteBatchEntryPTrieIndexContext ctx_t;
      auto ctx = new (a->AllocateAligned(sizeof(ctx_t))) ctx_t();
      ctx->fallback_context = fallback->NewContext(a);
      return ctx;
    }
    WriteBatchEntryIndex* New(WriteBatchEntryIndexContext* ctx,
                              WriteBatchKeyExtractor e,
                              const Comparator* c, Arena* a,
                              bool overwrite_key) const override {
      auto ptrie_ctx = static_cast<WriteBatchEntryPTrieIndexContext*>(ctx);
      auto get_trie = [&](size_t valsize) {
        void* buffer = a->AllocateAligned(sizeof(terark::SubPatricia));
        size_t new_root = ptrie_ctx->patricia.new_root(valsize);
        assert(new_root != terark::MainPatricia::mem_alloc_fail);
        return new (buffer) terark::SubPatricia(&ptrie_ctx->patricia, new_root, valsize);
      };
      if (strcmp(c->Name(), BytewiseComparator()->Name()) != 0) {
        return fallback->New(ptrie_ctx->fallback_context, e, c, a, overwrite_key);
      } else if (overwrite_key) {
        typedef WriteBatchEntryPTrieIndex<true> index_t;
        return new (a->AllocateAligned(sizeof(index_t))) index_t(get_trie(index_t::trie_value_size),
                                                                 e, c, a);
      } else {
        typedef WriteBatchEntryPTrieIndex<false> index_t;
        return new (a->AllocateAligned(sizeof(index_t))) index_t(get_trie(index_t::trie_value_size),
                                                                 e, c, a);
      }
    }
    PTrieIndexFactory(const WriteBatchEntryIndexFactory* _fallback)
      : fallback(_fallback) {
    }
   private:
    const WriteBatchEntryIndexFactory* fallback;
  };
  if (fallback == nullptr) {
    fallback = WriteBatchEntryRBTreeIndexFactory();
  }
  static PTrieIndexFactory factory(fallback);
  return &factory;
}

} // namespace rocksdb
