DemoInputEvents = {}

function DemoInputEvents:Create()
end


function DemoInputEvents:Start()



end
function DemoInputEvents:Tick(deltaTime)

	 if PlayerInput.WasJustActivated("Game", "MainMenu") then
        self.world:LoadScene("SC_VideoPlayer_MainMenu")
    end

end