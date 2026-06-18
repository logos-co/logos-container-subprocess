// Link-time provider for the logos-container factory seam.
//
// Defines LogosCore::makeContainer() (declared in the logos-container contract)
// to return this repo's SubprocessContainer, upcast to the ModuleContainer
// interface. Linking this library makes the subprocess container the build's
// default; a different container repo would provide its own definition of the
// same symbol. Consumers (logos-liblogos) call makeContainer() and never name
// SubprocessContainer.
#include <logos_container/container_factory.h>
#include "subprocess_container.h"

namespace LogosCore {

std::shared_ptr<ModuleContainer> makeContainer() {
    return std::make_shared<::SubprocessContainer>();
}

} // namespace LogosCore
