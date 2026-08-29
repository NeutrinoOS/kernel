#pragma once

#include "neutrino/exception_context.h"

struct InterruptFrame : NeuExceptionContext {};

static_assert(sizeof(InterruptFrame) == sizeof(NeuExceptionContext));
