#pragma once

#include <string>
#include <set>
#include <vector>
#include <mutex>
#include <cstddef>

// Mempool entry structure - optimized for fee rate ordering
struct MempoolEntry {
    std::string txid;
    std::string tx_hex;
    int64_t fee_sats;
    size_t tx_size;  // in bytes
    double fee_rate; // sats per byte
    
    MempoolEntry(std::string id, std::string hex, int64_t fee, size_t size);
    
    // Comparator for ordering by fee rate (descending - highest fee first)
    bool operator<(const MempoolEntry& other) const;
};

// Thread-safe mempool with automatic fee rate ordering
class Mempool {
public:
    // Add transaction to mempool
    void add(MempoolEntry entry);
    
    // Get all transactions (ordered by fee rate)
    std::vector<MempoolEntry> get_all() const;
    
    // Remove transaction by txid
    bool remove(const std::string& txid);
    
    // Get mempool size
    size_t size() const;
    
    // Clear all transactions
    void clear();

private:
    std::multiset<MempoolEntry> entries_;  // Automatically sorted by fee rate
    mutable std::mutex mutex_;
};

// Global mempool instance
extern Mempool g_mempool;
