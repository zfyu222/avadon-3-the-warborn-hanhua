#include <array>
#include <iomanip>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include "renderer.hpp"
#include <bcrypt.h>
#include "forward_metadata.hpp"
#include "MinHook.h"
#ifdef AV3_ENHANCEMENT
#include "camera_follow.hpp"
#endif

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")

extern "C" {
FARPROC g_forwards[AVZH_FORWARD_COUNT]{};
void* g_original_sprintf = nullptr;
}
extern "C" void sprintf_entry();

namespace {
static_assert(sizeof(void*) == 4, "x86 game");
constexpr uintptr_t image_base = 0x400000;
constexpr uintptr_t layout_address = 0x488780;
constexpr uintptr_t measure_address = 0x488600;
constexpr uintptr_t sprintf_address = 0x4caaf5;
constexpr uintptr_t expand_address = 0x489c70;
constexpr uintptr_t line_height_address = 0x6b4edc;
constexpr uintptr_t clip_flag_address = 0x6b4cb4;
constexpr uintptr_t clip_start_address = 0x6b46a8;
constexpr uintptr_t clip_end_address = 0x6b46ac;
constexpr uintptr_t alpha_address = 0x5f5f80;
constexpr size_t text_limit = 4096;
constexpr char supported_sha[] = "c9b1e33cbd14fa21ea876f50355ad7369a9fd74186e08e669210acf0cab7c503";

using Layout = void(__cdecl*)(void*, uint32_t, uint32_t, const char*, short, short, short, short, short);
using Measure = short(__cdecl*)(const char*);
using Expand = void(__cdecl*)(char*);
Layout original_layout = nullptr;
Measure original_measure = nullptr;
uintptr_t game_base = 0;
std::wstring data_path;
INIT_ONCE real_once = INIT_ONCE_STATIC_INIT;
INIT_ONCE adapter_once = INIT_ONCE_STATIC_INIT;
HMODULE system_winmm = nullptr;
thread_local avzh::Renderer renderer;
avzh::FontSettings font_settings;
std::mutex log_mutex;
std::set<uint32_t> consumed;

struct Entry {
    std::string source, target;
    std::unique_ptr<avzh::Dictionary> format;
};
std::unordered_map<uint32_t, Entry> native_strings;
std::unordered_map<uint32_t, std::string> resource_strings;
struct WideSlot {
    uint32_t rva;
    std::string expected;
    std::wstring replacement;
    bool length_prefixed = false;
};
std::vector<WideSlot> wide_slots;

struct FormatCall {
    char* output;
    const Entry* entry;
    uintptr_t resume;
    uint32_t rva;
    std::wstring display;
};
struct FormattedText {
    std::string original;
    std::wstring translation;
    uint32_t rva;
};
thread_local std::array<FormatCall, 8> format_calls{};
thread_local size_t format_depth = 0;
thread_local std::unordered_map<const char*, FormattedText> formatted_text;

template<class T> T& at(uintptr_t address) {
    return *reinterpret_cast<T*>(game_base + address - image_base);
}

void log(const std::string& description, uint32_t identifier = 0) noexcept {
    try {
        std::lock_guard<std::mutex> lock(log_mutex);
        if (data_path.empty() || (identifier && (consumed.size() >= 128 || !consumed.insert(identifier).second))) return;
        std::ofstream output((data_path + L"\\runtime.log").c_str(), std::ios::app);
        output << description;
        if (identifier) output << " " << std::hex << identifier;
        output << '\n';
    } catch (...) {}
}

#ifdef AV3_ENHANCEMENT
#include "enhancement.hpp"
#endif

std::string executable_hash(const std::wstring& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!input || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot open executable for hashing");
    struct CloseAlgorithm {
        BCRYPT_ALG_HANDLE value;
        ~CloseAlgorithm() { BCryptCloseAlgorithmProvider(value, 0); }
    } close_algorithm{algorithm};
    DWORD object_size = 0, result_size = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &result_size, 0) < 0)
        throw std::runtime_error("SHA256 metadata unavailable");
    std::vector<unsigned char> object(object_size);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot initialize SHA256");
    struct CloseHash {
        BCRYPT_HASH_HANDLE value;
        ~CloseHash() { BCryptDestroyHash(value); }
    } close_hash{hash};
    std::array<unsigned char, 65536> bytes{};
    while (input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || input.gcount()) {
        if (BCryptHashData(hash, bytes.data(), static_cast<ULONG>(input.gcount()), 0) < 0)
            throw std::runtime_error("Cannot hash executable");
    }
    std::array<unsigned char, 32> digest{};
    if (input.bad() || BCryptFinishHash(hash, digest.data(), digest.size(), 0) < 0)
        throw std::runtime_error("Cannot finalize executable hash");
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (auto byte : digest) output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}

void load_registry() {
    std::ifstream input((data_path + L"\\resources.bin").c_str(), std::ios::binary);
    auto read = [&](void* data, size_t size) {
        if (!input.read(static_cast<char*>(data), size)) throw std::runtime_error("Truncated resource registry");
    };
    auto integer = [&]() { uint32_t result = 0; read(&result, sizeof(result)); return result; };
    auto string = [&](uint32_t size) {
        if (!size || size > 16384) throw std::runtime_error("Invalid registry string length");
        std::string result(size, '\0');
        read(result.data(), size);
        return result;
    };
    char magic[8];
    read(magic, sizeof(magic));
    if (memcmp(magic, "AV3LOC1", sizeof(magic))) throw std::runtime_error("Registry version mismatch");
    const auto count = integer();
    if (count > 46655) throw std::runtime_error("Registry count limit");
    for (uint32_t i = 0; i < count; ++i) {
        const auto address = integer(), original_length = integer(), translated_length = integer();
        auto original = string(original_length);
        auto translated = string(translated_length);
        if (address & 0x40000000) {
            auto replacement = avzh::utf8(translated);
            if ((address & 1) || replacement.size() * sizeof(wchar_t) != original.size())
                throw std::runtime_error("Invalid wide window text slot");
            wide_slots.push_back({address & 0x3fffffff, std::move(original), std::move(replacement),
                                  (address & 0x80000000) != 0});
        } else if (address) {
            Entry entry{std::move(original), std::move(translated), nullptr};
            if (address & 0x80000000) {
                entry.format = std::make_unique<avzh::Dictionary>();
                entry.format->add(entry.source, entry.target);
                entry.format->prepare();
            }
            if (!native_strings.emplace(address & 0x7fffffff, std::move(entry)).second)
                throw std::runtime_error("Duplicate native string RVA");
        } else {
            size_t used = 0;
            const auto number = std::stoul(original, &used);
            if (used != original.size() || !number || number > 65535 ||
                !resource_strings.emplace(number, std::move(translated)).second)
                throw std::runtime_error("Duplicate or invalid resource marker");
        }
    }
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing registry bytes");
}

void apply_wide_slots(char* image, size_t size) {
    uint32_t last = 0;
    std::sort(wide_slots.begin(), wide_slots.end(),
              [](const WideSlot& a, const WideSlot& b) { return a.rva < b.rva; });
    for (const auto& slot : wide_slots) {
        const size_t end = static_cast<size_t>(slot.rva) + slot.expected.size();
        if (!slot.rva || slot.rva < last || end < slot.rva || end > size ||
            memcmp(image + slot.rva, slot.expected.data(), slot.expected.size()))
            throw std::runtime_error("Wide window text does not match the original image");
        if (slot.length_prefixed) {
            WORD characters = 0;
            if (slot.rva < 2) throw std::runtime_error("Wide resource is missing its length");
            memcpy(&characters, image + slot.rva - 2, sizeof(characters));
            if (characters != slot.replacement.size()) throw std::runtime_error("Wide resource length mismatch");
        } else if (size - end < 2 || image[end] || image[end + 1]) {
            throw std::runtime_error("Wide window text is missing its terminator");
        }
        last = static_cast<uint32_t>(end + (slot.length_prefixed ? 0 : 2));
    }
    struct WritableSlots {
        struct Region { char* address; size_t size; DWORD original; };
        std::vector<Region> regions;
        ~WritableSlots() {
            for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
                DWORD ignored = 0;
                VirtualProtect(it->address, it->size, it->original, &ignored);
            }
        }
    } writable;
    writable.regions.reserve(wide_slots.size());
    for (const auto& slot : wide_slots) {
        auto* address = image + slot.rva;
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(address, &region, sizeof(region)) ||
            (region.Protect != PAGE_READONLY && region.Protect != PAGE_READWRITE &&
             region.Protect != PAGE_WRITECOPY) ||
            static_cast<size_t>(address - static_cast<char*>(region.BaseAddress)) +
                slot.expected.size() > region.RegionSize)
            throw std::runtime_error("Wide window text is outside a readable data region");
        DWORD original = 0;
        if (!VirtualProtect(address, slot.expected.size(), PAGE_READWRITE, &original))
            throw std::runtime_error("Cannot open wide window text for replacement");
        writable.regions.push_back({address, slot.expected.size(), original});
    }
    for (const auto& slot : wide_slots)
        memcpy(image + slot.rva, slot.replacement.data(), slot.expected.size());
}

std::wstring to_wide(const std::string& value, UINT codepage) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(codepage, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Cannot decode game text");
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(codepage, 0, value.data(), static_cast<int>(value.size()), wide.data(), size);
    return wide;
}

void expand_tokens(std::wstring& target) {
    const auto expand = reinterpret_cast<Expand>(game_base + expand_address - image_base);
    for (const char* name : {"[N]", "[C]", "[P]", "[O]"}) {
        const auto token = to_wide(name, 1252);
        auto position = target.find(token);
        if (position == std::wstring::npos) continue;
        char buffer[1024]{};
        memcpy(buffer, name, 3);
        expand(buffer);
        const auto replacement = to_wide(buffer, 1252);
        while (position != std::wstring::npos) {
            target.replace(position, token.size(), replacement);
            position = target.find(token, position + replacement.size());
        }
    }
}

int digit(wchar_t value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'Z') return value - 'A' + 10;
    if (value >= 'a' && value <= 'z') return value - 'a' + 10;
    return -1;
}

bool resolve_markers(std::wstring_view source, std::wstring& target) {
    size_t consumed_bytes = 0;
    for (auto marker = source.find(L'~'); marker != std::wstring_view::npos && marker + 4 < source.size();
         marker = source.find(L'~', marker + 1)) {
        if (source[marker + 4] != '~') continue;
        uint32_t ordinal = 0;
        for (size_t i = 1; i <= 3; ++i) {
            const int part = digit(source[marker + i]);
            if (part < 0) { ordinal = 0; break; }
            ordinal = ordinal * 36 + part;
        }
        const auto resource = resource_strings.find(ordinal);
        if (resource == resource_strings.end()) continue;
        if (!(marker == 1 && source[0] == '$'))
            target.append(source.substr(consumed_bytes, marker - consumed_bytes));
        auto text = avzh::utf8(resource->second);
        bool opening = true;
        for (auto& character : text) {
            if (character == L'_') {
                character = opening ? L'“' : L'”';
                opening = !opening;
            }
        }
        target += text;
        consumed_bytes = marker + 5;
        marker += 4;
        log("resource ordinal", ordinal);
    }
    if (!consumed_bytes) return false;
    target.append(source.substr(consumed_bytes));
    return true;
}

const Entry* copied_text(uintptr_t address, std::string_view source) {
    uintptr_t first = 0, last = 0;
    if (address == 0x6b4ee0 || (address >= 0x6acea8 && address <= 0x6ad2a8 &&
                               (address - 0x6acea8) % 0x200 == 0)) {
        // Tutorial producers copy full strings into fixed display buffers.
        first = 0x5c8a00;
        last = 0x5cc100;
    } else if (address == 0x5fbb70 || address == 0x5fbc70) {
        first = 0x5cb3c4;
        last = 0x5cb3f8;
    }
    if (!first) return nullptr;
    for (const auto& pair : native_strings) {
        if (pair.first < first - image_base || pair.first > last - image_base || pair.second.source != source) continue;
        return &pair.second;
    }
    return nullptr;
}

std::wstring book_notice(std::string_view source) {
    constexpr std::string_view prefix = "This book is about ";
    if (source.substr(0, prefix.size()) != prefix) return {};
    const auto opening = native_strings.find(0x1cb8a8);
    if (opening == native_strings.end()) return {};
    for (const auto rva : {0x1cb880, 0x1cb8d0}) {
        const auto ending = native_strings.find(rva);
        if (ending == native_strings.end()) continue;
        const auto& suffix = ending->second.source;
        if (source.size() <= prefix.size() + suffix.size() ||
            source.substr(source.size() - suffix.size()) != suffix) continue;
        auto lead = avzh::utf8(opening->second.target);
        if (!lead.empty() && lead.back() == L' ') lead.pop_back();
        const auto subject = source.substr(prefix.size(), source.size() - prefix.size() - suffix.size());
        return lead + to_wide(std::string(subject), 1252) + avzh::utf8(ending->second.target);
    }
    return {};
}

const wchar_t* menu_label(uintptr_t address, std::string_view source) {
    if ((address == 0x5d20f8 || address == 0x5d3a5c) && source == "Hard") return L"困难";
    if ((address == 0x5d2adc || address == 0x5d2ae4) && source == "Male") return L"男性";
    return nullptr;
}

bool localize(const char* input, std::wstring& target) {
    if (!input) return false;
    const auto formatted = formatted_text.find(input);
    const auto pointer = reinterpret_cast<uintptr_t>(input);
    const auto native = pointer >= game_base ? native_strings.find(pointer - game_base) : native_strings.end();
    if (formatted != formatted_text.end() && formatted->second.original == input) {
        target = formatted->second.translation;
        log("formatted native RVA", formatted->second.rva);
    } else if (native != native_strings.end() && native->second.source == input) {
        target = avzh::utf8(native->second.target);
        log("native RVA", native->first);
    } else {
        const auto length = strnlen(input, text_limit + 1);
        if (length > text_limit) return false;
        const auto address = pointer >= game_base ? pointer - game_base + image_base : 0;
        const auto source = std::string_view(input, length);
        if (const auto* label = menu_label(address, source)) {
            target = label;
        } else if (address == 0x5f314e && source == "Wait") {
            target = L"等待";
        } else if (const auto* entry = copied_text(address, source)) {
            target = avzh::utf8(entry->target);
        } else if (address == 0x6b4ee0 && !(target = book_notice(source)).empty()) {
            // The notice buffer is a byte copy of the composed text.
        } else if (!resolve_markers(to_wide(std::string(input, length), 1252), target)) {
            return false;
        }
    }
#ifndef AV3_OFFLINE_TEST
    expand_tokens(target);
#endif
    std::wstring resolved;
    if (resolve_markers(target, resolved)) target = std::move(resolved);
    return true;
}

int x(uint32_t packed) { return static_cast<int16_t>(packed >> 16); }
int y(uint32_t packed) { return static_cast<int16_t>(packed & 0xffff); }
short native_short(int value) { return static_cast<short>(std::clamp(value, -32768, 32767)); }

bool ascii_word(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
}

bool break_before(wchar_t prior, wchar_t next) {
    constexpr std::wstring_view opening = L"（《「『【〔“‘(";
    if (avzh::closing(next) || opening.find(prior) != std::wstring_view::npos) return false;
    if (prior == L' ' && ascii_word(next)) return true;
    return prior >= 0x2e80 || next >= 0x2e80;
}

std::vector<std::wstring> wrap_chinese(const std::wstring& text, int width, int step) {
    std::vector<std::wstring> lines;
    size_t begin = 0;
    while (true) {
        const auto separator = text.find_first_of(L"|\r\n", begin);
        const auto end = separator == std::wstring::npos ? text.size() : separator;
        if (begin == end) lines.emplace_back();
        while (begin < end) {
            size_t position = begin, preferred = begin;
            while (position < end) {
                size_t following = position + 1;
                if (text[position] >= 0xd800 && text[position] <= 0xdbff && following < end &&
                    text[following] >= 0xdc00 && text[following] <= 0xdfff) ++following;
                if (renderer.measure(text.substr(begin, following - begin), step) > width && position > begin)
                    break;
                position = following;
                if (position < end && break_before(text[position - 1], text[position])) preferred = position;
            }
            if (position < end && preferred > begin) position = preferred;
            auto line = text.substr(begin, position - begin);
            while (!line.empty() && line.back() == L' ') line.pop_back();
            lines.push_back(std::move(line));
            begin = position;
            while (begin < end && text[begin] == L' ') ++begin;
        }
        if (separator == std::wstring::npos) break;
        begin = separator + 1;
        if (text[separator] == L'\r' && begin < text.size() && text[begin] == L'\n') ++begin;
    }
    return lines;
}

struct Projection {
    GLint prior = GL_MODELVIEW;
    Projection(int width, int height) {
        glGetIntegerv(GL_MATRIX_MODE, &prior);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, width, 0, height, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
    }
    ~Projection() {
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(prior);
    }
};

bool render(uint32_t first, uint32_t last, const char* text, short align, short font,
            short red, short green, short blue) {
    const auto dc = wglGetCurrentDC();
    RECT client{};
    const auto window = dc ? WindowFromDC(dc) : nullptr;
    if (!window || !GetClientRect(window, &client)) return false;
    const auto top_window = GetAncestor(window, GA_ROOT);
    static HWND checked_window = nullptr;
    if (top_window && top_window != checked_window) {
        wchar_t title[64]{};
        if (GetWindowTextW(top_window, title, 64) > 0 && !wcscmp(title, L"Avadon 3: The Warborn"))
            SetWindowTextW(top_window, L"阿瓦登 3：战裔");
        checked_window = top_window;
    }
    avzh::TextState state;
    state.settings = font_settings;
    state.screen_width = client.right;
    state.screen_height = client.bottom;
    state.left = x(first);
    state.top = y(first);
    state.right = x(last);
    state.bottom = y(last);
    if (state.screen_width <= 0 || state.screen_height <= 0 || (align != 2 && state.right <= state.left))
        return false;
    std::wstring wide;
    if (!localize(text, wide)) return false;
    const auto formatted = formatted_text.find(text);
    if ((formatted != formatted_text.end() && formatted->second.original == text &&
         formatted->second.rva == 0x5d3cac - image_base) ||
        text == &at<char>(0x5d3cc8) || text == &at<char>(0x5d3cdc) || text == &at<char>(0x5c9b08))
        state.settings.percent = 100; // Native rows and the copyright footer have fixed spacing.
    state.align = align;
    state.font = font;
    state.red = red / 31.0f;
    state.green = green / 31.0f;
    state.blue = blue / 31.0f;
    state.alpha = at<float>(alpha_address);
    state.clip = at<unsigned char>(clip_flag_address) == 0;
    state.clip_left = x(at<uint32_t>(clip_start_address));
    state.clip_top = y(at<uint32_t>(clip_start_address));
    state.clip_right = x(at<uint32_t>(clip_end_address));
    state.clip_bottom = y(at<uint32_t>(clip_end_address));
    Projection projection(state.screen_width, state.screen_height);
    avzh::TextResult result;
    if (align == 0 || (align == 2 && wide.find_first_of(L"|\r\n") != std::wstring::npos)) {
        const int width = std::max(1, state.right - state.left - 6);
        const int step = state.settings.step(font);
        state.align = 2;
        for (auto& line : wrap_chinese(wide, width, step)) {
            const auto drawn = renderer.draw(std::move(line), state);
            result.last_x = drawn.last_x;
            result.last_y = drawn.last_y;
            result.height += step;
            state.top += step;
        }
    } else {
        result = renderer.draw(std::move(wide), state);
    }
    at<short>(line_height_address) = native_short(result.height);
    at<short>(0x6b52ec) = native_short(result.last_x);
    at<short>(0x6b52f0) = native_short(result.last_y);
    return true;
}

void __cdecl layout_hook(void* destination, uint32_t first, uint32_t last, const char* text,
                         short align, short font, short red, short green, short blue) noexcept {
    try {
        if (render(first, last, text, align, font, red, green, blue)) return;
    } catch (const std::exception& error) { log(error.what()); }
    original_layout(destination, first, last, text, align, font, red, green, blue);
}

short __cdecl measure_hook(const char* source) noexcept {
    try {
        std::wstring target;
        if (localize(source, target)) {
            int longest = 0;
            size_t begin = 0;
            for (auto end = target.find_first_of(L"|\r\n"); end != std::wstring::npos;
                 end = target.find_first_of(L"|\r\n", begin)) {
                longest = std::max(longest, renderer.measure(target.substr(begin, end - begin), font_settings.step(12)));
                begin = end + 1;
            }
            longest = std::max(longest, renderer.measure(target.substr(begin), font_settings.step(12)));
            return native_short(longest);
        }
    } catch (const std::exception& error) { log(error.what()); }
    return original_measure(source);
}

BOOL CALLBACK initialize_adapter(PINIT_ONCE, void*, void**) noexcept {
    try {
        wchar_t executable[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        if (!length || length == MAX_PATH) return TRUE;
        const std::wstring path(executable);
        data_path = path.substr(0, path.find_last_of(L"\\/")) + L"\\Avadon3Chinese";
        if (executable_hash(path) != supported_sha) throw std::runtime_error("Unsupported executable");
        game_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        load_registry();
        const auto settings_path = data_path + L"\\font.ini";
        font_settings = avzh::FontSettings::load(settings_path);
        auto* layout = &at<unsigned char>(layout_address);
        auto* measure = &at<unsigned char>(measure_address);
        auto* formatter = &at<unsigned char>(sprintf_address);
        const unsigned char layout_start[] = {0x55,0x8b,0xec,0x81,0xec,0x80,0x08,0,0};
        const unsigned char measure_start[] = {0x55,0x8b,0xec,0x81,0xec,0x30,0x04,0,0};
        const unsigned char sprintf_start[] = {0x8b,0xff,0x55,0x8b,0xec,0x83,0xec,0x20};
        if (memcmp(layout, layout_start, sizeof(layout_start)) || memcmp(measure, measure_start, sizeof(measure_start))
            || memcmp(formatter, sprintf_start, sizeof(sprintf_start)))
            throw std::runtime_error("Unexpected entry prologue");
        MH_STATUS status = MH_Initialize();
        if (status == MH_OK)
            status = MH_CreateHook(layout, reinterpret_cast<void*>(&layout_hook), reinterpret_cast<void**>(&original_layout));
        if (status == MH_OK)
            status = MH_CreateHook(measure, reinterpret_cast<void*>(&measure_hook), reinterpret_cast<void**>(&original_measure));
        if (status == MH_OK)
            status = MH_CreateHook(formatter, reinterpret_cast<void*>(&sprintf_entry), &g_original_sprintf);
#ifdef AV3_ENHANCEMENT
        if (status == MH_OK) install_enhancements();
#endif
        if (status == MH_OK) status = MH_EnableHook(MH_ALL_HOOKS);
        if (status != MH_OK) {
            MH_Uninitialize();
            throw std::runtime_error(MH_StatusToString(status));
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game_base);
        const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS32*>(game_base + dos->e_lfanew);
        try {
            apply_wide_slots(reinterpret_cast<char*>(game_base), pe->OptionalHeader.SizeOfImage);
            log("Wide window text active: " + std::to_string(wide_slots.size()));
        } catch (const std::exception& error) {
            log(std::string("Wide window text fallback: ") + error.what());
        }
        log("Hooks active: " + std::to_string(resource_strings.size()) + " resources, "
            + std::to_string(native_strings.size()) + " native strings");
    } catch (const std::exception& error) { log(std::string("Initialization failed: ") + error.what()); }
    return TRUE;
}

BOOL CALLBACK initialize_system(PINIT_ONCE, void*, void**) noexcept {
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) return FALSE;
    const auto path = std::wstring(system) + L"\\winmm.dll";
    system_winmm = LoadLibraryW(path.c_str());
    if (!system_winmm) return FALSE;
    for (unsigned i = 0; i < AVZH_FORWARD_COUNT; ++i)
        g_forwards[i] = GetProcAddress(system_winmm, avzh_forward_names[i] ? avzh_forward_names[i] :
                                       MAKEINTRESOURCEA(avzh_forward_ordinals[i]));
    return TRUE;
}

DWORD WINAPI bootstrap(void*) noexcept {
    InitOnceExecuteOnce(&adapter_once, initialize_adapter, nullptr, nullptr);
    return 0;
}
}

std::wstring display_argument(const char* argument) {
    std::wstring display;
    if (argument && !localize(argument, display)) display = to_wide(argument, 1252);
    return display;
}

std::wstring damage_type(std::wstring value) {
    static constexpr std::pair<std::wstring_view, std::wstring_view> names[] = {
        {L"Physical", L"物理"}, {L"Energy", L"能量"}, {L"Fire", L"火焰"}, {L"Curse", L"诅咒"},
        {L"Poison", L"毒素"}, {L"Acid", L"酸蚀"}, {L"Cold", L"寒冷"}, {L"Mental", L"精神"}
    };
    for (const auto& name : names) if (value == name.first) return std::wstring(name.second);
    return value;
}

std::wstring compose_display(uintptr_t caller, const char* format, const uintptr_t* arguments, const Entry* entry) {
    const auto pointer = reinterpret_cast<uintptr_t>(format);
    if (pointer >= game_base) {
        if (const auto* label = menu_label(pointer - game_base + image_base, format)) return label;
    }
    const auto string_arg = [&](size_t i) { return reinterpret_cast<const char*>(arguments[i]); };
    if (caller == 0x48c619 && !strcmp(format, "%s")) {
        std::wstring result;
        localize(string_arg(0), result);
        return result;
    }
    if (caller == 0x403433 && !strcmp(format, "%s%s"))
        return display_argument(string_arg(0)) + display_argument(string_arg(1));
    if (caller == 0x48c6b5 && !strcmp(format, "%s%s%s")) {
        const std::string_view prefix(string_arg(0));
        const std::string_view suffix(string_arg(2));
        const auto middle = display_argument(string_arg(1));
        if (prefix == "Places " && suffix == " on ground.") {
            const auto effect = middle == L"Acid" ? L"酸液" : middle == L"Ice" ? L"冰霜" :
                                middle == L"Blessing" ? L"祝福" : damage_type(middle);
            return L"在地面上生成" + effect + L"。";
        }
        if ((prefix == "Start Combat (" || prefix == "End Combat (") && suffix == ")")
            return std::wstring(prefix == "Start Combat (" ? L"开始战斗（" : L"结束战斗（") + middle + L"）";
    }
    if ((caller == 0x48813f || caller == 0x48855f) && !strcmp(format, "%s%s%s%s%s") &&
        !strcmp(string_arg(0), "This book is about ")) {
        std::string notice;
        for (size_t i = 0; i < 5; ++i) notice += string_arg(i);
        return book_notice(notice);
    }
    if (caller == 0x447da2 && !strcmp(format, "%s %s"))
        return display_argument(string_arg(0)) + L" " + display_argument(string_arg(1));
    if (caller == 0x426b92 && !strcmp(format, "%s - %s")) {
        auto key = display_argument(string_arg(1));
        if (key == L"No Shortcut") key = L"无快捷键";
        else if (key == L"Space") key = L"空格";
        return display_argument(string_arg(0)) + L" - " + key;
    }
    if ((caller == 0x422a87 || caller == 0x422d37) && !strcmp(format, "%d. %s")) {
        std::wstring choice;
        if (!localize(string_arg(1), choice)) return {};
        return std::to_wstring(static_cast<int>(arguments[0])) + L". " + choice;
    }
    if (!entry || !entry->format ||
        !(caller == 0x417ceb || caller == 0x417cc1 || (caller >= 0x4033e9 && caller <= 0x4040ee) ||
          (caller >= 0x447c59 && caller <= 0x448d4f))) return {};
    std::wstring result;
    size_t argument = 0;
    for (const auto& part : avzh::format_parts(entry->target)) {
        result += avzh::utf8(part.literal);
        if (part.spec.empty()) continue;
        if (part.spec == "%%") result += L'%';
        else if (part.spec == "%s") {
            auto value = display_argument(string_arg(argument));
            if (argument == 1 && (caller == 0x403875 || caller == 0x4038aa)) value = damage_type(std::move(value));
            result += value;
            ++argument;
        }
        else if (part.spec == "%d") result += std::to_wstring(static_cast<int>(arguments[argument++]));
        else return {};
    }
    return result;
}

extern "C" bool __cdecl begin_sprintf(const char* format, char* output, uintptr_t resume,
                                     const uintptr_t* arguments) noexcept {
    if (!output) return false;
    formatted_text.erase(output);
    const auto address = reinterpret_cast<uintptr_t>(format);
    if (address < game_base || format_depth == format_calls.size()) return false;
    const auto rva = address - game_base;
    const auto entry = native_strings.find(rva);
    const auto* record = entry != native_strings.end() && entry->second.source == format ? &entry->second : nullptr;
    try {
        auto display = compose_display(resume - game_base + image_base, format, arguments, record);
        if (!record && display.empty()) return false;
        format_calls[format_depth++] = {output, record, resume, rva, std::move(display)};
        return true;
    } catch (...) { return false; }
}

extern "C" uintptr_t __cdecl finish_sprintf(int length) noexcept {
    auto call = std::move(format_calls[--format_depth]);
    try {
        if (length > 0 && length <= static_cast<int>(text_limit)) {
            std::string original(call.output, length);
            std::string translation;
            if (call.display.empty() && call.entry) {
                if (call.entry->format) call.entry->format->lookup(original, translation);
                else if (original == call.entry->source) translation = call.entry->target;
            }
            auto display = call.display.empty() ? avzh::utf8(translation) : std::move(call.display);
            if (!display.empty()) {
                if (formatted_text.size() >= 128) formatted_text.clear();
                formatted_text.insert_or_assign(call.output,
                    FormattedText{std::move(original), std::move(display), call.rva});
            }
        }
    } catch (...) {}
    return call.resume;
}

// Call the game's variadic formatter with its original stack and arguments.
// Only observed translations are kept for the display hooks, never written to the game buffer.
extern "C" __declspec(naked) void sprintf_return() {
    __asm {
        lea esp, [esp-4]
        pushfd
        pushad
        push eax
        call finish_sprintf
        add esp, 4
        mov [esp+36], eax
        popad
        popfd
        ret
    }
}

extern "C" __declspec(naked) void sprintf_entry() {
    __asm {
        pushfd
        pushad
        lea ebx, [esp+48]
        mov eax, [esp+44]
        mov edx, [esp+40]
        mov ecx, [esp+36]
        push ebx
        push ecx
        push edx
        push eax
        call begin_sprintf
        add esp, 16
        test al, al
        jz forward_original
        mov dword ptr [esp+36], offset sprintf_return
    forward_original:
        popad
        popfd
        jmp dword ptr [g_original_sprintf]
    }
}

extern "C" void __cdecl ResolveForward(unsigned index) noexcept {
    if (!InitOnceExecuteOnce(&real_once, initialize_system, nullptr, nullptr) ||
        index >= AVZH_FORWARD_COUNT || !g_forwards[index]) ExitProcess(ERROR_PROC_NOT_FOUND);
}

extern "C" DWORD WINAPI ProxyClock() {
    ResolveForward(AVZH_CLOCK_INDEX);
    InitOnceExecuteOnce(&adapter_once, initialize_adapter, nullptr, nullptr);
    return reinterpret_cast<DWORD(WINAPI*)()>(g_forwards[AVZH_CLOCK_INDEX])();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        const auto thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

#include "forward_stubs.inc"

#ifdef AV3_OFFLINE_TEST
#include <cstdarg>
#include <cstdio>

int __cdecl fixture_formatter(char* output, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(output, 128, format, arguments);
    va_end(arguments);
    return length;
}

int wmain(int count, wchar_t** arguments) {
    if (count != 2) return 1;
    data_path = arguments[1];
    load_registry();
    data_path.clear();
    using Format = int(__cdecl*)(char*, const char*, ...);
    g_original_sprintf = reinterpret_cast<void*>(&fixture_formatter);
    const auto format = reinterpret_cast<Format>(&sprintf_entry);
    std::wstring result;
    char output[128]{};
    native_strings.clear();
    resource_strings.clear();
    constexpr char template_text[] = "Welcome %s (%d)";
    constexpr char literal[] = "Hard";
    game_base = reinterpret_cast<uintptr_t>(template_text) & ~uintptr_t(0xfffff);
    Entry translated{template_text, "欢迎%s（%d）", std::make_unique<avzh::Dictionary>()};
    translated.format->add(translated.source, translated.target);
    translated.format->prepare();
    native_strings.emplace(reinterpret_cast<uintptr_t>(template_text) - game_base, std::move(translated));
    native_strings.emplace(reinterpret_cast<uintptr_t>(literal) - game_base, Entry{literal, "困难", nullptr});
    resource_strings.emplace(1, "测试角色");
    if (format(output, template_text, "~001~", 3) != 17 || strcmp(output, "Welcome ~001~ (3)")) return 3;
    if (!localize(output, result) || result != L"欢迎测试角色（3）" || format_depth) return 4;
    if (format(output, literal) != 4 || strcmp(output, literal) || !localize(output, result) || result != L"困难") return 5;
    const std::string other_literal("Hard");
    if (format(output, other_literal.c_str()) != 4 || localize(output, result)) return 6;
    static constexpr char difficulty[] = "Hard";
    game_base = reinterpret_cast<uintptr_t>(difficulty) - (0x5d20f8 - image_base);
    if (format(output, difficulty) != 4 || strcmp(output, difficulty) ||
        !localize(output, result) || result != L"困难") return 29;
    static constexpr char gender[] = "Male";
    game_base = reinterpret_cast<uintptr_t>(gender) - (0x5d2adc - image_base);
    if (!localize(gender, result) || result != L"男性" || menu_label(0x5d2adc, "Female")) return 30;
    const wchar_t image[] = L"#Text Files (*.TXT)\0*.txt\0All Files (*.*)\0*.*\0";
    const wchar_t expected_label[] = L"Text Files (*.TXT)";
    auto* page = static_cast<char*>(VirtualAlloc(nullptr, sizeof(image), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!page) return 7;
    memcpy(page, image, sizeof(image));
    wide_slots = {{2, std::string(reinterpret_cast<const char*>(expected_label), sizeof(expected_label) - 2),
                   L"文本文件 (*.TXT)"}};
    wide_slots[0].replacement.resize(wide_slots[0].expected.size() / 2, L' ');
    DWORD previous = 0;
    if (!VirtualProtect(page, sizeof(image), PAGE_READONLY, &previous)) return 9;
    apply_wide_slots(page, sizeof(image));
    const auto suffix = 2 + wide_slots[0].expected.size();
    MEMORY_BASIC_INFORMATION region{};
    const bool verified = !memcmp(page + 2, wide_slots[0].replacement.data(), wide_slots[0].expected.size()) &&
        !memcmp(page + suffix, reinterpret_cast<const char*>(image) + suffix, sizeof(image) - suffix) &&
        VirtualQuery(page, &region, sizeof(region)) && region.Protect == PAGE_READONLY;
    VirtualFree(page, 0, MEM_RELEASE);
    if (!verified) return 10;
    if (avzh::FontSettings{100,true}.step(12) != 13 ||
        avzh::FontSettings{150,true}.step(12) != 19 || avzh::FontSettings{200,false}.step(12) != 25) return 11;
    resource_strings[2] = "他说。_你好。_";
    std::wstring quotation;
    if (!resolve_markers(L"~002~", quotation) || quotation != L"他说。“你好。”") return 16;
    constexpr char inventory_format[] = "%s Inventory:";
    Entry inventory{inventory_format, "%s物品栏：", std::make_unique<avzh::Dictionary>()};
    const uintptr_t inventory_arguments[] = {reinterpret_cast<uintptr_t>("Raidon")};
    if (compose_display(0x417ceb, inventory_format, inventory_arguments, &inventory) != L"Raidon物品栏：") return 17;
    constexpr char armor_format[] = "Armor: +%d%%";
    Entry armor{armor_format, "护甲：+%d%%", std::make_unique<avzh::Dictionary>()};
    const uintptr_t armor_arguments[] = {3};
    if (compose_display(0x4480f4, armor_format, armor_arguments, &armor) != L"护甲：+3%") return 18;
    native_strings.emplace(0x1cbd90, Entry{"Tutorial fixture", "教程示例", nullptr});
    if (!copied_text(0x6b4ee0, "Tutorial fixture") || copied_text(0x6b4ee1, "Tutorial fixture")) return 19;
    const uintptr_t shortcut_arguments[] = {reinterpret_cast<uintptr_t>("Journal"), reinterpret_cast<uintptr_t>("No Shortcut")};
    if (compose_display(0x426b92, "%s - %s", shortcut_arguments, nullptr) != L"Journal - 无快捷键") return 20;
    Entry damage{"%s %s damage.%s", "%s%s伤害。%s", std::make_unique<avzh::Dictionary>()};
    const uintptr_t damage_arguments[] = {reinterpret_cast<uintptr_t>("7-21"),
        reinterpret_cast<uintptr_t>("Physical"), reinterpret_cast<uintptr_t>("")};
    if (compose_display(0x4038aa, damage.source.c_str(), damage_arguments, &damage) != L"7-21物理伤害。") return 21;
    const uintptr_t ground_arguments[] = {reinterpret_cast<uintptr_t>("Places "),
        reinterpret_cast<uintptr_t>("Acid"), reinterpret_cast<uintptr_t>(" on ground.")};
    if (compose_display(0x48c6b5, "%s%s%s", ground_arguments, nullptr) != L"在地面上生成酸液。") return 22;
    const uintptr_t combat_arguments[] = {reinterpret_cast<uintptr_t>("Start Combat ("),
        reinterpret_cast<uintptr_t>("f"), reinterpret_cast<uintptr_t>(")")};
    if (compose_display(0x48c6b5, "%s%s%s", combat_arguments, nullptr) != L"开始战斗（f）") return 23;
    native_strings.emplace(0x1cb8a8, Entry{"This book is about ", "这本书讲的是 ", nullptr});
    native_strings.emplace(0x1cb8d0, Entry{". This entry has been added to your codex (which can be read in the Journal).",
        "。这条内容已添加到你的法典中（可在日志中阅读）。", nullptr});
    resource_strings[3] = "凯梅里亚";
    const auto notice = book_notice("This book is about ~003~. This entry has been added to your codex (which can be read in the Journal).");
    std::wstring translated_notice;
    if (!resolve_markers(notice, translated_notice) ||
        translated_notice != L"这本书讲的是凯梅里亚。这条内容已添加到你的法典中（可在日志中阅读）。") return 24;
#ifdef AV3_ENHANCEMENT
    if (scaled_speed(24, 100) != 24 || scaled_speed(24, 175) != 42 ||
        scaled_speed(24, 200) != 48 || scaled_speed(20000, 200) != 32767 ||
        scaled_speed(-1, 200) != -1) return 12;
    avzh::CameraFollow follow;
    avzh::CameraFrame frame;
    frame.allowed = true;
    frame.mode = 10;
    frame.actors[0] = {true, 256, 256, 1};
    if (follow.update(frame).follow) return 13;
    frame.actors[0].x += 8;
    if (!follow.update(frame).follow) return 14;
    frame.mode = 15;
    frame.preferred = 2;
    frame.actors[0].x += 8;
    if (follow.update(frame).follow) return 15;
#endif
    return 0;
}
#endif
