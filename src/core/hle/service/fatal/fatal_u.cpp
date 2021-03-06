// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/service/fatal/fatal_u.h"

namespace Service {
namespace Fatal {

Fatal_U::Fatal_U(std::shared_ptr<Module> module) : Module::Interface(std::move(module), "fatal:u") {
    static const FunctionInfo functions[] = {
        {1, &Fatal_U::FatalSimple, "FatalSimple"},
        {2, &Fatal_U::TransitionToFatalError, "TransitionToFatalError"},
    };
    RegisterHandlers(functions);
}

} // namespace Fatal
} // namespace Service
