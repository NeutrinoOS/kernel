#include "kernel/version.hpp"

#ifndef NEUTRINO_KERNEL_VERSION
#define NEUTRINO_KERNEL_VERSION "0.0.0-dev"
#endif

namespace kernel_version {

const char* release() {
    return NEUTRINO_KERNEL_VERSION;
}

}  // namespace kernel_version
