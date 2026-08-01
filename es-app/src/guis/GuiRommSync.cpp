//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend (RomM Edition - unofficial fork)
//  GuiRommSync.cpp
//
//  Modal busy screen that runs a RomM library sync on a worker thread and
//  presents the result.
//

#include "guis/GuiRommSync.h"

#include "Log.h"
#include "Window.h"
#include "guis/GuiMsgBox.h"
#include "utils/LocalizationUtil.h"
#include "utils/StringUtil.h"

GuiRommSync::GuiRommSync(std::function<void()> updateCallback)
    : mRenderer {Renderer::getInstance()}
    , mUpdateCallback(updateCallback)
{
    setSize(mRenderer->getScreenWidth(), mRenderer->getScreenHeight());
    setPosition(0.0f, 0.0f);

    mBusyAnim.setSize(mSize);
    mBusyAnim.setText(_("SYNCING FROM ROMM..."));
    mBusyAnim.onSizeChanged();

    mSyncThread = std::make_unique<std::thread>([this]() {
        mStats = RommManager::getInstance().syncLibrary(
            mStopRequested, [this](const std::string& status) {
                const std::lock_guard<std::mutex> lock {mStatusMutex};
                mStatusText = status;
            });
        mDone = true;
    });
}

GuiRommSync::~GuiRommSync()
{
    mStopRequested = true;
    if (mSyncThread) {
        mSyncThread->join();
        mSyncThread.reset();
    }
    if ((mStats.stubsCreated > 0 || mStats.mediaDownloaded > 0) && mUpdateCallback)
        mUpdateCallback();
}

void GuiRommSync::update(int deltaTime)
{
    if (!mDone) {
        {
            const std::lock_guard<std::mutex> lock {mStatusMutex};
            if (mStatusText != mRenderedStatusText) {
                mRenderedStatusText = mStatusText;
                mBusyAnim.setText(mRenderedStatusText);
                mBusyAnim.onSizeChanged();
            }
        }
        mBusyAnim.update(deltaTime);
    }
    else if (!mResultShown) {
        mResultShown = true;

        std::string message;
        if (!mStats.errors.empty() && mStats.platformsMatched == 0) {
            message = _("ROMM SYNC FAILED") + "\n" + mStats.errors.front();
        }
        else {
            message = Utils::String::format(
                _("ROMM SYNC COMPLETED\n%i PLATFORMS MATCHED\n%i GAMES ADDED\n%i GAMES ALREADY "
                  "PRESENT\n%i COVER IMAGES DOWNLOADED"),
                mStats.platformsMatched, mStats.stubsCreated, mStats.gamesExisting,
                mStats.mediaDownloaded);
            if (!mStats.errors.empty())
                message.append("\n")
                    .append(Utils::String::format(_("%i ERRORS (SEE LOG)"),
                                                  static_cast<int>(mStats.errors.size())));
        }

        for (const std::string& error : mStats.errors)
            LOG(LogError) << "GuiRommSync: " << error;

        mWindow->pushGui(new GuiMsgBox(
            message, _("OK"), [this] { delete this; }, "", nullptr, "", nullptr, "", nullptr,
            nullptr, true, true));
    }

    GuiComponent::update(deltaTime);
}

void GuiRommSync::render(const glm::mat4& parentTrans)
{
    glm::mat4 trans {parentTrans * getTransform()};
    renderChildren(trans);

    if (!mDone)
        mBusyAnim.render(trans);
}

bool GuiRommSync::input(InputConfig* config, Input input)
{
    // While the sync is running the back button requests a stop; the worker
    // thread checks the flag between server calls.
    if (!mDone && input.value != 0 && config->isMappedTo("b", input)) {
        mStopRequested = true;
        return true;
    }
    return GuiComponent::input(config, input);
}

std::vector<HelpPrompt> GuiRommSync::getHelpPrompts()
{
    std::vector<HelpPrompt> prompts;
    prompts.push_back(HelpPrompt("b", _("cancel")));
    return prompts;
}
