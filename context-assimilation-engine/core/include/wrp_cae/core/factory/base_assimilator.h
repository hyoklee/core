#ifndef WRP_CAE_CORE_BASE_ASSIMILATOR_H_
#define WRP_CAE_CORE_BASE_ASSIMILATOR_H_

#include <wrp_cae/core/factory/assimilation_ctx.h>

namespace wrp_cae::core {

/**
 * BaseAssimilator - Abstract interface for data assimilators
 * Concrete implementations handle different data sources (file, URL, etc.)
 */
class BaseAssimilator {
 public:
  virtual ~BaseAssimilator() = default;

  /**
   * Schedule assimilation tasks based on the provided context
   * @param ctx Assimilation context with source, destination, and metadata
   * @return 0 on success, non-zero error code on failure
   */
  virtual int Schedule(const AssimilationCtx& ctx) = 0;
};

}  // namespace wrp_cae::core

#endif  // WRP_CAE_CORE_BASE_ASSIMILATOR_H_
