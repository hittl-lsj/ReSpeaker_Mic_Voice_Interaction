#pragma once
#include <string>
#include <map>
#include <functional>

/**
 * @brief 基于 libcurl 的 HTTP 客户端
 *
 * 支持 GET/POST 和流式响应（SSE），用于调用各种 LLM API。
 */
class HttpClient {
public:
    /**
     * @param timeout_ms 超时（毫秒）
     * @param proxy      HTTP 代理地址，空字符串 = 直连不走代理
     *                   （开发机可用 "http://127.0.0.1:7897"，部署环境通常留空）
     */
    HttpClient(long timeout_ms = 90000, const std::string& proxy = "");

    /**
     * @brief POST JSON 并返回响应体
     * @param url 请求地址
     * @param json_body JSON 请求体
     * @param headers 额外 HTTP 请求头
     */
    std::string post(const std::string& url,
                     const std::string& json_body,
                     const std::map<std::string, std::string>& headers = {});

    /**
     * @brief POST JSON + 流式读取
     * @param url 请求地址
     * @param json_body JSON 请求体
     * @param on_chunk 每收到一块数据就回调一次
     * @param headers 额外 HTTP 请求头
     * @param should_cancel 返回 true 时中止请求
     */
    void post_stream(const std::string& url,
                     const std::string& json_body,
                     std::function<void(const std::string& chunk)> on_chunk,
                     const std::map<std::string, std::string>& headers = {},
                     std::function<bool()> should_cancel = {});

    /**
     * @brief POST multipart form
     * @param url 请求地址
     * @param file_path 上传文件路径
     * @param field_name multipart 的文件字段名
     * @param headers 额外 HTTP 请求头
     */
    std::string post_file(const std::string& url,
                          const std::string& file_path,
                          const std::string& field_name = "file",
                          const std::map<std::string, std::string>& headers = {});

    /** 设置超时（毫秒） */
    void set_timeout(long timeout_ms);

    /** 设置 HTTP 代理（空字符串 = 直连） */
    void set_proxy(const std::string& proxy);

private:
    long        timeout_ms_;
    std::string proxy_;
};
