MainMenu = {}

function MainMenu:GatherProperties()
	return {
		{name="scenes", type=DatumType.String, array = true },
		{name="comboBox", type=DatumType.ComboBox },
		{name="inputActions", type=DatumType.Asset },

	}
end

function MainMenu:Create()
end

function MainMenu:Awake()
end

function MainMenu:Start()

	PlayerInput.LoadActions(self.inputActions)
	for i, scene in ipairs(self.scenes) do
		self.comboBox:AddOption(scene)
	end
	self.comboBox:SetSelectedIndex(0)

end
function MainMenu:OnConfirm()
	self.world:LoadScene(self.comboBox:GetSelectedOption())
end