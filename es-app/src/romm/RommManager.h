//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend (RomM Edition - unofficial fork)
//  RommManager.h
//
//  Client for a RomM server (https://romm.app): library sync, media download
//  and on-demand game download. Games that exist on the server but not locally
//  are represented by small stub files that are replaced with the real game
//  file the first time they are launched.
//

#ifndef ES_APP_ROMM_ROMM_MANAGER_H
#define ES_APP_ROMM_ROMM_MANAGER_H

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

class RommManager
{
public:
    static RommManager& getInstance();

    struct SyncStats {
        int platformsMatched {0};
        int platformsSkipped {0};
        int stubsCreated {0};
        int gamesExisting {0};
        int gamesSkipped {0};
        int mediaDownloaded {0};
        std::vector<std::string> errors;
    };

    // True if a server URL has been configured.
    bool isConfigured() const;

    // Contacts the server and returns an empty string on success, otherwise
    // a human-readable error.
    std::string testConnection();

    // Fetches the full platform/rom listing from the server and creates stub
    // files and gamelist.xml metadata entries for every game that maps to a
    // loaded ES-DE system. Runs blocking; call from a worker thread.
    SyncStats syncLibrary(std::atomic<bool>& stopRequested,
                          const std::function<void(const std::string&)>& statusCallback);

    // Whether the file at path is a RomM stub awaiting download.
    static bool isStub(const std::string& path);

    // Downloads the real game file over a stub, in place. Returns true on
    // success. Runs blocking; the game launch flow calls this synchronously
    // while the launch screen is displayed.
    bool downloadStubbedGame(const std::string& path, std::string& errorMsg);

    static const inline std::string STUB_MAGIC {"ROMM-STUB-V1"};

private:
    RommManager() {}

    // A sync target: an ES-DE system that is either currently loaded, or
    // defined in es_systems.xml but not loaded because its directory holds
    // no games yet (which is exactly the case on a first sync against an
    // empty library).
    struct TargetSystem {
        std::string name;
        std::string dirPath;
        std::vector<std::string> extensions; // Lowercase, with leading dot.
    };

    // name -> target, built from the es_systems.xml configuration files.
    std::map<std::string, TargetSystem> parseSystemsConfig();

    struct HttpResponse {
        long code {0};
        std::string body;
        std::string error;
        bool ok() const { return error.empty() && code >= 200 && code < 300; }
    };

    std::string baseURL() const;
    std::string userPwd() const;

    HttpResponse apiGet(const std::string& path);
    // Downloads url (absolute or server-relative) to destPath via a temporary
    // file. Returns an empty string on success, otherwise an error message.
    std::string downloadToFile(const std::string& url, const std::string& destPath);

    // RomM platform slug -> ES-DE system name, for the cases where they
    // differ. Identical names are matched directly against loaded systems.
    static const std::map<std::string, std::string>& slugMap();
};

#endif // ES_APP_ROMM_ROMM_MANAGER_H
