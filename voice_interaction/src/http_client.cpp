#include "http_client.hpp"
#include <curl/curl.h>
#include <cstring>
#include <iostream>
#include <sstream>

// ====== 回调的上下文 ======
struct WriteContext {
    std::string data;
};

// libcurl 写回调
static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* ctx = static_cast<WriteContext*>(userdata);
    ctx->data.append(static_cast<char*>(ptr), total);
    return total;
}

// libcurl 流式写回调
struct StreamContext {
    std::function<void(const std::string&)> on_chunk;
    std::function<bool()> should_cancel;
    std::string leftover;  // 上次没消费完的半行
};

static size_t stream_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* ctx = static_cast<StreamContext*>(userdata);
    if (ctx->should_cancel && ctx->should_cancel()) return 0;
    std::string chunk(static_cast<char*>(ptr), total);

    // 按行拆分，保证不会把一条完整消息切成两段
    ctx->leftover += chunk;
    size_t pos;
    while ((pos = ctx->leftover.find('\n')) != std::string::npos) {
        std::string line = ctx->leftover.substr(0, pos);
        ctx->leftover = ctx->leftover.substr(pos + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            ctx->on_chunk(line);
    }
    return total;
}

static int progress_cb(void* userdata, curl_off_t, curl_off_t,
                       curl_off_t, curl_off_t) {
    auto* ctx = static_cast<StreamContext*>(userdata);
    return (ctx->should_cancel && ctx->should_cancel()) ? 1 : 0;
}

// ====== HttpClient ======

HttpClient::HttpClient(long timeout_ms, const std::string& proxy)
    : timeout_ms_(timeout_ms), proxy_(proxy) {
    curl_global_init(CURL_GLOBAL_ALL);
}

void HttpClient::set_proxy(const std::string& proxy) {
    proxy_ = proxy;
}

std::string HttpClient::post(const std::string& url,
                              const std::string& json_body,
                              const std::map<std::string, std::string>& headers) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    WriteContext ctx;
    struct curl_slist* hlist = nullptr;

    // 设置 headers
    for (auto& [k, v] : headers) {
        std::string h = k + ": " + v;
        hlist = curl_slist_append(hlist, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);  // 连接超时 3s，离线快速失败
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // 代理可配：空 = 直连
    if (!proxy_.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_.c_str());

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "[HttpClient] " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return ctx.data;
}

void HttpClient::post_stream(const std::string& url,
                              const std::string& json_body,
                              std::function<void(const std::string&)> on_chunk,
                              const std::map<std::string, std::string>& headers,
                              std::function<bool()> should_cancel) {
    CURL* curl = curl_easy_init();
    if (!curl) return;

    StreamContext ctx{on_chunk, should_cancel, ""};
    struct curl_slist* hlist = nullptr;

    for (auto& [k, v] : headers) {
        std::string h = k + ": " + v;
        hlist = curl_slist_append(hlist, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);  // 连接超时 3s
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    // 代理可配：空 = 直连
    if (!proxy_.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_.c_str());

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK && res != CURLE_ABORTED_BY_CALLBACK &&
        !(ctx.should_cancel && ctx.should_cancel())) {
        std::cerr << "[HttpClient stream] " << curl_easy_strerror(res) << std::endl;
    }

    // 发剩余数据
    if (!ctx.leftover.empty() &&
        !(ctx.should_cancel && ctx.should_cancel())) on_chunk(ctx.leftover);

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
}

std::string HttpClient::post_file(const std::string& url,
                                   const std::string& file_path,
                                   const std::string& field_name,
                                   const std::map<std::string, std::string>& headers) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    WriteContext ctx;
    struct curl_slist* hlist = nullptr;
    for (auto& [k, v] : headers)
        hlist = curl_slist_append(hlist, (k + ": " + v).c_str());

    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, field_name.c_str());
    curl_mime_filedata(part, file_path.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    curl_easy_perform(curl);

    curl_slist_free_all(hlist);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return ctx.data;
}

void HttpClient::set_timeout(long timeout_ms) {
    timeout_ms_ = timeout_ms;
}
