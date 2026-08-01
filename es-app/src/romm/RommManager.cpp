//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend (RomM Edition - unofficial fork)
//  RommManager.cpp
//
//  Client for a RomM server: library sync, media download and on-demand
//  game download.
//
//  Server API notes (verified against RomM 4.9.x):
//  * Basic auth is accepted on all API endpoints.
//  * GET /api/roms answers a paginated envelope {"items": [...], "total": N},
//    never a bare array, and the platform filter parameter is "platform_ids".
//  * Game content is served from /api/roms/{id}/content/{fs_name}.
//

#include "romm/RommManager.h"

#include "FileData.h"
#include "HttpReq.h"
#include "Log.h"
#include "Settings.h"
#include "SystemData.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include <curl/curl.h>
#include <pugixml.hpp>
#include <rapidjson/document.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <tuple>

namespace
{
    size_t writeToString(void* buff, size_t size, size_t nmemb, void* strPtr)
    {
        static_cast<std::string*>(strPtr)->append(static_cast<char*>(buff), size * nmemb);
        return size * nmemb;
    }

    size_t writeToStream(void* buff, size_t size, size_t nmemb, void* streamPtr)
    {
        static_cast<std::ofstream*>(streamPtr)->write(static_cast<char*>(buff), size * nmemb);
        return size * nmemb;
    }
} // namespace

RommManager& RommManager::getInstance()
{
    static RommManager instance;
    return instance;
}

bool RommManager::isConfigured() const { return !baseURL().empty(); }

std::string RommManager::baseURL() const
{
    std::string url {Settings::getInstance()->getString("RommServerURL")};
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    return url;
}

std::string RommManager::userPwd() const
{
    const std::string user {Settings::getInstance()->getString("RommUsername")};
    const std::string pass {Settings::getInstance()->getString("RommPassword")};
    if (user.empty())
        return "";
    return user + ":" + pass;
}

RommManager::HttpResponse RommManager::apiGet(const std::string& path)
{
    HttpResponse response;
    CURL* handle {curl_easy_init()};
    if (handle == nullptr) {
        response.error = "curl_easy_init failed";
        return response;
    }

    const std::string url {baseURL() + path};
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    const std::string credentials {userPwd()};
    if (!credentials.empty()) {
        curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(handle, CURLOPT_USERPWD, credentials.c_str());
    }

    const CURLcode result {curl_easy_perform(handle)};
    if (result != CURLE_OK)
        response.error = curl_easy_strerror(result);
    else
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.code);
    curl_easy_cleanup(handle);
    return response;
}

std::string RommManager::downloadToFile(const std::string& url, const std::string& destPath)
{
    const std::string tempPath {destPath + ".rommdl"};
    std::ofstream stream {tempPath, std::ios::binary};
    if (!stream.is_open())
        return "Couldn't open " + tempPath + " for writing";

    CURL* handle {curl_easy_init()};
    if (handle == nullptr) {
        stream.close();
        Utils::FileSystem::removeFile(tempPath);
        return "curl_easy_init failed";
    }

    const std::string fullURL {url.rfind("http", 0) == 0 ? url : baseURL() + url};
    curl_easy_setopt(handle, CURLOPT_URL, fullURL.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToStream);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &stream);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    // No total timeout as game files can be large; abort on a stalled
    // transfer instead.
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 30L);
    const std::string credentials {userPwd()};
    if (!credentials.empty()) {
        curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(handle, CURLOPT_USERPWD, credentials.c_str());
    }

    const CURLcode result {curl_easy_perform(handle)};
    long code {0};
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(handle);
    stream.close();

    if (result != CURLE_OK || code < 200 || code >= 300) {
        Utils::FileSystem::removeFile(tempPath);
        if (result != CURLE_OK)
            return std::string {curl_easy_strerror(result)};
        return "HTTP error " + std::to_string(code) + " for " + fullURL;
    }

    if (Utils::FileSystem::exists(destPath))
        Utils::FileSystem::removeFile(destPath);
    if (!Utils::FileSystem::renameFile(tempPath, destPath, true)) {
        Utils::FileSystem::removeFile(tempPath);
        return "Couldn't move downloaded file into place at " + destPath;
    }
    return "";
}

std::string RommManager::testConnection()
{
    const HttpResponse response {apiGet("/api/platforms")};
    if (!response.error.empty())
        return response.error;
    if (response.code == 401 || response.code == 403)
        return "Authentication failed (HTTP " + std::to_string(response.code) + ")";
    if (!response.ok())
        return "Server answered HTTP " + std::to_string(response.code);
    return "";
}

bool RommManager::isStub(const std::string& path)
{
    std::ifstream stream {path, std::ios::binary};
    if (!stream.is_open())
        return false;
    std::string magic(STUB_MAGIC.size(), '\0');
    stream.read(magic.data(), static_cast<std::streamsize>(STUB_MAGIC.size()));
    return stream.gcount() == static_cast<std::streamsize>(STUB_MAGIC.size()) &&
           magic == STUB_MAGIC;
}

bool RommManager::downloadStubbedGame(const std::string& path, std::string& errorMsg)
{
    std::ifstream stream {path};
    if (!stream.is_open()) {
        errorMsg = "Couldn't read stub file";
        return false;
    }
    std::string line;
    std::string romId;
    std::string fileName;
    long long expectedSize {-1};
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("id=", 0) == 0)
            romId = line.substr(3);
        else if (line.rfind("size=", 0) == 0)
            expectedSize = std::stoll(line.substr(5));
        else if (line.rfind("file=", 0) == 0)
            fileName = line.substr(5);
    }
    stream.close();

    if (romId.empty() || fileName.empty()) {
        errorMsg = "Stub file is malformed";
        return false;
    }
    if (!isConfigured()) {
        errorMsg = "No RomM server configured";
        return false;
    }

    LOG(LogInfo) << "RommManager: Downloading \"" << fileName << "\" (rom id " << romId
                 << ") from " << baseURL();

    const std::string url {"/api/roms/" + romId + "/content/" + HttpReq::urlEncode(fileName)};
    const std::string downloadError {downloadToFile(url, path)};
    if (!downloadError.empty()) {
        errorMsg = downloadError;
        return false;
    }

    if (expectedSize >= 0) {
        const long long actualSize {
            static_cast<long long>(Utils::FileSystem::getFileSize(path))};
        if (actualSize != expectedSize) {
            LOG(LogWarning) << "RommManager: Size mismatch for \"" << fileName << "\" (expected "
                            << expectedSize << " bytes, got " << actualSize << " bytes)";
        }
    }
    return true;
}

const std::map<std::string, std::string>& RommManager::slugMap()
{
    // RomM platform slug -> ES-DE system name, inverted from RomM's own
    // examples/config.es-de.example.yml. Slugs that already equal the ES-DE
    // system name are matched directly and don't need an entry here.
    static const std::map<std::string, std::string> map {
        {"3ds", "n3ds"},
        {"acpc", "amstradcpc"},
        {"amiga-cd32", "amigacd32"},
        {"apf", "apfm1000"},
        {"apple-iigs", "apple2gs"},
        {"appleii", "apple2"},
        {"arcadia-2001", "arcadia"},
        {"astrocade", "astrocde"},
        {"atari-jaguar-cd", "atarijaguarcd"},
        {"atari-st", "atarist"},
        {"atari8bit", "atari800"},
        {"casio-pv-1000", "pv1000"},
        {"commodore-cdtv", "cdtv"},
        {"creativision", "crvision"},
        {"dc", "dreamcast"},
        {"dragon-32-slash-64", "dragon32"},
        {"fairchild-channel-f", "channelf"},
        {"fm-towns", "fmtowns"},
        {"g-and-w", "gameandwatch"},
        {"game-dot-com", "gamecom"},
        {"genesis", "megadrive"},
        {"genesis-slash-megadrive", "megadrive"},
        {"handheld-electronic-lcd", "lcdgames"},
        {"jaguar", "atarijaguar"},
        {"lynx", "atarilynx"},
        {"mac", "macintosh"},
        {"mega-duck-slash-cougar-boy", "megaduck"},
        {"neo-geo-cd", "neogeocd"},
        {"neo-geo-pocket", "ngp"},
        {"neo-geo-pocket-color", "ngpc"},
        {"neogeoaes", "neogeo"},
        {"ngc", "gc"},
        {"64dd", "n64dd"},
        {"nintendo-64dd", "n64dd"},
        {"odyssey-2-slash-videopac-g7000", "odyssey2"},
        {"palm-os", "palm"},
        {"pc-8800-series", "pc88"},
        {"pc-9800-series", "pc98"},
        {"pc-fx", "pcfx"},
        {"pokemon-mini", "pokemini"},
        {"sam-coupe", "samcoupe"},
        {"sms", "mastersystem"},
        {"sega32", "sega32x"},
        {"segacd", "megacd"},
        {"sfam", "sfc"},
        {"sg1000", "sg-1000"},
        {"sharp-x68000", "x68000"},
        {"ti-99", "ti99"},
        {"turbografx-cd", "tg-cd"},
        {"turbografx16--1", "tg16"},
        {"turbografx-16-slash-pc-engine-cd", "tg-cd"},
        {"vic-20", "vic20"},
        {"videopac-g7400", "videopac"},
        {"vsmile", "vsmile"},
        {"watara-slash-quickshot-supervision", "supervision"},
        {"win", "pc"},
        {"z-machine", "zmachine"},
        {"zxs", "zxspectrum"},
    };
    return map;
}

std::map<std::string, RommManager::TargetSystem> RommManager::parseSystemsConfig()
{
    // Parse es_systems.xml directly so platforms can be synced into systems
    // that ES-DE hasn't loaded because their directories hold no games yet.
    // Same file resolution and %ROMPATH% expansion as
    // SystemData::createSystemDirectories().
    std::map<std::string, TargetSystem> systems;
    std::vector<std::string> configPaths {SystemData::getConfigPath()};
    const std::string rompath {FileData::getROMDirectory()};

    // If the custom es_systems.xml file has the loadExclusive tag then the
    // bundled configuration file is not processed, same as during regular
    // system loading.
    bool onlyProcessCustomFile {false};
    if (configPaths.size() > 1) {
        pugi::xml_document doc;
#if defined(_WIN64)
        const pugi::xml_parse_result& res {
            doc.load_file(Utils::String::stringToWideString(configPaths.front()).c_str())};
#else
        const pugi::xml_parse_result& res {doc.load_file(configPaths.front().c_str())};
#endif
        if (res && doc.child("loadExclusive"))
            onlyProcessCustomFile = true;
    }

    // Process the custom file last so it overrides the bundled one.
    std::reverse(configPaths.begin(), configPaths.end());

    for (auto& configPath : configPaths) {
        if (onlyProcessCustomFile && configPath == configPaths.front())
            continue;
        pugi::xml_document doc;
#if defined(_WIN64)
        const pugi::xml_parse_result& res {
            doc.load_file(Utils::String::stringToWideString(configPath).c_str())};
#else
        const pugi::xml_parse_result& res {doc.load_file(configPath.c_str())};
#endif
        if (!res)
            continue;
        const pugi::xml_node& systemList {doc.child("systemList")};
        if (!systemList)
            continue;

        for (pugi::xml_node system {systemList.child("system")}; system;
             system = system.next_sibling("system")) {
            TargetSystem target;
            target.name = system.child("name").text().get();
            std::string path {system.child("path").text().get()};
            if (target.name.empty() || path.find("%ROMPATH%") != 0)
                continue;
            path = Utils::String::replace(path, "%ROMPATH%", rompath);
            path = Utils::String::replace(path, "//", "/");
            target.dirPath = path;
            for (std::string extension :
                 Utils::String::delimitedStringToVector(
                     Utils::String::toLower(system.child("extension").text().get()), " "))
                target.extensions.emplace_back(extension);
            systems[target.name] = target;
        }
    }
    return systems;
}

RommManager::SyncStats RommManager::syncLibrary(
    std::atomic<bool>& stopRequested,
    const std::function<void(const std::string&)>& statusCallback)
{
    SyncStats stats;

    statusCallback("CONTACTING SERVER...");
    const HttpResponse platformsResponse {apiGet("/api/platforms")};
    if (!platformsResponse.ok()) {
        stats.errors.emplace_back(
            "Couldn't list platforms: " +
            (platformsResponse.error.empty() ?
                 "HTTP " + std::to_string(platformsResponse.code) :
                 platformsResponse.error));
        return stats;
    }

    rapidjson::Document platformsDoc;
    platformsDoc.Parse(platformsResponse.body.c_str());
    if (platformsDoc.HasParseError() || !platformsDoc.IsArray()) {
        stats.errors.emplace_back("Platform listing was not valid JSON");
        return stats;
    }

    const bool downloadMedia {Settings::getInstance()->getBool("RommDownloadMedia")};
    const std::string appDataDir {Utils::FileSystem::getAppDataDirectory()};
    const std::map<std::string, TargetSystem> configSystems {parseSystemsConfig()};

    for (auto& platform : platformsDoc.GetArray()) {
        if (stopRequested)
            return stats;
        if (!platform.IsObject() || !platform.HasMember("id"))
            continue;

        const int platformId {platform["id"].GetInt()};
        const std::string slug {platform.HasMember("slug") && platform["slug"].IsString() ?
                                    platform["slug"].GetString() :
                                    ""};
        const std::string fsSlug {platform.HasMember("fs_slug") &&
                                          platform["fs_slug"].IsString() ?
                                      platform["fs_slug"].GetString() :
                                      ""};
        int romCount {0};
        if (platform.HasMember("rom_count") && platform["rom_count"].IsInt())
            romCount = platform["rom_count"].GetInt();
        if (romCount == 0)
            continue;

        // Resolve the RomM platform to an ES-DE system: explicit mapping
        // first, then a direct name match, for both slug flavors. A currently
        // loaded system takes priority, but systems that are defined in
        // es_systems.xml without being loaded (because their directories hold
        // no games yet) are valid targets too.
        TargetSystem target;
        std::vector<std::string> candidateNames;
        for (const std::string& candidate : {slug, fsSlug}) {
            if (candidate.empty())
                continue;
            const auto it = slugMap().find(candidate);
            if (it != slugMap().cend())
                candidateNames.emplace_back(it->second);
            candidateNames.emplace_back(candidate);
        }
        for (const std::string& candidateName : candidateNames) {
            SystemData* system {SystemData::getSystemByName(candidateName)};
            if (system != nullptr && !system->isCollection()) {
                target.name = system->getName();
                target.dirPath = system->getRootFolder()->getPath();
                for (const std::string& extension : system->getExtensions())
                    target.extensions.emplace_back(Utils::String::toLower(extension));
                break;
            }
            const auto it = configSystems.find(candidateName);
            if (it != configSystems.cend()) {
                target = it->second;
                break;
            }
        }
        if (target.name.empty()) {
            ++stats.platformsSkipped;
            LOG(LogDebug) << "RommManager: No ES-DE system matches RomM platform \"" << slug
                          << "\", skipping";
            continue;
        }
        ++stats.platformsMatched;
        statusCallback(Utils::String::toUpper(target.name) + " (" + std::to_string(romCount) +
                       " GAMES)");

        const std::vector<std::string>& systemExtensions {target.extensions};
        const std::string& systemDir {target.dirPath};
        if (!Utils::FileSystem::exists(systemDir))
            Utils::FileSystem::createDirectory(systemDir);

        // Entries to merge into the gamelist: fs_name -> {name, summary}.
        std::vector<std::tuple<std::string, std::string, std::string>> gamelistEntries;

        int offset {0};
        const int pageSize {100};
        while (true) {
            if (stopRequested)
                return stats;
            const HttpResponse romsResponse {
                apiGet("/api/roms?platform_ids=" + std::to_string(platformId) +
                       "&limit=" + std::to_string(pageSize) + "&offset=" + std::to_string(offset))};
            if (!romsResponse.ok()) {
                stats.errors.emplace_back("Couldn't list roms for " + slug);
                break;
            }
            rapidjson::Document romsDoc;
            romsDoc.Parse(romsResponse.body.c_str());
            if (romsDoc.HasParseError() || !romsDoc.IsObject() || !romsDoc.HasMember("items") ||
                !romsDoc["items"].IsArray()) {
                stats.errors.emplace_back("Rom listing for " + slug + " was not valid JSON");
                break;
            }

            const auto& items = romsDoc["items"];
            for (auto& rom : items.GetArray()) {
                if (stopRequested)
                    return stats;
                if (!rom.IsObject() || !rom.HasMember("id") || !rom.HasMember("fs_name"))
                    continue;

                const std::string fsName {rom["fs_name"].GetString()};
                const bool multiFile {rom.HasMember("has_multiple_files") &&
                                      rom["has_multiple_files"].IsBool() &&
                                      rom["has_multiple_files"].GetBool()};
                std::string extension {rom.HasMember("fs_extension") &&
                                               rom["fs_extension"].IsString() ?
                                           rom["fs_extension"].GetString() :
                                           ""};
                if (multiFile || extension.empty()) {
                    ++stats.gamesSkipped;
                    continue;
                }
                extension = "." + Utils::String::toLower(extension);
                if (std::find(systemExtensions.cbegin(), systemExtensions.cend(), extension) ==
                    systemExtensions.cend()) {
                    ++stats.gamesSkipped;
                    continue;
                }

                const std::string gamePath {systemDir + "/" + fsName};
                if (Utils::FileSystem::exists(gamePath) && !isStub(gamePath)) {
                    ++stats.gamesExisting;
                }
                else {
                    long long fileSize {0};
                    if (rom.HasMember("fs_size_bytes") && rom["fs_size_bytes"].IsInt64())
                        fileSize = rom["fs_size_bytes"].GetInt64();
                    std::ofstream stubStream {gamePath, std::ios::binary | std::ios::trunc};
                    if (!stubStream.is_open()) {
                        stats.errors.emplace_back("Couldn't write stub " + gamePath);
                        continue;
                    }
                    stubStream << STUB_MAGIC << "\n"
                               << "id=" << rom["id"].GetInt() << "\n"
                               << "size=" << fileSize << "\n"
                               << "file=" << fsName << "\n";
                    stubStream.close();
                    ++stats.stubsCreated;
                }

                const std::string displayName {rom.HasMember("name") && rom["name"].IsString() ?
                                                   rom["name"].GetString() :
                                                   ""};
                const std::string summary {rom.HasMember("summary") &&
                                                   rom["summary"].IsString() ?
                                               rom["summary"].GetString() :
                                               ""};
                if (!displayName.empty())
                    gamelistEntries.emplace_back(fsName, displayName, summary);

                if (downloadMedia && rom.HasMember("path_cover_large") &&
                    rom["path_cover_large"].IsString()) {
                    std::string coverURL {rom["path_cover_large"].GetString()};
                    if (!coverURL.empty()) {
                        std::string coverExtension {".png"};
                        const std::string coverPath {coverURL.substr(0, coverURL.find('?'))};
                        const size_t dotPosition {coverPath.find_last_of('.')};
                        if (dotPosition != std::string::npos)
                            coverExtension = coverPath.substr(dotPosition);
                        const std::string stemName {rom.HasMember("fs_name_no_ext") &&
                                                            rom["fs_name_no_ext"].IsString() ?
                                                        rom["fs_name_no_ext"].GetString() :
                                                        fsName};
                        const std::string mediaDir {appDataDir + "/downloaded_media/" +
                                                    target.name + "/covers"};
                        const std::string mediaPath {mediaDir + "/" + stemName + coverExtension};
                        if (!Utils::FileSystem::exists(mediaPath)) {
                            Utils::FileSystem::createDirectory(mediaDir);
                            // The ?ts cache-buster confuses some proxies, strip it.
                            if (downloadToFile(coverPath, mediaPath).empty())
                                ++stats.mediaDownloaded;
                        }
                    }
                }
            }

            const int received {static_cast<int>(items.Size())};
            offset += received;
            int total {-1};
            if (romsDoc.HasMember("total") && romsDoc["total"].IsInt())
                total = romsDoc["total"].GetInt();
            // A short page is the last page, and that guard must come first
            // so a server whose total disagrees with reality can't spin this
            // loop forever.
            if (received == 0 || received < pageSize || (total >= 0 && offset >= total))
                break;
        }

        // Merge names and descriptions into the ES-DE gamelist so a synced
        // library is browsable with proper titles right away. Existing
        // entries are left alone as they may contain user edits.
        if (!gamelistEntries.empty()) {
            const std::string gamelistDir {appDataDir + "/gamelists/" + target.name};
            const std::string gamelistPath {gamelistDir + "/gamelist.xml"};
            Utils::FileSystem::createDirectory(gamelistDir);

            pugi::xml_document doc;
            if (Utils::FileSystem::exists(gamelistPath))
                doc.load_file(gamelistPath.c_str());
            pugi::xml_node root {doc.child("gameList")};
            if (!root)
                root = doc.append_child("gameList");

            for (const auto& entry : gamelistEntries) {
                const std::string relativePath {"./" + std::get<0>(entry)};
                bool exists {false};
                for (pugi::xml_node game : root.children("game")) {
                    if (relativePath == game.child("path").text().get()) {
                        exists = true;
                        break;
                    }
                }
                if (exists)
                    continue;
                pugi::xml_node game {root.append_child("game")};
                game.append_child("path").text().set(relativePath.c_str());
                game.append_child("name").text().set(std::get<1>(entry).c_str());
                if (!std::get<2>(entry).empty())
                    game.append_child("desc").text().set(std::get<2>(entry).c_str());
            }
            doc.save_file(gamelistPath.c_str());
        }
    }

    return stats;
}
