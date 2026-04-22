#pragma once

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginsettingswindow.h"

#include <string>
#include <utility>
#include <vector>

constexpr const char* kPluginVersion = "0.1.0";

class TilYouLose final :
    public BakkesMod::Plugin::BakkesModPlugin,
    public BakkesMod::Plugin::PluginSettingsWindow
{
public:
    void onLoad() override;
    void onUnload() override;

    // PluginSettingsWindow
    void RenderSettings() override;
    std::string GetPluginName() override;
    void SetImGuiContext(uintptr_t ctx) override;

private:
    void OnMatchEnded(std::string eventName);
    bool IsPlayerTheLoser();
    bool IsCurrentPlaylistTargeted();
    int  GetCurrentPlaylistId();
    void ScheduleQuit();

    static const std::vector<std::pair<int, std::string>>& KnownPlaylists();

    bool alreadyTriggeredThisMatch_ = false;
};
