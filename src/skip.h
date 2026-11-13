#pragma once
#include "mem.h"

#include <windows.h>

namespace skip
{
    enum class InstallResult
    {
        Ok,           // at least one feature is active
        NotReady,     // the game's code is not there yet (still packed) - try again later
        Unsupported,  // signatures don't match this executable - nothing was changed
        Failed,       // patching failed - nothing was changed
    };

    // Resolves the game's functions/data by signature, validates them and installs the hooks.
    // Must be called once, from a thread other than the game's (it suspends the other threads while patching).
    InstallResult Install(HMODULE game);

    // Same, for an explicit image / code range (used by the simulated game in tests/).
    InstallResult InstallIn(const mem::Region& image, const mem::Region& text);
    // Tests only: replaces the keyboard query for the extra skip key.
    void SetSkipKeyProbeForTests(bool (*probe)());
}
