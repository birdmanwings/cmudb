#include "common/config.h"

namespace cmudb {
std::atomic<bool> ENABLE_LOGGING(false);  // for virtual table
//  std::chrono::duration<long long int> LOG_TIMEOUT =
//   std::chrono::seconds(1);
std::chrono::duration<long long int, std::milli> LOG_TIMEOUT =
    std::chrono::milliseconds(100);
}
