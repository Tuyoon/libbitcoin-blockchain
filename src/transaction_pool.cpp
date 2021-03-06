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
#include <bitcoin/blockchain/transaction_pool.hpp>

#include <algorithm>
#include <cstddef>
#include <system_error>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/block_chain.hpp>
#include <bitcoin/blockchain/validate_transaction.hpp>

namespace libbitcoin {
namespace blockchain {
    
using namespace chain;
using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using std::placeholders::_4;

transaction_pool::transaction_pool(threadpool& pool, block_chain& chain,
    size_t capacity)
  : stopped_(true), buffer_(capacity), dispatch_(pool), blockchain_(chain) 
{
}

transaction_pool::~transaction_pool()
{
    delete_all(error::service_stopped);
}

void transaction_pool::start()
{
    stopped_ = false;

    // Subscribe to blockchain (organizer) reorg notifications.
    blockchain_.subscribe_reorganize(
        std::bind(&transaction_pool::handle_reorganized,
            this, _1, _2, _3, _4));
}

void transaction_pool::stop()
{
    // Stop doesn't need to be called externally and could be made private.
    // This will arise from a reorg shutdown message, so transaction_pool
    // is automatically registered for shutdown in the following sequence.
    // blockchain->organizer(orphan/block pool)->transaction_pool
    stopped_ = true;
}

bool transaction_pool::stopped()
{
    return stopped_;
}

void transaction_pool::validate(const transaction& tx,
    validate_handler handler)
{
    dispatch_.ordered(&transaction_pool::do_validate,
        this, tx, handler);
}

void transaction_pool::do_validate(const transaction& tx,
    validate_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped, tx, hash_digest(), index_list());
        return;
    }

    const auto validate = std::make_shared<validate_transaction>(
        blockchain_, tx, *this, dispatch_);

    validate->start(
        dispatch_.ordered_delegate(&transaction_pool::handle_validated,
            this, _1, _2, _3, _4, handler));
}

void transaction_pool::handle_validated(const code& ec,
    const transaction& tx, const hash_digest& hash,
    const index_list& unconfirmed, validate_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped, tx, hash, index_list());
        return;
    }

    if (ec == error::input_not_found || ec == error::validate_inputs_failed)
    {
        BITCOIN_ASSERT(unconfirmed.size() == 1);
        handler(ec, tx, hash, unconfirmed);
        return;
    }

    if (ec)
    {
        BITCOIN_ASSERT(unconfirmed.empty());
        handler(ec, tx, hash, index_list());
        return;
    }

    // Recheck the memory pool, as a duplicate may have been added.
    if (is_in_pool(hash))
        handler(error::duplicate, tx, hash, index_list());
    else
        handler(error::success, tx, hash, unconfirmed);
}

// handle_confirm will never fire if handle_validate returns a failure code.
void transaction_pool::store(const transaction& tx,
    confirm_handler handle_confirm, validate_handler handle_validate)
{
    if (stopped())
    {
        handle_validate(error::service_stopped, tx, hash_digest(),
            index_list());
        return;
    }

    validate(tx, 
        std::bind(&transaction_pool::do_store,
            this, _1, _2, _3, _4, handle_confirm, handle_validate));
}

void transaction_pool::do_store(const code& ec, const transaction& tx,
    const hash_digest& hash, const index_list& unconfirmed,
    confirm_handler handle_confirm, validate_handler handle_validate)
{
    if (!ec)
    {
        add(tx, handle_confirm);
        log::debug(LOG_BLOCKCHAIN)
            << "Transaction saved to mempool (" << buffer_.size() << ")";
    }

    handle_validate(ec, tx, hash, unconfirmed);
}

void transaction_pool::fetch(const hash_digest& transaction_hash,
    fetch_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped, transaction());
        return;
    }

    const auto tx_fetcher = [this, transaction_hash, handler]()
    {
        const auto it = find(transaction_hash);
        if (it == buffer_.end())
            handler(error::not_found, transaction());
        else
            handler(error::success, it->tx);
    };

    dispatch_.ordered(tx_fetcher);
}

void transaction_pool::exists(const hash_digest& tx_hash,
    exists_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    const auto get_existence = [this, tx_hash, handler]()
    {
        handler(is_in_pool(tx_hash) ? error::success : error::not_found);
    };

    dispatch_.ordered(get_existence);
}

void transaction_pool::handle_reorganized(const code& ec, size_t fork_point,
    const block_chain::list& new_blocks,
    const block_chain::list& replaced_blocks)
{
    if (ec == error::service_stopped)
    {
        stop();
        return;
    }

    if (ec)
    {
        log::debug(LOG_BLOCKCHAIN)
            << "Failure in tx pool reorganize: " << ec.message();
        stop();
        return;
    }

    log::debug(LOG_BLOCKCHAIN)
        << "Reorganize: tx pool size (" << buffer_.size()
        << ") forked at (" << fork_point
        << ") new blocks (" << new_blocks.size()
        << ") replace blocks (" << replaced_blocks.size() << ")";

    if (replaced_blocks.empty())
        dispatch_.ordered(
            std::bind(&transaction_pool::delete_superseded,
                this, new_blocks));
    else
        dispatch_.ordered(
            std::bind(&transaction_pool::delete_all,
                this, error::blockchain_reorganized));

    // new blocks come in - remove txs in new
    // old blocks taken out - resubmit txs in old
    blockchain_.subscribe_reorganize(
        std::bind(&transaction_pool::handle_reorganized,
            this, _1, _2, _3, _4));
}

void transaction_pool::add(const transaction& tx, confirm_handler handler)
{
    // When a new tx is added to the buffer drop the oldest.
    if (buffer_.size() == buffer_.capacity())
        delete_package(error::pool_filled);

    // Store a precomputed tx hash to make lookups faster.
    buffer_.push_back({ tx.hash(), tx, handler });
}

// There has been a reorg, clear the memory pool.
// The alternative would be resubmit all tx from the cleared blocks.
// Ordering would be reverse of chain age and then mempool by age.
void transaction_pool::delete_all(const code& ec)
{
    // See http://www.jwz.org/doc/worse-is-better.html
    // for why we take this approach.
    // We return with an error_code and don't handle this case.
    for (const auto& entry: buffer_)
        entry.handle_confirm(ec, entry.tx, entry.hash);

    buffer_.clear();
}

// Delete mempool txs that are obsoleted by new blocks acceptance.
void transaction_pool::delete_superseded(const block_chain::list& blocks)
{
    // Deletion by hash returns success code, the other a double-spend error.
    delete_confirmed_in_blocks(blocks);
    delete_spent_in_blocks(blocks);
}

// Delete mempool txs that are duplicated in the new blocks.
void transaction_pool::delete_confirmed_in_blocks(
    const block_chain::list& blocks)
{
    if (stopped() || buffer_.empty())
        return;

    for (const auto block: blocks)
        for (const auto& tx: block->transactions)
            delete_single(tx, error::success);
}

// Delete all txs that spend a previous output of any tx in the new blocks.
void transaction_pool::delete_spent_in_blocks(
    const block_chain::list& blocks)
{
    if (stopped() || buffer_.empty())
        return;

    for (const auto block: blocks)
        for (const auto& tx: block->transactions)
            for (const auto& input: tx.inputs)
                delete_dependencies(input.previous_output,
                    error::double_spend);
}

// Delete any tx that spends any output of this tx.
void transaction_pool::delete_dependencies(const output_point& point,
    const code& ec)
{
    // TODO: implement output_point comparison operator.
    const auto comparitor = [&point](const input& input)
    {
        return input.previous_output.index == point.index &&
            input.previous_output.hash == point.hash;
    };

    delete_dependencies(comparitor, ec);
}

// Delete any tx that spends any output of this tx.
void transaction_pool::delete_dependencies(const hash_digest& tx_hash,
    const code& ec)
{
    const auto comparitor = [&tx_hash](const input& input)
    {
        return input.previous_output.hash == tx_hash;
    };

    delete_dependencies(comparitor, ec);
}

// This is horribly inefficient, but it's simple.
// TODO: Create persistent multi-indexed memory pool (including age and
// children) and perform this pruning trivialy (and add policy over it).
void transaction_pool::delete_dependencies(input_compare is_dependency,
    const code& ec)
{
    std::vector<entry> dependencies;
    for (const auto& entry: buffer_)
        for (const auto& input: entry.tx.inputs)
            if (is_dependency(input))
            {
                dependencies.push_back(entry);
                break;
            }

    // We queue deletion to protect the iterator.
    for (const auto& dependency: dependencies)
        delete_package(dependency.tx, dependency.hash, ec);
}

void transaction_pool::delete_package(const code& ec)
{
    if (stopped() || buffer_.empty())
        return;

    // Must copy the entry because it is going to be deleted from the list.
    const auto oldest = buffer_.front();

    oldest.handle_confirm(ec, oldest.tx, oldest.hash);
    delete_package(oldest.tx, oldest.hash, ec);
}

void transaction_pool::delete_package(const transaction& tx,
    const hash_digest& tx_hash, const code& ec)
{
    if (delete_single(tx, tx_hash, ec))
        delete_dependencies(tx_hash, ec);
}

bool transaction_pool::delete_single(const transaction& tx, 
    const hash_digest& tx_hash, const code& ec)
{
    if (stopped())
        return false;

    const auto matched = [&tx_hash](const entry& entry)
    {
        return entry.hash == tx_hash;
    };

    const auto it = std::find_if(buffer_.begin(), buffer_.end(), matched);
    if (it == buffer_.end())
        return false;

    it->handle_confirm(ec, tx, tx_hash);
    buffer_.erase(it);
    return true;
}

bool transaction_pool::delete_single(const transaction& tx, const code& ec)
{
    return delete_single(tx, tx.hash(), ec);
}

bool transaction_pool::find(transaction& out_tx,
    const hash_digest& tx_hash) const
{
    const auto it = find(tx_hash);
    const auto found = it != buffer_.end();
    if (found)
        out_tx = it->tx;

    return found;
}

transaction_pool::iterator transaction_pool::find(
    const hash_digest& tx_hash) const
{
    const auto found = [&tx_hash](const entry& entry)
    {
        return entry.hash == tx_hash;
    };

    return std::find_if(buffer_.begin(), buffer_.end(), found);
}

bool transaction_pool::is_in_pool(const hash_digest& tx_hash) const
{
    return find(tx_hash) != buffer_.end();
}

bool transaction_pool::is_spent_in_pool(const transaction& tx) const
{
    const auto found = [this, &tx](const input& input)
    {
        return is_spent_in_pool(input.previous_output);
    };

    const auto& inputs = tx.inputs;
    const auto spend = std::find_if(inputs.begin(), inputs.end(), found);
    return spend != inputs.end();
}

bool transaction_pool::is_spent_in_pool(const output_point& outpoint) const
{
    const auto found = [this, &outpoint](const entry& entry)
    {
        return is_spent_by_tx(outpoint, entry.tx);
    };

    const auto spend = std::find_if(buffer_.begin(), buffer_.end(), found);
    return spend != buffer_.end();
}

bool transaction_pool::is_spent_by_tx(const output_point& outpoint,
    const transaction& tx)
{
    const auto found = [&outpoint](const input& input)
    {
        return input.previous_output == outpoint;
    };

    const auto& inputs = tx.inputs;
    const auto spend = std::find_if(inputs.begin(), inputs.end(), found);
    return spend != inputs.end();
}

} // namespace blockchain
} // namespace libbitcoin
