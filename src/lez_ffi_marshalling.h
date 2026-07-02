#pragma once

#include <indexer_ffi.h>

#include <nlohmann/json.hpp>
#include <string>

// FFI marshalling helpers
//
// Bridge the raw C structs returned by the indexer FFI (indexer_ffi.h) to the
// compact JSON the LEZ explorer's Block/Transaction/Account models consume, and
// parse the std::string inputs back into FFI types. Kept in their own
// translation unit so lez_indexer_module_impl.cpp stays focused on the query
// flow and the JSON data model lives in one place.
//
// This header pulls in the FFI types + nlohmann::json, so it is included only
// from .cpp files — NOT from lez_indexer_module_impl.h, whose Qt-free,
// FFI-free shape the universal codegen parses.
//
// These never take ownership of FFI memory: the caller frees it with the
// matching free_ffi_* function AFTER marshalling.
//
// Large 64-bit values (ids, timestamps, validity windows) and 128-bit values
// (balances, nonces) are emitted as decimal STRINGS to avoid the double-
// precision loss of JSON numbers; counts/sizes are emitted as numbers.
namespace marshalling {

    // Lower-case hex of `length` raw bytes (32-byte hashes/ids/keys, 64-byte
    // signatures).
    std::string bytesToHex(const uint8_t* data, size_t length);

    // FfiU128 is a 16-byte little-endian integer (balances, nonces); rendered
    // as a decimal string.
    std::string u128LeToDecimal(const uint8_t data[16]);

    // 64-bit value as a decimal string.
    std::string u64ToString(uint64_t v);

    // Parse a hex string (optionally 0x-prefixed) into a fixed 32-byte buffer.
    // Returns false unless it decodes to exactly 32 bytes.
    bool hexToBytes32(const std::string& hex, FfiBytes32* out);

    // True iff `s` is exactly `num_chars` hex digits (no 0x prefix, no whitespace).
    // For validating fixed-width hex identifiers (e.g. a 64-char channel id) before
    // trusting them as-is, such as when composing a filesystem path.
    bool isHex(const std::string& s, size_t num_chars);

    // Base58 (plain Bitcoin alphabet, no checksum) of `length` raw bytes. This is
    // the canonical string form of an LEZ *account id* — it matches lee::AccountId
    // Display, wallet_ffi's account_id_to_base58, and the wallet UI's Base58.js.
    // Hashes/tx ids/program ids stay hex; only account ids are Base58.
    std::string bytes32ToBase58(const uint8_t* data, size_t length);

    // Parse an account id string into a fixed 32-byte buffer, accepting EITHER
    // Base58 (canonical) or 64-char hex (optionally 0x-prefixed). Base58 is tried
    // first; since a 32-byte value's Base58 is ~44 chars, a 64-hex string fails the
    // 32-byte check and falls through to hex. Returns false unless it resolves to
    // exactly 32 bytes.
    bool accountStrToBytes32(const std::string& account_id, FfiBytes32* out);

    nlohmann::json ffiAccountToJson(const FfiAccount& account);
    nlohmann::json ffiTransactionToJson(const FfiTransaction& tx);
    nlohmann::json ffiBlockToJson(const FfiBlock& block);

    // Compact serialization (no spaces/newlines) of an object or array.
    std::string jsonToCompactString(const nlohmann::json& j);

} // namespace marshalling
