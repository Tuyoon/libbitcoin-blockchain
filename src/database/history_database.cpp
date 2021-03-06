/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/blockchain/database/history_database.hpp>

#include <boost/filesystem.hpp>
//#include <boost/iostreams/stream.hpp>
//#include <bitcoin/blockchain/pointer_array_source.hpp>

namespace libbitcoin {
namespace blockchain {

constexpr size_t number_buckets = 97210744;
BC_CONSTEXPR size_t header_size = htdb_record_header_fsize(number_buckets);
BC_CONSTEXPR size_t initial_lookup_file_size = header_size + min_records_fsize;

BC_CONSTEXPR position_type allocator_offset = header_size;
BC_CONSTEXPR size_t alloc_record_size = map_record_fsize_multimap<short_hash>();

constexpr size_t value_size = 1 + 36 + 4 + 8;
BC_CONSTEXPR size_t row_record_size = record_fsize_htdb<hash_digest>(value_size);

history_database::history_database(const boost::filesystem::path& lookup_filename,
    const boost::filesystem::path& rows_filename)
  : lookup_file_(lookup_filename), 
    header_(lookup_file_, 0),
    allocator_(lookup_file_, allocator_offset, alloc_record_size),
    start_lookup_(header_, allocator_, lookup_filename.string()),
    rows_file_(rows_filename), 
    rows_(rows_file_, 0, row_record_size),
    linked_rows_(rows_),
    map_(start_lookup_, linked_rows_, rows_filename.string())
{
    BITCOIN_ASSERT(lookup_file_.data() != nullptr);
    BITCOIN_ASSERT(rows_file_.data() != nullptr);
}

void history_database::create()
{
    lookup_file_.resize(initial_lookup_file_size);
    header_.create(number_buckets);
    allocator_.create();

    rows_file_.resize(min_records_fsize);
    rows_.create();
}

void history_database::start()
{
    header_.start();
    allocator_.start();
    rows_.start();
}

void history_database::add_output(
    const short_hash& key, const chain::output_point& outpoint,
    const uint32_t output_height, const uint64_t value)
{
    auto write = [&](uint8_t* data)
    {
        auto serial = make_serializer(data);
        serial.write_byte(0);
        data_chunk raw_outpoint = outpoint.to_data();
        serial.write_data(raw_outpoint);
        serial.write_4_bytes_little_endian(output_height);
        serial.write_8_bytes_little_endian(value);
    };
    map_.add_row(key, write);
}

void history_database::add_spend(
    const short_hash& key, const chain::output_point& previous,
    const chain::input_point& spend, const size_t spend_height)
{
    auto write = [&](uint8_t* data)
    {
        auto serial = make_serializer(data);
        serial.write_byte(1);
        data_chunk raw_spend = spend.to_data();
        serial.write_data(raw_spend);
        serial.write_4_bytes_little_endian(spend_height);
        serial.write_8_bytes_little_endian(
            block_chain::spend_checksum(previous));
    };
    map_.add_row(key, write);
}

void history_database::delete_last_row(const short_hash& key)
{
    map_.delete_last_row(key);
}

// Each row contains a start byte which signifies whether
// it is an output or a spend.
inline block_chain::point_kind marker_to_kind(uint8_t marker)
{
    BITCOIN_ASSERT(marker == 0 || marker == 1);
    return marker == 0 ? block_chain::point_kind::output : 
        block_chain::point_kind::spend;
}

block_chain::history history_database::get(const short_hash& key,
    const size_t limit, const size_t from_height) const
{
    // Read the height value from the row.
    auto read_height = [](const uint8_t* data)
    {
        constexpr position_type height_position = 1 + 36;
        return from_little_endian_unsafe<uint32_t>(data + height_position);
    };

    // Read a row from the data into the history list.
    block_chain::history history;
    auto read_row = [&history](const uint8_t* data/*, std::streamsize length*/)
    {
//        boost::iostreams::stream<byte_pointer_array_source> istream(data, length);
//        istream.exceptions(std::ios_base::failbit);
//
//        block_chain::history_row result
//        {
//            // output or spend?
//            marker_to_id(read_byte(istream)),
//            // point
//            chain::point::factory_from_data(istream),
//            // height
//            read_4_bytes(istream),
//            // value or checksum
//            read_8_bytes(istream)
//        };
//
////        if (!istream)
////            throw end_of_stream();
//
//        return result;

        auto deserial = make_deserializer_unsafe(data);
        return block_chain::history_row
        {
            // output or spend?
            marker_to_kind(deserial.read_byte()),

            // point
            chain::point::factory_from_data(deserial),

            // height
            deserial.read_4_bytes_little_endian(),

            // value or checksum
            { deserial.read_8_bytes_little_endian() }
        };
    };

    const index_type start = map_.lookup(key);
    for (const index_type index: multimap_iterable(linked_rows_, start))
    {
        // Stop once we reach the limit (if specified).
        if (limit && history.size() >= limit)
            break;

        const record_type data = linked_rows_.get(index);

        // Skip rows below from_height (if specified).
        if (from_height && read_height(data) < from_height)
            continue;

        // Read this row into the list.
        history.emplace_back(read_row(data /*, value_size*/));
    }

    return history;
}

void history_database::sync()
{
    allocator_.sync();
    rows_.sync();
}

history_statinfo history_database::statinfo() const
{
    return
    {
        header_.size(),
        allocator_.count(),
        rows_.count()
    };
}

} // namespace blockchain
} // namespace libbitcoin
