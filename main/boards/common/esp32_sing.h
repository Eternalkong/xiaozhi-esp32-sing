#pragma once

#include <string>
#include <queue>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include "music.h"

// MP3解码器支持（与 Esp32Music 保持一致）
extern "C" {
#include "mp3dec.h"
}

struct SingAudioChunk {
    uint8_t* data;
    size_t size;
    SingAudioChunk() : data(nullptr), size(0) {}
    SingAudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}
};

enum DisplayMode {
    DISPLAY_MODE_SPECTRUM = 0,
    DISPLAY_MODE_STATIC = 1,
};

class Esp32Sing : public Music {
public:
    Esp32Sing();
    ~Esp32Sing();

    bool Download(const std::string& song_name, const std::string& artist_name) override;
    std::string GetDownloadResult() override;

    bool StartStreaming(const std::string& music_url) override; // 为兼容，允许URL直连
    bool StopStreaming() override;

    // 对齐基类接口的必要覆写
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }
    virtual int16_t* GetAudioData() override { return nullptr; }

    // Sing 专属：通过 ID 启动流式播放
    bool StartStreamingById(const std::string& song_id);

    // 显示控制
    void SetDisplayMode(DisplayMode mode);

    // 配置 sing 服务端参数
    inline void SetBaseHost(const std::string& host) { base_host_ = host; }
    inline void SetOpenTimeoutMs(int ms) { open_timeout_ms_ = ms; }
    inline int GetOpenTimeoutMs() const { return open_timeout_ms_; }

    // 名称到ID的占位查找（可由服务端/本地实现）
    std::string LookupSongId(const std::string& song_name, const std::string& artist_name);

private:
    void DownloadAudioStreamById(const std::string& song_id);
    void DownloadAudioStreamFromUrl();
    void PlayAudioStream();

    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    void ClearAudioBuffer();
    void ResetSampleRate();
    size_t SkipId3Tag(uint8_t* data, size_t size);

    // 歌词相关（占位）
    bool DownloadLyrics(const std::string& lyric_url);
    bool ParseLyrics(const std::string& lyric_content);
    void LyricDisplayThread();
    void UpdateLyricDisplay(int64_t current_time_ms);

private:
    // sing 服务端参数
    std::string base_host_ = "http://8.134.249.85:18080";
    int open_timeout_ms_ = 10000; // 打开连接和首字节等待超时，默认10秒

    // 当前流信息
    std::string current_stream_url_;
    std::string current_query_value_;
    std::string current_song_id_;
    std::string current_song_name_;

    // 显示与歌词
    std::atomic<bool> song_name_displayed_;
    std::string current_lyric_url_;
    std::vector<std::string> lyrics_;
    std::atomic<int> current_lyric_index_;
    std::thread lyric_thread_;
    std::atomic<bool> is_lyric_running_;
    std::atomic<DisplayMode> display_mode_;

    // 下载与播放状态
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::thread play_thread_;
    std::thread download_thread_;

    // 时间与统计
    std::atomic<int64_t> current_play_time_ms_;
    std::atomic<int64_t> last_frame_time_ms_;
    std::atomic<uint64_t> total_frames_decoded_;

    // 音频缓冲
    std::queue<SingAudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_;

    // MP3 解码器
    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_;

    // WAV 解析状态
    bool wav_mode_ = false;
    bool wav_header_parsed_ = false;
    int wav_channels_ = 1;
    int wav_sample_rate_ = 16000;
    int wav_bits_per_sample_ = 16;

    // 最近一次下载结果（用于兼容 Download 返回值）
    std::string last_downloaded_data_;

    // 与 esp32_music 接口一致的缓冲配置（从头文件可见）
    static constexpr size_t MAX_BUFFER_SIZE = 128 * 1024; // 可根据内存调优
    static constexpr size_t MIN_BUFFER_SIZE = 16 * 1024;  // 播放启动门槛
};