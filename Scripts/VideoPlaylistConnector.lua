-- VideoPlaylistConnector
--
-- Drop this script onto a Node, drag in a VideoPlaylistPlayer and a Quad
-- widget. The connector forwards the active clip's texture onto the quad on
-- every clip change so the visual surface updates automatically when the
-- playlist advances. TrueSeamless mode swaps which child is "primary" on each
-- transition, so we re-fetch GetCurrentTexture() in OnPlaylistItemStarted.

VideoPlaylistConnector = {}

function VideoPlaylistConnector:GatherProperties()
    return {
        {name="playlistPlayer", type=DatumType.Node3D },
        {name="quadImage",      type=DatumType.Widget },
    }
end

function VideoPlaylistConnector:Start()
    if self.playlistPlayer == nil then
        Log.Warning("VideoPlaylistConnector: playlistPlayer is unset")
        return
    end

    -- Bind the quad to whichever child is currently the primary. Fires on the
    -- first clip AND on every advance, so TrueSeamless swaps stay correct.
    self.playlistPlayer:ConnectSignal("OnPlaylistItemStarted", self, function(s, playlistName, assetName)
        if s.quadImage ~= nil then
            local tex = s.playlistPlayer:GetCurrentTexture()
            if tex ~= nil then
                s.quadImage:SetTexture(tex)
            end
        end
    end)

    self.playlistPlayer:ConnectSignal("OnPlaylistStarted", self, function(s, playlistName, assetName)
       local t = s;
    end)

    self.playlistPlayer:ConnectSignal("OnPlaylistEnded", self, function(s, playlistName, assetName)
        local t = s;

    end)
end
