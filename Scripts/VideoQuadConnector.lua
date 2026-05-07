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
        Log.Debug(string.format("VideoPlayer ready: %.2fs duration", s.videoPlayer:GetDuration()))
        s.videoPlayer:Play()
    end)

    self.videoPlayer:ConnectSignal("OnFinished", self, function(s)
        Log.Debug(string.format("VideoPlayer finished at %.2fs", s.videoPlayer:GetTime()))
    end)

    self.videoPlayer:ConnectSignal("OnError", self, function(s, message)
        Log.Error("VideoPlayer error: " .. tostring(message))
    end)

end

function VideoQuadConnector:Tick(deltaTime)

	if Input.IsKeyPressed(Key.Space) then
		self.videoPlayer:Stop()
	end
	if Input.IsKeyPressed(Key.T) then
	self.videoPlayer:Seek(1.0)
	end
end
