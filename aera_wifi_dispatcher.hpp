#pragma once

// Runs slow Wi-Fi lifecycle work away from the recovery UI thread. Radio
// transitions use last-request-wins semantics, while status refreshes are
// folded together so repeated UI events cannot create a backlog.
class AeraWifiDispatcher {
public:
    enum class Request {
        StartRadio,
        StopRadio,
        RefreshStatus,
        RestoreSession,
    };

    static void Submit(Request request);
    static bool Active();
};
