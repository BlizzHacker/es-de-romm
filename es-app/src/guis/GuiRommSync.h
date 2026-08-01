//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend (RomM Edition - unofficial fork)
//  GuiRommSync.h
//
//  Modal busy screen that runs a RomM library sync on a worker thread and
//  presents the result.
//

#ifndef ES_APP_GUIS_GUI_ROMM_SYNC_H
#define ES_APP_GUIS_GUI_ROMM_SYNC_H

#include "GuiComponent.h"
#include "components/BusyComponent.h"
#include "romm/RommManager.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

class GuiRommSync : public GuiComponent
{
public:
    GuiRommSync(std::function<void()> updateCallback);
    ~GuiRommSync();

    void update(int deltaTime) override;
    void render(const glm::mat4& parentTrans) override;
    bool input(InputConfig* config, Input input) override;
    std::vector<HelpPrompt> getHelpPrompts() override;

private:
    Renderer* mRenderer;
    BusyComponent mBusyAnim;
    std::function<void()> mUpdateCallback;

    std::unique_ptr<std::thread> mSyncThread;
    std::atomic<bool> mDone {false};
    std::atomic<bool> mStopRequested {false};
    bool mResultShown {false};

    std::mutex mStatusMutex;
    std::string mStatusText;
    std::string mRenderedStatusText;

    RommManager::SyncStats mStats;
};

#endif // ES_APP_GUIS_GUI_ROMM_SYNC_H
