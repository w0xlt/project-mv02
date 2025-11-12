#include "block_assembly.h"
#include <stdexcept>
#include "sha256mini/sha256.h"

namespace blkasm {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

/* -------- Small helpers (binary/varint/script) - exact behavior kept -------- */
static void write_u32_le(std::vector<u8>& out, u32 v) {
    out.push_back(u8(v)); out.push_back(u8(v>>8)); out.push_back(u8(v>>16)); out.push_back(u8(v>>24));
}
static void write_u64_le(std::vector<u8>& out, u64 v) { for (int i=0;i<8;++i) out.push_back(u8(v>>(8*i))); }
static void write_varint(std::uint64_t v, std::vector<u8>& out); // forward decl (below)
static void write_varint(std::vector<u8>& out, std::uint64_t v) { write_varint(v, out); }
static void write_varint(std::uint64_t v, std::vector<u8>& out) {
    if (v < 0xFD) out.push_back(u8(v));
    else if (v <= 0xFFFF) { out.push_back(0xFD); out.push_back(u8(v)); out.push_back(u8(v>>8)); }
    else if (v <= 0xFFFFFFFFULL) { out.push_back(0xFE); write_u32_le(out, u32(v)); }
    else { out.push_back(0xFF); write_u64_le(out, v); }
}
static void append_data(std::vector<u8>& out, const std::vector<u8>& data) { out.insert(out.end(), data.begin(), data.end()); }

static std::vector<u8> script_push_data(const std::vector<u8>& data) {
    std::vector<u8> s; size_t n=data.size();
    if (n==0) { s.push_back(0x00); }
    else if (n<0x4c) { s.push_back(u8(n)); append_data(s,data); }
    else if (n<=0xff) { s.push_back(0x4c); s.push_back(u8(n)); append_data(s,data); }
    else if (n<=0xffff) { s.push_back(0x4d); s.push_back(u8(n)); s.push_back(u8(n>>8)); append_data(s,data); }
    else { s.push_back(0x4e); s.push_back(u8(n)); s.push_back(u8(n>>8)); s.push_back(u8(n>>16)); s.push_back(u8(n>>24)); append_data(s,data); }
    return s;
}
static std::vector<u8> encode_scriptnum(std::int64_t v) {
    if (v==0) return {};
    std::vector<u8> r; bool neg=v<0; std::uint64_t av=neg?std::uint64_t(-v):std::uint64_t(v);
    while (av) { r.push_back(u8(av&0xff)); av>>=8; }
    if (r.back()&0x80) r.push_back(neg?0x80:0x00);
    else if (neg) r.back()|=0x80;
    return r;
}

/* -------------------- TX model -------------------- */
struct WitnessItem { std::vector<u8> data; };
struct TxIn { std::array<u8,32> prev_hash{}; u32 prev_index{0xffffffff}; std::vector<u8> scriptSig; u32 sequence{0xffffffff}; };
struct TxOut { std::int64_t value{0}; std::vector<u8> scriptPubKey; };
struct Tx {
    std::int32_t version{2};
    std::vector<TxIn> vin;
    std::vector<TxOut> vout;
    std::vector<std::vector<WitnessItem>> witness;
    u32 lock_time{0};
    bool has_witness() const { for (const auto& w : witness) if (!w.empty()) return true; return false; }
    std::vector<u8> serialize(bool with_witness) const {
        std::vector<u8> out;
        write_u32_le(out, u32(version));
        bool ww = with_witness && has_witness(); if (ww) { out.push_back(0x00); out.push_back(0x01); }
        write_varint(out, vin.size());
        for (const auto& in : vin) {
            out.insert(out.end(), in.prev_hash.begin(), in.prev_hash.end());
            write_u32_le(out, in.prev_index);
            write_varint(out, in.scriptSig.size()); append_data(out, in.scriptSig);
            write_u32_le(out, in.sequence);
        }
        write_varint(out, vout.size());
        for (const auto& o : vout) {
            for (int i=0;i<8;++i) out.push_back(u8((std::uint64_t)o.value>>(8*i)));
            write_varint(out, o.scriptPubKey.size()); append_data(out, o.scriptPubKey);
        }
        if (ww) {
            for (const auto& w : witness) {
                write_varint(out, w.size());
                for (const auto& it : w) { write_varint(out, it.data.size()); append_data(out, it.data); }
            }
        }
        write_u32_le(out, lock_time);
        return out;
    }
};

/* -------------------- SHA256d with order calibration -------------------- */
static std::array<u8,32> reverse32(const std::array<u8,32>& a) {
    std::array<u8,32> r{}; for (int i=0;i<32;++i) r[i] = a[31-i]; return r;
}
static std::array<u8,32> sha256d_raw(const u8* p, size_t n) {
    auto h1 = sha256mini::SHA256::Hash(p, n);
    auto h2 = sha256mini::SHA256::Hash(h1.data(), h1.size());
    return h2; // library-defined byte order (we'll calibrate)
}
static std::array<u8,32> sha256d_raw(const std::vector<u8>& v) { return sha256d_raw(v.data(), v.size()); }
static bool g_hash_needs_reverse = false; // if true, reverse to get LITTLE-ENDIAN
static inline std::array<u8,32> sha256d_LE(const std::vector<u8>& v) {
    auto r = sha256d_raw(v);
    return g_hash_needs_reverse ? reverse32(r) : r; // ensure returned bytes are LITTLE-ENDIAN
}

/* -------------------- Strip witness (base serialization) -------------------- */
static std::uint32_t read_u32_le(const std::vector<u8>& v, size_t& p) {
    if (p+4>v.size()) throw std::runtime_error("read_u32_le OOB");
    std::uint32_t x = (std::uint32_t)v[p] | (std::uint32_t(v[p+1])<<8) | (std::uint32_t(v[p+2])<<16) | (std::uint32_t(v[p+3])<<24); p+=4; return x;
}
static std::uint64_t read_u64_le(const std::vector<u8>& v, size_t& p) {
    if (p+8>v.size()) throw std::runtime_error("read_u64_le OOB");
    std::uint64_t x=0; for (int i=0;i<8;++i) x|=std::uint64_t(v[p+i])<<(8*i); p+=8; return x;
}
static std::uint64_t read_varint_adv(const std::vector<u8>& v, size_t& p) {
    if (p>=v.size()) throw std::runtime_error("read_varint OOB");
    u8 ch = v[p++];
    if (ch < 0xFD) return ch;
    if (ch == 0xFD) { if (p+2>v.size()) throw std::runtime_error("OOB"); std::uint64_t x=v[p]|(std::uint64_t(v[p+1])<<8); p+=2; return x; }
    if (ch == 0xFE) { if (p+4>v.size()) throw std::runtime_error("OOB"); std::uint64_t x=v[p]|(std::uint64_t(v[p+1])<<8)|(std::uint64_t(v[p+2])<<16)|(std::uint64_t(v[p+3])<<24); p+=4; return x; }
    return read_u64_le(v,p);
}
static void append_bytes(std::vector<u8>& out, const std::vector<u8>& src, size_t a, size_t b) {
    if (b>src.size()||a>b) throw std::runtime_error("append_bytes OOB"); out.insert(out.end(), src.begin()+a, src.begin()+b);
}
static std::vector<u8> strip_witness(const std::vector<u8>& tx) {
    size_t p=0;
    if (tx.size()<10) throw std::runtime_error("tx too short");
    std::vector<u8> base;
    append_bytes(base, tx, p, p+4); p+=4; // version
    bool has_wit=false;
    if (p+2<=tx.size() && tx[p]==0x00 && tx[p+1]!=0x00) { has_wit=true; p+=2; }
    size_t vin_start=p; std::uint64_t vin_cnt=read_varint_adv(tx,p); append_bytes(base,tx,vin_start,p);
    for (std::uint64_t i=0;i<vin_cnt;++i) {
        if (p+36>tx.size()) throw std::runtime_error("vin OOB");
        append_bytes(base,tx,p,p+36); p+=36;
        size_t sls=p; std::uint64_t sl=read_varint_adv(tx,p); append_bytes(base,tx,sls,p);
        if (p+sl>tx.size()) throw std::runtime_error("script OOB");
        append_bytes(base,tx,p,p+sl); p+=sl;
        if (p+4>tx.size()) throw std::runtime_error("seq OOB");
        append_bytes(base,tx,p,p+4); p+=4;
    }
    size_t vout_start=p; std::uint64_t vout_cnt=read_varint_adv(tx,p); append_bytes(base,tx,vout_start,p);
    for (std::uint64_t i=0;i<vout_cnt;++i) {
        if (p+8>tx.size()) throw std::runtime_error("value OOB");
        append_bytes(base,tx,p,p+8); p+=8;
        size_t pls=p; std::uint64_t pl=read_varint_adv(tx,p); append_bytes(base,tx,pls,p);
        if (p+pl>tx.size()) throw std::runtime_error("pk OOB");
        append_bytes(base,tx,p,p+pl); p+=pl;
    }
    if (has_wit) {
        for (std::uint64_t i=0;i<vin_cnt;++i) {
            std::uint64_t n = read_varint_adv(tx,p);
            for (std::uint64_t j=0;j<n;++j) {
                std::uint64_t sz = read_varint_adv(tx,p);
                if (p+sz>tx.size()) throw std::runtime_error("wit OOB");
                p += sz;
            }
        }
    }
    if (p+4>tx.size()) throw std::runtime_error("lock OOB");
    append_bytes(base,tx,p,p+4); p+=4;
    if (p!=tx.size()) throw std::runtime_error("trailing bytes in tx");
    return base;
}

/* -------------------- Merkle (LITTLE-ENDIAN leaves & nodes) -------------------- */
std::array<u8,32> merkle_root(std::vector<std::array<u8,32>> hashes_le) {
    if (hashes_le.empty()) { std::array<u8,32> z{}; z.fill(0); return z; }
    while (hashes_le.size()>1) {
        std::vector<std::array<u8,32>> next; next.reserve((hashes_le.size()+1)/2);
        for (size_t i=0;i<hashes_le.size(); i+=2) {
            const auto& L = hashes_le[i];
            const auto& R = (i+1<hashes_le.size()) ? hashes_le[i+1] : hashes_le[i];
            std::vector<u8> cat; cat.reserve(64);
            cat.insert(cat.end(), L.begin(), L.end()); // concat LITTLE-ENDIAN bytes
            cat.insert(cat.end(), R.begin(), R.end());
            auto d_le = sha256d_LE(cat);              // normalize to LITTLE-ENDIAN
            next.push_back(d_le);
        }
        hashes_le.swap(next);
    }
    return hashes_le[0];
}

/* -------------------- Block model -------------------- */
struct Block {
    std::int32_t version{0};
    std::array<u8,32> prev{}, merkle{};
    u32 time{0}, bits{0}, nonce{0};
    std::vector<std::vector<u8>> txs;
    std::vector<u8> serialize() const {
        std::vector<u8> out;
        write_u32_le(out, u32(version));
        out.insert(out.end(), prev.begin(), prev.end());
        out.insert(out.end(), merkle.begin(), merkle.end());
        write_u32_le(out, time); write_u32_le(out, bits); write_u32_le(out, nonce);
        write_varint(out, txs.size()); for (const auto& t : txs) append_data(out, t);
        return out;
    }
};

/* -------------------- Coinbase & commitment -------------------- */
static Tx build_coinbase(std::int64_t coinbase_value,
                         std::int32_t height,
                         const std::vector<u8>& coinbase_flags,
                         const std::vector<u8>& payout_script,
                         bool want_commitment)
{
    Tx cb; cb.version = 1; cb.vin.resize(1);
    cb.vin[0].prev_hash.fill(0); cb.vin[0].prev_index = 0xffffffffu; cb.vin[0].sequence = 0xffffffffu;

    std::vector<u8> ss;
    auto hpush = script_push_data(encode_scriptnum(height)); ss.insert(ss.end(), hpush.begin(), hpush.end());
    if (!coinbase_flags.empty()) {
        auto fpush = script_push_data(coinbase_flags); ss.insert(ss.end(), fpush.begin(), fpush.end());
    }
    std::vector<u8> extranonce(8,0); auto epush = script_push_data(extranonce); ss.insert(ss.end(), epush.begin(), epush.end());
    cb.vin[0].scriptSig = std::move(ss);

    cb.vout.push_back(TxOut{coinbase_value, payout_script});
    if (want_commitment) {
        cb.vout.push_back(TxOut{0, {}});              // placeholder
        cb.witness.resize(1); cb.witness[0].push_back(WitnessItem{std::vector<u8>(32,0)}); // reserved
    }
    return cb;
}
static std::vector<u8> build_commitment_script(const std::array<u8,32>& wmr_le, const std::vector<u8>& nonce32) {
    std::vector<u8> buf; buf.reserve(64);
    buf.insert(buf.end(), wmr_le.begin(), wmr_le.end());
    buf.insert(buf.end(), nonce32.begin(), nonce32.end());
    // normalize to 32 bytes LE, then place raw bytes into the script
    auto h1 = sha256d_LE(buf);
    std::vector<u8> scr;
    scr.push_back(0x6a); scr.push_back(0x24); // OP_RETURN + push 36
    scr.insert(scr.end(), {0xaa,0x21,0xa9,0xed});
    scr.insert(scr.end(), h1.begin(), h1.end()); // raw 32 bytes
    return scr;
}

BlockAssemblyResult assemble_block_proposal(const GbtBlockTemplateData& in,
                                            const std::vector<u8>& payout_script)
{
    // --- Endianness calibration (CRITICAL) ---
    if (!in.non_cb_tx_bytes.empty() && in.first_non_cb_expected_txid_le.has_value()) {
        auto base0 = strip_witness(in.non_cb_tx_bytes[0]);
        auto raw0  = sha256d_raw(base0);                 // library's native order
        auto want_le = in.first_non_cb_expected_txid_le.value();
        g_hash_needs_reverse = (raw0 != want_le && reverse32(raw0) == want_le);
    } else {
        g_hash_needs_reverse = false;
    }

    // Coinbase
    Tx coinbase = build_coinbase(in.coinbase_value, in.height, in.coinbase_flags, payout_script, in.segwit_active);

    // Witness commitment
    if (in.segwit_active) {
        std::array<u8,32> z{}; z.fill(0);
        std::vector<std::array<u8,32>> wtxids_le; wtxids_le.reserve(1+in.non_cb_tx_bytes.size());
        wtxids_le.push_back(z); // coinbase wtxid = 0^32
        for (const auto& txb : in.non_cb_tx_bytes) wtxids_le.push_back(sha256d_LE(txb));
        auto wmr_le = merkle_root(wtxids_le);
        std::vector<u8> nonce32(32,0);
        auto commit_script = build_commitment_script(wmr_le, nonce32);
        if (coinbase.vout.size()>=2) coinbase.vout[1] = TxOut{0, commit_script};
        else coinbase.vout.push_back(TxOut{0, commit_script});
        coinbase.witness = { { WitnessItem{nonce32} } };
    }

    // Serialize & IDs
    auto cb_ser_with   = coinbase.serialize(true);
    auto cb_ser_no_wit = coinbase.serialize(false);

    BlockAssemblyResult out;
    out.coinbase_txid_le = sha256d_LE(cb_ser_no_wit); // LITTLE-ENDIAN bytes

    std::vector<std::vector<u8>> txs_bytes; txs_bytes.reserve(1+in.non_cb_tx_bytes.size());
    txs_bytes.push_back(cb_ser_with); for (const auto& b : in.non_cb_tx_bytes) txs_bytes.push_back(b);

    std::vector<std::array<u8,32>> txids_from_bytes_le; txids_from_bytes_le.reserve(txs_bytes.size());
    txids_from_bytes_le.push_back(out.coinbase_txid_le);
    for (const auto& txb : in.non_cb_tx_bytes) {
        auto base = strip_witness(txb);
        txids_from_bytes_le.push_back(sha256d_LE(base));  // LITTLE-ENDIAN
    }
    out.txids_from_bytes_le = txids_from_bytes_le;

    // Merkle root from bytes (authoritative)
    out.merkle_root_le = merkle_root(out.txids_from_bytes_le);

    // Header + full block
    struct Block blk;
    blk.version = in.version;
    blk.prev    = in.prev_hash_le;
    blk.merkle  = out.merkle_root_le;
    blk.time    = in.curtime;
    blk.bits    = in.bits_u32;
    blk.nonce   = 0;
    blk.txs     = txs_bytes;
    out.block_bytes = blk.serialize();

    return out;
}

} // namespace blkasm