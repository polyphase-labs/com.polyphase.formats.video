VideoQuadConnector = {}

function VideoQuadConnector:GatherProperties()
	return {
        {name="videoPlayer", type=DatumType.Node3D },
        {name="quadImage", type=DatumType.Widget }
	}
end

function VideoQuadConnector:Create()
end

function VideoQuadConnector:Awake()
end


function VideoQuadConnector:Start()
 self.videoPlayer:ConnectSignal("OnReady", self, function(s)
        s.quadImage:SetTexture(s.videoPlayer:GetTexture())
        s.videoPlayer:Play()
    end)

    self.videoPlayer:ConnectSignal("OnFinished", self, function(s)
       local t = s;
    end)

    self.videoPlayer:ConnectSignal("OnError", self, function(s, message)
        Log.Error("VideoPlayer error: " .. tostring(message))
    end)

end

function VideoQuadConnector:Tick(deltaTime)

end
