#include "bytes/bytes.h"
#include "bytes/iobuf_file.h"
#include "bytes/iobuf_parser.h"
#include "random/generators.h"
#include "reflection/adl.h"
#include "storage/compacted_index_reader.h"
#include "storage/compacted_index_writer.h"
#include "storage/compaction_reducers.h"
#include "storage/segment_utils.h"
#include "test_utils/fixture.h"
#include "units.h"

#include <boost/test/unit_test_suite.hpp>

struct compacted_topic_fixture {};
FIXTURE_TEST(format_verification, compacted_topic_fixture) {
    iobuf index_data;
    auto idx = storage::make_file_backed_compacted_index(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      1_KiB);
    const auto key = random_generators::get_bytes(1024);
    idx.index(key, model::offset(42), 66).get();
    idx.close().get();
    info("{}", idx);
    BOOST_REQUIRE_EQUAL(index_data.size_bytes(), 1047);
    iobuf_parser p(index_data.share(0, index_data.size_bytes()));
    (void)p.consume_type<uint16_t>(); // SIZE
    (void)p.consume_type<uint8_t>();  // TYPE
    auto [offset, _1] = p.read_varlong();
    BOOST_REQUIRE_EQUAL(model::offset(offset), model::offset(42));
    auto [delta, _2] = p.read_varlong();
    BOOST_REQUIRE_EQUAL(delta, 66);
    const auto key_result = p.read_bytes(1024);
    BOOST_REQUIRE_EQUAL(key, key_result);
    auto footer = reflection::adl<storage::compacted_index::footer>{}.from(p);
    info("{}", footer);
    BOOST_REQUIRE_EQUAL(footer.keys, 1);
    BOOST_REQUIRE_EQUAL(
      footer.size,
      sizeof(uint16_t)
        + 1 /*type*/ + 1 /*offset*/ + 2 /*delta*/ + 1024 /*key*/);
    BOOST_REQUIRE_EQUAL(footer.version, 0);
    BOOST_REQUIRE(footer.crc != 0);
}
FIXTURE_TEST(format_verification_max_key, compacted_topic_fixture) {
    iobuf index_data;
    auto idx = storage::make_file_backed_compacted_index(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      1_MiB);
    const auto key = random_generators::get_bytes(1_MiB);
    idx.index(key, model::offset(42), 66).get();
    idx.close().get();
    info("{}", idx);

    BOOST_REQUIRE_EQUAL(
      index_data.size_bytes(),
      storage::compacted_index::footer_size
        + std::numeric_limits<uint16_t>::max());
    iobuf_parser p(index_data.share(0, index_data.size_bytes()));

    const size_t entry = p.consume_type<uint16_t>(); // SIZE
    BOOST_REQUIRE_EQUAL(
      entry, std::numeric_limits<uint16_t>::max() - sizeof(uint16_t));
}
FIXTURE_TEST(format_verification_roundtrip, compacted_topic_fixture) {
    iobuf index_data;
    auto idx = storage::make_file_backed_compacted_index(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      1_MiB);
    const auto key = random_generators::get_bytes(20);
    idx.index(key, model::offset(42), 66).get();
    idx.close().get();
    info("{}", idx);

    auto rdr = storage::make_file_backed_compacted_reader(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      32_KiB);
    auto footer = rdr.load_footer().get0();
    BOOST_REQUIRE_EQUAL(footer.keys, 1);
    BOOST_REQUIRE_EQUAL(footer.version, 0);
    BOOST_REQUIRE(footer.crc != 0);
    auto vec = compaction_index_reader_to_memory(std::move(rdr)).get0();
    BOOST_REQUIRE_EQUAL(vec.size(), 1);
    BOOST_REQUIRE_EQUAL(vec[0].offset, model::offset(42));
    BOOST_REQUIRE_EQUAL(vec[0].delta, 66);
    BOOST_REQUIRE_EQUAL(vec[0].key, key);
}
FIXTURE_TEST(
  format_verification_roundtrip_exceeds_capacity, compacted_topic_fixture) {
    iobuf index_data;
    auto idx = storage::make_file_backed_compacted_index(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      1_MiB);
    const auto key = random_generators::get_bytes(1_MiB);
    idx.index(key, model::offset(42), 66).get();
    idx.close().get();
    info("{}", idx);

    auto rdr = storage::make_file_backed_compacted_reader(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      32_KiB);
    auto footer = rdr.load_footer().get0();
    BOOST_REQUIRE_EQUAL(footer.keys, 1);
    BOOST_REQUIRE_EQUAL(footer.version, 0);
    BOOST_REQUIRE(footer.crc != 0);
    auto vec = compaction_index_reader_to_memory(std::move(rdr)).get0();
    BOOST_REQUIRE_EQUAL(vec.size(), 1);
    BOOST_REQUIRE_EQUAL(vec[0].offset, model::offset(42));
    BOOST_REQUIRE_EQUAL(vec[0].delta, 66);
    BOOST_REQUIRE_EQUAL(vec[0].key.size(), 65529);
    BOOST_REQUIRE_EQUAL(vec[0].key, bytes_view(key.data(), 65529));
}

FIXTURE_TEST(truncation_reducer_drop_all, compacted_topic_fixture) {
    iobuf index_data;
    auto idx = storage::make_file_backed_compacted_index(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      1_MiB);
    for (auto i = 0; i < 100; ++i) {
        const auto key = random_generators::get_bytes(1_KiB);
        idx.index(key, model::offset(i), 0).get();
    }
    idx.truncate(model::offset(0)).get();
    idx.close().get();
    info("{}", idx);

    auto rdr = storage::make_file_backed_compacted_reader(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      32_KiB);
    auto bitmap = rdr
                    .consume(
                      storage::internal::truncation_offset_reducer(),
                      model::no_timeout)
                    .get0();
    BOOST_REQUIRE_EQUAL(bitmap.cardinality(), 0);
}
FIXTURE_TEST(truncation_reducer_drop_some, compacted_topic_fixture) {
    iobuf index_data;
    auto idx = storage::make_file_backed_compacted_index(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      1_MiB);
    for (auto i = 0; i < 100; ++i) {
        const auto key = random_generators::get_bytes(1_KiB);
        idx.index(key, model::offset(i), 0).get();
    }
    idx.truncate(model::offset(50)).get();
    idx.close().get();
    info("{}", idx);

    auto rdr = storage::make_file_backed_compacted_reader(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      32_KiB);
    auto bitmap = rdr
                    .consume(
                      storage::internal::truncation_offset_reducer(),
                      model::no_timeout)
                    .get0();
    info("bitmap: {}", bitmap.toString());
    BOOST_REQUIRE_EQUAL(bitmap.cardinality(), 50);

    rdr.reset();
    auto vec = compaction_index_reader_to_memory(rdr).get0();
    for (auto bit : bitmap) {
        const auto& e = vec[bit];
        const model::offset o = e.offset + model::offset(e.delta);
        BOOST_REQUIRE_LT(o, model::offset(50));
    }
}
FIXTURE_TEST(truncation_reducer_with_key_reducer, compacted_topic_fixture) {
    iobuf index_data;
    auto idx = storage::make_file_backed_compacted_index(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      // FORCE eviction with every key basically
      1_KiB);

    const auto key1 = random_generators::get_bytes(1_KiB);
    const auto key2 = random_generators::get_bytes(1_KiB);
    for (auto i = 0; i < 100; ++i) {
        bytes_view put_key;
        if (i % 2) {
            put_key = key1;
        } else {
            put_key = key2;
        }
        idx.index(put_key, model::offset(i), 0).get();
    }
    idx.truncate(model::offset(50)).get();
    idx.close().get();
    info("{}", idx);

    auto rdr = storage::make_file_backed_compacted_reader(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      32_KiB);
    auto truncate_bitmap = rdr
                             .consume(
                               storage::internal::truncation_offset_reducer(),
                               model::no_timeout)
                             .get0();
    info("truncate bitmap: {}", truncate_bitmap.toString());
    BOOST_REQUIRE_EQUAL(truncate_bitmap.cardinality(), 50);

    // get all keys
    auto vec = compaction_index_reader_to_memory(rdr).get0();
    BOOST_REQUIRE_EQUAL(vec.size(), 101 /*100 entries + 1 truncation*/);

    // final entry bitmap
    rdr.reset();
    auto key_bitmap = rdr
                        .consume(
                          storage::internal::compaction_key_reducer(
                            std::move(truncate_bitmap)),
                          model::no_timeout)
                        .get0();
    info("key bitmap: {}", key_bitmap.toString());
    BOOST_REQUIRE_EQUAL(key_bitmap.cardinality(), 2);
    for (auto bit : key_bitmap) {
        const auto& e = vec[bit];
        const model::offset o = e.offset + model::offset(e.delta);
        BOOST_REQUIRE_LT(o, model::offset(50));
    }
}
FIXTURE_TEST(key_reducer_no_truncate_filter, compacted_topic_fixture) {
    iobuf index_data;
    auto idx = storage::make_file_backed_compacted_index(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      // FORCE eviction with every key basically
      1_KiB);

    const auto key1 = random_generators::get_bytes(1_KiB);
    const auto key2 = random_generators::get_bytes(1_KiB);
    for (auto i = 0; i < 100; ++i) {
        bytes_view put_key;
        if (i % 2) {
            put_key = key1;
        } else {
            put_key = key2;
        }
        idx.index(put_key, model::offset(i), 0).get();
    }
    idx.close().get();
    info("{}", idx);

    auto rdr = storage::make_file_backed_compacted_reader(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      32_KiB);
    auto key_bitmap = rdr
                        .consume(
                          storage::internal::compaction_key_reducer(
                            std::nullopt),
                          model::no_timeout)
                        .get0();

    // get all keys
    auto vec = compaction_index_reader_to_memory(rdr).get0();
    BOOST_REQUIRE_EQUAL(vec.size(), 100);

    info("key bitmap: {}", key_bitmap.toString());
    BOOST_REQUIRE_EQUAL(key_bitmap.cardinality(), 2);
    BOOST_REQUIRE(key_bitmap.contains(98));
    BOOST_REQUIRE(key_bitmap.contains(99));
}

FIXTURE_TEST(key_reducer_max_mem, compacted_topic_fixture) {
    iobuf index_data;
    auto idx = storage::make_file_backed_compacted_index(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      // FORCE eviction with every key basically
      1_KiB);

    const auto key1 = random_generators::get_bytes(1_KiB);
    const auto key2 = random_generators::get_bytes(1_KiB);
    for (auto i = 0; i < 100; ++i) {
        bytes_view put_key;
        if (i % 2) {
            put_key = key1;
        } else {
            put_key = key2;
        }
        idx.index(put_key, model::offset(i), 0).get();
    }
    idx.close().get();
    info("{}", idx);

    auto rdr = storage::make_file_backed_compacted_reader(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      32_KiB);

    rdr.reset();
    auto small_mem_bitmap = rdr
                              .consume(
                                storage::internal::compaction_key_reducer(
                                  std::nullopt, 1_KiB),
                                model::no_timeout)
                              .get0();

    /*
      There are 2 keys exactly.
      Each key is exactly 1KB
      We need 2KB + 1 byte to ensure it fits in the memory map
     */
    rdr.reset();
    auto exact_mem_bitmap = rdr
                              .consume(
                                storage::internal::compaction_key_reducer(
                                  std::nullopt, 2_KiB + 1),
                                model::no_timeout)
                              .get0();

    // get all keys
    auto vec = compaction_index_reader_to_memory(rdr).get0();
    BOOST_REQUIRE_EQUAL(vec.size(), 100);

    info("small key bitmap: {}", small_mem_bitmap.toString());
    info("exact key bitmap: {}", exact_mem_bitmap.toString());
    BOOST_REQUIRE_EQUAL(exact_mem_bitmap.cardinality(), 2);
    BOOST_REQUIRE_EQUAL(small_mem_bitmap.cardinality(), 100);
    BOOST_REQUIRE(exact_mem_bitmap.contains(98));
    BOOST_REQUIRE(exact_mem_bitmap.contains(99));
}
FIXTURE_TEST(index_filtered_copy_tests, compacted_topic_fixture) {
    iobuf index_data;
    auto idx = storage::make_file_backed_compacted_index(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      // FORCE eviction with every key basically
      1_KiB);

    const auto key1 = random_generators::get_bytes(1_KiB);
    const auto key2 = random_generators::get_bytes(1_KiB);
    for (auto i = 0; i < 100; ++i) {
        bytes_view put_key;
        if (i % 2) {
            put_key = key1;
        } else {
            put_key = key2;
        }
        idx.index(put_key, model::offset(i), 0).get();
    }
    idx.close().get();
    info("{}", idx);

    auto rdr = storage::make_file_backed_compacted_reader(
      "dummy name",
      ss::file(ss::make_shared(iobuf_file(index_data))),
      ss::default_priority_class(),
      32_KiB);

    auto bitmap = storage::internal::index_of_index_of_entries(rdr).get0();
    {
        auto vec = compaction_index_reader_to_memory(rdr).get0();
        BOOST_REQUIRE_EQUAL(vec.size(), 100);
    }
    info("key bitmap: {}", bitmap.toString());
    BOOST_REQUIRE_EQUAL(bitmap.cardinality(), 2);
    BOOST_REQUIRE(bitmap.contains(98));
    BOOST_REQUIRE(bitmap.contains(99));

    // the main test
    iobuf final_data;
    auto final_idx = storage::make_file_backed_compacted_index(
      "dummy name - final idx",
      ss::file(ss::make_shared(iobuf_file(final_data))),
      ss::default_priority_class(),
      // FORCE eviction with every key basically
      1_KiB);

    rdr.reset();
    rdr
      .consume(
        storage::internal::index_filtered_copy_reducer(
          std::move(bitmap), final_idx),
        model::no_timeout)
      .get();
    final_idx.close().get();
    {
        auto final_rdr = storage::make_file_backed_compacted_reader(
          "dummy name - final ",
          ss::file(ss::make_shared(iobuf_file(final_data))),
          ss::default_priority_class(),
          32_KiB);
        auto vec = compaction_index_reader_to_memory(final_rdr).get0();
        BOOST_REQUIRE_EQUAL(vec.size(), 2);
        BOOST_REQUIRE_EQUAL(vec[0].offset, model::offset(98));
        BOOST_REQUIRE_EQUAL(vec[1].offset, model::offset(99));
    }
}
