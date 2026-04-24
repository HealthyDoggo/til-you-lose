#include "pch.h"
#include "TilYouLose.h"

BAKKESMOD_PLUGIN(TilYouLose,
    "Closes Rocket League when you lose a match in selected playlists",
    "0.1.0", PLUGINTYPE_FREEPLAY)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

namespace {

// Rocket League playlist IDs exposed in the settings UI. Not exhaustive,
// but covers the standard competitive and casual lineups plus extra modes.
const std::vector<std::pair<int, std::string>> kPlaylistCatalog = {
    {  1, "Casual Duel"      },
    {  2, "Casual Doubles"   },
    {  3, "Casual Standard"  },
    {  4, "Casual Chaos"     },
    { 10, "Ranked Duel"      },
    { 11, "Ranked Doubles"   },
    { 13, "Ranked Standard"  },
    { 27, "Hoops"            },
    { 28, "Rumble"           },
    { 29, "Dropshot"         },
    { 30, "Snow Day"         },
    { 34, "Tournament"       },
};

constexpr const char* kEnabledCvar     = "tyl_enabled";
constexpr const char* kPlaylistsCvar   = "tyl_playlists";
constexpr const char* kDelayCvar       = "tyl_delay_seconds";
constexpr const char* kForceQuitCvar   = "tyl_force_quit";

std::set<int> ParsePlaylists(const std::string& csv)
{
    std::set<int> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        try {
            if (!item.empty()) out.insert(std::stoi(item));
        } catch (...) {
            // skip malformed entries
        }
    }
    return out;
}

std::string JoinPlaylists(const std::set<int>& ids)
{
    std::string out;
    bool first = true;
    for (int id : ids) {
        if (!first) out += ",";
        out += std::to_string(id);
        first = false;
    }
    return out;
}

} // namespace

const std::vector<std::pair<int, std::string>>& TilYouLose::KnownPlaylists()
{
    return kPlaylistCatalog;
}

void TilYouLose::onLoad()
{
    _globalCvarManager = cvarManager;

    cvarManager->registerCvar(kEnabledCvar, "1",
        "Enable Til You Lose (master switch)", true, true, 0, true, 1);
    cvarManager->registerCvar(kPlaylistsCvar, "10,11,13",
        "Comma-separated list of playlist IDs that trigger quit-on-loss");
    cvarManager->registerCvar(kDelayCvar, "3",
        "Seconds to wait after match end before closing the game",
        true, true, 0, true, 60);
    cvarManager->registerCvar(kForceQuitCvar, "0",
        "Force-terminate the process instead of calling the quit command",
        true, true, 0, true, 1);

    // Natural match end.
    gameWrapper->HookEvent(
        "Function TAGame.GameEvent_Soccar_TA.EventMatchEnded",
        [this](std::string name) { OnMatchEnded(name); });

    // Post-match screen transition / forfeits.
    gameWrapper->HookEvent(
        "Function TAGame.GameEvent_Soccar_TA.OnMatchEnded",
        [this](std::string name) { OnMatchEnded(name); });

    // Reset the per-match latch when a new match starts.
    gameWrapper->HookEvent(
        "Function TAGame.GameEvent_Soccar_TA.InitGame",
        [this](std::string) { alreadyTriggeredThisMatch_ = false; });
    gameWrapper->HookEvent(
        "Function TAGame.GameEvent_TA.EventMatchStarted",
        [this](std::string) { alreadyTriggeredThisMatch_ = false; });

    cvarManager->log(std::string("[TilYouLose] Loaded v") + kPluginVersion);
}

void TilYouLose::onUnload()
{
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_Soccar_TA.EventMatchEnded");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_Soccar_TA.OnMatchEnded");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_Soccar_TA.InitGame");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_TA.EventMatchStarted");
    cvarManager->log("[TilYouLose] Unloaded");
}

int TilYouLose::GetCurrentPlaylistId()
{
    ServerWrapper server = gameWrapper->GetOnlineGame();
    if (server.IsNull()) server = gameWrapper->GetCurrentGameState();
    if (server.IsNull()) return -1;

    PlaylistWrapper playlist = server.GetPlaylist();
    if (playlist.memory_address == 0) return -1;
    return playlist.GetPlaylistId();
}

bool TilYouLose::IsCurrentPlaylistTargeted()
{
    const int id = GetCurrentPlaylistId();
    if (id < 0) return false;
    const auto selected = ParsePlaylists(
        cvarManager->getCvar(kPlaylistsCvar).getStringValue());
    return selected.count(id) > 0;
}

bool TilYouLose::IsPlayerTheLoser()
{
    ServerWrapper server = gameWrapper->GetOnlineGame();
    if (server.IsNull()) server = gameWrapper->GetCurrentGameState();
    if (server.IsNull()) return false;

    int playerTeam = -1;
    CarWrapper localCar = gameWrapper->GetLocalCar();
    if (!localCar.IsNull()) {
        PriWrapper pri = localCar.GetPRI();
        if (!pri.IsNull()) playerTeam = pri.GetTeamNum();
    }
    if (playerTeam < 0) {
        PlayerControllerWrapper pc = gameWrapper->GetPlayerController();
        if (!pc.IsNull()) {
            PriWrapper pri = pc.GetPRI();
            if (!pri.IsNull()) playerTeam = pri.GetTeamNum();
        }
    }
    if (playerTeam < 0) return false;

    int myScore = -1;
    int bestOtherScore = -1;
    ArrayWrapper<TeamWrapper> teams = server.GetTeams();
    for (int i = 0; i < teams.Count(); ++i) {
        TeamWrapper team = teams.Get(i);
        if (team.IsNull()) continue;
        const int teamIdx = team.GetTeamIndex();
        const int score   = team.GetScore();
        if (teamIdx == playerTeam) {
            myScore = score;
        } else if (score > bestOtherScore) {
            bestOtherScore = score;
        }
    }
    if (myScore < 0 || bestOtherScore < 0) return false;
    return myScore < bestOtherScore;
}

void TilYouLose::OnMatchEnded(std::string /*eventName*/)
{
    if (alreadyTriggeredThisMatch_) return;
    if (!cvarManager->getCvar(kEnabledCvar).getBoolValue()) return;

    if (!IsCurrentPlaylistTargeted()) {
        cvarManager->log("[TilYouLose] Match ended - playlist not targeted, staying open");
        return;
    }
    if (!IsPlayerTheLoser()) {
        cvarManager->log("[TilYouLose] Match ended - you did not lose, staying open");
        return;
    }

    alreadyTriggeredThisMatch_ = true;
    cvarManager->log("[TilYouLose] Loss detected - scheduling quit");
    ScheduleQuit();
}

void TilYouLose::ScheduleQuit()
{
    const float delay     = cvarManager->getCvar(kDelayCvar).getFloatValue();
    const bool  forceQuit = cvarManager->getCvar(kForceQuitCvar).getBoolValue();

    gameWrapper->SetTimeout([forceQuit](GameWrapper*) {
        _globalCvarManager->log("[TilYouLose] Closing Rocket League now");
        if (forceQuit) {
            ::ExitProcess(0);
        } else {
            _globalCvarManager->executeCommand("quit", false);
        }
    }, delay);
}

// ---------------------------------------------------------------------------
// Settings UI (F2 -> Plugins -> Til You Lose)
// ---------------------------------------------------------------------------

std::string TilYouLose::GetPluginName()
{
    return "Til You Lose";
}

void TilYouLose::SetImGuiContext(uintptr_t ctx)
{
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

void TilYouLose::RenderSettings()
{
    ImGui::TextUnformatted(
        "Closes Rocket League after you lose a match in any of the selected playlists.");
    ImGui::Separator();

    {
        auto cv = cvarManager->getCvar(kEnabledCvar);
        bool v = cv.getBoolValue();
        if (ImGui::Checkbox("Plugin enabled", &v)) cv.setValue(v);
    }
    {
        auto cv = cvarManager->getCvar(kForceQuitCvar);
        bool v = cv.getBoolValue();
        if (ImGui::Checkbox("Force-terminate process (skip clean quit)", &v)) cv.setValue(v);
    }
    {
        auto cv = cvarManager->getCvar(kDelayCvar);
        float v = cv.getFloatValue();
        if (ImGui::SliderFloat("Delay before closing (seconds)", &v, 0.0f, 60.0f, "%.1f")) {
            cv.setValue(v);
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Playlists that trigger quit-on-loss:");

    auto playlistsCvar = cvarManager->getCvar(kPlaylistsCvar);
    auto selected = ParsePlaylists(playlistsCvar.getStringValue());
    bool changed = false;

    for (const auto& entry : KnownPlaylists()) {
        const int id = entry.first;
        const std::string& name = entry.second;
        bool on = selected.count(id) > 0;
        const std::string label =
            std::to_string(id) + " - " + name + "##tyl_pl_" + std::to_string(id);
        if (ImGui::Checkbox(label.c_str(), &on)) {
            if (on) selected.insert(id);
            else    selected.erase(id);
            changed = true;
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Custom playlist IDs (comma-separated):");
    {
        std::string raw = JoinPlaylists(selected);
        char buf[256] = {0};
        strncpy_s(buf, raw.c_str(), sizeof(buf) - 1);
        if (ImGui::InputText("##tyl_playlists_raw", buf, sizeof(buf))) {
            selected = ParsePlaylists(buf);
            changed = true;
        }
    }

    if (changed) {
        playlistsCvar.setValue(JoinPlaylists(selected));
    }

    ImGui::Separator();
    const int currentId = GetCurrentPlaylistId();
    if (currentId >= 0) {
        ImGui::Text("Current playlist ID: %d (%s)",
            currentId,
            selected.count(currentId) ? "targeted" : "ignored");
    } else {
        ImGui::TextUnformatted("Not currently in a match.");
    }
}
