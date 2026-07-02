#include "lez_ffi_marshalling.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace marshalling {

    namespace {
        // Single hex nibble -> 0..15, or -1 if not a hex digit.
        int hexNibble(char c) {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

        // Plain Bitcoin Base58 alphabet (no checksum, no version byte). Must stay
        // byte-for-byte identical to the `base58` crate (lee::AccountId) and the
        // wallet UI's Base58.js so account-id strings round-trip across the stack.
        constexpr char kBase58Alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

        // Position of `c` in the Base58 alphabet, or -1 if not a Base58 char.
        int base58Index(char c) {
            for (int i = 0; i < 58; ++i) {
                if (kBase58Alphabet[i] == c)
                    return i;
            }
            return -1;
        }

        // Decode a Base58 string (whitespace-trimmed) into raw bytes. Returns
        // false on any character outside the alphabet. Big-integer base 58 -> 256,
        // leading '1's map to leading zero bytes. Mirrors Base58.js::decode.
        bool base58Decode(const std::string& s, std::vector<uint8_t>* out) {
            size_t begin = 0;
            size_t end = s.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(s[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
                --end;
            if (begin == end) {
                return false;
            }

            size_t leadingZeros = 0;
            for (size_t i = begin; i < end && s[i] == '1'; ++i) {
                ++leadingZeros;
            }

            std::vector<uint8_t> digits; // base-256, least-significant first
            for (size_t i = begin; i < end; ++i) {
                const int idx = base58Index(s[i]);
                if (idx < 0) {
                    return false;
                }
                int carry = idx;
                for (size_t j = 0; j < digits.size(); ++j) {
                    carry += digits[j] * 58;
                    digits[j] = static_cast<uint8_t>(carry & 0xFF);
                    carry >>= 8;
                }
                while (carry > 0) {
                    digits.push_back(static_cast<uint8_t>(carry & 0xFF));
                    carry >>= 8;
                }
            }

            out->assign(leadingZeros, 0);
            for (size_t i = digits.size(); i-- > 0;) {
                out->push_back(digits[i]);
            }
            return true;
        }
    } // namespace

    // Lower-case hex of `length` raw bytes (used for 32-byte hashes/ids/keys and
    // 64-byte signatures).
    std::string bytesToHex(const uint8_t* data, const size_t length) {
        static const char* digits = "0123456789abcdef";
        std::string out;
        out.resize(length * 2);
        for (size_t i = 0; i < length; ++i) {
            out[2 * i] = digits[(data[i] >> 4) & 0xF];
            out[2 * i + 1] = digits[data[i] & 0xF];
        }
        return out;
    }

    // FfiU128 is a 16-byte little-endian integer (balances, nonces). C++ has no
    // native u128, so build the decimal string via __uint128_t (GCC/Clang, 64-bit).
    std::string u128LeToDecimal(const uint8_t data[16]) {
#if defined(__SIZEOF_INT128__) && __SIZEOF_INT128__ >= 16
        __uint128_t v = 0;
        for (int i = 0; i < 16; ++i) {
            v |= static_cast<__uint128_t>(data[i]) << (i * 8);
        }
        if (v == 0) {
            return "0";
        }
        char buf[40];
        int n = 0;
        while (v) {
            buf[n++] = static_cast<char>('0' + static_cast<int>(v % 10));
            v /= 10;
        }
        std::reverse(buf, buf + n);
        return std::string(buf, n);
#else
#error "u128LeToDecimal requires __uint128_t; build with GCC or Clang on 64-bit"
#endif
    }

    // 64-bit values are emitted as decimal STRINGS, not JSON numbers: JSON
    // numbers are doubles in many parsers and silently lose precision above 2^53.
    std::string u64ToString(uint64_t v) {
        return std::to_string(v);
    }

    // Parse a hex string (optionally 0x-prefixed) into a fixed 32-byte FfiBytes32.
    // Returns false unless it decodes to exactly 32 bytes.
    bool hexToBytes32(const std::string& hex, FfiBytes32* out) {
        size_t begin = 0;
        size_t end = hex.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(hex[begin])))
            ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(hex[end - 1])))
            --end;

        if (end - begin >= 2 && hex[begin] == '0' && (hex[begin + 1] == 'x' || hex[begin + 1] == 'X')) {
            begin += 2;
        }

        if (end - begin != 64) {
            return false;
        }

        for (size_t i = 0; i < 32; ++i) {
            const int hi = hexNibble(hex[begin + 2 * i]);
            const int lo = hexNibble(hex[begin + 2 * i + 1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            out->data[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        return true;
    }

    // True iff `s` is exactly `num_chars` hex digits (no prefix/whitespace).
    bool isHex(const std::string& s, const size_t num_chars) {
        if (s.size() != num_chars) {
            return false;
        }
        for (const char c : s) {
            if (hexNibble(c) < 0) {
                return false;
            }
        }
        return true;
    }

    // Base58-encode `length` raw bytes (plain Bitcoin alphabet). Big-integer
    // base 256 -> 58, leading zero bytes map to leading '1's. Mirrors
    // Base58.js::encode so account ids match the wallet UI and canonical LEZ.
    std::string bytes32ToBase58(const uint8_t* data, const size_t length) {
        size_t leadingZeros = 0;
        while (leadingZeros < length && data[leadingZeros] == 0) {
            ++leadingZeros;
        }

        std::vector<uint8_t> digits; // base-58, least-significant first
        for (size_t i = 0; i < length; ++i) {
            int carry = data[i];
            for (size_t j = 0; j < digits.size(); ++j) {
                carry += digits[j] * 256;
                digits[j] = static_cast<uint8_t>(carry % 58);
                carry /= 58;
            }
            while (carry > 0) {
                digits.push_back(static_cast<uint8_t>(carry % 58));
                carry /= 58;
            }
        }

        std::string out(leadingZeros, '1');
        for (size_t i = digits.size(); i-- > 0;) {
            out += kBase58Alphabet[digits[i]];
        }
        return out;
    }

    // Accept an account id as Base58 (canonical) or 64-char hex. Base58 first:
    // a real 32-byte account is ~44 Base58 chars, so a 64-hex string either hits a
    // non-Base58 char (e.g. '0') or decodes to the wrong length and falls through.
    bool accountStrToBytes32(const std::string& account_id, FfiBytes32* out) {
        std::vector<uint8_t> bytes;
        if (base58Decode(account_id, &bytes) && bytes.size() == 32) {
            std::memcpy(out->data, bytes.data(), 32);
            return true;
        }
        return hexToBytes32(account_id, out);
    }

    nlohmann::json ffiAccountToJson(const FfiAccount& account) {
        nlohmann::json obj;
        obj["program_owner"] = bytesToHex(reinterpret_cast<const uint8_t*>(account.program_owner.data), 32);
        obj["balance"] = u128LeToDecimal(account.balance.data);
        obj["nonce"] = u128LeToDecimal(account.nonce.data);
        obj["data_size"] = static_cast<int>(account.data_len);
        return obj;
    }

    nlohmann::json ffiTransactionToJson(const FfiTransaction& tx) {
        nlohmann::json obj;

        switch (tx.kind) {
        case Public: {
            const FfiPublicTransactionBody* body = tx.body.public_body;
            if (!body) {
                break;
            }
            obj["type"] = "Public";
            obj["hash"] = bytesToHex(body->hash.data, 32);
            obj["program_id"] = bytesToHex(reinterpret_cast<const uint8_t*>(body->message.program_id.data), 32);

            nlohmann::json accounts = nlohmann::json::array();
            const FfiAccountIdList& ids = body->message.account_ids;
            const FfiNonceList& nonces = body->message.nonces;
            for (uintptr_t i = 0; i < ids.len; ++i) {
                nlohmann::json ref;
                ref["account_id"] = bytes32ToBase58(ids.entries[i].data, 32);
                ref["nonce"] = i < nonces.len ? u128LeToDecimal(nonces.entries[i].data) : std::string("0");
                accounts.push_back(ref);
            }
            obj["accounts"] = accounts;

            nlohmann::json instructionData = nlohmann::json::array();
            const FfiInstructionDataList& instr = body->message.instruction_data;
            for (uintptr_t i = 0; i < instr.len; ++i) {
                instructionData.push_back(static_cast<std::int64_t>(instr.entries[i]));
            }
            obj["instruction_data"] = instructionData;
            obj["signature_count"] = static_cast<int>(body->witness_set.len);
            break;
        }
        case Private: {
            const FfiPrivateTransactionBody* body = tx.body.private_body;
            if (!body) {
                break;
            }
            obj["type"] = "PrivacyPreserving";
            obj["hash"] = bytesToHex(body->hash.data, 32);

            nlohmann::json accounts = nlohmann::json::array();
            const FfiAccountIdList& ids = body->message.public_account_ids;
            const FfiNonceList& nonces = body->message.nonces;
            for (uintptr_t i = 0; i < ids.len; ++i) {
                nlohmann::json ref;
                ref["account_id"] = bytes32ToBase58(ids.entries[i].data, 32);
                ref["nonce"] = i < nonces.len ? u128LeToDecimal(nonces.entries[i].data) : std::string("0");
                accounts.push_back(ref);
            }
            obj["accounts"] = accounts;

            obj["new_commitments_count"] = static_cast<int>(body->message.new_commitments.len);
            obj["nullifiers_count"] = static_cast<int>(body->message.new_nullifiers.len);
            obj["encrypted_states_count"] = static_cast<int>(body->message.encrypted_private_post_states.len);
            obj["validity_window_start"] = u64ToString(body->message.block_validity_window[0]);
            obj["validity_window_end"] = u64ToString(body->message.block_validity_window[1]);
            obj["signature_count"] = static_cast<int>(body->witness_set.len);
            obj["proof_size"] = static_cast<int>(body->proof.len);
            break;
        }
        case ProgramDeploy: {
            const FfiProgramDeploymentTransactionBody* body = tx.body.program_deployment_body;
            if (!body) {
                break;
            }
            obj["type"] = "ProgramDeployment";
            obj["hash"] = bytesToHex(body->hash.data, 32);
            obj["bytecode_size"] = static_cast<int>(body->message.len);
            break;
        }
        }

        return obj;
    }

    namespace {
        std::string bedrockStatusToString(FfiBedrockStatus status) {
            switch (status) {
            case Safe:
                return "Safe";
            case Finalized:
                return "Finalized";
            case Pending:
            default:
                return "Pending";
            }
        }
    } // namespace

    nlohmann::json ffiBlockToJson(const FfiBlock& block) {
        nlohmann::json obj;
        obj["block_id"] = u64ToString(block.header.block_id);
        obj["hash"] = bytesToHex(block.header.hash.data, 32);
        obj["prev_block_hash"] = bytesToHex(block.header.prev_block_hash.data, 32);
        obj["timestamp"] = u64ToString(block.header.timestamp);
        obj["signature"] = bytesToHex(block.header.signature.data, 64);
        obj["bedrock_status"] = bedrockStatusToString(block.bedrock_status);

        nlohmann::json transactions = nlohmann::json::array();
        for (uintptr_t i = 0; i < block.body.len; ++i) {
            transactions.push_back(ffiTransactionToJson(block.body.entries[i]));
        }
        obj["transactions"] = transactions;
        return obj;
    }

    std::string jsonToCompactString(const nlohmann::json& j) {
        return j.dump();
    }

} // namespace marshalling
