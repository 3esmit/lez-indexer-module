// Deterministic Indexer FFI replacement for lifecycle tests.

#include <indexer_ffi.h>

#include <cstdlib>

namespace {
    OperationStatus start_status = Ok;
    OperationStatus stop_status = Ok;
    int start_calls = 0;
    int stop_calls = 0;
} // namespace

void reset_indexer_ffi_mock() {
    start_status = Ok;
    stop_status = Ok;
    start_calls = 0;
    stop_calls = 0;
}

void set_indexer_ffi_start_status(const OperationStatus status) {
    start_status = status;
}

int indexer_ffi_start_calls() {
    return start_calls;
}

int indexer_ffi_stop_calls() {
    return stop_calls;
}

extern "C" {

InitializedIndexerServiceFFIResult start_indexer(const Runtime*, const char*, const char*) {
    ++start_calls;
    if (start_status != Ok) {
        return {nullptr, start_status};
    }
    return {new IndexerServiceFFI{}, Ok};
}

OperationStatus stop_indexer(IndexerServiceFFI* indexer) {
    ++stop_calls;
    delete indexer;
    return stop_status;
}

void init_logger(const char*) {}

void free_cstring(char* value) {
    std::free(value);
}

LastBlockIdResult query_last_block(const IndexerServiceFFI*) {
    return {0, false, InitializationError};
}

char* query_status(const IndexerServiceFFI*) {
    return nullptr;
}

PointerResult_FfiBlockOpt__OperationStatus query_block(const IndexerServiceFFI*, FfiBlockId) {
    return {nullptr, InitializationError};
}

PointerResult_FfiBlockOpt__OperationStatus query_block_by_hash(const IndexerServiceFFI*, FfiHashType) {
    return {nullptr, InitializationError};
}

PointerResult_FfiAccount__OperationStatus query_account(const IndexerServiceFFI*, FfiAccountId) {
    return {nullptr, InitializationError};
}

PointerResult_FfiOption_FfiTransaction_____OperationStatus query_transaction(const IndexerServiceFFI*, FfiHashType) {
    return {nullptr, InitializationError};
}

PointerResult_FfiVec_FfiBlock_____OperationStatus query_block_vec(const IndexerServiceFFI*, FfiOption_u64, uint64_t) {
    return {nullptr, InitializationError};
}

PointerResult_FfiVec_FfiTransaction_____OperationStatus query_transactions_by_account(
    const IndexerServiceFFI*,
    FfiAccountId,
    uint64_t,
    uint64_t
) {
    return {nullptr, InitializationError};
}

void free_ffi_account(FfiAccount*) {}
void free_ffi_block(FfiBlock) {}
void free_ffi_block_opt(FfiBlockOpt*) {}
void free_ffi_block_vec(FfiVec_FfiBlock*) {}
void free_ffi_transaction(FfiTransaction) {}
void free_ffi_transaction_opt(FfiOption_FfiTransaction*) {}
void free_ffi_transaction_vec(FfiVec_FfiTransaction*) {}

bool is_ok(const OperationStatus* status) {
    return status && *status == Ok;
}

bool is_error(const OperationStatus* status) {
    return !is_ok(status);
}
}
