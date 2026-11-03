#pragma once

#include "platform.h"
#include "urlClient.h"

#include <memory>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace Tangram {

// Platform for the benchmark replay
// - file: URLs are read synchronously (like MockPlatform), so scene loading doesn't depend on thread timing
// - offline mode: network requests fail immediately and are counted as fixture misses
// - fetch mode: network requests go through curl (UrlClient) and are logged with the current phase,
//   so the fixture can be pruned per phase afterwards
// - no system fonts (and no fontconfig, unlike LinuxPlatform): only the fallback fonts given in
//   SceneOptions, so results don't depend on the host
class BenchPlatform : public Platform {
public:
    BenchPlatform(bool _fetch, const std::string& _requestLog);
    ~BenchPlatform() override;

    void shutdown() override;
    void requestRender() const override {}
    std::vector<FontSourceHandle> systemFontFallbacksHandle() const override { return {}; }
    FontSourceHandle systemFont(const std::string& _name, const std::string& _weight,
                                const std::string& _face) const override { return {}; }

    bool startUrlRequestImpl(const Url& _url, const HttpOptions& _options,
                             const UrlRequestHandle _request, UrlRequestId& _id) override;
    void cancelUrlRequestImpl(const UrlRequestId _id) override;

    void setPhase(const std::string& _phase);
    const std::string& phase() const { return m_phase; }

    std::atomic<uint64_t> networkRequests{0};  // offline: misses; fetch: requests sent
    std::atomic<uint64_t> networkFailures{0};  // fetch mode only

private:
    bool m_fetch;
    std::unique_ptr<UrlClient> m_urlClient;
    FILE* m_log = nullptr;
    std::mutex m_logMutex;
    std::string m_phase;
};

}
