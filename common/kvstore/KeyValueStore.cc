#include "common/kvstore/KeyValueStore.h"
#include "common/Timer.h"
#include "lmdb.h"

#include <utility>

using namespace std;
namespace sorbet {
constexpr string_view VERSION_KEY = "DB_FORMAT_VERSION"sv;
constexpr size_t MAX_DB_SIZE_BYTES =
    2L * 1024 * 1024 * 1024; // 2G. This is both maximum fs db size and max virtual memory usage.
struct KeyValueStore::DBEnv {
    MDB_env *env;
};

struct ReadOnlyKeyValueStore::Txn {
    MDB_txn *txn;
};

struct ReadOnlyKeyValueStore::Dbi {
    MDB_dbi dbi;
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
    : version(move(version)), path(move(path)), flavor(move(flavor)), dbEnv(make_unique<DBEnv>()) {
    ENFORCE(!this->version.empty());
    bool expected = false;
    if (!kvstoreInUse.compare_exchange_strong(expected, true)) {
        throw_mdb_error("Cannot create two kvstore instances simultaneously.", 0);
    }

    int rc = mdb_env_create(&dbEnv->env);
    if (rc != 0) {
        goto fail;
    }
    rc = mdb_env_set_mapsize(dbEnv->env, MAX_DB_SIZE_BYTES);
    if (rc != 0) {
        goto fail;
    }
    rc = mdb_env_set_maxdbs(dbEnv->env, 3);
    if (rc != 0) {
        goto fail;
    }
    // MDB_NOTLS specifies not to use thread local storage for transactions. Makes it possible to share read-only
    // transactions between threads.
    rc = mdb_env_open(dbEnv->env, this->path.c_str(), MDB_NOTLS, 0664);
    if (rc != 0) {
        goto fail;
    }
    return;
fail:
    throw_mdb_error("failed to create database"sv, rc);
}
KeyValueStore::~KeyValueStore() noexcept(false) {
    mdb_env_close(dbEnv->env);
    bool expected = true;
    if (!kvstoreInUse.compare_exchange_strong(expected, false)) {
        throw_mdb_error("Cannot create two kvstore instances simultaneously.", 0);
    }
}

ReadOnlyKeyValueStore::ReadOnlyKeyValueStore(unique_ptr<KeyValueStore> kvstore, bool disableInit)
    : kvstore(move(kvstore)), dbi(make_unique<Dbi>()), mainTxn(make_unique<Txn>()), wrongVersion(false), _sessionId(0) {
    // This parameter should always be true; I just needed an extra arg to disambiguate.
    ENFORCE(disableInit);
}

ReadOnlyKeyValueStore::ReadOnlyKeyValueStore(unique_ptr<KeyValueStore> kvstore)
    : kvstore(move(kvstore)), dbi(make_unique<Dbi>()), mainTxn(make_unique<Txn>()), wrongVersion(false), _sessionId(0) {
    createMainTransaction();
    wrongVersion = readString(VERSION_KEY) != this->kvstore->version;
}

ReadOnlyKeyValueStore::~ReadOnlyKeyValueStore() {
    abort();
}

void ReadOnlyKeyValueStore::createMainTransaction() {
    // This function should not be called twice.
    ENFORCE(mainTxn->txn == nullptr);
    auto &dbEnv = *kvstore->dbEnv;
    auto rc = mdb_txn_begin(dbEnv.env, nullptr, MDB_RDONLY, &mainTxn->txn);
    if (rc != 0) {
        goto fail;
    }
    rc = mdb_dbi_open(mainTxn->txn, kvstore->flavor.c_str(), 0, &dbi->dbi);
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
    if (mainTxn->txn == nullptr) {
        return;
    }
    // Abort the main transaction.
    mdb_txn_abort(mainTxn->txn);
    mainTxn->txn = nullptr;
    ENFORCE(kvstore != nullptr);
    mdb_close(kvstore->dbEnv->env, dbi->dbi);
}

ReadOnlyKeyValueStore::Txn &ReadOnlyKeyValueStore::getThreadTxn() const {
    return *mainTxn;
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
    int rc = mdb_get(txn, dbi->dbi, &kv, &data);
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
    : ReadOnlyKeyValueStore(move(kvstore), true), writerId(this_thread::get_id()), readTxn(make_unique<Txn>()) {
    createMainTransaction();
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
    if (mainTxn->txn == nullptr) {
        return;
    }
    // If other threads try to abort or commit a write transaction, we will end up in a deadlock the next time a write
    // transaction begins.
    if (this_thread::get_id() != writerId) {
        throw_mdb_error("KeyValueStore can only write from thread that created it"sv, 0);
    }
    mdb_txn_abort(readTxn->txn);
    readTxn->txn = nullptr;
    ReadOnlyKeyValueStore::abort();
}

int OwnedKeyValueStore::commit() {
    // Note: txn being null indicates that the transaction has already ended, perhaps due to a commit.
    // This should never happen.
    if (mainTxn->txn == nullptr) {
        ENFORCE(false);
        return 0;
    }
    // If other threads try to abort or commit a write transaction, we will end up in a deadlock the next time a write
    // transaction begins.
    if (this_thread::get_id() != writerId) {
        throw_mdb_error("KeyValueStore can only write from thread that created it"sv, 0);
    }

    // Commit the read-only transaction.
    mdb_txn_commit(readTxn->txn);
    readTxn->txn = nullptr;

    // Commit the main transaction.
    int rc = mdb_txn_commit(mainTxn->txn);
    mainTxn->txn = nullptr;
    ENFORCE(kvstore != nullptr);
    mdb_close(kvstore->dbEnv->env, dbi->dbi);
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

    int rc = mdb_put(mainTxn->txn, dbi->dbi, &kv, &dv, 0);
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

    int rc = mdb_drop(mainTxn->txn, dbi->dbi, 0);
    if (rc != 0) {
        goto fail;
    }
    rc = commit();
    if (rc != 0) {
        goto fail;
    }
    createMainTransaction();
    return;
fail:
    throw_mdb_error("failed to clear the database"sv, rc);
}

ReadOnlyKeyValueStore::Txn &OwnedKeyValueStore::getThreadTxn() const {
    if (this_thread::get_id() == writerId) {
        return *mainTxn;
    } else {
        return *readTxn;
    }
}

void OwnedKeyValueStore::createMainTransaction() {
    if (writerId != this_thread::get_id()) {
        throw_mdb_error("KeyValueStore can only write from thread that created it"sv, 0);
    }

    auto &dbEnv = *kvstore->dbEnv;
    auto rc = mdb_txn_begin(dbEnv.env, nullptr, 0, &mainTxn->txn);
    if (rc != 0) {
        goto fail;
    }
    rc = mdb_dbi_open(mainTxn->txn, kvstore->flavor.c_str(), MDB_CREATE, &dbi->dbi);
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
    // so that the reader transaction can use it, and then re-open the transaction
    // for future writes.
    rc = mdb_txn_commit(mainTxn->txn);
    if (rc != 0) {
        goto fail;
    }
    rc = mdb_txn_begin(dbEnv.env, nullptr, 0, &mainTxn->txn);
    if (rc != 0) {
        goto fail;
    }

    // Create the read-only transaction
    rc = mdb_txn_begin(kvstore->dbEnv->env, nullptr, MDB_RDONLY, &readTxn->txn);
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
