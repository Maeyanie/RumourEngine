#pragma once

namespace RumourEngine::Api {

    /** Queue an event for every listener registered for its type; RumourEngine's event thread delivers it. */
    void DispatchEvent(std::string_view type, const std::string& json);

    /** Record why the current call failed, for RumourEngine_GetLastError on this thread. */
    void SetLastError(std::string message);

    /** The debug inbox: RumourEngine_Inbox next to the log, if it exists. Each *.json file there
     *  holds a request, or an array of them, as AddRumour takes; {"retire": id} or
     *  {"retire": {"source", "key"}} retires one. Each file is renamed .done or .failed.
     *  For testing without a console mod. Worker thread, once a save is ready. */
    void PollInbox();
}
