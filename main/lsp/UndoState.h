#ifndef RUBY_TYPER_LSP_UNDOSTATE_H
#define RUBY_TYPER_LSP_UNDOSTATE_H

#include "ast/ast.h"
#include "core/NameHash.h"
#include "core/core.h"

namespace sorbet::realmain::lsp {
class LSPConfiguration;
/**
 * Contains the LSPTypechecker state that is needed to cancel a running slow path operation and any subsequent fast
 * path operations that have preempted it.
 */
class UndoState final {
    const LSPConfiguration &config;
    // Stores the pre-slow-path global state.
    std::unique_ptr<core::GlobalState> evictedGs;
    // Stores index trees containing data stored in `gs` that have been evicted during the slow path operation.
    UnorderedMap<u4, std::vector<u1>> evictedIndexed;
    // Stores the index trees stored in `gs` that were evicted because the slow path operation replaced `gs`.
    UnorderedMap<u4, std::vector<u1>> evictedIndexedFinalGS;
    // Stores the list of files that had errors before the slow path began.
    std::vector<core::FileRef> evictedFilesThatHaveErrors;

public:
    UndoState(const LSPConfiguration &config, std::unique_ptr<core::GlobalState> evictedGs,
              UnorderedMap<u4, std::vector<u1>> evictedIndexedFinalGS,
              std::vector<core::FileRef> evictedFilesThatHaveErrors);

    /**
     * Records that the given items were evicted from LSPTypechecker following a typecheck run.
     */
    void recordEvictedState(core::FileRef fref, std::vector<u1> evictedIndexTree);

    /**
     * Undoes the slow path changes represented by this class. and clears the client's error list for any files that
     * were newly introduced with the canceled update. Returns a list of files that need to be retypechecked to update
     * their error lists.
     */
    std::vector<core::FileRef> restore(std::unique_ptr<core::GlobalState> &gs, std::vector<std::vector<u1>> &indexed,
                                       UnorderedMap<u4, std::vector<u1>> &indexedFinalGS,
                                       std::vector<core::FileRef> &filesThatHaveErrors);
};

} // namespace sorbet::realmain::lsp
#endif
