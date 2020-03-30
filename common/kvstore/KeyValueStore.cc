#include "common/kvstore/KeyValueStore.h"
#include "common/Timer.h"
#include "lmdb.h"

#include <utility>

using namespace std;
namespace sorbet {
constexpr string_view VERSION_KEY = "DB_FORMAT_VERSION"sv;
constexpr size_t MAX_DB_SIZE_BYTES =
    2L * 1024 * 1024 * 1024; // 2G. This is both maximum fs db size and max virtual memory usage.
struct KeyValueStore::DBState {
    MDB_env *env;
};

struct ReadOnlyKeyValueStore::Txn {
    MDB_txn *txn;
};

struct ReadOnlyKeyValueStore::TxnState {
    MDB_dbi dbi;
    // The main transaction.
    MDB_txn *txn;
};

struct OwnedKeyValueStore::ReadTxnState {
    MDB_txn *readTxn;
};

namespace {
static void print_mdb_error(string_view what, int err) {
    fmt::print(stderr, "mdb error: {}: {}\n", what, mdb_strerror(err));
}

static void throw_mdb_error(string_view what, int err) {
    print_mdb_error(what, err);
    throw invalid_argument(string(what));
}

static atomic<u4> globalSessionId = 0;
// Only one kvstore can be created per process -- the MDB env is shared. Used to enforce that we never create
// multiple simultaneous kvstores.
static atomic<bool> kvstoreInUse = false;
} // namespace

KeyValueStore::KeyValueStore(string version, string path, string flavor)
    : version(move(version)), path(move(path)), flavor(move(flavor)), dbState(make_unique<DBState>()) {
    ENFORCE(!this->version.empty());
    bool expected = false;
    if (!kvstoreInUse.compare_exchange_strong(expected, true)) {
        throw_mdb_error("Cannot create two kvstore instances simultaneously.", 0);
    }

    int rc = mdb_env_create(&dbState->env);
    if (rc != 0) {
        goto fail;
    }
    rc = mdb_env_set_mapsize(dbState->env, MAX_DB_SIZE_BYTES);
    if (rc != 0) {
        goto fail;
    }
    rc = mdb_env_set_maxdbs(dbState->env, 3);
    if (rc != 0) {
        goto fail;
    }
    // MDB_NOTLS specifies not to use thread local storage for transactions. Makes it possible to share read-only
    // transactions between threads.
    rc = mdb_env_open(dbState->env, this->path.c_str(), MDB_NOTLS, 0664);
    if (rc != 0) {
        goto fail;
    }
    return;
fail:
    throw_mdb_error("failed to create database"sv, rc);
}
KeyValueStore::~KeyValueStore() noexcept(false) {
    mdb_env_close(dbState->env);
    bool expected = true;
    if (!kvstoreInUse.compare_exchange_strong(expected, false)) {
        throw_mdb_error("Cannot create two kvstore instances simultaneously.", 0);
    }
}

ReadOnlyKeyValueStore::ReadOnlyKeyValueStore(unique_ptr<KeyValueStore> kvstore)
    : kvstore(move(kvstore)), txnState(make_unique<TxnState>()), wrongVersion(false), _sessionId(0) {
    createMainTransaction();
    wrongVersion = readString(VERSION_KEY) != this->kvstore->version;
}

// Constructor used by OwnedKeyValueStore. Defers initialization to txnState.
ReadOnlyKeyValueStore::ReadOnlyKeyValueStore(unique_ptr<KeyValueStore> kvstore, unique_ptr<TxnState> txnState)
    : kvstore(move(kvstore)), txnState(move(txnState)), wrongVersion(false) {}

ReadOnlyKeyValueStore::~ReadOnlyKeyValueStore() {
    abort();
}

void ReadOnlyKeyValueStore::createMainTransaction() {
    // This function should not be called twice.
    ENFORCE(txnState->txn == nullptr);
    auto &dbState = *kvstore->dbState;
    auto rc = mdb_txn_begin(dbState.env, nullptr, MDB_RDONLY, &txnState->txn);
    if (rc != 0) {
        goto fail;
    }
    rc = mdb_dbi_open(txnState->txn, kvstore->flavor.c_str(), 0, &txnState->dbi);
    if (rc != 0) {
        // DB doesn't exist. Act as if it is the wrong version.
        if (rc == MDB_NOTFOUND) {
            wrongVersion = true;
            return;
        }
        goto fail;
    }
    // Increment session. Used for debug assertions.
    _sessionId = globalSessionId++;
    return;
fail:
    throw_mdb_error("failed to create transaction"sv, rc);
}

u4 ReadOnlyKeyValueStore::sessionId() const {
    return _sessionId;
}

void ReadOnlyKeyValueStore::abort() {
    // Note: txn being null indicates that the transaction has already ended, perhaps due to a commit.
    if (txnState->txn == nullptr) {
        return;
    }
    // Abort the main transaction.
    mdb_txn_abort(txnState->txn);
    txnState->txn = nullptr;
    ENFORCE(kvstore != nullptr);
    mdb_close(kvstore->dbState->env, txnState->dbi);
}

ReadOnlyKeyValueStore::Txn ReadOnlyKeyValueStore::getThreadTxn() const {
    return {txnState->txn};
}

u1 *ReadOnlyKeyValueStore::read(string_view key) const {
    if (wrongVersion) {
        return nullptr;
    }
    MDB_txn *txn = getThreadTxn().txn;
    MDB_val kv;
    kv.mv_size = key.size();
    kv.mv_data = (void *)key.data();
    MDB_val data;
    int rc = mdb_get(txn, txnState->dbi, &kv, &data);
    if (rc != 0) {
        if (rc == MDB_NOTFOUND) {
            return nullptr;
        }
        throw_mdb_error("failed read from the database"sv, rc);
    }
    return (u1 *)data.mv_data;
}

string_view ReadOnlyKeyValueStore::readString(string_view key) const {
    auto rawData = read(key);
    if (!rawData) {
        return string_view();
    }
    size_t sz;
    memcpy(&sz, rawData, sizeof(sz));
    string_view result(((const char *)rawData) + sizeof(sz), sz);
    return result;
}

unique_ptr<KeyValueStore> ReadOnlyKeyValueStore::close(unique_ptr<ReadOnlyKeyValueStore> roKvstore) {
    if (roKvstore == nullptr) {
        return nullptr;
    }
    roKvstore->abort();
    return move(roKvstore->kvstore);
}

OwnedKeyValueStore::OwnedKeyValueStore(unique_ptr<KeyValueStore> kvstore)
    : ReadOnlyKeyValueStore(move(kvstore), make_unique<TxnState>()), writerId(this_thread::get_id()),
      readTxnState(make_unique<ReadTxnState>()) {
    refreshMainTransaction();
    {
        auto dbVersion = readString(VERSION_KEY);
        if (dbVersion != this->kvstore->version) {
            clear();
            writeString(VERSION_KEY, this->kvstore->version);
        }
    }
}

OwnedKeyValueStore::~OwnedKeyValueStore() {
    abort();
}

void OwnedKeyValueStore::abort() {
    // Note: txn being null indicates that the transaction has already ended, perhaps due to a commit.
    if (txnState->txn == nullptr) {
        return;
    }
    // If other threads try to abort or commit a write transaction, we will end up in a deadlock the next time a write
    // transaction begins.
    if (this_thread::get_id() != writerId) {
        throw_mdb_error("KeyValueStore can only write from thread that created it"sv, 0);
    }
    mdb_txn_abort(readTxnState->readTxn);
    readTxnState->readTxn = nullptr;
    ReadOnlyKeyValueStore::abort();
}

int OwnedKeyValueStore::commit() {
    // Note: txn being null indicates that the transaction has already ended, perhaps due to a commit.
    // This should never happen.
    if (txnState->txn == nullptr) {
        ENFORCE(false);
        return 0;
    }
    // If other threads try to abort or commit a write transaction, we will end up in a deadlock the next time a write
    // transaction begins.
    if (this_thread::get_id() != writerId) {
        throw_mdb_error("KeyValueStore can only write from thread that created it"sv, 0);
    }

    // Commit the read-only transaction.
    mdb_txn_commit(readTxnState->readTxn);
    readTxnState->readTxn = nullptr;

    // Commit the main transaction.
    int rc = mdb_txn_commit(txnState->txn);
    txnState->txn = nullptr;
    ENFORCE(kvstore != nullptr);
    mdb_close(kvstore->dbState->env, txnState->dbi);
    return rc;
}

void OwnedKeyValueStore::write(string_view key, const vector<u1> &value) {
    if (writerId != this_thread::get_id()) {
        throw_mdb_error("KeyValueStore can only write from thread that created it"sv, 0);
    }

    MDB_val kv;
    MDB_val dv;
    kv.mv_size = key.size();
    kv.mv_data = (void *)key.data();
    dv.mv_size = value.size();
    dv.mv_data = (void *)value.data();

    int rc = mdb_put(txnState->txn, txnState->dbi, &kv, &dv, 0);
    if (rc != 0) {
        throw_mdb_error("failed write into database"sv, rc);
    }
}

void OwnedKeyValueStore::writeString(string_view key, string_view value) {
    vector<u1> rawData(value.size() + sizeof(size_t));
    size_t sz = value.size();
    memcpy(rawData.data(), &sz, sizeof(sz));
    memcpy(rawData.data() + sizeof(sz), value.data(), sz);
    write(key, move(rawData));
}

void OwnedKeyValueStore::clear() {
    if (writerId != this_thread::get_id()) {
        throw_mdb_error("KeyValueStore can only write from thread that created it"sv, 0);
    }

    int rc = mdb_drop(txnState->txn, txnState->dbi, 0);
    if (rc != 0) {
        goto fail;
    }
    rc = commit();
    if (rc != 0) {
        goto fail;
    }
    refreshMainTransaction();
    return;
fail:
    throw_mdb_error("failed to clear the database"sv, rc);
}

ReadOnlyKeyValueStore::Txn OwnedKeyValueStore::getThreadTxn() const {
    if (this_thread::get_id() == writerId) {
        return {txnState->txn};
    } else {
        return {readTxnState->readTxn};
    }
}

void OwnedKeyValueStore::refreshMainTransaction() {
    if (writerId != this_thread::get_id()) {
        throw_mdb_error("KeyValueStore can only write from thread that created it"sv, 0);
    }

    auto &dbState = *kvstore->dbState;
    auto rc = mdb_txn_begin(dbState.env, nullptr, 0, &txnState->txn);
    if (rc != 0) {
        goto fail;
    }
    rc = mdb_dbi_open(txnState->txn, kvstore->flavor.c_str(), MDB_CREATE, &txnState->dbi);
    if (rc != 0) {
        goto fail;
    }
    // Increment session. Used for debug assertions.
    _sessionId = globalSessionId++;

    // Per the docs for mdb_dbi_open:
    //
    // The database handle will be private to the current transaction
    // until the transaction is successfully committed. If the
    // transaction is aborted the handle will be closed
    // automatically. After a successful commit the handle will reside
    // in the shared environment, and may be used by other
    // transactions.
    //
    // So we commit immediately to force the dbi into the shared space
    // so that readers can use it, and then re-open the transaction
    // for future writes.
    rc = mdb_txn_commit(txnState->txn);
    if (rc != 0) {
        goto fail;
    }
    rc = mdb_txn_begin(dbState.env, nullptr, 0, &txnState->txn);
    if (rc != 0) {
        goto fail;
    }

    // Create the read-only transaction
    rc = mdb_txn_begin(kvstore->dbState->env, nullptr, MDB_RDONLY, &readTxnState->readTxn);
    if (rc != 0) {
        goto fail;
    }

    return;
fail:
    throw_mdb_error("failed to create transaction"sv, rc);
}

unique_ptr<KeyValueStore> OwnedKeyValueStore::abort(unique_ptr<OwnedKeyValueStore> ownedKvstore) {
    // `Close` defers to the virtual abort method, which does the right thing.
    return ReadOnlyKeyValueStore::close(move(ownedKvstore));
}

unique_ptr<KeyValueStore> OwnedKeyValueStore::bestEffortCommit(spdlog::logger &logger,
                                                               unique_ptr<OwnedKeyValueStore> ownedKvstore) {
    if (ownedKvstore == nullptr) {
        return nullptr;
    }
    Timer timeit(logger, "kvstore.bestEffortCommit");

    int rc = ownedKvstore->commit();
    if (rc != 0) {
        print_mdb_error("failed to commit transaction", rc);
    }
    return move(ownedKvstore->kvstore);
}

} // namespace sorbet
