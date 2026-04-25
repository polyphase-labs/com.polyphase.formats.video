#pragma once

// Small accessor so any addon source file can reach the PolyphaseEngineAPI* that
// OnLoad caches. Avoids threading the pointer through every node/decoder.

struct PolyphaseEngineAPI;

namespace VideoPlayerAddon
{
    PolyphaseEngineAPI* GetEngineAPI();
    void SetEngineAPI(PolyphaseEngineAPI* api);
}
