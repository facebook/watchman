#include "watchman/saved_state/SavedStateFactory.h"
#include "watchman/Errors.h"
#include "watchman/saved_state/LocalSavedStateInterface.h"

#if HAVE_MANIFOLD
#include "watchman/facebook/saved_state/ManifoldSavedStateInterface.h" // @manual
#endif

namespace watchman {

std::unique_ptr<SavedStateInterface> getInterface(
    w_string_piece storageType,
    const json_ref& savedStateConfig,
    const SCM* scm,
    const std::shared_ptr<watchman_root> root) {
  unused_parameter(root);
#if HAVE_MANIFOLD
  if (storageType == "manifold") {
    return std::make_unique<ManifoldSavedStateInterface>(
        savedStateConfig, scm, root);
  }
#endif
  if (storageType == "local") {
    return std::make_unique<LocalSavedStateInterface>(savedStateConfig, scm);
  }
  throw QueryParseError("invalid storage type '", storageType, "'");
}

} // namespace watchman
