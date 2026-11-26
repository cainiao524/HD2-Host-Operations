#ifndef UNICODE
#define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "overlay.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "effects.h"
#include "i18n.h"

enum { STATUS_LENGTH = 320 };

static SRWLOCK menu_lock = SRWLOCK_INIT;
static volatile LONG shutdown_requested;
static struct {
    bool want_visible;
    bool enabled;
    bool pending;
    MenuCommand command;
    OperationPreset request;
    wchar_t status[STATUS_LENGTH];
} menu;

static bool stopping(void)
{
    return InterlockedCompareExchange(&shutdown_requested, 0, 0) != 0;
}

static void copy_status_locked(const wchar_t *status)
{
    size_t length = 0;
    if (status) {
        while (length < STATUS_LENGTH - 1 && status[length])
            ++length;
    }
    if (length)
        wmemcpy(menu.status, status, length);
    menu.status[length] = L'\0';
}

void host_ui_closed(void)
{
    AcquireSRWLockExclusive(&menu_lock);
    menu.want_visible = false;
    ReleaseSRWLockExclusive(&menu_lock);
}

bool menu_toggle(void)
{
    if (stopping()) return false;
    AcquireSRWLockExclusive(&menu_lock);
    bool visible = !menu.want_visible;
    menu.want_visible = visible;
    ReleaseSRWLockExclusive(&menu_lock);
    overlay_set_visible(visible);
    return true;
}

void menu_hide(void)
{
    host_ui_closed();
    overlay_set_visible(false);
}

void menu_shutdown(void)
{
    /* HF_EVENT_SESSION_END can run under the loader lock after other threads are gone. */
    InterlockedExchange(&shutdown_requested, 1);
    overlay_stop();
}

bool menu_take_request(MenuCommand *command, OperationPreset *preset)
{
    if (!command || !preset || stopping())
        return false;
    AcquireSRWLockExclusive(&menu_lock);
    bool ready = menu.pending && menu.enabled && !stopping();
    if (ready) {
        *command = menu.command;
        *preset = menu.request;
        menu.pending = false;
    }
    ReleaseSRWLockExclusive(&menu_lock);
    return ready;
}

void menu_report(const wchar_t *status)
{
    if (stopping())
        return;
    AcquireSRWLockExclusive(&menu_lock);
    copy_status_locked(status);
    ReleaseSRWLockExclusive(&menu_lock);
}

void menu_set_enabled(bool enabled)
{
    if (stopping())
        return;
    AcquireSRWLockExclusive(&menu_lock);
    menu.enabled = enabled;
    if (!enabled)
        menu.pending = false;
    ReleaseSRWLockExclusive(&menu_lock);
}

static const PlanetEntry *find_planet(uint32_t index)
{
    if (index != HOST_OPERATIONS_NO_INDEX) {
        for (size_t i = 0; i < planet_count; ++i)
            if (planets[i].index == index)
                return &planets[i];
    }
    return NULL;
}

static bool known_faction(uint32_t faction)
{
    return faction == 0 || (faction >= 2 && faction <= 4);
}

static bool valid_preset(const OperationPreset *preset)
{
    /* The difficulty field is kept in the file for old presets but never checked or used. */
    if (!find_planet(preset->planet) ||
        (preset->region != HOST_OPERATIONS_NO_INDEX && preset->region >= 8) ||
        !known_faction(preset->faction) || preset->effect_count > HOST_OPERATIONS_MAX_EFFECTS ||
        (preset->template_index != HOST_OPERATIONS_NO_TEMPLATE && preset->template_index >= HOST_OPERATIONS_TEMPLATES))
        return false;

    for (uint32_t i = 0; i < HOST_OPERATIONS_MISSION_SLOTS; ++i) {
        if (!preset->missions[i]) continue;
        bool known = false;
        for (size_t m = 0; m < mission_catalog_count && !known; ++m) known = mission_catalog[m].id == preset->missions[i];
        if (!known) return false;
    }

    /* Any settingsHash (a planet's or a template's) and any effect ID may be used; only duplicates are refused. */
    for (uint32_t i = 0; i < preset->effect_count; ++i) {
        if (!preset->effects[i] || effect_position(preset->effects, i, preset->effects[i]) < i)
            return false;
    }
    return true;
}

static const wchar_t *skip_space(const wchar_t *text)
{
    while (*text == L' ' || *text == L'\t')
        ++text;
    return text;
}

static const wchar_t *parse_uint32(const wchar_t *text, uint32_t *out)
{
    text = skip_space(text);
    uint32_t base = 10;
    if (text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) {
        base = 16;
        text += 2;
    }
    const wchar_t *start = text;
    uint32_t value = 0;
    for (; *text; ++text) {
        uint32_t digit;
        if (*text >= L'0' && *text <= L'9')
            digit = (uint32_t)(*text - L'0');
        else if (*text >= L'a' && *text <= L'f')
            digit = (uint32_t)(*text - L'a' + 10);
        else if (*text >= L'A' && *text <= L'F')
            digit = (uint32_t)(*text - L'A' + 10);
        else
            break;
        if (digit >= base || value > (UINT32_MAX - digit) / base)
            return NULL;
        value = value * base + digit;
    }
    if (text == start)
        return NULL;
    *out = value;
    return skip_space(text);
}

static bool uint32_value(const wchar_t *text, uint32_t *out)
{
    const wchar_t *end = parse_uint32(text, out);
    return end && *end == L'\0';
}

/* Single versioned record. Main validates native support before saving or applying it. */
static SRWLOCK preset_io_lock = SRWLOCK_INIT;
enum { PRESET_FILE_LIMIT = 1024 };

static bool ensure_directory(const wchar_t *path)
{
    if (CreateDirectoryW(path, NULL))
        return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool preset_path(wchar_t path[MAX_PATH], wchar_t *temporary)
{
    static const wchar_t parent[] = L"\\HellForge";
    static const wchar_t directory[] = L"\\HostOperationsTest";
    static const wchar_t filename[] = L"\\presets.ini";
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    if (!((n >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
          (n >= 2 && path[0] == L'\\' && path[1] == L'\\')))
        return false;
    while (n && (path[n - 1] == L'\\' || path[n - 1] == L'/'))
        --n;
    if (n + ARRAYSIZE(parent) + ARRAYSIZE(directory) + ARRAYSIZE(filename) - 2 > MAX_PATH)
        return false;
    wcscpy(path + n, parent);
    if (temporary && !ensure_directory(path))
        return false;
    wcscpy(path + n + ARRAYSIZE(parent) - 1, directory);
    if (temporary && (!ensure_directory(path) || !GetTempFileNameW(path, L"HFO", 0, temporary)))
        return false;
    wcscat(path, filename);
    return true;
}

static bool parse_effects(const wchar_t *text, OperationPreset *preset)
{
    if (!*skip_space(text))
        return true;
    do {
        if (preset->effect_count == HOST_OPERATIONS_MAX_EFFECTS)
            return false;
        text = parse_uint32(text, &preset->effects[preset->effect_count]);
        if (!text)
            return false;
        ++preset->effect_count;
        if (!*text)
            return true;
    } while (*text++ == L',');
    return false;
}

/* Version 1 stored the first ID as a key for each complete variant group. */
static bool expand_legacy_groups(OperationPreset *preset)
{
    OperationPreset expanded = *preset;
    expanded.effect_count = 0;
    for (uint32_t i = 0; i < preset->effect_count; ++i) {
        size_t group = 0;
        while (group < variant_count && variants[group].ids[0] != preset->effects[i])
            ++group;
        if (group == variant_count)
            return false;
        const VariantEntry *variant = &variants[group];
        for (size_t j = 0; j < ARRAYSIZE(variant->ids) && variant->ids[j]; ++j)
            if (!append_effect_id(expanded.effects, &expanded.effect_count, variant->ids[j]))
                return false;
    }
    *preset = expanded;
    return true;
}

static bool parse_preset(wchar_t *text, OperationPreset *out)
{
    OperationPreset preset = {.template_index = HOST_OPERATIONS_NO_TEMPLATE};
    uint32_t version = 0;
    const struct { const wchar_t *key; uint32_t *value; } fields[] = {
        {L"version", &version}, {L"planet", &preset.planet}, {L"region", &preset.region},
        {L"difficulty", &preset.difficulty}, {L"operation_template", &preset.operation_template},
        {L"biome", &preset.biome}, {L"faction", &preset.faction}, {L"effects", NULL},
        /* version 3 */
        {L"template_index", &preset.template_index}, {L"seed", &preset.seed},
        /* optional (test15) */
        {L"mission1", &preset.missions[0]}, {L"mission2", &preset.missions[1]}, {L"mission3", &preset.missions[2]},
    };
    const unsigned base_fields = (1u << 8) - 1;
    unsigned seen = 0;
    bool header = false;
    if (*text == 0xFEFF)
        ++text;
    while (*text) {
        wchar_t *line = text;
        while (*text && *text != L'\n')
            ++text;
        wchar_t *end = text;
        if (*text)
            ++text;
        while (end > line && (end[-1] == L' ' || end[-1] == L'\t' || end[-1] == L'\r'))
            --end;
        *end = L'\0';
        line = (wchar_t *)skip_space(line);
        if (!*line || *line == L';' || *line == L'#')
            continue;
        if (!header) {
            if (wcscmp(line, L"[HostOperations]") != 0)
                return false;
            header = true;
            continue;
        }
        wchar_t *equal = wcschr(line, L'=');
        if (!equal)
            return false;
        wchar_t *key_end = equal;
        while (key_end > line && (key_end[-1] == L' ' || key_end[-1] == L'\t'))
            --key_end;
        *key_end = L'\0';
        size_t i = 0;
        while (i < ARRAYSIZE(fields) && wcscmp(line, fields[i].key) != 0)
            ++i;
        if (i == ARRAYSIZE(fields) || (seen & (1u << i)))
            return false;
        seen |= 1u << i;
        if (fields[i].value) {
            if (!uint32_value(equal + 1, fields[i].value))
                return false;
        } else if (!parse_effects(equal + 1, &preset)) {
            return false;
        }
    }
    /* These fields were controls in test5. War-table selection owns them now. */
    preset.region = HOST_OPERATIONS_NO_INDEX;
    preset.operation_template = 0;
    preset.difficulty = 0;
    preset.seed = 0;
    const unsigned optional = 7u << 10;
    const unsigned required = version == 3 ? ((1u << ARRAYSIZE(fields)) - 1) & ~optional : base_fields;
    if ((seen & ~optional) != required || version < 1 || version > 3 ||
        (version == 1 && !expand_legacy_groups(&preset)) || !valid_preset(&preset))
        return false;
    *out = preset;
    return true;
}

static bool read_preset(OperationPreset *preset)
{
    wchar_t path[MAX_PATH];
    if (!preset_path(path, NULL))
        return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    char bytes[PRESET_FILE_LIMIT];
    DWORD size = 0;
    BOOL read_ok = ReadFile(file, bytes, sizeof bytes, &size, NULL);
    CloseHandle(file);
    if (!read_ok || !size || size >= sizeof bytes || memchr(bytes, 0, size))
        return false;
    wchar_t ini[PRESET_FILE_LIMIT];
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)size, ini, ARRAYSIZE(ini));
    if (!length)
        return false;
    ini[length] = L'\0';
    return parse_preset(ini, preset);
}

static bool publish_preset(const wchar_t *temporary, const wchar_t *path)
{
#if defined(FILE_RENAME_FLAG_POSIX_SEMANTICS) && NTDDI_VERSION >= NTDDI_WIN10_RS1
    /* A reader with the previous snapshot open can finish reading that snapshot. */
    HANDLE file = CreateFileW(temporary, DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    union {
        FILE_RENAME_INFO info;
        unsigned char data[offsetof(FILE_RENAME_INFO, FileName) + MAX_PATH * sizeof(wchar_t)];
    } rename = {0};
    const size_t name_offset = offsetof(FILE_RENAME_INFO, FileName);
    size_t name_bytes = wcslen(path) * sizeof(wchar_t);
    rename.info.Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename.info.FileNameLength = (DWORD)name_bytes;
    memcpy(rename.data + name_offset, path, name_bytes + sizeof(wchar_t));
    bool renamed = SetFileInformationByHandle(file, FileRenameInfoEx, &rename,
                                              (DWORD)(name_offset + name_bytes + sizeof(wchar_t))) != 0;
    DWORD error = renamed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (renamed)
        return true;
    if (error != ERROR_INVALID_FUNCTION && error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED)
        return false;
#endif
    return MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

static bool write_preset(const OperationPreset *preset)
{
    char bytes[PRESET_FILE_LIMIT];
    int length = snprintf(bytes, sizeof bytes,
        "[HostOperations]\r\nversion=3\r\nplanet=%" PRIu32 "\r\nregion=%" PRIu32
        "\r\ndifficulty=%" PRIu32 "\r\noperation_template=%" PRIu32 "\r\nbiome=%" PRIu32
        "\r\nfaction=%" PRIu32 "\r\ntemplate_index=%" PRIu32 "\r\nseed=%" PRIu32
        "\r\nmission1=%" PRIu32 "\r\nmission2=%" PRIu32 "\r\nmission3=%" PRIu32 "\r\neffects=",
        preset->planet, preset->region, preset->difficulty, preset->operation_template,
        preset->biome, preset->faction, preset->template_index, preset->seed,
        preset->missions[0], preset->missions[1], preset->missions[2]);
    if (length < 0 || (size_t)length >= sizeof bytes)
        return false;
    for (uint32_t i = 0; i < preset->effect_count; ++i) {
        size_t available = sizeof bytes - (size_t)length;
        int written = snprintf(bytes + length, available, "%s%" PRIu32, i ? "," : "", preset->effects[i]);
        if (written < 0 || (size_t)written >= available)
            return false;
        length += written;
    }
    if ((size_t)length + 2 >= sizeof bytes)
        return false;
    bytes[length++] = '\r';
    bytes[length++] = '\n';

    wchar_t path[MAX_PATH], temporary[MAX_PATH];
    if (!preset_path(path, temporary))
        return false;
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    bool saved = false;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        bool ready = WriteFile(file, bytes, (DWORD)length, &written, NULL) &&
                     written == (DWORD)length && FlushFileBuffers(file);
        if (CloseHandle(file) && ready)
            saved = publish_preset(temporary, path);
    }
    if (!saved)
        DeleteFileW(temporary);
    return saved;
}

/* Sync code layout (little endian): version 1, planet u16, biome u32, faction u8, template u8 (0xFF: the game's own),
 * missions 3 x u32, effect count u8, effects u32 each, then CRC-16/CCITT of all of that. */
enum { CODE_VERSION = 1, CODE_BYTES = 1 + 2 + 4 + 1 + 1 + 4 * HOST_OPERATIONS_MISSION_SLOTS + 1 + 4 * HOST_OPERATIONS_MAX_EFFECTS + 2 };
static const char code_digits[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
static const char code_prefix[] = "HF1-";

static uint16_t code_crc(const unsigned char *data, size_t size)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static void put_u32(unsigned char *at, uint32_t value)
{
    for (int i = 0; i < 4; ++i) at[i] = (unsigned char)(value >> (8 * i));
}

static uint32_t get_u32(const unsigned char *at)
{
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

bool menu_preset_code(const OperationPreset *preset, char *out, size_t capacity)
{
    if (!preset || !out || !valid_preset(preset) || preset->planet > 0xFFFF) return false;
    unsigned char bytes[CODE_BYTES];
    size_t n = 0;
    bytes[n++] = CODE_VERSION;
    bytes[n++] = (unsigned char)preset->planet;
    bytes[n++] = (unsigned char)(preset->planet >> 8);
    put_u32(bytes + n, preset->biome); n += 4;
    bytes[n++] = (unsigned char)preset->faction;
    bytes[n++] = preset->template_index == HOST_OPERATIONS_NO_TEMPLATE ? 0xFF : (unsigned char)preset->template_index;
    for (uint32_t i = 0; i < HOST_OPERATIONS_MISSION_SLOTS; ++i) { put_u32(bytes + n, preset->missions[i]); n += 4; }
    bytes[n++] = (unsigned char)preset->effect_count;
    for (uint32_t i = 0; i < preset->effect_count; ++i) { put_u32(bytes + n, preset->effects[i]); n += 4; }
    const uint16_t crc = code_crc(bytes, n);
    bytes[n++] = (unsigned char)crc;
    bytes[n++] = (unsigned char)(crc >> 8);
    const size_t digits = (n * 8 + 4) / 5;
    if (capacity < sizeof code_prefix + digits) return false;
    memcpy(out, code_prefix, sizeof code_prefix - 1);
    char *at = out + sizeof code_prefix - 1;
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        buffer = buffer << 8 | bytes[i];
        bits += 8;
        while (bits >= 5) { *at++ = code_digits[(buffer >> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits) *at++ = code_digits[(buffer << (5 - bits)) & 31];
    *at = 0;
    return true;
}

static int code_digit(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    const char *at = c ? strchr(code_digits, c) : NULL;
    return at ? (int)(at - code_digits) : -1;
}

bool menu_parse_code(const char *text, OperationPreset *out)
{
    if (!text || !out) return false;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    if (_strnicmp(text, code_prefix, sizeof code_prefix - 1) != 0) return false;
    text += sizeof code_prefix - 1;
    unsigned char bytes[CODE_BYTES + 1];
    size_t n = 0;
    uint32_t buffer = 0;
    int bits = 0;
    for (; *text; ++text) {
        if (*text == '-' || *text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') continue;
        const int digit = code_digit(*text);
        if (digit < 0) return false;
        buffer = buffer << 5 | (uint32_t)digit;
        bits += 5;
        if (bits >= 8) {
            if (n == sizeof bytes) return false;
            bytes[n++] = (unsigned char)(buffer >> (bits - 8));
            bits -= 8;
        }
    }
    const size_t fixed = 1 + 2 + 4 + 1 + 1 + 4 * HOST_OPERATIONS_MISSION_SLOTS + 1;
    if (n < fixed + 2 || bytes[0] != CODE_VERSION) return false;
    const uint32_t effects = bytes[fixed - 1];
    if (effects > HOST_OPERATIONS_MAX_EFFECTS || n < fixed + 4 * effects + 2) return false;
    const size_t body = fixed + 4 * effects;
    if (code_crc(bytes, body) != (uint16_t)(bytes[body] | bytes[body + 1] << 8)) return false;
    OperationPreset preset = {0};
    size_t at = 1;
    preset.planet = (uint32_t)bytes[at] | (uint32_t)bytes[at + 1] << 8; at += 2;
    preset.region = HOST_OPERATIONS_NO_INDEX;
    preset.biome = get_u32(bytes + at); at += 4;
    preset.faction = bytes[at++];
    preset.template_index = bytes[at] == 0xFF ? HOST_OPERATIONS_NO_TEMPLATE : bytes[at];
    ++at;
    for (uint32_t i = 0; i < HOST_OPERATIONS_MISSION_SLOTS; ++i) { preset.missions[i] = get_u32(bytes + at); at += 4; }
    preset.effect_count = bytes[at++];
    for (uint32_t i = 0; i < preset.effect_count; ++i) { preset.effects[i] = get_u32(bytes + at); at += 4; }
    if (!valid_preset(&preset)) return false;
    *out = preset;
    return true;
}

bool menu_load_saved(OperationPreset *preset)
{
    return preset && !stopping() && read_preset(preset);
}

bool menu_save_preset(const OperationPreset *preset)
{
    if (!preset || stopping() || !valid_preset(preset) || !TryAcquireSRWLockExclusive(&preset_io_lock))
        return false;
    bool saved = write_preset(preset);
    ReleaseSRWLockExclusive(&preset_io_lock);
    return saved;
}

bool menu_clear_saved(void)
{
    if (stopping() || !TryAcquireSRWLockExclusive(&preset_io_lock))
        return false;
    bool cleared = false;
    wchar_t path[MAX_PATH];
    if (preset_path(path, NULL)) {
        cleared = DeleteFileW(path) != 0;
        if (!cleared) {
            DWORD error = GetLastError();
            cleared = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
    }
    ReleaseSRWLockExclusive(&preset_io_lock);
    return cleared;
}

static bool language_path(wchar_t path[MAX_PATH])
{
    if (!preset_path(path, NULL)) return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash || (size_t)(slash - path) + 14 >= MAX_PATH) return false;
    wcscpy(slash, L"\\language.txt");
    return true;
}

void menu_load_language(void)
{
    wchar_t path[MAX_PATH];
    char text[8] = {0};
    if (!language_path(path)) return;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD size = 0;
    ReadFile(file, text, sizeof text - 1, &size, NULL);
    CloseHandle(file);
    if (!strncmp(text, "en", 2)) set_ui_language(LANG_EN);
    else if (!strncmp(text, "hans", 4)) set_ui_language(LANG_ZH_HANS);
    else if (!strncmp(text, "zh", 2)) set_ui_language(LANG_ZH);
}

void menu_save_language(void)
{
    wchar_t path[MAX_PATH], directory[MAX_PATH];
    if (!language_path(path)) return;
    wcscpy(directory, path);
    wchar_t *slash = wcsrchr(directory, L'\\');
    if (slash) *slash = 0;
    wchar_t *parent = wcsrchr(directory, L'\\');
    if (parent) { *parent = 0; ensure_directory(directory); *parent = L'\\'; }
    ensure_directory(directory);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const char *code = ui_language() == LANG_EN ? "en" : ui_language() == LANG_ZH_HANS ? "hans" : "zh";
    WriteFile(file, code, (DWORD)strlen(code), &written, NULL);
    CloseHandle(file);
}


void host_ui_status(wchar_t *status, size_t capacity, bool *editable, bool *pending)
{
    AcquireSRWLockShared(&menu_lock);
    if (status && capacity) {
        size_t length = wcslen(menu.status);
        if (length >= capacity) length = capacity - 1;
        wmemcpy(status, menu.status, length);
        status[length] = 0;
    }
    *editable = menu.enabled && !stopping();
    *pending = menu.pending;
    ReleaseSRWLockShared(&menu_lock);
}

bool host_ui_submit(MenuCommand command, const OperationPreset *preset)
{
    if (command != MENU_CLOSE_PLANET && command != MENU_OPEN_TEMPORARILY && command != MENU_SAVE_AND_OPEN)
        return false;
    if (command != MENU_CLOSE_PLANET && (!preset || !valid_preset(preset))) {
        menu_report(tr(T_OP_CHECK));
        return false;
    }
    AcquireSRWLockExclusive(&menu_lock);
    bool accepted = !stopping() && menu.enabled && !menu.pending;
    if (accepted) {
        menu.command = command;
        if (preset) menu.request = *preset;
        menu.pending = true;
        copy_status_locked(tr(T_SUBMIT_SENT));
    }
    ReleaseSRWLockExclusive(&menu_lock);
    return accepted;
}
