#include "esp32_sing.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "assets/lang_config.h"
#include "protocols/protocol.h"
#include "display/display.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <thread>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Esp32Sing"

// ========== 简单的ESP32认证函数（与 Esp32Music 对齐） ==========
/**
 * @brief 获取设备MAC地址
 * @return MAC地址字符串
 */
static std::string get_device_mac_sing() {
    return SystemInfo::GetMacAddress();
}

/**
 * @brief 获取设备芯片ID（用MAC无冒号形式代替）
 * @return 芯片ID字符串
 */
static std::string get_device_chip_id_sing() {
    std::string mac = SystemInfo::GetMacAddress();
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
    return mac;
}

/**
 * @brief 生成动态密钥（SHA256）
 */
static std::string generate_dynamic_key_sing(int64_t timestamp) {
    const std::string secret_key = "your-esp32-secret-key-2024";
    std::string mac = get_device_mac_sing();
    std::string chip_id = get_device_chip_id_sing();
    std::string data = mac + ":" + chip_id + ":" + std::to_string(timestamp) + ":" + secret_key;

    unsigned char hash[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash, 0);
    std::string key;
    for (int i = 0; i < 16; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", hash[i]);
        key += hex;
    }
    return key;
}

/**
 * @brief 为HTTP请求添加认证头
 */
static void add_auth_headers_sing(Http* http) {
    int64_t timestamp = esp_timer_get_time() / 1000000; // 秒
    std::string dynamic_key = generate_dynamic_key_sing(timestamp);
    std::string mac = get_device_mac_sing();
    std::string chip_id = get_device_chip_id_sing();
    if (http) {
        http->SetHeader("X-MAC-Address", mac);
        http->SetHeader("MAC", mac);      // 服务端优先识别 MAC 或 X-MAC
        http->SetHeader("X-MAC", mac);
        http->SetHeader("X-Chip-ID", chip_id);
        http->SetHeader("X-Timestamp", std::to_string(timestamp));
        http->SetHeader("X-Dynamic-Key", dynamic_key);
        ESP_LOGI(TAG, "Added auth headers - MAC: %s, ChipID: %s, Timestamp: %lld",
                 mac.c_str(), chip_id.c_str(), timestamp);
    }
}

static std::string url_encode_simple(const std::string& str) {
    std::string encoded;
    char hex[4];
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            // 使用 %20 编码空格，避免被服务端按 '+' 解析为空格
            encoded += "%20";
        } else if (c == '+') {
            // 在查询参数中，'+' 作为字面量需要编码为 %2B
            encoded += "%2B";
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

Esp32Sing::Esp32Sing() : current_stream_url_(), current_song_id_(), current_song_name_(),
                         song_name_displayed_(false), current_lyric_url_(), lyrics_(), current_lyric_index_(-1),
                         lyric_thread_(), is_lyric_running_(false), display_mode_(DISPLAY_MODE_SPECTRUM),
                         is_playing_(false), is_downloading_(false), play_thread_(), download_thread_(),
                         current_play_time_ms_(0), last_frame_time_ms_(0), total_frames_decoded_(0),
                         audio_buffer_(), buffer_mutex_(), buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr),
                         mp3_frame_info_(), mp3_decoder_initialized_(false), last_downloaded_data_() {
    ESP_LOGI(TAG, "Sing player initialized");
    InitializeMp3Decoder();

    // 允许通过 NVS 设置覆盖默认服务端，例如设置 key: (ns="sing", key="host")
    Settings sing_settings("sing", false);
    std::string host = sing_settings.GetString("host");
    if (!host.empty()) {
        ESP_LOGI(TAG, "Override sing base host from settings: %s", host.c_str());
        base_host_ = host;
    }
}

Esp32Sing::~Esp32Sing() {
    ESP_LOGI(TAG, "Destroying sing player");
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    if (download_thread_.joinable()) download_thread_.join();
    if (play_thread_.joinable()) play_thread_.join();
    if (lyric_thread_.joinable()) lyric_thread_.join();
    ClearAudioBuffer();
    CleanupMp3Decoder();
}

bool Esp32Sing::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    mp3_decoder_initialized_ = (mp3_decoder_ != nullptr);
    if (!mp3_decoder_initialized_) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
    }
    return mp3_decoder_initialized_;
}

void Esp32Sing::CleanupMp3Decoder() {
    if (mp3_decoder_) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
        mp3_decoder_initialized_ = false;
    }
}

void Esp32Sing::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    while (!audio_buffer_.empty()) {
        auto ch = audio_buffer_.front();
        if (ch.data) heap_caps_free(ch.data);
        audio_buffer_.pop();
    }
    buffer_size_ = 0;
}

void Esp32Sing::ResetSampleRate() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 &&
        codec->output_sample_rate() != codec->original_output_sample_rate()) {
        ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz",
                 codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1)) { // -1 表示重置到原始值
            ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
        } else {
            ESP_LOGW(TAG, "无法重置采样率到原始值");
        }
    }
}

std::string Esp32Sing::LookupSongId(const std::string& song_name, const std::string& artist_name) {
    // 占位实现：直接返回空，表示需要服务端或本地映射支持
    // 未来可：请求自建索引API，或本地缓存表进行映射
    (void)song_name; (void)artist_name;
    return std::string();
}

bool Esp32Sing::Download(const std::string& song_name, const std::string& artist_name) {
    // 构建“歌手+歌曲/仅歌曲”直连请求，并直接启动流式播放
    if (song_name.empty() && artist_name.empty()) {
        ESP_LOGE(TAG, "Both song and artist are empty");
        return false;
    }

    current_song_name_ = song_name;
    std::string path = "/stream"; // 流式端点，支持 GET 查询参数

    // 为避免 '+' 被当作空格，统一先拼接原始字符串，再整体 URL 编码（将 '+' 编为 %2B）
    std::string raw_query;
    if (!artist_name.empty() && !song_name.empty()) {
        raw_query = artist_name + "+" + song_name;
    } else if (!song_name.empty()) {
        raw_query = song_name;
    } else { // 仅歌手
        raw_query = artist_name;
    }
    // 直接使用 GET 携带查询，避免依赖服务端记忆
    current_query_value_ = raw_query;
    std::string full_url = base_host_ + path + "?raw_query=" + url_encode_simple(raw_query);
    ESP_LOGI(TAG, "Sing request URL (GET): %s", full_url.c_str());

    current_stream_url_ = full_url;
    last_downloaded_data_ = full_url;

    bool ok = StartStreaming(full_url);
    if (!ok) {
        ESP_LOGE(TAG, "Failed to start streaming for URL: %s", full_url.c_str());
        return false;
    }
    return true;
}

std::string Esp32Sing::GetDownloadResult() {
    return last_downloaded_data_;
}

bool Esp32Sing::StartStreaming(const std::string& music_url) {
    // 为兼容基类接口，允许直接传完整URL（不推荐）。
    if (music_url.empty()) {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }

    // 与ID逻辑复用线程管理（安全退出，防止上次404后卡住）
    ESP_LOGI(TAG, "StartStreaming: stopping previous threads if any");
    is_downloading_ = false;
    is_playing_ = false;

    // 先通知所有等待者
    { std::lock_guard<std::mutex> lock(buffer_mutex_); buffer_cv_.notify_all(); }

    // 安全等待下载线程退出（加入超时，避免卡死在Open/Read）
    if (download_thread_.joinable()) {
        ESP_LOGI(TAG, "Joining previous download thread with timeout");
        // 通过等待让下载线程在 is_downloading_/is_playing_ 置为 false 后退出循环
        bool finished = false; int waits = 0; const int max_waits = 100; // 1s
        while (!finished && waits < max_waits) {
            vTaskDelay(pdMS_TO_TICKS(10)); waits++;
            if (!download_thread_.joinable()) { finished = true; break; }
        }
        if (download_thread_.joinable()) {
            ESP_LOGW(TAG, "Download thread join timeout, detaching to avoid block");
            download_thread_.detach();
        } else {
            ESP_LOGI(TAG, "Previous download thread joined");
        }
    }

    // 安全等待播放线程退出（加入超时，避免卡死）
    if (play_thread_.joinable()) {
        ESP_LOGI(TAG, "Joining previous play thread with timeout");
        is_playing_ = false;
        { std::lock_guard<std::mutex> lock(buffer_mutex_); buffer_cv_.notify_all(); }
        bool finished = false; int waits = 0; const int max_waits = 100; // 1s
        while (!finished && waits < max_waits) {
            vTaskDelay(pdMS_TO_TICKS(10)); waits++;
        }
        if (play_thread_.joinable()) {
            ESP_LOGW(TAG, "Play thread join timeout, detaching to avoid block");
            play_thread_.detach();
        } else {
            ESP_LOGI(TAG, "Previous play thread joined");
        }
    }

    ClearAudioBuffer();
    // 重置解析模式
    wav_mode_ = false;
    wav_header_parsed_ = false;
    wav_channels_ = 1;
    wav_sample_rate_ = 16000;
    wav_bits_per_sample_ = 16;

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192; // 与现有music保持一致，后续可调
    cfg.prio = 5;
    cfg.thread_name = "sing_stream";
    esp_pthread_set_cfg(&cfg);

    // 直接使用URL（不带ID拼装）。
    current_stream_url_ = music_url;

    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Sing::DownloadAudioStreamFromUrl, this);
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Sing::PlayAudioStream, this);

    ESP_LOGI(TAG, "Sing streaming (URL) threads started");
    return true;
}

bool Esp32Sing::StartStreamingById(const std::string& song_id) {
    if (song_id.empty()) {
        ESP_LOGE(TAG, "Song ID is empty");
        return false;
    }

    ESP_LOGD(TAG, "Starting sing streaming for ID: %s", song_id.c_str());

    // 停止上次的下载/播放线程，确保不会卡住
    ESP_LOGI(TAG, "StartStreamingById: stopping previous threads if any");
    is_downloading_ = false;
    is_playing_ = false;

    { std::lock_guard<std::mutex> lock(buffer_mutex_); buffer_cv_.notify_all(); }

    if (download_thread_.joinable()) {
        ESP_LOGI(TAG, "Joining previous download thread");
        download_thread_.join();
        ESP_LOGI(TAG, "Previous download thread joined");
    }
    if (play_thread_.joinable()) {
        ESP_LOGI(TAG, "Joining previous play thread with timeout");
        is_playing_ = false;
        { std::lock_guard<std::mutex> lock(buffer_mutex_); buffer_cv_.notify_all(); }
        bool finished = false; int waits = 0; const int max_waits = 100;
        while (!finished && waits < max_waits) {
            vTaskDelay(pdMS_TO_TICKS(10)); waits++;
        }
        if (play_thread_.joinable()) {
            ESP_LOGW(TAG, "Play thread join timeout, detaching to avoid block");
            play_thread_.detach();
        } else {
            ESP_LOGI(TAG, "Previous play thread joined");
        }
    }

    ClearAudioBuffer();

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192; // 先保持8KB安全余量
    cfg.prio = 5;
    cfg.thread_name = "sing_stream";
    esp_pthread_set_cfg(&cfg);

    current_song_id_ = song_id;
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Sing::DownloadAudioStreamById, this, current_song_id_);

    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Sing::PlayAudioStream, this);

    ESP_LOGI(TAG, "Sing streaming (ID) threads started");
    return true;
}

bool Esp32Sing::StopStreaming() {
    ESP_LOGI(TAG, "Stopping sing streaming - downloading=%d, playing=%d", is_downloading_.load(), is_playing_.load());
    ResetSampleRate();
    if (!is_playing_ && !is_downloading_) return true;
    is_downloading_ = false;
    is_playing_ = false;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    if (download_thread_.joinable()) download_thread_.join();
    if (play_thread_.joinable()) {
        is_playing_ = false;
        { std::lock_guard<std::mutex> lock(buffer_mutex_); buffer_cv_.notify_all(); }
        bool finished = false; int waits = 0; const int max_waits = 100;
        while (!finished && waits < max_waits) {
            vTaskDelay(pdMS_TO_TICKS(10)); waits++;
            if (!play_thread_.joinable()) { finished = true; break; }
        }
        if (play_thread_.joinable()) play_thread_.join();
    }
    ESP_LOGI(TAG, "Sing streaming stopped");
    return true;
}

void Esp32Sing::DownloadAudioStreamById(const std::string& song_id) {
    // 通过 GET 方式，直接构造查询 URL 并复用 URL 下载逻辑
    std::string query_raw = song_id.empty() ? current_song_id_ : song_id;
    current_query_value_ = query_raw;
    current_stream_url_ = base_host_ + "/stream?raw_query=" + url_encode_simple(query_raw);
    ESP_LOGI(TAG, "Sing ID request URL (GET): %s", current_stream_url_.c_str());
    DownloadAudioStreamFromUrl();
}

void Esp32Sing::DownloadAudioStreamFromUrl() {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);

    if (current_stream_url_.empty()) {
        ESP_LOGE(TAG, "current_stream_url_ is empty");
        is_downloading_ = false;
        return;
    }

    http->SetHeader("User-Agent", "ESP32-Sing-Player/1.0");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Range", "bytes=0-");
    // 避免 Keep-Alive 导致服务端复用连接、在 404/错误后第二次请求被卡住
    // 显式关闭连接，确保每次播放尝试都建立新的 TCP 会话
    http->SetHeader("Connection", "close");
    add_auth_headers_sing(http.get());

    // 针对 convert_stream_simple 使用 POST；/stream 及其他使用 GET 携带查询
    bool use_post = current_stream_url_.find("/convert_stream_simple") != std::string::npos;
    if (use_post) {
        std::string boundary = "----ESP32_SING_BOUNDARY";
        http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        http->SetHeader("Transfer-Encoding", "chunked");
        ESP_LOGI(TAG, "Opening HTTP POST: %s", current_stream_url_.c_str());
        bool opened = http->Open("POST", current_stream_url_);
        if (!opened) {
            ESP_LOGW(TAG, "Open failed, retrying once: %s", current_stream_url_.c_str());
            vTaskDelay(pdMS_TO_TICKS(500));
            opened = http->Open("POST", current_stream_url_);
        }
        if (!opened) {
            ESP_LOGE(TAG, "Failed to connect to sing URL: %s", current_stream_url_.c_str());
            is_downloading_ = false;
            return;
        }
        // 写入表单字段：query
        std::string query_field;
        query_field += "--" + boundary + "\r\n";
        query_field += "Content-Disposition: form-data; name=\"query\"\r\n\r\n";
        query_field += current_query_value_ + "\r\n";
        http->Write(query_field.c_str(), query_field.size());
        // 结束multipart
        std::string end_field = "--" + boundary + "--\r\n";
        http->Write(end_field.c_str(), end_field.size());
        // 结束块
        http->Write("", 0);
    } else {
        ESP_LOGI(TAG, "Opening HTTP GET: %s", current_stream_url_.c_str());
        bool opened = http->Open("GET", current_stream_url_);
        if (!opened) {
            ESP_LOGW(TAG, "Open failed, retrying once: %s", current_stream_url_.c_str());
            vTaskDelay(pdMS_TO_TICKS(500));
            opened = http->Open("GET", current_stream_url_);
        }
        if (!opened) {
            ESP_LOGE(TAG, "Failed to connect to sing URL: %s", current_stream_url_.c_str());
            is_downloading_ = false;
            return;
        }
    }

    int status_code = http->GetStatusCode();
    if (status_code == 404) {
        ESP_LOGE(TAG, "HTTP GET 404: resource not found, stopping stream");
        http->Close();
        // 给网络栈一点时间释放旧连接，避免后续Open卡住
        vTaskDelay(pdMS_TO_TICKS(100));
        // 基本清理（避免在下载线程中调用 StopStreaming 造成自我等待）
        ResetSampleRate();
        is_downloading_ = false;
        is_playing_ = false;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
            ClearAudioBuffer();
        }
        // 停止歌词线程（如在运行）
        if (is_lyric_running_) {
            is_lyric_running_ = false;
            if (lyric_thread_.joinable()) {
                lyric_thread_.join();
            }
        }
        ESP_LOGI(TAG, "Sing 404 cleanup done: downloading=%d, playing=%d", is_downloading_.load(), is_playing_.load());
        // 反馈提示音并回到监听态，避免卡住后续音频
        auto& app = Application::GetInstance();
        app.PlaySound(Lang::Sounds::P3_VIBRATION);
        app.StartListening();
        return;
    } else if (status_code != 200 && status_code != 206) {
        ESP_LOGE(TAG, "HTTP GET failed: %d", status_code);
        http->Close();
        // 给网络栈一点时间释放旧连接，避免后续Open卡住
        vTaskDelay(pdMS_TO_TICKS(100));
        // 基本清理，确保后续可以重新播放
        ResetSampleRate();
        is_downloading_ = false;
        is_playing_ = false;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
            ClearAudioBuffer();
        }
        // 停止歌词线程（如在运行）
        if (is_lyric_running_) {
            is_lyric_running_ = false;
            if (lyric_thread_.joinable()) {
                lyric_thread_.join();
            }
        }
        ESP_LOGI(TAG, "Sing cleanup done after HTTP error: %d", status_code);
        return;
    }
    ESP_LOGI(TAG, "Started sing URL stream, status: %d", status_code);

    const size_t chunk_size = 4096;
    char buffer[chunk_size];
    size_t total_downloaded = 0;

    bool got_first_byte = false;
    bool first_byte_timeout = false;
    auto start_ts_read = esp_timer_get_time() / 1000; // ms
    while (is_downloading_ && is_playing_) {
        int bytes_read = http->Read(buffer, chunk_size);
        if (bytes_read < 0) { ESP_LOGE(TAG, "Read error: %d", bytes_read); break; }
        if (bytes_read == 0) {
            if (!got_first_byte) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                if ((now_ms - start_ts_read) >= open_timeout_ms_) {
                    ESP_LOGE(TAG, "Timeout waiting for first byte after %d ms", open_timeout_ms_);
                    first_byte_timeout = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            } else {
                ESP_LOGI(TAG, "Sing URL stream completed, total: %d", total_downloaded);
                break;
            }
        }
        got_first_byte = true;

        uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data) { ESP_LOGE(TAG, "Alloc chunk failed"); break; }
        memcpy(chunk_data, buffer, bytes_read);

        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock, [this]{ return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });
            if (is_downloading_) {
                audio_buffer_.push(SingAudioChunk(chunk_data, (size_t)bytes_read));
                buffer_size_ += (size_t)bytes_read;
                total_downloaded += (size_t)bytes_read;
                buffer_cv_.notify_one();
            } else {
                heap_caps_free(chunk_data);
                break;
            }
        }
    }

    http->Close();
    if (first_byte_timeout) {
        // 给网络栈时间释放旧连接，避免后续Open卡住
        vTaskDelay(pdMS_TO_TICKS(100));
        ResetSampleRate();
        is_downloading_ = false;
        is_playing_ = false;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
            ClearAudioBuffer();
        }
        // 停止歌词线程（如在运行）
        if (is_lyric_running_) {
            is_lyric_running_ = false;
            if (lyric_thread_.joinable()) {
                lyric_thread_.join();
            }
        }
        ESP_LOGI(TAG, "Sing cleanup done after first-byte timeout");
        // 保持空闲态，启用唤醒词即可
        return;
    }
    is_downloading_ = false;
    { std::lock_guard<std::mutex> lock(buffer_mutex_); buffer_cv_.notify_all(); }
}

void Esp32Sing::PlayAudioStream() {
    ESP_LOGI(TAG, "Starting sing playback");
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        ESP_LOGE(TAG, "Audio codec not available");
        is_playing_ = false;
        return;
    }
    if (!codec->output_enabled()) {
        // 自动开启输出，避免门槛导致无声
        codec->EnableOutput(true);
    }
    // MP3 解码器按需初始化：如果检测到MP3才需要

    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        buffer_cv_.wait(lock, [this]{ return buffer_size_ >= MIN_BUFFER_SIZE || (!is_downloading_ && !audio_buffer_.empty()); });
    }

    ESP_LOGI(TAG, "Playback start with buffer: %d", buffer_size_);
    size_t total_played = 0;
    uint8_t* mp3_input_buffer = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!mp3_input_buffer) {
        ESP_LOGE(TAG, "Alloc MP3 input buffer failed");
        is_playing_ = false;
        return;
    }
    int bytes_left = 0;
    uint8_t* read_ptr = nullptr;
    bool id3_processed = false;

    while (is_playing_) {
        auto& app = Application::GetInstance();
        DeviceState st = app.GetDeviceState();
        if (st == kDeviceStateListening || st == kDeviceStateSpeaking) {
            app.ToggleChatState();
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        } else if (st != kDeviceStateIdle) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!song_name_displayed_ && !current_song_name_.empty()) {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                std::string formatted = "《" + current_song_name_ + "》播放中...";
                display->SetMusicInfo(formatted.c_str());
                song_name_displayed_ = true;
                if (display_mode_ == DISPLAY_MODE_SPECTRUM) display->start();
            }
        }

        if (bytes_left < 4096) {
            SingAudioChunk chunk;
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (!is_downloading_) {
                        ESP_LOGI(TAG, "Playback finished, total: %d", total_played);
                        break;
                    }
                    buffer_cv_.wait(lock, [this]{ return !audio_buffer_.empty() || !is_downloading_; });
                    if (audio_buffer_.empty()) continue;
                }
                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;
                buffer_cv_.notify_one();
            }
            if (chunk.data && chunk.size > 0) {
                // 在首块数据上尝试格式检测
                if (!wav_header_parsed_) {
                    if (chunk.size >= 44 && memcmp(chunk.data, "RIFF", 4) == 0 && memcmp(chunk.data + 8, "WAVE", 4) == 0) {
                        // 解析WAV头 (简化处理，假设有fmt chunk)
                        wav_mode_ = true;
                        // 查找fmt块
                        size_t pos = 12; // RIFF(12字节)后开始
                        while (pos + 8 <= chunk.size) {
                            uint32_t chunk_size = *(uint32_t*)(chunk.data + pos + 4);
                            if (memcmp(chunk.data + pos, "fmt ", 4) == 0) {
                                if (pos + 8 + 16 <= chunk.size) {
                                    uint16_t audio_format = *(uint16_t*)(chunk.data + pos + 8);
                                    uint16_t num_channels = *(uint16_t*)(chunk.data + pos + 10);
                                    uint32_t sample_rate = *(uint32_t*)(chunk.data + pos + 12);
                                    uint16_t bits_per_sample = *(uint16_t*)(chunk.data + pos + 22);
                                    wav_channels_ = num_channels;
                                    wav_sample_rate_ = (int)sample_rate;
                                    wav_bits_per_sample_ = (int)bits_per_sample;
                                    ESP_LOGI(TAG, "Detected WAV: fmt audio_format=%d, channels=%d, rate=%d, bps=%d", audio_format, wav_channels_, wav_sample_rate_, wav_bits_per_sample_);
                                }
                            }
                            if (memcmp(chunk.data + pos, "data", 4) == 0) {
                                // data块开始，后续数据为PCM
                                size_t data_offset = pos + 8;
                                // 将剩余data内容写入到播放缓冲（移除WAV头）
                                size_t remain = (data_offset < chunk.size) ? (chunk.size - data_offset) : 0;
                                if (remain > 0) {
                                    // 将data部分覆盖到chunk开头便于统一读取
                                    memmove(chunk.data, chunk.data + data_offset, remain);
                                    chunk.size = remain;
                                } else {
                                    chunk.size = 0;
                                }
                                wav_header_parsed_ = true;
                                // 清理ID3流程，不适用于WAV
                                id3_processed = true;
                                // 播放路径改为PCM直送
                            }
                            pos += 8 + chunk_size;
                        }
                        if (!wav_header_parsed_) {
                            // 如果首块没遇到data，暂存，其后续块继续解析
                            ESP_LOGI(TAG, "WAV header parsed but data chunk not found in first buffer");
                            // 将首块数据复制到mp3_input_buffer以便后续继续解析
                        }
                    }
                }
                if (bytes_left > 0 && read_ptr != mp3_input_buffer) {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }
                size_t space = 8192 - bytes_left;
                size_t copy_size = std::min(chunk.size, space);
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += (int)copy_size;
                read_ptr = mp3_input_buffer;
                if (!id3_processed && !wav_mode_ && bytes_left >= 10) {
                    size_t skip = SkipId3Tag(read_ptr, (size_t)bytes_left);
                    if (skip > 0) {
                        read_ptr += skip;
                        bytes_left -= (int)skip;
                    }
                    id3_processed = true;
                }
                heap_caps_free(chunk.data);
            }
        }

        if (wav_mode_) {
            // 直接将 mp3_input_buffer 中的线性PCM发送（假设16bit PCM）
            if (bytes_left > 0) {
                // 如果是双声道，降为单声道
                int16_t* pcm16 = (int16_t*)read_ptr;
                int sample_count = bytes_left / 2; // 16-bit
                std::vector<int16_t> mono;
                if (wav_channels_ == 2) {
                    int mono_samples = sample_count / 2;
                    mono.resize(mono_samples);
                    for (int i = 0; i < mono_samples; ++i) {
                        int16_t left = pcm16[i * 2];
                        int16_t right = pcm16[i * 2 + 1];
                        mono[i] = (int16_t)((left + right) / 2);
                    }
                } else {
                    mono.assign(pcm16, pcm16 + sample_count);
                }

                AudioStreamPacket packet;
                packet.sample_rate = wav_sample_rate_;
                packet.frame_duration = 60;
                packet.timestamp = 0;
                packet.payload.resize(mono.size() * sizeof(int16_t));
                memcpy(packet.payload.data(), mono.data(), packet.payload.size());
                app.AddAudioData(std::move(packet));
                total_played += packet.payload.size();

                // 清空已消费字节
                bytes_left = 0;
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            continue;
        }

        // 非WAV：走MP3解码路径
        if (!mp3_decoder_initialized_) {
            // 在首次需要解码时初始化
            if (!InitializeMp3Decoder()) {
                ESP_LOGE(TAG, "Failed to init MP3 decoder");
                is_playing_ = false;
                break;
            }
        }

        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0) {
            bytes_left = 0;
            continue;
        }
        if (sync_offset > 0) {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }

        int16_t pcm_buffer[2304];
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);
        if (decode_result == 0) {
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0) continue;

            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) /
                                    (mp3_frame_info_.samprate * mp3_frame_info_.nChans);
            current_play_time_ms_ += frame_duration_ms;

            int16_t* final_pcm_data = pcm_buffer;
            int final_sample_count = mp3_frame_info_.outputSamps;
            std::vector<int16_t> mono_buffer;
            if (mp3_frame_info_.nChans == 2) {
                int stereo_samples = mp3_frame_info_.outputSamps;
                int mono_samples = stereo_samples / 2;
                mono_buffer.resize(mono_samples);
                for (int i = 0; i < mono_samples; ++i) {
                    int left = pcm_buffer[i * 2];
                    int right = pcm_buffer[i * 2 + 1];
                    mono_buffer[i] = (int16_t)((left + right) / 2);
                }
                final_pcm_data = mono_buffer.data();
                final_sample_count = mono_samples;
            }

            AudioStreamPacket packet;
            packet.sample_rate = mp3_frame_info_.samprate;
            packet.frame_duration = 60;
            packet.timestamp = 0;
            size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
            packet.payload.resize(pcm_size_bytes);
            memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);

            app.AddAudioData(std::move(packet));
            total_played += pcm_size_bytes;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    heap_caps_free(mp3_input_buffer);
    is_playing_ = false;

    // 播放结束：对齐音乐模块的处理，恢复采样率并保持空闲态（可唤醒）
    {
        ResetSampleRate();
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->SetMusicInfo("");
        }
        ESP_LOGI(TAG, "Sing playback finished: reset sample rate and stay idle");
    }
}

void Esp32Sing::SetDisplayMode(DisplayMode mode) {
    display_mode_.store(mode);
}

size_t Esp32Sing::SkipId3Tag(uint8_t* data, size_t size) {
    if (size < 10) return 0;
    if (memcmp(data, "ID3", 3) != 0) return 0;
    size_t tag_size = ((data[6] & 0x7F) << 21) | ((data[7] & 0x7F) << 14) | ((data[8] & 0x7F) << 7) | (data[9] & 0x7F);
    return 10 + tag_size;
}

bool Esp32Sing::DownloadLyrics(const std::string& lyric_url) {
    (void)lyric_url; return false; // 占位：sing暂不实现歌词
}

bool Esp32Sing::ParseLyrics(const std::string& lyric_content) {
    (void)lyric_content; return false; // 占位
}

void Esp32Sing::LyricDisplayThread() {
    // 占位
}

void Esp32Sing::UpdateLyricDisplay(int64_t current_time_ms) {
    (void)current_time_ms; // 占位
}