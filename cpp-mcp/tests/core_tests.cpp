#include "devbox/config.hpp"
#include "devbox/native.hpp"
#include "devbox/result.hpp"
#include "devbox/scoped_thread.hpp"
#include <iostream>
#include <limits>
#include <random>
#include <thread>

using namespace devbox;
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
int main() {
    try {
        {
            Json fixtures = Json::array(
                {nullptr, true, false, -123, UINT64_MAX, 1.25, -0.0, std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::quiet_NaN(), "", "quoted\"\\\n\t", "é😀",
                 std::string(700000, 'x'), Json::array(), Json::object(), Json::binary({0, 1, 255})});
            std::mt19937 random(23);
            for (std::size_t i = 0; i < 256; ++i) {
                std::string bytes;
                for (std::size_t j = 0; j < i; ++j)
                    bytes += static_cast<char>(random() & 255);
                fixtures.push_back(Json{{bytes, Json::array({bytes, i, Json{{"nested", bytes}}})}});
            }
            for (const auto& value : fixtures) {
                for (const auto errors : {Json::error_handler_t::replace, Json::error_handler_t::ignore})
                    require(json_dump(value, errors) == value.dump(-1, ' ', false, errors),
                            "compact JSON output preserves reference bytes and UTF-8 policy");
                bool strict_rejected = false;
                std::string expected;
                try {
                    expected = value.dump();
                } catch (const Json::type_error&) {
                    strict_rejected = true;
                }
                try {
                    const auto actual = json_dump(value);
                    require(!strict_rejected && actual == expected, "strict JSON output parity");
                } catch (const Json::type_error&) {
                    require(strict_rejected, "strict JSON output rejected valid input");
                }
            }
        }
        {
            const auto saved_curl = environment("CURL_CA_BUNDLE"), saved_ssl = environment("SSL_CERT_FILE");
            ScopeExit restore([&] {
                set_environment("CURL_CA_BUNDLE", saved_curl);
                set_environment("SSL_CERT_FILE", saved_ssl);
            });
            set_environment("SSL_CERT_FILE", "explicit-ssl-ca.pem");
            set_environment("CURL_CA_BUNDLE", "explicit-curl-ca.pem");
            require(tls_ca_bundle() == path_from_utf8("explicit-curl-ca.pem"), "explicit curl CA priority");
            set_environment("CURL_CA_BUNDLE", "");
            require(tls_ca_bundle() == path_from_utf8("explicit-ssl-ca.pem"), "explicit SSL CA fallback");
        }
        require(join({"", "", "x"}, "\n") == "\n\nx", "leading empty lines");
        require(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "SHA256");
        require(base64_encode("hello") == "aGVsbG8=", "base64 encoding");
        const auto bytes = base64_decode("aGVsbG8=");
        require(std::string(bytes.begin(), bytes.end()) == "hello", "base64 decode");
        require(js_length("a😀b") == 4, "UTF16 length");
        require(js_slice("a😀b", 1, 3) == "😀", "UTF16 slice");
        require(js_slice("a😀b", 1, 2) == "�", "lone surrogate replacement");
        require(sanitize_utf8(std::string("a\xf0\x9f", 3)) == "a�", "partial UTF8 sequence");
        require(utc_from_millis(0) == "1970-01-01T00:00:00.000Z", "UTC formatting");
        require(parse_utc("1970-01-01T01:00:00.125+01:00") == 125, "UTC offset parse");
        auto out = shape_output("a\nb\nc\n", "tail", 1000, 2);
        require(out.text == "... head lines omitted ...\nb\nc\n" && out.original_lines == 3, "line shaping");
        out = shape_output(std::string(500, 'a'), "summary", 120, 0);
        require(out.truncated && js_length(out.text) <= 120, "bounded summary");
        const auto good = result_process("done", Json{{"x", 1}}, "hello", "", 0, true);
        require(!good["isError"].get<bool>() && good["structuredContent"]["stdout"] == "hello" &&
                    !good["structuredContent"].contains("stderr"),
                "success envelope");
        const auto bad = result_process("failed", std::nullopt, "", "", 1, false);
        require(bad["structuredContent"].contains("stderr") && bad["structuredContent"]["exitCode"] == 1,
                "error streams");
        const auto env = parse_env_text("A=one\nB=\"two\\nlines\"\nC='literal $A'\nD=tail # comment\n");
        require(env["A"] == "one" && env["B"] == "two\nlines" && env["C"] == "literal $A" &&
                    env["D"] == "tail",
                "environment syntax");
        require(Url::parse("https://EXAMPLE.COM:443/path?q=a#b").str() == "https://example.com/path?q=a#b",
                "URL normalization");
        require(Url::parse("http://[::1]:8100/").origin() == "http://[::1]:8100", "IPv6 origin");
        const auto root = fs::temp_directory_path() / path_from_utf8("devbox-cpp-core-" + uuid());
        fs::create_directory(root);
        try {
            const auto nested = root / "existing" / "nested";
            ensure_directory(nested);
            write_file(nested / "preserved.txt", "preserve");
            ensure_directory(nested);
            require(read_file(nested / "preserved.txt") == "preserve",
                    "existing directory fast path preserves contents");
            bool rejected_directory = false;
            try {
                ensure_directory(nested / "preserved.txt");
            } catch (const fs::filesystem_error&) {
                rejected_directory = true;
            }
            require(rejected_directory, "directory creation rejects an existing ordinary file");
            write_json_atomic(root / "state.json", Json{{"state", "old"}});
            write_json_atomic(root / "state.json", Json{{"state", "new"}});
            require(read_json(root / "state.json")["state"] == "new", "atomic JSON replacement");
            const auto journal = root / "journal.json";
            const auto padding = std::string(256 * 1024, 'x');
            write_json_atomic(journal, Json{{"revision", 0}, {"padding", padding}});
            std::atomic_bool stop_reader{false}, reader_started{false};
            std::exception_ptr reader_error, writer_error;
            ScopedThread reader([&] {
                reader_started = true;
                try {
                    while (!stop_reader) {
                        const auto value = read_json(journal);
                        require(value["padding"] == padding, "concurrent journal snapshot");
                    }
                } catch (...) {
                    reader_error = std::current_exception();
                }
            });
            while (!reader_started)
                std::this_thread::yield();
            try {
                for (int i = 1; i <= 100; ++i)
                    write_json_atomic(journal, Json{{"revision", i}, {"padding", padding}});
            } catch (...) {
                writer_error = std::current_exception();
            }
            stop_reader = true;
            reader.join();
            if (writer_error)
                std::rethrow_exception(writer_error);
            if (reader_error)
                std::rethrow_exception(reader_error);
            require(read_json(journal)["revision"] == 100, "journal readers permit atomic replacement");
        } catch (...) {
            fs::remove_all(root);
            throw;
        }
        fs::remove_all(root);
        auto cancel = std::make_shared<Cancellation>();
        std::thread thread([cancel] {
            std::this_thread::sleep_for(Millis(10));
            cancel->cancel();
        });
        require(cancel->wait_for(Millis(1000)), "cancellation wakeup");
        thread.join();
        std::cout << "C++ core checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
