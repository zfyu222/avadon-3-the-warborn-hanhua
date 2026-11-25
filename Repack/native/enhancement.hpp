// Included by the optional adapter after its base-address and logging helpers.
constexpr uintptr_t game_mode = 0x6b52fc;
constexpr uintptr_t panel_mode = 0x6b5300;
constexpr uintptr_t modal_input = 0x6b4e1e;
constexpr uintptr_t area_layer = 0x87041c;
constexpr uintptr_t actor_base = 0x876e78;
constexpr uintptr_t actor_stride = 0x1ab0;

using MatchKey = unsigned(__cdecl*)(int, short);
using UpdateScene = void(__cdecl*)();
using WalkVector = void(__thiscall*)(void*, int, int, short);
using WalkAxis = unsigned(__thiscall*)(void*, short);
MatchKey original_match_key = nullptr;
UpdateScene original_update_scene = nullptr, original_world_labels = nullptr;
WalkVector original_walk_vector = nullptr;
WalkAxis original_walk_axis = nullptr;
unsigned movement_percent = 100, marker_mode = 0;
avzh::CameraFollow camera_follow;

bool foreground_game() {
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

unsigned __declspec(noinline) match_inventory_key(int key, short binding) {
    // Avadon 3 already closes the inventory with binding 3. Only its native
    // close branch is filtered when the corresponding option is turned off.
    if (binding == 3 && at<short>(panel_mode) == 10 &&
        reinterpret_cast<uintptr_t>(_ReturnAddress()) == game_base + 0x6df58) return 0;
    return original_match_key(key, binding);
}

bool party_exploration_actor(const void* actor) {
    const auto offset = reinterpret_cast<uintptr_t>(actor) - (game_base + actor_base - image_base);
    return at<short>(game_mode) == 10 && offset < 8 * actor_stride && offset % actor_stride == 0;
}

short scaled_speed(short speed, unsigned percent) {
    return speed <= 0 ? speed : static_cast<short>(std::min(32767u, (speed * percent + 50u) / 100u));
}

__declspec(noinline) void __fastcall walk_vector(void* position, void*, int x, int y, short speed) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != game_base + 0x4efe6 ||
        !party_exploration_actor(static_cast<unsigned char*>(position) - 0x20)) {
        original_walk_vector(position, x, y, speed);
        return;
    }
    original_walk_vector(position, x, y, scaled_speed(speed, movement_percent));
    *reinterpret_cast<short*>(static_cast<unsigned char*>(position) + 0x12) = speed;
}

__declspec(noinline) unsigned __fastcall walk_axis(void* actor, void*, short axis) {
    const auto call = reinterpret_cast<uintptr_t>(_ReturnAddress()) - game_base;
    if ((call != 0x4f1e8 && call != 0x4f200 && call != 0x4f212 && call != 0x4f22a) ||
        !party_exploration_actor(actor)) return original_walk_axis(actor, axis);
    auto& speed = *reinterpret_cast<short*>(static_cast<unsigned char*>(actor) + 0x32);
    const auto normal = speed;
    speed = scaled_speed(normal, movement_percent);
    const auto result = original_walk_axis(actor, axis);
    speed = normal;
    return result;
}

void __declspec(noinline) update_camera() {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == game_base + 0x6d170) {
        avzh::CameraFrame frame;
        frame.mode = at<short>(game_mode);
        frame.layer = at<short>(area_layer);
        frame.tick = at<uint32_t>(0x6b4e6c);
        frame.allowed = (frame.mode == 10 || frame.mode == 15) && !at<short>(panel_mode) &&
            !at<unsigned char>(modal_input) && foreground_game() && frame.layer >= 0 && frame.layer < 2;
        if (frame.allowed) {
            if (frame.mode == 15)
                frame.preferred = reinterpret_cast<short(__cdecl*)()>(game_base + 0x5fa10)();
            const auto active = reinterpret_cast<unsigned(__cdecl*)(short)>(game_base + 0x5f850);
            for (short index = 0; index < 2; ++index) {
                const auto actor = actor_base + index * actor_stride;
                auto& sample = frame.actors[index];
                sample.x = at<int>(actor + 0x20);
                sample.y = at<int>(actor + 0x24);
                sample.motion = at<short>(actor + 0x1670) == 1 ? 1 : 0;
                sample.valid = (active(index) & 0xff) && at<short>(actor + 0x30) == frame.layer &&
                    sample.x >= 0 && sample.y >= 0 && sample.x <= 32768 && sample.y <= 32768;
            }
        }
        const auto target = camera_follow.update(frame);
        if (target.follow)
            reinterpret_cast<void(__cdecl*)(int, int)>(game_base + 0x3a9e0)(target.x, target.y);
    }
    original_update_scene();
}

uint32_t marker_point(int x, int y) {
    return static_cast<uint32_t>(static_cast<uint16_t>(x)) << 16 | static_cast<uint16_t>(y);
}

void draw_interactables() {
    const auto mode = at<short>(game_mode);
    if ((mode != 10 && mode != 15) || at<short>(panel_mode) || at<unsigned char>(modal_input) ||
        !foreground_game() || (marker_mode == 1 && !(GetKeyState(VK_SHIFT) & 0x8000))) return;
    const auto first = at<uint32_t>(clip_start_address), last = at<uint32_t>(clip_end_address);
    const int left = static_cast<short>(first >> 16), top = static_cast<short>(first);
    const int right = static_cast<short>(last >> 16), bottom = static_cast<short>(last);
    constexpr int width = 22, height = 28;
    if (right - left < width || bottom - top < height) return;
    using ObjectPredicate = unsigned(__thiscall*)(const void*);
    using Draw = void(__cdecl*)(int, uint32_t, uint32_t, uint32_t, uint32_t, float, float, float, float);
    const auto current = reinterpret_cast<ObjectPredicate>(game_base + 0x3ed70);
    const auto interactive = reinterpret_cast<ObjectPredicate>(game_base + 0x401e0);
    const auto visible = reinterpret_cast<unsigned(__cdecl*)(short, short)>(game_base + 0x36fa0);
    const auto in_sight = reinterpret_cast<unsigned(__cdecl*)(int, int, int, int, short)>(game_base + 0x36e40);
    const auto draw = reinterpret_cast<Draw>(game_base + 0x9740);
    const auto rect = [&](int x, int y, int w, int h, float r, float g, float b) {
        draw(0, marker_point(x, y), marker_point(x + w, y + h), first, last, r, g, b, 1.0f);
    };
    for (int index = 0; index < 1200; ++index) {
        const int id = at<int>(0x6a55a8 + index * 4);
        if (id < 9000 || id >= 9600) continue;
        const auto object = 0x9f6898 + (id - 9000) * 0x490;
        const int x0 = at<short>(object + 0x3a), y0 = at<short>(object + 0x38);
        const int x1 = at<short>(object + 0x3e), y1 = at<short>(object + 0x3c);
        if (x0 >= x1 || y0 >= y1 || x1 <= left || x0 >= right || y1 <= top || y0 >= bottom) continue;
        const auto* instance = &at<unsigned char>(object);
        if (at<short>(object + 4) || !(current(instance) & 0xff) || !(interactive(instance) & 0xff) ||
            !(visible(at<short>(object + 0x14), at<short>(object + 0x18)) & 0xff) ||
            !(in_sight(at<int>(object + 0x28), at<int>(object + 0x2c),
                       at<int>(object + 0x30), at<int>(object + 0x34), 1) & 0xff)) continue;
        const int x = std::clamp((x0 + x1) / 2 - width / 2, left, right - width);
        const int y = std::clamp(y0 + 2, top, bottom - height);
        rect(x, y, width, height, 1, 1, 1);
        rect(x + 2, y + 2, 18, 24, 31, 27, 5);
        rect(x + 9, y + 5, 4, 11, 1, 1, 1);
        rect(x + 9, y + 19, 4, 4, 1, 1, 1);
    }
}

void __declspec(noinline) world_labels() {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == game_base + 0x2fc2f) draw_interactables();
    original_world_labels();
}

template <size_t N>
bool install_hook(uintptr_t entry, const unsigned char (&signature)[N],
                  std::initializer_list<uintptr_t> calls, void* detour, void** original) {
    for (const auto site : calls) {
        const auto* bytes = &at<unsigned char>(site);
        int32_t displacement;
        memcpy(&displacement, bytes + 1, sizeof(displacement));
        if (*bytes != 0xe8 || static_cast<intptr_t>(site + 5) + displacement != static_cast<intptr_t>(entry))
            return false;
    }
    auto* target = &at<unsigned char>(entry);
    return !memcmp(target, signature, N) && MH_CreateHook(target, detour, original) == MH_OK;
}

void install_enhancements() {
    const auto path = data_path + L"\\enhancement.ini";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const auto setting = [&](const wchar_t* section, const wchar_t* key, int fallback) {
        return GetPrivateProfileIntW(section, key, fallback, path.c_str());
    };
    font_settings = avzh::FontSettings::load(path);
    const unsigned char inventory[] = {0x55,0x8b,0xec,0x0f,0xbf,0x45,0x0c};
    const unsigned char camera[] = {0x55,0x8b,0xec,0x81,0xec,0xac,0,0,0};
    const unsigned char markers[] = {0x55,0x8b,0xec,0x83,0xec,0x38};
    const unsigned char vector[] = {0x55,0x8b,0xec,0x83,0xec,8,0x56};
    const unsigned char axis[] = {0x55,0x8b,0xec,0x83,0xec,0x2c,0x56};
    if (!setting(L"Inventory", L"ToggleHotkey", 1) &&
        !install_hook(0x4339d0, inventory, {0x46df53}, reinterpret_cast<void*>(&match_inventory_key),
                      reinterpret_cast<void**>(&original_match_key))) log("Inventory setting skipped: runtime signature mismatch");
    if (setting(L"Camera", L"FollowCharacter", 0) &&
        !install_hook(0x43ac10, camera, {0x46d16b}, reinterpret_cast<void*>(&update_camera),
                      reinterpret_cast<void**>(&original_update_scene))) log("Camera setting skipped: runtime signature mismatch");
    marker_mode = std::min(2u, setting(L"Interaction", L"HighlightOnShift", 0));
    if (marker_mode && !install_hook(0x43dcb0, markers, {0x42fc2a}, reinterpret_cast<void*>(&world_labels),
                                    reinterpret_cast<void**>(&original_world_labels))) log("Markers skipped: runtime signature mismatch");
    movement_percent = std::clamp(setting(L"Movement", L"SpeedPercent", 100), 100u, 200u);
    if (movement_percent > 100) {
        const bool vector_ready = install_hook(0x4633e0, vector, {0x44efe1}, reinterpret_cast<void*>(&walk_vector),
                                               reinterpret_cast<void**>(&original_walk_vector));
        if (!vector_ready || !install_hook(0x44fd00, axis, {0x44f1e3,0x44f1fb,0x44f20d,0x44f225},
                                           reinterpret_cast<void*>(&walk_axis), reinterpret_cast<void**>(&original_walk_axis))) {
            if (vector_ready) MH_RemoveHook(&at<unsigned char>(0x4633e0));
            log("Exploration speed skipped: runtime signature mismatch");
        }
    }
}
