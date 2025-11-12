#ifndef WRP_CAE_CORE_CONSTANTS_H_
#define WRP_CAE_CORE_CONSTANTS_H_

#include <chimaera/chimaera.h>

namespace wrp_cae::core {

// CAE Pool ID - used consistently across tests and utilities
// This ID uniquely identifies the CAE core container pool
constexpr chi::PoolId kCaePoolId(400, 0);

}  // namespace wrp_cae::core

#endif  // WRP_CAE_CORE_CONSTANTS_H_
