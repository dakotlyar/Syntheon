// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/vulkan/layer_manifests.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

namespace sb::gpu::vk {

namespace {

// A just-enough JSON value (the manifests are small): objects, arrays and strings; numbers,
// booleans and null are skipped.
struct Json {
    enum class Kind { None, String, Object, Array } kind = Kind::None;
    std::string text;
    std::map<std::string, Json> members;
    std::vector<Json> items;
};

class Parser
{
public:
    explicit Parser(const std::string &s) : m_s(s) {}
    bool parse(Json *out)
    {
        if (!value(out, 0))
            return false;
        skip();
        return m_i == m_s.size();
    }

private:
    void skip()
    {
        while (m_i < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_i])))
            ++m_i;
    }
    bool string(std::string *out)
    {
        if (m_i >= m_s.size() || m_s[m_i] != '"')
            return false;
        ++m_i;
        std::string r;
        while (m_i < m_s.size() && m_s[m_i] != '"') {
            char c = m_s[m_i++];
            if (c == '\\' && m_i < m_s.size()) {
                const char e = m_s[m_i++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'u': m_i = std::min(m_s.size(), m_i + 4); c = '?'; break;  // not needed for paths
                default: c = e; break;  // \\ \" \/
                }
            }
            r += c;
        }
        if (m_i >= m_s.size())
            return false;
        ++m_i;
        *out = r;
        return true;
    }
    bool value(Json *out, int depth)
    {
        if (depth > 16)
            return false;
        skip();
        if (m_i >= m_s.size())
            return false;
        const char c = m_s[m_i];
        if (c == '"') {
            out->kind = Json::Kind::String;
            return string(&out->text);
        }
        if (c == '{') {
            ++m_i;
            out->kind = Json::Kind::Object;
            skip();
            if (m_i < m_s.size() && m_s[m_i] == '}') {
                ++m_i;
                return true;
            }
            for (;;) {
                skip();
                std::string key;
                if (!string(&key))
                    return false;
                skip();
                if (m_i >= m_s.size() || m_s[m_i++] != ':')
                    return false;
                if (!value(&out->members[key], depth + 1))
                    return false;
                skip();
                if (m_i >= m_s.size())
                    return false;
                const char d = m_s[m_i++];
                if (d == '}')
                    return true;
                if (d != ',')
                    return false;
            }
        }
        if (c == '[') {
            ++m_i;
            out->kind = Json::Kind::Array;
            skip();
            if (m_i < m_s.size() && m_s[m_i] == ']') {
                ++m_i;
                return true;
            }
            for (;;) {
                out->items.emplace_back();
                if (!value(&out->items.back(), depth + 1))
                    return false;
                skip();
                if (m_i >= m_s.size())
                    return false;
                const char d = m_s[m_i++];
                if (d == ']')
                    return true;
                if (d != ',')
                    return false;
            }
        }
        // number, true, false, null: skipped
        const std::size_t start = m_i;
        while (m_i < m_s.size() && !std::strchr(",}] \t\r\n", m_s[m_i]))
            ++m_i;
        return m_i > start;
    }
    const std::string &m_s;
    std::size_t m_i = 0;
};

bool isAbsolute(const std::string &p)
{
    return (!p.empty() && (p[0] == '/' || p[0] == '\\')) || (p.size() > 2 && p[1] == ':' && (p[2] == '/' || p[2] == '\\'));
}

std::string folderOf(const std::string &path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// "..\foo.dll" next to "C:\a\b\x.json" -> "C:\a\foo.dll": '.' and '..' folded, separators kept as written.
std::string resolve(const std::string &library, const std::string &manifestPath)
{
    if (library.empty() || isAbsolute(library) || library.find_first_of("/\\") == std::string::npos)
        return library;  // absolute, or a bare name the loader finds itself
    std::string joined = folderOf(manifestPath);
    joined += joined.empty() ? "" : "/";
    joined += library;
    std::vector<std::string> parts;
    std::string current, prefix;
    std::size_t i = 0;
    if (!joined.empty() && (joined[0] == '/' || joined[0] == '\\'))
        prefix = "/", i = 1;
    else if (joined.size() > 2 && joined[1] == ':')
        prefix = joined.substr(0, 2) + "/", i = 3;
    for (; i <= joined.size(); ++i) {
        if (i == joined.size() || joined[i] == '/' || joined[i] == '\\') {
            if (current == "..") {
                if (!parts.empty())
                    parts.pop_back();
            } else if (!current.empty() && current != ".") {
                parts.push_back(current);
            }
            current.clear();
        } else {
            current += joined[i];
        }
    }
    std::string out = prefix;
    for (std::size_t k = 0; k < parts.size(); ++k)
        out += (k ? "/" : "") + parts[k];
    return out;
}

void addEntry(const Json &layer, const std::string &manifestPath, std::vector<LayerEntry> *out)
{
    if (layer.kind != Json::Kind::Object)
        return;
    const auto name = layer.members.find("name"), lib = layer.members.find("library_path");
    if (name == layer.members.end() || lib == layer.members.end() || name->second.kind != Json::Kind::String
        || lib->second.kind != Json::Kind::String || name->second.text.empty() || lib->second.text.empty())
        return;
    out->push_back({name->second.text, resolve(lib->second.text, manifestPath)});
}

} // namespace

std::vector<LayerEntry> parseLayerManifest(const std::string &json, const std::string &manifestPath)
{
    std::vector<LayerEntry> out;
    Json root;
    if (!Parser(json).parse(&root) || root.kind != Json::Kind::Object)
        return out;
    if (const auto l = root.members.find("layer"); l != root.members.end())
        addEntry(l->second, manifestPath, &out);
    if (const auto ls = root.members.find("layers"); ls != root.members.end() && ls->second.kind == Json::Kind::Array) {
        for (const Json &item : ls->second.items)
            addEntry(item, manifestPath, &out);
    }
    return out;
}

bool isDriverInternalLayer(const std::string &name)
{
    for (const char *prefix : {"VK_LAYER_NV_optimus", "VK_LAYER_NV_present", "VK_LAYER_AMD_switchable_graphics", "VK_LAYER_MESA_device_select"}) {
        if (name.rfind(prefix, 0) == 0)
            return true;
    }
    return false;
}

std::string normalisePath(std::string path)
{
    for (char &c : path)
        c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
    return path;
}

bool libraryMatches(const std::string &library, const std::string &loadedPath)
{
    const std::string lib = normalisePath(library), loaded = normalisePath(loadedPath);
    if (lib.empty() || loaded.empty())
        return false;
    if (lib.find('/') == std::string::npos)  // a bare name: any loaded path ending in it
        return loaded == lib || (loaded.size() > lib.size() && loaded.compare(loaded.size() - lib.size(), lib.size(), lib) == 0
                                 && loaded[loaded.size() - lib.size() - 1] == '/');
    return lib == loaded;
}

std::vector<std::string> activeLayers(const std::vector<LayerEntry> &entries, const std::vector<std::string> &loadedPaths)
{
    std::vector<std::string> out;
    for (const LayerEntry &e : entries) {
        if (e.name == "VK_LAYER_KHRONOS_validation" || isDriverInternalLayer(e.name))
            continue;
        for (const std::string &loaded : loadedPaths) {
            if (libraryMatches(e.library, loaded)) {
                out.push_back(e.name);
                break;
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace sb::gpu::vk
