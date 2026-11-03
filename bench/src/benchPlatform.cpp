#include "benchPlatform.h"
#include "log.h"

namespace Tangram {

BenchPlatform::BenchPlatform(bool _fetch, const std::string& _requestLog) : m_fetch(_fetch) {
    if (m_fetch) { m_urlClient = std::make_unique<UrlClient>(UrlClient::Options{}); }
    if (!_requestLog.empty()) {
        m_log = fopen(_requestLog.c_str(), "a");
        if (!m_log) { LOGE("Unable to open request log %s", _requestLog.c_str()); }
    }
}

BenchPlatform::~BenchPlatform() {
    if (m_log) { fclose(m_log); }
}

void BenchPlatform::shutdown() {
    m_urlClient.reset();  // stop curl worker thread
    Platform::shutdown();
}

void BenchPlatform::cancelUrlRequestImpl(const UrlRequestId _id) {
    if (!m_urlClient) { return; }
    if (_id == UrlRequestId(-1)) {
        m_urlClient->cancelAllRequests();
    } else {
        m_urlClient->cancelRequest(_id);
    }
}

void BenchPlatform::setPhase(const std::string& _phase) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_phase = _phase;
}

bool BenchPlatform::startUrlRequestImpl(const Url& _url, const HttpOptions& _options,
                                        const UrlRequestHandle _request, UrlRequestId& _id) {
    if (_url.hasFileScheme()) {
        UrlResponse response;
        auto allocator = [&](size_t size) {
            response.content.resize(size);
            return response.content.data();
        };
        if (!Platform::bytesFromFileSystem(_url.path().c_str(), allocator)) {
            response.error = "Unable to read file";
        }
        onUrlResponse(_request, std::move(response));
        return false;
    }

    ++networkRequests;
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        if (m_log) { fprintf(m_log, "%s\t%s\n", m_phase.c_str(), _url.string().c_str()); }
    }

    if (!m_urlClient) {
        LOGW("Offline: tile missing from fixture: %s", _url.string().c_str());
        UrlResponse response;
        response.error = "offline benchmark run";
        onUrlResponse(_request, std::move(response));
        return false;
    }

    std::string url = _url.string();
    _id = m_urlClient->addRequest(url, _options, [this, _request, url](UrlResponse&& response) {
        if (response.error) {
            ++networkFailures;
            LOGE("Fetch failed: %s: %s", url.c_str(), response.error);
        }
        onUrlResponse(_request, std::move(response));
    });
    return true;
}

}
