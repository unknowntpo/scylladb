/*
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "token_metadata.hh"
#include <optional>
#include "locator/snitch_base.hh"
#include "locator/abstract_replication_strategy.hh"
#include "log.hh"
#include "partition_range_compat.hh"
#include <unordered_map>
#include <algorithm>
#include <boost/icl/interval.hpp>
#include <boost/icl/interval_map.hpp>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <boost/range/adaptors.hpp>
#include "seastar/core/smp.hh"
#include "utils/stall_free.hh"
#include "utils/fb_utilities.hh"

namespace locator {

static logging::logger tlogger("token_metadata");

template <typename C, typename V>
static void remove_by_value(C& container, V value) {
    for (auto it = container.begin(); it != container.end();) {
        if (it->second == value) {
            it = container.erase(it);
        } else {
            it++;
        }
    }
}

class token_metadata_impl final {
public:
    using inet_address = gms::inet_address;
private:
    /**
     * Maintains token to endpoint map of every node in the cluster.
     * Each Token is associated with exactly one Address, but each Address may have
     * multiple tokens.  Hence, the BiMultiValMap collection.
     */
    // FIXME: have to be BiMultiValMap
    std::unordered_map<token, inet_address> _token_to_endpoint_map;

    // Track the unique set of nodes in _token_to_endpoint_map
    std::unordered_set<inet_address> _normal_token_owners;

    std::unordered_map<token, inet_address> _bootstrap_tokens;
    std::unordered_set<inet_address> _leaving_endpoints;
    // The map between the existing node to be replaced and the replacing node
    std::unordered_map<inet_address, inet_address> _replacing_endpoints;

    std::unordered_map<sstring, boost::icl::interval_map<token, std::unordered_set<inet_address>>> _pending_ranges_interval_map;

    std::vector<token> _sorted_tokens;

    topology _topology;

    long _ring_version = 0;
    static thread_local long _static_ring_version;

    // Note: if any member is added to this class
    // clone_async() must be updated to copy that member.

    void sort_tokens();

    struct shallow_copy {};
    token_metadata_impl(shallow_copy, const token_metadata_impl& o) noexcept
            : _topology(topology::config{})
    {}

public:
    token_metadata_impl(token_metadata::config cfg) noexcept : _topology(std::move(cfg.topo_cfg)) {};
    token_metadata_impl(const token_metadata_impl&) = delete; // it's too huge for direct copy, use clone_async()
    token_metadata_impl(token_metadata_impl&&) noexcept = default;
    const std::vector<token>& sorted_tokens() const;
    future<> update_normal_tokens(std::unordered_set<token> tokens, inet_address endpoint);
    const token& first_token(const token& start) const;
    size_t first_token_index(const token& start) const;
    std::optional<inet_address> get_endpoint(const token& token) const;
    std::vector<token> get_tokens(const inet_address& addr) const;
    const std::unordered_map<token, inet_address>& get_token_to_endpoint() const {
        return _token_to_endpoint_map;
    }

    const std::unordered_set<inet_address>& get_leaving_endpoints() const {
        return _leaving_endpoints;
    }

    const std::unordered_map<token, inet_address>& get_bootstrap_tokens() const {
        return _bootstrap_tokens;
    }

    void update_topology(inet_address ep, endpoint_dc_rack dr, std::optional<node::state> opt_st) {
        _topology.add_or_update_endpoint(ep, std::move(dr), std::move(opt_st));
    }

    /**
     * Creates an iterable range of the sorted tokens starting at the token next
     * after the given one.
     *
     * @param start A token that will define the beginning of the range
     *
     * @return The requested range (see the description above)
     */
    boost::iterator_range<token_metadata::tokens_iterator> ring_range(const token& start) const;

    boost::iterator_range<token_metadata::tokens_iterator> ring_range(
        const std::optional<dht::partition_range::bound>& start) const;

    topology& get_topology() {
        return _topology;
    }

    const topology& get_topology() const {
        return _topology;
    }

    void debug_show() const;

    /**
     * Store an end-point to host ID mapping.  Each ID must be unique, and
     * cannot be changed after the fact.
     *
     * @param hostId
     * @param endpoint
     */
    void update_host_id(const host_id& host_id, inet_address endpoint);

    /** Return the unique host ID for an end-point. */
    host_id get_host_id(inet_address endpoint) const;

    /// Return the unique host ID for an end-point or nullopt if not found.
    std::optional<host_id> get_host_id_if_known(inet_address endpoint) const;

    /** Return the end-point for a unique host ID */
    std::optional<inet_address> get_endpoint_for_host_id(host_id) const;

    /** @return a copy of the endpoint-to-id map for read-only operations */
    std::unordered_map<inet_address, host_id> get_endpoint_to_host_id_map_for_reading() const;

    void add_bootstrap_token(token t, inet_address endpoint);

    void add_bootstrap_tokens(std::unordered_set<token> tokens, inet_address endpoint);

    void remove_bootstrap_tokens(std::unordered_set<token> tokens);

    void add_leaving_endpoint(inet_address endpoint);
    void del_leaving_endpoint(inet_address endpoint);
public:
    void remove_endpoint(inet_address endpoint);

    bool is_normal_token_owner(inet_address endpoint) const;

    bool is_leaving(inet_address endpoint) const;

    // Is this node being replaced by another node
    bool is_being_replaced(inet_address endpoint) const;

    // Is any node being replaced by another node
    bool is_any_node_being_replaced() const;

    void add_replacing_endpoint(inet_address existing_node, inet_address replacing_node);

    void del_replacing_endpoint(inet_address existing_node);
public:

    /**
     * Create a full copy of token_metadata_impl using asynchronous continuations.
     * The caller must ensure that the cloned object will not change if
     * the function yields.
     */
    future<token_metadata_impl> clone_async() const noexcept;

    /**
     * Create a copy of TokenMetadata with only tokenToEndpointMap. That is, pending ranges,
     * bootstrap tokens and leaving endpoints are not included in the copy.
     * The caller must ensure that the cloned object will not change if
     * the function yields.
     */
    future<token_metadata_impl> clone_only_token_map(bool clone_sorted_tokens = true) const noexcept;

    /**
     * Create a copy of TokenMetadata with tokenToEndpointMap reflecting situation after all
     * current leave operations have finished.
     *
     * @return new token metadata
     */
    future<token_metadata_impl> clone_after_all_left() const noexcept {
      return clone_only_token_map(false).then([this] (token_metadata_impl all_left_metadata) {
        for (auto endpoint : _leaving_endpoints) {
            all_left_metadata.remove_endpoint(endpoint);
        }
        all_left_metadata.sort_tokens();

        return all_left_metadata;
      });
    }

    /**
     * Destroy the token_metadata members using continuations
     * to prevent reactor stalls.
     */
    future<> clear_gently() noexcept;

public:
    dht::token_range_vector get_primary_ranges_for(std::unordered_set<token> tokens) const;

    dht::token_range_vector get_primary_ranges_for(token right) const;
    static boost::icl::interval<token>::interval_type range_to_interval(range<dht::token> r);
    static range<dht::token> interval_to_range(boost::icl::interval<token>::interval_type i);

private:
    void set_pending_ranges(const sstring& keyspace_name, std::unordered_multimap<range<token>, inet_address> new_pending_ranges, can_yield);

public:
    bool has_pending_ranges(sstring keyspace_name, inet_address endpoint) const;

     /**
     * Calculate pending ranges according to bootsrapping and leaving nodes. Reasoning is:
     *
     * (1) When in doubt, it is better to write too much to a node than too little. That is, if
     * there are multiple nodes moving, calculate the biggest ranges a node could have. Cleaning
     * up unneeded data afterwards is better than missing writes during movement.
     * (2) When a node leaves, ranges for other nodes can only grow (a node might get additional
     * ranges, but it will not lose any of its current ranges as a result of a leave). Therefore
     * we will first remove _all_ leaving tokens for the sake of calculation and then check what
     * ranges would go where if all nodes are to leave. This way we get the biggest possible
     * ranges with regard current leave operations, covering all subsets of possible final range
     * values.
     * (3) When a node bootstraps, ranges of other nodes can only get smaller. Without doing
     * complex calculations to see if multiple bootstraps overlap, we simply base calculations
     * on the same token ring used before (reflecting situation after all leave operations have
     * completed). Bootstrapping nodes will be added and removed one by one to that metadata and
     * checked what their ranges would be. This will give us the biggest possible ranges the
     * node could have. It might be that other bootstraps make our actual final ranges smaller,
     * but it does not matter as we can clean up the data afterwards.
     *
     * NOTE: This is heavy and ineffective operation. This will be done only once when a node
     * changes state in the cluster, so it should be manageable.
     */
    future<> update_pending_ranges(
            const token_metadata& unpimplified_this,
            const abstract_replication_strategy& strategy, const sstring& keyspace_name, dc_rack_fn& get_dc_rack);
    void calculate_pending_ranges_for_leaving(
        const token_metadata& unpimplified_this,
        const abstract_replication_strategy& strategy,
        std::unordered_multimap<range<token>, inet_address>& new_pending_ranges,
        mutable_token_metadata_ptr all_left_metadata) const;
    void calculate_pending_ranges_for_bootstrap(
        const abstract_replication_strategy& strategy,
        std::unordered_multimap<range<token>, inet_address>& new_pending_ranges,
        mutable_token_metadata_ptr all_left_metadata, dc_rack_fn& get_dc_rack) const;
    void calculate_pending_ranges_for_replacing(
        const token_metadata& unpimplified_this,
        const abstract_replication_strategy& strategy,
        std::unordered_multimap<range<token>, inet_address>& new_pending_ranges) const;
public:

    token get_predecessor(token t) const;

    // Returns nodes that are officially part of the ring. It does not include
    // node that is still joining the cluster, e.g., a node that is still
    // streaming data before it finishes the bootstrap process and turns into
    // NORMAL status.
    const std::unordered_set<inet_address>& get_all_endpoints() const noexcept {
        return _normal_token_owners;
    }

    /* Returns the number of different endpoints that own tokens in the ring.
     * Bootstrapping tokens are not taken into account. */
    size_t count_normal_token_owners() const;
private:
    future<> update_normal_token_owners();

public:
    // returns empty vector if keyspace_name not found.
    inet_address_vector_topology_change pending_endpoints_for(const token& token, const sstring& keyspace_name) const;

public:
    /** @return an endpoint to token multimap representation of tokenToEndpointMap (a copy) */
    std::multimap<inet_address, token> get_endpoint_to_token_map_for_reading() const;
    /**
     * @return a (stable copy, won't be modified) Token to Endpoint map for all the normal and bootstrapping nodes
     *         in the cluster.
     */
    std::map<token, inet_address> get_normal_and_bootstrapping_token_to_endpoint_map() const;

    long get_ring_version() const {
        return _ring_version;
    }

    void invalidate_cached_rings() {
        _ring_version = ++_static_ring_version;
        tlogger.debug("ring_version={}", _ring_version);
    }

    friend class token_metadata;
};

thread_local long token_metadata_impl::_static_ring_version;

token_metadata::tokens_iterator::tokens_iterator(const token& start, const token_metadata_impl* token_metadata)
    : _token_metadata(token_metadata) {
    _cur_it = _token_metadata->sorted_tokens().begin() + _token_metadata->first_token_index(start);
    _remaining = _token_metadata->sorted_tokens().size();
}

bool token_metadata::tokens_iterator::operator==(const tokens_iterator& it) const {
    return _remaining == it._remaining;
}

const token& token_metadata::tokens_iterator::operator*() const {
    return *_cur_it;
}

token_metadata::tokens_iterator& token_metadata::tokens_iterator::operator++() {
    ++_cur_it;
    if (_cur_it == _token_metadata->sorted_tokens().end()) {
        _cur_it = _token_metadata->sorted_tokens().begin();
    }
    --_remaining;
    return *this;
}

inline
boost::iterator_range<token_metadata::tokens_iterator>
token_metadata_impl::ring_range(const token& start) const {
    auto begin = token_metadata::tokens_iterator(start, this);
    auto end = token_metadata::tokens_iterator();
    return boost::make_iterator_range(begin, end);
}

future<token_metadata_impl> token_metadata_impl::clone_async() const noexcept {
    auto ret = co_await clone_only_token_map();
    ret._bootstrap_tokens.reserve(_bootstrap_tokens.size());
    for (const auto& p : _bootstrap_tokens) {
        ret._bootstrap_tokens.emplace(p);
        co_await coroutine::maybe_yield();
    }
    ret._leaving_endpoints = _leaving_endpoints;
    ret._replacing_endpoints = _replacing_endpoints;
    for (const auto& p : _pending_ranges_interval_map) {
        ret._pending_ranges_interval_map.emplace(p);
        co_await coroutine::maybe_yield();
    }
    ret._ring_version = _ring_version;
    co_return ret;
}

future<token_metadata_impl> token_metadata_impl::clone_only_token_map(bool clone_sorted_tokens) const noexcept {
    auto ret = token_metadata_impl(shallow_copy{}, *this);
    ret._token_to_endpoint_map.reserve(_token_to_endpoint_map.size());
    for (const auto& p : _token_to_endpoint_map) {
        ret._token_to_endpoint_map.emplace(p);
        co_await coroutine::maybe_yield();
    }
    ret._normal_token_owners = _normal_token_owners;
    ret._topology = co_await _topology.clone_gently();
    if (clone_sorted_tokens) {
        ret._sorted_tokens = _sorted_tokens;
        co_await coroutine::maybe_yield();
    }
    co_return ret;
}

future<> token_metadata_impl::clear_gently() noexcept {
    co_await utils::clear_gently(_token_to_endpoint_map);
    co_await utils::clear_gently(_normal_token_owners);
    co_await utils::clear_gently(_bootstrap_tokens);
    co_await utils::clear_gently(_leaving_endpoints);
    co_await utils::clear_gently(_replacing_endpoints);
    co_await utils::clear_gently(_pending_ranges_interval_map);
    co_await utils::clear_gently(_sorted_tokens);
    co_await _topology.clear_gently();
    co_return;
}

void token_metadata_impl::sort_tokens() {
    std::vector<token> sorted;
    sorted.reserve(_token_to_endpoint_map.size());

    for (auto&& i : _token_to_endpoint_map) {
        sorted.push_back(i.first);
    }

    std::sort(sorted.begin(), sorted.end());

    _sorted_tokens = std::move(sorted);
}

const std::vector<token>& token_metadata_impl::sorted_tokens() const {
    return _sorted_tokens;
}

std::vector<token> token_metadata_impl::get_tokens(const inet_address& addr) const {
    std::vector<token> res;
    for (auto&& i : _token_to_endpoint_map) {
        if (i.second == addr) {
            res.push_back(i.first);
        }
    }
    std::sort(res.begin(), res.end());
    return res;
}

future<> token_metadata_impl::update_normal_tokens(std::unordered_set<token> tokens, inet_address endpoint) {
    if (tokens.empty()) {
        co_return;
    }

    if (!_topology.has_endpoint(endpoint)) {
        on_internal_error(tlogger, format("token_metadata_impl: {} must be a member of topology to update normal tokens", endpoint));
    }

    bool should_sort_tokens = false;

    // Phase 1: erase all tokens previously owned by the endpoint.
    for(auto it = _token_to_endpoint_map.begin(), ite = _token_to_endpoint_map.end(); it != ite;) {
        co_await coroutine::maybe_yield();
        if(it->second == endpoint) {
            auto tokit = tokens.find(it->first);
            if (tokit == tokens.end()) {
                // token no longer owned by endpoint
                it = _token_to_endpoint_map.erase(it);
                continue;
            }
            // token ownership did not change,
            // no further update needed for it.
            tokens.erase(tokit);
        }
        ++it;
    }

    // Phase 2:
    // a. ...
    // b. update pending _bootstrap_tokens and _leaving_endpoints
    // c. update _token_to_endpoint_map with the new endpoint->token mappings
    //    - set `should_sort_tokens` if new tokens were added
    remove_by_value(_bootstrap_tokens, endpoint);
    _leaving_endpoints.erase(endpoint);
    invalidate_cached_rings();
    for (const token& t : tokens)
    {
        co_await coroutine::maybe_yield();
        auto prev = _token_to_endpoint_map.insert(std::pair<token, inet_address>(t, endpoint));
        should_sort_tokens |= prev.second; // new token inserted -> sort
        if (prev.first->second != endpoint) {
            tlogger.debug("Token {} changing ownership from {} to {}", t, prev.first->second, endpoint);
            prev.first->second = endpoint;
        }
    }

    co_await update_normal_token_owners();

    // New tokens were added to _token_to_endpoint_map
    // so re-sort all tokens.
    if (should_sort_tokens) {
        sort_tokens();
    }
    co_return;
}

size_t token_metadata_impl::first_token_index(const token& start) const {
    if (_sorted_tokens.empty()) {
        auto msg = format("sorted_tokens is empty in first_token_index!");
        tlogger.error("{}", msg);
        throw std::runtime_error(msg);
    }
    auto it = std::lower_bound(_sorted_tokens.begin(), _sorted_tokens.end(), start);
    if (it == _sorted_tokens.end()) {
        return 0;
    } else {
        return std::distance(_sorted_tokens.begin(), it);
    }
}

const token& token_metadata_impl::first_token(const token& start) const {
    return _sorted_tokens[first_token_index(start)];
}

std::optional<inet_address> token_metadata_impl::get_endpoint(const token& token) const {
    auto it = _token_to_endpoint_map.find(token);
    if (it == _token_to_endpoint_map.end()) {
        return std::nullopt;
    } else {
        return it->second;
    }
}

void token_metadata_impl::debug_show() const {
    auto reporter = std::make_shared<timer<lowres_clock>>();
    reporter->set_callback ([reporter, this] {
        fmt::print("Endpoint -> Token\n");
        for (auto x : _token_to_endpoint_map) {
            fmt::print("inet_address={}, token={}\n", x.second, x.first);
        }
        fmt::print("Sorted Token\n");
        for (auto x : _sorted_tokens) {
            fmt::print("token={}\n", x);
        }
    });
    reporter->arm_periodic(std::chrono::seconds(1));
}

void token_metadata_impl::update_host_id(const host_id& host_id, inet_address endpoint) {
    _topology.add_or_update_endpoint(endpoint, host_id);
}

host_id token_metadata_impl::get_host_id(inet_address endpoint) const {
    if (const auto* node = _topology.find_node(endpoint)) [[likely]] {
        return node->host_id();
    } else {
        throw std::runtime_error(format("host_id for endpoint {} is not found", endpoint));
    }
}

std::optional<host_id> token_metadata_impl::get_host_id_if_known(inet_address endpoint) const {
    if (const auto* node = _topology.find_node(endpoint)) [[likely]] {
        return node->host_id();
    } else {
        return std::nullopt;
    }
}

std::optional<inet_address> token_metadata_impl::get_endpoint_for_host_id(host_id host_id) const {
    if (const auto* node = _topology.find_node(host_id)) [[likely]] {
        return node->endpoint();
    } else {
        return std::nullopt;
    }
}

std::unordered_map<inet_address, host_id> token_metadata_impl::get_endpoint_to_host_id_map_for_reading() const {
    const auto& nodes = _topology.get_nodes_by_endpoint();
    std::unordered_map<inet_address, host_id> map;
    map.reserve(nodes.size());
    for (const auto& [endpoint, node] : nodes) {
        map[endpoint] = node->host_id();
    }
    return map;
}

bool token_metadata_impl::is_normal_token_owner(inet_address endpoint) const {
    return _normal_token_owners.contains(endpoint);
}

void token_metadata_impl::add_bootstrap_token(token t, inet_address endpoint) {
    std::unordered_set<token> tokens{t};
    add_bootstrap_tokens(tokens, endpoint);
}

boost::iterator_range<token_metadata::tokens_iterator>
token_metadata_impl::ring_range(const std::optional<dht::partition_range::bound>& start) const {
    auto r = ring_range(start ? start->value().token() : dht::minimum_token());

    if (!r.empty()) {
        // We should skip the first token if it's excluded by the range.
        if (start
            && !start->is_inclusive()
            && !start->value().has_key()
            && start->value().token() == *r.begin())
        {
            r.pop_front();
        }
    }

    return r;
}

void token_metadata_impl::add_bootstrap_tokens(std::unordered_set<token> tokens, inet_address endpoint) {
    for (auto t : tokens) {
        auto old_endpoint = _bootstrap_tokens.find(t);
        if (old_endpoint != _bootstrap_tokens.end() && (*old_endpoint).second != endpoint) {
            auto msg = format("Bootstrap Token collision between {} and {} (token {}", (*old_endpoint).second, endpoint, t);
            throw std::runtime_error(msg);
        }

        auto old_endpoint2 = _token_to_endpoint_map.find(t);
        if (old_endpoint2 != _token_to_endpoint_map.end() && (*old_endpoint2).second != endpoint) {
            auto msg = format("Bootstrap Token collision between {} and {} (token {}", (*old_endpoint2).second, endpoint, t);
            throw std::runtime_error(msg);
        }
    }

    std::erase_if(_bootstrap_tokens, [endpoint] (const std::pair<token, inet_address>& n) { return n.second == endpoint; });

    for (auto t : tokens) {
        _bootstrap_tokens[t] = endpoint;
    }
}

void token_metadata_impl::remove_bootstrap_tokens(std::unordered_set<token> tokens) {
    if (tokens.empty()) {
        tlogger.warn("tokens is empty in remove_bootstrap_tokens!");
        return;
    }
    for (auto t : tokens) {
        _bootstrap_tokens.erase(t);
    }
}

bool token_metadata_impl::is_leaving(inet_address endpoint) const {
    return _leaving_endpoints.contains(endpoint);
}

bool token_metadata_impl::is_being_replaced(inet_address endpoint) const {
    return _replacing_endpoints.contains(endpoint);
}

bool token_metadata_impl::is_any_node_being_replaced() const {
    return !_replacing_endpoints.empty();
}

void token_metadata_impl::remove_endpoint(inet_address endpoint) {
    remove_by_value(_bootstrap_tokens, endpoint);
    remove_by_value(_token_to_endpoint_map, endpoint);
    _normal_token_owners.erase(endpoint);
    _topology.remove_endpoint(endpoint);
    _leaving_endpoints.erase(endpoint);
    del_replacing_endpoint(endpoint);
    invalidate_cached_rings();
}

token token_metadata_impl::get_predecessor(token t) const {
    auto& tokens = sorted_tokens();
    auto it = std::lower_bound(tokens.begin(), tokens.end(), t);
    if (it == tokens.end() || *it != t) {
        auto msg = format("token error in get_predecessor!");
        tlogger.error("{}", msg);
        throw std::runtime_error(msg);
    }
    if (it == tokens.begin()) {
        // If the token is the first element, its preprocessor is the last element
        return tokens.back();
    } else {
        return *(--it);
    }
}

dht::token_range_vector token_metadata_impl::get_primary_ranges_for(std::unordered_set<token> tokens) const {
    dht::token_range_vector ranges;
    ranges.reserve(tokens.size() + 1); // one of the ranges will wrap
    for (auto right : tokens) {
        auto left = get_predecessor(right);
        ::compat::unwrap_into(
                wrapping_range<token>(range_bound<token>(left, false), range_bound<token>(right)),
                dht::token_comparator(),
                [&] (auto&& rng) { ranges.push_back(std::move(rng)); });
    }
    return ranges;
}

dht::token_range_vector token_metadata_impl::get_primary_ranges_for(token right) const {
    return get_primary_ranges_for(std::unordered_set<token>{right});
}

boost::icl::interval<token>::interval_type
token_metadata_impl::range_to_interval(range<dht::token> r) {
    bool start_inclusive = false;
    bool end_inclusive = false;
    token start = dht::minimum_token();
    token end = dht::maximum_token();

    if (r.start()) {
        start = r.start()->value();
        start_inclusive = r.start()->is_inclusive();
    }

    if (r.end()) {
        end = r.end()->value();
        end_inclusive = r.end()->is_inclusive();
    }

    if (start_inclusive == false && end_inclusive == false) {
        return boost::icl::interval<token>::open(std::move(start), std::move(end));
    } else if (start_inclusive == false && end_inclusive == true) {
        return boost::icl::interval<token>::left_open(std::move(start), std::move(end));
    } else if (start_inclusive == true && end_inclusive == false) {
        return boost::icl::interval<token>::right_open(std::move(start), std::move(end));
    } else {
        return boost::icl::interval<token>::closed(std::move(start), std::move(end));
    }
}

range<dht::token>
token_metadata_impl::interval_to_range(boost::icl::interval<token>::interval_type i) {
    bool start_inclusive;
    bool end_inclusive;
    auto bounds = i.bounds().bits();
    if (bounds == boost::icl::interval_bounds::static_open) {
        start_inclusive = false;
        end_inclusive = false;
    } else if (bounds == boost::icl::interval_bounds::static_left_open) {
        start_inclusive = false;
        end_inclusive = true;
    } else if (bounds == boost::icl::interval_bounds::static_right_open) {
        start_inclusive = true;
        end_inclusive = false;
    } else if (bounds == boost::icl::interval_bounds::static_closed) {
        start_inclusive = true;
        end_inclusive = true;
    } else {
        throw std::runtime_error("Invalid boost::icl::interval<token> bounds");
    }
    return range<dht::token>({{i.lower(), start_inclusive}}, {{i.upper(), end_inclusive}});
}

void token_metadata_impl::set_pending_ranges(const sstring& keyspace_name,
        std::unordered_multimap<range<token>, inet_address> new_pending_ranges,
        can_yield can_yield) {
    if (new_pending_ranges.empty()) {
        _pending_ranges_interval_map.erase(keyspace_name);
        return;
    }
    std::unordered_map<range<token>, std::unordered_set<inet_address>> map;
    std::unordered_set<inet_address> endpoints;
    for (const auto& x : new_pending_ranges) {
        if (can_yield) {
            seastar::thread::maybe_yield();
        }
        map[x.first].emplace(x.second);
        auto ins = endpoints.emplace(x.second);
        if (ins.second) { // insertion took place, i.e. -- new endpoint
            if (!_topology.has_endpoint(x.second)) {
                on_internal_error(tlogger, format("token_metadata_impl: {} must be member or pending to set pending tokens", x.second));
            }
        }
    }

    // construct a interval map to speed up the search
    boost::icl::interval_map<token, std::unordered_set<inet_address>> interval_map;
    for (auto& m : map) {
        if (can_yield) {
            seastar::thread::maybe_yield();
        }
        interval_map +=
                std::make_pair(range_to_interval(m.first), std::move(m.second));
    }
    _pending_ranges_interval_map[keyspace_name] = std::move(interval_map);
}


bool
token_metadata_impl::has_pending_ranges(sstring keyspace_name, inet_address endpoint) const {
    const auto it = _pending_ranges_interval_map.find(keyspace_name);
    if (it == _pending_ranges_interval_map.end()) {
        return false;
    }
    for (const auto& item : it->second) {
        const auto& nodes = item.second;
        if (nodes.contains(endpoint)) {
            return true;
        }
    }
    return false;
}

// Called from a seastar thread
void token_metadata_impl::calculate_pending_ranges_for_leaving(
        const token_metadata& unpimplified_this,
        const abstract_replication_strategy& strategy,
        std::unordered_multimap<range<token>, inet_address>& new_pending_ranges,
        mutable_token_metadata_ptr all_left_metadata) const {
    if (_leaving_endpoints.empty()) {
        return;
    }
    // get all ranges that will be affected by leaving nodes
    std::unordered_set<range<token>> affected_ranges;
    for (auto endpoint : _leaving_endpoints) {
        auto r = strategy.get_ranges(endpoint, unpimplified_this).get0();
        for (const dht::token_range& x : r) {
            affected_ranges.emplace(x);
        }
    }
    // for each of those ranges, find what new nodes will be responsible for the range when
    // all leaving nodes are gone.
    auto metadata = token_metadata(std::make_unique<token_metadata_impl>(clone_only_token_map().get0()));
    auto affected_ranges_size = affected_ranges.size();
    tlogger.debug("In calculate_pending_ranges: affected_ranges.size={} stars", affected_ranges_size);
    for (const auto& r : affected_ranges) {
        auto t = r.end() ? r.end()->value() : dht::maximum_token();
        auto current_endpoints = strategy.calculate_natural_endpoints(t, metadata).get0();
        auto new_endpoints = strategy.calculate_natural_endpoints(t, *all_left_metadata).get0();
        for (auto ep : new_endpoints) {
          if (!current_endpoints.contains(ep)) {
            new_pending_ranges.emplace(r, ep);
          }
        }
        seastar::thread::maybe_yield();
    }
    metadata.clear_gently().get();
    tlogger.debug("In calculate_pending_ranges: affected_ranges.size={} ends", affected_ranges_size);
}

// Called from a seastar thread
void token_metadata_impl::calculate_pending_ranges_for_replacing(
        const token_metadata& unpimplified_this,
        const abstract_replication_strategy& strategy,
        std::unordered_multimap<range<token>, inet_address>& new_pending_ranges) const {
    if (_replacing_endpoints.empty()) {
        return;
    }
    for (const auto& node : _replacing_endpoints) {
        auto existing_node = node.first;
        auto replacing_node = node.second;
        auto address_ranges = strategy.get_ranges(existing_node, unpimplified_this).get0();
        for (const dht::token_range& x : address_ranges) {
            seastar::thread::maybe_yield();
            tlogger.debug("Node {} replaces {} for range {}", replacing_node, existing_node, x);
            new_pending_ranges.emplace(x, replacing_node);
        }
    }
}

// Called from a seastar thread
void token_metadata_impl::calculate_pending_ranges_for_bootstrap(
        const abstract_replication_strategy& strategy,
        std::unordered_multimap<range<token>, inet_address>& new_pending_ranges,
        mutable_token_metadata_ptr all_left_metadata, dc_rack_fn& get_dc_rack) const {
    // For each of the bootstrapping nodes, simply add and remove them one by one to
    // allLeftMetadata and check in between what their ranges would be.
    std::unordered_multimap<inet_address, token> bootstrap_addresses;
    for (auto& x : _bootstrap_tokens) {
        bootstrap_addresses.emplace(x.second, x.first);
    }

    // TODO: share code with unordered_multimap_to_unordered_map
    std::unordered_map<inet_address, std::unordered_set<token>> tmp;
    for (auto& x : bootstrap_addresses) {
        auto& addr = x.first;
        auto& t = x.second;
        tmp[addr].insert(t);
    }
    for (auto& x : tmp) {
        auto& endpoint = x.first;
        auto& tokens = x.second;
        all_left_metadata->update_topology(endpoint, get_dc_rack(endpoint), node::state::joining);
        all_left_metadata->update_normal_tokens(tokens, endpoint).get();
        auto address_ranges = strategy.get_ranges(endpoint, *all_left_metadata).get0();
        for (const dht::token_range& x : address_ranges) {
            new_pending_ranges.emplace(x, endpoint);
        }
        all_left_metadata->_impl->remove_endpoint(endpoint);
    }
    all_left_metadata->_impl->sort_tokens();
}

future<> token_metadata_impl::update_pending_ranges(
        const token_metadata& unpimplified_this,
        const abstract_replication_strategy& strategy, const sstring& keyspace_name, dc_rack_fn& get_dc_rack) {
    tlogger.debug("calculate_pending_ranges: keyspace_name={}, bootstrap_tokens={}, leaving nodes={}, replacing_endpoints={}",
        keyspace_name, _bootstrap_tokens, _leaving_endpoints, _replacing_endpoints);
    if (_bootstrap_tokens.empty() && _leaving_endpoints.empty() && _replacing_endpoints.empty()) {
        tlogger.debug("No bootstrapping, leaving nodes, replacing nodes -> empty pending ranges for {}", keyspace_name);
        set_pending_ranges(keyspace_name, std::unordered_multimap<range<token>, inet_address>(), can_yield::no);
        return make_ready_future<>();
    }

    return async([this, &unpimplified_this, &strategy, keyspace_name, &get_dc_rack] () mutable {
        std::unordered_multimap<range<token>, inet_address> new_pending_ranges;
        calculate_pending_ranges_for_replacing(unpimplified_this, strategy, new_pending_ranges);
        // Copy of metadata reflecting the situation after all leave operations are finished.
        auto all_left_metadata = make_token_metadata_ptr(std::make_unique<token_metadata_impl>(clone_after_all_left().get0()));
        calculate_pending_ranges_for_leaving(unpimplified_this, strategy, new_pending_ranges, all_left_metadata);
        // At this stage newPendingRanges has been updated according to leave operations. We can
        // now continue the calculation by checking bootstrapping nodes.
        calculate_pending_ranges_for_bootstrap(strategy, new_pending_ranges, all_left_metadata, get_dc_rack);
        all_left_metadata->clear_gently().get();

        // At this stage newPendingRanges has been updated according to leaving and bootstrapping nodes.
        set_pending_ranges(keyspace_name, std::move(new_pending_ranges), can_yield::yes);
    });

}

size_t token_metadata_impl::count_normal_token_owners() const {
    return _normal_token_owners.size();
}

future<> token_metadata_impl::update_normal_token_owners() {
    std::unordered_set<inet_address> eps;
    for (auto [t, ep]: _token_to_endpoint_map) {
        eps.insert(ep);
        co_await coroutine::maybe_yield();
    }
    _normal_token_owners = std::move(eps);
}

void token_metadata_impl::add_leaving_endpoint(inet_address endpoint) {
     _leaving_endpoints.emplace(endpoint);
}

void token_metadata_impl::del_leaving_endpoint(inet_address endpoint) {
     _leaving_endpoints.erase(endpoint);
}

void token_metadata_impl::add_replacing_endpoint(inet_address existing_node, inet_address replacing_node) {
    tlogger.info("Added node {} as pending replacing endpoint which replaces existing node {}",
            replacing_node, existing_node);
    _replacing_endpoints[existing_node] = replacing_node;
}

void token_metadata_impl::del_replacing_endpoint(inet_address existing_node) {
    if (_replacing_endpoints.contains(existing_node)) {
        tlogger.info("Removed node {} as pending replacing endpoint which replaces existing node {}",
                _replacing_endpoints[existing_node], existing_node);
    }
    _replacing_endpoints.erase(existing_node);
}

inet_address_vector_topology_change token_metadata_impl::pending_endpoints_for(const token& token, const sstring& keyspace_name) const {
    // Fast path 0: pending ranges not found for this keyspace_name
    const auto pr_it = _pending_ranges_interval_map.find(keyspace_name);
    if (pr_it == _pending_ranges_interval_map.end()) {
        return {};
    }

    // Fast path 1: empty pending ranges for this keyspace_name
    const auto& ks_map = pr_it->second;
    if (ks_map.empty()) {
        return {};
    }

    // Slow path: lookup pending ranges
    inet_address_vector_topology_change endpoints;
    auto interval = range_to_interval(range<dht::token>(token));
    const auto it = ks_map.find(interval);
    if (it != ks_map.end()) {
        // interval_map does not work with std::vector, convert to std::vector of ips
        endpoints = inet_address_vector_topology_change(it->second.begin(), it->second.end());
    }
    return endpoints;
}

std::map<token, inet_address> token_metadata_impl::get_normal_and_bootstrapping_token_to_endpoint_map() const {
    std::map<token, inet_address> ret(_token_to_endpoint_map.begin(), _token_to_endpoint_map.end());
    ret.insert(_bootstrap_tokens.begin(), _bootstrap_tokens.end());
    return ret;
}

std::multimap<inet_address, token> token_metadata_impl::get_endpoint_to_token_map_for_reading() const {
    std::multimap<inet_address, token> cloned;
    for (const auto& x : _token_to_endpoint_map) {
        cloned.emplace(x.second, x.first);
    }
    return cloned;
}

token_metadata::token_metadata(std::unique_ptr<token_metadata_impl> impl)
    : _impl(std::move(impl)) {
}

token_metadata::token_metadata(config cfg)
        : _impl(std::make_unique<token_metadata_impl>(std::move(cfg))) {
}

token_metadata::~token_metadata() = default;


token_metadata::token_metadata(token_metadata&&) noexcept = default;

token_metadata& token_metadata::token_metadata::operator=(token_metadata&&) noexcept = default;

const std::vector<token>&
token_metadata::sorted_tokens() const {
    return _impl->sorted_tokens();
}

future<>
token_metadata::update_normal_tokens(std::unordered_set<token> tokens, inet_address endpoint) {
    return _impl->update_normal_tokens(std::move(tokens), endpoint);
}

const token&
token_metadata::first_token(const token& start) const {
    return _impl->first_token(start);
}

size_t
token_metadata::first_token_index(const token& start) const {
    return _impl->first_token_index(start);
}

std::optional<inet_address>
token_metadata::get_endpoint(const token& token) const {
    return _impl->get_endpoint(token);
}

std::vector<token>
token_metadata::get_tokens(const inet_address& addr) const {
    return _impl->get_tokens(addr);
}

const std::unordered_map<token, inet_address>&
token_metadata::get_token_to_endpoint() const {
    return _impl->get_token_to_endpoint();
}

const std::unordered_set<inet_address>&
token_metadata::get_leaving_endpoints() const {
    return _impl->get_leaving_endpoints();
}

const std::unordered_map<token, inet_address>&
token_metadata::get_bootstrap_tokens() const {
    return _impl->get_bootstrap_tokens();
}

void
token_metadata::update_topology(inet_address ep, endpoint_dc_rack dr, std::optional<node::state> opt_st) {
    _impl->update_topology(ep, std::move(dr), std::move(opt_st));
}

boost::iterator_range<token_metadata::tokens_iterator>
token_metadata::ring_range(const token& start) const {
    return _impl->ring_range(start);
}

boost::iterator_range<token_metadata::tokens_iterator>
token_metadata::ring_range(const std::optional<dht::partition_range::bound>& start) const {
    return _impl->ring_range(start);
}

topology&
token_metadata::get_topology() {
    return _impl->get_topology();
}

const topology&
token_metadata::get_topology() const {
    return _impl->get_topology();
}

void
token_metadata::debug_show() const {
    _impl->debug_show();
}

void
token_metadata::update_host_id(const host_id& host_id, inet_address endpoint) {
    _impl->update_host_id(host_id, endpoint);
}

host_id
token_metadata::get_host_id(inet_address endpoint) const {
    return _impl->get_host_id(endpoint);
}

std::optional<host_id>
token_metadata::get_host_id_if_known(inet_address endpoint) const {
    return _impl->get_host_id_if_known(endpoint);
}

std::optional<token_metadata::inet_address>
token_metadata::get_endpoint_for_host_id(host_id host_id) const {
    return _impl->get_endpoint_for_host_id(host_id);
}

host_id_or_endpoint token_metadata::parse_host_id_and_endpoint(const sstring& host_id_string) const {
    auto res = host_id_or_endpoint(host_id_string);
    res.resolve(*this);
    return res;
}

std::unordered_map<inet_address, host_id>
token_metadata::get_endpoint_to_host_id_map_for_reading() const {
    return _impl->get_endpoint_to_host_id_map_for_reading();
}

void
token_metadata::add_bootstrap_token(token t, inet_address endpoint) {
    _impl->add_bootstrap_token(t, endpoint);
}

void
token_metadata::add_bootstrap_tokens(std::unordered_set<token> tokens, inet_address endpoint) {
    _impl->add_bootstrap_tokens(std::move(tokens), endpoint);
}

void
token_metadata::remove_bootstrap_tokens(std::unordered_set<token> tokens) {
    _impl->remove_bootstrap_tokens(std::move(tokens));
}

void
token_metadata::add_leaving_endpoint(inet_address endpoint) {
    _impl->add_leaving_endpoint(endpoint);
}

void
token_metadata::del_leaving_endpoint(inet_address endpoint) {
    _impl->del_leaving_endpoint(endpoint);
}

void
token_metadata::remove_endpoint(inet_address endpoint) {
    _impl->remove_endpoint(endpoint);
    _impl->sort_tokens();
}

bool
token_metadata::is_normal_token_owner(inet_address endpoint) const {
    return _impl->is_normal_token_owner(endpoint);
}

bool
token_metadata::is_leaving(inet_address endpoint) const {
    return _impl->is_leaving(endpoint);
}

bool
token_metadata::is_being_replaced(inet_address endpoint) const {
    return _impl->is_being_replaced(endpoint);
}

bool
token_metadata::is_any_node_being_replaced() const {
    return _impl->is_any_node_being_replaced();
}

void token_metadata::add_replacing_endpoint(inet_address existing_node, inet_address replacing_node) {
    _impl->add_replacing_endpoint(existing_node, replacing_node);
}

void token_metadata::del_replacing_endpoint(inet_address existing_node) {
    _impl->del_replacing_endpoint(existing_node);
}

future<token_metadata> token_metadata::clone_async() const noexcept {
    return _impl->clone_async().then([] (token_metadata_impl impl) {
        return make_ready_future<token_metadata>(std::make_unique<token_metadata_impl>(std::move(impl)));
    });
}

future<token_metadata>
token_metadata::clone_only_token_map() const noexcept {
    return _impl->clone_only_token_map().then([] (token_metadata_impl impl) {
        return token_metadata(std::make_unique<token_metadata_impl>(std::move(impl)));
    });
}

future<token_metadata>
token_metadata::clone_after_all_left() const noexcept {
    return _impl->clone_after_all_left().then([] (token_metadata_impl impl) {
        return token_metadata(std::make_unique<token_metadata_impl>(std::move(impl)));
    });
}

future<> token_metadata::clear_gently() noexcept {
    return _impl->clear_gently();
}

dht::token_range_vector
token_metadata::get_primary_ranges_for(std::unordered_set<token> tokens) const {
    return _impl->get_primary_ranges_for(std::move(tokens));
}

dht::token_range_vector
token_metadata::get_primary_ranges_for(token right) const {
    return _impl->get_primary_ranges_for(right);
}

boost::icl::interval<token>::interval_type
token_metadata::range_to_interval(range<dht::token> r) {
    return token_metadata_impl::range_to_interval(std::move(r));
}

range<dht::token>
token_metadata::interval_to_range(boost::icl::interval<token>::interval_type i) {
    return token_metadata_impl::interval_to_range(std::move(i));
}

bool
token_metadata::has_pending_ranges(sstring keyspace_name, inet_address endpoint) const {
    return _impl->has_pending_ranges(std::move(keyspace_name), endpoint);
}

future<>
token_metadata::update_pending_ranges(const abstract_replication_strategy& strategy, const sstring& keyspace_name, dc_rack_fn& get_dc_rack) {
    return _impl->update_pending_ranges(*this, strategy, keyspace_name, get_dc_rack);
}

token
token_metadata::get_predecessor(token t) const {
    return _impl->get_predecessor(t);
}

const std::unordered_set<inet_address>&
token_metadata::get_all_endpoints() const {
    return _impl->get_all_endpoints();
}

size_t
token_metadata::count_normal_token_owners() const {
    return _impl->count_normal_token_owners();
}

inet_address_vector_topology_change
token_metadata::pending_endpoints_for(const token& token, const sstring& keyspace_name) const {
    return _impl->pending_endpoints_for(token, keyspace_name);
}

std::multimap<inet_address, token>
token_metadata::get_endpoint_to_token_map_for_reading() const {
    return _impl->get_endpoint_to_token_map_for_reading();
}

std::map<token, inet_address>
token_metadata::get_normal_and_bootstrapping_token_to_endpoint_map() const {
    return _impl->get_normal_and_bootstrapping_token_to_endpoint_map();
}

long
token_metadata::get_ring_version() const {
    return _impl->get_ring_version();
}

void
token_metadata::invalidate_cached_rings() {
    _impl->invalidate_cached_rings();
}

void shared_token_metadata::set(mutable_token_metadata_ptr tmptr) noexcept {
    if (_shared->get_ring_version() >= tmptr->get_ring_version()) {
        on_internal_error(tlogger, format("shared_token_metadata: must not set non-increasing version: {} -> {}", _shared->get_ring_version(), tmptr->get_ring_version()));
    }
    _shared = std::move(tmptr);
}

future<> shared_token_metadata::mutate_token_metadata(seastar::noncopyable_function<future<> (token_metadata&)> func) {
    auto lk = co_await get_lock();
    auto tm = co_await _shared->clone_async();
    // bump the token_metadata ring_version
    // to invalidate cached token/replication mappings
    // when the modified token_metadata is committed.
    tm.invalidate_cached_rings();
    co_await func(tm);
    set(make_token_metadata_ptr(std::move(tm)));
}

future<> shared_token_metadata::mutate_on_all_shards(sharded<shared_token_metadata>& stm, seastar::noncopyable_function<future<> (token_metadata&)> func) {
    auto base_shard = this_shard_id();
    assert(base_shard == 0);
    auto lk = co_await stm.local().get_lock();

    std::vector<mutable_token_metadata_ptr> pending_token_metadata_ptr;
    pending_token_metadata_ptr.resize(smp::count);
    auto tmptr = make_token_metadata_ptr(co_await stm.local().get()->clone_async());
    auto& tm = *tmptr;
    // bump the token_metadata ring_version
    // to invalidate cached token/replication mappings
    // when the modified token_metadata is committed.
    tm.invalidate_cached_rings();
    co_await func(tm);

    // Apply the mutated token_metadata only after successfully cloning it on all shards.
    pending_token_metadata_ptr[base_shard] = tmptr;
    co_await smp::invoke_on_others(base_shard, [&] () -> future<> {
        pending_token_metadata_ptr[this_shard_id()] = make_token_metadata_ptr(co_await tm.clone_async());
    });

    co_await stm.invoke_on_all([&] (shared_token_metadata& stm) {
        stm.set(std::move(pending_token_metadata_ptr[this_shard_id()]));
    });
}

host_id_or_endpoint::host_id_or_endpoint(const sstring& s, param_type restrict) {
    switch (restrict) {
    case param_type::host_id:
        try {
            id = host_id(utils::UUID(s));
        } catch (const marshal_exception& e) {
            throw std::invalid_argument(format("Invalid host_id {}: {}", s, e.what()));
        }
        break;
    case param_type::endpoint:
        try {
            endpoint = gms::inet_address(s);
        } catch (std::invalid_argument& e) {
            throw std::invalid_argument(format("Invalid inet_address {}: {}", s, e.what()));
        }
        break;
    case param_type::auto_detect:
        try {
            id = host_id(utils::UUID(s));
        } catch (const marshal_exception& e) {
            try {
                endpoint = gms::inet_address(s);
            } catch (std::invalid_argument& e) {
                throw std::invalid_argument(format("Invalid host_id or inet_address {}", s));
            }
        }
    }
}

void host_id_or_endpoint::resolve(const token_metadata& tm) {
    if (id) {
        auto endpoint_opt = tm.get_endpoint_for_host_id(id);
        if (!endpoint_opt) {
            throw std::runtime_error(format("Host ID {} not found in the cluster", id));
        }
        endpoint = *endpoint_opt;
    } else {
        auto opt_id = tm.get_host_id_if_known(endpoint);
        if (!opt_id) {
            throw std::runtime_error(format("Host inet address {} not found in the cluster", endpoint));
        }
        id = *opt_id;
    }
}

} // namespace locator
