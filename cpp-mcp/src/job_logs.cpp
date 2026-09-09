#include "devbox/jobs.hpp"
#include "devbox/scoped_thread.hpp"
#include <algorithm>
#include <deque>
#include <fstream>
#include <thread>
namespace devbox {
fs::path rotated_log_path(const fs::path& path, std::size_t index) {
    return index ? path_from_utf8(path_text(path) + '.' + std::to_string(index)) : path;
}
namespace {
class RotatingSink {
    fs::path path_;
    std::ofstream file_;
    std::uint64_t maximum_, current_ = 0, total_ = 0, performed_ = 0;
    std::size_t rotations_;
    std::optional<std::string> failure_;
    void open() {
        file_.open(path_, std::ios::binary | std::ios::app);
        if (!file_)
            throw Error("Cannot open rotating log " + path_text(path_));
        current_ = fs::file_size(path_);
    }
    void rotate() {
        file_.flush();
        if (!file_)
            throw Error("Cannot flush rotating log " + path_text(path_));
        file_.close();
        std::error_code ec;
        fs::remove(rotated_log_path(path_, rotations_), ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            throw std::system_error(ec);
        if (rotations_) {
            for (auto index = rotations_; index > 0; --index) {
                const auto source = rotated_log_path(path_, index - 1);
                const auto target = rotated_log_path(path_, index);
                if (fs::exists(source))
                    replace_state_file(source, target);
            }
        }
        open();
        ++performed_;
    }

  public:
    RotatingSink(fs::path path, std::uint64_t maximum, std::size_t rotations)
        : path_(std::move(path)), maximum_(std::max<std::uint64_t>(4096, maximum)), rotations_(rotations) {
        ensure_directory(path_.parent_path());
        open();
    }
    void fail(std::string message) {
        if (!failure_)
            failure_ = std::move(message);
    }
    void write(std::string_view bytes) {
        if (failure_)
            return;
        try {
            while (!bytes.empty()) {
                if (current_ >= maximum_)
                    rotate();
                const auto count =
                    static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), maximum_ - current_));
                file_.write(bytes.data(), static_cast<std::streamsize>(count));
                if (!file_)
                    throw Error("Cannot write rotating log " + path_text(path_));
                current_ += count;
                total_ += count;
                bytes.remove_prefix(count);
            }
            // Make logs visible to a separate status reader during the running command.
            file_.flush();
            if (!file_)
                throw Error("Cannot flush rotating log " + path_text(path_));
        } catch (const std::exception& error) {
            fail(error.what());
        }
    }
    Json finish() {
        if (file_.is_open()) {
            file_.flush();
            if (!file_)
                fail("Cannot flush rotating log " + path_text(path_));
            file_.close();
        }
        std::error_code ec;
        const auto size = fs::file_size(path_, ec);
        if (!ec)
            current_ = size;
        return Json{{"maxBytes", maximum_},
                    {"rotations", rotations_},
                    {"rotationsPerformed", performed_},
                    {"totalBytes", total_},
                    {"currentBytes", current_},
                    {"truncated", performed_ > 0},
                    {"failed", failure_.has_value()},
                    {"error", failure_ ? Json(*failure_) : Json(nullptr)}};
    }
};
std::string tail_one(const fs::path& path, std::size_t max_chars) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return {};
    if (ec)
        throw std::system_error(ec);
    const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(size, std::max<std::size_t>(4096, max_chars * 4)));
    std::string bytes;
    try {
        bytes = read_file_range(path, size - count, count);
    } catch (const std::system_error& error) {
        if (error.code() == std::errc::no_such_file_or_directory)
            return {};
        throw;
    }
    auto text = sanitize_utf8(bytes);
    const auto length = js_length(text);
    return length > max_chars ? js_slice(text, length - max_chars, length) : text;
}
} // namespace
struct JobLogPump::State {
    struct Chunk {
        OutputStream stream;
        std::string bytes;
    };
    RotatingSink stdout_sink, stderr_sink;
    std::deque<Chunk> queue;
    std::mutex mutex;
    std::condition_variable ready, room;
    bool closing = false;
    std::optional<std::string> queue_failure;
    Json result;
    ScopedThread worker;
    State(const fs::path& out, const fs::path& err, std::uint64_t maximum, std::size_t rotations)
        : stdout_sink(out, maximum, rotations), stderr_sink(err, maximum, rotations),
          worker([this] { run(); }) {}
    void run() {
        while (true) {
            Chunk chunk;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return closing || !queue.empty(); });
                if (queue.empty())
                    break;
                chunk = std::move(queue.front());
                queue.pop_front();
                room.notify_one();
            }
            auto& sink = chunk.stream == OutputStream::stdout_stream ? stdout_sink : stderr_sink;
            sink.write(chunk.bytes);
        }
        if (queue_failure) {
            stdout_sink.fail(*queue_failure);
            stderr_sink.fail(*queue_failure);
        }
        const auto out = stdout_sink.finish(), err = stderr_sink.finish();
        result = Json{
            {"stdout", out},
            {"stderr", err},
            {"truncated", out["truncated"] == true || err["truncated"] == true || queue_failure.has_value()}};
    }
    void finish() {
        {
            std::lock_guard lock(mutex);
            closing = true;
            ready.notify_all();
            room.notify_all();
        }
        if (worker.joinable())
            worker.join();
    }
    ~State() {
        finish();
    }
};
JobLogPump::JobLogPump(const fs::path& out, const fs::path& err, std::uint64_t maximum, std::size_t rotations)
    : state_(std::make_unique<State>(out, err, maximum, rotations)) {}
JobLogPump::~JobLogPump() = default;
void JobLogPump::push(OutputStream stream, std::string_view bytes) {
    // Queue both streams in process-observation order, with at most 4 MiB of 16 KiB chunks.
    while (!bytes.empty()) {
        std::unique_lock lock(state_->mutex);
        if (state_->closing || state_->queue_failure)
            return;
        if (!state_->room.wait_for(lock, Millis(1000),
                                   [&] { return state_->queue.size() < 256 || state_->closing; })) {
            state_->queue_failure =
                "Job log writer queue remained stalled for 1000 ms; remaining output was not retained.";
            return;
        }
        if (state_->closing)
            return;
        const auto count = std::min<std::size_t>(16384, bytes.size());
        state_->queue.push_back({stream, std::string(bytes.substr(0, count))});
        bytes.remove_prefix(count);
        state_->ready.notify_one();
    }
}
Json JobLogPump::finish() {
    state_->finish();
    return state_->result;
}
Json log_metadata(const fs::path& path, std::size_t rotations) {
    Json segments = Json::array();
    std::uint64_t total = 0;
    bool rotated = false;
    for (std::size_t index = 0; index <= rotations; ++index) {
        std::error_code ec;
        const auto size = fs::file_size(rotated_log_path(path, index), ec);
        if (ec == std::errc::no_such_file_or_directory)
            continue;
        if (ec)
            throw std::system_error(ec);
        total += size;
        rotated = rotated || index > 0;
        segments.push_back(Json{{"index", index}, {"bytes", size}});
    }
    return Json{{"totalBytes", total}, {"segments", segments}, {"rotated", rotated}};
}
std::string read_log_tail(const fs::path& path, std::size_t max_chars, std::size_t rotations) {
    std::size_t remaining = std::max<std::size_t>(1, max_chars);
    std::string text;
    for (std::size_t index = 0; index <= rotations && remaining; ++index) {
        const auto part = tail_one(rotated_log_path(path, index), remaining);
        const auto length = js_length(part);
        remaining -= std::min(remaining, length);
        text = part + text;
    }
    const auto length = js_length(text);
    return length > max_chars ? js_slice(text, length - max_chars, length) : text;
}
} // namespace devbox
