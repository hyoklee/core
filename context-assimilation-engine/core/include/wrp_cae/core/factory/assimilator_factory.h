#ifndef WRP_CAE_CORE_ASSIMILATOR_FACTORY_H_
#define WRP_CAE_CORE_ASSIMILATOR_FACTORY_H_

#include <memory>
#include <string>
#include <wrp_cae/core/factory/base_assimilator.h>

namespace wrp_cae::core {

// Forward declarations
class BinaryFileAssimilator;

}  // namespace wrp_cae::core

namespace wrp_cte::core {
class Client;
}  // namespace wrp_cte::core

namespace wrp_cae::core {

/**
 * AssimilatorFactory - Factory for creating appropriate assimilator instances
 * Selects the correct assimilator based on the source URL protocol
 */
class AssimilatorFactory {
 public:
  /**
   * Constructor with CTE client
   * @param cte_client Shared pointer to initialized CTE client
   */
  explicit AssimilatorFactory(std::shared_ptr<wrp_cte::core::Client> cte_client);

  /**
   * Get an assimilator instance for the given source URL
   * @param src Source URL (protocol::path format)
   * @return Unique pointer to an assimilator instance, or nullptr if unsupported
   */
  std::unique_ptr<BaseAssimilator> Get(const std::string& src);

 private:
  /**
   * Extract protocol from URL (part before ::)
   * @param url URL in format protocol::path
   * @return Protocol string, or empty string if no protocol found
   */
  std::string GetUrlProtocol(const std::string& url);

  std::shared_ptr<wrp_cte::core::Client> cte_client_;
};

}  // namespace wrp_cae::core

#endif  // WRP_CAE_CORE_ASSIMILATOR_FACTORY_H_
