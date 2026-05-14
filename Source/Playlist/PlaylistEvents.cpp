#include "Playlist/PlaylistEvents.h"

#include "Datum.h"

namespace VideoPlayerAddon
{

PlaylistEventDispatcher& PlaylistEventDispatcher::Get()
{
    static PlaylistEventDispatcher sInstance;
    return sInstance;
}

PlaylistEventDispatcher::ListenerId
PlaylistEventDispatcher::Connect(const std::string& eventName, const ScriptFunc& fn)
{
    if (eventName.empty() || !fn.IsValid())
    {
        return kInvalidId;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    Entry e;
    e.id    = mNextId++;
    e.event = eventName;
    e.fn    = fn;
    mEntries.push_back(std::move(e));
    return mEntries.back().id;
}

bool PlaylistEventDispatcher::Disconnect(ListenerId id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
    {
        if (it->id == id)
        {
            mEntries.erase(it);
            return true;
        }
    }
    return false;
}

void PlaylistEventDispatcher::Fire(const std::string& eventName,
                                   const std::string& playlistName,
                                   const std::string& assetName)
{
    // Snapshot matching listeners under the mutex so a callback that mutates
    // the dispatcher (Connect/Disconnect inside the handler) doesn't deadlock
    // or invalidate iterators.
    std::vector<ScriptFunc> callbacks;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        callbacks.reserve(mEntries.size());
        for (const Entry& e : mEntries)
        {
            if (e.event == eventName)
            {
                callbacks.push_back(e.fn);
            }
        }
    }

    if (callbacks.empty()) return;

    // ScriptFunc::Call takes Datum* params + count. Build once and reuse.
    Datum params[2];
    params[0] = Datum(playlistName);
    params[1] = Datum(assetName);

    for (const ScriptFunc& fn : callbacks)
    {
        fn.Call(2, params);
    }
}

void PlaylistEventDispatcher::Clear()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mEntries.clear();
}

} // namespace VideoPlayerAddon
