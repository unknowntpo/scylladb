/*
 * Copyright (C) 2017-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <optional>

#include "sstables/sstables.hh"
#include "sstables/shared_sstable.hh"
#include "sstables/index_reader.hh"
#include "sstables/binary_search.hh"
#include "sstables/writer.hh"
#include "compaction/compaction_manager.hh"
#include "replica/memtable-sstable.hh"
#include "dht/i_partitioner.hh"
#include "test/lib/test_services.hh"
#include "test/lib/sstable_test_env.hh"
#include "test/lib/reader_concurrency_semaphore.hh"
#include "gc_clock.hh"
#include <seastar/core/coroutine.hh>

using namespace sstables;
using namespace std::chrono_literals;

// Must be called in a seastar thread.
sstables::shared_sstable make_sstable_containing(std::function<sstables::shared_sstable()> sst_factory, lw_shared_ptr<replica::memtable> mt);
sstables::shared_sstable make_sstable_containing(sstables::shared_sstable sst, lw_shared_ptr<replica::memtable> mt);
sstables::shared_sstable make_sstable_containing(std::function<sstables::shared_sstable()> sst_factory, std::vector<mutation> muts);
sstables::shared_sstable make_sstable_containing(sstables::shared_sstable sst, std::vector<mutation> muts);

inline future<> write_memtable_to_sstable_for_test(replica::memtable& mt, sstables::shared_sstable sst) {
    return write_memtable_to_sstable(mt, sst, sst->manager().configure_writer("memtable"));
}

shared_sstable make_sstable(sstables::test_env& env, schema_ptr s, sstring dir, std::vector<mutation> mutations,
        sstable_writer_config cfg, sstables::sstable::version_types version, gc_clock::time_point query_time = gc_clock::now());

inline shared_sstable make_sstable(sstables::test_env& env, schema_ptr s, std::vector<mutation> mutations,
        sstable_writer_config cfg, sstables::sstable::version_types version, gc_clock::time_point query_time = gc_clock::now()) {
    return make_sstable(env, std::move(s), env.tempdir().path().native(), std::move(mutations), std::move(cfg), version, query_time);
}

namespace sstables {

using sstable_ptr = shared_sstable;

class test {
    sstable_ptr _sst;
public:

    test(sstable_ptr s) : _sst(s) {}

    summary& _summary() {
        return _sst->_components->summary;
    }

    future<temporary_buffer<char>> data_read(reader_permit permit, uint64_t pos, size_t len) {
        return _sst->data_read(pos, len, default_priority_class(), std::move(permit));
    }

    std::unique_ptr<index_reader> make_index_reader(reader_permit permit) {
        return std::make_unique<index_reader>(_sst, std::move(permit), default_priority_class(),
                                              tracing::trace_state_ptr(), use_caching::yes);
    }

    struct index_entry {
        sstables::key sstables_key;
        partition_key key;
        uint64_t promoted_index_size;

        key_view get_key() const {
            return sstables_key;
        }
    };

    future<std::vector<index_entry>> read_indexes(reader_permit permit) {
        std::vector<index_entry> entries;
        auto s = _sst->get_schema();
        auto ir = make_index_reader(std::move(permit));
        std::exception_ptr err = nullptr;
        try {
            while (!ir->eof()) {
                co_await ir->read_partition_data();
                auto pk = ir->get_partition_key();
                entries.emplace_back(index_entry{sstables::key::from_partition_key(*s, pk),
                                        pk, ir->get_promoted_index_size()});
                co_await ir->advance_to_next_partition();
            }
        } catch (...) {
            err = std::current_exception();
        }
        co_await ir->close();
        if (err) {
            co_return coroutine::exception(std::move(err));
        }
        co_return entries;
    }

    future<> read_statistics() {
        return _sst->read_statistics(default_priority_class());
    }

    statistics& get_statistics() {
        return _sst->_components->statistics;
    }

    future<> read_summary() noexcept {
        return _sst->read_summary(default_priority_class());
    }

    future<summary_entry&> read_summary_entry(size_t i) {
        return _sst->read_summary_entry(i);
    }

    summary& get_summary() {
        return _sst->_components->summary;
    }

    summary move_summary() {
        return std::move(_sst->_components->summary);
    }

    future<> read_toc() noexcept {
        return _sst->read_toc();
    }

    auto& get_components() {
        return _sst->_recognized_components;
    }

    template <typename T>
    int binary_search(const dht::i_partitioner& p, const T& entries, const key& sk) {
        return sstables::binary_search(p, entries, sk);
    }

    void change_generation_number(sstables::generation_type generation) {
        _sst->_generation = generation;
    }

    void change_dir(sstring dir) {
        _sst->_storage->change_dir_for_test(dir);
    }

    void set_data_file_size(uint64_t size) {
        _sst->_data_file_size = size;
    }

    void set_data_file_write_time(db_clock::time_point wtime) {
        _sst->_data_file_write_time = wtime;
    }

    void set_run_identifier(sstables::run_id identifier) {
        _sst->_run_identifier = identifier;
    }

    future<> store() {
        _sst->_recognized_components.erase(component_type::Index);
        _sst->_recognized_components.erase(component_type::Data);
        return seastar::async([sst = _sst] {
            sst->open_sstable(default_priority_class());
            sst->write_statistics(default_priority_class());
            sst->write_compression(default_priority_class());
            sst->write_filter(default_priority_class());
            sst->write_summary(default_priority_class());
            sst->seal_sstable(false).get();
        });
    }

    // Used to create synthetic sstables for testing leveled compaction strategy.
    void set_values_for_leveled_strategy(uint64_t fake_data_size, uint32_t sstable_level, int64_t max_timestamp, const partition_key& first_key, const partition_key& last_key) {
        _sst->_data_file_size = fake_data_size;
        _sst->_bytes_on_disk = fake_data_size;
        // Create a synthetic stats metadata
        stats_metadata stats = {};
        // leveled strategy sorts sstables by age using max_timestamp, let's set it to 0.
        stats.max_timestamp = max_timestamp;
        stats.sstable_level = sstable_level;
        _sst->_components->statistics.contents[metadata_type::Stats] = std::make_unique<stats_metadata>(std::move(stats));
        _sst->_components->summary.first_key.value = sstables::key::from_partition_key(*_sst->_schema, first_key).get_bytes();
        _sst->_components->summary.last_key.value = sstables::key::from_partition_key(*_sst->_schema, last_key).get_bytes();
        _sst->set_first_and_last_keys();
        _sst->_run_identifier = run_id::create_random_id();
        _sst->_shards.push_back(this_shard_id());
    }

    void set_values(const partition_key& first_key, const partition_key& last_key, stats_metadata stats, uint64_t data_file_size = 1) {
        _sst->_data_file_size = data_file_size;
        _sst->_bytes_on_disk = data_file_size;
        // scylla component must be present for a sstable to be considered fully expired.
        _sst->_recognized_components.insert(component_type::Scylla);
        _sst->_components->statistics.contents[metadata_type::Stats] = std::make_unique<stats_metadata>(std::move(stats));
        _sst->_components->summary.first_key.value = sstables::key::from_partition_key(*_sst->_schema, first_key).get_bytes();
        _sst->_components->summary.last_key.value = sstables::key::from_partition_key(*_sst->_schema, last_key).get_bytes();
        _sst->set_first_and_last_keys();
        _sst->_components->statistics.contents[metadata_type::Compaction] = std::make_unique<compaction_metadata>();
        _sst->_run_identifier = run_id::create_random_id();
        _sst->_shards.push_back(this_shard_id());
    }

    void rewrite_toc_without_scylla_component() {
        _sst->_recognized_components.erase(component_type::Scylla);
        remove_file(_sst->filename(component_type::TOC)).get();
        _sst->_storage->open(*_sst, default_priority_class());
        _sst->seal_sstable(false).get();
    }

    future<> remove_component(component_type c) {
        return remove_file(_sst->filename(c));
    }

    const sstring filename(component_type c) const {
        return _sst->filename(c);
    }

    void set_shards(std::vector<unsigned> shards) {
        _sst->_shards = std::move(shards);
    }

    static future<> create_links(const sstable& sst, const sstring& dir) {
        return sst._storage->create_links(sst, dir);
    }

    future<> move_to_new_dir(sstring new_dir, generation_type new_generation) {
        co_await _sst->_storage->move(*_sst, std::move(new_dir), new_generation, nullptr);
        _sst->_generation = std::move(new_generation);
    }

    static fs::path filename(const sstable& sst, component_type c) {
        return fs::path(sst.filename(c));
    }

    sstring storage_prefix() const {
        return _sst->_storage->prefix();
    }
};

inline auto replacer_fn_no_op() {
    return [](sstables::compaction_completion_desc desc) -> void {};
}

template<typename AsyncAction>
requires requires (AsyncAction aa, sstables::sstable::version_types& c) { { aa(c) } -> std::same_as<future<>>; }
inline
future<> for_each_sstable_version(AsyncAction action) {
    return seastar::do_for_each(all_sstable_versions, std::move(action));
}

} // namespace sstables

// Must be used in a seastar thread
class compaction_manager_for_testing {
    struct wrapped_compaction_manager {
        tasks::task_manager tm;
        compaction_manager cm;
        explicit wrapped_compaction_manager(bool enabled);
        // Must run in a seastar thread
        ~wrapped_compaction_manager();
    };

    lw_shared_ptr<wrapped_compaction_manager> _wcm;
public:
    explicit compaction_manager_for_testing(bool enabled = true) : _wcm(make_lw_shared<wrapped_compaction_manager>(enabled)) {}

    compaction_manager& operator*() noexcept {
        return _wcm->cm;
    }
    const compaction_manager& operator*() const noexcept {
        return _wcm->cm;
    }

    compaction_manager* operator->() noexcept {
        return &_wcm->cm;
    }
    const compaction_manager* operator->() const noexcept {
        return &_wcm->cm;
    }
};

class compaction_manager_test {
    compaction_manager& _cm;
public:
    explicit compaction_manager_test(compaction_manager& cm) noexcept : _cm(cm) {}

    future<> run(sstables::run_id output_run_id, table_state& table_s, noncopyable_function<future<> (sstables::compaction_data&)> job);

    void propagate_replacement(table_state& table_s, const std::vector<sstables::shared_sstable>& removed, const std::vector<sstables::shared_sstable>& added) {
        _cm.propagate_replacement(table_s, removed, added);
    }
private:
    sstables::compaction_data& register_compaction(shared_ptr<compaction::compaction_task_executor> task);

    void deregister_compaction(const sstables::compaction_data& c);
};

using can_purge_tombstones = compaction_manager::can_purge_tombstones;
future<compaction_result> compact_sstables(compaction_manager& cm, sstables::compaction_descriptor descriptor, table_state& table_s,
        std::function<shared_sstable()> creator, sstables::compaction_sstable_replacer_fn replacer = sstables::replacer_fn_no_op(),
        can_purge_tombstones can_purge = can_purge_tombstones::yes);

shared_sstable make_sstable_easy(test_env& env, flat_mutation_reader_v2 rd, sstable_writer_config cfg,
        sstables::generation_type gen, const sstables::sstable::version_types version = sstables::get_highest_sstable_version(), int expected_partition = 1);
shared_sstable make_sstable_easy(test_env& env, lw_shared_ptr<replica::memtable> mt, sstable_writer_config cfg,
        sstables::generation_type gen, const sstable::version_types v = sstables::get_highest_sstable_version(), int estimated_partitions = 1, gc_clock::time_point = gc_clock::now());


inline shared_sstable make_sstable_easy(test_env& env, flat_mutation_reader_v2 rd, sstable_writer_config cfg,
        const sstables::sstable::version_types version = sstables::get_highest_sstable_version(), int expected_partition = 1) {
    return make_sstable_easy(env, std::move(rd), std::move(cfg), env.new_generation(), version, expected_partition);
}
inline shared_sstable make_sstable_easy(test_env& env, lw_shared_ptr<replica::memtable> mt, sstable_writer_config cfg,
        const sstable::version_types version = sstables::get_highest_sstable_version(), int estimated_partitions = 1, gc_clock::time_point query_time = gc_clock::now()) {
    return make_sstable_easy(env, std::move(mt), std::move(cfg), env.new_generation(), version, estimated_partitions, query_time);
}
