#pragma once
#include <windows.h>

// Observe the game's native SDK separately from OptiScaler's FG replacement.
namespace NativeXeFG
{
bool Hook(HMODULE module);
bool HasLatencyContext();
bool UsesLatencyContext(void* context);
} // namespace NativeXeFG
