#include "mempool/mempool.h"
#include "logging/logging.h"
#include <sstream>

Mempool g_mempool;

MempoolEntry::MempoolEntry(std::string id, std::string hex, int64_t fee, size_t size)
    : txid(std::move(id))
    , tx_hex(std::move(hex))
    , fee_sats(fee)
    , tx_size(size)
    , fee_rate(size > 0 ? static_cast<double>(fee) / size : 0.0)
{}

bool MempoolEntry::operator<(const MempoolEntry& other) const {
    // Higher fee rate comes first
    if (fee_rate != other.fee_rate) {
        return fee_rate > other.fee_rate;
    }
    // If same fee rate, order by txid for deterministic ordering
    return txid < other.txid;
}

void Mempool::add(MempoolEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if transaction already exists
    for (const auto& e : entries_) {
        if (e.txid == entry.txid) {
            throw std::runtime_error("Transaction already in mempool");
        }
    }
    
    std::ostringstream log;
    log << "Added to mempool: " << entry.txid 
        << " (fee_rate: " << entry.fee_rate << " sats/byte)";
    
    entries_.insert(std::move(entry));
    
    LOG_INFO(log.str());
}

std::vector<MempoolEntry> Mempool::get_all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<MempoolEntry>(entries_.begin(), entries_.end());
}

bool Mempool::remove(const std::string& txid) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->txid == txid) {
            entries_.erase(it);
            LOG_INFO("Removed from mempool: " + txid);
            return true;
        }
    }
    return false;
}

size_t Mempool::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void Mempool::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    LOG_INFO("Mempool cleared");
}
