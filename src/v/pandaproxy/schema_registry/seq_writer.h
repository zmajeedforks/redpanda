//// Copyright 2021 Vectorized, Inc.
////
//// Use of this software is governed by the Business Source License
//// included in the file licenses/BSL.md
////
//// As of the Change Date specified in that file, in accordance with
//// the Business Source License, use of this software will be governed
//// by the Apache License, Version 2.

#pragma once

#include "kafka/client/client.h"
#include "pandaproxy/schema_registry/sharded_store.h"
#include "pandaproxy/schema_registry/types.h"
#include "random/simple_time_jitter.h"
#include "utils/retry.h"

namespace pandaproxy::schema_registry {

using namespace std::chrono_literals;

static const int max_retries = 32;

class seq_writer final : public ss::peering_sharded_service<seq_writer> {
public:
    seq_writer(
      ss::smp_service_group smp_group,
      ss::sharded<kafka::client::client>& client,
      sharded_store& store)
      : _smp_opts(ss::smp_submit_to_options{smp_group})
      , _client(client)
      , _store(store) {}

    ss::future<> read_sync();

    // API for readers: notify us when they have read and applied an offset
    ss::future<> advance_offset(model::offset offset);

    ss::future<schema_id>
    write_subject_version(subject sub, schema_definition def, schema_type type);

private:
    ss::smp_submit_to_options _smp_opts;

    ss::sharded<kafka::client::client>& _client;
    sharded_store& _store;

    void advance_offset_inner(model::offset offset);

    struct WriteCollision : public std::exception {
        const char* what() const throw() { return "Write Collision"; }
    };

    simple_time_jitter<ss::lowres_clock> _jitter{
      std::chrono::milliseconds{100}};

    /// Helper for write paths that use sequence+retry logic to synchronize
    /// multiple writing nodes.
    template<typename F>
    auto sequenced_write(F f) {
        auto base_backoff = _jitter.next_duration();
        auto remote = [base_backoff, f](seq_writer& seq) {
            auto units = ss::get_units(seq._write_sem, 1);

            return retry_with_backoff(
              max_retries,
              [f, &seq]() { return seq.sequenced_write_inner(f); },
              base_backoff);
        };

        return container().invoke_on(0, _smp_opts, remote);
    }

    /// The part of sequenced_write that runs on shard zero
    ///
    /// This is declared as a separate member function rather than
    /// inline in sequenced_write, to avoid compiler issues (and resulting
    /// crashes) seen when passing in a coroutine lambda.
    template<typename F>
    ss::future<typename std::invoke_result_t<F, model::offset, seq_writer&>::
                 value_type::value_type>
    sequenced_write_inner(F f) {
        // If we run concurrently with them, redundant replays to the store
        // will be safely dropped based on offset.
        co_await read_sync();

        auto next_offset = _loaded_offset + model::offset{1};
        auto r = co_await f(next_offset, *this);

        if (r.has_value()) {
            co_return r.value();
        } else {
            throw WriteCollision();
        }
    }

    ss::future<bool>
    produce_and_check(model::offset write_at, model::record_batch batch);

    /// Block until this offset is available, fetching if necessary
    ss::future<> wait_for(model::offset offset);

    // Global (Shard 0) State
    // ======================

    /// Serialize wait_for operations, to avoid issuing
    /// gratuitous number of reads to the topic on concurrent GETs.
    ss::semaphore _wait_for_sem{1};

    /// Shard 0 only: Reads have progressed as far as this offset
    model::offset _loaded_offset{-1};

    /// Shard 0 only: Serialize write operations.
    ss::semaphore _write_sem{1};

    // ======================
    // End of Shard 0 state
};

} // namespace pandaproxy::schema_registry
