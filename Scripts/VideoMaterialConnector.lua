VideoMaterialConnector = {}

function VideoMaterialConnector:GatherProperties()
	return {
        -- Use the generic DatumType.Node (matches the engine's
        -- FirstPersonController / ThirdPersonController convention) so the
        -- inspector accepts a VideoPlayer3D — which is a Node3D subclass —
        -- when it's dropped into the slot.
        {name="videoPlayer", type=DatumType.Node },
        {name="materialAsset", type=DatumType.Asset }
	}
end

function VideoMaterialConnector:Create()
end

function VideoMaterialConnector:Awake()
end

local function bindVideoTexture(s)
    s.materialAsset:SetTexture(1, s.videoPlayer:GetTexture())
    s.videoPlayer:Play()
end

-- Wire the VideoPlayer signals in :Start(). Node-path properties (the
-- "videoPlayer" slot) are resolved by the scene loader BETWEEN :Awake() and
-- :Start() (Scene.cpp ~ResolvePendingNodePaths after the Awake pass), so
-- self.videoPlayer is guaranteed to be the resolved node by the time Start
-- runs but is still nil during Awake. The IsReady() fallback below catches
-- the rare race where the VideoPlayer's async FFmpeg open completes before
-- Start gets the chance to ConnectSignal("OnReady").
function VideoMaterialConnector:Start()
    if self.videoPlayer == nil then
        Log.Error("VideoMaterialConnector: videoPlayer property is not set on " .. tostring(self:GetOwner():GetName()))
        return
    end

    self.videoPlayer:ConnectSignal("OnReady", self, bindVideoTexture)

    self.videoPlayer:ConnectSignal("OnFinished", self, function(s)
    end)

    self.videoPlayer:ConnectSignal("OnError", self, function(s, message)
        Log.Error("VideoPlayer error: " .. tostring(message))
    end)

    self.videoPlayer:ConnectSignal("OnLoop", self, function(s)
    end)

    if self.videoPlayer.IsReady ~= nil and self.videoPlayer:IsReady() then
        bindVideoTexture(self)
    end
end

function VideoMaterialConnector:Tick(deltaTime)
end
