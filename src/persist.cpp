#include "persist.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace persist {

namespace {

// Resolve the plugin's own directory (where the stored file lives).
std::wstring GetPluginDir()
{
    wchar_t path[MAX_PATH] = {0};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&GetPluginDir, &self);
    if (!self) return L"";
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    wchar_t* fslash = wcsrchr(path, L'/');
    wchar_t* last = (slash && (!fslash || slash > fslash)) ? slash : fslash;
    if (last) last[1] = L'\0';
    return std::wstring(path);
}

std::wstring FilePath()
{
    return GetPluginDir() + L"last_positions.json";
}

// Minimal URL-escape for player names in the JSON key (names are simple,
// but this guards against quotes/braces in a player-chosen name).
std::string EscName(const char* name)
{
    std::string out;
    for (const char* p = name; *p; ++p)
    {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += static_cast<char>(c);
        }
        else
        {
            out += static_cast<char>(c);
        }
    }
    return out;
}

} // namespace

bool LoadLastPosition(const char* name, float& x, float& y, float& z, float& heading)
{
    FILE* f = _wfopen(FilePath().c_str(), L"r");
    if (!f) return false;

    bool found = false;
    char line[512];
    // Very small manual scan for: "<escaped name>" : { "x": ..., "y": ..., "z": ..., "heading": ... }
    std::string key = "\"" + EscName(name) + "\"";
    while (fgets(line, sizeof(line), f))
    {
        if (strstr(line, key.c_str()))
        {
            // Read the remainder of this record (the object body).
            std::string blob = line;
            char buf[512];
            while (blob.find('}') == std::string::npos && fgets(buf, sizeof(buf), f))
            {
                blob += buf;
                if (blob.size() > 8000) break;
            }

            auto findFloat = [&blob](const char* field, float& out) {
                const char* p = strstr(blob.c_str(), field);
                if (!p) return false;
                p = strchr(p, ':');
                if (!p) return false;
                out = static_cast<float>(atof(p + 1));
                return true;
            };

            if (findFloat("\"x\"", x) && findFloat("\"y\"", y) &&
                findFloat("\"z\"", z) && findFloat("\"heading\"", heading))
            {
                found = true;
            }
            break;
        }
    }
    fclose(f);
    return found;
}

void SaveLastPosition(const char* name, float x, float y, float z, float heading)
{
    // Read existing file to preserve other players.
    bool existing = false;
    std::string content;
    {
        FILE* f = _wfopen(FilePath().c_str(), L"r");
        if (f)
        {
            char buf[1024];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                content.append(buf, n);
            fclose(f);
            existing = !content.empty();
        }
    }

    std::string key = "\"" + EscName(name) + "\"";
    char entry[512];
    snprintf(entry, sizeof(entry),
             "{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"heading\":%.2f}",
             x, y, z, heading);
    std::string newRec = key + ":" + entry;

    FILE* f = _wfopen(FilePath().c_str(), L"w");
    if (!f) return;

    fputs("{\"players\":{", f);
    if (existing && content.find(key) == std::string::npos)
    {
        // Trim leading "{ \"players\":{ " and trailing "} }" if present.
        std::string inner = content;
        size_t brace = inner.find('{');
        size_t closing = inner.rfind('}');
        if (brace != std::string::npos && closing > brace + 1)
            inner = inner.substr(brace + 1, closing - brace - 1);
        // cut the "players":{" prefix
        size_t p = inner.find('{');
        size_t colon = inner.find(':');
        if (p != std::string::npos && colon > p)
            inner = inner.substr(colon + 1);
        // strip surrounding whitespace
        size_t a = inner.find_first_not_of(" \t\r\n");
        size_t b = inner.find_last_not_of(" \t\r\n");
        if (a != std::string::npos && b >= a)
            inner = inner.substr(a, b - a + 1);
        if (!inner.empty() && inner != "\"players\":")
        {
            fputs(inner.c_str(), f);
            fputs(",\n", f);
        }
    }
    else if (existing)
    {
        // Replace the value for this key: simplest is to keep last-known
        // simple behaviour — we rewrite the whole map fresh each time.
        // For a name that exists, we just emit the (only) record below.
    }

    fputs(newRec.c_str(), f);
    fputs("}}", f);
    fclose(f);
}

} // namespace persist