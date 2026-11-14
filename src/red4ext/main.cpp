#include <RED4ext/Api/ApiVersion.hpp>
#include <RED4ext/Api/v1/EMainReason.hpp>
#include <RED4ext/Api/v1/Logger.hpp>
#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/PluginInfo.hpp>
#include <RED4ext/Api/v1/Runtime.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>
#include <RED4ext/Api/v1/SemVer.hpp>
#include <RED4ext/Api/v1/Version.hpp>
#include <RED4ext/CNamePool.hpp>
#include <RED4ext/CString.hpp>
#include <RED4ext/Common.hpp>
#include <RED4ext/GameApplication.hpp>
#include <RED4ext/GameStates.hpp>
#include <RED4ext/RTTISystem.hpp>
#include <RED4ext/RTTITypes.hpp>
#include <RED4ext/Scripting/Functions.hpp>
#include <RED4ext/Scripting/IScriptable.hpp>
#include <RED4ext/Scripting/Stack.hpp>
#include <RED4ext/Scripting/Utils.hpp>
#include <fmod.hpp>
#include <fmod_errors.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

/// Native audio playback utility exposed to REDscript / CET.
/// Thin wrapper around FMOD Core providing async streamed playback on a
/// pool of logical channels.
struct AudioPlayer : RED4ext::IScriptable
{
    /// Returns the RTTI type for AudioPlayer.
    RED4ext::CClass* GetNativeType();
};

static RED4ext::TTypedClass<AudioPlayer> s_type("AudioPlayer");

RED4ext::CClass* AudioPlayer::GetNativeType()
{
    return &s_type;
}

// Used to access RED4ext services (logger, etc.).
const RED4ext::v1::Sdk* g_sdk = nullptr;
RED4ext::v1::PluginHandle g_handle = nullptr;

namespace
{
    using namespace RED4ext;

    constexpr int32_t kChannelCount = 64;
    constexpr uint32_t kMaxLoadAttempts = 3;

    /// Per-channel state. Holds the loading FMOD sound, its channel once
    /// playback started, and all parameters captured at Play() time so the
    /// game-thread update loop can apply them when the async stream becomes
    /// ready.
    struct ChannelSlot
    {
        FMOD::Sound* sound = nullptr;
        FMOD::Channel* channel = nullptr;
        int32_t startPos = 0;
        float volume = 1.0f;
        float fade = 0.0f;
        bool pending = false;
        std::string path;
    };

    FMOD::System* g_fmod = nullptr;
    bool g_fmodReady = false;
    std::array<ChannelSlot, kChannelCount + 1> g_slots{};
    std::unordered_map<std::string, uint32_t> g_failures;
    std::filesystem::path g_binDir;

    /// Log an FMOD result when it carries an error.
    void LogFmod(FMOD_RESULT r, const char* where)
    {
        if (r == FMOD_OK)
            return;
        if (g_sdk && g_sdk->logger)
            g_sdk->logger->ErrorF(g_handle, "%s: %s", where, FMOD_ErrorString(r));
    }

    /// Log a free-form error line via the RED4ext logger.
    void LogError(const char* msg)
    {
        if (g_sdk && g_sdk->logger)
            g_sdk->logger->Error(g_handle, msg);
    }

    /// Decode a UTF-8 C string into a std::filesystem::path.
    std::filesystem::path PathFromUtf8(const char* p)
    {
        const auto begin = reinterpret_cast<const char8_t*>(p);
        return std::filesystem::path(std::u8string(begin, begin + std::char_traits<char>::length(p)));
    }

    /// Encode a std::filesystem::path as a UTF-8 std::string.
    std::string PathToUtf8(const std::filesystem::path& p)
    {
        const auto utf8 = p.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    /// Resolve the directory that contains the host game executable.
    std::filesystem::path ResolveGameBin()
    {
        constexpr DWORD kMax = 32768;
        std::vector<wchar_t> buf(kMax);
        const DWORD len = GetModuleFileNameW(nullptr, buf.data(), kMax);
        if (len == 0 || len >= kMax)
            return {};
        return std::filesystem::path(buf.data(), buf.data() + len).parent_path();
    }

    /// Clamp a caller-provided channel id into [0, kChannelCount].
    /// Accepts -1 as an alias for channel 0 (matches the original Lua API).
    /// \return true if the id is valid after normalization.
    bool NormalizeChannel(int32_t& id)
    {
        if (id == -1)
        {
            id = 0;
            return true;
        }
        if (id < 0 || id > kChannelCount)
        {
            if (g_sdk && g_sdk->logger)
                g_sdk->logger->ErrorF(g_handle, "Invalid channel ID: %i", id);
            return false;
        }
        return true;
    }

    /// Stop and release every FMOD resource held by a slot.
    void StopSlot(ChannelSlot& s)
    {
        s.pending = false;
        if (s.channel)
        {
            LogFmod(s.channel->stop(), "FMOD::Channel::stop");
            s.channel = nullptr;
        }
        if (s.sound)
        {
            LogFmod(s.sound->release(), "FMOD::Sound::release");
            s.sound = nullptr;
        }
    }

    /// Stop playback on every slot. Used on unload and on game-state exit.
    void StopAll()
    {
        for (auto& s : g_slots)
            StopSlot(s);
    }

    /// Schedule a linear fade-in on a freshly started channel.
    /// The channel is paused around the fade-point setup so no sample plays
    /// before the ramp is in place.
    void ApplyFadeIn(FMOD::Channel* ch, float duration)
    {
        LogFmod(ch->setPaused(true), "setPaused(true)");

        FMOD::System* sys = nullptr;
        if (ch->getSystemObject(&sys) != FMOD_OK || !sys)
        {
            LogFmod(ch->setPaused(false), "setPaused(false)");
            return;
        }

        int rate = 0;
        if (sys->getSoftwareFormat(&rate, nullptr, nullptr) != FMOD_OK)
        {
            LogFmod(ch->setPaused(false), "setPaused(false)");
            return;
        }

        unsigned long long dsp = 0;
        LogFmod(ch->getDSPClock(0, &dsp), "getDSPClock");
        LogFmod(ch->addFadePoint(dsp, 0.0f), "addFadePoint start");
        LogFmod(ch->addFadePoint(dsp + static_cast<unsigned long long>(rate * duration), 1.0f), "addFadePoint end");
        LogFmod(ch->setPaused(false), "setPaused(false)");
    }

    /// Walk every pending slot and promote ready streams to playing channels.
    /// Called once per game tick while the game is in the Running state.
    void PromotePending()
    {
        for (auto& s : g_slots)
        {
            if (!s.sound || !s.pending)
                continue;

            FMOD_OPENSTATE state = FMOD_OPENSTATE_ERROR;
            if (s.sound->getOpenState(&state, 0, 0, 0) != FMOD_OK)
            {
                StopSlot(s);
                continue;
            }

            if (state == FMOD_OPENSTATE_READY)
            {
                s.pending = false;

                unsigned int lenMs = 0;
                LogFmod(s.sound->getLength(&lenMs, FMOD_TIMEUNIT_MS), "getLength");
                const int32_t start = std::clamp(s.startPos, 0, static_cast<int32_t>(lenMs));
                const float vol = std::max(0.0f, s.volume);

                FMOD_RESULT r = g_fmod->playSound(s.sound, nullptr, false, &s.channel);
                if (r != FMOD_OK || !s.channel)
                {
                    LogFmod(r, "FMOD::System::playSound");
                    StopSlot(s);
                    continue;
                }

                LogFmod(s.channel->setPosition(start, FMOD_TIMEUNIT_MS), "setPosition");
                LogFmod(s.channel->setVolume(vol), "setVolume");
                ApplyFadeIn(s.channel, s.fade);
            }
            else if (state == FMOD_OPENSTATE_ERROR)
            {
                const uint32_t attempts = ++g_failures[s.path];
                if (g_sdk && g_sdk->logger)
                    g_sdk->logger->ErrorF(g_handle,
                                          "Failed to load '%s' (attempt %u).",
                                          s.path.c_str(), attempts);
                StopSlot(s);
            }
        }
    }

    /// Core: synchronously create a stream just to read its length in ms.
    /// \return Length in milliseconds, or 0 on failure.
    uint32_t FetchLength(const char* utf8Path)
    {
        if (!g_fmod)
            return 0;

        FMOD::Sound* snd = nullptr;
        FMOD_RESULT r = g_fmod->createSound(utf8Path, FMOD_CREATESTREAM, nullptr, &snd);
        if (r != FMOD_OK)
        {
            if (g_sdk && g_sdk->logger)
                g_sdk->logger->ErrorF(g_handle,
                                      "createSound: %s. Path: %s",
                                      FMOD_ErrorString(r), utf8Path);
            return 0;
        }

        unsigned int ms = 0;
        LogFmod(snd->getLength(&ms, FMOD_TIMEUNIT_MS), "getLength");
        LogFmod(snd->release(), "FMOD::Sound::release");
        return ms;
    }

    /// Resolve a script-provided path. `startPos == -1` marks the input as a
    /// stream URL and is passed through verbatim; everything else is joined
    /// against the game bin directory.
    std::string ResolveTarget(const CString& scriptPath, int32_t startPos)
    {
        if (startPos == -1)
            return scriptPath.c_str();
        const auto rel = PathFromUtf8(scriptPath.c_str());
        return PathToUtf8(g_binDir / rel);
    }

    /// Core: queue an async stream for playback on the given channel.
    void StartPlayback(int32_t id, const std::string& target,
                       int32_t startPos, float volume, float fade)
    {
        if (!g_fmod || !NormalizeChannel(id))
            return;

        auto it = g_failures.find(target);
        if (it != g_failures.end() && it->second >= kMaxLoadAttempts)
        {
            if (g_sdk && g_sdk->logger)
                g_sdk->logger->ErrorF(g_handle,
                                      "Resource '%s' exceeded max load attempts.",
                                      target.c_str());
            return;
        }

        auto& slot = g_slots[id];
        StopSlot(slot);
        LogFmod(g_fmod->createStream(target.c_str(),
                                     FMOD_2D | FMOD_NONBLOCKING,
                                     nullptr, &slot.sound),
                "FMOD::System::createStream");
        slot.startPos = startPos;
        slot.volume = volume;
        slot.fade = fade;
        slot.pending = true;
        slot.path = target;
    }

    /// Core: set volume on a currently playing channel (clamped to >= 0).
    void SetChannelVolume(int32_t id, float volume)
    {
        if (!NormalizeChannel(id))
            return;
        auto& slot = g_slots[id];
        if (slot.channel)
            LogFmod(slot.channel->setVolume(std::max(0.0f, volume)),
                    "FMOD::Channel::setVolume");
    }

    /// Core: stop a single channel.
    void StopChannel(int32_t id)
    {
        if (!NormalizeChannel(id))
            return;
        StopSlot(g_slots[id]);
    }

    /// REDscript wrapper for:
    ///   Int32 GetSongLength(String path)
    /// \param frame Reads [path], advances code.
    /// \param out Destination for length in ms.
    void RS_GetSongLength(IScriptable*, CStackFrame* frame, int32_t* out, int64_t)
    {
        CString path;
        GetParameter(frame, &path);
        frame->code++;

        const auto target = PathToUtf8(g_binDir / PathFromUtf8(path.c_str()));
        const uint32_t ms = FetchLength(target.c_str());
        if (out)
            *out = static_cast<int32_t>(ms);
    }

    /// REDscript wrapper for:
    ///   Play(Int32 channelID, String path, Int32 startPos, Float volume, Float fade)
    /// \param frame Reads [channelID, path, startPos, volume, fade], advances code.
    void RS_Play(IScriptable*, CStackFrame* frame, void*, int64_t)
    {
        int32_t id = 0;
        CString path;
        int32_t startPos = 0;
        float volume = 1.0f;
        float fade = 0.0f;
        GetParameter(frame, &id);
        GetParameter(frame, &path);
        GetParameter(frame, &startPos);
        GetParameter(frame, &volume);
        GetParameter(frame, &fade);
        frame->code++;

        StartPlayback(id, ResolveTarget(path, startPos), startPos, volume, fade);
    }

    /// REDscript wrapper for:
    ///   Stop(Int32 channelID)
    /// \param frame Reads [channelID], advances code.
    void RS_Stop(IScriptable*, CStackFrame* frame, void*, int64_t)
    {
        int32_t id = 0;
        GetParameter(frame, &id);
        frame->code++;
        StopChannel(id);
    }

    /// REDscript wrapper for:
    ///   SetVolume(Int32 channelID, Float volume)
    /// \param frame Reads [channelID, volume], advances code.
    void RS_SetVolume(IScriptable*, CStackFrame* frame, void*, int64_t)
    {
        int32_t id = 0;
        float volume = 1.0f;
        GetParameter(frame, &id);
        GetParameter(frame, &volume);
        frame->code++;
        SetChannelVolume(id, volume);
    }

    /// Game state: enter Running.
    bool OnEnter(CGameApplication*)
    {
        return true;
    }

    /// Game state: tick while Running. Drives the async-load promotion and
    /// pumps FMOD's internal update.
    bool OnUpdate(CGameApplication*)
    {
        if (!g_fmodReady || !g_fmod)
            return false;
        PromotePending();
        g_fmod->update();
        return false;
    }

    /// Game state: exit Running — release every active channel.
    bool OnExit(CGameApplication*)
    {
        StopAll();
        return true;
    }

    /// Register the AudioPlayer RTTI type.
    void RegisterTypes()
    {
        CNamePool::Add("AudioPlayer");
        s_type.flags = {.isNative = true};
        CRTTISystem::Get()->RegisterType(&s_type);
    }

    /// Register all AudioPlayer static natives.
    void PostRegisterTypes()
    {
        auto* rtti = CRTTISystem::Get();
        s_type.parent = rtti->GetClass("IScriptable");

        CBaseFunction::Flags flags = {.isNative = true, .isStatic = true};

        auto* getLen = CClassStaticFunction::Create(&s_type, "GetSongLength", "GetSongLength", &RS_GetSongLength, flags);
        getLen->AddParam("String", "path");
        getLen->SetReturnType("Int32");
        s_type.RegisterFunction(getLen);

        auto* play = CClassStaticFunction::Create(&s_type, "Play", "Play", &RS_Play, flags);
        play->AddParam("Int32", "channelID");
        play->AddParam("String", "path");
        play->AddParam("Int32", "startPos");
        play->AddParam("Float", "volume");
        play->AddParam("Float", "fade");
        s_type.RegisterFunction(play);

        auto* stop = CClassStaticFunction::Create(&s_type, "Stop", "Stop", &RS_Stop, flags);
        stop->AddParam("Int32", "channelID");
        s_type.RegisterFunction(stop);

        auto* setVol = CClassStaticFunction::Create(&s_type, "SetVolume", "SetVolume", &RS_SetVolume, flags);
        setVol->AddParam("Int32", "channelID");
        setVol->AddParam("Float", "volume");
        s_type.RegisterFunction(setVol);
    }

    /// Create and initialize the FMOD system.
    /// \return true on success.
    bool InitFmod()
    {
        FMOD_RESULT r = FMOD::System_Create(&g_fmod);
        if (r != FMOD_OK || !g_fmod)
        {
            LogFmod(r, "FMOD::System_Create");
            g_fmod = nullptr;
            return false;
        }
        r = g_fmod->init(kChannelCount + 1, FMOD_INIT_NORMAL, nullptr);
        if (r != FMOD_OK)
        {
            LogFmod(r, "FMOD::System::init");
            LogFmod(g_fmod->release(), "FMOD::System::release");
            g_fmod = nullptr;
            return false;
        }
        g_fmodReady = true;
        return true;
    }

    /// Tear down the FMOD system and release every slot.
    void ShutdownFmod()
    {
        StopAll();
        if (g_fmod)
        {
            if (g_fmodReady)
                LogFmod(g_fmod->close(), "FMOD::System::close");
            LogFmod(g_fmod->release(), "FMOD::System::release");
            g_fmod = nullptr;
        }
        g_fmodReady = false;
    }
}

/// RED4ext plugin entry: init FMOD and register the native class on load,
/// tear FMOD down on unload.
RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle handle,
                                        RED4ext::v1::EMainReason reason,
                                        const RED4ext::v1::Sdk* sdk)
{
    switch (reason)
    {
    case RED4ext::v1::EMainReason::Load:
    {
        g_sdk = sdk;
        g_handle = handle;
        g_binDir = ResolveGameBin();
        if (g_binDir.empty())
        {
            LogError("Failed to determine game executable directory");
            return false;
        }
        if (!InitFmod())
            return false;

        RED4ext::v1::GameState state;
        state.OnEnter = &OnEnter;
        state.OnUpdate = &OnUpdate;
        state.OnExit = &OnExit;
        sdk->gameStates->Add(handle, RED4ext::EGameStateType::Running, &state);

        auto* rtti = RED4ext::CRTTISystem::Get();
        rtti->AddRegisterCallback(RegisterTypes);
        rtti->AddPostRegisterCallback(PostRegisterTypes);
        break;
    }
    case RED4ext::v1::EMainReason::Unload:
        ShutdownFmod();
        break;
    }
    return true;
}

/// Provide plugin metadata to RED4ext.
RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* info)
{
    info->name = L"AudioPlayer";
    info->author = L"koryboc";
    info->version = RED4EXT_V1_SEMVER(2, 5, 0);
    info->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST;
    info->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

/// Report supported RED4ext API version.
RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}
